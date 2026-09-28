// lm_cond.h —— 协程条件变量（Phase 7.3 跨线程唤醒原语）
// 对标 lthread lthread_cond_t：wait/signal/broadcast。
//
// 设计要点：
//   - waiter 队列：lm_cond_node_t 单链（co + sched），mutex 保护。
//     wait 时挂当前协程到队尾 + yield（切回 reactor 等 signal 唤醒）；
//     signal 时从队头取一个，wakeup（post 到目标 scheduler + reactor self-pipe）。
//   - 跨线程：wait 记录协程所属 scheduler（lm_scheduler_get_current），
//     signal/broadcast 用 lm_scheduler_wakeup 投递到该 scheduler + 唤醒 reactor。
//     线程 A signal → 线程 B 的 reactor 从 epoll_wait 返回 → drain_ready resume 协程。
//   - 无 scheduler 时 wait 是 no-op（非 reactor 上下文，C 测试场景）。
//
// 使用约束：
//   - wait 必须在协程内调用（lm_co_current != NULL）且有 scheduler。
//   - signal/broadcast 可在任意线程调用（mutex 保护 waiter 队列）。
//   - destroy 时 waiter 队列内残留协程由 owner 管（不自动 wakeup，避免语义歧义）。
#ifndef LM_COND_H
#define LM_COND_H

#include "lm_co.h"
#include "lm_scheduler.h"
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct lm_cond_node {
    lm_co_t* co;                  /* 等待协程 */
    lm_scheduler_t* sched;        /* 协程所属 scheduler（wait 时记录，signal 时投递目标） */
    struct lm_cond_node* next;    /* waiter 链 */
} lm_cond_node_t;

typedef struct lm_cond {
    lm_cond_node_t* head;         /* waiter 队列头（FIFO） */
    lm_cond_node_t* tail;         /* waiter 队列尾 */
    pthread_mutex_t mutex;        /* 保护 waiter 队列（跨线程 signal/broadcast） */
} lm_cond_t;

/* 创建条件变量。失败返回 NULL。 */
lm_cond_t* lm_cond_new(void);
void lm_cond_destroy(lm_cond_t* cond);

/* wait：挂当前协程到 waiter 队列 + yield（切回 reactor 等 signal 唤醒）。
 * 必须在协程内调用（lm_co_current != NULL）且有 scheduler。
 * 非协程上下文或无 scheduler 时 no-op（C 测试场景）。 */
void lm_cond_wait(lm_cond_t* cond);

/* signal：唤醒一个 waiter（FIFO 队头）。
 * 从 waiter 队列取一个协程，wakeup 到它的 scheduler + reactor。
 * 可在任意线程调用（mutex 保护）。无 waiter 时 no-op。 */
void lm_cond_signal(lm_cond_t* cond);

/* broadcast：唤醒全部 waiter。
 * 遍历 waiter 队列，逐个 wakeup。可在任意线程调用。 */
void lm_cond_broadcast(lm_cond_t* cond);

#ifdef __cplusplus
}
#endif

#endif /* LM_COND_H */
