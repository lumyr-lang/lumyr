#ifndef LUMYR_VM_EXEC_CTX_H
#define LUMYR_VM_EXEC_CTX_H

#include "bytecode.h"
#include "lumyr_value_type.h"
#include "ast/stackframe.h"

/*
 * VM 执行上下文
 *
 * 本结构体封装了 vm_run 函数中的局部变量，使得各个业务场景模块
 * 可以通过这个结构体来访问执行状态，而不需要传递大量的参数。
 */

typedef struct VmExecCtx {
    BytecodeFunc* bf;        /* 当前执行的字节码函数 */
    StackFrame* frame;       /* 当前栈帧 */
    EvalCtx* ctx;            /* 求值上下文 */
    Value* stack;            /* 操作数栈（Value 类型） */
    int* sp;                 /* 指向栈指针的指针 */
    int* pc;                 /* 指向程序计数器的指针 */
    Instruction* in;         /* 当前指令 */
    int max_stack;           /* 最大栈深度 */
    /* 生成器相关 */
    int is_generator;        /* 是否是生成器模式 */
    void* gen_ctx;           /* 生成器上下文（GeneratorObject*） */
} VmExecCtx;

/* 初始化执行上下文 */
static inline void vm_exec_ctx_init(VmExecCtx* exec_ctx,
                                     BytecodeFunc* bf,
                                     StackFrame* frame,
                                     EvalCtx* ctx,
                                     Value* stack,
                                     int* sp,
                                     int* pc,
                                     int max_stack) {
    exec_ctx->bf = bf;
    exec_ctx->frame = frame;
    exec_ctx->ctx = ctx;
    exec_ctx->stack = stack;
    exec_ctx->sp = sp;
    exec_ctx->pc = pc;
    exec_ctx->in = NULL;
    exec_ctx->max_stack = max_stack;
    exec_ctx->is_generator = 0;
    exec_ctx->gen_ctx = NULL;
}

#endif /* LUMYR_VM_EXEC_CTX_H */
