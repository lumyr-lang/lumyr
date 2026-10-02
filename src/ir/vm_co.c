// vm_co.c —— 协程 VM 状态管理（Phase 5）
// 协程在同线程复用 _Thread_local 的 g_stack_mgr + 异常栈（g_try_stack/g_unwind/
// g_fin_stack/g_err_jmp/g_thread_root 等），yield/resume 必须保存/恢复这组状态，
// 否则 reactor resume 不同协程会互相污染（try 链错乱、sp 混乱、错误着陆垫错位）。
//
// 设计：
//   - VMCoState 挂在 lm_co_t.vm_state（不透明指针，kit/runtime 不直接访问）
//   - g_co_baseline（TLS）：reactor.run() 进入时保存的 _Thread_local 快照，
//     协程 yield 时恢复到基线，切回 reactor 主循环
//   - resume_hook：swapcontext 切到协程前，恢复协程 vm_state 到 _Thread_local
//   - yield_hook：swapcontext 切回 reactor 前，保存协程 vm_state，恢复基线
//   - release_hook：lm_co_destroy 前释放 vm_state 内存
//   - trampoline：协程 entry，含 vm_except_enter_thread_root + setjmp 根垫，
//     未捕获错误 longjmp 到根垫而非 exit(1)，记录到 co->vm_state->error
#include "vm_co.h"
#include "lm_co.h"          /* lm_co_t, lm_co_spawn, lm_co_set_vm_hooks */
#include "lm_reactor.h"     /* lm_reactor_t, lm_reactor_add_timer */
#include "lm_scheduler.h"   /* Phase 8.6 修复：timer 回调投递回 reactor（get_current/wakeup） */
#include "stack_manager.h"  /* g_stack_mgr */
#include "lumyr_value.h"    /* g_err_jmp, val_none, lumyr_make_string */
#include "vm_types.h"       /* VMExecCtx, VMExceptState, vm_except_* */
#include "vm_exec.h"        /* vm_call_func_value */
#include "ast/stackframe.h" /* stackframe_new, stackframe_destroy */
#include "gc_runtime.h"     /* gc_protect_push/pop */
#include "lm_type.h"        /* lumyr_get/set_current_class：访问控制上下文随协程迁移 */

#include <stdlib.h>
#include <string.h>
#include <setjmp.h>

/* ============================================================
 * VMCoState：协程专属 VM 状态快照
 * 协程自带 baseline（resume 时保存调用方状态，yield 时恢复），
 * 不依赖 reactor.run() 设置全局基线——无 reactor 时协程也能用。
 * ============================================================ */
typedef struct {
    /* 协程执行状态（yield 时保存，resume 时恢复到 _Thread_local）*/
    int            stack_sp[4];   /* g_stack_mgr->sp 快照（4 核心栈） */
    jmp_buf*       err_jmp;       /* g_err_jmp 快照（runtime_error 着陆垫） */
    VMExceptState  except_state;  /* 异常状态快照（g_try_stack/g_unwind/g_fin_stack/g_thread_root 等） */
    const char*    current_class; /* g_current_class 快照（当前方法属主类，访问控制上下文） */
    /* 调用方基线（resume 时保存当前 _Thread_local，yield 时恢复）*/
    int            baseline_sp[4];
    jmp_buf*       baseline_err_jmp;
    VMExceptState  baseline_except;
    const char*    baseline_class;
    /* 本轮 resume 恢复后的 4 栈 sp（= 协程入口/上次让出点的平衡栈深）。
     * vm_co_can_preempt 据此判断时间片抢占点是否栈平衡：非平衡派发点
     * 禁止让出（per-thread 共享栈数据区的活值会被同线程他协程覆盖）。
     * Phase 8.5 J：内置检查点复用此规则——内置 C 代码不触碰操作数栈，
     * 故内置期间 sp 恒等于进入时深度。语句级调用时该深度 == entry_sp，
     * checkpoint 让出安全；嵌套表达式调用（如 `7 + json()`）深度 >
     * entry_sp，checkpoint 拒让（防共享栈上活值被覆写），与 TR-4.3 一致。
     * 故无需独立的 builtin_entry_sp 机制。 */
    int            entry_sp[4];
    /* Phase 7.4：跨线程迁移搬栈。栈池 per-thread（g_stack_mgr 是 TLS 指针，
     * 栈数据物理绑定创建线程），迁移 yield（migrate_sched 非空）时把活数据
     * （stacks[i][0..sp[i])）拷入 mig_copy，跨线程 resume 时写回目标线程栈池。
     * 迁移后置 migrated=0（写回一次即失效，避免同线程 resume 重复写回）。 */
    void*          mig_copy[4];   /* 各栈活数据拷贝（迁移期间持有） */
    size_t         mig_size[4];   /* 各栈拷贝字节数（0=该栈无活数据） */
    int            migrated;      /* 上次 yield 是否为迁移 yield */
    /* 结果/错误（trampoline 写入）*/
    Value          result;        /* 协程正常返回值 */
    Value          error;         /* 协程未捕获错误（VAL_ERROR 或错误 Value） */
    int            has_result;     /* 1=正常结束有 result */
    int            has_error;      /* 1=有未捕获 error */
} VMCoState;

