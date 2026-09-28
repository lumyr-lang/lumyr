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
#include "stack_manager.h"  /* g_stack_mgr */
#include "lumyr_value.h"    /* g_err_jmp, val_none, lumyr_make_string */
#include "vm_types.h"       /* VMExecCtx, VMExceptState, vm_except_* */
#include "vm_exec.h"        /* vm_call_func_value */
#include "ast/stackframe.h" /* stackframe_new, stackframe_destroy */
#include "gc_runtime.h"     /* gc_protect_push/pop */

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
    /* 调用方基线（resume 时保存当前 _Thread_local，yield 时恢复）*/
    int            baseline_sp[4];
    jmp_buf*       baseline_err_jmp;
    VMExceptState  baseline_except;
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
    if(!g_stack_mgr) return;  /* 非迁移且本线程无栈池（C 测试场景） */
    /* 保存当前 _Thread_local 到 baseline（调用方基线：reactor 主循环或外层协程）*/
    for(int i = 0; i < 4; i++) st->baseline_sp[i] = g_stack_mgr->sp[i];
    st->baseline_err_jmp = g_err_jmp;
    vm_except_save_state(&st->baseline_except);
    /* 恢复协程 vm_state 到 _Thread_local（首次 resume 时 stack_sp 全 0、except 清零）*/
    for(int i = 0; i < 4; i++) g_stack_mgr->sp[i] = st->stack_sp[i];
    g_err_jmp = st->err_jmp;
    vm_except_restore_state(&st->except_state);
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
    if(co->state != LM_CO_DEAD) {
        for(int i = 0; i < 4; i++) st->stack_sp[i] = g_stack_mgr->sp[i];
        st->err_jmp = g_err_jmp;
        vm_except_save_state(&st->except_state);
        if (co->migrate_sched) {
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
    }
    /* 恢复 baseline（调用方状态）*/
    for(int i = 0; i < 4; i++) g_stack_mgr->sp[i] = st->baseline_sp[i];
    g_err_jmp = st->baseline_err_jmp;
    vm_except_restore_state(&st->baseline_except);
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

void vm_co_hooks_register(void) {
    lm_co_set_vm_hooks(vm_co_resume_hook, vm_co_yield_hook, vm_co_release_hook);
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
    /* lm_co_spawn 用 vm_co_trampoline 作 entry；stack_size=0 用默认 64KiB */
    lm_co_t* co = lm_co_spawn(vm_co_trampoline, sc, 0);
    if(!co) { free(sc); return NULL; }
    sc->co = co;  /* 回填，trampoline 内写 result/error 用 */
    /* 分配 VMCoState（全 0：协程从空状态开始——空 try 栈、thread_root=0、sp=0） */
    VMCoState* st = (VMCoState*)calloc(1, sizeof(VMCoState));
    if(!st) { lm_co_destroy(co); return NULL; }
    co->vm_state = st;
    return co;
}

/* ============================================================
 * timer 回调：reactor addTimer(ms, cb) 触发时调
 * 在 reactor 主循环上下文执行。spawn 协程跑 cb，传 timer_id 作参数。
 * 协程立即 resume；若 cb 不 yield（快速返回），协程 DEAD 后销毁。
 * 若 cb yield，协程挂起——Phase 5 简化：timer cb 不应 yield（无 fd 事件
 * 关联，reactor 不会 resume 它）。完整方案需 reactor 维护就绪协程队列（Phase 6）。
 * ============================================================ */
typedef struct {
    Value func;   /* lm 回调函数（VAL_FUNC） */
} TimerCbCtx;

static void vm_co_timer_cb(int timer_id, void* arg) {
    TimerCbCtx* tc = (TimerCbCtx*)arg;
    if(!tc) return;
    /* spawn 协程跑 cb，传 timer_id 作参数 */
    Value tid = lumyr_make_int64(timer_id);
    lm_co_t* co = vm_co_spawn(tc->func, tid);
    if(co) {
        lm_co_resume(co);  /* 协程跑 cb，可能 yield 切回这里 */
        if(lm_co_is_dead(co)) lm_co_destroy(co);
        /* yield 的协程泄漏（无 reactor 就绪队列管理）——Phase 5 简化，文档说明 */
    }
    free(tc);
}

/* BUILTIN_REACTOR_ADD_TIMER 调用：注册 timer，cb 为 lm 函数。
 * 返回 timer_id（>0），失败 -1。 */
int vm_co_add_timer(lm_reactor_t* r, uint64_t ms, Value cb) {
    TimerCbCtx* tc = (TimerCbCtx*)malloc(sizeof(TimerCbCtx));
    if(!tc) return -1;
    tc->func = cb;
    int id = lm_reactor_add_timer(r, ms, vm_co_timer_cb, tc);
    if(id <= 0) { free(tc); return -1; }
    return id;
}
