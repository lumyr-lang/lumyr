// lm_cond.c —— 协程条件变量实现（Phase 8.3：收敛到 butex）
// 语义与契约见 lm_cond.h。
#include "lm_cond.h"
#include "lm_butex.h"
#include <stdlib.h>

lm_cond_t* lm_cond_new(void) {
    lm_cond_t* c = (lm_cond_t*)calloc(1, sizeof(lm_cond_t));
    if (!c) return NULL;
    atomic_init(&c->word, 0);
    return c;
}

void lm_cond_destroy(lm_cond_t* cond) {
    /* 仍有等待者时由 owner 管（不自动 wakeup，语义同旧版）。 */
    free(cond);
}

void lm_cond_wait(lm_cond_t* cond) {
    if (!cond) return;
    uint32_t expected = atomic_load_explicit(&cond->word, memory_order_acquire);
    /* butex 内部：协程挂入 waiter 表 + yield；唤醒方定向 post 回本 scheduler。
     * 非协程/无 scheduler 上下文时 butex 不睡立即返回（spurious 语义）。 */
    lm_butex_wait(&cond->word, expected);
}

void lm_cond_signal(lm_cond_t* cond) {
    if (!cond) return;
    /* butex 契约：先改字、后唤醒 */
    atomic_fetch_add_explicit(&cond->word, 1, memory_order_release);
    lm_butex_wake(&cond->word, 1);
}

void lm_cond_broadcast(lm_cond_t* cond) {
    if (!cond) return;
    /* 一次字变更：所有等待者的 expected 均为旧值，wake_all 全部命中 */
    atomic_fetch_add_explicit(&cond->word, 1, memory_order_release);
    lm_butex_wake_all(&cond->word);
}
