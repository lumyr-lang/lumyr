// lm_blocking_pool.c —— Phase 8.5 F：阻塞 syscall 流放池实现
// 对标 Tokio `tokio/src/runtime/blocking/pool.rs`：
//   独立 OS 线程池，承接阻塞 syscall，完成后回投原协程。
//
// 设计要点：
//   - 任务队列：mutex 保护的单链 FIFO，head/tail 串联 lm_blocking_task_t。
//   - 唤醒：提交方 push 后改 sleepWord + lm_futex_wake，无 waiter 时零 syscall。
//   - 睡眠：worker pop 空时 lm_futex_wait(sleepWord, 10s)，
//     超时且线程数 > min(1) 则退出收缩。
//   - 按需扩容：提交时无空闲线程且未达上限则新建 worker。
//   - 线程退出：detach 线程，从 g_pool.n 原子递减（pool 线程指针不回收，
//     避免 pthread_t 复用 UAF——detached 线程退出后 pthread_t 失效）。
#include "lm_blocking_pool.h"
#include "lm_scheduler.h"
#include "lm_butex.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>

#define LM_BLOCKING_MAX     512
#define LM_BLOCKING_MIN     1
#define LM_BLOCKING_KEEP_ALIVE_US  10000000L  /* 10s，对齐 Tokio KEEP_ALIVE */

typedef struct lm_blocking_task {
    lm_blocking_fn       fn;
    void*                arg;
    lm_blocking_done_cb  done_cb;
    lm_co_t*             co;
    struct lm_blocking_task* next;
} lm_blocking_task_t;

typedef struct {
    lm_blocking_task_t*  head;
    lm_blocking_task_t*  tail;
    pthread_mutex_t      mutex;
    _Atomic uint32_t     sleepWord;   /* 睡眠字（futex wait/wake） */
    _Atomic int          n;           /* 当前线程数 */
    _Atomic int          idle;        /* 空闲线程数 */
    _Atomic int          stop;        /* 停止标志 */
} lm_blocking_pool_t;

static lm_blocking_pool_t g_pool = {
    .head = NULL, .tail = NULL,
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .sleepWord = 0,
    .n = 0, .idle = 0, .stop = 0
};

/* 取一个任务（mutex 保护）。队列空返回 NULL。 */
static lm_blocking_task_t* pool_pop_task(void) {
    pthread_mutex_lock(&g_pool.mutex);
    lm_blocking_task_t* t = g_pool.head;
    if (t) {
        g_pool.head = t->next;
        if (!g_pool.head) g_pool.tail = NULL;
    }
    pthread_mutex_unlock(&g_pool.mutex);
    return t;
}

/* 推一个任务到队尾（mutex 保护）。 */
static void pool_push_task(lm_blocking_task_t* t) {
    t->next = NULL;
    pthread_mutex_lock(&g_pool.mutex);
    if (g_pool.tail) {
        g_pool.tail->next = t;
    } else {
        g_pool.head = t;
    }
    g_pool.tail = t;
    pthread_mutex_unlock(&g_pool.mutex);
}

/* worker 主循环：pop → 执行 → done_cb → 空闲时 futex_wait 10s。 */
static void* blocking_worker_main(void* arg) {
    (void)arg;
    atomic_fetch_add_explicit(&g_pool.n, 1, memory_order_relaxed);
    for (;;) {
        lm_blocking_task_t* t = pool_pop_task();
        if (t) {
            atomic_fetch_sub_explicit(&g_pool.idle, 1, memory_order_relaxed);
            void* result = t->fn(t->arg);
            if (t->done_cb) t->done_cb(t->co, result);
            free(t);
            atomic_fetch_add_explicit(&g_pool.idle, 1, memory_order_relaxed);
            continue;
        }
        /* 无任务：futex_wait 10s，超时且线程数 > min 则退出收缩。 */
        uint32_t expected = atomic_load_explicit(&g_pool.sleepWord,
                                                 memory_order_acquire);
        int woken = lm_futex_wait(&g_pool.sleepWord, expected,
                                  LM_BLOCKING_KEEP_ALIVE_US);
        if (atomic_load_explicit(&g_pool.stop, memory_order_acquire)) break;
        if (!woken) {
            /* 超时未唤醒：若线程数 > min 则退出，否则继续等。 */
            int n = atomic_load_explicit(&g_pool.n, memory_order_relaxed);
            if (n > LM_BLOCKING_MIN) {
                atomic_fetch_sub_explicit(&g_pool.idle, 1, memory_order_relaxed);
                atomic_fetch_sub_explicit(&g_pool.n, 1, memory_order_relaxed);
                break;
            }
        }
    }
    return NULL;
}

