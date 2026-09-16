/*
 * 生成器模块实现
 *
 * 本模块提供生成器（generator）的支持，包括生成器对象的创建、执行、暂停、恢复，
 * 以及生成器的 GC 标记等功能。
 */

#include "vm_generator.h"
#include "vm_internal.h"
#include "vm_try_context.h"
#include "lumyr_log.h"
#include "lm_value.h"
#include <setjmp.h>
#include "gc_runtime.h"
#include <stdlib.h>
#include <string.h>

/* ========== 生成器状态管理（线程局部变量） ========== */

/* 当前正在执行的生成器（NULL = 普通执行） */
static _Thread_local GeneratorObject* s_current_gen = NULL;
/* 生成器 yield 时的返回值传递 */
static _Thread_local Value s_gen_yield_result;
static _Thread_local int s_gen_yielded = 0;

/* 获取当前正在执行的生成器（NULL = 普通执行） */
GeneratorObject* vm_get_current_generator(void) {
    return s_current_gen;
}

/* 设置当前正在执行的生成器 */
void vm_set_current_generator(GeneratorObject* gen) {
    s_current_gen = gen;
}

/* 获取生成器 yield 时的返回值 */
Value vm_get_gen_yield_result(void) {
    return s_gen_yield_result;
}

/* 设置生成器 yield 时的返回值 */
void vm_set_gen_yield_result(Value v) {
    s_gen_yield_result = v;
}

/* 获取是否已经 yield */
int vm_get_gen_yielded(void) {
    return s_gen_yielded;
}

/* 设置是否已经 yield */
void vm_set_gen_yielded(int yielded) {
    s_gen_yielded = yielded;
}

/* ========== 暂停生成器 GC 根注册 ==========
 * 生成器 yield 暂停后，其 stack/frame 不再是 VM 当前 GC 根，
 * 但内部仍持有 GC 对象引用（字符串/数组/map）。若不注册为 GC 根，
 * GC 会错误回收这些对象，内存复用后覆盖生成器字段导致堆破坏。
 */
static GeneratorObject** s_paused_gens = NULL;
static int s_paused_gen_cnt = 0;
static int s_paused_gen_cap = 0;

static void paused_gen_add(GeneratorObject* gen) {
    if(s_paused_gen_cnt >= s_paused_gen_cap) {
        int nc = s_paused_gen_cap > 0 ? s_paused_gen_cap * 2 : 16;
        GeneratorObject** ns = (GeneratorObject**)realloc(s_paused_gens, (size_t)nc * sizeof(GeneratorObject*));
        if(!ns) { exit(EXIT_FAILURE); }
        s_paused_gens = ns;
        s_paused_gen_cap = nc;
    }
    s_paused_gens[s_paused_gen_cnt++] = gen;
}

static void paused_gen_remove(GeneratorObject* gen) {
    for(int i = 0; i < s_paused_gen_cnt; i++) {
        if(s_paused_gens[i] == gen) {
            s_paused_gens[i] = s_paused_gens[--s_paused_gen_cnt];
            return;
        }
    }
}

/* 递归标记单个生成器持有的所有 GC 对象引用（包括被包装的子生成器） */
static void mark_generator_refs(GeneratorObject* gen, int depth) {
    if(!gen || depth > 16) return;  /* 防止循环引用导致无限递归 */
    /* 标记执行栈中的 Value */
    if(gen->stack && gen->sp > 0) {
        for(int j = 0; j < gen->sp; j++) {
            gc_mark(gen->stack[j]);
        }
    }
    /* 标记栈帧及父帧链中的局部变量 */
    StackFrame* f = gen->frame;
    while(f) {
        if(f->vals) {
            for(int j = 0; j < f->cnt; j++) {
                gc_mark(f->vals[j]);
            }
        }
        f = f->parent;
    }
    /* 标记包装生成器持有的函数对象 */
    if(gen->is_wrapped && gen->wrap_fn) {
        RuntimeFunc* rf = gen->wrap_fn;
        if(rf->captures) {
            for(int j = 0; j < rf->capture_count; j++) {
                gc_mark(rf->captures[j]);
            }
        }
    }
    /* 标记 send_value / yield_value / pending_exception */
    gc_mark(gen->yield_value);
    gc_mark(gen->send_value);
    gc_mark(gen->pending_exception);
    /* 递归标记被包装的子生成器（关键：子生成器可能不在暂停列表中，
     * 因为 generator_resume 返回时会从列表移除，但它仍持有 GC 对象引用） */
    if(gen->is_wrapped) {
        mark_generator_refs(gen->wrapped_gen, depth + 1);
        mark_generator_refs(gen->wrapped_gen2, depth + 1);
    }
}

