/*
 * 生成器模块实现
 *
 * 本模块提供生成器（generator）的支持，包括生成器对象的创建、执行、暂停、恢复，
 * 以及生成器的 GC 标记等功能。
 */

#include "vm_generator.h"
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


