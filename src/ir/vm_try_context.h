#ifndef LUMYR_VM_TRY_CONTEXT_H
#define LUMYR_VM_TRY_CONTEXT_H

#include <setjmp.h>
#include "lumyr_value_type.h"

/*
 * VM try-catch 上下文管理
 *
 * 本模块提供 VM 中 try-catch-finally 上下文的管理，包括栈的扩容、
 * 状态变量的维护等。这些状态被 vm.c 和 vm_generator.c 共享。
 */

/* ========== try-catch 上下文状态变量（线程局部） ========== */

extern _Thread_local jmp_buf* vm_jbs;
extern _Thread_local jmp_buf** vm_prev;
extern _Thread_local int vm_depth;
extern _Thread_local int* vm_sp;
extern _Thread_local int* vm_target;   /* 每层的 catch 目标（longjmp 后自动变量不可靠） */
extern _Thread_local int* vm_tn;       /* 每层 TRY 时的调用栈深度（GET_ERR 截断残留） */
extern _Thread_local int* vm_fn;       /* 每层 TRY 时的 finally 完成栈深度 */
extern _Thread_local int* vm_fin_act;  /* finally 完成动作：1=JMP 2=RETHROW 3=BREAK 4=CONT 5=RETURN */
extern _Thread_local int* vm_fin_tgt;
extern _Thread_local int* vm_fin_dep;  /* FIN_PUSH 时的恢复深度（FINISH act=1/3/4 恢复，防循环内 depth 漂移） */
extern _Thread_local int vm_fin_n;
extern _Thread_local int vm_cap;          /* 错误处理器栈容量 */
extern _Thread_local Value vm_pend_val;   /* 挂起返回的值（PEND_RETURN 存，FINISH act5 恢复） */

/* g_err_jmp 已在 lumyr_value.h 中定义为 _Thread_local */

/* ========== 函数声明 ========== */

/* 确保 try-catch 栈容量足够，不足时扩容 */
void vm_ensure(int need);

#endif /* LUMYR_VM_TRY_CONTEXT_H */
