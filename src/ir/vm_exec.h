#ifndef LUMYR_VM_EXEC_H
#define LUMYR_VM_EXEC_H

#include "bytecode.h"
#include "lumyr_value_type.h"
#include "ast/stackframe.h"
#include "vm_types.h"

/*
 * VM 执行模块
 *
 * 本模块提供 VM 的核心执行功能，包括字节码指令的执行和内置函数的执行。
 */

/* 虚拟机主循环：执行字节码函数 */
Value vm_run(BytecodeFunc* bf, StackFrame* frame, EvalCtx* ctx);

/* 执行内置指令（全局形式：argv[0] 即首参/receiver） */
int vm_exec_builtin(VMExecCtx* ctx, const Instruction* in);

/* 执行内置方法调用指令（方法/属性形式：VALUE 栈顶 argc 个实参 + receiver） */
int vm_exec_builtin_method(VMExecCtx* ctx, const Instruction* in);

/* 内置统一分发：switch(BuiltinId) → 内层按 receiver 运行时类型分派。
 * argv[0] 为 receiver。is_method=1（方法形式）argc 不含 receiver；
 * is_method=0（全局形式）argv[0]=首参即 receiver，argc 含它。
 * 返回 1=成功（结果写入 *out）；0=错误；VM_LOOP_UNWIND=回调异常向上传播 */
int builtin_dispatch(VMExecCtx* ctx, int id, Value* argv, int argc, Value* out, int is_method);

/* 调用一个函数值（VAL_FUNC）：绑定实参并执行，返回值写入 *out。
 * OPC_CALLV 与 map/filter/reduce 高阶内置共用。
 * 返回 1=成功；0=失败（已打印错误）；VM_LOOP_UNWIND=异常穿过本调用 */
int vm_call_func_value(VMExecCtx* ctx, Value fv, int argc, Value* args, Value* out);

/* BuiltinId 的反汇编名（bytecode.c 名表用） */
const char* builtin_id_name(int id);

#endif /* LUMYR_VM_EXEC_H */
