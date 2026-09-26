/*
 * vm_generator.c - 生成器运行时实现
 *
 * 4 核心栈快照/恢复设计：
 *   - 生成器挂起时把全局栈 [caller_base_sp, sp) 区间数据拷贝到 gen 备份；
 *   - 恢复时把备份数据还原到全局栈 [caller_base_sp, base+gen_sp) 区域。
 *   调用方栈 [0, base) 数据不动，挂起期间生成器数据安全保留在 gen 中。
 *
 * OPC_YIELD 在 vm_exec.c 中处理：弹 yield 值写入 s_gen_yield_result，
 * 设置 s_gen_yielded=1，更新 gen->pc，return 0 结束 vm_exec_loop。
 */
#include "vm_generator.h"
#include "vm_types.h"
#include "stack_manager.h"
#include "ast/stackframe.h"
#include "ir_compile.h"
#include "lumyr_value.h"
#include "lumyr_value_type.h"
#include "lm_value.h"
#include "lm_type.h"
#include "gc_runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ========== 线程局部变量定义 ========== */
_Thread_local GeneratorObject* s_current_gen = NULL;
_Thread_local Value s_gen_yield_result;
_Thread_local int s_gen_yielded = 0;

/* ========== 状态访问函数 ========== */
GeneratorObject* vm_get_current_generator(void) { return s_current_gen; }
void vm_set_current_generator(GeneratorObject* gen) { s_current_gen = gen; }
Value vm_get_gen_yield_result(void) { return s_gen_yield_result; }
void vm_set_gen_yield_result(Value v) { s_gen_yield_result = v; }
int vm_get_gen_yielded(void) { return s_gen_yielded; }
void vm_set_gen_yielded(int y) { s_gen_yielded = y; }

/* ========== runtime_undefined ========== */
void runtime_undefined(const char* what, const char* name) {
    char buf[256];
    snprintf(buf, sizeof(buf),
             "运行时错误：未定义%s %s / runtime error: undefined %s %s",
             what, name ? name : "?", what, name ? name : "?");
    /* 经 runtime_error 走协作式 throw：try 可捕获；无 try 由 VM 退出 */
    runtime_error(buf);
}

/* ========== GC 标记 ========== */
/* 全局暂停生成器链表：当前实现无链表，预留扩展点 */
void lumyr_gc_mark_paused_generators(void) { /* 预留 */ }

/* 标记单个生成器 Value 持有的引用 */
void lumyr_gc_mark_generator(Value v) {
    if(v.type != VAL_GENERATOR || !v.v.generator) return;
    GeneratorObject* gen = (GeneratorObject*)v.v.generator;
    /* 标记 yield_value / send_value */
    if(gen->yield_value.type != VAL_NONE && gen->yield_value.type != VAL_INT64
       && gen->yield_value.type != VAL_DOUBLE && gen->yield_value.type != VAL_BOOL
       && gen->yield_value.type != VAL_CHAR) {
        gc_mark(gen->yield_value);
    }
    if(gen->send_value.type != VAL_NONE && gen->send_value.type != VAL_INT64
       && gen->send_value.type != VAL_DOUBLE && gen->send_value.type != VAL_BOOL
       && gen->send_value.type != VAL_CHAR) {
        gc_mark(gen->send_value);
    }
    /* 标记 4 栈备份中的引用类型 Value */
    if(gen->val_backup) {
        for(int i = 0; i < gen->sp_val; i++) {
            Value* vp = &gen->val_backup[i];
            if(vp->type != VAL_NONE && vp->type != VAL_INT64 && vp->type != VAL_DOUBLE
               && vp->type != VAL_BOOL && vp->type != VAL_CHAR) {
                gc_mark(*vp);
            }
        }
    }
    /* 标记 frame 中的 Value（stackframe 自身也可能持有堆引用） */
    if(gen->frame && gen->frame->vals) {
        for(int i = 0; i < gen->frame->cnt; i++) {
            Value* vp = &gen->frame->vals[i];
            if(vp->type != VAL_NONE && vp->type != VAL_INT64 && vp->type != VAL_DOUBLE
               && vp->type != VAL_BOOL && vp->type != VAL_CHAR) {
                gc_mark(*vp);
            }
        }
    }
}