/* GC 标记回调：遍历所有暂停生成器，标记其 stack 和 frame 中的 Value */
void lumyr_gc_mark_paused_generators(void) {
    for(int i = 0; i < s_paused_gen_cnt; i++) {
        GeneratorObject* gen = s_paused_gens[i];
        mark_generator_refs(gen, 0);
    }
}

/* GC 标记回调：标记单个生成器 Value 持有的所有引用。
 * gc_mark 遇到 VAL_GENERATOR 时调用此函数，确保生成器无论在栈上、数组中、
 * map 中还是暂停列表中，其持有的引用（wrapped_gen/wrap_fn/stack/frame 等）
 * 都能被正确标记，避免 GC 错误回收导致堆破坏。 */
void lumyr_gc_mark_generator(Value v) {
    if(v.type != VAL_GENERATOR || !v.v.generator) return;
    GeneratorObject* gen = (GeneratorObject*)v.v.generator;
    mark_generator_refs(gen, 0);
}

/* ========== 生成器实现 ========== */

/* 前向声明 */
void generator_free_try_context(GeneratorObject* gen);
int wrapped_gen_next(GeneratorObject* gen, Value* result, StackFrame* frame, EvalCtx* ctx);
int generator_resume(GeneratorObject* gen, Value* result, Value* send_val, StackFrame* frame, EvalCtx* ctx);

void runtime_undefined(const char* what, const char* name)
{
    LOG_ERROR("Runtime Error: 未定义%s: %s\n", what, name);
    exit(EXIT_FAILURE);
}

int generator_resume(GeneratorObject* gen, Value* result, Value* send_val, StackFrame* frame, EvalCtx* ctx)
{
    if(gen->finished) { *result = val_none(); return 0; }
    /* 包装生成器：直接调用包装逻辑，不执行字节码 */
    if(gen->is_wrapped) {
        /* 包装生成器恢复时从暂停 GC 根列表移除，返回值（暂停）时重新添加。
         * 包装生成器不是通过 OPC_YIELD 暂停，而是 wrapped_gen_next 返回值暂停，
         * 因此需要在这里单独管理 GC 根注册，否则内部持有的 GC 对象会被错误回收。 */
        paused_gen_remove(gen);
        int has = wrapped_gen_next(gen, result, frame, ctx);
        if(has) paused_gen_add(gen);
        return has;
    }

    /* 设置 send_value（如果有） */
    if(send_val) {
        gen->send_value = *send_val;
        gen->has_send_value = 1;
    } else {
        gen->has_send_value = 0;
    }

    /* setjmp 恢复点：vm_run 中遇到 OPC_YIELD 时 longjmp 到这里 */
    if(setjmp(gen->resume_point) == 0) {
        /* 第一次进入或从 next 恢复：调用 vm_run 执行字节码 */
        s_current_gen = gen;
        paused_gen_remove(gen);  /* 恢复执行前从暂停 GC 根列表移除（执行期间 stack/frame 是当前 VM 根） */
        EvalCtx ctx;
        memset(&ctx, 0, sizeof(ctx));
        gen->ctx = &ctx;
        Value ret = vm_run(gen->bf, gen->frame, &ctx);
        /* vm_run 正常返回：生成器结束，释放 try-catch 上下文 */
        s_current_gen = NULL;
        gen->finished = 1;
        generator_free_try_context(gen);
        paused_gen_remove(gen);  /* 生成器结束时从暂停 GC 根列表移除 */
        *result = ret;
        return 0;
    } else {
        /* 从 yield longjmp 回来：返回 yield 的值。
         * 注意：不调用 paused_gen_remove(gen)，生成器保持在暂停 GC 根列表中，
         * 因为它仍然持有 GC 对象引用（stack/frame 中的 Value），直到下次恢复或被释放。 */
        s_current_gen = NULL;
        *result = s_gen_yield_result;
        return 1;
    }
}

