// lm_cond.c —— 协程条件变量实现（Phase 7.3 跨线程唤醒原语）
// waiter 队列（mutex 保护的 FIFO 单链）+ wait yield / signal wakeup。
// 详见 lm_cond.h 设计说明。
#include "lm_cond.h"
#include <stdlib.h>

lm_cond_t* lm_cond_new(void) {
    lm_cond_t* c = (lm_cond_t*)calloc(1, sizeof(lm_cond_t));
    if (!c) return NULL;
    pthread_mutex_init(&c->mutex, NULL);
    return c;
}

void lm_cond_destroy(lm_cond_t* cond) {
    if (!cond) return;
    /* waiter 队列内残留协程由 owner 管（不自动 wakeup，避免语义歧义）。
     * 仅释放 node 结构 + cond 本身。 */
    pthread_mutex_lock(&cond->mutex);
    lm_cond_node_t* cur = cond->head;
    while (cur) {
        lm_cond_node_t* next = cur->next;
        free(cur);
        cur = next;
    }
    pthread_mutex_unlock(&cond->mutex);
    pthread_mutex_destroy(&cond->mutex);
    free(cond);
}

void lm_cond_wait(lm_cond_t* cond) {
    if (!cond) return;
    lm_co_t* co = lm_co_current();
    lm_scheduler_t* sched = lm_scheduler_get_current();
    if (!co || !sched) return;  /* 非协程上下文或无 scheduler，no-op */
    /* 挂当前协程到 waiter 队尾 */
    lm_cond_node_t* node = (lm_cond_node_t*)malloc(sizeof(lm_cond_node_t));
    if (!node) return;
    node->co = co;
    node->sched = sched;
    node->next = NULL;
    pthread_mutex_lock(&cond->mutex);
    if (cond->tail) {
        cond->tail->next = node;
    } else {
        cond->head = node;
    }
    cond->tail = node;
    pthread_mutex_unlock(&cond->mutex);
    /* yield 切回 reactor：协程挂起在 waiter 队列，等 signal wakeup
     * （signal 调 lm_scheduler_wakeup 投递到 sched + reactor self-pipe 唤醒，
     *  下一轮 drain_ready resume 本协程从 yield 后继续）。 */
    lm_co_yield();
}

void lm_cond_signal(lm_cond_t* cond) {
    if (!cond) return;
    /* 从 waiter 队头取一个，wakeup 到它的 scheduler */
    pthread_mutex_lock(&cond->mutex);
    lm_cond_node_t* node = cond->head;
    if (node) {
        cond->head = node->next;
        if (!cond->head) cond->tail = NULL;
    }
    pthread_mutex_unlock(&cond->mutex);
    if (node) {
        lm_scheduler_wakeup(node->sched, node->co);
        free(node);
    }
}

void lm_cond_broadcast(lm_cond_t* cond) {
    if (!cond) return;
    /* 取整个 waiter 队列，逐个 wakeup */
    pthread_mutex_lock(&cond->mutex);
    lm_cond_node_t* cur = cond->head;
    cond->head = NULL;
    cond->tail = NULL;
    pthread_mutex_unlock(&cond->mutex);
    while (cur) {
        lm_cond_node_t* next = cur->next;
        lm_scheduler_wakeup(cur->sched, cur->co);
        free(cur);
        cur = next;
    }
}
