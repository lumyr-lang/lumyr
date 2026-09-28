// lm_scheduler.c —— per-thread 协程调度器实现（Phase 7.2）
// 对标 lthread per-thread IO scheduler：reactor + 就绪队列 + current 协程。
// 详见 lm_scheduler.h 设计说明。
#include "lm_scheduler.h"
#include <stdlib.h>

/* ============================================================
 * TLS：当前线程 scheduler 指针
 * 用 _Thread_local 而非 pthread_key：直接内存访问，无 pthread_getspecific
 * 调用帧。lm_co_resume 在协程嵌套 resume 热点调本函数判断是否投递就绪队列，
 * 用 _Thread_local 避免额外栈帧把协程栈（64KiB）顶到 guard page。
 * ============================================================ */

/* 外部链接的 _Thread_local：header 内 static inline lm_scheduler_get_current
 * 直接访问本变量，零调用帧。set_current 仍走函数（非热点，不在协程嵌套栈上）。 */
_Thread_local lm_scheduler_t* g_current_sched = NULL;

void lm_scheduler_set_current(lm_scheduler_t* s) {
    g_current_sched = s;
}

/* ============================================================
 * 生命周期
 * ============================================================ */

lm_scheduler_t* lm_scheduler_new(lm_reactor_t* reactor) {
    lm_scheduler_t* s = (lm_scheduler_t*)calloc(1, sizeof(lm_scheduler_t));
    if (!s) return NULL;
    s->reactor = reactor;
    s->current = NULL;
    s->ready_head = NULL;
    s->ready_tail = NULL;
    pthread_mutex_init(&s->ready_mutex, NULL);
    pthread_cond_init(&s->idle_cond, NULL);
    s->stop = 0;
    return s;
}

void lm_scheduler_destroy(lm_scheduler_t* s) {
    if (!s) return;
    /* 不销毁 reactor（caller 管）；不销毁队列内协程（owner 管）。
     * 队列内残留协程由 owner 自行 destroy，scheduler 仅释放自身结构。 */
    pthread_mutex_destroy(&s->ready_mutex);
    pthread_cond_destroy(&s->idle_cond);
    free(s);
}

/* ============================================================
 * 就绪队列（mutex 保护的 FIFO 单链）
 * ============================================================ */

void lm_scheduler_post(lm_scheduler_t* s, lm_co_t* co) {
    if (!s || !co) return;
    if (co->queued) return;   /* 防重复入队 */
    co->queued = 1;
    co->next = NULL;
    pthread_mutex_lock(&s->ready_mutex);
    if (s->ready_tail) {
        s->ready_tail->next = co;
    } else {
        s->ready_head = co;
    }
    s->ready_tail = co;
    /* Phase 7.4：锁内 signal idle_cond——compute worker 阻塞 pop 时靠它唤醒。
     * 无 waiter（IO scheduler drain 由 reactor 驱动）时 signal 为 no-op，无害。 */
    pthread_cond_signal(&s->idle_cond);
    pthread_mutex_unlock(&s->ready_mutex);
}

/* Phase 7.3：跨线程唤醒——post + wakeup reactor。
 * post 后写 reactor self-pipe 唤醒目标线程（mutex 保护 post 跨线程安全，
 * self-pipe 写非阻塞，管道满返回 EAGAIN 不阻塞调用方）。 */
void lm_scheduler_wakeup(lm_scheduler_t* s, lm_co_t* co) {
    if (!s || !co) return;
    lm_scheduler_post(s, co);
    if (s->reactor) lm_reactor_wakeup(s->reactor);
}

lm_co_t* lm_scheduler_pop(lm_scheduler_t* s) {
    if (!s) return NULL;
    pthread_mutex_lock(&s->ready_mutex);
    lm_co_t* co = s->ready_head;
    if (co) {
        s->ready_head = co->next;
        if (!s->ready_head) s->ready_tail = NULL;
        co->next = NULL;
        co->queued = 0;
    }
    pthread_mutex_unlock(&s->ready_mutex);
    return co;
}

/* ============================================================
 * 就绪队列消费：循环 pop + resume 直到空
 * ============================================================ */

void lm_scheduler_drain_ready(lm_scheduler_t* s) {
    if (!s) return;
    for (;;) {
        lm_co_t* co = lm_scheduler_pop(s);
        if (!co) break;
        /* 置 current=co：使协程内 lm_co_resume 走嵌套直连路径（同步切栈），
         * 而非再次投递到本队列（否则死循环）。yield 后清 current=NULL。 */
        s->current = co;
        lm_co_resume(co);
        s->current = NULL;
        /* Phase 7.4：computeBegin 让出后迁移到 compute worker（此时协程栈
         * 已让出，post 安全——迁移协议见 lm_co.h）。 */
        lm_scheduler_handle_migrate(co);
        /* DEAD 协程不自动销毁：避免与 lm Coroutine.destroy 双重释放。
         * owner（Coroutine 实例 / timer cb）负责 destroy。 */
    }
}

/* Phase 7.4：阻塞 pop——compute worker 主循环用。
 * 与 post 共用 ready_mutex：入队 signal 与这里 wait 互斥，无丢唤醒竞态
 * （wait 前 recheck ready_head，signal 到达时必在锁外之后被观察到）。 */
lm_co_t* lm_scheduler_pop_blocking(lm_scheduler_t* s) {
    if (!s) return NULL;
    pthread_mutex_lock(&s->ready_mutex);
    while (!s->ready_head && !s->stop) {
        pthread_cond_wait(&s->idle_cond, &s->ready_mutex);
    }
    lm_co_t* co = s->ready_head;
    if (co) {
        s->ready_head = co->next;
        if (!s->ready_head) s->ready_tail = NULL;
        co->next = NULL;
        co->queued = 0;
    }
    pthread_mutex_unlock(&s->ready_mutex);
    return co;
}

/* Phase 7.4：yield 后迁移处理（IO drain 与 compute worker 循环统一调用）。
 * 目标为 compute scheduler（reactor=NULL）时 post 内 signal 唤醒 worker；
 * 目标为 IO scheduler 时 lm_scheduler_wakeup 额外写 self-pipe 唤醒 reactor。 */
void lm_scheduler_handle_migrate(lm_co_t* co) {
    if (!co || !co->migrate_sched) return;
    lm_scheduler_t* target = co->migrate_sched;
    co->migrate_sched = NULL;
    lm_scheduler_wakeup(target, co);
}

void lm_scheduler_stop(lm_scheduler_t* s) {
    if (!s) return;
    s->stop = 1;
}
