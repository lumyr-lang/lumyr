#ifndef LUMYR_VM_INTERNAL_H
#define LUMYR_VM_INTERNAL_H

#include "bytecode.h"
#include "lumyr_value_type.h"
#include "ast/stackframe.h"
#include "lm_thread.h"
#include "vm_types.h"

/*
 * VM 内部函数声明
 *
 * 本头文件声明 VM 内部使用的函数，这些函数不对外暴露，
 * 仅供 vm.c 和 vm_generator.c 等内部模块使用。
 */

/* 虚拟机主循环：执行字节码函数 */
Value vm_run(BytecodeFunc* bf, StackFrame* frame, EvalCtx* ctx);

/* 调用 RuntimeFunc：执行运行时函数 */
Value vm_call_rf(RuntimeFunc* rf, Value* args, int argc, StackFrame* parent, EvalCtx* ctx);


/* ========== VM 全局状态变量 ========== */

/* 当前线程的全局帧（主线程 = vm_run_main 的 top；线程体启动时从 data 继承并写入本线程 TLS） */
extern _Thread_local StackFrame* s_global_frame;

/* 线程模式：最外层 vm_run 不自行 unregister，由 vm_thread_body 在 set_result 后统一注销。
 * 深度计数器确保嵌套 vm_run 正常 register/unregister，skip 标志只影响最外层。 */
extern _Thread_local int tls_vm_run_depth;
extern _Thread_local int tls_skip_vm_unregister;


/* ========== VM 内部函数声明 ========== */

void vm_thread_body(ThreadLaunch* t);
void paused_gen_add(GeneratorObject* gen);
void paused_gen_remove(GeneratorObject* gen);
int try_operator_overload(const char* op_name, Value l, Value r,
                                  Value* stack, int* sp, StackFrame* frame,
                                  EvalCtx* ctx, Value* result);

#endif /* LUMYR_VM_INTERNAL_H */