/* ============================================================
 * hook 实现：resume / yield / release
 * ============================================================ */

/* resume 前恢复协程 vm_state 到 _Thread_local。
 * 先保存当前 _Thread_local 到 baseline（调用方状态），再恢复协程状态。
 * Phase 7.4：迁移 resume（migrated=1）时先确保本线程栈池就绪（worker 首次
 * 时 g_stack_mgr 为 NULL）并把搬运的活数据写回，再走统一恢复路径。
 * co==NULL：no-op（保留兼容，正常不传 NULL）。 */
static void vm_co_resume_hook(lm_co_t* co) {
    if(!co) return;
    VMCoState* st = (VMCoState*)co->vm_state;
    if(!st) return;  /* 未注册 spawn（C 测试场景），no-op */
    if (st->migrated) {
        if (!g_stack_mgr) stack_global_init(0);   /* worker 首次：建本线程栈池 */
        for (int i = 0; i < STACK_TYPE_COUNT; i++) {
            if (st->mig_size[i] > 0) {
                memcpy(g_stack_mgr->stacks[i], st->mig_copy[i], st->mig_size[i]);
            }
            g_stack_mgr->sp[i] = st->stack_sp[i];
            free(st->mig_copy[i]);
            st->mig_copy[i] = NULL;
            st->mig_size[i] = 0;
        }
        st->migrated = 0;
    }
    if(!g_stack_mgr) stack_global_init(0);  /* 本线程首次 resume VM 协程：建本线程栈池。
     * 能走到这里的协程在本线程必无栈数据：fresh 协程（sp 全 0，spawn 即 stealable=1，
     * 可被 WSQ 窃取到任意线程）或 migrated（上方分支已建池并恢复 mig_copy）。
     * sp>0 的未迁移协程在首个非迁移 yield 后 stealable=0，不会被跨线程投递，
     * 故此处建池后恢复 sp 不会读到空池垃圾。 */
    /* 保存当前 _Thread_local 到 baseline（调用方基线：reactor 主循环或外层协程）*/
    for(int i = 0; i < 4; i++) st->baseline_sp[i] = g_stack_mgr->sp[i];
    st->baseline_err_jmp = g_err_jmp;
    vm_except_save_state(&st->baseline_except);
    st->baseline_class = lumyr_get_current_class();
    /* 恢复协程 vm_state 到 _Thread_local（首次 resume 时 stack_sp 全 0、except 清零）*/
    for(int i = 0; i < 4; i++) g_stack_mgr->sp[i] = st->stack_sp[i];
    g_err_jmp = st->err_jmp;
    vm_except_restore_state(&st->except_state);
    /* 恢复协程自己的方法属主类：同线程交错的两个协程若执行不同类的方法，
     * 不恢复会让 private/protected 访问检查误用他协程的类上下文。 */
    lumyr_set_current_class(st->current_class);
    /* 记录本协程本轮运行的入口栈深（= 上次让出点的平衡栈深），
     * vm_co_can_preempt 据此禁止在操作数栈非平衡点抢占。 */
    for(int i = 0; i < 4; i++) st->entry_sp[i] = g_stack_mgr->sp[i];
}

/* yield 前保存协程 vm_state + 恢复 baseline。
 * DEAD 时（trampoline 结束后 lm_co_resume 检测 DEAD 调本 hook）只恢复 baseline，
 * 不保存协程状态（协程要销毁，保存无意义）。
 * Phase 7.4：迁移 yield（migrate_sched 非空）时额外把活数据搬入 mig_copy
 * （栈数据物理绑定当前线程，目标线程栈池里没有；顺序：先搬协程数据，再恢复
 * baseline——baseline 恢复会覆盖 sp，搬家必须在它之前）。 */
