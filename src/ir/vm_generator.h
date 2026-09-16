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

#endif /* LUMYR_VM_GENERATOR_H */
