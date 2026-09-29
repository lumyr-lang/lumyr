// lm_compute.c —— compute worker 池实现（Phase 7.4）
// 对标 lthread_compute.c：worker 线程池 + 协程跨线程迁移。
// 与 lthread 的差异：lthread 用 ucontext 直接在线程间 switch 上下文
// （PENDING 状态防双线程同栈）；lm 的迁移走"标记 + yield + 调度方 post"协议
// （见 lm_co.h），复用既有 scheduler 就绪队列与跨线程唤醒（Phase 7.2/7.3），
// worker 无 reactor，靠 post 内 cond signal 唤醒。
#include "lm_compute.h"
#include "lm_scheduler.h"
#include "lm_co.h"
#include "lm_reactor.h"   /* Phase 8.5：lm_now_ns() 长调度告警 */
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>

/* compute 池：N worker 线程，各持一个无 reactor scheduler（就绪队列 +
 * idle_cond），只跑 compute 协程。全局单例，懒初始化。 */
typedef struct lm_compute_pool_s {
    lm_scheduler_t** scheds;  /* 每 worker 一个 scheduler（reactor=NULL） */
    pthread_t* threads;
    _Atomic int n;            /* worker 数（成功创建数）；懒初始化发布后只读 */
    _Atomic unsigned next;    /* round-robin 分发指针（多线程原子自增） */
} lm_compute_pool_t;

static lm_compute_pool_t g_pool = { NULL, NULL, 0, 0 };
/* 懒初始化互斥：多 IO 线程/sysmon 并发首次 computeBegin 时防双初始化
 *（双 calloc + 双 pthread_create → worker 线程与 scheduler 泄漏）。 */
static pthread_mutex_t g_pool_init_mutex = PTHREAD_MUTEX_INITIALIZER;

/* worker 主循环：阻塞 pop + resume + 迁移处理。
 * resume 返回（协程 yield 或 DEAD）后调 handle_migrate：
 *   - computeEnd 回家 → post 回老家 IO scheduler + self-pipe 唤醒 reactor；
 *   - 纯 yield 挂起（migrate_sched 空）→ no-op，等 coWakeup(co, worker_sched)
 *     或 cond 唤醒再入队。 */
static void* compute_worker_run(void* arg) {
    lm_scheduler_t* s = (lm_scheduler_t*)arg;
    lm_scheduler_set_current(s);
    for (;;) {
        lm_co_t* co = lm_scheduler_pop_blocking(s);
        if (!co) {
            if (s->stop) break;   /* stop 置位且队列空：worker 退出 */
            continue;
        }
        atomic_store_explicit(&s->current, co, memory_order_release);
        lm_co_resume(co);
        atomic_store_explicit(&s->current, NULL, memory_order_release);
        /* Phase 8.5 C：长调度墙钟告警（同 drain_ready）。 */
        {
            uint64_t elapsed_ns = lm_now_ns() - co->last_resume_ns;
            if (elapsed_ns > (uint64_t)LM_SCHED_LONG_SCHED_MS * 1000000ULL) {
                fprintf(stderr,
                    "[compute] long schedule: co=%p elapsed=%.2fms (threshold=%dms)\n",
                    (void*)co, elapsed_ns / 1000000.0, LM_SCHED_LONG_SCHED_MS);
            }
        }
        /* Phase 8.5：时间片耗尽重入队 vs 迁移——互斥（同 drain_ready）。
         * migrate_sched 非空时由 handle_migrate 投递，不重入队本 worker。 */
        if (co->slice_yield) {
            co->slice_yield = 0;
            if (atomic_load_explicit(&co->state, memory_order_acquire) != LM_CO_DEAD &&
                atomic_load_explicit(&co->migrate_sched, memory_order_acquire) == NULL) {
                lm_scheduler_post(s, co);
            }
        }
        lm_scheduler_handle_migrate(co);
        /* DEAD 协程不自动销毁（owner 管，与 IO scheduler 契约一致）。 */
    }
    return NULL;
}

