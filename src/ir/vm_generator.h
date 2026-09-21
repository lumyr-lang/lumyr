#ifndef LUMYR_VM_GENERATOR_H
#define LUMYR_VM_GENERATOR_H

#include "vm_types.h"
#include "bytecode.h"
#include "ast/ast_node.h"
#include "lumyr_value_type.h"
#include <setjmp.h>

/*
 * 生成器模块
 *
 * 本模块提供生成器（generator）的支持，包括生成器对象的创建、执行、暂停、恢复，
 * 以及生成器的 GC 标记等功能。
 */

/* 前向声明 */
typedef struct StackFrame StackFrame;
typedef struct EvalCtx EvalCtx;
typedef struct RuntimeFunc RuntimeFunc;

/* ========== 生成器 GC 标记 ========== */

/* GC 标记回调：遍历所有暂停生成器，标记其 stack 和 frame 中的 Value */
void lumyr_gc_mark_paused_generators(void);

/* GC 标记回调：标记单个生成器 Value 持有的所有引用 */
void lumyr_gc_mark_generator(Value v);

/* ========== 生成器状态管理 ========== */

/* 获取当前正在执行的生成器（NULL = 普通执行） */
GeneratorObject* vm_get_current_generator(void);

/* 设置当前正在执行的生成器 */
void vm_set_current_generator(GeneratorObject* gen);

/* 获取生成器 yield 时的返回值 */
Value vm_get_gen_yield_result(void);

/* 设置生成器 yield 时的返回值 */
void vm_set_gen_yield_result(Value v);

/* 获取是否已经 yield */
int vm_get_gen_yielded(void);

/* 设置是否已经 yield */
void vm_set_gen_yielded(int yielded);


/* ========== 生成器对象管理 ========== */

/* 创建生成器对象（不开始执行） */
GeneratorObject* generator_new(BytecodeFunc* bf, StackFrame* parent_frame, int arg_cnt, const Value* args);

/* 创建生成器对象（frame 已由调用方建好并绑定参数；生成器接管 frame 生命周期） */
GeneratorObject* generator_new_with_frame(BytecodeFunc* bf, StackFrame* frame);

/* 销毁生成器对象 */
void generator_free(GeneratorObject* gen);

/* 生成器执行函数：恢复状态，执行到下一个 yield 或 return
 * 返回 1 = 正常 yield，结果在 *result；返回 0 = 生成器结束 */
int generator_resume(GeneratorObject* gen, Value* result, Value* send_val, StackFrame* frame, EvalCtx* ctx);

/* 保存当前 try-catch 上下文到生成器对象（yield 时调用） */
void generator_save_try_context(GeneratorObject* gen);

/* 从生成器对象恢复 try-catch 上下文（恢复执行时调用） */
void generator_restore_try_context(GeneratorObject* gen);

/* 释放生成器保存的 try-catch 上下文 */
void generator_free_try_context(GeneratorObject* gen);

/* 包装生成器的 next() 处理 */
int wrapped_gen_next(GeneratorObject* gen, Value* result, StackFrame* frame, EvalCtx* ctx);

/* 未定义变量/函数：统一报错退出（与 ast_interp.c 输出一致） */
void runtime_undefined(const char* what, const char* name);


/* ========== 生成器状态变量（线程局部） ========== */

/* 当前正在执行的生成器（NULL = 普通执行） */
extern _Thread_local GeneratorObject* s_current_gen;
/* 生成器 yield 时的返回值传递 */
extern _Thread_local Value s_gen_yield_result;
extern _Thread_local int s_gen_yielded;

#endif /* LUMYR_VM_GENERATOR_H */