/* ========== try-catch 上下文保存（当前为空实现，预留扩展） ========== */
void generator_save_try_context(GeneratorObject* gen) { (void)gen; }
void generator_restore_try_context(GeneratorObject* gen) { (void)gen; }
void generator_free_try_context(GeneratorObject* gen) { (void)gen; }

/* ========== 内部辅助：备份容量确保 ========== */
static void ensure_val_cap(GeneratorObject* gen, int need) {
    if(need <= gen->cap_val) return;
    int nc = gen->cap_val > 0 ? gen->cap_val : 8;
    while(nc < need) nc *= 2;
    Value* p = (Value*)realloc(gen->val_backup, sizeof(Value) * nc);
    if(!p) { perror("gen val_backup realloc"); return; }
    gen->val_backup = p;
    gen->cap_val = nc;
}
static void ensure_i64_cap(GeneratorObject* gen, int need) {
    if(need <= gen->cap_i64) return;
    int nc = gen->cap_i64 > 0 ? gen->cap_i64 : 8;
    while(nc < need) nc *= 2;
    int64_t* p = (int64_t*)realloc(gen->i64_backup, sizeof(int64_t) * nc);
    if(!p) { perror("gen i64_backup realloc"); return; }
    gen->i64_backup = p;
    gen->cap_i64 = nc;
}
static void ensure_dbl_cap(GeneratorObject* gen, int need) {
    if(need <= gen->cap_dbl) return;
    int nc = gen->cap_dbl > 0 ? gen->cap_dbl : 8;
    while(nc < need) nc *= 2;
    double* p = (double*)realloc(gen->dbl_backup, sizeof(double) * nc);
    if(!p) { perror("gen dbl_backup realloc"); return; }
    gen->dbl_backup = p;
    gen->cap_dbl = nc;
}
static void ensure_ptr_cap(GeneratorObject* gen, int need) {
    if(need <= gen->cap_ptr) return;
    int nc = gen->cap_ptr > 0 ? gen->cap_ptr : 8;
    while(nc < need) nc *= 2;
    void** p = (void**)realloc(gen->ptr_backup, sizeof(void*) * nc);
    if(!p) { perror("gen ptr_backup realloc"); return; }
    gen->ptr_backup = p;
    gen->cap_ptr = nc;
}

/* ========== 创建/销毁生成器 ========== */
GeneratorObject* generator_new(BytecodeFunc* bf, StackFrame* parent_frame, int arg_cnt, const Value* args) {
    if(!bf) return NULL;
    GeneratorObject* gen = (GeneratorObject*)calloc(1, sizeof(GeneratorObject));
    if(!gen) { perror("generator_new"); return NULL; }
    gen->bf = bf;
    gen->frame = stackframe_new(parent_frame);
    /* 绑定参数到 frame（按形参名 + 形参位置） */
    int name_slots = bf->param_cnt + bf->has_variadic;
    for(int i = 0; i < arg_cnt && i < name_slots; i++) {
        const char* pname = (i < bf->param_cnt && bf->params[i]) ? bf->params[i] : "_";
        stackframe_bind(gen->frame, pname, args[i]);
    }
    gen->pc = 0;
    gen->sp_val = gen->sp_i64 = gen->sp_dbl = gen->sp_ptr = 0;
    gen->cap_val = gen->cap_i64 = gen->cap_dbl = gen->cap_ptr = 0;
    gen->val_backup = NULL; gen->i64_backup = NULL;
    gen->dbl_backup = NULL; gen->ptr_backup = NULL;
    gen->finished = 0;
    gen->started = 0;
    gen->yield_value = val_none();
    gen->send_value = val_none();
    return gen;
}

