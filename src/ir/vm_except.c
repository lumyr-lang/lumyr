/*
 * vm_except.c - VM 异常机制（try/catch/throw）
 * 协作式跨帧展开（不使用 setjmp/longjmp）：
 *   throw 时沿 try 上下文栈定位捕获帧；
 *   若捕获帧不是当前帧，设置 g_unwind，由各层 vm_exec_loop / vm_exec_call
 *   检测后销毁中间帧并向外传播，直到捕获帧清除展开并跳到 catch_pc。
 */
#include "vm_types.h"
#include "stack_manager.h"
#include "ast/stackframe.h"
#include "ir_types.h"
#include "lumyr_value_type.h"
#include "lm_value.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* try 上下文节点（TRY 压入，ENDTRY 弹出） */
typedef struct TryCtxNode {
    StackFrame* frame;      /* try 所在栈帧 */
    int catch_pc;           /* catch 入口 pc（0=无 catch） */
    int fin_pc;             /* finally 入口 pc（0=无 finally，Task 8 后续） */
    struct TryCtxNode* prev;
} TryCtxNode;
static TryCtxNode* g_try_stack = NULL;

/* 协作式展开状态 */
typedef struct {
    int active;
    Value error;
    Value throw_val;     /* 原始 throw 值（跨帧展开时同步给 GET_ERR） */
    StackFrame* target_frame;
    int catch_pc;
} UnwindState;
static UnwindState g_unwind;

/* 当前已捕获错误（GET_ERR 读取） */
static Value g_current_error;
/* 原始 throw 值（catch 变量绑定它，而非包装后的 ValueError） */
static Value g_current_throw_val;

/* 把任意抛出值规范化为 VAL_ERROR */
static Value ensure_error(Value v) {
    if (v.type == VAL_ERROR) return v;
    Value e;
    memset(&e, 0, sizeof(e));
    e.type = VAL_ERROR;
    e.v.err.type = strdup("RuntimeError");
    if (v.type == VAL_STRING) {
        const char* s = lumyr_str_cstr(&v);
        e.v.err.message = strdup(s ? s : "");
    } else {
        e.v.err.message = strdup("exception");
    }
    e.v.err.stack = NULL;
    return e;
}

/* ========== TRY：注册异常处理器 ========== */
int vm_exec_try(VMExecCtx* ctx, Instruction* in) {
    TryCtxNode* n = (TryCtxNode*)malloc(sizeof(TryCtxNode));
    if (!n) { perror("vm_exec_try"); exit(EXIT_FAILURE); }
    n->frame    = ctx->frame;
    n->catch_pc = in->a;
    n->fin_pc   = in->b;
    n->prev     = g_try_stack;
    g_try_stack = n;
    return 1;
}

/* ========== ENDTRY：正常路径退出，弹出处理器 ========== */
int vm_exec_endtry(VMExecCtx* ctx, Instruction* in) {
    (void)ctx;
    if (g_try_stack) {
        TryCtxNode* n = g_try_stack;
        g_try_stack = n->prev;
        free(n);
    }
    (void)in;
    return 1;
}

/* ========== THROW：弹值，定位处理器，同层跳转或启动跨帧展开 ========== */
int vm_exec_throw(VMExecCtx* ctx, Instruction* in) {
    (void)in;
    Value v;
    stack_vm_pop(g_stack_mgr, STACK_VALUE, &v);
    Value err = ensure_error(v);

    /* 沿 try 栈找第一个有 catch 的处理器 */
    TryCtxNode* t = g_try_stack;
    while (t && t->catch_pc == 0) t = t->prev;

    if (!t) {
        /* 未捕获：打印错误信息并终止 */
        fprintf(stderr, "未捕获错误 [%s]: %s\n",
                err.v.err.type ? err.v.err.type : "Error",
                err.v.err.message ? err.v.err.message : "");
        exit(1);
    }

    /* 弹出 target 之上被异常穿过的 try 节点（仅 finally 的节点后续任务处理） */
    while (g_try_stack && g_try_stack != t) {
        TryCtxNode* d = g_try_stack;
        g_try_stack = d->prev;
        free(d);
    }

    if (t->frame == ctx->frame) {
        /* 同层捕获：先弹出当前 try 处理器，避免 catch 块内 throw 被同一 catch 重复捕获 */
        int cpc = t->catch_pc;
        if (g_try_stack == t) {
            g_try_stack = t->prev;
            free(t);
        }
        g_current_error = err;
        g_current_throw_val = v;
        ctx->pc = cpc;
    } else {
        /* 跨帧：启动协作式展开，由 vm_except_check_unwind 驱动 */
        g_unwind.active = 1;
        g_unwind.error = err;
        g_unwind.throw_val = v;
        g_unwind.target_frame = t->frame;
        g_unwind.catch_pc = t->catch_pc;
    }
    return 1;
}

/* ========== GET_ERR：把原始 throw 值压 VALUE 栈（catch 变量绑定它） ========== */
int vm_exec_get_err(VMExecCtx* ctx, Instruction* in) {
    (void)ctx;
    (void)in;
    Value e = g_current_throw_val;
    stack_vm_push(g_stack_mgr, STACK_VALUE, &e);
    return 1;
}

/* ========== 主循环底部调用：驱动协作式展开 ========== */
int vm_except_check_unwind(VMExecCtx* ctx) {
    if (!g_unwind.active) return 0;
    if (g_unwind.target_frame == ctx->frame) {
        /* 到达捕获帧：弹出当前 try 处理器，避免 catch 块内 throw 被同一 catch 重复捕获 */
        if (g_try_stack) {
            TryCtxNode* n = g_try_stack;
            g_try_stack = n->prev;
            free(n);
        }
        g_current_error = g_unwind.error;
        g_current_throw_val = g_unwind.throw_val;
        ctx->pc = g_unwind.catch_pc;
        g_unwind.active = 0;
        return 1;
    }
    return -1;
}

