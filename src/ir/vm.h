#ifndef LUMYR_IR_VM_H
#define LUMYR_IR_VM_H

#include "bytecode.h"
#include "lumyr_value_type.h"
#include "lm_thread.h"

// 顶层入口：建顶层栈帧执行 main 字节码
Value vm_run_main(BytecodeFunc* main_fn);

// FuncEntry 签名入口（注册到 RuntimeFunc.entry，由 OP_CALL 调用）
Value vm_func_entry(int arg_cnt, const Value* args, EvalCtx* ctx, StackFrame* frame);

// VM 通道线程体：thread(f, args...) 的工作线程入口（供 lumyr_thread_start 回调）
void vm_thread_body(ThreadLaunch* t);

#endif // LUMYR_IR_VM_H
