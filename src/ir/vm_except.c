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
#include "gc_runtime.h"
#include "lm_type.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* try 上下文节点（TRY 压入，ENDTRY 弹出）
 * 全部状态 _Thread_local：每个工作线程独立的 try 栈/展开状态/挂起返回，
 * 线程内未捕获错误在 vm_except_throw_value 中按本线程 try 栈判定（空则 exit），
 * 不会跨线程命中主线程的 catch 帧。 */
typedef struct TryCtxNode {
    StackFrame* frame;      /* try 所在栈帧 */
    int catch_pc;           /* catch 入口 pc（0=无 catch） */
    int fin_pc;             /* finally 入口 pc（0=无 finally，Task 8 后续） */
    int caught;             /* 已进入 catch 块（节点保留至 ENDTRY）：
                             * catch 内再 throw 时本层 finally 仍须执行，
                             * 且同一 catch 不得重复捕获 */
    struct TryCtxNode* prev;
} TryCtxNode;
static _Thread_local TryCtxNode* g_try_stack = NULL;

/* 协作式展开状态 */
typedef struct {
    int active;
    Value error;
    Value throw_val;     /* 原始 throw 值（跨帧展开时同步给 GET_ERR） */
    StackFrame* target_frame;
    int catch_pc;
} UnwindState;
static _Thread_local UnwindState g_unwind;

/* 当前已捕获错误（GET_ERR 读取） */
static _Thread_local Value g_current_error;
/* 原始 throw 值（catch 变量绑定它，而非包装后的 ValueError） */
static _Thread_local Value g_current_throw_val;

/* 工作线程根帧模式：1=当前执行体是工作线程根帧，未捕获错误不再 exit
 * 进程，记录错误后 longjmp 到 vm_thread_body 的根兜底。 */
static _Thread_local int g_thread_root = 0;
static _Thread_local Value g_thread_root_err;
/* 错误已压入 protect 栈（防跨线程并发 GC 回收）；重入不重复压 */
static _Thread_local int g_thread_root_pushed = 0;

/* finally 完成动作节点（FIN_PUSH 压入，FINISH 消费）。
 * 前置于 throw_value：异常穿过仅 finally 的 try 时需压入 RETHROW 动作。 */
typedef struct FinNode {
    int action;                 /* 1=JMP 2=RETHROW 3=BREAK 4=CONT */
    int target;
    struct FinNode* next;
} FinNode;
/* 必须 _Thread_local：finally 完成动作栈每线程独立。旧声明为普通全局，
 * 多线程并发时一个线程的 FINISH 会消费并 free 另一线程压入的节点
 * → double-free / 动作错乱。 */
static _Thread_local FinNode* g_fin_stack = NULL;

/* 把任意抛出值规范化为 VAL_ERROR */
static Value ensure_error(Value v) {
    if (v.type == VAL_ERROR) return v;
    Value e;
    memset(&e, 0, sizeof(e));
    e.type = VAL_ERROR;
    e.v.err.type = strdup("RuntimeError");
    e.v.err.stack = NULL;
    if (v.type == VAL_STRING) {
        const char* s = lumyr_str_cstr(&v);
        e.v.err.message = strdup(s ? s : "");
    } else if ((v.type == VAL_CLASS_PTR || v.type == VAL_STRUCT_PTR) && v.v.struct_ptr) {
        /* Error 类实例（LumyrFunction/Error）：读取 message/errType 字段，
         * 未捕获打印保留真实错误信息；字段不存在时回退通用描述。
         * find_field 不查父链（字段已扁平化），不存在返回 NULL 不报错 */
        RuntimeTypeInfo* info = *(RuntimeTypeInfo**)v.v.struct_ptr;
        FieldInfo* fm = info ? lumyr_type_find_field(info, "message") : NULL;
        if (fm && fm->valtype == VAL_STRING) {
            Value m = lumyr_field_get(v, "message");
            const char* ms = lumyr_str_cstr(&m);
            e.v.err.message = strdup(ms ? ms : "exception");
            FieldInfo* ft = lumyr_type_find_field(info, "errType");
            if (ft && ft->valtype == VAL_STRING) {
                Value t = lumyr_field_get(v, "errType");
                const char* ts = lumyr_str_cstr(&t);
                if (ts) { free(e.v.err.type); e.v.err.type = strdup(ts); }
            }
        } else {
            e.v.err.message = strdup("exception");
        }
    } else {
        e.v.err.message = strdup("exception");
    }
    return e;
}

