/*
 * vm.c - VM 解释器主入口
 * 主执行循环在 vm_exec.c，类型定义在 vm_type.h
 */
#include "vm_types.h"
#include "vm.h"
#include "stack_manager.h"
#include "ast/stackframe.h"
#include "gc_runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 外部函数（定义在 vm_exec.c / vm_exec_call.c）
 * 注：不 include vm_exec.h——其 vm_run 旧原型（3 参）与本文件定义（1 参）冲突 */
Value vm_execute(VMExecCtx* ctx);
int vm_call_func_value(VMExecCtx* ctx, Value fv, int argc, Value* args, Value* out);

/* ============================================================
 * VM 主入口：执行字节码函数
 * ============================================================ */
Value vm_run(BytecodeFunc* fn) {
    if(!fn) return val_none();

    /* 初始化栈管理器（程序启动时就初始化） */
    if (!g_stack_mgr) {
        int ret = stack_global_init(4096);
    }

    VMExecCtx ctx = {0};
    ctx.fn = fn;
    ctx.code = fn->code;
    ctx.const_pool = fn->const_pool;
    ctx.const_cnt = fn->const_cnt;
    ctx.syms = (const char**)fn->syms;
    ctx.sym_cnt = fn->sym_cnt;

    /* 创建全局栈帧 */
    ctx.frame = stackframe_new(NULL);

    /* 执行字节码 */
    Value result = vm_execute(&ctx);

    /* 清理栈帧 */
    stackframe_destroy(ctx.frame);

    return result;
}

/* VM 入口（main 函数） */
Value vm_run_main(BytecodeFunc* main_fn) {
    if(!main_fn) {
        fprintf(stderr, "VM: no main function\n");
        return val_none();
    }

    vm_run(main_fn);
    return val_none();
}

/* ============================================================
 * VM 通道线程体：thread(f, args...) 的工作线程入口
 * 每线程独立执行器状态：
 *   - g_stack_mgr（4 核心栈）为 _Thread_local，此处为本线程初始化
 *   - 异常状态（try 栈/展开/挂起返回）为 _Thread_local（vm_except.c），
 *     未捕获错误在 vm_except_throw_value 按本线程空 try 栈 exit(1)，与主线程一致
 *   - runtime_error 的 g_err_jmp 为 _Thread_local（本线程 NULL → 打印后退出进程）
 * 根帧独立（parent=NULL）：脚本顶层变量不跨线程可见，参数经克隆传值。
 * 结果写回：vm_call_func_value 返回后结果在 C 栈上，先 gc_protect_push 再用
 * set_result_protected 写回线程表（clone 期间防 GC 误回收），最后 pop。
 * ============================================================ */
void vm_thread_body(ThreadLaunch* t) {
    /* 本线程的 4 核心栈管理器（与 vm_run 首次初始化同深度） */
    stack_global_init(4096);

    /* 函数值由调用方堆交接（函数为引用语义，浅拷贝共享 RuntimeFunc） */
    Value fv = *(Value*)t->data;
    free(t->data);

    VMExecCtx ctx = {0};
    ctx.frame = stackframe_new(NULL);

    Value result = val_none();
    int rc = vm_call_func_value(&ctx, fv, t->argc, t->args, &result);
    if(rc != 1) {
        /* 硬错误（错误详情已打印）：与主执行循环 handled==0 语义一致，中止进程 */
        stackframe_destroy(ctx.frame);
        stack_global_destroy();
        fprintf(stderr, "VM: 线程函数执行失败 / thread function failed\n");
        exit(1);
    }

    gc_protect_push(result);
    lumyr_thread_set_result_protected(t, result);
    gc_protect_pop();

    stackframe_destroy(ctx.frame);
    stack_global_destroy();
}

/* FuncEntry 签名入口（注册到 RuntimeFunc.entry，由 OP_CALL 调用）
 * 弱定义桩：编译通道用 vm_run_main 直接执行 main 字节码，函数调用走解释器 payload；
 * 此桩保证链接符号存在，实际 VM 字节码函数调用分派待 Phase C 完善。
 * 原 lm_class.c 中的弱定义随统一架构删除，迁移至此处（VM 入口归 vm.c） */
__attribute__((weak)) Value vm_func_entry(int arg_cnt, const Value* args, EvalCtx* ctx, StackFrame* frame) {
    (void)arg_cnt;
    (void)args;
    (void)ctx;
    (void)frame;
    return val_none();
}