static void vm_co_yield_hook(lm_co_t* co) {
    if(!g_stack_mgr || !co) return;
    VMCoState* st = (VMCoState*)co->vm_state;
    if(!st) return;
    /* 保存协程执行状态（DEAD 时跳过——协程要销毁）*/
    if(atomic_load_explicit(&co->state, memory_order_acquire) != LM_CO_DEAD) {
        for(int i = 0; i < 4; i++) st->stack_sp[i] = g_stack_mgr->sp[i];
        st->err_jmp = g_err_jmp;
        vm_except_save_state(&st->except_state);
        st->current_class = lumyr_get_current_class();
        if (atomic_load_explicit(&co->migrate_sched, memory_order_acquire)) {
            st->migrated = 1;
            for (int i = 0; i < STACK_TYPE_COUNT; i++) {
                int sp = g_stack_mgr->sp[i];
                if (sp > 0) {
                    size_t bytes = (size_t)sp * stack_get_elem_size((StackType)i);
                    void* buf = realloc(st->mig_copy[i], bytes);
                    /* 搬家失败 = 栈数据丢失（静默继续会数据损坏），直接终止 */
                    if (!buf) abort();
                    memcpy(buf, g_stack_mgr->stacks[i], bytes);
                    st->mig_copy[i] = buf;
                    st->mig_size[i] = bytes;
                } else {
                    free(st->mig_copy[i]);
                    st->mig_copy[i] = NULL;
                    st->mig_size[i] = 0;
                }
            }
        }
        /* Phase 8.2：维护 stealable（机制级迁移安全标志，scheduler 窃取分流依据）。
         * 本次 yield 做了 mig_copy（compute 迁移路径）→ 栈数据已在堆上，
         * 任何线程 resume 都可经 mig_copy 恢复 → 可窃取；
         * 否则栈数据物理留在本线程栈池 → 只能本线程 resume → 不可窃取。
         * resume hook 消费 mig_copy 后协程重新运行，栈数据回到新线程池，
         * 下次 yield 时本标志按同一规则重判。 */
        co->stealable = (st->migrated) ? 1 : 0;   /* _Atomic 字段：裸写即 seq_cst 原子存 */
    }
    /* 恢复 baseline（调用方状态）*/
    for(int i = 0; i < 4; i++) g_stack_mgr->sp[i] = st->baseline_sp[i];
    g_err_jmp = st->baseline_err_jmp;
    vm_except_restore_state(&st->baseline_except);
    lumyr_set_current_class(st->baseline_class);
}

/* destroy 前释放 vm_state 内存。
 * 不 free try_stack/fin_stack 链表节点：节点属于各自协程的 stackframe，
 * 协程正常结束 try 栈已空；异常未捕获时 trampoline 根垫已处理。
 * result/error Value 由 GC 管理对象，不显式释放。 */
static void vm_co_release_hook(lm_co_t* co) {
    if(!co || !co->vm_state) return;
    VMCoState* st = (VMCoState*)co->vm_state;
    /* Phase 7.4：迁移中途销毁（computeBegin 后未 computeEnd 即 destroy）时
     * 释放搬家缓冲，防泄漏。 */
    for (int i = 0; i < STACK_TYPE_COUNT; i++) {
        free(st->mig_copy[i]);
    }
    free(st);
    co->vm_state = NULL;
}

/* Phase 8.6 F：VM 层换出裁量权（can_swap hook）。
 * DONTNEED 版换出基址不变（mmap 映射保留），故无栈内自引用/指针重定位问题——
 * can_swap 仅做策略门限：迁移中途不换、栈深超阈值不换（限 buffer 拷贝成本）。
 * 未注册时 reaper 不换出（runtime 不猜），故本函数是换出的必要条件。 */
#define LM_VM_SWAP_MAX_DEPTH (16 * 1024)   /* 栈深上限 16KiB，超此不换（限拷贝成本） */