/* 创建生成器对象（frame 已由调用方建好并绑定参数；生成器接管 frame 生命周期） */
GeneratorObject* generator_new_with_frame(BytecodeFunc* bf, StackFrame* frame) {
    if(!bf) return NULL;
    GeneratorObject* gen = (GeneratorObject*)calloc(1, sizeof(GeneratorObject));
    if(!gen) { perror("generator_new_with_frame"); return NULL; }
    gen->bf = bf;
    gen->frame = frame;  /* 接管 frame */
    gen->pc = 0;
    gen->sp_val = gen->sp_i64 = gen->sp_dbl = gen->sp_ptr = 0;
    gen->cap_val = gen->cap_i64 = gen->cap_dbl = gen->cap_ptr = 0;
    gen->val_backup = NULL; gen->i64_backup = NULL;
    gen->dbl_backup = NULL; gen->ptr_backup = NULL;
    gen->finished = 0;
    gen->started = 0;
    gen->yield_value = val_none();
    gen->send_value = val_none();
    return gen;
}

void generator_free(GeneratorObject* gen) {
    if(!gen) return;
    if(gen->frame) stackframe_destroy(gen->frame);
    free(gen->val_backup);
    free(gen->i64_backup);
    free(gen->dbl_backup);
    free(gen->ptr_backup);
    free(gen);
}

/* ========== 恢复生成器执行 ==========
 * 返回 1 = 正常 yield，结果在 *result；返回 0 = 生成器结束（result 为 NONE） */