int wrapped_gen_next(GeneratorObject* gen, Value* result, StackFrame* frame, EvalCtx* ctx)
{
    if(gen->finished) { *result = val_none(); return 0; }

    switch(gen->wrap_type) {
        case WRAP_MAP: {
            Value v;
            int has = generator_resume(gen->wrapped_gen, &v, NULL, frame, ctx);
            if(!has) { gen->finished = 1; *result = val_none(); return 0; }
            Value a1[1] = { v };
            Value r = vm_call_rf(gen->wrap_fn, a1, 1, frame, ctx);
            *result = r;
            return 1;
        }
        case WRAP_FILTER: {
            while(1) {
                Value v;
                int has = generator_resume(gen->wrapped_gen, &v, NULL, frame, ctx);
                if(!has) { gen->finished = 1; *result = val_none(); return 0; }
                Value a1[1] = { v };
                Value r = vm_call_rf(gen->wrap_fn, a1, 1, frame, ctx);
                if(lumyr_to_bool(r)) { *result = v; return 1; }
            }
        }
        case WRAP_SKIP: {
            while(gen->wrap_index < gen->wrap_arg) {
                Value v;
                int has = generator_resume(gen->wrapped_gen, &v, NULL, frame, ctx);
                if(!has) { gen->finished = 1; *result = val_none(); return 0; }
                gen->wrap_index++;
            }
            Value v;
            int has = generator_resume(gen->wrapped_gen, &v, NULL, frame, ctx);
            if(!has) { gen->finished = 1; *result = val_none(); return 0; }
            *result = v;
            return 1;
        }
        case WRAP_TAKE: {
            if(gen->wrap_index >= gen->wrap_arg) { gen->finished = 1; *result = val_none(); return 0; }
            Value v;
            int has = generator_resume(gen->wrapped_gen, &v, NULL, frame, ctx);
            if(!has) { gen->finished = 1; *result = val_none(); return 0; }
            gen->wrap_index++;
            *result = v;
            return 1;
        }
        case WRAP_ENUMERATE: {
            Value v;
            int has = generator_resume(gen->wrapped_gen, &v, NULL, frame, ctx);
            if(!has) { gen->finished = 1; *result = val_none(); return 0; }
            Value arr = val_array(2);
            arr.v.array->items[0] = val_int(gen->wrap_index);
            arr.v.array->items[1] = v;
            gen->wrap_index++;
            *result = arr;
            return 1;
        }
        case WRAP_CHAIN: {
            Value v;
            int has = generator_resume(gen->wrapped_gen, &v, NULL, frame, ctx);
            if(has) { *result = v; return 1; }
            has = generator_resume(gen->wrapped_gen2, &v, NULL, frame, ctx);
            if(!has) { gen->finished = 1; *result = val_none(); return 0; }
            *result = v;
            return 1;
        }
        case WRAP_ZIP: {
            Value v1, v2;
            int has1 = generator_resume(gen->wrapped_gen, &v1, NULL, frame, ctx);
            int has2 = generator_resume(gen->wrapped_gen2, &v2, NULL, frame, ctx);
            if(!has1 || !has2) { gen->finished = 1; *result = val_none(); return 0; }
            Value arr = val_array(2);
            arr.v.array->items[0] = v1;
            arr.v.array->items[1] = v2;
            *result = arr;
            return 1;
        }
        default: {
            char buf[256];
            snprintf(buf, sizeof(buf), "未知包装生成器类型: wrap_type=%d is_wrapped=%d gen=%p",
                     gen->wrap_type, gen->is_wrapped, (void*)gen);
            runtime_error(buf);
        }
    }
    return 0;
}

void generator_free_try_context(GeneratorObject* gen)
{
    free(gen->saved_vm_jbs);
    free(gen->saved_vm_prev);
    free(gen->saved_vm_sp);
    free(gen->saved_vm_target);
    free(gen->saved_vm_tn);
    free(gen->saved_vm_fn);
    free(gen->saved_vm_fin_act);
    free(gen->saved_vm_fin_tgt);
    free(gen->saved_vm_fin_dep);
    gen->saved_vm_jbs = NULL;
    gen->saved_vm_prev = NULL;
    gen->saved_vm_sp = NULL;
    gen->saved_vm_target = NULL;
    gen->saved_vm_tn = NULL;
    gen->saved_vm_fn = NULL;
    gen->saved_vm_fin_act = NULL;
    gen->saved_vm_fin_tgt = NULL;
    gen->saved_vm_fin_dep = NULL;
}

void generator_restore_try_context(GeneratorObject* gen)
{
    int depth = gen->saved_vm_depth;
    int fin_n = gen->saved_vm_fin_n;

    /* 确保数组容量足够 */
    vm_ensure(depth > fin_n ? depth : fin_n);

    vm_depth = depth;
    vm_fin_n = fin_n;
    /* g_err_jmp/vm_jbs/vm_prev 不恢复：它们指向 yield 时的旧 C 栈帧，
     * 生成器恢复时旧栈帧已销毁，longjmp 到旧缓冲区是未定义行为（0xC0000005）。
     * 这些指针/缓冲区在 vm_run 中通过重新执行 setjmp 在当前栈帧重建。
     * 这里只恢复不依赖 C 栈帧的逻辑状态（vm_sp/vm_target/vm_tn/vm_fn）。 */

    if(depth > 0) {
        memcpy(vm_sp, gen->saved_vm_sp, (size_t)depth * sizeof(int));
        memcpy(vm_target, gen->saved_vm_target, (size_t)depth * sizeof(int));
        memcpy(vm_tn, gen->saved_vm_tn, (size_t)depth * sizeof(int));
        memcpy(vm_fn, gen->saved_vm_fn, (size_t)depth * sizeof(int));
    }

    if(fin_n > 0) {
        memcpy(vm_fin_act, gen->saved_vm_fin_act, (size_t)fin_n * sizeof(int));
        memcpy(vm_fin_tgt, gen->saved_vm_fin_tgt, (size_t)fin_n * sizeof(int));
        memcpy(vm_fin_dep, gen->saved_vm_fin_dep, (size_t)fin_n * sizeof(int));
    }
}