static int vm_co_can_swap(lm_co_t* co) {
    if (!co) return 0;
    if (!co->vm_state) return 1;   /* 纯 C 协程无 VM 状态，DONTNEED 换出安全 */
    VMCoState* st = (VMCoState*)co->vm_state;
    /* 迁移中途（mig_copy 非空 = 活数据已搬出，协程处于跨线程迁移态）不换：
     * 迁移是瞬态，协程即将在目标线程 resume，换出徒增开销。 */
    for (int i = 0; i < 4; i++) {
        if (st->mig_copy[i]) return 0;
    }
#ifdef LM_CTX_FCONTEXT
    /* fcontext：ctx.sp 是冻结 sp，栈深 = stack_base - ctx.sp。超阈值不换。 */
    void* sp = co->ctx.sp;
    void* base = co->stack_base;
    if (sp && base && (char*)base > (char*)sp) {
        size_t used = (size_t)((char*)base - (char*)sp);
        if (used > LM_VM_SWAP_MAX_DEPTH) return 0;
    }
    return 1;
#else
    /* ucontext 后端：swap_out 本就禁用（lm_co.c co_swap_out 跳过），否决。 */
    return 0;
#endif
}

/* 抢占安全门（根因修复）：仅当 4 个操作数栈当前 sp 都等于本协程本轮 resume
 * 的入口基线（= 上次让出点栈深）时才允许时间片抢占。不相等说明实参（含
 * receiver）或表达式中间值正压在栈上——此刻让出，这些活值会留在 per-thread
 * 共享栈数据区，被同线程后运行的协程压值覆盖，resume 后弹出串改的对象
 * （file.recv 类型串改根因）。无栈池/无 vm_state（纯 C 场景）时放行。 */
static int vm_co_can_preempt(lm_co_t* co) {
    if (!co || !co->vm_state || !g_stack_mgr) return 1;
    VMCoState* st = (VMCoState*)co->vm_state;
    for (int i = 0; i < STACK_TYPE_COUNT; i++) {
        if (g_stack_mgr->sp[i] != st->entry_sp[i]) {
            return 0;
        }
    }
    return 1;
}

void vm_co_hooks_register(void) {
    lm_co_set_vm_hooks(vm_co_resume_hook, vm_co_yield_hook, vm_co_release_hook);
    lm_co_set_can_swap_hook(vm_co_can_swap);   /* Phase 8.6 F：注册换出裁量 */
    lm_co_set_can_preempt_hook(vm_co_can_preempt); /* 非平衡栈禁止 slice 抢占 */
}

/* ============================================================
 * reactor 基线管理：BUILTIN_REACTOR_RUN 调用
 * 协程自带 baseline 后，reactor 基线主要用于"reactor 主循环恢复到 run 进入前状态"。
 * enter 保存 run 进入前的 _Thread_local，leave 恢复（防 run 内 vm_state 被改）。
 * ============================================================ */
static _Thread_local int g_co_baseline_sp[4] = {0,0,0,0};
static _Thread_local jmp_buf* g_co_baseline_err_jmp = NULL;
static _Thread_local VMExceptState g_co_baseline_except;
static _Thread_local int g_co_baseline_set = 0;

void vm_co_enter_reactor_baseline(void) {
    if(g_co_baseline_set) return;  /* 已有基线（嵌套 reactor.run，不支持）*/
    if(g_stack_mgr) {
        for(int i = 0; i < 4; i++) g_co_baseline_sp[i] = g_stack_mgr->sp[i];
    }
    g_co_baseline_err_jmp = g_err_jmp;
    vm_except_save_state(&g_co_baseline_except);
    g_co_baseline_set = 1;
}

void vm_co_leave_reactor_baseline(void) {
    if(!g_co_baseline_set) return;
    if(g_stack_mgr) {
        for(int i = 0; i < 4; i++) g_stack_mgr->sp[i] = g_co_baseline_sp[i];
    }
    g_err_jmp = g_co_baseline_err_jmp;
    vm_except_restore_state(&g_co_baseline_except);
    g_co_baseline_set = 0;
}

/* ============================================================
 * 协程 result/error 设置（trampoline 写入）
 * ============================================================ */
void vm_co_set_result(lm_co_t* co, Value result) {
    if(!co || !co->vm_state) return;
    VMCoState* st = (VMCoState*)co->vm_state;
    st->result = result;
    st->has_result = 1;
}

void vm_co_set_error(lm_co_t* co, Value error) {
    if(!co || !co->vm_state) return;
    VMCoState* st = (VMCoState*)co->vm_state;
    st->error = error;
    st->has_error = 1;
}

