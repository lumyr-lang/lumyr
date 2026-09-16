/*
 * VM try-catch 上下文管理实现
 *
 * 本模块提供 VM 中 try-catch-finally 上下文的管理，包括栈的扩容、
 * 状态变量的维护等。这些状态被 vm.c 和 vm_generator.c 共享。
 */

#include "vm_try_context.h"
#include "lumyr_log.h"
#include <stdlib.h>
#include <string.h>

/* ========== try-catch 上下文状态变量（线程局部） ========== */

_Thread_local jmp_buf* vm_jbs = NULL;
_Thread_local jmp_buf** vm_prev = NULL;
_Thread_local int vm_depth = 0;
_Thread_local int* vm_sp = NULL;
_Thread_local int* vm_target = NULL;   /* 每层的 catch 目标（longjmp 后自动变量不可靠） */
_Thread_local int* vm_tn = NULL;       /* 每层 TRY 时的调用栈深度（GET_ERR 截断残留） */
_Thread_local int* vm_fn = NULL;       /* 每层 TRY 时的 finally 完成栈深度 */
_Thread_local int* vm_fin_act = NULL;  /* finally 完成动作：1=JMP 2=RETHROW 3=BREAK 4=CONT 5=RETURN */
_Thread_local int* vm_fin_tgt = NULL;
_Thread_local int* vm_fin_dep = NULL;  /* FIN_PUSH 时的恢复深度（FINISH act=1/3/4 恢复，防循环内 depth 漂移） */
_Thread_local int vm_fin_n = 0;
_Thread_local int vm_cap = 0;          /* 错误处理器栈容量 */
_Thread_local Value vm_pend_val;       /* 挂起返回的值（PEND_RETURN 存，FINISH act5 恢复） */

/* ========== 函数实现 ========== */

/* 确保 try-catch 栈容量足够，不足时扩容 */
void vm_ensure(int need)
{
    if(need <= vm_cap) return;
    int nc = vm_cap > 0 ? vm_cap * 2 : 64;
    jmp_buf* nj = (jmp_buf*)realloc(vm_jbs, (size_t)nc * sizeof(jmp_buf));
    if(!nj) { LOG_ERROR("vm: try 栈扩容内存不足\n"); exit(EXIT_FAILURE); }
    vm_jbs = nj;
    jmp_buf** np = (jmp_buf**)realloc(vm_prev, (size_t)nc * sizeof(jmp_buf*));
    if(!np) { LOG_ERROR("vm: try 栈扩容内存不足\n"); exit(EXIT_FAILURE); }
    vm_prev = np;
    int* nsp = (int*)realloc(vm_sp, (size_t)nc * sizeof(int));
    if(!nsp) { LOG_ERROR("vm: try 栈扩容内存不足\n"); exit(EXIT_FAILURE); }
    vm_sp = nsp;
    int* nt = (int*)realloc(vm_target, (size_t)nc * sizeof(int));
    if(!nt) { LOG_ERROR("vm: try 栈扩容内存不足\n"); exit(EXIT_FAILURE); }
    vm_target = nt;
    int* ntn = (int*)realloc(vm_tn, (size_t)nc * sizeof(int));
    if(!ntn) { LOG_ERROR("vm: try 栈扩容内存不足\n"); exit(EXIT_FAILURE); }
    vm_tn = ntn;
    int* nfn = (int*)realloc(vm_fn, (size_t)nc * sizeof(int));
    if(!nfn) { LOG_ERROR("vm: try 栈扩容内存不足\n"); exit(EXIT_FAILURE); }
    vm_fn = nfn;
    int* nfa = (int*)realloc(vm_fin_act, (size_t)nc * sizeof(int));
    if(!nfa) { LOG_ERROR("vm: try 栈扩容内存不足\n"); exit(EXIT_FAILURE); }
    vm_fin_act = nfa;
    int* nft = (int*)realloc(vm_fin_tgt, (size_t)nc * sizeof(int));
    if(!nft) { LOG_ERROR("vm: try 栈扩容内存不足\n"); exit(EXIT_FAILURE); }
    vm_fin_tgt = nft;
    int* nfd = (int*)realloc(vm_fin_dep, (size_t)nc * sizeof(int));
    if(!nfd) { LOG_ERROR("vm: try 栈扩容内存不足\n"); exit(EXIT_FAILURE); }
    vm_fin_dep = nfd;
    vm_cap = nc;
}
