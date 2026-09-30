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
    LM_CO_SWAPPING = 4,  /* reaper 正在搬栈（换出中）；resume 见此态自旋等待 */
} lm_co_state_t;

typedef struct lm_co_s {
    int co_id;
    void* stack_mmap;        /* mmap 起点（低地址），用于 munmap */
    size_t stack_size;      /* 可用栈大小（不含 guard page） */
    lm_ctx_t ctx;            /* 协程上下文（lm_ctx_make 写入 / lm_ctx_jump 保存） */
    lm_ctx_t resume_ctx;     /* resume 调用方的上下文（yield / DEAD 切回目标） */
    _Atomic lm_co_state_t state;   /* 原子：resume CAS 抢 RUNNING / reaper CAS 抢 SWAPPING 仲裁栈所有权 */
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
     * 放在 vm_state 之后，不改变 vm_state 偏移（排查 _ctx_done 崩溃）。
     * Phase 8.13：原子化——跨线程并发 post（sysmon stuck 救援重投 vs 迟到的
     * 真唤醒投递）下，非原子 check-then-set 会双过导致同协程重复入队、
     * 两线程并发 resume 同一条栈；lm_scheduler_post 改用 CAS 占位。 */
    _Atomic int queued;
    /* Phase 7.4：compute 池迁移标记（加在结构体末尾，不改变现有字段偏移——ABI 原则）。
     * home_sched：computeBegin 时记录老家 IO scheduler（非空 = 协程处于 compute 池）；
     * migrate_sched：yield 让出后由调度方 post 到的目标 scheduler。
     * 迁移协议（PENDING 模式）：协程栈内只设标记 + yield，真正的
     * post 由调度方在 lm_co_resume 返回（栈已让出）后执行——防双线程同栈竞态
     * （若 yield 前 post，worker 可能抢在 yield 前切栈 → 两线程同时在一条栈上）。 */
    _Atomic(struct lm_scheduler_s*) home_sched;     /* 原子：sysmon 强制迁移与协程 computeBegin/End 并发写 */
    _Atomic(struct lm_scheduler_s*) migrate_sched;
    /* ASAN fiber 注解状态（仅 ASAN 构建由 lm_co.c 内部读写，恒占字段保持布局一致）：
     * asan_fake 本协程的 fake stack 保存槽；asan_caller_bottom/size 为 resume
     * 调用方栈区间（低地址底 + 大小），yield/结束切回时的目标栈声明。 */
    void* asan_fake;
    void* asan_caller_bottom;
    size_t asan_caller_size;
    /* Phase 8.2：调度分流标志（结构体末尾追加，保持既有字段偏移——ABI 原则）。
     * pinned：IO 亲和标志——协程与本线程 reactor 绑定（fd 等待自动置位、
     *         accept loop 显式置位、协程内使用 reactor 网络栈时由 socket
     *         层自动置位、协程内 spawn 继承父值），pinned 协程
     *         只走目标 scheduler 的 mutex 定向队列，永不入 WSQ / 不被窃取，
     *         sysmon 也不得强制迁移：fd 注册与唤醒路径无法随协程迁走。
     * stealable：机制级迁移安全——由 vm hook 维护：spawn 时 1（未运行无栈数据），
     *         yield 时有 mig_copy（compute 迁移）则 1，否则 0（VM 栈数据物理
     *         绑定本线程栈池，跨线程 resume 必须走 mig_copy 恢复路径）。
     * scheduler post 分流：pinned || !stealable → mutex 定向队列；
     *         中立可偷 → 本线程 WSQ / 跨线程全局队列。 */
    int pinned;
    /* 原子：vm yield_hook（owner 线程）/ reaper（swap_out/in）写，
     * scheduler post 分流（任意线程）读——TSAN 登记竞态。 */
    _Atomic int stealable;
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
    /* Phase 8.6：栈分级与 idle 换出字段（结构体末尾追加，ABI 不变）。
     * stack_class：LM_STACK_SMALL(16KiB)/LM_STACK_NORMAL(128KiB)，选栈池桶用。
     *   spawn 时按 stack_size 推断（≤SMALL→SMALL，否则 NORMAL）。VM 协程维持 NORMAL。
     * swap_buffer/swap_size：idle 换出时把已用栈段 memcpy 到堆 buffer（堆栈
     *   缓冲降级——仅 idle>30s 执行，非每切换都拷贝）。
     *   未换出时 swap_buffer=NULL。
     * swap_state：换出/换入互斥状态机（_Atomic，CAS 仲裁换出与唤醒竞态）：
     *   0=NORMAL 1=SWAPPING_OUT(reaper 拷贝中) 2=SWAPPED(无栈,buffer 持内容)
     *   3=SWAPPING_IN(唤醒方拷回中)。resume 入口检测 SWAPPED 触发换入。
     * reg_prev/reg_next：全局协程注册表 intrusive 链，reaper 扫描 idle 候选用。
     *   spawn 加头、destroy 摘除，reaper 取快照后无锁遍历。 */
    int stack_class;
    void*  swap_buffer;
    size_t swap_size;
    _Atomic int swap_state;
    struct lm_co_s* reg_prev;
    struct lm_co_s* reg_next;
    /* Phase 8.8：fd 等待字回溯（结构体末尾追加，ABI 不变）。
     * co_wait_fd_timeout 注册等待时记录当前等待的 conn 方向字（rg/wg）与 conn
     * 指针，cleanup 时清空。协程仍挂起在等待字上被强制 destroy（accept/handler
     * 挂起协程随框架 destroyActive/acceptCo.destroy 释放）时，lm_co_destroy 据此
     * CAS 解仲裁 + 清 conn->co，防 close_notify 唤醒已释放的悬垂协程（UAF）。
     * 无等待时恒 NULL。
     * Phase 8.13：指针自身原子化（_Atomic 限定符置于 * 后：原子指针指向
     * 原子字）——sysmon stuck 扫描跨线程读本字段，与等待登记/清除方并发。 */
    _Atomic uintptr_t* _Atomic waiting_word;
    void* waiting_conn;
    /* Phase 8.11：就绪时间戳（结构体末尾追加，ABI 不变）。
     * post/post_local/post_lifo 入队时记录 CLOCK_MONOTONIC ns；
     * lm_co_resume 抢到 RUNNING 后 exchange 取走清零，差值即 pending_time
     * （ready→被执行延迟）记入全局直方图（lm_sched_stats）。
     * 0 = 不在就绪队列（直连 resume 路径不统计）。跨线程写（投递方）/读（resume 方）。 */
    _Atomic uint64_t ready_ts;
    /* Phase 8.13：等待源登记 + stuck 检测字段（结构体末尾追加，ABI 不变）。
     * wait_kind：协程挂起时登记的等待源类别（LM_WAIT_*），sysmon stuck 扫描
     *   据此校验「等待源状态 vs 协程状态」一致性——精确检测丢唤醒，不依赖
     *   超时猜测（长 idle 连接挂起数小时是正常态，不误报）。
     *   登记顺序：先写等待指针字段（waiting_word/waiting_sched）、最后
     *   store-release wait_kind；清除顺序相反。sysmon acquire 读 kind 后
     *   必见指针字段。
     * waiting_sched：butex 等待登记时 retain 的唤醒目标 scheduler，供 sysmon
     *   救援重投用；等待退出时经 lm_co_release_waiting_sched 在注册表锁内
     *   exchange NULL 后 release（与 sysmon 持锁扫描互斥，关闭
     *   「扫描方 retain vs 等待方 release」竞态导致的 scheduler UAF）。
     * stuck_votes：sysmon 连续疑似计数（>=2 才确认告警）——吸收
     *   「源侧刚完成、投递在飞」的 µs 级瞬态窗口（扫描间隔 1s 下瞬态
     *   不可能跨两轮）。仅 sysmon 单线程读写。 */
    _Atomic int wait_kind;
    _Atomic(struct lm_scheduler_s*) waiting_sched;
    _Atomic int stuck_votes;
    /* R3：coSleep 挂起登记（结构体末尾追加，ABI 不变）。
     * 协程挂起在 coSleep 定时器上时指向 CoSleepCtx（lm_co.c 内部类型），
     * 正常唤醒/destroy 均经 exchange 取走并取消定时器——防 destroy 后
     * 定时器回调 wakeup 悬垂协程（UAF）。无挂起时恒 NULL。
     * 仅协程所属线程（登记/清理）与 destroy 调用方访问，无跨线程读写。 */
    _Atomic(void*) sleep_ctx;
} lm_co_t;

