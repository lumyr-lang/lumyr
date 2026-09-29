// lm_timer.h —— 集中化定时器线程（Phase 8.4）
// 借鉴 brpc bthread timer_thread：全局单 TimerThread + 分桶 + 最小堆。
//
// 设计依据（phase8-mn-scheduler-research.md G4 缺口）：
//   lumyr 定时器以 IO 超时为主——秒级、稀疏、大量取消。最小堆插入/取消
//   O(log N) 且支持任意精度，堆顶即最近到期；分桶解决多生产者对单堆的
//   schedule 锁竞争（pthread id hash % numBuckets，对齐 timer_thread 的
//   _nearest_run_time per-bucket）。不用时间轮：时间轮精度受 tick 约束，
//   lumyr 超时需 ms 级任意精度，堆更合适。
//
// 与旧实现差异（每 reactor 各自最小堆 → 全局集中）：
//   - 旧：每 reactor 线程各自维护最小堆（N 线程 N 份超时计算），
//         跨线程定时器投递仍需 self-pipe。
//   - 新：全局单 TimerThread 独立线程，到期回调直接投递回注册归属
//         scheduler（lm_scheduler_wakeup）；reactor 主循环只读原子快照
//         lm_timer_nearest_ms() 计算 epoll_wait/kevent 超时，无堆操作。
//
// 回调契约（MUST NOT block，对齐 timer_thread.h:50-52）：
//   到期回调在 timer 线程上下文执行——严禁阻塞（如 IO、锁长持有），
//   否则拖延所有其他定时器的到期精度。需阻塞的工作应 post 协程到
//   scheduler 由 reactor 线程跑，不在 timer 线程内做。
#ifndef LM_TIMER_H
#define LM_TIMER_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 定时器 ID（>0 有效；0 = 无效）。编码 (version<<32)|slot_index，
 * version 每次分配 +2（跳过 0 初值），cancel 后 version 再 +2，
 * 使旧 id 的 version 永不与活跃 id 复用——cancel 仲裁只认当前 version。 */
typedef uint64_t lm_timer_id_t;
#define LM_TIMER_INVALID_ID 0ULL

/* 到期回调：在 timer 线程执行；MUST NOT block。
 * id 为触发的定时器 ID，arg 为 lm_timer_add 注册时的 arg。 */
typedef void (*lm_timer_fn_t)(lm_timer_id_t id, void* arg);

/* 启动全局定时器线程。numBuckets=0 取 CPU 数（clamp 4..1024）。
 * 重复启动为 no-op（已启动则返回 0）。返回 0 成功，-1 失败。
 * 进程退出前调 lm_timer_thread_stop 回收线程。 */
int lm_timer_thread_start(size_t numBuckets);

/* 停止全局定时器线程（唤醒并 join）。可重复调用。 */
void lm_timer_thread_stop(void);

/* 注册定时器。deadline_ms 为绝对单调时间（CLOCK_MONOTONIC 毫秒）。
 * 返回 timer_id（>0），失败 LM_TIMER_INVALID_ID。到期时 fn(id, arg)
 * 在 timer 线程执行。线程安全。 */
lm_timer_id_t lm_timer_add(uint64_t deadline_ms, lm_timer_fn_t fn, void* arg);

/* 取消定时器。返回 0=已取消，-1=未找到（已到期/已取消/无效），
 * 1=正在执行（cb 已在 timer 线程跑，不可打断）。
 * 用 version 仲裁：CAS version removed 标记，防止 ABA 复用。 */
int lm_timer_cancel(lm_timer_id_t id);

/* 取最近到期时间（绝对 ms）。无定时器返回 UINT64_MAX。
 * 原子快照（无锁），供各 reactor 线程读 epoll_wait/kevent 超时用。 */
uint64_t lm_timer_nearest_ms(void);

#ifdef __cplusplus
}
#endif

#endif /* LM_TIMER_H */
