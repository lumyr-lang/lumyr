// vm_co.h —— 协程 VM 状态管理（Phase 5）
// 协程在同线程复用 _Thread_local 的 g_stack_mgr + 异常栈，yield/resume 必须保存/恢复。
// 本模块提供：
//   - VMCoState 结构（g_stack_mgr sp + g_err_jmp + VMExceptState + result/error）
//   - resume/yield/release hook 实现（注册给 lm_co）
//   - reactor 基线 enter/leave（BUILTIN_REACTOR_RUN 调用）
//   - 协程 result/error 设置（trampoline 写入）
//   - co_spawn trampoline（BUILTIN_CO_SPAWN 调用，含协程级错误隔离）
#ifndef LUMYR_VM_CO_H
#define LUMYR_VM_CO_H

#include "vm_types.h"   /* Value（经 lm_value.h 传递引入） */
#include "lm_co.h"       /* lm_co_t（typedef struct lm_co_s） */
#include "lm_reactor.h"  /* lm_reactor_t（typedef struct lm_reactor_s） */

#ifdef __cplusplus
extern "C" {
#endif

/* 注册 resume/yield/release hook 给 lm_co。
 * 在 vm 初始化时调（vm_run_main 或 vm_execute 首次进入）。
 * 注册后，lm_co_resume/yield/destroy 会调 hook 保存/恢复 _Thread_local 状态。 */
void vm_co_hooks_register(void);

/* reactor 基线管理：BUILTIN_REACTOR_RUN 进入时 enter（保存当前 _Thread_local），
 * 返回时 leave（恢复基线，释放）。协程 yield 时恢复到基线。 */
void vm_co_enter_reactor_baseline(void);
void vm_co_leave_reactor_baseline(void);

/* 协程 result/error 设置（trampoline 写入 co->vm_state） */
void vm_co_set_result(lm_co_t* co, Value result);
void vm_co_set_error(lm_co_t* co, Value error);

/* 创建协程并跑 lm 函数（BUILTIN_CO_SPAWN 调用）。
 * 分配 CoSpawnCtx（func+arg+co）+ VMCoState（全 0，协程从空状态开始），
 * lm_co_spawn 用 vm_co_trampoline 作 entry。
 * 失败返回 NULL。返回的 lm_co_t 由调用方负责 resume/destroy。 */
lm_co_t* vm_co_spawn(Value func, Value arg);

/* reactor addTimer(ms, cb) 的 lm 回调封装（BUILTIN_REACTOR_ADD_TIMER 调用）。
 * timer 触发时 spawn 协程跑 cb，传 timer_id 作参数。
 * Phase 8.4：timer 集中到全局 TimerThread，返回 lm_timer_id_t（>0），
 * 失败 LM_TIMER_INVALID_ID（0）。 */
lm_timer_id_t vm_co_add_timer(lm_reactor_t* r, uint64_t ms, Value cb);

#ifdef __cplusplus
}
#endif

#endif /* LUMYR_VM_CO_H */
