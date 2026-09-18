/*
 * vm.c - VM 解释器主入口
 * 主执行循环在 vm_exec.c，类型定义在 vm_type.h
 */
#include "vm_types.h"
#include "vm.h"
#include "stack_manager.h"
#include "ast/stackframe.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 外部函数（定义在 vm_exec.c） */
Value vm_execute(VMExecCtx* ctx);

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