void generator_save_try_context(GeneratorObject* gen)
{
    int depth = vm_depth;
    int fin_n = vm_fin_n;
    gen->saved_vm_depth = depth;
    gen->saved_vm_fin_n = fin_n;
    gen->saved_g_err_jmp = g_err_jmp;

    /* 先释放旧数组，避免多次 yield 导致内存泄漏 */
    generator_free_try_context(gen);

    /* 分配内存保存数组 */
    if(depth > 0) {
        gen->saved_vm_jbs = (jmp_buf*)malloc((size_t)depth * sizeof(jmp_buf));
        gen->saved_vm_prev = (jmp_buf**)malloc((size_t)depth * sizeof(jmp_buf*));
        gen->saved_vm_sp = (int*)malloc((size_t)depth * sizeof(int));
        gen->saved_vm_target = (int*)malloc((size_t)depth * sizeof(int));
        gen->saved_vm_tn = (int*)malloc((size_t)depth * sizeof(int));
        gen->saved_vm_fn = (int*)malloc((size_t)depth * sizeof(int));
        memcpy(gen->saved_vm_jbs, vm_jbs, (size_t)depth * sizeof(jmp_buf));
        memcpy(gen->saved_vm_prev, vm_prev, (size_t)depth * sizeof(jmp_buf*));
        memcpy(gen->saved_vm_sp, vm_sp, (size_t)depth * sizeof(int));
        memcpy(gen->saved_vm_target, vm_target, (size_t)depth * sizeof(int));
        memcpy(gen->saved_vm_tn, vm_tn, (size_t)depth * sizeof(int));
        memcpy(gen->saved_vm_fn, vm_fn, (size_t)depth * sizeof(int));
    } else {
        gen->saved_vm_jbs = NULL;
        gen->saved_vm_prev = NULL;
        gen->saved_vm_sp = NULL;
        gen->saved_vm_target = NULL;
        gen->saved_vm_tn = NULL;
        gen->saved_vm_fn = NULL;
    }

    if(fin_n > 0) {
        gen->saved_vm_fin_act = (int*)malloc((size_t)fin_n * sizeof(int));
        gen->saved_vm_fin_tgt = (int*)malloc((size_t)fin_n * sizeof(int));
        gen->saved_vm_fin_dep = (int*)malloc((size_t)fin_n * sizeof(int));
        memcpy(gen->saved_vm_fin_act, vm_fin_act, (size_t)fin_n * sizeof(int));
        memcpy(gen->saved_vm_fin_tgt, vm_fin_tgt, (size_t)fin_n * sizeof(int));
        memcpy(gen->saved_vm_fin_dep, vm_fin_dep, (size_t)fin_n * sizeof(int));
    } else {
        gen->saved_vm_fin_act = NULL;
        gen->saved_vm_fin_tgt = NULL;
        gen->saved_vm_fin_dep = NULL;
    }
}

void generator_free(GeneratorObject* gen)
{
    if(!gen) return;
    paused_gen_remove(gen);
    generator_free_try_context(gen);
    if(gen->stack) free(gen->stack);
    if(gen->frame) stackframe_destroy(gen->frame);
    free(gen);
}

GeneratorObject* generator_new(BytecodeFunc* bf, StackFrame* parent_frame,
                                        int arg_cnt, const Value* args)
{
    GeneratorObject* gen = (GeneratorObject*)calloc(1, sizeof(GeneratorObject));
    if(!gen) { LOG_ERROR("generator_new: 内存不足\n"); exit(EXIT_FAILURE); }
    gen->bf = bf;
    /* 生成器使用独立栈帧，不持有父栈帧指针（避免父栈帧被释放后的 UAF）。
       生成器通过全局符号表访问全局变量和函数；局部变量在生成器自己的栈帧中。 */
    gen->frame = stackframe_new(NULL);
    (void)parent_frame;  /* 保留参数兼容性，实际不使用 */
    gen->max_stack = bc_analyze_stack(bf, NULL, 0);
    if(gen->max_stack < 0) gen->max_stack = 64;
    gen->stack = (Value*)malloc(sizeof(Value) * (gen->max_stack + 64));
    gen->sp = 0;
    gen->pc = 0;
    gen->finished = 0;
    gen->started = 0;
    gen->ctx = NULL;
    /* 绑定参数到栈帧 */
    for(int i = 0; i < bf->param_cnt && i < arg_cnt; i++) {
        if(bf->params[i]) stackframe_bind(gen->frame, bf->params[i], args[i]);
    }
    return gen;
}




