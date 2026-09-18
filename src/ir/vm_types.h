/*
 * vm_types.h - VM 类型定义
 * 包含 VM 执行上下文、异常处理、生成器等结构体和枚举
 */
#ifndef LUMYR_VM_TYPES_H
#define LUMYR_VM_TYPES_H

#include "bytecode.h"
#include "stack_manager.h"
#include "ast/ast_node.h"
#include "ast/stackframe.h"
#include "lm_value.h"
#include <setjmp.h>

/* ========== VM 执行上下文（4 核心栈设计） ========== */
typedef struct {
    BytecodeFunc* fn;           /* 当前函数字节码 */
    Instruction* code;          /* 指令数组 */
    int pc;                     /* 指令指针 */
    StackFrame* frame;          /* 当前栈帧 */
    Value* consts;               /* 常量池 */
    const char** syms;          /* 符号表 */
    int const_cnt;
    int sym_cnt;
    VMStackManager stacks;       /* 4 核心栈管理器 */
} VMExecCtx;

/* ========== 生成器支持 ========== */
typedef enum {
    WRAP_NONE = 0,
    WRAP_MAP = 1,
    WRAP_FILTER = 2,
    WRAP_SKIP = 3,
    WRAP_TAKE = 4,
    WRAP_ENUMERATE = 5,
    WRAP_CHAIN = 6,
    WRAP_ZIP = 7
} WrapType;

/* 生成器对象 */
typedef struct GeneratorObject {
    BytecodeFunc* bf;
    StackFrame* frame;
    Value* stack;
    int sp;
    int pc;
    int max_stack;
    int finished;
    int started;
    jmp_buf resume_point;
    Value yield_value;
    Value send_value;
} GeneratorObject;

/* ========== 指令处理函数类型 ========== */
typedef void (*VMInstrHandler)(VMExecCtx* ctx, Instruction* in);

#endif /* LUMYR_VM_TYPES_H */