typedef void (*lm_co_entry_t)(void*);

/* Phase 8.5：reduction 预算参数。
 * LM_SCHED_REDS：每次 resume 装载的预算值；
 * LM_SCHED_MIN_REDS：最小切换钳制（=预算/10=400），
 *   消耗 < 400 按 400 记账，防"换进即换出"协程白嫖。 */
#define LM_SCHED_REDS      4000
#define LM_SCHED_MIN_REDS  (LM_SCHED_REDS / 10)

/* Phase 8.5：reduction 扣减宏。
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

/* ============================================================
 * Phase 8.5 I：cc 通道 ABI 契约（编译器生成代码 reduction 扣减）
 *
 * cc 通道（ir_cgen 生成的原生代码通道）与 VM 通道共用 lm_co_t.reds 字段。
 * 契约：cc 通道生成代码在函数序言 + 循环回边生成如下指令序列：
 *
 *   ; 函数序言
 *   mov  rAx, [co + offsetof(reds)]      ; 装载预算
 *
 *   ; 循环回边（back-edge）
 *   dec  rAx                              ; 扣 1
 *   jnz  .loop_body                       ; >0 继续
 *   ; 预算耗尽：保存活值到栈帧，调 lm_co_yield
 *   mov  [co + offsetof(slice_yield)], 1  ; 标记时间片耗尽
 *   call lm_co_yield                      ; 指令边界让出（栈帧一致）
 *   ; yield 返回后重装预算（lm_co_resume 已装，cc 侧无需重装）
 *   mov  rAx, [co + offsetof(reds)]       ; resume 已装 LM_SCHED_REDS
 *   jmp  .loop_body
 *
 * 编译约束：
 *   1. -fno-omit-frame-pointer（GC 保守扫描需栈帧锚定）
 *   2. 让出点前所有活 Value 必须溢出到栈帧（寄存器中的 GC 对象
 *      在 yield 期间可能被回收——保守扫描只扫栈不扫寄存器）
 *   3. reds 扣减频率：循环回边 + 函数调用点（与 VM 通道 B 步对齐）
 *   4. 不在纯算术线性无回边代码插桩（无回边 = 有限执行，不会饿死）
 *
 * 不写实际插桩代码——ir_cgen 重构时落地。
 * ============================================================ */

