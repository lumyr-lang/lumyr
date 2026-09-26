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
    ConstEntry* const_pool;     /* 统一常量池（大常量：int64/uint64/double/string） */
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

/* 生成器对象
 * 4 核心栈备份设计：yield 时把生成器自身压栈的数据 [base_sp, sp) 拷贝到 gen 备份；
 * resume 时把备份数据还原到全局栈 [base_sp, base_sp+gen_sp) 区域，sp 一起恢复。
 * 调用方栈 [0, base_sp) 不动，挂起期间生成器数据安全保留在 gen 中。 */
typedef struct GeneratorObject {
    BytecodeFunc* bf;
    StackFrame* frame;
    /* 4 核心栈备份数据（生成器挂起时保存，恢复时还原到全局栈） */
    Value*   val_backup;     /* VALUE 栈备份 */
    int64_t* i64_backup;     /* INT64 栈备份 */
    double*  dbl_backup;     /* DOUBLE 栈备份 */
    void**   ptr_backup;     /* PTR 栈备份 */
    int sp_val, sp_i64, sp_dbl, sp_ptr;  /* 各栈当前深度（生成器自身压栈数） */
    int cap_val, cap_i64, cap_dbl, cap_ptr;  /* 各备份容量 */
    int pc;                  /* 挂起时的 pc（下次从这里继续） */
    int finished;
    int started;
    Value yield_value;       /* 上次 yield 的值 */
    Value send_value;        /* send() 发送的值 */
} GeneratorObject;

/* ========== 一层函数执行结束时的返回槽 ==========
   直接承载 4 栈之一的原始值，避免 typed<->Value 装箱与类型信息丢失。
   et 为 ExprType：NONE=v / INT=i / DOUBLE=d / PTR=p。 */
typedef struct {
    int     et;
    Value   v;     /* EXPR_TYPE_NONE -> VALUE 栈 */
    int64_t i;     /* EXPR_TYPE_INT -> INT64 栈 */
    double  d;     /* EXPR_TYPE_DOUBLE -> DOUBLE 栈 */
    void*   p;     /* EXPR_TYPE_PTR -> PTR 栈 */
} RetSlot;

/* ========== 指令处理函数类型 ========== */
typedef void (*VMInstrHandler)(VMExecCtx* ctx, Instruction* in);

/* ========== 可重入执行循环（vm_exec.c）：运行 ctx->fn，返回值写 *ret ========== */
/* 返回码 */
#define VM_LOOP_NORMAL 0   /* 正常结束（RETURN/RETURN_NIL/自然末尾） */
#define VM_LOOP_UNWIND 1   /* 异常跨帧展开中，调用者须继续向外传播 */
int vm_exec_loop(VMExecCtx* ctx, RetSlot* ret);
/* 受防护执行循环（vm_exec.c）：接通 kit/runtime runtime_error 与 VM 协作式异常，
   返回码与 vm_exec_loop 相同（VM_LOOP_NORMAL / VM_LOOP_UNWIND） */
int vm_exec_guarded(VMExecCtx* ctx, RetSlot* ret);

/* 异常展开检测（vm_except.c）：
   0=无展开；1=当前帧是捕获目标（已设置 current_error 并重定位 pc）；-1=需向外传播 */
int vm_except_check_unwind(VMExecCtx* ctx);

/* FINISH 后若挂起返回已走完所有 finally：取出返回值（1=有，0=无） */
int vm_except_take_pending_return(RetSlot* out);

/* 内部指令抛出异常（vm_except.c）：以 throw 值走分派；未捕获则 exit(1) */
int vm_except_throw_value(VMExecCtx* ctx, Value v);
void vm_except_raise_str(VMExecCtx* ctx, const char* type, const char* msg);
/* 跨帧展开是否激活（vm_except.c）：runtime_error 长跳落地后判定走向 */
int vm_except_unwind_active(void);

/* 返回值独立化（字符串堆值深拷贝），定义在 vm_exec.c */
Value ret_value_detach(Value v);

#endif /* LUMYR_VM_TYPES_H */
