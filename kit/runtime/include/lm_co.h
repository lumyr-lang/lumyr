// lm_co.h —— 有栈协程（切换层：Phase 8.1 起默认 fcontext 汇编切换）
// 让 native 阻塞调用（如 recv/send）在协程内 yield，reactor 调度回来 resume。
// C 调用栈被冻结，yield 点继续执行——VM 的 C 递归调用栈（vm_exec_loop →
// vm_bind_and_run → lumyr_socket_recv → lm_co_yield）跨 yield 完整保留，
// 不需要改造 VM 字节码或 ir_cgen。
//
// 协程挂起时整条 C 调用栈冻结，GC 必须把该栈当根扫（Phase 3 gc_register_coroutine）。
//
// 栈分配：mmap + 末页 guard page（PROT_NONE），深递归爆栈触发 SIGSEGV 而非静默破坏。
// 默认 128KiB，可配。
//
// 切换原语见 lm_coro_ctx.h：默认 fcontext 纯用户态汇编切换（x86_64/arm64），
// -DLM_CTX_UCONTEXT 回退 POSIX ucontext（排障对照）。
#ifndef LM_CO_H
#define LM_CO_H

#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>   /* Phase 8.5：preempt_flag _Atomic + atomic_init */
#include "lm_coro_ctx.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LM_CO_READY = 0,     /* 已创建待调度 */
    LM_CO_RUNNING = 1,   /* 正在执行 */
    LM_CO_SUSPENDED = 2, /* yield 挂起等待事件 */
    LM_CO_DEAD = 3,      /* 已结束（entry 返回或异常） */
} lm_co_state_t;

