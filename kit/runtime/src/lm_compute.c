// lm_compute.c —— compute worker 池实现（Phase 7.4）
// worker 线程池 + 协程跨线程迁移。
// 迁移走"标记 + yield + 调度方 post"协议（PENDING 状态防双线程同栈）
// （见 lm_co.h），复用既有 scheduler 就绪队列与跨线程唤醒（Phase 7.2/7.3），
// worker 无 reactor，靠 post 内 cond signal 唤醒。
#include "lm_compute.h"
#include "lm_scheduler.h"
#include "lm_co.h"
#include "lm_reactor.h"   /* Phase 8.5：lm_now_ns() 长调度告警 */
#include "lm_butex.h"     /* Phase 8.10：shrink 时 lm_butex_wake 唤醒待退出 worker */
#include "lm_sched_stats.h" /* Phase 8.11：reduction 账本 + 长调度告警计数 */
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>

/* compute 池：N worker 线程，各持一个无 reactor scheduler（就绪队列 +
 * idle_cond），只跑 compute 协程。全局单例，懒初始化。
 * Phase 8.10：容量制——scheds/threads 按 capacity（硬上限）分配，n 为当前
 * 活跃数，支持按需扩容（maybe_grow/grow）与优雅缩容（shrink）。 */
typedef struct lm_compute_pool_s {
    lm_scheduler_t** scheds;  /* capacity 大小数组，前 n 个活跃 */
    pthread_t* threads;
    _Atomic int n;            /* 活跃 worker 数（懒初始化发布后原子增/缩） */
    int capacity;             /* 数组容量 = 硬上限（max_workers 解析结果） */
    _Atomic unsigned next;    /* round-robin 分发指针（多线程原子自增） */
} lm_compute_pool_t;

static lm_compute_pool_t g_pool = { NULL, NULL, 0, 0, 0 };
/* 懒初始化互斥：多 IO 线程/sysmon 并发首次 computeBegin 时防双初始化
 *（双 calloc + 双 pthread_create → worker 线程与 scheduler 泄漏）。
 * Phase 8.10 起兼作扩容/缩容互斥（数组与 n 的一致性）。 */
static pthread_mutex_t g_pool_init_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Phase 8.10 硬上限解析：env LM_MAX_WORKERS，默认 4×CPU 数，封顶
 * LM_MAX_WORKERS_CAP(256)。缓存一次（worker 上限运行期不变）。 */
static int max_workers(void) {
    static _Atomic int cached = 0;
    int v = atomic_load_explicit(&cached, memory_order_acquire);
    if (v == 0) {
        const char* e = getenv("LM_MAX_WORKERS");
        long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
        if (ncpu < 1) ncpu = 1;
        int env = e ? atoi(e) : 0;
        v = env > 0 ? env : (int)(ncpu * 4);
        if (v > LM_MAX_WORKERS_CAP) v = LM_MAX_WORKERS_CAP;
        if (v < 1) v = 1;
        atomic_store_explicit(&cached, v, memory_order_release);
    }
    return v;
}

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
        if (!co) break;   /* stop / reject_new 置位且全源空：worker 退出 */
        atomic_store_explicit(&s->current, co, memory_order_release);
        lm_co_resume(co);
        atomic_store_explicit(&s->current, NULL, memory_order_release);
        /* Phase 8.5 C：长调度墙钟告警（同 drain_ready）。 */
        {
            uint64_t elapsed_ns = lm_now_ns() - co->last_resume_ns;
            if (elapsed_ns > (uint64_t)LM_SCHED_LONG_SCHED_MS * 1000000ULL) {
                lm_sched_stats_long_sched();   /* Phase 8.11：长调度告警计数 */
                fprintf(stderr,
                    "[compute] long schedule: co=%p elapsed=%.2fms (threshold=%dms)\n",
                    (void*)co, elapsed_ns / 1000000.0, LM_SCHED_LONG_SCHED_MS);
            }
        }
        /* Phase 8.5 统一收敛：slice_yield 重入队已上移到 lm_co_resume 返回段
         * 统一处理（覆盖所有 resume 路径），本点不再重复——否则与 resume 路径
         * 双重入队。迁移互斥由 handle_migrate 在下点处理（migrate_sched 非空时
         * resume 的 slice_yield 块已跳过，两路径天然互斥）。 */
        lm_scheduler_handle_migrate(co);
        /* DEAD 协程不自动销毁（owner 管，与 IO scheduler 契约一致）。 */
    }
    /* Phase 8.10：缩容（reject_new）退出的 worker 自我释放 scheduler——
     * release owner 引用，无异步引用时归零 free 并从 g_scheds 注销。
     * stop 路径由 owner 管理生命周期，不在此自我释放。 */
    if (atomic_load_explicit(&s->reject_new, memory_order_acquire)) {
        lm_scheduler_destroy(s);
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
    int cap = max_workers();              /* Phase 8.10：硬上限 */
    int n = (int)ncpu;
    if (n > cap) n = cap;                 /* 初始规模不超过硬上限 */
    /* 容量制：数组按硬上限分配，前 n 个活跃；扩容在空槽位追加。 */
    lm_scheduler_t** scheds = (lm_scheduler_t**)calloc((size_t)cap, sizeof(lm_scheduler_t*));
    pthread_t* threads = (pthread_t*)calloc((size_t)cap, sizeof(pthread_t));
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
    g_pool.capacity = cap;
    atomic_store_explicit(&g_pool.n, ok, memory_order_release);
    pthread_mutex_unlock(&g_pool_init_mutex);
    return 0;
}