/* ========== TRY：注册异常处理器 ========== */
int vm_exec_try(VMExecCtx* ctx, Instruction* in) {
    TryCtxNode* n = (TryCtxNode*)malloc(sizeof(TryCtxNode));
    if (!n) { perror("vm_exec_try"); exit(EXIT_FAILURE); }
    n->frame    = ctx->frame;
    n->catch_pc = in->a;
    n->fin_pc   = in->b;
    n->caught   = 0;   /* 必须显式初始化：malloc 残留非零会使 catch
                        * 被误判"已使用"而静默跳过（多线程高频暴露） */
    n->prev     = g_try_stack;
    g_try_stack = n;
    return 1;
}

/* 帧返回时清理本帧注册、尚未弹出的 try 节点（try 内 return 不经过
 * ENDTRY/FINISH；残留节点会使后续 throw 被死帧处理器错误捕获并静默吞错）。
 * 同帧节点在链上连续（子帧节点总在父帧节点之上），弹到首个异帧节点即止。 */
void vm_except_leave_frame(VMExecCtx* ctx) {
    while(g_try_stack && g_try_stack->frame == ctx->frame) {
        TryCtxNode* d = g_try_stack;
        g_try_stack = d->prev;
        free(d);
    }
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
    return vm_except_throw_value(ctx, v);
}

/* 供其它指令内部调用：直接以给定 throw 值走分派（不经过栈）。
 * 返回 1 表示已被同层捕获或启动跨帧展开；未捕获时内部 exit(1)。 */