int lm_compute_init(void) {
    if (atomic_load_explicit(&g_pool.n, memory_order_acquire) > 0) return 0;
    pthread_mutex_lock(&g_pool_init_mutex);
    if (atomic_load_explicit(&g_pool.n, memory_order_acquire) > 0) {
        pthread_mutex_unlock(&g_pool_init_mutex);
        return 0;   /* 双检：并发初始化已由他人完成 */
    }
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    if (ncpu < 1) ncpu = 1;
    int n = (int)ncpu;
    lm_scheduler_t** scheds = (lm_scheduler_t**)calloc((size_t)n, sizeof(lm_scheduler_t*));
    pthread_t* threads = (pthread_t*)calloc((size_t)n, sizeof(pthread_t));
    if (!scheds || !threads) {
        free(scheds);
        free(threads);
        pthread_mutex_unlock(&g_pool_init_mutex);
        return -1;
    }
    int ok = 0;
    for (int i = 0; i < n; i++) {
        lm_scheduler_t* s = lm_scheduler_new(NULL);
        if (!s) break;
        if (pthread_create(&threads[ok], NULL, compute_worker_run, s) != 0) {
            lm_scheduler_destroy(s);
            break;
        }
        pthread_detach(threads[ok]);
        scheds[ok] = s;
        ok++;
    }
    if (ok == 0) {
        free(scheds);
        free(threads);
        pthread_mutex_unlock(&g_pool_init_mutex);
        return -1;
    }
    /* 部分失败时已成功的 worker 仍可用（池变小），不回收避免 double-free。
     * n 最后 release 发布：读方 acquire 见 n>0 时 scheds/threads 必已就绪。 */
    g_pool.scheds = scheds;
    g_pool.threads = threads;
    atomic_store_explicit(&g_pool.n, ok, memory_order_release);
    pthread_mutex_unlock(&g_pool_init_mutex);
    return 0;
}

int lm_compute_begin(void) {
    lm_co_t* co = lm_co_current();
    lm_scheduler_t* io = lm_scheduler_get_current();
    if (!co || !io) return -1;        /* 须在协程栈内且本线程有 scheduler */
    if (atomic_load_explicit(&co->home_sched, memory_order_acquire)) return -2;   /* 已在 compute 上下文（嵌套 begin） */
    if (lm_compute_init() != 0) return -3;
    int n = atomic_load_explicit(&g_pool.n, memory_order_acquire);
    if (n == 0) return -3;
    unsigned idx = atomic_fetch_add_explicit(&g_pool.next, 1, memory_order_relaxed);
    lm_scheduler_t* target = g_pool.scheds[idx % (unsigned)n];
    /* 引用计数化：迁移期间协程持目标 scheduler 引用，timer 回调/其他线程
     * 并发释放 scheduler 时不会 UAF（compute_test 压测第三层修复）。 */
    lm_co_set_home_sched(co, io);             /* 非空 = 处于 compute 池（computeEnd 依据） */
    lm_co_set_migrate_sched(co, target);      /* yield 后 drain_ready post 到该 worker */
    lm_co_yield();
    return 0;
}

int lm_compute_end(void) {
    lm_co_t* co = lm_co_current();
    if (!co) return -1;
    lm_scheduler_t* home = atomic_load_explicit(&co->home_sched, memory_order_acquire);
    if (!home) return -1;                       /* 不在 compute 上下文 */
    lm_co_set_migrate_sched(co, home);  /* 迁回老家：释放 worker 引用 + 转移老家引用 */
    lm_co_set_home_sched(co, NULL);     /* 清 home 标记，引用已由 migrate_sched 持有 */
    lm_co_yield();   /* worker resume 返回后 post 回家 + 唤醒 IO reactor */
    return 0;
}

/* Phase 8.5 E：sysmon 强制迁移取目标 scheduler（round-robin）。 */
lm_scheduler_t* lm_compute_pool_scheduler(void) {
    if (lm_compute_init() != 0) return NULL;
    int n = atomic_load_explicit(&g_pool.n, memory_order_acquire);
    if (n == 0) return NULL;
    unsigned idx = atomic_fetch_add_explicit(&g_pool.next, 1, memory_order_relaxed);
    return g_pool.scheds[idx % (unsigned)n];
}
