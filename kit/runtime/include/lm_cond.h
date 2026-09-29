// lm_cond.h —— 协程条件变量（Phase 7.3 引入；Phase 8.3 收敛到 butex）
// wait/signal/broadcast 全部基于 lm_butex 单原子字实现——
// 不再维护私有 waiter 队列，唤醒语义（丢唤醒防护、无 waiter 零系统调用）
// 统一由 butex 提供。
//
// 语义：
//   - wait：以当前字值为 expected 调 lm_butex_wait——仅当字未被
//     signal/broadcast 改变时挂起。须在协程内、scheduler 上下文调用；
//     非协程/无 scheduler 时不睡立即返回（与旧实现 no-op 等价，
//     谓词循环调用方按 spurious wakeup 处理）。
//   - signal：先改字（fetch_add）、后 lm_butex_wake 一个 waiter。
//   - broadcast：先改字、后 lm_butex_wake_all（一次字变更唤醒全部）。
//
// 使用约束：
//   - 标准谓词循环用法：while (条件不满足) cond.wait()。
//   - destroy 时仍有等待者的语义不变：由 owner 管（不自动 wakeup）。
#ifndef LM_COND_H
#define LM_COND_H

#include <stdint.h>
#include <stdatomic.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct lm_cond {
    _Atomic uint32_t word;        /* butex 状态字：signal/broadcast 递增 */
} lm_cond_t;

/* 创建条件变量。失败返回 NULL。 */
lm_cond_t* lm_cond_new(void);
void lm_cond_destroy(lm_cond_t* cond);

/* wait：挂起当前协程直到被 signal/broadcast（语义见文件头）。 */
void lm_cond_wait(lm_cond_t* cond);

/* signal：唤醒一个 waiter。无 waiter 时无系统调用。 */
void lm_cond_signal(lm_cond_t* cond);

/* broadcast：唤醒全部 waiter。 */
void lm_cond_broadcast(lm_cond_t* cond);

#ifdef __cplusplus
}
#endif

#endif /* LM_COND_H */