typedef struct lm_co_s {
    int co_id;
    void* stack_mmap;        /* mmap 起点（低地址），用于 munmap */
    size_t stack_size;      /* 可用栈大小（不含 guard page） */
    lm_ctx_t ctx;            /* 协程上下文（lm_ctx_make 写入 / lm_ctx_jump 保存） */
    lm_ctx_t resume_ctx;     /* resume 调用方的上下文（yield / DEAD 切回目标） */
    lm_co_state_t state;
    void (*entry)(void*);   /* 协程入口 */
    void* arg;
    /* Phase 3 GC 集成时记录栈区间供扫描（挂起时注册给 GC） */
    void* stack_base;        /* 栈基址（高地址，可用区末） */
    void* stack_top;         /* 栈顶（低地址，可用区起） */
    struct lm_co_s* next;    /* 就绪队列链 */
    /* Phase 5 VM 状态快照：协程在同线程复用 _Thread_local 的 g_stack_mgr
     * 与异常栈（g_try_stack/g_unwind/g_fin_stack/g_err_jmp/g_thread_root 等），
     * yield/resume 必须保存/恢复这组状态，否则 reactor resume 不同协程会互相污染。
     * vm_state 为不透明指针，由 vm 层（vm_co.c）分配/释放/读写，kit/runtime 不直接访问。
     * 未注册 hook 时为 NULL，协程不复用 VM 状态则无影响。 */
    void* vm_state;
    /* Phase 7.2：scheduler 就绪队列入队标记，防重复入队。
     * post 时置 1，pop 时清 0。无 scheduler 时恒 0（直连 resume 无入队）。
     * 放在 vm_state 之后，不改变 vm_state 偏移（排查 _ctx_done 崩溃）。 */
    int queued;
    /* Phase 7.4：compute 池迁移标记（加在结构体末尾，不改变现有字段偏移——ABI 原则）。
     * home_sched：computeBegin 时记录老家 IO scheduler（非空 = 协程处于 compute 池）；
     * migrate_sched：yield 让出后由调度方 post 到的目标 scheduler。
     * 迁移协议（对标 lthread PENDING 模式）：协程栈内只设标记 + yield，真正的
     * post 由调度方在 lm_co_resume 返回（栈已让出）后执行——防双线程同栈竞态
     * （若 yield 前 post，worker 可能抢在 yield 前切栈 → 两线程同时在一条栈上）。 */
    struct lm_scheduler_s* home_sched;
    struct lm_scheduler_s* migrate_sched;
    /* ASAN fiber 注解状态（仅 ASAN 构建由 lm_co.c 内部读写，恒占字段保持布局一致）：
     * asan_fake 本协程的 fake stack 保存槽；asan_caller_bottom/size 为 resume
     * 调用方栈区间（低地址底 + 大小），yield/结束切回时的目标栈声明。 */
    void* asan_fake;
    void* asan_caller_bottom;
    size_t asan_caller_size;
    /* Phase 8.2：调度分流标志（结构体末尾追加，保持既有字段偏移——ABI 原则）。
     * pinned：业务级定向——协程绑定本线程 reactor 的 fd（fd 等待自动置位、
     *         accept loop 显式置位、协程内 spawn 继承父值），pinned 协程
     *         只走目标 scheduler 的 mutex 定向队列，永不入 WSQ / 不被窃取。
     * stealable：机制级迁移安全——由 vm hook 维护：spawn 时 1（未运行无栈数据），
     *         yield 时有 mig_copy（compute 迁移）则 1，否则 0（VM 栈数据物理
     *         绑定本线程栈池，跨线程 resume 必须走 mig_copy 恢复路径）。
     * scheduler post 分流：pinned || !stealable → mutex 定向队列；
     *         中立可偷 → 本线程 WSQ / 跨线程全局队列。 */
    int pinned;
    int stealable;
    /* Phase 8.5：分通道抢占预算字段（结构体末尾追加，ABI 不变）。
     * reds：当前协程剩余 reduction 预算，resume 时装载 LM_SCHED_REDS(4000)，
     *       VM 通道由派发 handler 在 CALL/RETURN/JMP 回边/BUILTIN 扣减，
     *       cc 通道由生成代码在序言/回边扣减（两通道共用同字段）。
     *       预算耗尽在指令边界 lm_co_yield() 让出——VM 状态一致，无信号强抢。
     * last_resume_ns：上次 resume 的单调时间（CLOCK_MONOTONIC ns），
     *       drain 内长调度告警 + sysmon 跨 scheduler 扫描用。
     * preempt_flag：sysmon 异步置位（cc 通道轮询用；VM 通道走预算耗尽让出，暂不用）。 */
    int32_t reds;
    uint64_t last_resume_ns;
    _Atomic int preempt_flag;
    /* Phase 8.5：时间片耗尽让出标记。LM_BUMP_REDS 预算耗尽调 lm_co_yield 前置 1，
     * drain_ready 在 resume 返回后检测：若 slice_yield=1 则重新投递本协程到就绪队列
     * （协程已让出栈，post 安全），使 tight loop 协程按时间片轮转而非霸占线程。
     * 事件型 yield（fd/butex/compute 迁移）不设此标记——由事件回调/migrate 重入队。 */
    int slice_yield;
} lm_co_t;

typedef void (*lm_co_entry_t)(void*);

/* Phase 8.5：reduction 预算参数（对齐 BEAM CONTEXT_REDS=4000，erl_vm.h:53）。
 * LM_SCHED_REDS：每次 resume 装载的预算值；
 * LM_SCHED_MIN_REDS：最小切换钳制（=CONTEXT_REDS/10=400，erl_process.c:67），
 *   消耗 < 400 按 400 记账，防"换进即换出"协程白嫖。 */
#define LM_SCHED_REDS      4000
#define LM_SCHED_MIN_REDS  (LM_SCHED_REDS / 10)

/* Phase 8.5：reduction 扣减宏（对齐 BEAM bif.h:71 BUMP_REDS / :80 BUMP_ALL_REDS）。
 * LM_BUMP_REDS(co)：扣 1 预算，归零则在指令边界 lm_co_yield() 让出（VM 状态一致）。
 *   仅在协程上下文（co != NULL）且 reds > 0 时扣减，避免无协程场景误触发。
 * LM_BUMP_ALL_REDS(co)：强制清零预算，下次派发点必让出（长 C 内建主动让步用）。
 * 宏内联零函数调用，扣减与 handler 同栈帧。 */