int vm_except_throw_value(VMExecCtx* ctx, Value v) {
    Value err = ensure_error(v);

    /* catch 块内 throw：栈顶 try 节点已标记 caught（同层捕获时保留节点）。
     * 本 try 有 finally 则先压 RETHROW 跳 finally——finally 跑完由 FINISH
     * 重抛向外（同时弹出节点），保证 catch 内抛错时本层 finally 不丢失；
     * 无 finally 则节点随下面的处理器搜索被当作"target 之上"节点释放。 */
    if(g_try_stack && g_try_stack->frame == ctx->frame &&
       g_try_stack->caught) {
        if(g_try_stack->fin_pc != 0) {
            FinNode* fn = (FinNode*)malloc(sizeof(FinNode));
            if(!fn) { perror("caught fin"); exit(EXIT_FAILURE); }
            fn->action = 2;    /* RETHROW */
            fn->target = 0;
            fn->next   = g_fin_stack;
            g_fin_stack = fn;
            g_current_error     = err;
            g_current_throw_val = v;
            ctx->pc = g_try_stack->fin_pc;
            return 1;
        }
        /* 无 finally：落到搜索逻辑，caught 节点会被跳过并随之外抛 */
    }

    /* 异常首先穿过同帧「仅 finally」try（catch_pc==0, fin_pc!=0）：
     * 压入 RETHROW 完成动作并跳到 finally 入口；finally 执行完由 FINISH
     * 重新抛出（继续向外搜真正的 catch）。不弹/不 free 该 try 节点
     * （FINISH 时统一 pop_current_try）。
     * 例外：若 throw 来自 finally 块内部（pc > fin_pc），说明 finally
     * 已在执行中，不再跳回 finally（否则死循环），而是弹出当前 try
     * 并继续向外搜 catch。 */
    if(g_try_stack && g_try_stack->frame == ctx->frame &&
       g_try_stack->catch_pc == 0 && g_try_stack->fin_pc != 0) {
        if(ctx->pc > g_try_stack->fin_pc) {
            /* finally 内部 throw：弹出当前 try，继续向外传播 */
            TryCtxNode* d = g_try_stack;
            g_try_stack = d->prev;
            free(d);
        } else {
            FinNode* fn = (FinNode*)malloc(sizeof(FinNode));
            if(!fn) { perror("fin rethrow"); exit(EXIT_FAILURE); }
            fn->action = 2;    /* RETHROW */
            fn->target = 0;
            fn->next   = g_fin_stack;
            g_fin_stack = fn;
            g_current_error     = err;
            g_current_throw_val = v;
            ctx->pc = g_try_stack->fin_pc;
            return 1;
        }
    }

    /* 沿 try 栈找第一个有 catch 且未被使用过的处理器（caught 节点的
     * catch 块已在执行，不得重复捕获其再抛出的错误） */
    TryCtxNode* t = g_try_stack;
    while (t && (t->catch_pc == 0 || t->caught)) t = t->prev;

    if (!t) {
        /* 未捕获 */
        if(g_thread_root) {
            /* 工作线程根帧：只终止本线程。记录错误、设置 g_err 状态后
             * longjmp 到 vm_thread_body 根兜底（g_err_jmp 由其设置）。
             * 不能直接返回：出错指令状态已污染，执行链必须中止。
             * 首次记录把错误压 protect（跨线程并发 GC 保护）；异常逐帧
             * 重抛经过本分支时不重复压。 */
            if(!g_thread_root_pushed) {
                g_thread_root_err = err;
                gc_protect_push(err);
                g_thread_root_pushed = 1;
            }
            if(g_err_jmp) {
                g_err_type_set(err.v.err.type ? err.v.err.type : "RuntimeError");
                g_err_msg_set(err.v.err.message ? err.v.err.message : "");
                longjmp(*g_err_jmp, 1);
            }
            /* 根兜底未设置（不应发生）：退回致命退出 */
        }
        /* 主线程（或无兜底）：打印错误信息并终止进程 */
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
        /* 同层捕获：保留 try 节点并标记 caught（不释放）。
         * catch 块正常完成由 ENDTRY 弹出；catch 内 throw 时上面的
         * caught 分支保证本层 finally 先执行，且同一 catch 不会
         * 重复捕获。 */
        int cpc = t->catch_pc;
        t->caught = 1;
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

/* 便捷：构造一个 VAL_ERROR 并抛出（供位运算等做类型校验的指令使用） */
void vm_except_raise_str(VMExecCtx* ctx, const char* type, const char* msg) {
    Value e;
    memset(&e, 0, sizeof(e));
    e.type = VAL_ERROR;
    e.v.err.type = strdup(type ? type : "RuntimeError");
    e.v.err.message = strdup(msg ? msg : "");
    e.v.err.stack = NULL;
    vm_except_throw_value(ctx, e);
}

/* 跨帧展开是否激活（供 runtime_error 长跳落地后的受防护循环判定走向） */
int vm_except_unwind_active(void) {
    return g_unwind.active ? 1 : 0;
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
        /* 到达捕获帧：定位栈顶属于捕获帧的 try 节点，保留并标记 caught
         * （与同层捕获一致：catch 内 throw 时本层 finally 不丢失，同一
         * catch 不重复捕获；catch 正常完成由 ENDTRY 弹出）。 */
        if (g_try_stack && g_try_stack->frame == ctx->frame) {
            g_try_stack->caught = 1;
        }
        g_current_error = g_unwind.error;
        g_current_throw_val = g_unwind.throw_val;
        ctx->pc = g_unwind.catch_pc;
        g_unwind.active = 0;
        return 1;
    }
    return -1;
}

/* ========== finally 完成动作（FinNode/g_fin_stack 定义见文件头部） ========== */

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

static _Thread_local RetSlot g_pending_ret;
static _Thread_local int g_has_pending_ret = 0;   /* PEND_RETURN 设置：有值待返回 */
static _Thread_local int g_wants_return = 0;      /* FINISH 设置：finally 已走完，请求结束当前帧 */

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
        int kind = action & 0xff;      /* 1=JMP 2=RETHROW 3=BREAK 4=CONT */
        int levels = action >> 8;      /* 需穿越的 finally 层数（编译期按词法位置计算） */
        free(n);
        pop_current_try();
        if(kind == 2) {
            /* RETHROW：重新走完整 throw 分派。下一个栈顶若带 catch，
             * 必须先给其捕获机会——旧代码见外层 fin_pc 非空就直接跳
             * finally，错误地跨过外层 catch（多层嵌套 try 时 catch 被
             * 静默跳过）；栈顶若为 finally-only 节点，throw_value
             * 开头的同帧 fin 分支会自行链式跳转其 finally。 */
            Value rv = g_current_error;
            stack_vm_push(g_stack_mgr, STACK_VALUE, &rv);
            return vm_exec_throw(ctx, in);
        }
        if(levels > 0) {
            /* 定层链：每执行一层 levels--；到 0 即跳目标（目标仍在 owner try 内，
             * owner try 未被弹出，其 finally 之后随该 try 正常退出执行）。 */
            levels--;
            if(levels > 0) {
                TryCtxNode* o = same_frame_outer_fin(ctx);
                if(o) {
                    FinNode* m = (FinNode*)malloc(sizeof(FinNode));
                    if(!m){ perror("fin chain"); exit(EXIT_FAILURE); }
                    m->action = kind | (levels << 8); m->target = target;
                    m->next = g_fin_stack; g_fin_stack = m;
                    ctx->pc = o->fin_pc;
                    return 1;
                }
                /* 外层缺失（不应发生）：落到目标避免卡死 */
            }
            ctx->pc = target;          /* BREAK / CONT 到位 */
            return 1;
        }
        /* 兼容旧编码（无 levels）：沿所有同帧 finally 链继续 */
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
        switch(kind) {
        case 1: case 3: case 4:           /* JMP / BREAK / CONT：跳到目标 */
            ctx->pc = target;
            break;
        }
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
   a=异常类型字符串常量下标(-1=捕获全部)；命中则 fall through，不命中跳 b。
   "Error" 视为所有错误的基类，匹配任意 err.type；其他类型名精确匹配。 */
int vm_exec_catch_match(VMExecCtx* ctx, Instruction* in) {
    if(in->a < 0) return 1;   /* catch-all 子句 */
    const char* want = ctx->const_pool[in->a].s;
    const char* have = g_current_error.v.err.type;
    if(have && want) {
        if(strcmp(want, "Error") == 0) return 1;  /* 基类：捕获任意错误 */
        if(strcmp(have, want) == 0) return 1;
    }
    ctx->pc = in->b;
    return 1;
}

/* ========== 工作线程根帧模式 ========== */
void vm_except_enter_thread_root(void) {
    g_thread_root = 1;
    g_thread_root_err = val_none();
    g_thread_root_pushed = 0;
}

void vm_except_leave_thread_root(void) {
    g_thread_root = 0;
}

Value vm_except_take_thread_root_error(void) {
    /* VM 协作式 throw 路径：throw_value 已记录 ensure_error 的 VAL_ERROR，
     * 取出时解除其 protect（调用方随即自行 protect_push，中间无 GC 点） */
    if(g_thread_root_err.type == VAL_ERROR) {
        if(g_thread_root_pushed) {
            gc_protect_pop();
            g_thread_root_pushed = 0;
        }
        return g_thread_root_err;
    }
    /* kit runtime_error 路径（longjmp 直接到根垫，未记录 Value）：
     * 按 g_err_type/message 构造 */
    return lumyr_make_error(
        (g_err_type && g_err_type[0]) ? g_err_type : "RuntimeError",
        g_err_msg ? g_err_msg : "", NULL);
}