/* ============================================================
 * 协程 trampoline：BUILTIN_CO_SPAWN 的协程 entry
 * 参考 vm_thread_body (vm.c:76) 的根帧兜底机制，但复用同线程 g_stack_mgr
 * （由 resume/yield hook 保存/恢复）。
 * ============================================================ */
typedef struct {
    Value    func;   /* lm 函数值（VAL_FUNC） */
    Value    arg;     /* 单参数（多参用数组包装） */
    lm_co_t* co;      /* 所属协程（写 result/error 用） */
} CoSpawnCtx;

static void vm_co_trampoline(void* arg) {
    CoSpawnCtx* sc = (CoSpawnCtx*)arg;
    VMExecCtx co_ctx;
    memset(&co_ctx, 0, sizeof(co_ctx));
    co_ctx.frame = stackframe_new(NULL);  /* 独立根帧，parent=NULL，避免 frame 链污染 */

    /* 协程根兜底：未捕获错误 longjmp 到这里，只终止本协程不 exit 进程。
     * vm_except_enter_thread_root 设 g_thread_root=1，使 vm_except_throw_value
     * 未命中 catch 时 longjmp 到本根垫而非 exit(1)。
     * g_thread_root/g_err_jmp 作为 _Thread_local 由 yield hook 保存到 co->vm_state，
     * resume 时恢复，保证协程内 throw 走本根垫。 */
    jmp_buf rootpad;
    jmp_buf* prevJmp = g_err_jmp;
    g_err_jmp = &rootpad;
    vm_except_enter_thread_root();

    Value result = val_none();
    int jumped = 0;
    if(setjmp(rootpad) == 0) {
        int rc = vm_call_func_value(&co_ctx, sc->func, 1, &sc->arg, &result);
        g_err_jmp = prevJmp;
        vm_except_leave_thread_root();
        if(rc != 1) {
            /* 硬错误（指令 not handled 等）：记录为协程错误 */
            vm_co_set_error(sc->co,
                lumyr_make_string("coroutine hard error / 协程硬错误"));
        } else {
            vm_co_set_result(sc->co, result);
        }
    } else {
        /* 未捕获错误着陆：g_thread_root_err 已由异常机制设置 */
        jumped = 1;
        g_err_jmp = prevJmp;
        vm_except_leave_thread_root();
        Value err = vm_except_take_thread_root_error();
        gc_protect_push(err);
        vm_co_set_error(sc->co, err);
        gc_protect_pop();
    }
    stackframe_destroy(co_ctx.frame);
    free(sc);
    (void)jumped;
    /* entry 返回 → co_trampoline 设 DEAD → 显式 lm_ctx_jump 切回 resume_ctx（Phase 8.1） */
}

/* ============================================================
 * vm_co_spawn：创建协程并跑 lm 函数（BUILTIN_CO_SPAWN 调用）
 * ============================================================ */
lm_co_t* vm_co_spawn(Value func, Value arg) {
    CoSpawnCtx* sc = (CoSpawnCtx*)malloc(sizeof(CoSpawnCtx));
    if(!sc) return NULL;
    sc->func = func;
    sc->arg = arg;
    sc->co = NULL;
    /* lm_co_spawn 用 vm_co_trampoline 作 entry；stack_size=0 用默认 128KiB */
    lm_co_t* co = lm_co_spawn(vm_co_trampoline, sc, 0);
    if(!co) { free(sc); return NULL; }
    sc->co = co;  /* 回填，trampoline 内写 result/error 用 */
    /* 分配 VMCoState（全 0：协程从空状态开始——空 try 栈、thread_root=0、sp=0） */
    VMCoState* st = (VMCoState*)calloc(1, sizeof(VMCoState));
    if(!st) { lm_co_destroy(co); return NULL; }
    co->vm_state = st;
    return co;
}

/* Task 6：显式指定栈档的 spawn（LM_STACK_CLASS_SMALL/NORMAL）。 */
lm_co_t* vm_co_spawn_class(Value func, Value arg, int stack_class) {
    CoSpawnCtx* sc = (CoSpawnCtx*)malloc(sizeof(CoSpawnCtx));
    if(!sc) return NULL;
    sc->func = func;
    sc->arg = arg;
    sc->co = NULL;
    lm_co_t* co = lm_co_spawn_class(vm_co_trampoline, sc, stack_class);
    if(!co) { free(sc); return NULL; }
    sc->co = co;
    VMCoState* st = (VMCoState*)calloc(1, sizeof(VMCoState));
    if(!st) { lm_co_destroy(co); return NULL; }
    co->vm_state = st;
    return co;
}

