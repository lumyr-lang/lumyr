// lm_blocking_pool.h —— Phase 8.5 F：阻塞 syscall 流放池
// 设计：
//   独立 OS 线程池，承接阻塞 syscall（DNS 查询、文件 IO、C FFI 阻塞调用），
//   不卡 IO reactor 也不占 compute worker——阻塞工作在池内线程执行，
//   完成后经 lm_scheduler_wakeup 回投原协程到其 home scheduler。
//
// 生命周期（10s 空闲保活）：
//   - 按需扩容：提交时无空闲线程则新建（上限 LM_BLOCKING_MAX=512）；
//   - 空闲收缩：worker 无任务时 futex_wait 10s，超时且线程数 > min 则退出；
//   - min = 1（保底一个常驻）。
//
// 8.5 先交付池子 + 提交/回投机制，具体把哪些调用改走 blocking 池列入后续
// （避免一次性改太多）。
#ifndef LM_BLOCKING_POOL_H
#define LM_BLOCKING_POOL_H

#include "lm_co.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 阻塞任务回调签名：fn(arg) 在池内线程执行，返回值传给 done_cb。 */
typedef void* (*lm_blocking_fn)(void* arg);

/* 完成回调：在池内线程执行，负责把协程回投到其 home scheduler。
 * 参数：co = 提交时传入的协程，result = fn 的返回值。 */
typedef void (*lm_blocking_done_cb)(lm_co_t* co, void* result);

/* 提交阻塞任务到池：fn(arg) 在池内线程执行，完成后调 done_cb(co, result)。
 * co 可为 NULL（纯异步任务无协程回投）。返回 0 成功，-1 池满或 OOM。 */
int lm_blocking_submit(lm_blocking_fn fn, void* arg,
                       lm_blocking_done_cb done_cb, lm_co_t* co);

/* 协程内同步等待阻塞调用：fn(arg) 流放 blocking 池执行，当前协程 yield，
 * 完成后协程被回投到提交时所在 scheduler 续行（调度线程全程不阻塞）。
 * 结果经 resultOut 输出（可为 NULL）。
 * 返回 0 成功；-1 不在协程/调度器上下文；-2 提交失败（OOM/池停止）。
 * 非 0 返回时调用方应自行回退为直接同步执行，保证功能可用。 */
int lm_co_await_blocking(lm_blocking_fn fn, void* arg, void** resultOut);

/* 池内当前线程数（监控用）。 */
int lm_blocking_pool_size(void);

/* 池内空闲线程数（监控用）。 */
int lm_blocking_pool_idle(void);

/* Phase 8.13：查询协程的 blocking 任务是否仍在册（队列中或执行中）。
 * sysmon stuck 协程检测用：在册 = 回投尚未发生 = 协程悬挂属正常等待；
 * 不在册且协程仍 SUSPENDED 未入队 = 回投丢失（疑似丢唤醒）。 */
int lm_blocking_pool_task_pending(lm_co_t* co);

#ifdef __cplusplus
}
#endif

#endif /* LM_BLOCKING_POOL_H */