/* 按需新建 worker（池未达上限时）。返回 0 成功，-1 失败。 */
static int pool_maybe_spawn(void) {
    int n = atomic_load_explicit(&g_pool.n, memory_order_relaxed);
    if (n >= LM_BLOCKING_MAX) return -1;
    pthread_t tid;
    if (pthread_create(&tid, NULL, blocking_worker_main, NULL) != 0) return -1;
    pthread_detach(tid);
    atomic_fetch_add_explicit(&g_pool.idle, 1, memory_order_relaxed);
    return 0;
}

int lm_blocking_submit(lm_blocking_fn fn, void* arg,
                       lm_blocking_done_cb done_cb, lm_co_t* co) {
    if (!fn) return -1;
    if (atomic_load_explicit(&g_pool.stop, memory_order_acquire)) return -1;
    lm_blocking_task_t* t = (lm_blocking_task_t*)calloc(1, sizeof(lm_blocking_task_t));
    if (!t) return -1;
    t->fn = fn;
    t->arg = arg;
    t->done_cb = done_cb;
    t->co = co;
    pool_push_task(t);
    /* 唤醒一个空闲 worker；无空闲时按需新建。 */
    int idle = atomic_load_explicit(&g_pool.idle, memory_order_relaxed);
    if (idle > 0) {
        atomic_fetch_add_explicit(&g_pool.sleepWord, 1, memory_order_release);
        lm_futex_wake(&g_pool.sleepWord);
    } else {
        pool_maybe_spawn();
    }
    return 0;
}

int lm_blocking_pool_size(void) {
    return atomic_load_explicit(&g_pool.n, memory_order_relaxed);
}

int lm_blocking_pool_idle(void) {
    return atomic_load_explicit(&g_pool.idle, memory_order_relaxed);
}

/* ============================================================
 * 协程同步等待封装（lm_co_await_blocking）
 * 机制：把用户 fn + 参数 + 目标 scheduler 装入 awaitCtx 提交，
 *   池线程执行完经 await_done 把协程回投目标 scheduler 队列；
 *   提交方 yield，被 drain 唤醒续行后读结果。
 * awaitCtx 生命周期：提交方在 yield 返回、读取结果后释放——
 *   await_done 只回投不释放（协程尚未读到结果）。
 * scheduler 引用：提交时 retain（防回投前 scheduler 被销毁），
 *   await_done 回投完成后 release。
 * ============================================================ */
typedef struct {
    lm_blocking_fn  fn;
    void*           arg;
    lm_scheduler_t* sched;
    void*           result;
} lm_await_ctx;

/* 池线程执行：调用户 fn，把结果存入 ctx（任务 fn 签名要求返回 void*）。 */
static void* blocking_await_trampoline(void* arg) {
    lm_await_ctx* c = (lm_await_ctx*)arg;
    c->result = c->fn(c->arg);
    return c;
}

/* 池线程完成回调：回投协程到提交时所在 scheduler，归还 scheduler 引用。
 * 不释放 ctx：协程续行时要读 result。 */
static void blocking_await_done(lm_co_t* co, void* result) {
    lm_await_ctx* c = (lm_await_ctx*)result;
    lm_scheduler_t* sched = c->sched;
    if (co) lm_scheduler_wakeup(sched, co);
    lm_scheduler_release(sched);
}

int lm_co_await_blocking(lm_blocking_fn fn, void* arg, void** resultOut) {
    lm_co_t* co = lm_co_current();
    lm_scheduler_t* sched = lm_scheduler_get_current();
    if (!co || !sched) return -1;   /* 无协程/调度器上下文：调用方自行同步执行 */
    lm_await_ctx* c = (lm_await_ctx*)calloc(1, sizeof(lm_await_ctx));
    if (!c) return -2;
    c->fn = fn;
    c->arg = arg;
    c->sched = sched;
    lm_scheduler_retain(sched);
    if (lm_blocking_submit(blocking_await_trampoline, c,
                           blocking_await_done, co) != 0) {
        lm_scheduler_release(sched);
        free(c);
        return -2;
    }
    /* 让出本线程；完成回投后由 drain resume 续行。 */
    lm_co_yield();
    if (resultOut) *resultOut = c->result;
    free(c);
    return 0;
}