#define LM_BUMP_REDS(co) do {                                       \
    lm_co_t* _co = (co);                                            \
    if (_co && _co->reds > 0 && --_co->reds <= 0) {                 \
        _co->slice_yield = 1;   /* 标记时间片耗尽，drain_ready 重入队 */  \
        lm_co_yield();                                              \
    }                                                               \
} while (0)

#define LM_BUMP_ALL_REDS(co) do {                                   \
    lm_co_t* _co = (co);                                            \
    if (_co) _co->reds = 0;                                         \
} while (0)

/* 默认栈大小（128KiB，可配）。深递归 lumin 函数应显式调大。
 * 64KiB 在深嵌套 VM 调用热点（accept loop → createHandler → ctor → spawn，
 * 每层 vm_exec_loop + vm_call_func_value + vm_exec_call_method_dyn + builtin_dispatch
 * 栈帧较大）会被顶满触发 SIGBUS。128KiB 是 libco 默认值，足够覆盖 4+ 层 VM 嵌套。 */
#define LM_CO_DEFAULT_STACK_SIZE (128 * 1024)

/* 创建协程：分配 stack（mmap + 末页 guard page），makecontext 设置 entry。
 * 不立即执行，state=READY。返回 NULL 失败。
 * 调用方负责把 co 加入 reactor 就绪队列，reactor 主循环会调 lm_co_resume。
 * stack_size=0 用默认值。 */
lm_co_t* lm_co_spawn(lm_co_entry_t entry, void* arg, size_t stack_size);

/* resume 协程：从挂起点继续执行；协程内 lm_co_yield 会切回这里。
 * 协程 DEAD 状态 resume 是 no-op。entry 返回后自动转 DEAD（uc_link 自动切回）。 */
void lm_co_resume(lm_co_t* co);

/* 当前协程 yield：切回 resume 调用方（reactor 主循环）。
 * 必须在协程内调用（lm_co_current() != NULL），否则 no-op。
 * yield 前 state=RUNNING → SUSPENDED；resume 回来后 SUSPENDED → RUNNING。 */
void lm_co_yield(void);

/* 取当前协程指针（TLS）。reactor 主循环时返回 NULL（无协程上下文）。 */
lm_co_t* lm_co_current(void);

/* 协程是否已结束（DEAD）。 */
int lm_co_is_dead(lm_co_t* co);

/* 销毁协程：munmap stack，free struct。
 * DEAD 或 SUSPENDED 状态销毁安全；RUNNING 状态销毁未定义行为。 */
void lm_co_destroy(lm_co_t* co);

/* ============================================================
 * Phase 5: VM 状态保存/恢复 hook
 * 协程在同线程复用 _Thread_local 的 g_stack_mgr + 异常栈，yield/resume
 * 必须保存/恢复这组状态。vm 层（vm_co.c）注册 hook 读写 co->vm_state。
 *
 * on_resume(co): swapcontext 切到协程前调——恢复协程 vm_state 到 _Thread_local。
 *                co==NULL 时表示协程 DEAD 切回后恢复 reactor 基线。
 * on_yield(co):  swapcontext 切回 reactor 前调——保存协程 vm_state，恢复基线。
 * on_release(co): lm_co_destroy 前调——释放 co->vm_state 内存（不保存状态）。
 *
 * 调用时机由 lm_co.c 内部控制，外部只负责注册实现。
 * ============================================================ */
typedef void (*lm_co_vm_hook_t)(lm_co_t* co);
void lm_co_set_vm_hooks(lm_co_vm_hook_t on_resume, lm_co_vm_hook_t on_yield,
                        lm_co_vm_hook_t on_release);

#ifdef __cplusplus
}
#endif

#endif /* LM_CO_H */
