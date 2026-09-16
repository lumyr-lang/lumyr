#ifndef LUMYR_VM_TYPES_H
#define LUMYR_VM_TYPES_H

#include "bytecode.h"
#include "ast/ast_node.h"
#include "ast/stackframe.h"
#include "lm_value.h"
#include <setjmp.h>

/* 前向声明 */
typedef struct EvalCtx EvalCtx;
typedef struct RuntimeFunc RuntimeFunc;

/*
 * 虚拟机类型定义
 *
 * 本文件包含虚拟机（VM）模块使用的结构体和枚举定义，
 * 包括生成器对象、线程参数等。
 */

/* ========== 生成器支持 ========== */

/* 包装生成器类型枚举 */
typedef enum {
    WRAP_NONE = 0,       /* 非包装生成器（默认值） */
    WRAP_MAP = 1,       /* map：转换每个元素 */
    WRAP_FILTER = 2,    /* filter：过滤元素 */
    WRAP_SKIP = 3,      /* skip：跳过前 n 个元素 */
    WRAP_TAKE = 4,      /* take：取前 n 个元素 */
    WRAP_ENUMERATE = 5, /* enumerate：枚举 [index, value] */
    WRAP_CHAIN = 6,     /* chain：连接两个生成器 */
    WRAP_ZIP = 7        /* zip：压缩两个生成器 */
} WrapType;

/* 生成器对象前向声明 */
typedef struct GeneratorObject GeneratorObject;

/* 生成器对象：保存冻结的执行状态 */
typedef struct GeneratorObject {
    BytecodeFunc* bf;        /* 函数字节码 */
    StackFrame* frame;       /* 栈帧（局部变量） */
    Value* stack;            /* 执行栈 */
    int sp;                  /* 栈指针 */
    int pc;                  /* 指令指针 */
    int max_stack;           /* 最大栈深度 */
    int finished;            /* 是否执行完毕 */
    int started;             /* 是否已开始执行 */
    jmp_buf resume_point;    /* 恢复点（longjmp 用） */
    Value yield_value;       /* yield 的值 */
    Value send_value;        /* send() 发送的值（作为 yield 表达式的返回值） */
    int has_send_value;      /* 是否有 send_value（第一次 next() 没有） */
    EvalCtx* ctx;            /* 求值上下文 */
    int saved_depth;         /* 保存的 try 深度 */
    jmp_buf* saved_gj;       /* 保存的错误跳转点 */
    int saved_fin;           /* 保存的 finally 深度 */
    Value* old_gc_stack;     /* 保存的 GC 栈 */
    int* old_gc_sp;          /* 保存的 GC sp */
    StackFrame* old_gc_frame; /* 保存的 GC frame */
    /* try-catch 上下文保存（yield 时保存，恢复时恢复） */
    int saved_vm_depth;      /* 保存的 try 深度 */
    jmp_buf* saved_vm_jbs;   /* 保存的 jmp_buf 数组 */
    jmp_buf** saved_vm_prev;  /* 保存的 jmp_buf* 数组 */
    int* saved_vm_sp;        /* 保存的 sp 数组 */
    int* saved_vm_target;    /* 保存的 target 数组 */
    int* saved_vm_tn;        /* 保存的 tn 数组 */
    int* saved_vm_fn;        /* 保存的 fn 数组 */
    int saved_vm_fin_n;      /* 保存的 finally 完成栈深度 */
    int* saved_vm_fin_act;   /* 保存的 fin_act 数组 */
    int* saved_vm_fin_tgt;   /* 保存的 fin_tgt 数组 */
    int* saved_vm_fin_dep;   /* 保存的 fin_dep 数组 */
    jmp_buf* saved_g_err_jmp; /* 保存的当前错误跳转点 */
    Value pending_exception;  /* 待抛出的异常（GenThrow() 设置，恢复时抛出） */
    int has_pending_exception; /* 是否有待抛出的异常 */
    /* 包装生成器：map/filter/skip/take/enumerate/chain/zip */
    int is_wrapped;           /* 是否是包装生成器 */
    int wrap_type;            /* 包装类型：1=map 2=filter 3=skip 4=take 5=enumerate 6=chain 7=zip */
    GeneratorObject* wrapped_gen; /* 被包装的原始生成器 */
    RuntimeFunc* wrap_fn;     /* 转换/过滤函数 */
    int wrap_arg;             /* 额外参数（skip/take 的 n） */
    GeneratorObject* wrapped_gen2; /* 第二个生成器（chain/zip） */
    int wrap_index;           /* enumerate 的索引 */
} GeneratorObject;

/* ========== 线程支持 ========== */

/*
 * 线程参数（VM 通道）：函数 + 全局帧。全局变量存 vm_run_main 的顶层帧，
 * 线程函数经 parent 链访问；data 由线程体消费后 free。
 */
typedef struct {
    RuntimeFunc* rf;
    StackFrame* global_frame;
} VmThreadArg;

#endif /* LUMYR_VM_TYPES_H */
