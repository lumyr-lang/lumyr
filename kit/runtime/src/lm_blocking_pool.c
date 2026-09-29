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