int generator_resume(GeneratorObject* gen, Value* result, Value* send_val,
                     StackFrame* frame, EvalCtx* ctx) {
    (void)frame; (void)ctx;  /* 生成器自带 frame，调用方 ctx 不参与执行 */
    if(!gen || !gen->bf) {
        *result = val_none();
        return 0;
    }
    if(gen->finished) {
        *result = val_none();
        return 0;
    }
    /* 设置 send_value（下次 yield 时可用 receive() 读取） */
    if(send_val) gen->send_value = *send_val;
    else { gen->send_value.type = VAL_NONE; gen->send_value.v.i = 0; }

    /* 保存调用方全局栈 sp（生成器挂起期间调用方栈 [0, base_sp) 不动） */
    int base_val = g_stack_mgr->sp[STACK_VALUE];
    int base_i64 = g_stack_mgr->sp[STACK_INT64];
    int base_dbl = g_stack_mgr->sp[STACK_DOUBLE];
    int base_ptr = g_stack_mgr->sp[STACK_PTR];

    if(gen->started) {
        /* 恢复生成器栈：把备份数据拷贝到全局栈 [base, base+gen_sp) 区间 */
        stack_global_ensure(STACK_VALUE, base_val + gen->sp_val);
        stack_global_ensure(STACK_INT64, base_i64 + gen->sp_i64);
        stack_global_ensure(STACK_DOUBLE, base_dbl + gen->sp_dbl);
        stack_global_ensure(STACK_PTR, base_ptr + gen->sp_ptr);
        Value*   vstack = (Value*)g_stack_mgr->stacks[STACK_VALUE];
        int64_t* istack = (int64_t*)g_stack_mgr->stacks[STACK_INT64];
        double*  dstack = (double*)g_stack_mgr->stacks[STACK_DOUBLE];
        void**   pstack = (void**)g_stack_mgr->stacks[STACK_PTR];
        for(int i = 0; i < gen->sp_val; i++) vstack[base_val + i] = gen->val_backup[i];
        for(int i = 0; i < gen->sp_i64; i++) istack[base_i64 + i] = gen->i64_backup[i];
        for(int i = 0; i < gen->sp_dbl; i++) dstack[base_dbl + i] = gen->dbl_backup[i];
        for(int i = 0; i < gen->sp_ptr; i++) pstack[base_ptr + i] = gen->ptr_backup[i];
        g_stack_mgr->sp[STACK_VALUE]  = base_val + gen->sp_val;
        g_stack_mgr->sp[STACK_INT64] = base_i64 + gen->sp_i64;
        g_stack_mgr->sp[STACK_DOUBLE] = base_dbl + gen->sp_dbl;
        g_stack_mgr->sp[STACK_PTR]   = base_ptr + gen->sp_ptr;
    } else {
        /* 首次启动：全局栈保持调用方 sp，生成器从空栈开始 */
        gen->started = 1;
    }

    /* 设置当前生成器（OPC_YIELD 会读取） */
    GeneratorObject* prev_gen = s_current_gen;
    s_current_gen = gen;
    s_gen_yielded = 0;
    s_gen_yield_result = val_none();

    /* 构造生成器 VMExecCtx 并进入 vm_exec_loop */
    VMExecCtx gen_ctx;
    memset(&gen_ctx, 0, sizeof(gen_ctx));
    gen_ctx.fn         = gen->bf;
    gen_ctx.code       = gen->bf->code;
    gen_ctx.pc         = gen->pc;  /* 上次挂起 pc（首次为 0） */
    gen_ctx.frame      = gen->frame;
    gen_ctx.const_pool = gen->bf->const_pool;
    gen_ctx.syms       = (const char**)gen->bf->syms;
    gen_ctx.const_cnt  = gen->bf->const_cnt;
    gen_ctx.sym_cnt    = gen->bf->sym_cnt;
    /* stacks 字段未使用（vm_exec_guarded 走 g_stack_mgr 全局） */

    RetSlot ret;
    memset(&ret, 0, sizeof(ret));
    int status = vm_exec_guarded(&gen_ctx, &ret);
    (void)status;

    /* 恢复当前生成器指针 */
    s_current_gen = prev_gen;

    /* 保存生成器栈状态：把全局栈 [base, current_sp) 区间数据拷贝到备份 */
    int new_sp_val = g_stack_mgr->sp[STACK_VALUE]  - base_val;
    int new_sp_i64 = g_stack_mgr->sp[STACK_INT64] - base_i64;
    int new_sp_dbl = g_stack_mgr->sp[STACK_DOUBLE] - base_dbl;
    int new_sp_ptr = g_stack_mgr->sp[STACK_PTR]   - base_ptr;
    if(new_sp_val < 0) new_sp_val = 0;
    if(new_sp_i64 < 0) new_sp_i64 = 0;
    if(new_sp_dbl < 0) new_sp_dbl = 0;
    if(new_sp_ptr < 0) new_sp_ptr = 0;

    ensure_val_cap(gen, new_sp_val);
    ensure_i64_cap(gen, new_sp_i64);
    ensure_dbl_cap(gen, new_sp_dbl);
    ensure_ptr_cap(gen, new_sp_ptr);
    Value*   vstack = (Value*)g_stack_mgr->stacks[STACK_VALUE];
    int64_t* istack = (int64_t*)g_stack_mgr->stacks[STACK_INT64];
    double*  dstack = (double*)g_stack_mgr->stacks[STACK_DOUBLE];
    void**   pstack = (void**)g_stack_mgr->stacks[STACK_PTR];
    for(int i = 0; i < new_sp_val; i++) gen->val_backup[i] = vstack[base_val + i];
    for(int i = 0; i < new_sp_i64; i++) gen->i64_backup[i] = istack[base_i64 + i];
    for(int i = 0; i < new_sp_dbl; i++) gen->dbl_backup[i] = dstack[base_dbl + i];
    for(int i = 0; i < new_sp_ptr; i++) gen->ptr_backup[i] = pstack[base_ptr + i];
    gen->sp_val = new_sp_val;
    gen->sp_i64 = new_sp_i64;
    gen->sp_dbl = new_sp_dbl;
    gen->sp_ptr = new_sp_ptr;

    /* 保存 pc（下次从这里继续） */
    gen->pc = gen_ctx.pc;

    /* 恢复全局栈 sp 到调用方状态 */
    g_stack_mgr->sp[STACK_VALUE]  = base_val;
    g_stack_mgr->sp[STACK_INT64]  = base_i64;
    g_stack_mgr->sp[STACK_DOUBLE] = base_dbl;
    g_stack_mgr->sp[STACK_PTR]    = base_ptr;

    /* 判断结果 */
    if(s_gen_yielded) {
        s_gen_yielded = 0;
        *result = s_gen_yield_result;
        gen->yield_value = s_gen_yield_result;
        return 1;
    }
    /* 生成器自然结束（return / 走到末尾） */
    gen->finished = 1;
    *result = val_none();
    return 0;
}

/* ========== 包装 next()：供 BUILTIN_NEXT 调用 ========== */
int wrapped_gen_next(GeneratorObject* gen, Value* result, StackFrame* frame, EvalCtx* ctx) {
    return generator_resume(gen, result, NULL, frame, ctx);
}