/* ============================================================
 * Phase 8.10：弹性扩缩容
 * ============================================================ */

/* 扩容 1 个 worker（须持 g_pool_init_mutex）：未达硬上限才创建。
 * 新建 scheduler 由 lm_scheduler_new 内部注册进 g_scheds 参与窃取。
 * 返回 0 成功，-1 已达上限 / 未初始化 / 创建失败。 */
static int pool_grow_one_locked(void) {
    int n = atomic_load_explicit(&g_pool.n, memory_order_acquire);
    if (n <= 0 || n >= g_pool.capacity) return -1;
    lm_scheduler_t* s = lm_scheduler_new(NULL);
    if (!s) return -1;
    pthread_t th;
    if (pthread_create(&th, NULL, compute_worker_run, s) != 0) {
        lm_scheduler_destroy(s);
        return -1;
    }
    pthread_detach(th);
    g_pool.scheds[n] = s;
    g_pool.threads[n] = th;
    atomic_store_explicit(&g_pool.n, n + 1, memory_order_release);
    return 0;
}

int lm_compute_pool_maybe_grow(void) {
    if (atomic_load_explicit(&g_pool.n, memory_order_acquire) <= 0) return -1;
    pthread_mutex_lock(&g_pool_init_mutex);
    int rc = pool_grow_one_locked();
    pthread_mutex_unlock(&g_pool_init_mutex);
    return rc;
}

int lm_compute_pool_grow(int n) {
    if (n <= 0) return 0;
    if (lm_compute_init() != 0) return -1;
    int added = 0;
    pthread_mutex_lock(&g_pool_init_mutex);
    for (int i = 0; i < n; i++) {
        if (pool_grow_one_locked() != 0) break;   /* 达上限或失败即停 */
        added++;
    }
    pthread_mutex_unlock(&g_pool_init_mutex);
    return added;
}

int lm_compute_pool_shrink(int n) {
    if (n <= 0) return 0;
    if (atomic_load_explicit(&g_pool.n, memory_order_acquire) <= 0) return -1;
    int marked = 0;
    pthread_mutex_lock(&g_pool_init_mutex);
    int cur = atomic_load_explicit(&g_pool.n, memory_order_acquire);
    /* 从尾部摘（后进先出），至少保留 1 个 worker */
    while (marked < n && cur > 1) {
        cur--;
        lm_scheduler_t* s = g_pool.scheds[cur];
        g_pool.scheds[cur] = NULL;   /* 移出分发池：round-robin 不再选中 */
        atomic_store_explicit(&s->reject_new, 1, memory_order_release);
        atomic_fetch_add_explicit(&s->sleepWord, 1, memory_order_release);
        lm_butex_wake(&s->sleepWord, 1);   /* wake 它出来检查 reject_new 退出 */
        marked++;
    }
    atomic_store_explicit(&g_pool.n, cur, memory_order_release);
    pthread_mutex_unlock(&g_pool_init_mutex);
    return marked;
}

int lm_compute_pool_size(void) {
    return atomic_load_explicit(&g_pool.n, memory_order_acquire);
}

int lm_compute_pool_capacity(void) {
    return max_workers();
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