/* 默认栈大小（128KiB，可配）。深递归 lumin 函数应显式调大。
 * 64KiB 在深嵌套 VM 调用热点（accept loop → createHandler → ctor → spawn，
 * 每层 vm_exec_loop + vm_call_func_value + vm_exec_call_method_dyn + builtin_dispatch
 * 栈帧较大）会被顶满触发 SIGBUS。128KiB 默认值，足够覆盖 4+ 层 VM 嵌套。 */
#define LM_CO_DEFAULT_STACK_SIZE (128 * 1024)

/* Phase 8.6：栈分级常量。
 * LM_STACK_SMALL：IO 连接协程档（16KiB）。mmap 惰性分页使虚拟仅占触页物理。
 *   注：VM 协程本期维持 NORMAL（深嵌套 VM 帧需 128KiB，见上注）；SMALL 降级
 *   待栈深 profiling 验证后另立。此常量供栈池分桶 + 轻量 C 协程显式指定。
 * LM_STACK_NORMAL：VM/计算协程档（128KiB），默认值。 */
#define LM_STACK_SMALL   (16  * 1024)
#define LM_STACK_NORMAL  (128 * 1024)

/* Phase 8.6：栈类标记（stack_class 字段取值，选栈池桶用）。 */
#define LM_STACK_CLASS_SMALL   1
#define LM_STACK_CLASS_NORMAL  2

/* Phase 8.6：idle 换出状态机（swap_state 字段取值，_Atomic CAS 仲裁换出/换入竞态）。
 *   NORMAL       0  协程持栈，未换出
 *   SWAPPING_OUT 1  reaper 正在拷贝栈内容到堆 buffer（唤醒方见此态自旋等 SWAPPED）
 *   SWAPPED      2  协程无栈，内容在 swap_buffer；resume 入口触发换入
 *   SWAPPING_IN  3  唤醒方正在从 buffer 拷回池栈（reaper 见此态跳过） */
#define LM_CO_SWAP_NORMAL        0
#define LM_CO_SWAP_SWAPPING_OUT  1
#define LM_CO_SWAP_SWAPPED       2
#define LM_CO_SWAP_SWAPPING_IN   3

/* Phase 8.13：等待源类别（lm_co_t.wait_kind 字段取值）。
 * 挂起路径在让出前登记、恢复后清除；sysmon 按类别做「源状态 vs 协程状态」
 * 一致性校验（检测丢唤醒）。 */