/* ========== finally 完成动作（FIN_PUSH 压入，FINISH 消费） ========== */

typedef struct FinNode {
    int action;                 /* 1=JMP 2=RETHROW 3=BREAK 4=CONT */
    int target;
    struct FinNode* next;
} FinNode;
static FinNode* g_fin_stack = NULL;

/* FIN_PUSH：压入 finally 完成动作（a=action，b=目标pc） */
int vm_exec_fin_push(VMExecCtx* ctx, Instruction* in) {
    (void)ctx;
    FinNode* n = (FinNode*)malloc(sizeof(FinNode));
    if(!n) { perror("vm_exec_fin_push"); exit(EXIT_FAILURE); }
    n->action = in->a;
    n->target = in->b;
    n->next   = g_fin_stack;
    g_fin_stack = n;
    return 1;
}

/* ========== try 内 return：挂起返回，执行完所有 finally 后才真正返回 ========== */

static RetSlot g_pending_ret;
static int g_has_pending_ret = 0;   /* PEND_RETURN 设置：有值待返回 */
static int g_wants_return = 0;      /* FINISH 设置：finally 已走完，请求结束当前帧 */

/* PEND_RETURN：弹 VALUE 返回值挂起，跳 b=finally 入口；a=1 表示无值返回（不弹栈） */
int vm_exec_pend_return(VMExecCtx* ctx, Instruction* in) {
    Value v;
    if(in->a == 1) {
        v = val_none();
    } else {
        stack_vm_pop(g_stack_mgr, STACK_VALUE, &v);
    }
    g_pending_ret.et = EXPR_TYPE_NONE;
    g_pending_ret.v  = ret_value_detach(v);
    g_pending_ret.i  = 0;
    g_pending_ret.d  = 0.0;
    g_pending_ret.p  = NULL;
    g_has_pending_ret = 1;
    ctx->pc = in->b;
    return 1;
}

/* 弹出当前 try 节点（break/cont/rethrow/return 路径不经过 ENDTRY） */
static void pop_current_try(void) {
    if(g_try_stack) {
        TryCtxNode* t = g_try_stack;
        g_try_stack = t->prev;
        free(t);
    }
}

/* 同帧外层是否还有待执行 finally */
static TryCtxNode* same_frame_outer_fin(VMExecCtx* ctx) {
    TryCtxNode* o = g_try_stack;
    if(o && o->frame == ctx->frame && o->fin_pc != 0) return o;
    return NULL;
}

/* FINISH：finally 块末尾。三种来源——
   1) 挂起返回（PEND_RETURN）：弹try，外层fin继续 或 请求真正返回；
   2) FIN_PUSH 完成动作（JMP/BREAK/CONT/RETHROW）：弹try，外层fin继续 或 执行动作；
   3) 正常路径：no-op，pc 自然落到 after（ENDTRY）。 */
int vm_exec_finish(VMExecCtx* ctx, Instruction* in) {
    (void)in;

    /* 来源1：挂起返回 */
    if(g_has_pending_ret) {
        pop_current_try();
        TryCtxNode* o = same_frame_outer_fin(ctx);
        if(o) ctx->pc = o->fin_pc;
        else  g_wants_return = 1;
        return 1;
    }

    /* 来源2：FIN_PUSH 完成动作 */
    if(g_fin_stack) {
        FinNode* n = g_fin_stack; g_fin_stack = n->next;
        int action = n->action, target = n->target;
        free(n);
        pop_current_try();
        TryCtxNode* o = same_frame_outer_fin(ctx);
        if(o) {
            /* 还有外层 finally：重新压入同一动作，链式继续 */
            FinNode* m = (FinNode*)malloc(sizeof(FinNode));
            if(!m){ perror("fin chain"); exit(EXIT_FAILURE); }
            m->action = action; m->target = target;
            m->next = g_fin_stack; g_fin_stack = m;
            ctx->pc = o->fin_pc;
            return 1;
        }
        switch(action) {
        case 1: case 3: case 4:           /* JMP / BREAK / CONT：跳到目标 */
            ctx->pc = target;
            break;
        case 2: {                          /* RETHROW：压回错误，复用 THROW 搜外层 */
            stack_vm_push(g_stack_mgr, STACK_VALUE, &g_current_error);
            vm_exec_throw(ctx, in);
            break;
        }}
        return 1;
    }

    /* 来源3：正常路径 no-op */
    return 1;
}

/* vm_exec_loop 底部调用：取出已走完 finally 的挂起返回值 */
int vm_except_take_pending_return(RetSlot* out) {
    if (!g_wants_return) return 0;
    g_wants_return = 0;
    g_has_pending_ret = 0;
    *out = g_pending_ret;
    return 1;
}

/* CATCH_MATCH：多 catch 按类型匹配。
   a=异常类型字符串常量下标(-1=捕获全部)；命中则 fall through，不命中跳 b。 */
int vm_exec_catch_match(VMExecCtx* ctx, Instruction* in) {
    if(in->a < 0) return 1;   /* catch-all 子句 */
    const char* want = ctx->const_pool[in->a].s;
    const char* have = g_current_error.v.err.type;
    if(have && want && strcmp(have, want) == 0) return 1;
    ctx->pc = in->b;
    return 1;
}