/* ============================================================
 * timer 回调：reactor addTimer(ms, cb) 触发时调
 * Phase 8.4 设计（lm_reactor.c 主循环注释）："到期回调由 timer 线程直接执行
 * 并 wakeup 投递协程回本 reactor"。本函数在 timer 线程执行，职责仅是
 * spawn 协程 + 投递回 add 时的 scheduler（mutex 定向队列 + wakeup reactor），
 * 由 reactor 线程 drain_ready resume——绝不在 timer 线程直接 resume：
 * timer 线程无 VM TLS（g_stack_mgr/globals 根帧上下文），直接 resume 会以
 * RuntimeError 死亡且错误被吞（compute_test/timer_probe 曾因此挂死：
 * 回调抛"加法要求数值或字符串操作数"→ r.stop() 未执行）。
 * yield 的协程由 scheduler 就绪队列管理（修复旧"yield 泄漏"限制）；
 * DEAD 协程由 GC release hook 回收（与 Coroutine() builtin 同路径）。
 * ============================================================ */
typedef struct {
    Value func;   /* lm 回调函数（VAL_FUNC） */
    lm_scheduler_t* sched;   /* add 时的 scheduler（reactor 线程捕获），NULL=无 scheduler 走旧同步路径 */
} TimerCbCtx;

static void vm_co_timer_cb(lm_timer_id_t timer_id, void* arg) {
    TimerCbCtx* tc = (TimerCbCtx*)arg;
    if(!tc) return;
    /* spawn 协程跑 cb，传 timer_id 作参数。spawn 不依赖本线程 VM TLS
     * （CoSpawnCtx/VMCoState 堆分配 + 协程栈 mmap），跨线程安全。 */
    Value tid = lumyr_make_int64((int64_t)timer_id);
    lm_co_t* co = vm_co_spawn(tc->func, tid);
    if(co) {
        if (tc->sched) {
            /* 投递回 reactor：mutex 定向队列 + wakeup，drain_ready 在
             * reactor 线程 resume（协程从未运行、无栈数据，跨线程投递安全） */
            lm_scheduler_wakeup(tc->sched, co);
        } else {
            /* 无 scheduler（C 测试/极早期路径）：保持旧同步语义 */
            lm_co_resume(co);
            if(lm_co_is_dead(co)) lm_co_destroy(co);
            /* yield 的协程泄漏（无 reactor 就绪队列管理）——Phase 5 简化 */
        }
    }
    /* 释放 vm_co_add_timer 捕获时的引用（post 全部完成后才减，
     * 保证回调期间 scheduler 不会被 destroyScheduler 提前 free）。 */
    lm_scheduler_release(tc->sched);
    free(tc);
}

/* BUILTIN_REACTOR_ADD_TIMER 调用：注册 timer，cb 为 lm 函数。
 * Phase 8.4：转发到全局 TimerThread。返回 timer_id（>0），
 * 失败 LM_TIMER_INVALID_ID（0）。 */
lm_timer_id_t vm_co_add_timer(lm_reactor_t* r, uint64_t ms, Value cb) {
    TimerCbCtx* tc = (TimerCbCtx*)malloc(sizeof(TimerCbCtx));
    if(!tc) return LM_TIMER_INVALID_ID;
    tc->func = cb;
    /* add 调用发生在 reactor 线程（addTimer builtin），此刻 scheduler TLS
     * 已由 setScheduler 设定——捕获为回调协程的目标 scheduler。
     * 捕获即 retain（必须在挂入 timer 系统前持引用，否则 destroyScheduler
     * 可与回调并发 free 结构 → lm_scheduler_post UAF / 唤醒丢失挂死），
     * 回调结束 release；add 失败路径同样 release 防泄漏。 */
    tc->sched = lm_scheduler_get_current();
    lm_scheduler_retain(tc->sched);
    lm_timer_id_t id = lm_reactor_add_timer(r, ms, vm_co_timer_cb, tc);
    if(id == LM_TIMER_INVALID_ID) {
        lm_scheduler_release(tc->sched);
        free(tc); return LM_TIMER_INVALID_ID;
    }
    return id;
}
