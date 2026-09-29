// lm_butex.h —— 统一 futex 级阻塞原语（Phase 8.3）
// 对标 brpc butex + parking_lot ParkingLot：一个 32 位原子字同时服务
// 协程（bthread）与线程（pthread）。lumyr 的 cond / 后续 mutex / channel /
// join 全部收敛在这一个原语上——一处实现、一处优化。
//
// 双模式等待（按调用上下文自动分流）：
//   - 协程内 wait（lm_co_current 非空）：值比较通过则把协程挂入 butex
//     waiter 表后 yield（不占线程）；唤醒时定向 post 回协程所属 scheduler
//     （栈数据亲和，沿用 8.2 pinned 定向语义，不入 WSQ）。
//   - 线程内 wait（compute worker 主循环等无协程上下文处）：走平台 futex
//     封装。Linux = SYS_futex FUTEX_WAIT/WAKE_PRIVATE；macOS = __ulock_wait/
//     __ulock_wake（Chromium/Rust std/libc++ atomic_wait 生产使用），
//     syscall 不可用时（ENOSYS）保底 pthread cond。
//
// 使用契约（丢唤醒防护的核心，等待方与唤醒方共同遵守）：
//   等待方：lm_butex_wait(addr, expected) —— 仅当 *addr == expected 才入睡；
//           *addr 已变更时立即返回。内部"入队"在哈希桶锁内 recheck 当前值，
//           与唤醒方"先改字、后 wake"的顺序配对，关闭丢唤醒窗口。
//   唤醒方：必须【先】修改 *addr（如 atomic_fetch_add），【再】调 lm_butex_wake；
//           无 waiter 时 wake 不发任何系统调用（S4 验收口径）。
//
// 边界约定：
//   - fd 等待【不走】butex（对标 bthread：fd 等待走 EventDispatcher+epoll，
//     唤醒源是内核事件；butex 只承担唤醒源为用户态写的同步原语）。
//   - 协程等待必须在 scheduler 上下文（lm_scheduler_get_current 非空）：
//     跨线程唤醒须有投递目标；无 scheduler 时 wait 不入睡（约定同 lm_cond）。
#ifndef LM_BUTEX_H
#define LM_BUTEX_H

#include <stdint.h>
#include <stdatomic.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 阻塞等待：仅当 *addr == expected 时挂起当前执行流。
 * 返回值：0 = *addr 已变更未入睡；1 = 入睡后被唤醒。
 * addr 为 NULL 等非法入参时按未入睡返回（0）。 */
int lm_butex_wait(volatile _Atomic uint32_t* addr, uint32_t expected);

/* 唤醒至多 maxCount 个同一 addr 上的等待者（协程/线程混合队列）。
 * 返回实际唤醒数；无 waiter 时 0 系统调用并返回 0。
 * maxCount <= 0 为 no-op。 */
int lm_butex_wake(volatile _Atomic uint32_t* addr, int maxCount);

/* 唤醒同一 addr 上的全部等待者。返回实际唤醒数。 */
int lm_butex_wake_all(volatile _Atomic uint32_t* addr);

/* ============================================================
 * Phase 8.4：裸 futex 等待/唤醒（timer 线程专用，无 entry 表）
 * lm_butex_wait/wake 走哈希分桶 entry 表，支持协程+线程混合队列、
 * 多等待者广播；但 timer 线程只有一个等待者（自身），且无须协程
 * 感知——直接走平台 syscall，跳过 entry 表分配/摘链开销。
 *
 * lm_futex_wait：仅当 *word == expected 时挂起当前【线程】（非协程）；
 *   timeout_us < 0 无限等待，>=0 超时返回。返回 0=值已变更未睡或超时
 *   返回，1=入睡后被唤醒。caller 返回后须自行 recheck 条件与时间。
 * lm_futex_wake：唤醒一个在 word 上等待的线程。无等待者时无 syscall。
 * ============================================================ */
int lm_futex_wait(volatile _Atomic uint32_t* word, uint32_t expected, int64_t timeout_us);
int lm_futex_wake(volatile _Atomic uint32_t* word);

/* ---- S4 统计（8.11 可观测指标族预留）---- */

/* 无 waiter 的空唤醒次数（wake 调用时桶内无匹配 entry）。 */
long lm_butex_empty_wakes(void);

/* 线程等待者实际陷入平台阻塞 syscall 的次数（futex/__ulock_wait）。 */
long lm_butex_park_syscalls(void);

#ifdef __cplusplus
}
#endif

#endif /* LM_BUTEX_H */
