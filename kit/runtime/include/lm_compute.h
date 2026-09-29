// lm_compute.h —— compute worker 池（Phase 7.4）
// 对标 lthread compute scheduler：CPU 密集协程迁入独立 worker 线程池执行，
// 不卡 IO reactor——IO 协程（reactor 驱动）与 CPU 协程（worker 池驱动）分离。
//
// 设计要点：
//   - 池懒初始化：第一次 computeBegin 时创建 N 个 worker（N=CPU 核数），
//     每个 worker 线程各持一个无 reactor 的 lm_scheduler_t（reactor=NULL），
//     主循环阻塞 pop（idle_cond 等待）+ resume，computeEnd 后协程 post 回老家。
//   - 迁移协议（见 lm_co.h home_sched/migrate_sched 注释）：协程栈内只设标记
//     + yield，post 由调度方在 resume 返回（栈已让出）后执行——防双线程同栈。
//   - 协程在 compute 段内不得调用 IO 原语（coSleep/recv/send 等）：worker 无
//     reactor，timer/fd 事件无处挂靠。compute 段只做纯 CPU 计算。
//   - 池生命周期 = 进程生命周期（worker 为 detached 线程，进程退出自动回收），
//     与常驻服务场景一致，不提供显式 shutdown（避免过度设计）。
#ifndef LM_COMPUTE_H
#define LM_COMPUTE_H

#include "lm_scheduler.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 初始化 compute 池（N=CPU 核数 worker 线程）。幂等：已初始化直接返回 0。
 * 部分 worker 创建失败时已成功的 worker 仍可用（返回 0 但池变小）；
 * 全部失败返回 -1。 */
int lm_compute_init(void);

/* 当前协程迁入 compute 池：挑 worker（round-robin），记录老家 IO scheduler
 * 与迁移目标，yield 让出（IO 线程继续跑，drain_ready 返回后 post 到 worker）。
 * 必须在协程栈内、本线程有 scheduler 时调用。
 * 返回 0 成功；-1 上下文错误（不在协程内 / 无 scheduler）；-2 已在 compute
 * 上下文（嵌套 begin）；-3 池创建失败。 */
int lm_compute_begin(void);

/* 当前协程迁回老家 IO scheduler：设迁移目标 = 老家 + yield 让出（compute
 * worker 继续跑下一个任务，worker 循环 resume 返回后 post 回家 + 唤醒 IO
 * reactor）。必须先 computeBegin（home_sched 非空）。
 * 返回 0 成功；-1 不在 compute 上下文。 */
int lm_compute_end(void);

/* Phase 8.5 E：sysmon 强制迁移用——取一个 compute 池 scheduler（round-robin）。
 * 池未初始化时自动 init；init 失败返回 NULL。返回的 scheduler reactor=NULL。 */
lm_scheduler_t* lm_compute_pool_scheduler(void);

/* ============================================================
 * Phase 8.10：线程弹性——按需扩 worker + 硬上限 + 运行期调核
 * 借鉴 bthread signal_task 不足时 add_workers 当场扩容（task_control.cpp:685-693）
 * + Go newm/maxmcount=10000 硬上限思路（proc.go:2875、876）。
 * 弹性只作用于 compute 池与 blocking 池；IO 线程因 SO_REUSEPORT 绑定
 * listen fd 不参与弹性。
 * ============================================================ */

/* 硬上限：env LM_MAX_WORKERS 可配，默认 4×CPU 数，封顶 256。
 * 弹性必须有顶——线程风暴是事故不是弹性（对齐 maxmcount 思路）。 */
#define LM_MAX_WORKERS_CAP 256

/* 按需扩容 1 个 worker（由 overflow_wake_one 唤醒不足时调用）。
 * 未达硬上限才扩；新建 worker 即注册进 g_scheds 参与窃取。
 * 返回 0 成功扩容；-1 已达上限 / 池未初始化 / 创建失败。 */
int lm_compute_pool_maybe_grow(void);

/* 运行期增 worker：扩容 n 个（受硬上限约束）。返回实际新增数，-1 错误。 */
int lm_compute_pool_grow(int n);

/* 运行期减 worker：标记 n 个 worker 拒收新任务（reject_new），worker 排空
 * 本地队列后优雅退出（不做强杀）。从池尾部摘（后进先出）。
 * 返回实际标记数，-1 错误。至少保留 1 个 worker。 */
int lm_compute_pool_shrink(int n);

/* 当前 compute worker 数。 */
int lm_compute_pool_size(void);

/* compute worker 硬上限（max_workers 解析结果）。 */
int lm_compute_pool_capacity(void);

#ifdef __cplusplus
}
#endif

#endif /* LM_COMPUTE_H */