#define LM_WAIT_NONE     0   /* 未挂起/未登记 */
#define LM_WAIT_FD       1   /* fd 事件等待（co_wait_fd_timeout；waiting_word=conn 方向字） */
#define LM_WAIT_BUTEX    2   /* butex 用户态同步原语等待（cond/channel/join；waiting_word=butex 字） */
#define LM_WAIT_BLOCKING 3   /* blocking 池任务回投等待（lm_co_await_blocking） */

/* Phase 8.6：idle 换出阈值。SUSPENDED 且 now-last_resume_ns 超此值则换出候选。
 * 默认 30s（调研 §8.6）。reaper 每 LM_REAP_SCAN_INTERVAL_NS 扫描一次注册表。 */
#define LM_REAP_IDLE_THRESHOLD_NS  (30ULL * 1000000000ULL)  /* 30s */
#define LM_REAP_SCAN_INTERVAL_NS   (5ULL  * 1000000000ULL)  /* 5s 扫描间隔 */

/* 创建协程：分配 stack（mmap + 末页 guard page），makecontext 设置 entry。
 * 不立即执行，state=READY。返回 NULL 失败。
 * 调用方负责把 co 加入 reactor 就绪队列，reactor 主循环会调 lm_co_resume。
 * stack_size=0 用默认值（NORMAL）。stack_size≤LM_STACK_SMALL 用 SMALL 档。 */
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

/* R3：协程友好休眠 ms 毫秒——挂起当前协程，集中定时器线程到期投递唤醒，
 * 不阻塞 reactor 线程（对比 sleep 的 usleep 阻塞语义）。
 * 返回 0 成功；-1 非协程上下文或无 scheduler；-2 定时器注册失败。
 * 销毁安全：协程挂起中被 lm_co_destroy 强制销毁时自动取消定时器。 */
int lm_co_sleep_ms(long long ms);

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

/* ============================================================
 * Phase 8.6: 栈分级 + idle 换出 API
 *
 * spawn_class：显式指定栈档（LM_STACK_CLASS_SMALL/NORMAL），选栈池桶。
 *   等价 lm_co_spawn(entry,arg,0) 但栈档可控，供轻量 C 协程走 SMALL 档。
 *
 * can_swap hook：VM 层注册，reaper 换出前咨询。返回 1 允许换出，
 *   0 否决（如栈深超阈值、持 cPlist 自引用结构）。未注册时 reaper 不换出
 *   （runtime 不猜 VM 栈内布局，给 VM 层最终裁量权）。
 *
 * lm_co_reap_idle：reaper（sysmon 节流调用）扫描全局协程注册表，
 *   对 SUSPENDED+idle>LM_REAP_IDLE_THRESHOLD_NS+can_swap 通过的协程换出。
 *   外部一般不直接调，由 sysmon 内部触发。
 * ============================================================ */
lm_co_t* lm_co_spawn_class(lm_co_entry_t entry, void* arg, int stack_class);
typedef int (*lm_co_can_swap_fn)(lm_co_t* co);
void lm_co_set_can_swap_hook(lm_co_can_swap_fn fn);
void lm_co_reap_idle(uint64_t now_ns);

/* ============================================================
 * Phase 8.13: 协程注册表遍历 + 等待源登记支持（sysmon stuck 扫描用）
 *
 * lm_co_registry_foreach：持 g_co_reg_lock 遍历全部存活协程并逐个执行
 *   fn(co, ctx)——与 lm_co_reap_idle 同模式：destroy 阻塞于锁，保证回调
 *   期间 co 不被释放（无「快照后遍历」的 UAF 窗口）。
 *   回调约束：短小、禁 malloc/睡眠/长阻塞；允许取 butex 桶锁 / blocking
 *   池锁 / scheduler ready 锁（锁序 registry → 它们，反向路径不存在，
 *   无死锁环）。
 *
 * lm_co_release_waiting_sched：等待退出路径专用——在注册表锁内
 *   exchange waiting_sched=NULL 后 release。sysmon stuck 扫描持同一把锁
 *   遍历并 retain waiting_sched，互斥关闭「扫描方 retain vs 等待方
 *   release」竞态（防 scheduler 被提前释放后 sysmon retain 悬垂指针）。
 * ============================================================ */
typedef void (*lm_co_foreach_fn)(lm_co_t* co, void* ctx);
void lm_co_registry_foreach(lm_co_foreach_fn fn, void* ctx);
void lm_co_release_waiting_sched(lm_co_t* co);

#ifdef __cplusplus
}
#endif

#endif /* LM_CO_H */
