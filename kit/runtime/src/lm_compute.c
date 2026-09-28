// lm_compute.c —— compute worker 池实现（Phase 7.4）
// 对标 lthread_compute.c：worker 线程池 + 协程跨线程迁移。
// 与 lthread 的差异：lthread 用 ucontext 直接在线程间 switch 上下文
// （PENDING 状态防双线程同栈）；lm 的迁移走"标记 + yield + 调度方 post"协议
// （见 lm_co.h），复用既有 scheduler 就绪队列与跨线程唤醒（Phase 7.2/7.3），
// worker 无 reactor，靠 post 内 cond signal 唤醒。
#include "lm_compute.h"
#include "lm_scheduler.h"
#include "lm_co.h"
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>

/* compute 池：N worker 线程，各持一个无 reactor scheduler（就绪队列 +
 * idle_cond），只跑 compute 协程。全局单例，懒初始化。 */
typedef struct lm_compute_pool_s {
    lm_scheduler_t** scheds;  /* 每 worker 一个 scheduler（reactor=NULL） */
    pthread_t* threads;
    int n;                    /* worker 数（成功创建数） */
    unsigned next;            /* round-robin 分发指针 */
} lm_compute_pool_t;

static lm_compute_pool_t g_pool = { NULL, NULL, 0, 0 };

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
        s->current = co;
        lm_co_resume(co);
        s->current = NULL;
        lm_scheduler_handle_migrate(co);
        /* DEAD 协程不自动销毁（owner 管，与 IO scheduler 契约一致）。 */
    }
    return NULL;
}

int lm_compute_init(void) {
    if (g_pool.n > 0) return 0;
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    if (ncpu < 1) ncpu = 1;
    int n = (int)ncpu;
    lm_scheduler_t** scheds = (lm_scheduler_t**)calloc((size_t)n, sizeof(lm_scheduler_t*));
    pthread_t* threads = (pthread_t*)calloc((size_t)n, sizeof(pthread_t));
    if (!scheds || !threads) {
        free(scheds);
        free(threads);
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
        return -1;
    }
    /* 部分失败时已成功的 worker 仍可用（池变小），不回收避免 double-free。 */
    g_pool.scheds = scheds;
    g_pool.threads = threads;
    g_pool.n = ok;
    return 0;
}

int lm_compute_begin(void) {
    lm_co_t* co = lm_co_current();
    lm_scheduler_t* io = lm_scheduler_get_current();
    if (!co || !io) return -1;        /* 须在协程栈内且本线程有 scheduler */
    if (co->home_sched) return -2;    /* 已在 compute 上下文（嵌套 begin） */
    if (lm_compute_init() != 0 || g_pool.n == 0) return -3;
    lm_scheduler_t* target = g_pool.scheds[g_pool.next % (unsigned)g_pool.n];
    g_pool.next++;
    co->home_sched = io;              /* 非空 = 处于 compute 池（computeEnd 依据） */
    co->migrate_sched = target;       /* yield 后 drain_ready post 到该 worker */
    lm_co_yield();
    return 0;
}

int lm_compute_end(void) {
    lm_co_t* co = lm_co_current();
    if (!co || !co->home_sched) return -1;  /* 不在 compute 上下文 */
    co->migrate_sched = co->home_sched;     /* 迁回老家 */
    co->home_sched = NULL;
    lm_co_yield();   /* worker resume 返回后 post 回家 + 唤醒 IO reactor */
    return 0;
}
