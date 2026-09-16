// 字节码 VM 执行器
// 指令语义与 ast_interp.c 对齐（栈帧链变量、调用绑定、return 深拷贝、break/continue 编译期跳转）。
#include "vm.h"
#include "lumyr_log.h"
#include "ir_compile.h"
#include "lumyr_ffi.h"
#include "ast/stackframe.h"
#include "ast/func_compile.h"
#include "ast/ast_runtime_sym.h"
#include "lm_value.h"
#include "lm_runtime.h"
#include "gc_runtime.h"
#include "lm_thread.h"
#include "ast/ast_types.h"
#include "lm_class.h"
#include "stack_manager.h"

/* 线程模式：最外层 vm_run 不自行 unregister，由 vm_thread_body 在 set_result 后统一注销。
 * 深度计数器确保嵌套 vm_run 正常 register/unregister，skip 标志只影响最外层。 */
static _Thread_local int tls_vm_run_depth = 0;
static _Thread_local int tls_skip_vm_unregister = 0;
#include "lm_lock.h"
#include "lm_tls.h"
#include "lm_http.h"
#include "lm_json.h"
#include "lm_qs.h"
#include "lm_charset.h"
#include "lm_crypto.h"
#include "lm_regex.h"
#include "lm_time.h"
#include <stdio.h>
#include <stdlib.h>
#include <setjmp.h>

/* try/catch 错误处理器栈（VM 侧；C 生成侧用局部 jmp_buf）：动态扩容，无硬上限。
 * 注意 jmp_buf 经 realloc 移动时内容整体拷贝，setjmp 后再 longjmp(vm_jbs[d]) 语义不变。 */
static _Thread_local jmp_buf* vm_jbs = NULL;
static _Thread_local jmp_buf** vm_prev = NULL;
static _Thread_local int vm_depth = 0;
static _Thread_local int* vm_sp = NULL;
static _Thread_local int* vm_target = NULL;   /* 每层的 catch 目标（longjmp 后自动变量不可靠） */
static _Thread_local int* vm_tn = NULL;       /* 每层 TRY 时的调用栈深度（GET_ERR 截断残留） */
static _Thread_local int* vm_fn = NULL;       /* 每层 TRY 时的 finally 完成栈深度 */
static _Thread_local int* vm_fin_act = NULL;  /* finally 完成动作：1=JMP 2=RETHROW 3=BREAK 4=CONT 5=RETURN */
static _Thread_local int* vm_fin_tgt = NULL;
static _Thread_local int* vm_fin_dep = NULL;  /* FIN_PUSH 时的恢复深度（FINISH act=1/3/4 恢复，防循环内 depth 漂移） */
static _Thread_local int vm_fin_n = 0;
static _Thread_local int vm_cap = 0;          /* 错误处理器栈容量 */
static _Thread_local Value vm_pend_val;   /* 挂起返回的值（PEND_RETURN 存，FINISH act5 恢复） */

/* ========== 类型化专用栈（使用统一栈管理模块 stack_manager） ========== */
/* 原来的静态变量和栈操作函数已删除，统一使用 stack_manager 模块管理 */

#define INT_PUSH(val) do { stack_global_ensure(STACK_INT, 1); ((int*)stack_global_get_stack(STACK_INT))[(*stack_global_get_sp(STACK_INT))++] = (val); } while(0)
#define INT_POP() (((int*)stack_global_get_stack(STACK_INT))[--(*stack_global_get_sp(STACK_INT))])
#define INT_PEEK() (((int*)stack_global_get_stack(STACK_INT))[(*stack_global_get_sp(STACK_INT)) - 1])
#define INT_TOP(idx) (((int*)stack_global_get_stack(STACK_INT))[(*stack_global_get_sp(STACK_INT)) - 1 - (idx)])
#define DOUBLE_PUSH(val) do { stack_global_ensure(STACK_DOUBLE, 1); ((double*)stack_global_get_stack(STACK_DOUBLE))[(*stack_global_get_sp(STACK_DOUBLE))++] = (val); } while(0)
#define DOUBLE_POP() (((double*)stack_global_get_stack(STACK_DOUBLE))[--(*stack_global_get_sp(STACK_DOUBLE))])
#define DOUBLE_PEEK() (((double*)stack_global_get_stack(STACK_DOUBLE))[(*stack_global_get_sp(STACK_DOUBLE)) - 1])
#define DOUBLE_TOP(idx) (((double*)stack_global_get_stack(STACK_DOUBLE))[(*stack_global_get_sp(STACK_DOUBLE)) - 1 - (idx)])
#define FLOAT_PUSH(val) do { stack_global_ensure(STACK_FLOAT, 1); ((float*)stack_global_get_stack(STACK_FLOAT))[(*stack_global_get_sp(STACK_FLOAT))++] = (val); } while(0)
#define FLOAT_POP() (((float*)stack_global_get_stack(STACK_FLOAT))[--(*stack_global_get_sp(STACK_FLOAT))])
#define FLOAT_PEEK() (((float*)stack_global_get_stack(STACK_FLOAT))[(*stack_global_get_sp(STACK_FLOAT)) - 1])
#define FLOAT_TOP(idx) (((float*)stack_global_get_stack(STACK_FLOAT))[(*stack_global_get_sp(STACK_FLOAT)) - 1 - (idx)])
#define UINT_PUSH(val) do { stack_global_ensure(STACK_UINT, 1); ((unsigned int*)stack_global_get_stack(STACK_UINT))[(*stack_global_get_sp(STACK_UINT))++] = (val); } while(0)
#define UINT_POP() (((unsigned int*)stack_global_get_stack(STACK_UINT))[--(*stack_global_get_sp(STACK_UINT))])
#define UINT_PEEK() (((unsigned int*)stack_global_get_stack(STACK_UINT))[(*stack_global_get_sp(STACK_UINT)) - 1])
#define UINT_TOP(idx) (((unsigned int*)stack_global_get_stack(STACK_UINT))[(*stack_global_get_sp(STACK_UINT)) - 1 - (idx)])
#define BOOL_PUSH(val) do { stack_global_ensure(STACK_BOOL, 1); ((_Bool*)stack_global_get_stack(STACK_BOOL))[(*stack_global_get_sp(STACK_BOOL))++] = (val); } while(0)
#define BOOL_POP() (((_Bool*)stack_global_get_stack(STACK_BOOL))[--(*stack_global_get_sp(STACK_BOOL))])
#define CHAR_PUSH(val) do { stack_global_ensure(STACK_CHAR, 1); ((char*)stack_global_get_stack(STACK_CHAR))[(*stack_global_get_sp(STACK_CHAR))++] = (val); } while(0)
#define CHAR_POP() (((char*)stack_global_get_stack(STACK_CHAR))[--(*stack_global_get_sp(STACK_CHAR))])
#define BYTE_PUSH(val) do { stack_global_ensure(STACK_BYTE, 1); ((unsigned char*)stack_global_get_stack(STACK_BYTE))[(*stack_global_get_sp(STACK_BYTE))++] = (val); } while(0)
#define BYTE_POP() (((unsigned char*)stack_global_get_stack(STACK_BYTE))[--(*stack_global_get_sp(STACK_BYTE))])
#define INT8_PUSH(val) do { stack_global_ensure(STACK_INT8, 1); ((int8_t*)stack_global_get_stack(STACK_INT8))[(*stack_global_get_sp(STACK_INT8))++] = (val); } while(0)
#define INT8_POP() (((int8_t*)stack_global_get_stack(STACK_INT8))[--(*stack_global_get_sp(STACK_INT8))])
#define INT16_PUSH(val) do { stack_global_ensure(STACK_INT16, 1); ((int16_t*)stack_global_get_stack(STACK_INT16))[(*stack_global_get_sp(STACK_INT16))++] = (val); } while(0)
#define INT16_POP() (((int16_t*)stack_global_get_stack(STACK_INT16))[--(*stack_global_get_sp(STACK_INT16))])
#define INT32_PUSH(val) do { stack_global_ensure(STACK_INT32, 1); ((int32_t*)stack_global_get_stack(STACK_INT32))[(*stack_global_get_sp(STACK_INT32))++] = (val); } while(0)
#define INT32_POP() (((int32_t*)stack_global_get_stack(STACK_INT32))[--(*stack_global_get_sp(STACK_INT32))])
#define INT64_PUSH(val) do { stack_global_ensure(STACK_INT64, 1); ((int64_t*)stack_global_get_stack(STACK_INT64))[(*stack_global_get_sp(STACK_INT64))++] = (val); } while(0)
#define INT64_POP() (((int64_t*)stack_global_get_stack(STACK_INT64))[--(*stack_global_get_sp(STACK_INT64))])
#define UINT8_PUSH(val) do { stack_global_ensure(STACK_UINT8, 1); ((uint8_t*)stack_global_get_stack(STACK_UINT8))[(*stack_global_get_sp(STACK_UINT8))++] = (val); } while(0)
#define UINT8_POP() (((uint8_t*)stack_global_get_stack(STACK_UINT8))[--(*stack_global_get_sp(STACK_UINT8))])
#define UINT16_PUSH(val) do { stack_global_ensure(STACK_UINT16, 1); ((uint16_t*)stack_global_get_stack(STACK_UINT16))[(*stack_global_get_sp(STACK_UINT16))++] = (val); } while(0)
#define UINT16_POP() (((uint16_t*)stack_global_get_stack(STACK_UINT16))[--(*stack_global_get_sp(STACK_UINT16))])
#define UINT32_PUSH(val) do { stack_global_ensure(STACK_UINT32, 1); ((uint32_t*)stack_global_get_stack(STACK_UINT32))[(*stack_global_get_sp(STACK_UINT32))++] = (val); } while(0)
#define UINT32_POP() (((uint32_t*)stack_global_get_stack(STACK_UINT32))[--(*stack_global_get_sp(STACK_UINT32))])
#define UINT64_PUSH(val) do { stack_global_ensure(STACK_UINT64, 1); ((uint64_t*)stack_global_get_stack(STACK_UINT64))[(*stack_global_get_sp(STACK_UINT64))++] = (val); } while(0)
#define UINT64_POP() (((uint64_t*)stack_global_get_stack(STACK_UINT64))[--(*stack_global_get_sp(STACK_UINT64))])
#define LONG_PUSH(val) do { stack_global_ensure(STACK_LONG, 1); ((long*)stack_global_get_stack(STACK_LONG))[(*stack_global_get_sp(STACK_LONG))++] = (val); } while(0)
#define LONG_POP() (((long*)stack_global_get_stack(STACK_LONG))[--(*stack_global_get_sp(STACK_LONG))])
#define ULONG_PUSH(val) do { stack_global_ensure(STACK_ULONG, 1); ((unsigned long*)stack_global_get_stack(STACK_ULONG))[(*stack_global_get_sp(STACK_ULONG))++] = (val); } while(0)
#define ULONG_POP() (((unsigned long*)stack_global_get_stack(STACK_ULONG))[--(*stack_global_get_sp(STACK_ULONG))])
#define SIZE_T_PUSH(val) do { stack_global_ensure(STACK_SIZE_T, 1); ((size_t*)stack_global_get_stack(STACK_SIZE_T))[(*stack_global_get_sp(STACK_SIZE_T))++] = (val); } while(0)
#define SIZE_T_POP() (((size_t*)stack_global_get_stack(STACK_SIZE_T))[--(*stack_global_get_sp(STACK_SIZE_T))])
#define SSIZE_T_PUSH(val) do { stack_global_ensure(STACK_SSIZE_T, 1); ((ssize_t*)stack_global_get_stack(STACK_SSIZE_T))[(*stack_global_get_sp(STACK_SSIZE_T))++] = (val); } while(0)
#define SSIZE_T_POP() (((ssize_t*)stack_global_get_stack(STACK_SSIZE_T))[--(*stack_global_get_sp(STACK_SSIZE_T))])
#define LONG_DOUBLE_PUSH(val) do { stack_global_ensure(STACK_LONG_DOUBLE, 1); ((long double*)stack_global_get_stack(STACK_LONG_DOUBLE))[(*stack_global_get_sp(STACK_LONG_DOUBLE))++] = (val); } while(0)
#define LONG_DOUBLE_POP() (((long double*)stack_global_get_stack(STACK_LONG_DOUBLE))[--(*stack_global_get_sp(STACK_LONG_DOUBLE))])


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

/* 当前正在执行的生成器（NULL = 普通执行） */
static _Thread_local GeneratorObject* s_current_gen = NULL;
/* 生成器 yield 时的返回值传递 */
static _Thread_local Value s_gen_yield_result;
static _Thread_local int s_gen_yielded = 0;

/* ========== 暂停生成器 GC 根注册 ==========
 * 生成器 yield 暂停后，其 stack/frame 不再是 VM 当前 GC 根，
 * 但内部仍持有 GC 对象引用（字符串/数组/map）。若不注册为 GC 根，
 * GC 会错误回收这些对象，内存复用后覆盖生成器字段导致堆破坏。
 */
static GeneratorObject** s_paused_gens = NULL;
static int s_paused_gen_cnt = 0;
static int s_paused_gen_cap = 0;

static void paused_gen_add(GeneratorObject* gen) {
    if(s_paused_gen_cnt >= s_paused_gen_cap) {
        int nc = s_paused_gen_cap > 0 ? s_paused_gen_cap * 2 : 16;
        GeneratorObject** ns = (GeneratorObject**)realloc(s_paused_gens, (size_t)nc * sizeof(GeneratorObject*));
        if(!ns) { LOG_ERROR("vm: paused_gens 扩容内存不足\n"); exit(EXIT_FAILURE); }
        s_paused_gens = ns;
        s_paused_gen_cap = nc;
    }
    s_paused_gens[s_paused_gen_cnt++] = gen;
}

static void paused_gen_remove(GeneratorObject* gen) {
    for(int i = 0; i < s_paused_gen_cnt; i++) {
        if(s_paused_gens[i] == gen) {
            s_paused_gens[i] = s_paused_gens[--s_paused_gen_cnt];
            return;
        }
    }
}

/* 递归标记单个生成器持有的所有 GC 对象引用（包括被包装的子生成器） */
static void mark_generator_refs(GeneratorObject* gen, int depth) {
    if(!gen || depth > 16) return;  /* 防止循环引用导致无限递归 */
    /* 标记执行栈中的 Value */
    if(gen->stack && gen->sp > 0) {
        for(int j = 0; j < gen->sp; j++) {
            gc_mark(gen->stack[j]);
        }
    }
    /* 标记栈帧及父帧链中的局部变量 */
    StackFrame* f = gen->frame;
    while(f) {
        if(f->vals) {
            for(int j = 0; j < f->cnt; j++) {
                gc_mark(f->vals[j]);
            }
        }
        f = f->parent;
    }
    /* 标记包装生成器持有的函数对象 */
    if(gen->is_wrapped && gen->wrap_fn) {
        RuntimeFunc* rf = gen->wrap_fn;
        if(rf->captures) {
            for(int j = 0; j < rf->capture_count; j++) {
                gc_mark(rf->captures[j]);
            }
        }
    }
    /* 标记 send_value / yield_value / pending_exception */
    gc_mark(gen->yield_value);
    gc_mark(gen->send_value);
    gc_mark(gen->pending_exception);
    /* 递归标记被包装的子生成器（关键：子生成器可能不在暂停列表中，
     * 因为 generator_resume 返回时会从列表移除，但它仍持有 GC 对象引用） */
    if(gen->is_wrapped) {
        mark_generator_refs(gen->wrapped_gen, depth + 1);
        mark_generator_refs(gen->wrapped_gen2, depth + 1);
    }
}

/* GC 标记回调：遍历所有暂停生成器，标记其 stack 和 frame 中的 Value */
void lumyr_gc_mark_paused_generators(void) {
    for(int i = 0; i < s_paused_gen_cnt; i++) {
        GeneratorObject* gen = s_paused_gens[i];
        mark_generator_refs(gen, 0);
    }
}

/* GC 标记回调：标记单个生成器 Value 持有的所有引用。
 * gc_mark 遇到 VAL_GENERATOR 时调用此函数，确保生成器无论在栈上、数组中、
 * map 中还是暂停列表中，其持有的引用（wrapped_gen/wrap_fn/stack/frame 等）
 * 都能被正确标记，避免 GC 错误回收导致堆破坏。 */
void lumyr_gc_mark_generator(Value v) {
    if(v.type != VAL_GENERATOR || !v.v.generator) return;
    GeneratorObject* gen = (GeneratorObject*)v.v.generator;
    mark_generator_refs(gen, 0);
}

static void vm_ensure(int need)
{
    if(need <= vm_cap) return;
    int nc = vm_cap > 0 ? vm_cap * 2 : 64;
    jmp_buf* nj = (jmp_buf*)realloc(vm_jbs, (size_t)nc * sizeof(jmp_buf));
    if(!nj) { LOG_ERROR("vm: try 栈扩容内存不足\n"); exit(EXIT_FAILURE); }
    vm_jbs = nj;
    jmp_buf** np = (jmp_buf**)realloc(vm_prev, (size_t)nc * sizeof(jmp_buf*));
    if(!np) { LOG_ERROR("vm: try 栈扩容内存不足\n"); exit(EXIT_FAILURE); }
    vm_prev = np;
    int* na = (int*)realloc(vm_sp, (size_t)nc * sizeof(int));
    if(!na) { LOG_ERROR("vm: try 栈扩容内存不足\n"); exit(EXIT_FAILURE); }
    vm_sp = na;
    int* nt = (int*)realloc(vm_target, (size_t)nc * sizeof(int));
    if(!nt) { LOG_ERROR("vm: try 栈扩容内存不足\n"); exit(EXIT_FAILURE); }
    vm_target = nt;
    int* nn = (int*)realloc(vm_tn, (size_t)nc * sizeof(int));
    if(!nn) { LOG_ERROR("vm: try 栈扩容内存不足\n"); exit(EXIT_FAILURE); }
    vm_tn = nn;
    int* nf = (int*)realloc(vm_fn, (size_t)nc * sizeof(int));
    if(!nf) { LOG_ERROR("vm: try 栈扩容内存不足\n"); exit(EXIT_FAILURE); }
    vm_fn = nf;
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

/* ========== 生成器实现 ========== */

/* 创建生成器对象（不开始执行） */
static GeneratorObject* generator_new(BytecodeFunc* bf, StackFrame* parent_frame,
                                        int arg_cnt, const Value* args)
{
    GeneratorObject* gen = (GeneratorObject*)calloc(1, sizeof(GeneratorObject));
    if(!gen) { LOG_ERROR("generator_new: 内存不足\n"); exit(EXIT_FAILURE); }
    gen->bf = bf;
    /* 生成器使用独立栈帧，不持有父栈帧指针（避免父栈帧被释放后的 UAF）。
       生成器通过全局符号表访问全局变量和函数；局部变量在生成器自己的栈帧中。 */
    gen->frame = stackframe_new(NULL);
    (void)parent_frame;  /* 保留参数兼容性，实际不使用 */
    gen->max_stack = bc_analyze_stack(bf, NULL, 0);
    if(gen->max_stack < 0) gen->max_stack = 64;
    gen->stack = (Value*)malloc(sizeof(Value) * (gen->max_stack + 64));
    gen->sp = 0;
    gen->pc = 0;
    gen->finished = 0;
    gen->started = 0;
    gen->ctx = NULL;
    /* 绑定参数到栈帧 */
    for(int i = 0; i < bf->param_cnt && i < arg_cnt; i++) {
        if(bf->params[i]) stackframe_bind(gen->frame, bf->params[i], args[i]);
    }
    return gen;
}

/* 前向声明 */
static void generator_free_try_context(GeneratorObject* gen);

/* 销毁生成器对象 */
static void generator_free(GeneratorObject* gen)
{
    if(!gen) return;
    paused_gen_remove(gen);
    generator_free_try_context(gen);
    if(gen->stack) free(gen->stack);
    if(gen->frame) stackframe_destroy(gen->frame);
    free(gen);
}

/* 生成器执行函数：恢复状态，执行到下一个 yield 或 return
 * 返回 1 = 正常 yield，结果在 *result；返回 0 = 生成器结束 */
/* vm_run 前向声明（generator_resume 需要调用） */
static Value vm_run(BytecodeFunc* bf, StackFrame* frame, EvalCtx* ctx);

/* 生成器执行函数：恢复状态，调用 vm_run 执行到下一个 yield 或 return
 * 返回 1 = 正常 yield，结果在 *result；返回 0 = 生成器结束 */
/* 保存当前 try-catch 上下文到生成器对象（yield 时调用） */
static void generator_save_try_context(GeneratorObject* gen)
{
    int depth = vm_depth;
    int fin_n = vm_fin_n;
    gen->saved_vm_depth = depth;
    gen->saved_vm_fin_n = fin_n;
    gen->saved_g_err_jmp = g_err_jmp;

    /* 先释放旧数组，避免多次 yield 导致内存泄漏 */
    generator_free_try_context(gen);

    /* 分配内存保存数组 */
    if(depth > 0) {
        gen->saved_vm_jbs = (jmp_buf*)malloc((size_t)depth * sizeof(jmp_buf));
        gen->saved_vm_prev = (jmp_buf**)malloc((size_t)depth * sizeof(jmp_buf*));
        gen->saved_vm_sp = (int*)malloc((size_t)depth * sizeof(int));
        gen->saved_vm_target = (int*)malloc((size_t)depth * sizeof(int));
        gen->saved_vm_tn = (int*)malloc((size_t)depth * sizeof(int));
        gen->saved_vm_fn = (int*)malloc((size_t)depth * sizeof(int));
        memcpy(gen->saved_vm_jbs, vm_jbs, (size_t)depth * sizeof(jmp_buf));
        memcpy(gen->saved_vm_prev, vm_prev, (size_t)depth * sizeof(jmp_buf*));
        memcpy(gen->saved_vm_sp, vm_sp, (size_t)depth * sizeof(int));
        memcpy(gen->saved_vm_target, vm_target, (size_t)depth * sizeof(int));
        memcpy(gen->saved_vm_tn, vm_tn, (size_t)depth * sizeof(int));
        memcpy(gen->saved_vm_fn, vm_fn, (size_t)depth * sizeof(int));
    } else {
        gen->saved_vm_jbs = NULL;
        gen->saved_vm_prev = NULL;
        gen->saved_vm_sp = NULL;
        gen->saved_vm_target = NULL;
        gen->saved_vm_tn = NULL;
        gen->saved_vm_fn = NULL;
    }

    if(fin_n > 0) {
        gen->saved_vm_fin_act = (int*)malloc((size_t)fin_n * sizeof(int));
        gen->saved_vm_fin_tgt = (int*)malloc((size_t)fin_n * sizeof(int));
        gen->saved_vm_fin_dep = (int*)malloc((size_t)fin_n * sizeof(int));
        memcpy(gen->saved_vm_fin_act, vm_fin_act, (size_t)fin_n * sizeof(int));
        memcpy(gen->saved_vm_fin_tgt, vm_fin_tgt, (size_t)fin_n * sizeof(int));
        memcpy(gen->saved_vm_fin_dep, vm_fin_dep, (size_t)fin_n * sizeof(int));
    } else {
        gen->saved_vm_fin_act = NULL;
        gen->saved_vm_fin_tgt = NULL;
        gen->saved_vm_fin_dep = NULL;
    }
}

/* 从生成器对象恢复 try-catch 上下文（恢复执行时调用） */
static void generator_restore_try_context(GeneratorObject* gen)
{
    int depth = gen->saved_vm_depth;
    int fin_n = gen->saved_vm_fin_n;

    /* 确保数组容量足够 */
    vm_ensure(depth > fin_n ? depth : fin_n);

    vm_depth = depth;
    vm_fin_n = fin_n;
    /* g_err_jmp/vm_jbs/vm_prev 不恢复：它们指向 yield 时的旧 C 栈帧，
     * 生成器恢复时旧栈帧已销毁，longjmp 到旧缓冲区是未定义行为（0xC0000005）。
     * 这些指针/缓冲区在 vm_run 中通过重新执行 setjmp 在当前栈帧重建。
     * 这里只恢复不依赖 C 栈帧的逻辑状态（vm_sp/vm_target/vm_tn/vm_fn）。 */

    if(depth > 0) {
        memcpy(vm_sp, gen->saved_vm_sp, (size_t)depth * sizeof(int));
        memcpy(vm_target, gen->saved_vm_target, (size_t)depth * sizeof(int));
        memcpy(vm_tn, gen->saved_vm_tn, (size_t)depth * sizeof(int));
        memcpy(vm_fn, gen->saved_vm_fn, (size_t)depth * sizeof(int));
    }

    if(fin_n > 0) {
        memcpy(vm_fin_act, gen->saved_vm_fin_act, (size_t)fin_n * sizeof(int));
        memcpy(vm_fin_tgt, gen->saved_vm_fin_tgt, (size_t)fin_n * sizeof(int));
        memcpy(vm_fin_dep, gen->saved_vm_fin_dep, (size_t)fin_n * sizeof(int));
    }
}

/* 释放生成器保存的 try-catch 上下文 */
static void generator_free_try_context(GeneratorObject* gen)
{
    free(gen->saved_vm_jbs);
    free(gen->saved_vm_prev);
    free(gen->saved_vm_sp);
    free(gen->saved_vm_target);
    free(gen->saved_vm_tn);
    free(gen->saved_vm_fn);
    free(gen->saved_vm_fin_act);
    free(gen->saved_vm_fin_tgt);
    free(gen->saved_vm_fin_dep);
    gen->saved_vm_jbs = NULL;
    gen->saved_vm_prev = NULL;
    gen->saved_vm_sp = NULL;
    gen->saved_vm_target = NULL;
    gen->saved_vm_tn = NULL;
    gen->saved_vm_fn = NULL;
    gen->saved_vm_fin_act = NULL;
    gen->saved_vm_fin_tgt = NULL;
    gen->saved_vm_fin_dep = NULL;
}

/* 前向声明 */
static int generator_resume(GeneratorObject* gen, Value* result, Value* send_val, StackFrame* frame, EvalCtx* ctx);
static Value vm_call_rf(RuntimeFunc* rf, Value* args, int argc, StackFrame* parent, EvalCtx* ctx);

/* 包装生成器的 next() 处理 */
static int wrapped_gen_next(GeneratorObject* gen, Value* result, StackFrame* frame, EvalCtx* ctx)
{
    if(gen->finished) { *result = val_none(); return 0; }

    switch(gen->wrap_type) {
        case WRAP_MAP: {
            Value v;
            int has = generator_resume(gen->wrapped_gen, &v, NULL, frame, ctx);
            if(!has) { gen->finished = 1; *result = val_none(); return 0; }
            Value a1[1] = { v };
            Value r = vm_call_rf(gen->wrap_fn, a1, 1, frame, ctx);
            *result = r;
            return 1;
        }
        case WRAP_FILTER: {
            while(1) {
                Value v;
                int has = generator_resume(gen->wrapped_gen, &v, NULL, frame, ctx);
                if(!has) { gen->finished = 1; *result = val_none(); return 0; }
                Value a1[1] = { v };
                Value r = vm_call_rf(gen->wrap_fn, a1, 1, frame, ctx);
                if(lumyr_to_bool(r)) { *result = v; return 1; }
            }
        }
        case WRAP_SKIP: {
            while(gen->wrap_index < gen->wrap_arg) {
                Value v;
                int has = generator_resume(gen->wrapped_gen, &v, NULL, frame, ctx);
                if(!has) { gen->finished = 1; *result = val_none(); return 0; }
                gen->wrap_index++;
            }
            Value v;
            int has = generator_resume(gen->wrapped_gen, &v, NULL, frame, ctx);
            if(!has) { gen->finished = 1; *result = val_none(); return 0; }
            *result = v;
            return 1;
        }
        case WRAP_TAKE: {
            if(gen->wrap_index >= gen->wrap_arg) { gen->finished = 1; *result = val_none(); return 0; }
            Value v;
            int has = generator_resume(gen->wrapped_gen, &v, NULL, frame, ctx);
            if(!has) { gen->finished = 1; *result = val_none(); return 0; }
            gen->wrap_index++;
            *result = v;
            return 1;
        }
        case WRAP_ENUMERATE: {
            Value v;
            int has = generator_resume(gen->wrapped_gen, &v, NULL, frame, ctx);
            if(!has) { gen->finished = 1; *result = val_none(); return 0; }
            Value arr = val_array(2);
            arr.v.array->items[0] = val_int(gen->wrap_index);
            arr.v.array->items[1] = v;
            gen->wrap_index++;
            *result = arr;
            return 1;
        }
        case WRAP_CHAIN: {
            Value v;
            int has = generator_resume(gen->wrapped_gen, &v, NULL, frame, ctx);
            if(has) { *result = v; return 1; }
            has = generator_resume(gen->wrapped_gen2, &v, NULL, frame, ctx);
            if(!has) { gen->finished = 1; *result = val_none(); return 0; }
            *result = v;
            return 1;
        }
        case WRAP_ZIP: {
            Value v1, v2;
            int has1 = generator_resume(gen->wrapped_gen, &v1, NULL, frame, ctx);
            int has2 = generator_resume(gen->wrapped_gen2, &v2, NULL, frame, ctx);
            if(!has1 || !has2) { gen->finished = 1; *result = val_none(); return 0; }
            Value arr = val_array(2);
            arr.v.array->items[0] = v1;
            arr.v.array->items[1] = v2;
            *result = arr;
            return 1;
        }
        default: {
            char buf[256];
            snprintf(buf, sizeof(buf), "未知包装生成器类型: wrap_type=%d is_wrapped=%d gen=%p",
                     gen->wrap_type, gen->is_wrapped, (void*)gen);
            runtime_error(buf);
        }
    }
    return 0;
}

static int generator_resume(GeneratorObject* gen, Value* result, Value* send_val, StackFrame* frame, EvalCtx* ctx)
{
    if(gen->finished) { *result = val_none(); return 0; }
    /* 包装生成器：直接调用包装逻辑，不执行字节码 */
    if(gen->is_wrapped) {
        /* 包装生成器恢复时从暂停 GC 根列表移除，返回值（暂停）时重新添加。
         * 包装生成器不是通过 OPC_YIELD 暂停，而是 wrapped_gen_next 返回值暂停，
         * 因此需要在这里单独管理 GC 根注册，否则内部持有的 GC 对象会被错误回收。 */
        paused_gen_remove(gen);
        int has = wrapped_gen_next(gen, result, frame, ctx);
        if(has) paused_gen_add(gen);
        return has;
    }

    /* 设置 send_value（如果有） */
    if(send_val) {
        gen->send_value = *send_val;
        gen->has_send_value = 1;
    } else {
        gen->has_send_value = 0;
    }

    /* setjmp 恢复点：vm_run 中遇到 OPC_YIELD 时 longjmp 到这里 */
    if(setjmp(gen->resume_point) == 0) {
        /* 第一次进入或从 next 恢复：调用 vm_run 执行字节码 */
        s_current_gen = gen;
        paused_gen_remove(gen);  /* 恢复执行前从暂停 GC 根列表移除（执行期间 stack/frame 是当前 VM 根） */
        EvalCtx ctx;
        memset(&ctx, 0, sizeof(ctx));
        gen->ctx = &ctx;
        Value ret = vm_run(gen->bf, gen->frame, &ctx);
        /* vm_run 正常返回：生成器结束，释放 try-catch 上下文 */
        s_current_gen = NULL;
        gen->finished = 1;
        generator_free_try_context(gen);
        paused_gen_remove(gen);  /* 生成器结束时从暂停 GC 根列表移除 */
        *result = ret;
        return 0;
    } else {
        /* 从 yield longjmp 回来：返回 yield 的值。
         * 注意：不调用 paused_gen_remove(gen)，生成器保持在暂停 GC 根列表中，
         * 因为它仍然持有 GC 对象引用（stack/frame 中的 Value），直到下次恢复或被释放。 */
        s_current_gen = NULL;
        *result = s_gen_yield_result;
        return 1;
    }
}


// 未定义变量/函数：统一报错退出（与 ast_interp.c 输出一致）
static void runtime_undefined(const char* what, const char* name)
{
    LOG_ERROR("Runtime Error: 未定义%s: %s\n", what, name);
    exit(EXIT_FAILURE);
}

static Value vm_run(BytecodeFunc* bf, StackFrame* frame, EvalCtx* ctx);


// 线程参数（VM 通道）：函数 + 全局帧。全局变量存 vm_run_main 的顶层帧，
// 线程函数经 parent 链访问；data 由线程体消费后 free。
typedef struct {
    RuntimeFunc* rf;
    StackFrame* global_frame;
} VmThreadArg;

/* 当前线程的全局帧（主线程 = vm_run_main 的 top；线程体启动时从 data 继承并写入本线程 TLS） */
static _Thread_local StackFrame* s_global_frame = NULL;

// 线程体（VM 通道）：线程内执行 RuntimeFunc，与 vm_call_rf 语义一致
static void vm_thread_body(ThreadLaunch* t)
{
    VmThreadArg* a = (VmThreadArg*)t->data;
    RuntimeFunc* rf = a->rf;
    StackFrame* saved_global = s_global_frame;
    s_global_frame = a->global_frame;
    StackFrame* callee = stackframe_new(a->global_frame);
    if(interp_func_is_payload(rf)) {
        int pcnt = interp_func_param_cnt(rf);
        int i = 0;
        for(; i < pcnt; i++) {
            const char* pname = interp_func_param_name(rf, i);
            Value bound = (i < t->argc) ? t->args[i] : val_none();
            stackframe_bind(callee, pname, bound);
        }
        if(interp_func_has_variadic(rf)) {
            const char* vname = interp_func_param_name(rf, pcnt);
            int rest = t->argc - i;
            if(rest < 0) rest = 0;
            Value arr = val_array(rest);
            for(int k = 0; k < rest; k++)
                arr.v.array->items[k] = t->args[i + k];
            stackframe_bind(callee, vname, arr);
        }
    }
    closure_bind_cells(rf, callee);
    RuntimeFunc* prev_rf = interp_set_current_rf(rf);
    EvalCtx ctx = {0};
    g_trace_push("<thread>");
    Value r = rf->entry(t->argc, t->args, &ctx, callee);
    /* r 已被 OPC_RETURN 中的 gc_protect_push 保护（VM entry 已 unregister）。
     * 直接 set_result，完成后 pop 释放该 protect entry。 */
    lumyr_thread_set_result(t, r);
    gc_protect_pop();
    if(g_trace_n > 0) g_trace_n--;
    interp_set_current_rf(prev_rf);
    stackframe_destroy(callee);
    s_global_frame = saved_global;
    free(a);
}

// 通过函数值调用（高阶函数内部使用）：与 OPC_CALL 的调用语义一致
static Value vm_call_rf(RuntimeFunc* rf, Value* args, int argc, StackFrame* parent, EvalCtx* ctx)
{
    StackFrame* callee = stackframe_new(parent);
    if(interp_func_is_payload(rf)) {
        int pcnt = interp_func_param_cnt(rf);
        int i = 0;
        for(; i < pcnt; i++) {
            const char* pname = interp_func_param_name(rf, i);
            Value bound = (i < argc) ? args[i] : val_none();
            stackframe_bind(callee, pname, bound);
        }
        if(interp_func_has_variadic(rf)) {
            const char* vname = interp_func_param_name(rf, pcnt);
            int rest = argc - i;
            if(rest < 0) rest = 0;
            Value arr = val_array(rest);
            for(int k = 0; k < rest; k++) {
                arr.v.array->items[k] = args[i + k];
            }
            stackframe_bind(callee, vname, arr);
        }
    }
    closure_bind_cells(rf, callee);
    RuntimeFunc* prev_rf = interp_set_current_rf(rf);
    int saved_break = ctx->hit_break;
    int saved_cont = ctx->hit_continue;
    ctx->hit_break = 0;
    ctx->hit_continue = 0;
    g_trace_push("<anonymous>");
    Value ret = rf->entry(argc, args, ctx, callee);
    if(g_trace_n > 0) g_trace_n--;
    ctx->hit_break = saved_break;
    ctx->hit_continue = saved_cont;
    interp_set_current_rf(prev_rf);
    stackframe_destroy(callee);
    return ret;
}

Value vm_run_main(BytecodeFunc* main_fn)
{
    EvalCtx local_ctx = {0};
    StackFrame* top = stackframe_new(NULL);
    stackframe_set_shared(top);              // 全局共享帧：多线程沿 parent 链访问需加锁
    stackframe_set(top, "log", val_map());   // 预定义 log 对象（方法链 log.xxx）
    StackFrame* saved_global = s_global_frame;
    s_global_frame = top;
    /* 使用统一栈管理模块初始化所有类型的专用栈（避免代码中到处都是自己管理栈） */
    stack_global_init(1024);  /* 初始容量1024，需要时自动扩容 */
    Value ret = vm_run(main_fn, top, &local_ctx);
    /* 使用统一栈管理模块销毁所有类型的专用栈 */
    stack_global_destroy();
    s_global_frame = saved_global;
    stackframe_destroy(top);
    return ret;
}

// 函数入口：帧已由调用点建好并绑定参数，这里直接执行函数体字节码
Value vm_func_entry(int arg_cnt, const Value* args, EvalCtx* ctx, StackFrame* frame)
{
    RuntimeFunc* self = interp_current_rf();
    if(!self || !interp_func_is_payload(self)) {
        runtime_error("vm_func_entry: 缺少当前函数上下文");
        return val_none();
    }
    InterpFuncPayload* pl = (InterpFuncPayload*)self->captures;
    if(!pl->bytecode) return val_none();
    /* 如果是生成器函数，创建生成器对象并返回（不立即执行） */
    if(pl->is_generator) {
        GeneratorObject* gen = generator_new(pl->bytecode, frame, arg_cnt, args);
        Value gen_val;
        gen_val.type = VAL_GENERATOR;
        gen_val.v.generator = gen;
        return gen_val;
    }
    return vm_run(pl->bytecode, frame, ctx);
}

/* 运算符重载辅助函数：尝试调用 op_name 对应的重载函数
 * 成功返回 1，结果存入 *result；失败返回 0，调用方执行默认运算 */
static int try_operator_overload(const char* op_name, Value l, Value r,
                                  Value* stack, int* sp, StackFrame* frame,
                                  EvalCtx* ctx, Value* result)
{
    if(!sym_has(op_name)) return 0;
    Value fv = sym_get(op_name);
    if(fv.type != VAL_FUNC) return 0;
    RuntimeFunc* rf = fv.v.func.func_obj;
    StackFrame* callee = stackframe_new(frame);
    if(interp_func_is_payload(rf)) {
        int pcnt = interp_func_param_cnt(rf);
        Value args[2] = {l, r};
        for(int i = 0; i < pcnt; i++) {
            const char* pname = interp_func_param_name(rf, i);
            Value bound = (i < 2) ? args[i] : val_none();
            stackframe_bind(callee, pname, bound);
        }
    }
    /* 把参数压到栈上 */
    stack[(*sp)++] = l;
    stack[(*sp)++] = r;
    Value* eval_args = &stack[*sp - 2];
    RuntimeFunc* prev_rf = interp_set_current_rf(rf);
    g_trace_push(op_name);
    Value ret = rf->entry(2, eval_args, ctx, callee);
    if(g_trace_n > 0) g_trace_n--;
    interp_set_current_rf(prev_rf);
    *sp -= 2; /* 弹出参数 */
    stackframe_destroy(callee);
    *result = ret;
    return 1;
}


/*
 * 执行内置函数调用
 * 返回新的栈指针 sp
 */
static int vm_exec_builtin(Instruction in, Value* stack, int sp, StackFrame* frame, EvalCtx* ctx)
{
                int argc = in.b;
                switch(in.a) {
                    case BUILTIN_LEN: {
                        Value v = stack[--sp];
                        stack[sp++] = lumyr_len(v);
                        break;
                    }
                    case BUILTIN_TYPE: {
                        Value v = stack[--sp];
                        stack[sp++] = lumyr_type(v);
                        break;
                    }
                    case BUILTIN_INPUT: {
                        stack[sp++] = lumyr_input();
                        break;
                    }
                    case BUILTIN_RANGE: {
                        int n = in.b;
                        Value r = lumyr_range_n(&stack[sp - n], n);
                        sp = sp - n + 1;
                        sp--; stack[sp++] = r;
                        break;
                    }
                    case BUILTIN_SUBSTR: {
                        Value n = stack[--sp];
                        Value st = stack[--sp];
                        Value s = stack[--sp];
                        stack[sp++] = lumyr_substr(s, st, n);
                        break;
                    }
                    case BUILTIN_TOUPPER: {
                        Value v = stack[--sp];
                        stack[sp++] = lumyr_toupper(v);
                        break;
                    }
                    case BUILTIN_TOLOWER: {
                        Value v = stack[--sp];
                        stack[sp++] = lumyr_tolower(v);
                        break;
                    }
                    case BUILTIN_SPLIT: {
                        Value sep = stack[--sp];
                        Value s = stack[--sp];
                        stack[sp++] = lumyr_split(s, sep);
                        break;
                    }
                    case BUILTIN_DEL: {
                        Value idx = stack[--sp];
                        lumyr_del(&stack[sp-1], idx);
                        break;
                    }
                    case BUILTIN_INSERT: {
                        Value val = stack[--sp];
                        Value idx = stack[--sp];
                        lumyr_insert(&stack[sp-1], idx, val);
                        break;
                    }
                    case BUILTIN_FLOOR: { Value v = stack[--sp]; stack[sp++] = lumyr_floor(v); break; }
                    case BUILTIN_CEIL:  { Value v = stack[--sp]; stack[sp++] = lumyr_ceil(v); break; }
                    case BUILTIN_ABS:   { Value v = stack[--sp]; stack[sp++] = lumyr_abs(v); break; }
                    case BUILTIN_SQRT:  { Value v = stack[--sp]; stack[sp++] = lumyr_sqrt(v); break; }
                    case BUILTIN_MAX:
                    case BUILTIN_MIN: {
                        int n = in.b;
                        Value r = (in.a == BUILTIN_MAX) ? lumyr_max(&stack[sp - n], n) : lumyr_min(&stack[sp - n], n);
                        sp = sp - n + 1;
                        sp--; stack[sp++] = r;
                        break;
                    }
                    case BUILTIN_JOIN: {
                        Value sep = stack[--sp];
                        Value arr = stack[--sp];
                        stack[sp++] = lumyr_join(arr, sep);
                        break;
                    }
                    case BUILTIN_CONTAINS: {
                        Value needle = stack[--sp];
                        Value hay = stack[--sp];
                        stack[sp++] = lumyr_contains(hay, needle);
                        break;
                    }
                    case BUILTIN_REPEAT: {
                        Value n = stack[--sp];
                        Value s = stack[--sp];
                        stack[sp++] = lumyr_repeat(s, n);
                        break;
                    }
                    case BUILTIN_REPLACE: {
                        Value to = stack[--sp];
                        Value from = stack[--sp];
                        Value s = stack[--sp];
                        stack[sp++] = lumyr_replace(s, from, to);
                        break;
                    }
                    case BUILTIN_SUM: { Value v = stack[--sp]; stack[sp++] = lumyr_sum(v); break; }
                    case BUILTIN_AVG: { Value v = stack[--sp]; stack[sp++] = lumyr_avg(v); break; }
                    case BUILTIN_FORMAT: {
                        int n = in.b;
                        Value r = lumyr_format(&stack[sp - n], n);
                        sp = sp - n + 1;
                        sp--; stack[sp++] = r;
                        break;
                    }
                    case BUILTIN_SORT:    { Value v = stack[--sp]; stack[sp++] = lumyr_sort(v); break; }
                    case BUILTIN_REVERSE:{ Value v = stack[--sp]; stack[sp++] = lumyr_reverse(v); break; }
                    case BUILTIN_STRIP:   { Value v = stack[--sp]; stack[sp++] = lumyr_strip(v); break; }
                    case BUILTIN_STARTSWITH: { Value r = stack[--sp], l = stack[--sp]; stack[sp++] = lumyr_startswith(l, r); break; }
                    case BUILTIN_ENDSWITH:   { Value r = stack[--sp], l = stack[--sp]; stack[sp++] = lumyr_endswith(l, r); break; }
                    case BUILTIN_READ_FILE:  { int n2 = in.b; Value r = lumyr_read_file(&stack[sp - n2], n2); sp = sp - n2 + 1; sp--; stack[sp++] = r; break; }
                    case BUILTIN_WRITE_FILE: { int n2 = in.b; Value r = lumyr_write_file(&stack[sp - n2], n2); sp = sp - n2 + 1; sp--; stack[sp++] = r; break; }
                    case BUILTIN_FILE_EXISTS:{ int n2 = in.b; Value r = lumyr_file_exists(&stack[sp - n2], n2); sp = sp - n2 + 1; sp--; stack[sp++] = r; break; }
                    case BUILTIN_KEYS:     { int n2 = in.b; Value r = lumyr_map_keys(stack[sp - n2]); sp = sp - n2 + 1; sp--; stack[sp++] = r; break; }
                    case BUILTIN_VALUES:   { int n2 = in.b; Value r = lumyr_map_values(stack[sp - n2]); sp = sp - n2 + 1; sp--; stack[sp++] = r; break; }
                    case BUILTIN_THREAD: {
                        int argc = in.b;
                        if(argc < 1) runtime_error("thread() 至少需要一个函数参数");
                        Value fn = stack[sp - argc];
                        if(fn.type != VAL_FUNC) runtime_error("thread() 第一个参数必须是函数");
                        RuntimeFunc* rf = fn.v.func.func_obj;
                        int nargs = argc - 1;
                        VmThreadArg* a = (VmThreadArg*)malloc(sizeof(VmThreadArg));
                        if(!a) runtime_error("thread: 内存不足");
                        a->rf = rf;
                        a->global_frame = s_global_frame;
                        int tid = lumyr_thread_start(vm_thread_body, (void*)a, (nargs > 0) ? &stack[sp - nargs] : NULL, nargs);
                        sp = sp - argc + 1;
                        sp--; stack[sp++] = lumyr_make_int(tid);
                        break;
                    }
                    case BUILTIN_THREAD_JOIN: {
                        Value idv = stack[--sp];
                        if(idv.type != VAL_INT) runtime_error("thread_join() 参数必须是线程id（整数）");
                        stack[sp++] = lumyr_thread_join((int)idv.v.i);
                        break;
                    }
                    case BUILTIN_MUTEX:    { stack[sp++] = lumyr_make_int(lumyr_mutex_create()); break; }
                    case BUILTIN_RMUTEX:   { stack[sp++] = lumyr_make_int(lumyr_rmutex_create()); break; }
                    case BUILTIN_RWLOCK:   { stack[sp++] = lumyr_make_int(lumyr_rwlock_create()); break; }
                    case BUILTIN_SPINLOCK: { stack[sp++] = lumyr_make_int(lumyr_spinlock_create()); break; }
                    case BUILTIN_LOCK:   { Value v = stack[--sp]; if(v.type != VAL_INT) runtime_error("lock() 参数必须是锁id（整数）"); lumyr_lock((int)v.v.i); stack[sp++] = v; break; }
                    case BUILTIN_UNLOCK: { Value v = stack[--sp]; if(v.type != VAL_INT) runtime_error("unlock() 参数必须是锁id（整数）"); lumyr_unlock((int)v.v.i); stack[sp++] = v; break; }
                    case BUILTIN_TRYLOCK: {
                        Value v = stack[--sp];
                        if(v.type != VAL_INT) runtime_error("trylock() 参数必须是锁id（整数）");
                        stack[sp++] = lumyr_make_bool(lumyr_trylock((int)v.v.i));
                        break;
                    }
                    case BUILTIN_RDLOCK: { Value v = stack[--sp]; if(v.type != VAL_INT) runtime_error("rdlock() 参数必须是锁id（整数）"); lumyr_rdlock((int)v.v.i); stack[sp++] = v; break; }
                    case BUILTIN_WRLOCK: { Value v = stack[--sp]; if(v.type != VAL_INT) runtime_error("wrlock() 参数必须是锁id（整数）"); lumyr_wrlock((int)v.v.i); stack[sp++] = v; break; }
                    case BUILTIN_TRYRDLOCK: {
                        Value v = stack[--sp];
                        if(v.type != VAL_INT) runtime_error("tryrdlock() 参数必须是锁id（整数）");
                        stack[sp++] = lumyr_make_bool(lumyr_tryrdlock((int)v.v.i));
                        break;
                    }
                    case BUILTIN_TRYWRLOCK: {
                        Value v = stack[--sp];
                        if(v.type != VAL_INT) runtime_error("trywrlock() 参数必须是锁id（整数）");
                        stack[sp++] = lumyr_make_bool(lumyr_trywrlock((int)v.v.i));
                        break;
                    }
                    case BUILTIN_CONDVAR: { stack[sp++] = lumyr_make_int(lumyr_condvar_create()); break; }
                    case BUILTIN_COND_WAIT: {
                        Value lk = stack[--sp];
                        Value cd = stack[--sp];
                        if(lk.type != VAL_INT) runtime_error("cond_wait() 锁参数必须是锁id（整数）");
                        if(cd.type != VAL_INT) runtime_error("cond_wait() 条件参数必须是条件id（整数）");
                        lumyr_cond_wait((int)cd.v.i, (int)lk.v.i);
                        stack[sp++] = lk;    // 压回原值（表达式值）
                        break;
                    }
                    case BUILTIN_COND_SIGNAL: { Value v = stack[--sp]; if(v.type != VAL_INT) runtime_error("cond_signal() 参数必须是条件id（整数）"); lumyr_cond_signal((int)v.v.i); stack[sp++] = v; break; }
                    case BUILTIN_COND_BROADCAST: { Value v = stack[--sp]; if(v.type != VAL_INT) runtime_error("cond_broadcast() 参数必须是条件id（整数）"); lumyr_cond_broadcast((int)v.v.i); stack[sp++] = v; break; }
                    case BUILTIN_COND_TIMEDWAIT: {
                        Value ms = stack[--sp];
                        Value lk = stack[--sp];
                        Value cd = stack[--sp];
                        if(ms.type != VAL_INT) runtime_error("cond_wait_timeout() 超时参数必须是整数毫秒");
                        if(lk.type != VAL_INT) runtime_error("cond_wait_timeout() 锁参数必须是锁id（整数）");
                        if(cd.type != VAL_INT) runtime_error("cond_wait_timeout() 条件参数必须是条件id（整数）");
                        stack[sp++] = lumyr_make_bool(lumyr_cond_timedwait((int)cd.v.i, (int)lk.v.i, ms.v.i));
                        break;
                    }
                    case BUILTIN_THREADLOCAL_GET: {
                        Value nm = stack[--sp];
                        if(nm.type != VAL_STRING) runtime_error("threadlocal_get() 名字参数必须是字符串");
                        stack[sp++] = lumyr_tls_get(lumyr_str_cstr(&nm));
                        break;
                    }
                    case BUILTIN_THREADLOCAL_SET: {
                        Value v = stack[--sp];
                        Value nm = stack[--sp];
                        if(nm.type != VAL_STRING) runtime_error("threadlocal_set() 名字参数必须是字符串");
                        lumyr_tls_set(lumyr_str_cstr(&nm), v);
                        stack[sp++] = v;    // 压回原值（表达式值）
                        break;
                    }
                    case BUILTIN_HTTP_GET:
                    case BUILTIN_HTTP_POST:
                    case BUILTIN_HTTP_PUT:
                    case BUILTIN_ARRAY_ADD: {
                        if(in.b == 3) {
                            Value v = stack[--sp];
                            Value k = stack[--sp];
                            Value m = stack[--sp];
                            stack[sp++] = lumyr_map_add(m, k, v);
                        } else {
                            Value v = stack[--sp];
                            lumyr_array_add(&stack[sp-1], v);
                        }
                        break;
                    }
                    case BUILTIN_ARRAY_REMOVE: {
                        Value idx = stack[--sp];
                        lumyr_del(&stack[sp-1], idx);
                        break;
                    }
                    case BUILTIN_ARRAY_INDEXOF: {
                        Value x = stack[--sp];
                        Value arr = stack[--sp];
                        stack[sp++] = lumyr_index_of(arr, x);
                        break;
                    }
                    case BUILTIN_ARRAY_GET: {
                        Value i = stack[--sp];
                        Value arr = stack[--sp];
                        stack[sp++] = lumyr_array_get_safe(arr, i);
                        break;
                    }
                    case BUILTIN_ARRAY_SET: {
                        Value v = stack[--sp];
                        Value i = stack[--sp];
                        Value arr = stack[--sp];
                        stack[sp++] = lumyr_array_set_method(arr, i, v);
                        break;
                    }
                    case BUILTIN_ARRAY_FIRST: {
                        Value arr = stack[--sp];
                        stack[sp++] = lumyr_array_first(arr);
                        break;
                    }
                    case BUILTIN_ARRAY_LAST: {
                        Value arr = stack[--sp];
                        stack[sp++] = lumyr_array_last(arr);
                        break;
                    }
                    case BUILTIN_ARRAY_CLEAR: {
                        lumyr_array_clear(&stack[sp-1]);
                        break;
                    }
                    case BUILTIN_MAP_HAS: {
                        Value k = stack[--sp];
                        Value m = stack[--sp];
                        stack[sp++] = lumyr_make_bool(lumyr_map_has(m, k));
                        break;
                    }
                    case BUILTIN_JSON: {
                        Value enc = val_none();
                        Value v;
                        if(in.b >= 2) { enc = stack[--sp]; v = stack[--sp]; }
                        else { v = stack[--sp]; }
                        stack[sp++] = lumyr_json_parse_enc(lumyr_str_cstr(&v), enc);
                        break;
                    }
                    case BUILTIN_STRINGIFY: {
                        Value enc = val_none();
                        Value v;
                        if(in.b >= 2) { enc = stack[--sp]; v = stack[--sp]; }
                        else { v = stack[--sp]; }
                        char* js = lumyr_json_stringify_enc(v, enc);
                        Value r = lumyr_make_string(js);
                        free(js);
                        stack[sp++] = r;
                        break;
                    }
                    case BUILTIN_ARRAY_FLAT: {
                        Value depth = lumyr_make_int(1);
                        Value v;
                        if(in.b >= 2) { depth = stack[--sp]; v = stack[--sp]; }
                        else { v = stack[--sp]; }
                        stack[sp++] = lumyr_array_flat(v, lumyr_extract_int(depth));
                        break;
                    }
                    case BUILTIN_QS: {
                        Value enc = val_none();
                        Value v;
                        if(in.b >= 2) { enc = stack[--sp]; v = stack[--sp]; }
                        else { v = stack[--sp]; }
                        if(v.type == VAL_MAP || v.type == VAL_ARRAY) {
                            char* q = lumyr_qs_stringify_enc(v, enc);
                            stack[sp++] = lumyr_make_string(q);
                            free(q);
                        } else if(v.type == VAL_STRING) {
                            stack[sp++] = lumyr_qs_parse_enc(lumyr_str_cstr(&v), enc);
                        } else {
                            runtime_error("qs() 参数必须是字典/数组（序列化）或字符串（解析）");
                        }
                        break;
                    }
                    case BUILTIN_ARRAY_ADDALL: {
                        Value b = stack[--sp];
                        lumyr_array_addall(&stack[sp-1], b);
                        break;
                    }
                    case BUILTIN_BYTES: {
                        Value enc = val_none();
                        Value v;
                        if(in.b >= 2) { enc = stack[--sp]; v = stack[--sp]; }
                        else { v = stack[--sp]; }
                        stack[sp++] = lumyr_to_bytes(v, enc);
                        break;
                    }
                    case BUILTIN_STR: {
                        Value enc = val_none();
                        Value v;
                        if(in.b >= 2) { enc = stack[--sp]; v = stack[--sp]; }
                        else { v = stack[--sp]; }
                        stack[sp++] = lumyr_from_bytes(v, enc);
                        break;
                    }
                    case BUILTIN_ENCODE: {
                        Value enc = val_none();
                        Value v;
                        if(in.b >= 2) { enc = stack[--sp]; v = stack[--sp]; }
                        else { v = stack[--sp]; }
                        stack[sp++] = lumyr_to_bytes(v, enc);
                        break;
                    }
                    case BUILTIN_DECODE: {
                        Value enc = val_none();
                        Value v;
                        if(in.b >= 2) { enc = stack[--sp]; v = stack[--sp]; }
                        else { v = stack[--sp]; }
                        stack[sp++] = lumyr_from_bytes(v, enc);
                        break;
                    }
                    case BUILTIN_ENCODE_URL: {
                        Value v = stack[--sp];
                        char* r = lumyr_url_encode(v.type == VAL_STRING ? (lumyr_str_cstr(&v) ? lumyr_str_cstr(&v) : "") : "");
                        stack[sp++] = lumyr_make_string(r);
                        free(r);
                        break;
                    }
                    case BUILTIN_DECODE_URL: {
                        Value v = stack[--sp];
                        char* r = lumyr_url_decode(v.type == VAL_STRING ? (lumyr_str_cstr(&v) ? lumyr_str_cstr(&v) : "") : "");
                        stack[sp++] = lumyr_make_string(r);
                        free(r);
                        break;
                    }
                    case BUILTIN_MD5: {
                        Value v = stack[--sp];
                        const char* inp = v.type == VAL_STRING ? (lumyr_str_cstr(&v) ? lumyr_str_cstr(&v) : "") : "";
                        char* r = lumyr_md5_hex(inp, (int)strlen(inp));
                        stack[sp++] = lumyr_make_string(r);
                        free(r);
                        break;
                    }
                    case BUILTIN_ENCODE_BASE64: {
                        Value v = stack[--sp];
                        const char* inp = v.type == VAL_STRING ? (lumyr_str_cstr(&v) ? lumyr_str_cstr(&v) : "") : "";
                        char* r = lumyr_base64_encode(inp, (int)strlen(inp));
                        stack[sp++] = lumyr_make_string(r);
                        free(r);
                        break;
                    }
                    case BUILTIN_DECODE_BASE64: {
                        Value v = stack[--sp];
                        int olen = 0;
                        char* r = lumyr_base64_decode(v.type == VAL_STRING ? (lumyr_str_cstr(&v) ? lumyr_str_cstr(&v) : "") : "", &olen);
                        stack[sp++] = lumyr_make_string(r);
                        free(r);
                        break;
                    }
                    case BUILTIN_REGEX_MATCH: {
                        Value pat = stack[--sp];
                        Value str = stack[--sp];
                        stack[sp++] = lumyr_make_bool(lumyr_regex_match(
                            str.type == VAL_STRING ? (lumyr_str_cstr(&str) ? lumyr_str_cstr(&str) : "") : "",
                            pat.type == VAL_STRING ? (lumyr_str_cstr(&pat) ? lumyr_str_cstr(&pat) : "") : ""));
                        break;
                    }
                    case BUILTIN_REGEX_SEARCH: {
                        Value pat = stack[--sp];
                        Value str = stack[--sp];
                        stack[sp++] = lumyr_regex_search(
                            str.type == VAL_STRING ? (lumyr_str_cstr(&str) ? lumyr_str_cstr(&str) : "") : "",
                            pat.type == VAL_STRING ? (lumyr_str_cstr(&pat) ? lumyr_str_cstr(&pat) : "") : "");
                        break;
                    }
                    case BUILTIN_REGEX_REPLACE: {
                        Value repl = stack[--sp];
                        Value pat = stack[--sp];
                        Value str = stack[--sp];
                        char* r = lumyr_regex_replace(
                            str.type == VAL_STRING ? (lumyr_str_cstr(&str) ? lumyr_str_cstr(&str) : "") : "",
                            pat.type == VAL_STRING ? (lumyr_str_cstr(&pat) ? lumyr_str_cstr(&pat) : "") : "",
                            repl.type == VAL_STRING ? (lumyr_str_cstr(&repl) ? lumyr_str_cstr(&repl) : "") : "");
                        stack[sp++] = lumyr_make_string(r);
                        free(r);
                        break;
                    }
                    case BUILTIN_NOW: {
                        stack[sp++] = lumyr_now();
                        break;
                    }
                    case BUILTIN_TIMESTAMP: {
                        stack[sp++] = lumyr_make_double(lumyr_timestamp());
                        break;
                    }
                    case BUILTIN_TIMESTAMP_MS: {
                        stack[sp++] = lumyr_make_int(lumyr_timestamp_ms());
                        break;
                    }
                    case BUILTIN_SLEEP: {
                        Value v = stack[--sp];
                        lumyr_sleep_ms((long long)lumyr_extract_int(v));
                        stack[sp++] = val_none();
                        break;
                    }
                    case BUILTIN_DATE: {
                        char* r = lumyr_date_str();
                        stack[sp++] = lumyr_make_string(r);
                        free(r);
                        break;
                    }
                    case BUILTIN_TIME: {
                        char* r = lumyr_time_str();
                        stack[sp++] = lumyr_make_string(r);
                        free(r);
                        break;
                    }
                    case BUILTIN_DATETIME: {
                        char* r = lumyr_datetime_str();
                        stack[sp++] = lumyr_make_string(r);
                        free(r);
                        break;
                    }
                    case BUILTIN_FORMAT_TIME: {
                        Value ts = val_none();
                        Value fmt;
                        if(in.b >= 2) { ts = stack[--sp]; fmt = stack[--sp]; }
                        else { fmt = stack[--sp]; }
                        double tsv = (ts.type == VAL_NONE) ? -1.0 : (ts.type == VAL_DOUBLE ? ts.v.d : (double)lumyr_extract_int(ts));
                        char* r = lumyr_format_time(fmt.type == VAL_STRING ? (lumyr_str_cstr(&fmt) ? lumyr_str_cstr(&fmt) : "") : "", tsv);
                        stack[sp++] = lumyr_make_string(r);
                        free(r);
                        break;
                    }
                    case BUILTIN_LOG_DEBUG:
                    case BUILTIN_LOG_INFO:
                    case BUILTIN_LOG_WARN:
                    case BUILTIN_LOG_ERROR:
                    case BUILTIN_LOG_FATAL: {
                        int lvl = in.a - BUILTIN_LOG_DEBUG;
                        Value msg;
                        if(in.b >= 2) { msg = stack[--sp]; --sp; }  // 方法链：先弹消息，再丢弃 receiver
                        else { msg = stack[--sp]; }
                        char* ms = value_to_str(msg);
                        lumyr_log(lvl, ms);
                        free(ms);
                        stack[sp++] = val_none();
                        break;
                    }
                    case BUILTIN_GC_COUNT: {
                        stack[sp++] = lumyr_make_int((long long)gc_count());
                        break;
                    }
                    case BUILTIN_GC_BYTES: {
                        stack[sp++] = lumyr_make_int((long long)gc_bytes());
                        break;
                    }
                    case BUILTIN_GC_COLLECT: {
                        gc_collect_now();
                        stack[sp++] = val_none();
                        break;
                    }
                    case BUILTIN_GC_STW_NS: {
                        stack[sp++] = lumyr_make_int((long long)gc_stw_time_ns());
                        break;
                    }
                    case BUILTIN_NEXT: {
                        /* next(gen)：恢复生成器执行，返回 yield 值；结束返回 null。
                         * 不弹出 gen_val，让它始终留在 VM 栈上作为 GC 根，
                         * 避免 generator_resume 执行期间触发 GC 时生成器引用被错误回收。 */
                        Value gen_val = stack[sp - 1];
                        if(gen_val.type != VAL_GENERATOR) {
                            LOG_ERROR("Runtime Error: next() 需要生成器对象，实际类型: %d\n", gen_val.type);
                            exit(EXIT_FAILURE);
                        }
                        GeneratorObject* gen = (GeneratorObject*)gen_val.v.generator;
                        Value result;
                        int yielded = generator_resume(gen, &result, NULL, frame, ctx);
                        stack[sp - 1] = result;  /* 用结果覆盖栈顶的生成器 Value */
                        (void)yielded;
                        break;
                    }
                    case BUILTIN_SEND: {
                        /* send(gen, val)：向生成器发送值，恢复执行，返回下一个 yield 值。
                         * gen_val 留在栈上作为 GC 根，send_val 复制到 C 栈。 */
                        Value send_val = stack[--sp];
                        Value gen_val = stack[sp - 1];  /* 不弹出，留在栈上 */
                        if(gen_val.type != VAL_GENERATOR) {
                            LOG_ERROR("Runtime Error: send() 需要生成器对象，实际类型: %d\n", gen_val.type);
                            exit(EXIT_FAILURE);
                        }
                        GeneratorObject* gen = (GeneratorObject*)gen_val.v.generator;
                        if(!gen->started) {
                            LOG_ERROR("Runtime Error: send() 不能用于刚创建的生成器，请先调用 next()\n");
                            exit(EXIT_FAILURE);
                        }
                        Value result;
                        int yielded = generator_resume(gen, &result, &send_val, frame, ctx);
                        stack[sp - 1] = result;  /* 用结果覆盖栈顶 */
                        (void)yielded;
                        break;
                    }
                    case BUILTIN_RECEIVE: {
                        /* receive()：在生成器中获取 send() 发送的值；非生成器上下文返回 null */
                        if(s_current_gen && s_current_gen->has_send_value) {
                            stack[sp++] = s_current_gen->send_value;
                        } else {
                            stack[sp++] = val_none();
                        }
                        break;
                    }
                    case BUILTIN_CLOSE: {
                        /* close(gen)：关闭生成器，标记为已结束 */
                        Value gen_val = stack[--sp];
                        if(gen_val.type != VAL_GENERATOR) {
                            LOG_ERROR("Runtime Error: close() 需要生成器对象，实际类型: %d\n", gen_val.type);
                            exit(EXIT_FAILURE);
                        }
                        GeneratorObject* gen = (GeneratorObject*)gen_val.v.generator;
                        gen->finished = 1;
                        stack[sp++] = val_none();
                        break;
                    }
                    case BUILTIN_GEN_THROW: {
                        /* GenThrow(gen, err)：向生成器抛出异常，在 yield 位置抛出 */
                        Value err_val = stack[--sp];
                        Value gen_val = stack[--sp];
                        if(gen_val.type != VAL_GENERATOR) {
                            LOG_ERROR("Runtime Error: GenThrow() 需要生成器对象，实际类型: %d\n", gen_val.type);
                            exit(EXIT_FAILURE);
                        }
                        GeneratorObject* gen = (GeneratorObject*)gen_val.v.generator;
                        if(gen->finished) {
                            LOG_ERROR("Runtime Error: GenThrow() 生成器已结束\n");
                            exit(EXIT_FAILURE);
                        }
                        /* 设置待抛出的异常，恢复执行时会在 yield 位置抛出 */
                        gen->pending_exception = err_val;
                        gen->has_pending_exception = 1;
                        /* 恢复生成器执行，异常会在恢复时抛出 */
                        Value result;
                        generator_resume(gen, &result, NULL, frame, ctx);
                        stack[sp++] = result;
                        break;
                    }
                    case BUILTIN_CHAIN: {
                        /* chain(g1, g2)：连接两个生成器 */
                        Value g2 = stack[--sp];
                        Value g1 = stack[--sp];
                        if(g1.type != VAL_GENERATOR || g2.type != VAL_GENERATOR) {
                            runtime_error("chain() 参数必须是生成器");
                        }
                        GeneratorObject* wg = (GeneratorObject*)calloc(1, sizeof(GeneratorObject));
                        wg->is_wrapped = 1;
                        wg->wrap_type = WRAP_CHAIN;
                        wg->wrapped_gen = (GeneratorObject*)g1.v.generator;
                        wg->wrapped_gen2 = (GeneratorObject*)g2.v.generator;
                                                paused_gen_add(wg);  /* 注册为 GC 根，从创建到结束始终保持 */
Value gv; gv.type = VAL_GENERATOR; gv.v.generator = (void*)wg;
                        stack[sp++] = gv;
                        break;
                    }
                    case BUILTIN_ZIP: {
                        /* zip(g1, g2)：压缩两个生成器 */
                        Value g2 = stack[--sp];
                        Value g1 = stack[--sp];
                        if(g1.type != VAL_GENERATOR || g2.type != VAL_GENERATOR) {
                            runtime_error("zip() 参数必须是生成器");
                        }
                        GeneratorObject* wg = (GeneratorObject*)calloc(1, sizeof(GeneratorObject));
                        wg->is_wrapped = 1;
                        wg->wrap_type = WRAP_ZIP;
                        wg->wrapped_gen = (GeneratorObject*)g1.v.generator;
                        wg->wrapped_gen2 = (GeneratorObject*)g2.v.generator;
                                                paused_gen_add(wg);  /* 注册为 GC 根，从创建到结束始终保持 */
Value gv; gv.type = VAL_GENERATOR; gv.v.generator = (void*)wg;
                        stack[sp++] = gv;
                        break;
                    }
                    case BUILTIN_SKIP: {
                        /* skip(g, n)：跳过前 n 个元素 */
                        Value nval = stack[--sp];
                        Value gval = stack[--sp];
                        if(gval.type != VAL_GENERATOR) runtime_error("skip() 第一个参数必须是生成器");
                        int n = lumyr_extract_int(nval);
                        GeneratorObject* wg = (GeneratorObject*)calloc(1, sizeof(GeneratorObject));
                        wg->is_wrapped = 1;
                        wg->wrap_type = WRAP_SKIP;
                        wg->wrapped_gen = (GeneratorObject*)gval.v.generator;
                        wg->wrap_arg = n;
                                                paused_gen_add(wg);  /* 注册为 GC 根，从创建到结束始终保持 */
Value gv; gv.type = VAL_GENERATOR; gv.v.generator = (void*)wg;
                        stack[sp++] = gv;
                        break;
                    }
                    case BUILTIN_TAKE: {
                        /* take(g, n)：取前 n 个元素 */
                        Value nval = stack[--sp];
                        Value gval = stack[--sp];
                        if(gval.type != VAL_GENERATOR) runtime_error("take() 第一个参数必须是生成器");
                        int n = lumyr_extract_int(nval);
                        GeneratorObject* wg = (GeneratorObject*)calloc(1, sizeof(GeneratorObject));
                        wg->is_wrapped = 1;
                        wg->wrap_type = WRAP_TAKE;
                        wg->wrapped_gen = (GeneratorObject*)gval.v.generator;
                        wg->wrap_arg = n;
                                                paused_gen_add(wg);  /* 注册为 GC 根，从创建到结束始终保持 */
Value gv; gv.type = VAL_GENERATOR; gv.v.generator = (void*)wg;
                        stack[sp++] = gv;
                        break;
                    }
                    case BUILTIN_ENUMERATE: {
                        /* enumerate(g)：枚举 [index, value] */
                        Value gval = stack[--sp];
                        if(gval.type != VAL_GENERATOR) runtime_error("enumerate() 参数必须是生成器");
                        GeneratorObject* wg = (GeneratorObject*)calloc(1, sizeof(GeneratorObject));
                        wg->is_wrapped = 1;
                        wg->wrap_type = WRAP_ENUMERATE;
                        wg->wrapped_gen = (GeneratorObject*)gval.v.generator;
                                                paused_gen_add(wg);  /* 注册为 GC 根，从创建到结束始终保持 */
Value gv; gv.type = VAL_GENERATOR; gv.v.generator = (void*)wg;
                        stack[sp++] = gv;
                        break;
                    }
                    case BUILTIN_HTTP_DELETE:
                    case BUILTIN_HTTP_HEAD:
                    case BUILTIN_HTTP_PATCH: {
                        int n = in.b;
                        if(n < 1 || n > 3) runtime_error("requests 请求需要 1~3 个参数：url、可选 params、可选 config");
                        const char* m = "GET";
                        switch(in.a) {
                            case BUILTIN_HTTP_POST:   m = "POST"; break;
                            case BUILTIN_HTTP_PUT:    m = "PUT"; break;
                            case BUILTIN_ARRAY_ADD: {
                        if(in.b == 3) {
                            Value v = stack[--sp];
                            Value k = stack[--sp];
                            Value m = stack[--sp];
                            stack[sp++] = lumyr_map_add(m, k, v);
                        } else {
                            Value v = stack[--sp];
                            lumyr_array_add(&stack[sp-1], v);
                        }
                        break;
                    }
                    case BUILTIN_ARRAY_REMOVE: {
                        Value idx = stack[--sp];
                        lumyr_del(&stack[sp-1], idx);
                        break;
                    }
                    case BUILTIN_ARRAY_INDEXOF: {
                        Value x = stack[--sp];
                        Value arr = stack[--sp];
                        stack[sp++] = lumyr_index_of(arr, x);
                        break;
                    }
                    case BUILTIN_ARRAY_GET: {
                        Value i = stack[--sp];
                        Value arr = stack[--sp];
                        stack[sp++] = lumyr_array_get_safe(arr, i);
                        break;
                    }
                    case BUILTIN_ARRAY_SET: {
                        Value v = stack[--sp];
                        Value i = stack[--sp];
                        Value arr = stack[--sp];
                        stack[sp++] = lumyr_array_set_method(arr, i, v);
                        break;
                    }
                    case BUILTIN_ARRAY_FIRST: {
                        Value arr = stack[--sp];
                        stack[sp++] = lumyr_array_first(arr);
                        break;
                    }
                    case BUILTIN_ARRAY_LAST: {
                        Value arr = stack[--sp];
                        stack[sp++] = lumyr_array_last(arr);
                        break;
                    }
                    case BUILTIN_ARRAY_CLEAR: {
                        lumyr_array_clear(&stack[sp-1]);
                        break;
                    }
                    case BUILTIN_MAP_HAS: {
                        Value k = stack[--sp];
                        Value m = stack[--sp];
                        stack[sp++] = lumyr_make_bool(lumyr_map_has(m, k));
                        break;
                    }
                    case BUILTIN_JSON: {
                        Value enc = val_none();
                        Value v;
                        if(in.b >= 2) { enc = stack[--sp]; v = stack[--sp]; }
                        else { v = stack[--sp]; }
                        stack[sp++] = lumyr_json_parse_enc(lumyr_str_cstr(&v), enc);
                        break;
                    }
                    case BUILTIN_STRINGIFY: {
                        Value enc = val_none();
                        Value v;
                        if(in.b >= 2) { enc = stack[--sp]; v = stack[--sp]; }
                        else { v = stack[--sp]; }
                        char* js = lumyr_json_stringify_enc(v, enc);
                        Value r = lumyr_make_string(js);
                        free(js);
                        stack[sp++] = r;
                        break;
                    }
                    case BUILTIN_ARRAY_FLAT: {
                        Value depth = lumyr_make_int(1);
                        Value v;
                        if(in.b >= 2) { depth = stack[--sp]; v = stack[--sp]; }
                        else { v = stack[--sp]; }
                        stack[sp++] = lumyr_array_flat(v, lumyr_extract_int(depth));
                        break;
                    }
                    case BUILTIN_QS: {
                        Value enc = val_none();
                        Value v;
                        if(in.b >= 2) { enc = stack[--sp]; v = stack[--sp]; }
                        else { v = stack[--sp]; }
                        if(v.type == VAL_MAP || v.type == VAL_ARRAY) {
                            char* q = lumyr_qs_stringify_enc(v, enc);
                            stack[sp++] = lumyr_make_string(q);
                            free(q);
                        } else if(v.type == VAL_STRING) {
                            stack[sp++] = lumyr_qs_parse_enc(lumyr_str_cstr(&v), enc);
                        } else {
                            runtime_error("qs() 参数必须是字典/数组（序列化）或字符串（解析）");
                        }
                        break;
                    }
                    case BUILTIN_ARRAY_ADDALL: {
                        Value b = stack[--sp];
                        lumyr_array_addall(&stack[sp-1], b);
                        break;
                    }
                    case BUILTIN_BYTES: {
                        Value enc = val_none();
                        Value v;
                        if(in.b >= 2) { enc = stack[--sp]; v = stack[--sp]; }
                        else { v = stack[--sp]; }
                        stack[sp++] = lumyr_to_bytes(v, enc);
                        break;
                    }
                    case BUILTIN_STR: {
                        Value enc = val_none();
                        Value v;
                        if(in.b >= 2) { enc = stack[--sp]; v = stack[--sp]; }
                        else { v = stack[--sp]; }
                        stack[sp++] = lumyr_from_bytes(v, enc);
                        break;
                    }
                    case BUILTIN_ENCODE: {
                        Value enc = val_none();
                        Value v;
                        if(in.b >= 2) { enc = stack[--sp]; v = stack[--sp]; }
                        else { v = stack[--sp]; }
                        stack[sp++] = lumyr_to_bytes(v, enc);
                        break;
                    }
                    case BUILTIN_DECODE: {
                        Value enc = val_none();
                        Value v;
                        if(in.b >= 2) { enc = stack[--sp]; v = stack[--sp]; }
                        else { v = stack[--sp]; }
                        stack[sp++] = lumyr_from_bytes(v, enc);
                        break;
                    }
                    case BUILTIN_ENCODE_URL: {
                        Value v = stack[--sp];
                        char* r = lumyr_url_encode(v.type == VAL_STRING ? (lumyr_str_cstr(&v) ? lumyr_str_cstr(&v) : "") : "");
                        stack[sp++] = lumyr_make_string(r);
                        free(r);
                        break;
                    }
                    case BUILTIN_DECODE_URL: {
                        Value v = stack[--sp];
                        char* r = lumyr_url_decode(v.type == VAL_STRING ? (lumyr_str_cstr(&v) ? lumyr_str_cstr(&v) : "") : "");
                        stack[sp++] = lumyr_make_string(r);
                        free(r);
                        break;
                    }
                    case BUILTIN_MD5: {
                        Value v = stack[--sp];
                        const char* inp = v.type == VAL_STRING ? (lumyr_str_cstr(&v) ? lumyr_str_cstr(&v) : "") : "";
                        char* r = lumyr_md5_hex(inp, (int)strlen(inp));
                        stack[sp++] = lumyr_make_string(r);
                        free(r);
                        break;
                    }
                    case BUILTIN_ENCODE_BASE64: {
                        Value v = stack[--sp];
                        const char* inp = v.type == VAL_STRING ? (lumyr_str_cstr(&v) ? lumyr_str_cstr(&v) : "") : "";
                        char* r = lumyr_base64_encode(inp, (int)strlen(inp));
                        stack[sp++] = lumyr_make_string(r);
                        free(r);
                        break;
                    }
                    case BUILTIN_DECODE_BASE64: {
                        Value v = stack[--sp];
                        int olen = 0;
                        char* r = lumyr_base64_decode(v.type == VAL_STRING ? (lumyr_str_cstr(&v) ? lumyr_str_cstr(&v) : "") : "", &olen);
                        stack[sp++] = lumyr_make_string(r);
                        free(r);
                        break;
                    }
                    case BUILTIN_REGEX_MATCH: {
                        Value pat = stack[--sp];
                        Value str = stack[--sp];
                        stack[sp++] = lumyr_make_bool(lumyr_regex_match(
                            str.type == VAL_STRING ? (lumyr_str_cstr(&str) ? lumyr_str_cstr(&str) : "") : "",
                            pat.type == VAL_STRING ? (lumyr_str_cstr(&pat) ? lumyr_str_cstr(&pat) : "") : ""));
                        break;
                    }
                    case BUILTIN_REGEX_SEARCH: {
                        Value pat = stack[--sp];
                        Value str = stack[--sp];
                        stack[sp++] = lumyr_regex_search(
                            str.type == VAL_STRING ? (lumyr_str_cstr(&str) ? lumyr_str_cstr(&str) : "") : "",
                            pat.type == VAL_STRING ? (lumyr_str_cstr(&pat) ? lumyr_str_cstr(&pat) : "") : "");
                        break;
                    }
                    case BUILTIN_REGEX_REPLACE: {
                        Value repl = stack[--sp];
                        Value pat = stack[--sp];
                        Value str = stack[--sp];
                        char* r = lumyr_regex_replace(
                            str.type == VAL_STRING ? (lumyr_str_cstr(&str) ? lumyr_str_cstr(&str) : "") : "",
                            pat.type == VAL_STRING ? (lumyr_str_cstr(&pat) ? lumyr_str_cstr(&pat) : "") : "",
                            repl.type == VAL_STRING ? (lumyr_str_cstr(&repl) ? lumyr_str_cstr(&repl) : "") : "");
                        stack[sp++] = lumyr_make_string(r);
                        free(r);
                        break;
                    }
                    case BUILTIN_NOW: {
                        stack[sp++] = lumyr_now();
                        break;
                    }
                    case BUILTIN_TIMESTAMP: {
                        stack[sp++] = lumyr_make_double(lumyr_timestamp());
                        break;
                    }
                    case BUILTIN_TIMESTAMP_MS: {
                        stack[sp++] = lumyr_make_int(lumyr_timestamp_ms());
                        break;
                    }
                    case BUILTIN_SLEEP: {
                        Value v = stack[--sp];
                        lumyr_sleep_ms((long long)lumyr_extract_int(v));
                        stack[sp++] = val_none();
                        break;
                    }
                    case BUILTIN_DATE: {
                        char* r = lumyr_date_str();
                        stack[sp++] = lumyr_make_string(r);
                        free(r);
                        break;
                    }
                    case BUILTIN_TIME: {
                        char* r = lumyr_time_str();
                        stack[sp++] = lumyr_make_string(r);
                        free(r);
                        break;
                    }
                    case BUILTIN_DATETIME: {
                        char* r = lumyr_datetime_str();
                        stack[sp++] = lumyr_make_string(r);
                        free(r);
                        break;
                    }
                    case BUILTIN_FORMAT_TIME: {
                        Value ts = val_none();
                        Value fmt;
                        if(in.b >= 2) { ts = stack[--sp]; fmt = stack[--sp]; }
                        else { fmt = stack[--sp]; }
                        double tsv = (ts.type == VAL_NONE) ? -1.0 : (ts.type == VAL_DOUBLE ? ts.v.d : (double)lumyr_extract_int(ts));
                        char* r = lumyr_format_time(fmt.type == VAL_STRING ? (lumyr_str_cstr(&fmt) ? lumyr_str_cstr(&fmt) : "") : "", tsv);
                        stack[sp++] = lumyr_make_string(r);
                        free(r);
                        break;
                    }
                    case BUILTIN_LOG_DEBUG:
                    case BUILTIN_LOG_INFO:
                    case BUILTIN_LOG_WARN:
                    case BUILTIN_LOG_ERROR:
                    case BUILTIN_LOG_FATAL: {
                        int lvl = in.a - BUILTIN_LOG_DEBUG;
                        Value msg;
                        if(in.b >= 2) { msg = stack[--sp]; --sp; }  // 方法链：先弹消息，再丢弃 receiver
                        else { msg = stack[--sp]; }
                        char* ms = value_to_str(msg);
                        lumyr_log(lvl, ms);
                        free(ms);
                        stack[sp++] = val_none();
                        break;
                    }
                    case BUILTIN_HTTP_DELETE: m = "DELETE"; break;
                            case BUILTIN_HTTP_HEAD:   m = "HEAD"; break;
                            case BUILTIN_HTTP_PATCH:  m = "PATCH"; break;
                            default: break;
                        }
                        Value url    = stack[sp - n];
                        Value params = (n >= 2) ? stack[sp - n + 1] : val_none();
                        Value config = (n >= 3) ? stack[sp - n + 2] : val_none();
                        Value r = lumyr_http_request(m, url, params, config);
                        sp = sp - n + 1;
                        sp--; stack[sp++] = r;
                        break;
                    }
                    case BUILTIN_MAP:
                    case BUILTIN_FILTER:
                    case BUILTIN_REDUCE: {
                        int argc = in.b;
                        Value fn, arr, init = val_none();
                        if(argc == 3) { init = stack[--sp]; fn = stack[--sp]; arr = stack[--sp]; }
                        else { fn = stack[--sp]; arr = stack[--sp]; }
                        if(arr.type == VAL_MAP && in.a == BUILTIN_MAP) {
                            /* 字典 map：fn(value, key) → 新字典（键不变值映射） */
                            if(fn.type != VAL_FUNC) runtime_error("map() 第二个参数必须是函数");
                            RuntimeFunc* mrf = fn.v.func.func_obj;
                            Value mout = val_map();
                            MapIter it; map_iter_init(&it, arr.v.map);
                            Value mk, mv;
                            while(map_iter_next(&it, &mk, &mv)) {
                                Value a2[2];
                                a2[0] = mv;
                                a2[1] = mk;
                                Value r = vm_call_rf(mrf, a2, 2, frame, ctx);
                                lumyr_map_set(&mout, mk, r);
                            }
                            stack[sp++] = mout;
                            break;
                        }
                        /* 生成器支持：创建包装生成器 */
                        if(arr.type == VAL_GENERATOR && (in.a == BUILTIN_MAP || in.a == BUILTIN_FILTER)) {
                            if(fn.type != VAL_FUNC) runtime_error("map()/filter() 第二个参数必须是函数");
                            GeneratorObject* wrapped = (GeneratorObject*)arr.v.generator;
                            GeneratorObject* wg = (GeneratorObject*)calloc(1, sizeof(GeneratorObject));
                            wg->is_wrapped = 1;
                            wg->wrap_type = (in.a == BUILTIN_MAP) ? WRAP_MAP : WRAP_FILTER;
                            wg->wrapped_gen = wrapped;
                            wg->wrap_fn = fn.v.func.func_obj;
                            Value gv; gv.type = VAL_GENERATOR; gv.v.generator = (void*)wg;
                            stack[sp++] = gv;
                            break;
                        }
                        if(arr.type != VAL_ARRAY) runtime_error("map()/filter()/reduce() 第一个参数必须是数组");
                        if(fn.type != VAL_FUNC) runtime_error("map()/filter()/reduce() 第二个参数必须是函数");
                        RuntimeFunc* rf = fn.v.func.func_obj;
                        int n = arr.v.array->len;
                        if(in.a == BUILTIN_MAP) {
                            Value out = val_array(n);
                            for(int i = 0; i < n; i++) {
                                Value a1[1] = { arr.v.array->items[i] };
                                Value r = vm_call_rf(rf, a1, 1, frame, ctx);
                                out.v.array->items[i] = r;
                            }
                            stack[sp++] = out;
                        } else if(in.a == BUILTIN_FILTER) {
                            Value out = val_array(n);
                            int cnt = 0;
                            for(int i = 0; i < n; i++) {
                                Value a1[1] = { arr.v.array->items[i] };
                                Value r = vm_call_rf(rf, a1, 1, frame, ctx);
                                if(lumyr_to_bool(r)) out.v.array->items[cnt++] = arr.v.array->items[i];
                            }
                            out.v.array->len = cnt;
                            stack[sp++] = out;
                        } else {
                            Value acc = init;
                            for(int i = 0; i < n; i++) {
                                Value a2[2] = { acc, arr.v.array->items[i] };
                                acc = vm_call_rf(rf, a2, 2, frame, ctx);
                            }
                            stack[sp++] = acc;
                        }
                        break;
                    }
                    default:
                        runtime_error("未知内置函数");
                        break;
                }
                (void)argc;
    return sp;
}

static Value vm_run(BytecodeFunc* bf, StackFrame* frame, EvalCtx* ctx)
{
    /* 访问修饰符检查：保存旧的当前类名，设置为当前函数所属的类名 */
    const char* old_current_class = lumyr_get_current_class();
    lumyr_set_current_class(bf->class_name);
    /* 生成器上下文恢复：如果 s_current_gen 不为 NULL，从生成器对象恢复状态 */
    GeneratorObject* gen_ctx = s_current_gen;
    int is_generator = (gen_ctx != NULL);
    // 静态栈深度分析：精确分配执行栈（动态，无硬上限），并校验 IR 栈平衡
    int maxd = bc_analyze_stack(bf, NULL, 0);
    if(maxd < 0) exit(EXIT_FAILURE);   // 已打印下溢位置
    Value* stack;
    int sp;
    int pc;
    if(is_generator) {
        /* 生成器模式：复用生成器的 stack，从保存的 pc/sp 恢复 */
        stack = gen_ctx->stack;
        sp = gen_ctx->sp;
        pc = gen_ctx->pc;
        /* 如果是从 yield 恢复（不是第一次启动），恢复 try-catch 上下文 */
        if(gen_ctx->started) {
            generator_restore_try_context(gen_ctx);
        }
        /* 如果是从 yield 恢复（不是第一次启动），把 send_value 压入栈顶作为 yield 表达式的返回值。
         * 即使没有 send_value（第一次 next()），也压入 none 作为默认返回值，
         * 否则 yield 表达式后面的 OPC_POP/赋值会弹出空栈导致栈下溢（sp 变负），
         * 进而引发堆破坏（越界写覆盖相邻 GeneratorObject 字段）。 */
        if(gen_ctx->started) {
            if(gen_ctx->has_send_value) {
                stack[sp++] = gen_ctx->send_value;
            } else {
                stack[sp++] = val_none();
            }
        }
    } else {
        stack = (Value*)malloc(sizeof(Value) * (maxd + 2));
        if(!stack) { perror("vm_run"); exit(EXIT_FAILURE); }
        sp = 0;
        pc = 0;
    }
    /* 注册 GC 根：保存旧根（嵌套调用恢复用），设置当前线程的栈与帧 */
    Value* old_gc_stack; int* old_gc_sp; StackFrame* old_gc_frame;
    gc_get_roots(&old_gc_stack, &old_gc_sp, &old_gc_frame);
    gc_set_roots(stack, &sp, frame);
    /* 注册当前线程到全局 GC 线程注册表：GC 时扫描所有注册线程的栈和帧链，
     * 防止其他线程栈上持有的对象引用被误回收（多线程 UAF 根因）。 */
    gc_register_thread(stack, &sp, frame);
    tls_vm_run_depth++;
    /* 函数边界隔离 try 状态：进入保存，所有退出点恢复（try 内 return 不能泄漏） */
    int saved_depth = vm_depth;
    jmp_buf* saved_gj = g_err_jmp;
    int saved_fin = vm_fin_n;

    /* 生成器第一次启动：重置 try 状态，避免继承调用者的 try 上下文。
     * 生成器有独立的 try-catch 栈，不应与调用者共享；否则 yield 时保存的
     * vm_depth 会错误包含外部 try 深度，恢复时重新 setjmp 会覆盖外部缓冲区，
     * 导致外部 try-catch 失效（Bug: 生成器 next() 在外部 try 内时异常无法捕获）。 */
    if(is_generator && !gen_ctx->started) {
        vm_depth = 0;
        vm_fin_n = 0;
        g_err_jmp = NULL;
    }

    /* 生成器恢复时重新建立 try-catch 的 setjmp 缓冲区
     * （yield 时保存的缓冲区指向旧 C 栈帧，已销毁失效，必须在当前栈帧重新 setjmp）
     * setjmp 返回 0：继续建立下一层或进入正常执行
     * setjmp 返回非 0：异常被这一层 catch 捕获，恢复状态并跳转到 catch 块 */
    /* 生成器恢复时重新建立 try-catch 的 setjmp 缓冲区
     * （yield 时保存的缓冲区指向旧 C 栈帧，已销毁失效，必须在当前栈帧重新 setjmp）
     * 注意：setjmp/longjmp 之间的非 volatile 局部变量在 longjmp 后值未定义（C 标准），
     * 故 longjmp 回来后用 g_err_jmp - vm_jbs 反推层号，不依赖循环变量 i（与 OPC_TRY 一致） */
    int gen_caught_layer = -1;
    if(is_generator && gen_ctx->started && gen_ctx->saved_vm_depth > 0) {
        int depth = gen_ctx->saved_vm_depth;
        for(int i = 0; i < depth; i++) {
            if(setjmp(vm_jbs[i]) == 0) {
                vm_prev[i] = (i == 0) ? saved_gj : &vm_jbs[i-1];
                /* vm_sp[i]/vm_target[i]/vm_tn[i]/vm_fn[i] 已由 generator_restore_try_context 恢复 */
                vm_depth = i + 1;
                g_err_jmp = &vm_jbs[i];
            } else {
                /* longjmp 后局部变量 i 值未定义：用 g_err_jmp 反推层号（与 OPC_TRY else 分支一致） */
                int d2 = (int)(g_err_jmp - vm_jbs);
                if(d2 < 0 || d2 >= depth) d2 = depth - 1;
                gen_caught_layer = d2;
                sp = vm_sp[d2];
                vm_depth = d2 + 1;  /* catch 块与 try 块在同一层 try 保护区中 */
                g_err_jmp = vm_prev[d2];
                pc = vm_target[d2];
                break;
            }
        }
    }

    /* 生成器恢复时的待抛出异常（GenThrow）：
     * 现在 setjmp 缓冲区已在当前栈帧重新建立，longjmp 有效；
     * gen_caught_layer >= 0 表示重新 setjmp 时已经被 catch 捕获（不会走到这里） */
    if(is_generator && gen_ctx->started && gen_ctx->has_pending_exception && gen_caught_layer < 0) {
        stack[sp++] = gen_ctx->pending_exception;
        gen_ctx->has_pending_exception = 0;
        Value v = stack[--sp];
        const char* type = "Error";
        char* msg = NULL;
        if(v.type == VAL_ERROR) {
            type = v.v.err.type ? v.v.err.type : "Error";
            msg = strdup(v.v.err.message ? v.v.err.message : "");
        } else if(v.type == VAL_MAP) {
            if(lumyr_map_has(v, lumyr_make_string("type"))) {
                Value tv = lumyr_map_get(v, lumyr_make_string("type"));
                if(tv.type == VAL_STRING) type = lumyr_str_cstr(&tv);
            }
            if(lumyr_map_has(v, lumyr_make_string("message"))) {
                Value mv = lumyr_map_get(v, lumyr_make_string("message"));
                if(mv.type == VAL_STRING) msg = strdup(lumyr_str_cstr(&mv));
            }
        }
        if(!msg) msg = value_to_str(v);
        g_err_type_set(type);
        g_err_msg_set(msg);
        free(msg);
        if(g_err_jmp) longjmp(*g_err_jmp, 1);
        LOG_ERROR("Runtime Error: %s\n", g_err_msg);
        exit(EXIT_FAILURE);
    }

    if(getenv("LUMYR_BC_DUMP")) {
        LOG_ERROR("== bc dump: %s (code_len=%d, max_stack=%d) ==\n",
                bf->name ? bf->name : "<main>", bf->code_len, maxd);
        for(int i = 0; i < bf->code_len; i++) {
            Instruction in = bf->code[i];
            const char* n = (in.a >= 0 && in.a < bf->sym_cnt) ? bf->syms[in.a] : "?";
            LOG_ERROR("  %4d: op=%d a=%d(%s) b=%d\n", i, (int)in.op, in.a, n, in.b);
        }
    }

    for(;;) {
        gc_stw_check_fast();  /* 协作式 STW 安全点：内联快速路径，非 GC 时无函数调用开销 */
        Instruction in = bf->code[pc++];
        switch(in.op) {
            case OPC_NOP:
                break;
            case OPC_LOAD_CONST:
                stack[sp++] = bf->consts[in.a];
                break;
            case OPC_GETFUNC: {
                const char* fname = bf->syms[in.a];
                Value fv = val_none();
                if(sym_has(fname)) fv = sym_get(fname);
                else runtime_undefined("函数", fname);
                stack[sp++] = fv;
                break;
            }
            case OPC_MKCLOSURE: {
                // 沿当前帧链装箱该 lambda 的捕获变量，生成新闭包函数值
                const char* fname = bf->syms[in.a];
                if(!sym_has(fname)) runtime_undefined("函数", fname);
                Value tpl = sym_get(fname);
                if(tpl.type != VAL_FUNC) runtime_error("闭包模板不是函数");
                Value clos = closure_make_instance(tpl.v.func.func_obj, frame);
                stack[sp++] = clos;
                break;
            }
            case OPC_LOAD_VAR: {
                const char* name = bf->syms[in.a];
                _Bool fnd = 0;
                Value vv = stackframe_get(frame, name, &fnd);
                if(!fnd) runtime_undefined("变量", name);
                stack[sp++] = vv;
                break;
            }
            case OPC_LOAD_INT_VAR: {
                /* 声明为 int 类型的变量：直接从栈帧的 int_vals 数组读取，零提取、零类型检查
                   stackframe_get_int 直接返回原始 int 值，不需要从 Value 联合体提取 */
                const char* name = bf->syms[in.a];
                _Bool fnd = 0;
                int iv = stackframe_get_int(frame, name, &fnd);
                if(!fnd) runtime_undefined("变量", name);
                INT_PUSH(iv);
                break;
            }
            case OPC_PUSH_INT_CONST: {
                /* int 常量：直接把常量值压入 int 栈，零检查零转换
                   用于 <int>42 字面量赋值等场景，避免创建 Value 再提取的开销 */
                INT_PUSH(in.a);
                break;
            }
            case OPC_LOAD_DOUBLE_VAR: {
                /* 声明为 double 类型的变量：直接从栈帧的 double_vals 数组读取，零提取、零类型检查
                   stackframe_get_double 直接返回原始 double 值，不需要从 Value 联合体提取 */
                const char* name = bf->syms[in.a];
                _Bool fnd = 0;
                double dv = stackframe_get_double(frame, name, &fnd);
                if(!fnd) runtime_undefined("变量", name);
                DOUBLE_PUSH(dv);
                break;
            }
            case OPC_LOAD_FLOAT_VAR: {
                /* 声明为 float 类型的变量：直接从栈帧的 float_vals 数组读取，零提取、零类型检查 */
                const char* name = bf->syms[in.a];
                _Bool fnd = 0;
                float fv = stackframe_get_float(frame, name, &fnd);
                if(!fnd) runtime_undefined("变量", name);
                FLOAT_PUSH(fv);
                break;
            }
            case OPC_LOAD_UINT_VAR: {
                /* 声明为 uint 类型的变量：直接从栈帧的 uint_vals 数组读取，零提取、零类型检查 */
                const char* name = bf->syms[in.a];
                _Bool fnd = 0;
                unsigned int uv = stackframe_get_uint(frame, name, &fnd);
                if(!fnd) runtime_undefined("变量", name);
                UINT_PUSH(uv);
                break;
            }
            case OPC_LOAD_BOOL_VAR: {
                const char* name = bf->syms[in.a];
                _Bool fnd = 0;
                _Bool bv = stackframe_get_bool(frame, name, &fnd);
                if(!fnd) runtime_undefined("变量", name);
                BOOL_PUSH(bv);
                break;
            }
            case OPC_LOAD_CHAR_VAR: {
                const char* name = bf->syms[in.a];
                _Bool fnd = 0;
                char cv = stackframe_get_char(frame, name, &fnd);
                if(!fnd) runtime_undefined("变量", name);
                CHAR_PUSH(cv);
                break;
            }
            case OPC_LOAD_BYTE_VAR: {
                const char* name = bf->syms[in.a];
                _Bool fnd = 0;
                unsigned char bv = stackframe_get_byte(frame, name, &fnd);
                if(!fnd) runtime_undefined("变量", name);
                BYTE_PUSH(bv);
                break;
            }
            case OPC_LOAD_INT8_VAR: {
                const char* name = bf->syms[in.a];
                _Bool fnd = 0;
                int8_t i8v = stackframe_get_int8(frame, name, &fnd);
                if(!fnd) runtime_undefined("变量", name);
                INT8_PUSH(i8v);
                break;
            }
            case OPC_LOAD_INT16_VAR: {
                const char* name = bf->syms[in.a];
                _Bool fnd = 0;
                int16_t i16v = stackframe_get_int16(frame, name, &fnd);
                if(!fnd) runtime_undefined("变量", name);
                INT16_PUSH(i16v);
                break;
            }
            case OPC_LOAD_INT32_VAR: {
                const char* name = bf->syms[in.a];
                _Bool fnd = 0;
                int32_t i32v = stackframe_get_int32(frame, name, &fnd);
                if(!fnd) runtime_undefined("变量", name);
                INT32_PUSH(i32v);
                break;
            }
            case OPC_LOAD_INT64_VAR: {
                const char* name = bf->syms[in.a];
                _Bool fnd = 0;
                int64_t i64v = stackframe_get_int64(frame, name, &fnd);
                if(!fnd) runtime_undefined("变量", name);
                INT64_PUSH(i64v);
                break;
            }
            case OPC_LOAD_UINT8_VAR: {
                const char* name = bf->syms[in.a];
                _Bool fnd = 0;
                uint8_t u8v = stackframe_get_uint8(frame, name, &fnd);
                if(!fnd) runtime_undefined("变量", name);
                UINT8_PUSH(u8v);
                break;
            }
            case OPC_LOAD_UINT16_VAR: {
                const char* name = bf->syms[in.a];
                _Bool fnd = 0;
                uint16_t u16v = stackframe_get_uint16(frame, name, &fnd);
                if(!fnd) runtime_undefined("变量", name);
                UINT16_PUSH(u16v);
                break;
            }
            case OPC_LOAD_UINT32_VAR: {
                const char* name = bf->syms[in.a];
                _Bool fnd = 0;
                uint32_t u32v = stackframe_get_uint32(frame, name, &fnd);
                if(!fnd) runtime_undefined("变量", name);
                UINT32_PUSH(u32v);
                break;
            }
            case OPC_LOAD_UINT64_VAR: {
                const char* name = bf->syms[in.a];
                _Bool fnd = 0;
                uint64_t u64v = stackframe_get_uint64(frame, name, &fnd);
                if(!fnd) runtime_undefined("变量", name);
                UINT64_PUSH(u64v);
                break;
            }
            case OPC_LOAD_LONG_VAR: {
                const char* name = bf->syms[in.a];
                _Bool fnd = 0;
                long lv = stackframe_get_long(frame, name, &fnd);
                if(!fnd) runtime_undefined("变量", name);
                LONG_PUSH(lv);
                break;
            }
            case OPC_LOAD_ULONG_VAR: {
                const char* name = bf->syms[in.a];
                _Bool fnd = 0;
                unsigned long ulv = stackframe_get_ulong(frame, name, &fnd);
                if(!fnd) runtime_undefined("变量", name);
                ULONG_PUSH(ulv);
                break;
            }
            case OPC_LOAD_SIZE_T_VAR: {
                const char* name = bf->syms[in.a];
                _Bool fnd = 0;
                size_t stv = stackframe_get_size_t(frame, name, &fnd);
                if(!fnd) runtime_undefined("变量", name);
                SIZE_T_PUSH(stv);
                break;
            }
            case OPC_LOAD_SSIZE_T_VAR: {
                const char* name = bf->syms[in.a];
                _Bool fnd = 0;
                ssize_t sstv = stackframe_get_ssize_t(frame, name, &fnd);
                if(!fnd) runtime_undefined("变量", name);
                SSIZE_T_PUSH(sstv);
                break;
            }
            case OPC_LOAD_LONG_DOUBLE_VAR: {
                const char* name = bf->syms[in.a];
                _Bool fnd = 0;
                long double ldv = stackframe_get_long_double(frame, name, &fnd);
                if(!fnd) runtime_undefined("变量", name);
                LONG_DOUBLE_PUSH(ldv);
                break;
            }
            case OPC_LOAD_VAR_REF: {
                /* ref 参数：和 OPC_LOAD_VAR 行为相同（VM 模式下 struct 本来就是 Value(map)） */
                const char* name = bf->syms[in.a];
                _Bool fnd = 0;
                Value vv = stackframe_get(frame, name, &fnd);
                if(!fnd) runtime_undefined("变量", name);
                stack[sp++] = vv;
                break;
            }
            case OPC_STORE_INT_VAR: {
                /* 从 int 栈弹出 int 值，直接存储到 int 变量，零重复提取
                   stackframe_bind_int 同时更新 vals 和 int_vals，避免从 Value 重复提取
                   然后把int值包装成Value压回Value栈（赋值表达式有返回值，如 a = b = 5） */
                const char* name = bf->syms[in.a];
                int iv = INT_POP();  // 从 int 栈弹出 int 值
                /* 直接绑定 int 变量（同时更新 vals 和 int_vals，零重复提取） */
                stackframe_bind_int(frame, name, iv);
                /* 包装成 Value 压回（赋值表达式有返回值，如 a = b = 5） */
                Value ret;
                ret.type = 1;  // VAL_INT
                ret.v.i = iv;
                stack[sp++] = ret;
                break;
            }
            case OPC_STORE_DOUBLE_VAR: {
                /* 从 double 栈弹出 double 值，直接存储到 double 变量，零重复提取
                   stackframe_bind_double 同时更新 vals 和 double_vals，避免从 Value 重复提取 */
                const char* name = bf->syms[in.a];
                double dv = DOUBLE_POP();  // 从 double 栈弹出 double 值
                /* 直接绑定 double 变量（同时更新 vals 和 double_vals，零重复提取） */
                stackframe_bind_double(frame, name, dv);
                /* 包装成 Value 压回（赋值表达式有返回值，如 a = b = 5.0） */
                Value ret;
                ret.type = 2;  // VAL_DOUBLE
                ret.v.d = dv;
                stack[sp++] = ret;
                break;
            }
            case OPC_STORE_FLOAT_VAR: {
                /* 从 float 栈弹出 float 值，直接存储到 float 变量，零重复提取
                   stackframe_bind_float 同时更新 vals 和 float_vals，避免从 Value 重复提取 */
                const char* name = bf->syms[in.a];
                float fv = FLOAT_POP();  // 从 float 栈弹出 float 值
                /* 直接绑定 float 变量（同时更新 vals 和 float_vals，零重复提取） */
                stackframe_bind_float(frame, name, fv);
                /* 包装成 Value 压回（赋值表达式有返回值） */
                Value ret;
                ret.type = 2;  // VAL_DOUBLE（float 用 VAL_DOUBLE 存储）
                ret.v.d = (double)fv;
                stack[sp++] = ret;
                break;
            }
            case OPC_STORE_UINT_VAR: {
                /* 从 uint 栈弹出 uint 值，直接存储到 uint 变量，零重复提取
                   stackframe_bind_uint 同时更新 vals 和 uint_vals，避免从 Value 重复提取 */
                const char* name = bf->syms[in.a];
                unsigned int uv = UINT_POP();  // 从 uint 栈弹出 uint 值
                /* 直接绑定 uint 变量（同时更新 vals 和 uint_vals，零重复提取） */
                stackframe_bind_uint(frame, name, uv);
                /* 包装成 Value 压回（赋值表达式有返回值） */
                Value ret;
                ret.type = 1;  // VAL_INT（uint 用 VAL_INT 存储，long long 可以存储 uint32_t）
                ret.v.i = (long long)uv;
                stack[sp++] = ret;
                break;
            }
            case OPC_STORE_BOOL_VAR: {
                const char* name = bf->syms[in.a];
                _Bool bv = BOOL_POP();
                stackframe_bind_bool(frame, name, bv);
                Value ret;
                ret.type = VAL_BOOL;
                ret.v.i = bv ? 1 : 0;
                stack[sp++] = ret;
                break;
            }
            case OPC_STORE_CHAR_VAR: {
                const char* name = bf->syms[in.a];
                char cv = CHAR_POP();
                stackframe_bind_char(frame, name, cv);
                Value ret;
                ret.type = VAL_CHAR;
                ret.v.i = (long long)cv;
                stack[sp++] = ret;
                break;
            }
            case OPC_STORE_BYTE_VAR: {
                const char* name = bf->syms[in.a];
                unsigned char bv = BYTE_POP();
                stackframe_bind_byte(frame, name, bv);
                Value ret;
                ret.type = VAL_BYTE;
                ret.v.i = (long long)bv;
                stack[sp++] = ret;
                break;
            }
            case OPC_STORE_INT8_VAR: {
                const char* name = bf->syms[in.a];
                int8_t i8v = INT8_POP();
                stackframe_bind_int8(frame, name, i8v);
                Value ret;
                ret.type = VAL_INT8;
                ret.v.i = (long long)i8v;
                stack[sp++] = ret;
                break;
            }
            case OPC_STORE_INT16_VAR: {
                const char* name = bf->syms[in.a];
                int16_t i16v = INT16_POP();
                stackframe_bind_int16(frame, name, i16v);
                Value ret;
                ret.type = VAL_INT16;
                ret.v.i = (long long)i16v;
                stack[sp++] = ret;
                break;
            }
            case OPC_STORE_INT32_VAR: {
                const char* name = bf->syms[in.a];
                int32_t i32v = INT32_POP();
                stackframe_bind_int32(frame, name, i32v);
                Value ret;
                ret.type = VAL_INT32;
                ret.v.i = (long long)i32v;
                stack[sp++] = ret;
                break;
            }
            case OPC_STORE_INT64_VAR: {
                const char* name = bf->syms[in.a];
                int64_t i64v = INT64_POP();
                stackframe_bind_int64(frame, name, i64v);
                Value ret;
                ret.type = VAL_INT64;
                ret.v.i = (long long)i64v;
                stack[sp++] = ret;
                break;
            }
            case OPC_STORE_UINT8_VAR: {
                const char* name = bf->syms[in.a];
                uint8_t u8v = UINT8_POP();
                stackframe_bind_uint8(frame, name, u8v);
                Value ret;
                ret.type = VAL_UINT8;
                ret.v.i = (long long)u8v;
                stack[sp++] = ret;
                break;
            }
            case OPC_STORE_UINT16_VAR: {
                const char* name = bf->syms[in.a];
                uint16_t u16v = UINT16_POP();
                stackframe_bind_uint16(frame, name, u16v);
                Value ret;
                ret.type = VAL_UINT16;
                ret.v.i = (long long)u16v;
                stack[sp++] = ret;
                break;
            }
            case OPC_STORE_UINT32_VAR: {
                const char* name = bf->syms[in.a];
                uint32_t u32v = UINT32_POP();
                stackframe_bind_uint32(frame, name, u32v);
                Value ret;
                ret.type = VAL_UINT32;
                ret.v.i = (long long)u32v;
                stack[sp++] = ret;
                break;
            }
            case OPC_STORE_UINT64_VAR: {
                const char* name = bf->syms[in.a];
                uint64_t u64v = UINT64_POP();
                stackframe_bind_uint64(frame, name, u64v);
                Value ret;
                ret.type = VAL_UINT64;
                ret.v.i = (long long)u64v;
                stack[sp++] = ret;
                break;
            }
            case OPC_STORE_LONG_VAR: {
                const char* name = bf->syms[in.a];
                long lv = LONG_POP();
                stackframe_bind_long(frame, name, lv);
                Value ret;
                ret.type = VAL_LONG;
                ret.v.i = (long long)lv;
                stack[sp++] = ret;
                break;
            }
            case OPC_STORE_ULONG_VAR: {
                const char* name = bf->syms[in.a];
                unsigned long ulv = ULONG_POP();
                stackframe_bind_ulong(frame, name, ulv);
                Value ret;
                ret.type = VAL_ULONG;
                ret.v.i = (long long)ulv;
                stack[sp++] = ret;
                break;
            }
            case OPC_STORE_SIZE_T_VAR: {
                const char* name = bf->syms[in.a];
                size_t stv = SIZE_T_POP();
                stackframe_bind_size_t(frame, name, stv);
                Value ret;
                ret.type = VAL_SIZE_T;
                ret.v.i = (long long)stv;
                stack[sp++] = ret;
                break;
            }
            case OPC_STORE_SSIZE_T_VAR: {
                const char* name = bf->syms[in.a];
                ssize_t sstv = SSIZE_T_POP();
                stackframe_bind_ssize_t(frame, name, sstv);
                Value ret;
                ret.type = VAL_SSIZE_T;
                ret.v.i = (long long)sstv;
                stack[sp++] = ret;
                break;
            }
            case OPC_STORE_LONG_DOUBLE_VAR: {
                const char* name = bf->syms[in.a];
                long double ldv = LONG_DOUBLE_POP();
                stackframe_bind_long_double(frame, name, ldv);
                Value ret;
                ret.type = VAL_LONG_DOUBLE;
                ret.v.d = (double)ldv;
                stack[sp++] = ret;
                break;
            }
            case OPC_STORE_VAR: {
                const char* name = bf->syms[in.a];
                Value v = stack[--sp];
                /* struct 类型变量赋值时进行浅拷贝（C 语义：p2 = p1 是 memcpy，不是引用） */
                if(bf->var_struct_names && in.a >= 0 && in.a < bf->sym_cnt &&
                   bf->var_struct_names[in.a] && v.type == VAL_MAP) {
                    v = lumyr_map_shallow_copy(v);
                }
                /* 词法遮蔽：函数内赋值 = 绑定当前帧局部（C 语义：局部变量遮蔽全局同名）；
                   不再沿链更新父帧/全局。顶层（main 帧）赋值仍写入全局帧。 */
                stackframe_bind(frame, name, v);
                /* 记录变量类型标记：如果编译阶段记录了类型标记，则传播到 StackFrame */
                if(bf->var_type_tags && in.a >= 0 && in.a < bf->sym_cnt && bf->var_type_tags[in.a] >= 0) {
                    stackframe_set_type_tag(frame, name, bf->var_type_tags[in.a]);
                }
                stack[sp++] = v;             // 原值压回（表达式值）
                break;
            }
            case OPC_ADD: {
                Value r = stack[--sp], l = stack[--sp];
                Value result;
                if(try_operator_overload("+", l, r, stack, &sp, frame, ctx, &result)) {
                    stack[sp++] = result;
                } else {
                    stack[sp++] = lumyr_add(l, r);
                }
                break;
            }
            case OPC_SUB: { Value r = stack[--sp], l = stack[--sp]; stack[sp++] = lumyr_sub(l, r); break; }
            case OPC_MUL: { Value r = stack[--sp], l = stack[--sp]; stack[sp++] = lumyr_mul(l, r); break; }
            case OPC_DIV: { Value r = stack[--sp], l = stack[--sp]; stack[sp++] = lumyr_div(l, r); break; }
            case OPC_MOD: { Value r = stack[--sp], l = stack[--sp]; stack[sp++] = lumyr_mod(l, r); break; }
            case OPC_GT:  {
                Value r = stack[--sp], l = stack[--sp];
                Value result;
                if(try_operator_overload(">", l, r, stack, &sp, frame, ctx, &result)) {
                    stack[sp++] = result;
                } else {
                    stack[sp++] = lumyr_gt(l, r);
                }
                break;
            }
            case OPC_LT:  {
                Value r = stack[--sp], l = stack[--sp];
                Value result;
                if(try_operator_overload("<", l, r, stack, &sp, frame, ctx, &result)) {
                    stack[sp++] = result;
                } else {
                    stack[sp++] = lumyr_lt(l, r);
                }
                break;
            }
            case OPC_GE:  {
                Value r = stack[--sp], l = stack[--sp];
                Value result;
                if(try_operator_overload(">=", l, r, stack, &sp, frame, ctx, &result)) {
                    stack[sp++] = result;
                } else {
                    stack[sp++] = lumyr_ge(l, r);
                }
                break;
            }
            case OPC_LE:  {
                Value r = stack[--sp], l = stack[--sp];
                Value result;
                if(try_operator_overload("<=", l, r, stack, &sp, frame, ctx, &result)) {
                    stack[sp++] = result;
                } else {
                    stack[sp++] = lumyr_le(l, r);
                }
                break;
            }
            case OPC_EQ:  {
                Value r = stack[--sp], l = stack[--sp];
                Value result;
                if(try_operator_overload("==", l, r, stack, &sp, frame, ctx, &result)) {
                    stack[sp++] = result;
                } else {
                    stack[sp++] = lumyr_eq(l, r);
                }
                break;
            }
            case OPC_NE:  {
                Value r = stack[--sp], l = stack[--sp];
                Value result;
                if(try_operator_overload("!=", l, r, stack, &sp, frame, ctx, &result)) {
                    stack[sp++] = result;
                } else {
                    stack[sp++] = lumyr_ne(l, r);
                }
                break;
            }
            case OPC_IMPLEMENTS: {
                Value r = stack[--sp], l = stack[--sp];
                _Bool impl = 0;
                if(r.type == VAL_STRING) {
                    const char* iface_name = lumyr_str_cstr(&r);
                    /* class 实例：通过 lumyr_obj_implements_interface 判断 */
                    if(l.type == VAL_CLASS_PTR && l.v.struct_ptr) {
                        impl = lumyr_obj_implements_interface(l, iface_name);
                    }
                }
                stack[sp++] = val_bool(impl);
                break;
            }
            case OPC_NEG: { Value v = stack[--sp]; stack[sp++] = lumyr_unary_minus(v); break; }
            case OPC_POS: { Value v = stack[--sp]; stack[sp++] = lumyr_unary_plus(v); break; }
            case OPC_PRE_INC:  { const char* n = bf->syms[in.a]; _Bool fnd = 0;
                                 Value __old = stackframe_get(frame, n, &fnd);
                                 if(!fnd) runtime_undefined("变量", n);
                                 Value __nv = lumyr_pre_inc(&__old);
                                 stackframe_bind(frame, n, __nv);          // 词法遮蔽：写当前帧
                                 stack[sp++] = __nv; break; }
            case OPC_POST_INC: { const char* n = bf->syms[in.a]; _Bool fnd = 0;
                                 Value __old = stackframe_get(frame, n, &fnd);
                                 if(!fnd) runtime_undefined("变量", n);
                                 Value __nv = lumyr_post_inc(&__old);
                                 stackframe_bind(frame, n, __old);          // 参数已被改为新值
                                 stack[sp++] = __nv; break; }               // 返回值 = 旧值
            case OPC_PRE_DEC:  { const char* n = bf->syms[in.a]; _Bool fnd = 0;
                                 Value __old = stackframe_get(frame, n, &fnd);
                                 if(!fnd) runtime_undefined("变量", n);
                                 Value __nv = lumyr_pre_dec(&__old);
                                 stackframe_bind(frame, n, __nv);          // 词法遮蔽：写当前帧
                                 stack[sp++] = __nv; break; }
            case OPC_POST_DEC: { const char* n = bf->syms[in.a]; _Bool fnd = 0;
                                 Value __old = stackframe_get(frame, n, &fnd);
                                 if(!fnd) runtime_undefined("变量", n);
                                 Value __nv = lumyr_post_dec(&__old);
                                 stackframe_bind(frame, n, __old);          // 参数已被改为新值
                                 stack[sp++] = __nv; break; }               // 返回值 = 旧值
            case OPC_CAST_INT:    { Value v = stack[--sp]; stack[sp++] = lumyr_cast_int(v); break; }
            case OPC_CAST_DOUBLE: { Value v = stack[--sp]; stack[sp++] = lumyr_cast_double(v); break; }
            case OPC_CAST_CHAR:   { Value v = stack[--sp]; stack[sp++] = lumyr_cast_char(v); break; }
            case OPC_CAST_BOOL:   { Value v = stack[--sp]; stack[sp++] = lumyr_cast_bool(v); break; }
            case OPC_CAST_STRING: { Value v = stack[--sp]; stack[sp++] = lumyr_cast_string(v); break; }
            case OPC_CAST_ASCII:  { Value v = stack[--sp]; stack[sp++] = lumyr_cast_ascii(v); break; }
            case OPC_CAST_BYTE:   { Value v = stack[--sp]; stack[sp++] = lumyr_cast_byte(v); break; }
            case OPC_CAST_INT8:   { Value v = stack[--sp]; stack[sp++] = lumyr_cast_int8(v); break; }
            case OPC_CAST_INT16:  { Value v = stack[--sp]; stack[sp++] = lumyr_cast_int16(v); break; }
            case OPC_CAST_INT32:  { Value v = stack[--sp]; stack[sp++] = lumyr_cast_int32(v); break; }
            case OPC_CAST_INT64:  { Value v = stack[--sp]; stack[sp++] = lumyr_cast_int64(v); break; }
            case OPC_CAST_UINT8:  { Value v = stack[--sp]; stack[sp++] = lumyr_cast_uint8(v); break; }
            case OPC_CAST_UINT16: { Value v = stack[--sp]; stack[sp++] = lumyr_cast_uint16(v); break; }
            case OPC_CAST_UINT32: { Value v = stack[--sp]; stack[sp++] = lumyr_cast_uint32(v); break; }
            case OPC_CAST_UINT64: { Value v = stack[--sp]; stack[sp++] = lumyr_cast_uint64(v); break; }
            case OPC_CAST_LONG: { Value v = stack[--sp]; stack[sp++] = lumyr_cast_long(v); break; }
            case OPC_CAST_LONGLONG: { Value v = stack[--sp]; stack[sp++] = lumyr_cast_longlong(v); break; }
            case OPC_CAST_FLOAT: { Value v = stack[--sp]; stack[sp++] = lumyr_cast_float(v); break; }
            case OPC_LOGIC_NOT:   { Value v = stack[--sp]; stack[sp++] = lumyr_logic_not(v); break; }
            case OPC_ARRAY_LIT: {
                int n = in.b;
                int elem_type = in.a;
                fprintf(stderr, "[DEBUG] OPC_ARRAY_LIT: elem_type=%d, n=%d\n", elem_type, n);
                if(elem_type == VAL_INT) {
                    /* int 泛型数组：创建 TypedArray，元素类型为 VAL_INT，元素直接存储 int 值 */
                    Value arr = val_int_array(n);
                    TypedArray* tarr = arr.v.typed_array;
                    if(tarr && tarr->items) {
                        int* iitems = (int*)tarr->items;
                        for(int k = 0; k < n; k++) {
                            Value v = stack[sp - n + k];
                            iitems[k] = (v.type == VAL_INT) ? (int)v.v.i : (int)lumyr_cast_long(v).v.i;
                        }
                        tarr->len = n;
                    }
                    sp = sp - n + 1;
                    sp--; stack[sp++] = arr;
                } else {
                    /* 通用数组：创建 ValueArray，元素存储 Value */
                    Value arr = val_array(n);
                    for(int k = 0; k < n; k++)
                        arr.v.array->items[k] = stack[sp - n + k];
                    sp = sp - n + 1;
                    sp--; stack[sp++] = arr;   /* 安全原地写：先 pop 使槽位对 GC 不可见，再 push */
                }
                break;
            }
            case OPC_INT_ARRAY_LIT: {
                /* int 泛型数组：
                   a=1: 从 int 栈读取（零检查零转换）
                   a=0: 从 Value 栈读取（内联类型转换） */
                int n = in.b;
                Value arr = val_int_array(n);
                TypedArray* tarr = arr.v.typed_array;
                if(tarr && tarr->items) {
                    int* iitems = (int*)tarr->items;
                    if(in.a == 1) {
                        /* 从 int 栈读取：零检查零转换，使用栈缓存优化减少重复访问全局变量 */
                        StackCache int_cache;
                        STACK_CACHE_INIT(int_cache, STACK_INT);
                        int* int_stack_ptr = (int*)int_cache.stack;
                        int* int_sp_ptr = int_cache.sp;
                        for(int k = 0; k < n; k++) {
                            iitems[k] = int_stack_ptr[(*int_sp_ptr) - n + k];
                        }
                        (*int_sp_ptr) -= n;
                        /* Value 栈没有元素需要弹出，直接压入数组 */
                        stack[sp++] = arr;
                    } else {
                        /* 从 Value 栈读取：内联类型转换 */
                        for(int k = 0; k < n; k++) {
                            Value v = stack[sp - n + k];
                            switch(v.type) {
                                case VAL_INT:
                                case VAL_BYTE:
                                case VAL_CHAR:
                                case VAL_BOOL:
                                    iitems[k] = (int)v.v.i;
                                    break;
                                case VAL_DOUBLE:
                                    iitems[k] = (int)v.v.d;
                                    break;
                                default:
                                    iitems[k] = (int)lumyr_cast_long(v).v.i;
                                    break;
                            }
                        }
                        sp = sp - n + 1;
                        sp--; stack[sp++] = arr;
                    }
                    tarr->len = n;
                } else {
                    /* 数组创建失败，直接压入 */
                    stack[sp++] = arr;
                }
                break;
            }
            case OPC_DOUBLE_ARRAY_LIT: {
                /* double 泛型数组：
                   a=1: 从 double 栈读取（零检查零转换）
                   a=0: 从 Value 栈读取（内联类型转换） */
                int n = in.b;
                Value arr = val_double_array(n);
                TypedArray* tarr = arr.v.typed_array;
                if(tarr && tarr->items) {
                    double* ditems = (double*)tarr->items;
                    if(in.a == 1) {
                        /* 从 double 栈读取：零检查零转换 */
                        StackCache double_cache;
                        STACK_CACHE_INIT(double_cache, STACK_DOUBLE);
                        double* double_stack_ptr = (double*)double_cache.stack;
                        int* double_sp_ptr = double_cache.sp;
                        for(int k = 0; k < n; k++) {
                            ditems[k] = double_stack_ptr[(*double_sp_ptr) - n + k];
                        }
                        (*double_sp_ptr) -= n;
                        /* Value 栈没有元素需要弹出，直接压入数组 */
                        stack[sp++] = arr;
                    } else {
                        /* 从 Value 栈读取：内联类型转换 */
                        for(int k = 0; k < n; k++) {
                            Value v = stack[sp - n + k];
                            switch(v.type) {
                                case VAL_INT:
                                case VAL_BYTE:
                                case VAL_CHAR:
                                case VAL_BOOL:
                                    ditems[k] = (double)v.v.i;
                                    break;
                                case VAL_DOUBLE:
                                    ditems[k] = v.v.d;
                                    break;
                                default:
                                    ditems[k] = (double)lumyr_cast_long(v).v.i;
                                    break;
                            }
                        }
                        sp = sp - n + 1;
                        sp--; stack[sp++] = arr;
                    }
                    tarr->len = n;
                } else {
                    /* 数组创建失败，直接压入 */
                    stack[sp++] = arr;
                }
                break;
            }
            case OPC_FLOAT_ARRAY_LIT: {
                /* float 泛型数组：
                   a=1: 从 float 栈读取（零检查零转换）
                   a=0: 从 Value 栈读取（内联类型转换） */
                int n = in.b;
                Value arr = val_float_array(n);
                TypedArray* tarr = arr.v.typed_array;
                if(tarr && tarr->items) {
                    float* fitems = (float*)tarr->items;
                    if(in.a == 1) {
                        /* 从 float 栈读取：零检查零转换 */
                        StackCache float_cache;
                        STACK_CACHE_INIT(float_cache, STACK_FLOAT);
                        float* float_stack_ptr = (float*)float_cache.stack;
                        int* float_sp_ptr = float_cache.sp;
                        for(int k = 0; k < n; k++) {
                            fitems[k] = float_stack_ptr[(*float_sp_ptr) - n + k];
                        }
                        (*float_sp_ptr) -= n;
                        /* Value 栈没有元素需要弹出，直接压入数组 */
                        stack[sp++] = arr;
                    } else {
                        /* 从 Value 栈读取：内联类型转换 */
                        for(int k = 0; k < n; k++) {
                            Value v = stack[sp - n + k];
                            switch(v.type) {
                                case VAL_INT:
                                case VAL_BYTE:
                                case VAL_CHAR:
                                case VAL_BOOL:
                                    fitems[k] = (float)v.v.i;
                                    break;
                                case VAL_DOUBLE:
                                    fitems[k] = (float)v.v.d;
                                    break;
                                default:
                                    fitems[k] = (float)lumyr_cast_long(v).v.i;
                                    break;
                            }
                        }
                        sp = sp - n + 1;
                        sp--; stack[sp++] = arr;
                    }
                    tarr->len = n;
                } else {
                    /* 数组创建失败，直接压入 */
                    stack[sp++] = arr;
                }
                break;
            }
            case OPC_UINT_ARRAY_LIT: {
                /* uint 泛型数组：
                   a=1: 从 uint 栈读取（零检查零转换）
                   a=0: 从 Value 栈读取（内联类型转换） */
                int n = in.b;
                Value arr = val_uint_array(n);
                TypedArray* tarr = arr.v.typed_array;
                if(tarr && tarr->items) {
                    unsigned int* uitems = (unsigned int*)tarr->items;
                    if(in.a == 1) {
                        /* 从 uint 栈读取：零检查零转换 */
                        StackCache uint_cache;
                        STACK_CACHE_INIT(uint_cache, STACK_UINT);
                        unsigned int* uint_stack_ptr = (unsigned int*)uint_cache.stack;
                        int* uint_sp_ptr = uint_cache.sp;
                        for(int k = 0; k < n; k++) {
                            uitems[k] = uint_stack_ptr[(*uint_sp_ptr) - n + k];
                        }
                        (*uint_sp_ptr) -= n;
                        /* Value 栈没有元素需要弹出，直接压入数组 */
                        stack[sp++] = arr;
                    } else {
                        /* 从 Value 栈读取：内联类型转换 */
                        for(int k = 0; k < n; k++) {
                            Value v = stack[sp - n + k];
                            switch(v.type) {
                                case VAL_INT:
                                case VAL_BYTE:
                                case VAL_CHAR:
                                case VAL_BOOL:
                                    uitems[k] = (unsigned int)v.v.i;
                                    break;
                                case VAL_DOUBLE:
                                    uitems[k] = (unsigned int)v.v.d;
                                    break;
                                default:
                                    uitems[k] = (unsigned int)lumyr_cast_long(v).v.i;
                                    break;
                            }
                        }
                        sp = sp - n + 1;
                        sp--; stack[sp++] = arr;
                    }
                    tarr->len = n;
                } else {
                    /* 数组创建失败，直接压入 */
                    stack[sp++] = arr;
                }
                break;
            }
            case OPC_MAP_LIT: {
                int n = in.b;
                Value m = lumyr_map_lit(&stack[sp - 2 * n], n);
                sp = sp - 2 * n + 1;
                sp--; stack[sp++] = m;     /* 安全原地写 */
                break;
            }
            case OPC_INT_ARRAY_GET: {
                /* int 类型化数组元素访问：直接读取 int 值，压入 int 栈，零包装零 Value 开销
                   严格类型检查：必须是 int 类型化数组，否则直接抛异常，无须兼容和回退 */
                Value idx = stack[--sp];
                Value arr = stack[--sp];
                int iidx = (int)lumyr_extract_int(idx);
                char errbuf[256];
                if(arr.type != 14 /* VAL_TYPED_ARRAY */ || !arr.v.typed_array) {
                    snprintf(errbuf, sizeof(errbuf), "类型错误：OPC_INT_ARRAY_GET 需要 int 类型化数组，实际类型为 %s", val_typename(arr.type));
                    runtime_error(errbuf);
                }
                TypedArray* tarr = arr.v.typed_array;
                if(tarr->elem_type != 1 /* VAL_INT */) {
                    snprintf(errbuf, sizeof(errbuf), "类型错误：数组元素类型不匹配，期望 int，实际为 %s", val_typename(tarr->elem_type));
                    runtime_error(errbuf);
                }
                if(!tarr->items) {
                    runtime_error("数组错误：int 类型化数组 items 指针为空");
                }
                if(iidx < 0 || iidx >= tarr->len) {
                    snprintf(errbuf, sizeof(errbuf), "数组越界：索引 %d 超出范围 [0, %d)", iidx, tarr->len);
                    runtime_error(errbuf);
                }
                int val = ((int*)tarr->items)[iidx];  // 直接读取 int 值，零提取零转换！
                INT_PUSH(val);  // 压入 int 栈，零包装！
                break;
            }
            case OPC_DOUBLE_ARRAY_GET: {
                /* double 类型化数组元素访问：直接读取 double 值，压入 double 栈，零包装零 Value 开销
                   严格类型检查：必须是 double 类型化数组，否则直接抛异常，无须兼容和回退 */
                Value idx = stack[--sp];
                Value arr = stack[--sp];
                int iidx = (int)lumyr_extract_int(idx);
                char errbuf[256];
                if(arr.type != 14 /* VAL_TYPED_ARRAY */ || !arr.v.typed_array) {
                    snprintf(errbuf, sizeof(errbuf), "类型错误：OPC_DOUBLE_ARRAY_GET 需要 double 类型化数组，实际类型为 %s", val_typename(arr.type));
                    runtime_error(errbuf);
                }
                TypedArray* tarr = arr.v.typed_array;
                if(tarr->elem_type != 2 /* VAL_DOUBLE */) {
                    snprintf(errbuf, sizeof(errbuf), "类型错误：数组元素类型不匹配，期望 double，实际为 %s", val_typename(tarr->elem_type));
                    runtime_error(errbuf);
                }
                if(!tarr->items) {
                    runtime_error("数组错误：double 类型化数组 items 指针为空");
                }
                if(iidx < 0 || iidx >= tarr->len) {
                    snprintf(errbuf, sizeof(errbuf), "数组越界：索引 %d 超出范围 [0, %d)", iidx, tarr->len);
                    runtime_error(errbuf);
                }
                double val = ((double*)tarr->items)[iidx];  // 直接读取 double 值，零提取零转换！
                DOUBLE_PUSH(val);  // 压入 double 栈，零包装！
                break;
            }
            case OPC_FLOAT_ARRAY_GET: {
                /* float 类型化数组元素访问：直接读取 float 值，压入 float 栈，零包装零 Value 开销
                   严格类型检查：必须是 float 类型化数组，否则直接抛异常，无须兼容和回退 */
                Value idx = stack[--sp];
                Value arr = stack[--sp];
                int iidx = (int)lumyr_extract_int(idx);
                char errbuf[256];
                if(arr.type != 14 /* VAL_TYPED_ARRAY */ || !arr.v.typed_array) {
                    snprintf(errbuf, sizeof(errbuf), "类型错误：OPC_FLOAT_ARRAY_GET 需要 float 类型化数组，实际类型为 %s", val_typename(arr.type));
                    runtime_error(errbuf);
                }
                TypedArray* tarr = arr.v.typed_array;
                if(tarr->elem_type != VAL_FLOAT) {
                    snprintf(errbuf, sizeof(errbuf), "类型错误：数组元素类型不匹配，期望 float，实际为 %s", val_typename(tarr->elem_type));
                    runtime_error(errbuf);
                }
                if(!tarr->items) {
                    runtime_error("数组错误：float 类型化数组 items 指针为空");
                }
                if(iidx < 0 || iidx >= tarr->len) {
                    snprintf(errbuf, sizeof(errbuf), "数组越界：索引 %d 超出范围 [0, %d)", iidx, tarr->len);
                    runtime_error(errbuf);
                }
                float val = ((float*)tarr->items)[iidx];  // 直接读取 float 值，零提取零转换！
                FLOAT_PUSH(val);  // 压入 float 栈，零包装！
                break;
            }
            case OPC_UINT_ARRAY_GET: {
                /* uint 类型化数组元素访问：直接读取 uint 值，压入 uint 栈，零包装零 Value 开销
                   严格类型检查：必须是 uint 类型化数组，否则直接抛异常，无须兼容和回退 */
                Value idx = stack[--sp];
                Value arr = stack[--sp];
                int iidx = (int)lumyr_extract_int(idx);
                char errbuf[256];
                if(arr.type != VAL_TYPED_ARRAY || !arr.v.typed_array) {
                    snprintf(errbuf, sizeof(errbuf), "类型错误：OPC_UINT_ARRAY_GET 需要 uint 类型化数组，实际类型为 %s", val_typename(arr.type));
                    runtime_error(errbuf);
                }
                TypedArray* tarr = arr.v.typed_array;
                if(tarr->elem_type != VAL_UINT32) {
                    snprintf(errbuf, sizeof(errbuf), "类型错误：数组元素类型不匹配，期望 uint，实际为 %s", val_typename(tarr->elem_type));
                    runtime_error(errbuf);
                }
                if(!tarr->items) {
                    runtime_error("数组错误：uint 类型化数组 items 指针为空");
                }
                if(iidx < 0 || iidx >= tarr->len) {
                    snprintf(errbuf, sizeof(errbuf), "数组越界：索引 %d 超出范围 [0, %d)", iidx, tarr->len);
                    runtime_error(errbuf);
                }
                unsigned int val = ((unsigned int*)tarr->items)[iidx];  // 直接读取 uint 值，零提取零转换！
                UINT_PUSH(val);  // 压入 uint 栈，零包装！
                break;
            }
            case OPC_BOOL_ARRAY_LIT: {
                int n = in.b;
                if(in.a == 1) {
                    TypedArray* ta = (TypedArray*)gc_alloc(sizeof(TypedArray), VAL_TYPED_ARRAY);
                    ta->len = n;
                    ta->cap = n > 0 ? n : 1;
                    ta->elem_type = VAL_BOOL;
                    ta->items = gc_alloc((size_t)ta->cap * sizeof(_Bool), VAL_TYPED_ARRAY);
                    _Bool* bitems = (_Bool*)ta->items;
                    for(int k = 0; k < n; k++) {
                        bitems[k] = BOOL_POP();
                    }
                    Value ret;
                    ret.type = VAL_TYPED_ARRAY;
                    ret.v.typed_array = ta;
                    stack[sp++] = ret;
                } else {
                    Value* elems = &stack[sp - n];
                    TypedArray* ta = (TypedArray*)gc_alloc(sizeof(TypedArray), VAL_TYPED_ARRAY);
                    ta->len = n;
                    ta->cap = n > 0 ? n : 1;
                    ta->elem_type = VAL_BOOL;
                    ta->items = gc_alloc((size_t)ta->cap * sizeof(_Bool), VAL_TYPED_ARRAY);
                    _Bool* bitems = (_Bool*)ta->items;
                    for(int k = 0; k < n; k++) {
                        Value v = elems[k];
                        bitems[k] = (v.type == VAL_BOOL) ? (v.v.i ? 1 : 0) : (lumyr_cast_bool(v).v.i ? 1 : 0);
                    }
                    sp -= n;
                    Value ret;
                    ret.type = VAL_TYPED_ARRAY;
                    ret.v.typed_array = ta;
                    stack[sp++] = ret;
                }
                break;
            }
            case OPC_BOOL_ARRAY_GET: {
                Value idx = stack[--sp];
                Value arrv = stack[--sp];
                int bidx = (int)lumyr_extract_int(idx);
                char errbuf[256];
                if(arrv.type != VAL_TYPED_ARRAY || !arrv.v.typed_array) {
                    snprintf(errbuf, sizeof(errbuf), "类型错误：OPC_BOOL_ARRAY_GET 需要 bool 类型化数组，实际类型为 %s", val_typename(arrv.type));
                    runtime_error(errbuf);
                }
                TypedArray* tarr = arrv.v.typed_array;
                if(tarr->elem_type != VAL_BOOL) {
                    snprintf(errbuf, sizeof(errbuf), "类型错误：数组元素类型不匹配，期望 bool，实际为 %s", val_typename(tarr->elem_type));
                    runtime_error(errbuf);
                }
                if(bidx < 0 || bidx >= tarr->len) {
                    snprintf(errbuf, sizeof(errbuf), "数组越界：索引 %d 超出范围 [0, %d)", bidx, tarr->len);
                    runtime_error(errbuf);
                }
                _Bool val = ((_Bool*)tarr->items)[bidx];
                BOOL_PUSH(val);
                break;
            }
            case OPC_CHAR_ARRAY_LIT: {
                int n = in.b;
                if(in.a == 1) {
                    TypedArray* ta = (TypedArray*)gc_alloc(sizeof(TypedArray), VAL_TYPED_ARRAY);
                    ta->len = n;
                    ta->cap = n > 0 ? n : 1;
                    ta->elem_type = VAL_CHAR;
                    ta->items = gc_alloc((size_t)ta->cap * sizeof(char), VAL_TYPED_ARRAY);
                    char* citems = (char*)ta->items;
                    for(int k = 0; k < n; k++) {
                        citems[k] = CHAR_POP();
                    }
                    Value ret;
                    ret.type = VAL_TYPED_ARRAY;
                    ret.v.typed_array = ta;
                    stack[sp++] = ret;
                } else {
                    Value* elems = &stack[sp - n];
                    TypedArray* ta = (TypedArray*)gc_alloc(sizeof(TypedArray), VAL_TYPED_ARRAY);
                    ta->len = n;
                    ta->cap = n > 0 ? n : 1;
                    ta->elem_type = VAL_CHAR;
                    ta->items = gc_alloc((size_t)ta->cap * sizeof(char), VAL_TYPED_ARRAY);
                    char* citems = (char*)ta->items;
                    for(int k = 0; k < n; k++) {
                        Value v = elems[k];
                        citems[k] = (v.type == VAL_CHAR) ? (char)v.v.i : (char)lumyr_cast_char(v).v.i;
                    }
                    sp -= n;
                    Value ret;
                    ret.type = VAL_TYPED_ARRAY;
                    ret.v.typed_array = ta;
                    stack[sp++] = ret;
                }
                break;
            }
            case OPC_CHAR_ARRAY_GET: {
                Value idx = stack[--sp];
                Value arrv = stack[--sp];
                int cidx = (int)lumyr_extract_int(idx);
                char errbuf[256];
                if(arrv.type != VAL_TYPED_ARRAY || !arrv.v.typed_array) {
                    snprintf(errbuf, sizeof(errbuf), "类型错误：OPC_CHAR_ARRAY_GET 需要 char 类型化数组，实际类型为 %s", val_typename(arrv.type));
                    runtime_error(errbuf);
                }
                TypedArray* tarr = arrv.v.typed_array;
                if(tarr->elem_type != VAL_CHAR) {
                    snprintf(errbuf, sizeof(errbuf), "类型错误：数组元素类型不匹配，期望 char，实际为 %s", val_typename(tarr->elem_type));
                    runtime_error(errbuf);
                }
                if(cidx < 0 || cidx >= tarr->len) {
                    snprintf(errbuf, sizeof(errbuf), "数组越界：索引 %d 超出范围 [0, %d)", cidx, tarr->len);
                    runtime_error(errbuf);
                }
                char val = ((char*)tarr->items)[cidx];
                CHAR_PUSH(val);
                break;
            }
            case OPC_BYTE_ARRAY_LIT: {
                int n = in.b;
                if(in.a == 1) {
                    TypedArray* ta = (TypedArray*)gc_alloc(sizeof(TypedArray), VAL_TYPED_ARRAY);
                    ta->len = n;
                    ta->cap = n > 0 ? n : 1;
                    ta->elem_type = VAL_BYTE;
                    ta->items = gc_alloc((size_t)ta->cap * sizeof(unsigned char), VAL_TYPED_ARRAY);
                    unsigned char* byitems = (unsigned char*)ta->items;
                    for(int k = 0; k < n; k++) {
                        byitems[k] = BYTE_POP();
                    }
                    Value ret;
                    ret.type = VAL_TYPED_ARRAY;
                    ret.v.typed_array = ta;
                    stack[sp++] = ret;
                } else {
                    Value* elems = &stack[sp - n];
                    TypedArray* ta = (TypedArray*)gc_alloc(sizeof(TypedArray), VAL_TYPED_ARRAY);
                    ta->len = n;
                    ta->cap = n > 0 ? n : 1;
                    ta->elem_type = VAL_BYTE;
                    ta->items = gc_alloc((size_t)ta->cap * sizeof(unsigned char), VAL_TYPED_ARRAY);
                    unsigned char* byitems = (unsigned char*)ta->items;
                    for(int k = 0; k < n; k++) {
                        Value v = elems[k];
                        byitems[k] = (v.type == VAL_BYTE) ? (unsigned char)v.v.i : (unsigned char)lumyr_cast_byte(v).v.i;
                    }
                    sp -= n;
                    Value ret;
                    ret.type = VAL_TYPED_ARRAY;
                    ret.v.typed_array = ta;
                    stack[sp++] = ret;
                }
                break;
            }
            case OPC_BYTE_ARRAY_GET: {
                Value idx = stack[--sp];
                Value arrv = stack[--sp];
                int byidx = (int)lumyr_extract_int(idx);
                char errbuf[256];
                if(arrv.type != VAL_TYPED_ARRAY || !arrv.v.typed_array) {
                    snprintf(errbuf, sizeof(errbuf), "类型错误：OPC_BYTE_ARRAY_GET 需要 byte 类型化数组，实际类型为 %s", val_typename(arrv.type));
                    runtime_error(errbuf);
                }
                TypedArray* tarr = arrv.v.typed_array;
                if(tarr->elem_type != VAL_BYTE) {
                    snprintf(errbuf, sizeof(errbuf), "类型错误：数组元素类型不匹配，期望 byte，实际为 %s", val_typename(tarr->elem_type));
                    runtime_error(errbuf);
                }
                if(byidx < 0 || byidx >= tarr->len) {
                    snprintf(errbuf, sizeof(errbuf), "数组越界：索引 %d 超出范围 [0, %d)", byidx, tarr->len);
                    runtime_error(errbuf);
                }
                unsigned char val = ((unsigned char*)tarr->items)[byidx];
                BYTE_PUSH(val);
                break;
            }
            case OPC_INT8_ARRAY_LIT: {
                int n = in.b;
                if(in.a == 1) {
                    TypedArray* ta = (TypedArray*)gc_alloc(sizeof(TypedArray), VAL_TYPED_ARRAY);
                    ta->len = n;
                    ta->cap = n > 0 ? n : 1;
                    ta->elem_type = VAL_INT8;
                    ta->items = gc_alloc((size_t)ta->cap * sizeof(int8_t), VAL_TYPED_ARRAY);
                    int8_t* i8items = (int8_t*)ta->items;
                    for(int k = 0; k < n; k++) {
                        i8items[k] = INT8_POP();
                    }
                    Value ret;
                    ret.type = VAL_TYPED_ARRAY;
                    ret.v.typed_array = ta;
                    stack[sp++] = ret;
                } else {
                    Value* elems = &stack[sp - n];
                    TypedArray* ta = (TypedArray*)gc_alloc(sizeof(TypedArray), VAL_TYPED_ARRAY);
                    ta->len = n;
                    ta->cap = n > 0 ? n : 1;
                    ta->elem_type = VAL_INT8;
                    ta->items = gc_alloc((size_t)ta->cap * sizeof(int8_t), VAL_TYPED_ARRAY);
                    int8_t* i8items = (int8_t*)ta->items;
                    for(int k = 0; k < n; k++) {
                        Value v = elems[k];
                        i8items[k] = (v.type == VAL_INT8) ? (int8_t)v.v.i : (int8_t)lumyr_cast_int(v).v.i;
                    }
                    sp -= n;
                    Value ret;
                    ret.type = VAL_TYPED_ARRAY;
                    ret.v.typed_array = ta;
                    stack[sp++] = ret;
                }
                break;
            }
            case OPC_INT8_ARRAY_GET: {
                Value idx = stack[--sp];
                Value arrv = stack[--sp];
                int i8idx = (int)lumyr_extract_int(idx);
                char errbuf[256];
                if(arrv.type != VAL_TYPED_ARRAY || !arrv.v.typed_array) {
                    snprintf(errbuf, sizeof(errbuf), "类型错误：OPC_INT8_ARRAY_GET 需要 int8 类型化数组，实际类型为 %s", val_typename(arrv.type));
                    runtime_error(errbuf);
                }
                TypedArray* tarr = arrv.v.typed_array;
                if(tarr->elem_type != VAL_INT8) {
                    snprintf(errbuf, sizeof(errbuf), "类型错误：数组元素类型不匹配，期望 int8，实际为 %s", val_typename(tarr->elem_type));
                    runtime_error(errbuf);
                }
                if(i8idx < 0 || i8idx >= tarr->len) {
                    snprintf(errbuf, sizeof(errbuf), "数组越界：索引 %d 超出范围 [0, %d)", i8idx, tarr->len);
                    runtime_error(errbuf);
                }
                int8_t val = ((int8_t*)tarr->items)[i8idx];
                INT8_PUSH(val);
                break;
            }
            case OPC_INT16_ARRAY_LIT: {
                int n = in.b;
                if(in.a == 1) {
                    TypedArray* ta = (TypedArray*)gc_alloc(sizeof(TypedArray), VAL_TYPED_ARRAY);
                    ta->len = n;
                    ta->cap = n > 0 ? n : 1;
                    ta->elem_type = VAL_INT16;
                    ta->items = gc_alloc((size_t)ta->cap * sizeof(int16_t), VAL_TYPED_ARRAY);
                    int16_t* i16items = (int16_t*)ta->items;
                    for(int k = 0; k < n; k++) {
                        i16items[k] = INT16_POP();
                    }
                    Value ret;
                    ret.type = VAL_TYPED_ARRAY;
                    ret.v.typed_array = ta;
                    stack[sp++] = ret;
                } else {
                    Value* elems = &stack[sp - n];
                    TypedArray* ta = (TypedArray*)gc_alloc(sizeof(TypedArray), VAL_TYPED_ARRAY);
                    ta->len = n;
                    ta->cap = n > 0 ? n : 1;
                    ta->elem_type = VAL_INT16;
                    ta->items = gc_alloc((size_t)ta->cap * sizeof(int16_t), VAL_TYPED_ARRAY);
                    int16_t* i16items = (int16_t*)ta->items;
                    for(int k = 0; k < n; k++) {
                        Value v = elems[k];
                        i16items[k] = (v.type == VAL_INT16) ? (int16_t)v.v.i : (int16_t)lumyr_cast_int(v).v.i;
                    }
                    sp -= n;
                    Value ret;
                    ret.type = VAL_TYPED_ARRAY;
                    ret.v.typed_array = ta;
                    stack[sp++] = ret;
                }
                break;
            }
            case OPC_INT16_ARRAY_GET: {
                Value idx = stack[--sp];
                Value arrv = stack[--sp];
                int i16idx = (int)lumyr_extract_int(idx);
                char errbuf[256];
                if(arrv.type != VAL_TYPED_ARRAY || !arrv.v.typed_array) {
                    snprintf(errbuf, sizeof(errbuf), "类型错误：OPC_INT16_ARRAY_GET 需要 int16 类型化数组，实际类型为 %s", val_typename(arrv.type));
                    runtime_error(errbuf);
                }
                TypedArray* tarr = arrv.v.typed_array;
                if(tarr->elem_type != VAL_INT16) {
                    snprintf(errbuf, sizeof(errbuf), "类型错误：数组元素类型不匹配，期望 int16，实际为 %s", val_typename(tarr->elem_type));
                    runtime_error(errbuf);
                }
                if(i16idx < 0 || i16idx >= tarr->len) {
                    snprintf(errbuf, sizeof(errbuf), "数组越界：索引 %d 超出范围 [0, %d)", i16idx, tarr->len);
                    runtime_error(errbuf);
                }
                int16_t val = ((int16_t*)tarr->items)[i16idx];
                INT16_PUSH(val);
                break;
            }
            case OPC_INT32_ARRAY_LIT: {
                int n = in.b;
                if(in.a == 1) {
                    TypedArray* ta = (TypedArray*)gc_alloc(sizeof(TypedArray), VAL_TYPED_ARRAY);
                    ta->len = n;
                    ta->cap = n > 0 ? n : 1;
                    ta->elem_type = VAL_INT32;
                    ta->items = gc_alloc((size_t)ta->cap * sizeof(int32_t), VAL_TYPED_ARRAY);
                    int32_t* i32items = (int32_t*)ta->items;
                    for(int k = 0; k < n; k++) {
                        i32items[k] = INT32_POP();
                    }
                    Value ret;
                    ret.type = VAL_TYPED_ARRAY;
                    ret.v.typed_array = ta;
                    stack[sp++] = ret;
                } else {
                    Value* elems = &stack[sp - n];
                    TypedArray* ta = (TypedArray*)gc_alloc(sizeof(TypedArray), VAL_TYPED_ARRAY);
                    ta->len = n;
                    ta->cap = n > 0 ? n : 1;
                    ta->elem_type = VAL_INT32;
                    ta->items = gc_alloc((size_t)ta->cap * sizeof(int32_t), VAL_TYPED_ARRAY);
                    int32_t* i32items = (int32_t*)ta->items;
                    for(int k = 0; k < n; k++) {
                        Value v = elems[k];
                        i32items[k] = (v.type == VAL_INT32) ? (int32_t)v.v.i : (int32_t)lumyr_cast_int(v).v.i;
                    }
                    sp -= n;
                    Value ret;
                    ret.type = VAL_TYPED_ARRAY;
                    ret.v.typed_array = ta;
                    stack[sp++] = ret;
                }
                break;
            }
            case OPC_INT32_ARRAY_GET: {
                Value idx = stack[--sp];
                Value arrv = stack[--sp];
                int i32idx = (int)lumyr_extract_int(idx);
                char errbuf[256];
                if(arrv.type != VAL_TYPED_ARRAY || !arrv.v.typed_array) {
                    snprintf(errbuf, sizeof(errbuf), "类型错误：OPC_INT32_ARRAY_GET 需要 int32 类型化数组，实际类型为 %s", val_typename(arrv.type));
                    runtime_error(errbuf);
                }
                TypedArray* tarr = arrv.v.typed_array;
                if(tarr->elem_type != VAL_INT32) {
                    snprintf(errbuf, sizeof(errbuf), "类型错误：数组元素类型不匹配，期望 int32，实际为 %s", val_typename(tarr->elem_type));
                    runtime_error(errbuf);
                }
                if(i32idx < 0 || i32idx >= tarr->len) {
                    snprintf(errbuf, sizeof(errbuf), "数组越界：索引 %d 超出范围 [0, %d)", i32idx, tarr->len);
                    runtime_error(errbuf);
                }
                int32_t val = ((int32_t*)tarr->items)[i32idx];
                INT32_PUSH(val);
                break;
            }
            case OPC_INT64_ARRAY_LIT: {
                int n = in.b;
                if(in.a == 1) {
                    TypedArray* ta = (TypedArray*)gc_alloc(sizeof(TypedArray), VAL_TYPED_ARRAY);
                    ta->len = n;
                    ta->cap = n > 0 ? n : 1;
                    ta->elem_type = VAL_INT64;
                    ta->items = gc_alloc((size_t)ta->cap * sizeof(int64_t), VAL_TYPED_ARRAY);
                    int64_t* i64items = (int64_t*)ta->items;
                    for(int k = 0; k < n; k++) {
                        i64items[k] = INT64_POP();
                    }
                    Value ret;
                    ret.type = VAL_TYPED_ARRAY;
                    ret.v.typed_array = ta;
                    stack[sp++] = ret;
                } else {
                    Value* elems = &stack[sp - n];
                    TypedArray* ta = (TypedArray*)gc_alloc(sizeof(TypedArray), VAL_TYPED_ARRAY);
                    ta->len = n;
                    ta->cap = n > 0 ? n : 1;
                    ta->elem_type = VAL_INT64;
                    ta->items = gc_alloc((size_t)ta->cap * sizeof(int64_t), VAL_TYPED_ARRAY);
                    int64_t* i64items = (int64_t*)ta->items;
                    for(int k = 0; k < n; k++) {
                        Value v = elems[k];
                        i64items[k] = (v.type == VAL_INT64) ? (int64_t)v.v.i : (int64_t)lumyr_cast_int(v).v.i;
                    }
                    sp -= n;
                    Value ret;
                    ret.type = VAL_TYPED_ARRAY;
                    ret.v.typed_array = ta;
                    stack[sp++] = ret;
                }
                break;
            }
            case OPC_INT64_ARRAY_GET: {
                Value idx = stack[--sp];
                Value arrv = stack[--sp];
                int i64idx = (int)lumyr_extract_int(idx);
                char errbuf[256];
                if(arrv.type != VAL_TYPED_ARRAY || !arrv.v.typed_array) {
                    snprintf(errbuf, sizeof(errbuf), "类型错误：OPC_INT64_ARRAY_GET 需要 int64 类型化数组，实际类型为 %s", val_typename(arrv.type));
                    runtime_error(errbuf);
                }
                TypedArray* tarr = arrv.v.typed_array;
                if(tarr->elem_type != VAL_INT64) {
                    snprintf(errbuf, sizeof(errbuf), "类型错误：数组元素类型不匹配，期望 int64，实际为 %s", val_typename(tarr->elem_type));
                    runtime_error(errbuf);
                }
                if(i64idx < 0 || i64idx >= tarr->len) {
                    snprintf(errbuf, sizeof(errbuf), "数组越界：索引 %d 超出范围 [0, %d)", i64idx, tarr->len);
                    runtime_error(errbuf);
                }
                int64_t val = ((int64_t*)tarr->items)[i64idx];
                INT64_PUSH(val);
                break;
            }
            case OPC_UINT8_ARRAY_LIT: {
                int n = in.b;
                if(in.a == 1) {
                    TypedArray* ta = (TypedArray*)gc_alloc(sizeof(TypedArray), VAL_TYPED_ARRAY);
                    ta->len = n;
                    ta->cap = n > 0 ? n : 1;
                    ta->elem_type = VAL_UINT8;
                    ta->items = gc_alloc((size_t)ta->cap * sizeof(uint8_t), VAL_TYPED_ARRAY);
                    uint8_t* u8items = (uint8_t*)ta->items;
                    for(int k = 0; k < n; k++) {
                        u8items[k] = UINT8_POP();
                    }
                    Value ret;
                    ret.type = VAL_TYPED_ARRAY;
                    ret.v.typed_array = ta;
                    stack[sp++] = ret;
                } else {
                    Value* elems = &stack[sp - n];
                    TypedArray* ta = (TypedArray*)gc_alloc(sizeof(TypedArray), VAL_TYPED_ARRAY);
                    ta->len = n;
                    ta->cap = n > 0 ? n : 1;
                    ta->elem_type = VAL_UINT8;
                    ta->items = gc_alloc((size_t)ta->cap * sizeof(uint8_t), VAL_TYPED_ARRAY);
                    uint8_t* u8items = (uint8_t*)ta->items;
                    for(int k = 0; k < n; k++) {
                        Value v = elems[k];
                        u8items[k] = (v.type == VAL_UINT8) ? (uint8_t)v.v.i : (uint8_t)lumyr_cast_int(v).v.i;
                    }
                    sp -= n;
                    Value ret;
                    ret.type = VAL_TYPED_ARRAY;
                    ret.v.typed_array = ta;
                    stack[sp++] = ret;
                }
                break;
            }
            case OPC_UINT8_ARRAY_GET: {
                Value idx = stack[--sp];
                Value arrv = stack[--sp];
                int u8idx = (int)lumyr_extract_int(idx);
                char errbuf[256];
                if(arrv.type != VAL_TYPED_ARRAY || !arrv.v.typed_array) {
                    snprintf(errbuf, sizeof(errbuf), "类型错误：OPC_UINT8_ARRAY_GET 需要 uint8 类型化数组，实际类型为 %s", val_typename(arrv.type));
                    runtime_error(errbuf);
                }
                TypedArray* tarr = arrv.v.typed_array;
                if(tarr->elem_type != VAL_UINT8) {
                    snprintf(errbuf, sizeof(errbuf), "类型错误：数组元素类型不匹配，期望 uint8，实际为 %s", val_typename(tarr->elem_type));
                    runtime_error(errbuf);
                }
                if(u8idx < 0 || u8idx >= tarr->len) {
                    snprintf(errbuf, sizeof(errbuf), "数组越界：索引 %d 超出范围 [0, %d)", u8idx, tarr->len);
                    runtime_error(errbuf);
                }
                uint8_t val = ((uint8_t*)tarr->items)[u8idx];
                UINT8_PUSH(val);
                break;
            }
            case OPC_UINT16_ARRAY_LIT: {
                int n = in.b;
                if(in.a == 1) {
                    TypedArray* ta = (TypedArray*)gc_alloc(sizeof(TypedArray), VAL_TYPED_ARRAY);
                    ta->len = n;
                    ta->cap = n > 0 ? n : 1;
                    ta->elem_type = VAL_UINT16;
                    ta->items = gc_alloc((size_t)ta->cap * sizeof(uint16_t), VAL_TYPED_ARRAY);
                    uint16_t* u16items = (uint16_t*)ta->items;
                    for(int k = 0; k < n; k++) {
                        u16items[k] = UINT16_POP();
                    }
                    Value ret;
                    ret.type = VAL_TYPED_ARRAY;
                    ret.v.typed_array = ta;
                    stack[sp++] = ret;
                } else {
                    Value* elems = &stack[sp - n];
                    TypedArray* ta = (TypedArray*)gc_alloc(sizeof(TypedArray), VAL_TYPED_ARRAY);
                    ta->len = n;
                    ta->cap = n > 0 ? n : 1;
                    ta->elem_type = VAL_UINT16;
                    ta->items = gc_alloc((size_t)ta->cap * sizeof(uint16_t), VAL_TYPED_ARRAY);
                    uint16_t* u16items = (uint16_t*)ta->items;
                    for(int k = 0; k < n; k++) {
                        Value v = elems[k];
                        u16items[k] = (v.type == VAL_UINT16) ? (uint16_t)v.v.i : (uint16_t)lumyr_cast_int(v).v.i;
                    }
                    sp -= n;
                    Value ret;
                    ret.type = VAL_TYPED_ARRAY;
                    ret.v.typed_array = ta;
                    stack[sp++] = ret;
                }
                break;
            }
            case OPC_UINT16_ARRAY_GET: {
                Value idx = stack[--sp];
                Value arrv = stack[--sp];
                int u16idx = (int)lumyr_extract_int(idx);
                char errbuf[256];
                if(arrv.type != VAL_TYPED_ARRAY || !arrv.v.typed_array) {
                    snprintf(errbuf, sizeof(errbuf), "类型错误：OPC_UINT16_ARRAY_GET 需要 uint16 类型化数组，实际类型为 %s", val_typename(arrv.type));
                    runtime_error(errbuf);
                }
                TypedArray* tarr = arrv.v.typed_array;
                if(tarr->elem_type != VAL_UINT16) {
                    snprintf(errbuf, sizeof(errbuf), "类型错误：数组元素类型不匹配，期望 uint16，实际为 %s", val_typename(tarr->elem_type));
                    runtime_error(errbuf);
                }
                if(u16idx < 0 || u16idx >= tarr->len) {
                    snprintf(errbuf, sizeof(errbuf), "数组越界：索引 %d 超出范围 [0, %d)", u16idx, tarr->len);
                    runtime_error(errbuf);
                }
                uint16_t val = ((uint16_t*)tarr->items)[u16idx];
                UINT16_PUSH(val);
                break;
            }
            case OPC_UINT32_ARRAY_LIT: {
                int n = in.b;
                if(in.a == 1) {
                    TypedArray* ta = (TypedArray*)gc_alloc(sizeof(TypedArray), VAL_TYPED_ARRAY);
                    ta->len = n;
                    ta->cap = n > 0 ? n : 1;
                    ta->elem_type = VAL_UINT32;
                    ta->items = gc_alloc((size_t)ta->cap * sizeof(uint32_t), VAL_TYPED_ARRAY);
                    uint32_t* u32items = (uint32_t*)ta->items;
                    for(int k = 0; k < n; k++) {
                        u32items[k] = UINT32_POP();
                    }
                    Value ret;
                    ret.type = VAL_TYPED_ARRAY;
                    ret.v.typed_array = ta;
                    stack[sp++] = ret;
                } else {
                    Value* elems = &stack[sp - n];
                    TypedArray* ta = (TypedArray*)gc_alloc(sizeof(TypedArray), VAL_TYPED_ARRAY);
                    ta->len = n;
                    ta->cap = n > 0 ? n : 1;
                    ta->elem_type = VAL_UINT32;
                    ta->items = gc_alloc((size_t)ta->cap * sizeof(uint32_t), VAL_TYPED_ARRAY);
                    uint32_t* u32items = (uint32_t*)ta->items;
                    for(int k = 0; k < n; k++) {
                        Value v = elems[k];
                        u32items[k] = (v.type == VAL_UINT32) ? (uint32_t)v.v.i : (uint32_t)lumyr_cast_int(v).v.i;
                    }
                    sp -= n;
                    Value ret;
                    ret.type = VAL_TYPED_ARRAY;
                    ret.v.typed_array = ta;
                    stack[sp++] = ret;
                }
                break;
            }
            case OPC_UINT32_ARRAY_GET: {
                Value idx = stack[--sp];
                Value arrv = stack[--sp];
                int u32idx = (int)lumyr_extract_int(idx);
                char errbuf[256];
                if(arrv.type != VAL_TYPED_ARRAY || !arrv.v.typed_array) {
                    snprintf(errbuf, sizeof(errbuf), "类型错误：OPC_UINT32_ARRAY_GET 需要 uint32 类型化数组，实际类型为 %s", val_typename(arrv.type));
                    runtime_error(errbuf);
                }
                TypedArray* tarr = arrv.v.typed_array;
                if(tarr->elem_type != VAL_UINT32) {
                    snprintf(errbuf, sizeof(errbuf), "类型错误：数组元素类型不匹配，期望 uint32，实际为 %s", val_typename(tarr->elem_type));
                    runtime_error(errbuf);
                }
                if(u32idx < 0 || u32idx >= tarr->len) {
                    snprintf(errbuf, sizeof(errbuf), "数组越界：索引 %d 超出范围 [0, %d)", u32idx, tarr->len);
                    runtime_error(errbuf);
                }
                uint32_t val = ((uint32_t*)tarr->items)[u32idx];
                UINT32_PUSH(val);
                break;
            }
            case OPC_UINT64_ARRAY_LIT: {
                int n = in.b;
                if(in.a == 1) {
                    TypedArray* ta = (TypedArray*)gc_alloc(sizeof(TypedArray), VAL_TYPED_ARRAY);
                    ta->len = n;
                    ta->cap = n > 0 ? n : 1;
                    ta->elem_type = VAL_UINT64;
                    ta->items = gc_alloc((size_t)ta->cap * sizeof(uint64_t), VAL_TYPED_ARRAY);
                    uint64_t* u64items = (uint64_t*)ta->items;
                    for(int k = 0; k < n; k++) {
                        u64items[k] = UINT64_POP();
                    }
                    Value ret;
                    ret.type = VAL_TYPED_ARRAY;
                    ret.v.typed_array = ta;
                    stack[sp++] = ret;
                } else {
                    Value* elems = &stack[sp - n];
                    TypedArray* ta = (TypedArray*)gc_alloc(sizeof(TypedArray), VAL_TYPED_ARRAY);
                    ta->len = n;
                    ta->cap = n > 0 ? n : 1;
                    ta->elem_type = VAL_UINT64;
                    ta->items = gc_alloc((size_t)ta->cap * sizeof(uint64_t), VAL_TYPED_ARRAY);
                    uint64_t* u64items = (uint64_t*)ta->items;
                    for(int k = 0; k < n; k++) {
                        Value v = elems[k];
                        u64items[k] = (v.type == VAL_UINT64) ? (uint64_t)v.v.i : (uint64_t)lumyr_cast_int(v).v.i;
                    }
                    sp -= n;
                    Value ret;
                    ret.type = VAL_TYPED_ARRAY;
                    ret.v.typed_array = ta;
                    stack[sp++] = ret;
                }
                break;
            }
            case OPC_UINT64_ARRAY_GET: {
                Value idx = stack[--sp];
                Value arrv = stack[--sp];
                int u64idx = (int)lumyr_extract_int(idx);
                char errbuf[256];
                if(arrv.type != VAL_TYPED_ARRAY || !arrv.v.typed_array) {
                    snprintf(errbuf, sizeof(errbuf), "类型错误：OPC_UINT64_ARRAY_GET 需要 uint64 类型化数组，实际类型为 %s", val_typename(arrv.type));
                    runtime_error(errbuf);
                }
                TypedArray* tarr = arrv.v.typed_array;
                if(tarr->elem_type != VAL_UINT64) {
                    snprintf(errbuf, sizeof(errbuf), "类型错误：数组元素类型不匹配，期望 uint64，实际为 %s", val_typename(tarr->elem_type));
                    runtime_error(errbuf);
                }
                if(u64idx < 0 || u64idx >= tarr->len) {
                    snprintf(errbuf, sizeof(errbuf), "数组越界：索引 %d 超出范围 [0, %d)", u64idx, tarr->len);
                    runtime_error(errbuf);
                }
                uint64_t val = ((uint64_t*)tarr->items)[u64idx];
                UINT64_PUSH(val);
                break;
            }
            case OPC_LONG_ARRAY_LIT: {
                int n = in.b;
                if(in.a == 1) {
                    TypedArray* ta = (TypedArray*)gc_alloc(sizeof(TypedArray), VAL_TYPED_ARRAY);
                    ta->len = n;
                    ta->cap = n > 0 ? n : 1;
                    ta->elem_type = VAL_LONG;
                    ta->items = gc_alloc((size_t)ta->cap * sizeof(long), VAL_TYPED_ARRAY);
                    long* litems = (long*)ta->items;
                    for(int k = 0; k < n; k++) {
                        litems[k] = LONG_POP();
                    }
                    Value ret;
                    ret.type = VAL_TYPED_ARRAY;
                    ret.v.typed_array = ta;
                    stack[sp++] = ret;
                } else {
                    Value* elems = &stack[sp - n];
                    TypedArray* ta = (TypedArray*)gc_alloc(sizeof(TypedArray), VAL_TYPED_ARRAY);
                    ta->len = n;
                    ta->cap = n > 0 ? n : 1;
                    ta->elem_type = VAL_LONG;
                    ta->items = gc_alloc((size_t)ta->cap * sizeof(long), VAL_TYPED_ARRAY);
                    long* litems = (long*)ta->items;
                    for(int k = 0; k < n; k++) {
                        Value v = elems[k];
                        litems[k] = (v.type == VAL_LONG) ? (long)v.v.i : (long)lumyr_cast_int(v).v.i;
                    }
                    sp -= n;
                    Value ret;
                    ret.type = VAL_TYPED_ARRAY;
                    ret.v.typed_array = ta;
                    stack[sp++] = ret;
                }
                break;
            }
            case OPC_LONG_ARRAY_GET: {
                Value idx = stack[--sp];
                Value arrv = stack[--sp];
                int lidx = (int)lumyr_extract_int(idx);
                char errbuf[256];
                if(arrv.type != VAL_TYPED_ARRAY || !arrv.v.typed_array) {
                    snprintf(errbuf, sizeof(errbuf), "类型错误：OPC_LONG_ARRAY_GET 需要 long 类型化数组，实际类型为 %s", val_typename(arrv.type));
                    runtime_error(errbuf);
                }
                TypedArray* tarr = arrv.v.typed_array;
                if(tarr->elem_type != VAL_LONG) {
                    snprintf(errbuf, sizeof(errbuf), "类型错误：数组元素类型不匹配，期望 long，实际为 %s", val_typename(tarr->elem_type));
                    runtime_error(errbuf);
                }
                if(lidx < 0 || lidx >= tarr->len) {
                    snprintf(errbuf, sizeof(errbuf), "数组越界：索引 %d 超出范围 [0, %d)", lidx, tarr->len);
                    runtime_error(errbuf);
                }
                long val = ((long*)tarr->items)[lidx];
                LONG_PUSH(val);
                break;
            }
            case OPC_ULONG_ARRAY_LIT: {
                int n = in.b;
                if(in.a == 1) {
                    TypedArray* ta = (TypedArray*)gc_alloc(sizeof(TypedArray), VAL_TYPED_ARRAY);
                    ta->len = n;
                    ta->cap = n > 0 ? n : 1;
                    ta->elem_type = VAL_ULONG;
                    ta->items = gc_alloc((size_t)ta->cap * sizeof(unsigned long), VAL_TYPED_ARRAY);
                    unsigned long* ulitems = (unsigned long*)ta->items;
                    for(int k = 0; k < n; k++) {
                        ulitems[k] = ULONG_POP();
                    }
                    Value ret;
                    ret.type = VAL_TYPED_ARRAY;
                    ret.v.typed_array = ta;
                    stack[sp++] = ret;
                } else {
                    Value* elems = &stack[sp - n];
                    TypedArray* ta = (TypedArray*)gc_alloc(sizeof(TypedArray), VAL_TYPED_ARRAY);
                    ta->len = n;
                    ta->cap = n > 0 ? n : 1;
                    ta->elem_type = VAL_ULONG;
                    ta->items = gc_alloc((size_t)ta->cap * sizeof(unsigned long), VAL_TYPED_ARRAY);
                    unsigned long* ulitems = (unsigned long*)ta->items;
                    for(int k = 0; k < n; k++) {
                        Value v = elems[k];
                        ulitems[k] = (v.type == VAL_ULONG) ? (unsigned long)v.v.i : (unsigned long)lumyr_cast_int(v).v.i;
                    }
                    sp -= n;
                    Value ret;
                    ret.type = VAL_TYPED_ARRAY;
                    ret.v.typed_array = ta;
                    stack[sp++] = ret;
                }
                break;
            }
            case OPC_ULONG_ARRAY_GET: {
                Value idx = stack[--sp];
                Value arrv = stack[--sp];
                int ulidx = (int)lumyr_extract_int(idx);
                char errbuf[256];
                if(arrv.type != VAL_TYPED_ARRAY || !arrv.v.typed_array) {
                    snprintf(errbuf, sizeof(errbuf), "类型错误：OPC_ULONG_ARRAY_GET 需要 unsigned long 类型化数组，实际类型为 %s", val_typename(arrv.type));
                    runtime_error(errbuf);
                }
                TypedArray* tarr = arrv.v.typed_array;
                if(tarr->elem_type != VAL_ULONG) {
                    snprintf(errbuf, sizeof(errbuf), "类型错误：数组元素类型不匹配，期望 unsigned long，实际为 %s", val_typename(tarr->elem_type));
                    runtime_error(errbuf);
                }
                if(ulidx < 0 || ulidx >= tarr->len) {
                    snprintf(errbuf, sizeof(errbuf), "数组越界：索引 %d 超出范围 [0, %d)", ulidx, tarr->len);
                    runtime_error(errbuf);
                }
                unsigned long val = ((unsigned long*)tarr->items)[ulidx];
                ULONG_PUSH(val);
                break;
            }
            case OPC_SIZE_T_ARRAY_LIT: {
                int n = in.b;
                if(in.a == 1) {
                    TypedArray* ta = (TypedArray*)gc_alloc(sizeof(TypedArray), VAL_TYPED_ARRAY);
                    ta->len = n;
                    ta->cap = n > 0 ? n : 1;
                    ta->elem_type = VAL_SIZE_T;
                    ta->items = gc_alloc((size_t)ta->cap * sizeof(size_t), VAL_TYPED_ARRAY);
                    size_t* stitems = (size_t*)ta->items;
                    for(int k = 0; k < n; k++) {
                        stitems[k] = SIZE_T_POP();
                    }
                    Value ret;
                    ret.type = VAL_TYPED_ARRAY;
                    ret.v.typed_array = ta;
                    stack[sp++] = ret;
                } else {
                    Value* elems = &stack[sp - n];
                    TypedArray* ta = (TypedArray*)gc_alloc(sizeof(TypedArray), VAL_TYPED_ARRAY);
                    ta->len = n;
                    ta->cap = n > 0 ? n : 1;
                    ta->elem_type = VAL_SIZE_T;
                    ta->items = gc_alloc((size_t)ta->cap * sizeof(size_t), VAL_TYPED_ARRAY);
                    size_t* stitems = (size_t*)ta->items;
                    for(int k = 0; k < n; k++) {
                        Value v = elems[k];
                        stitems[k] = (v.type == VAL_SIZE_T) ? (size_t)v.v.i : (size_t)lumyr_cast_int(v).v.i;
                    }
                    sp -= n;
                    Value ret;
                    ret.type = VAL_TYPED_ARRAY;
                    ret.v.typed_array = ta;
                    stack[sp++] = ret;
                }
                break;
            }
            case OPC_SIZE_T_ARRAY_GET: {
                Value idx = stack[--sp];
                Value arrv = stack[--sp];
                int stidx = (int)lumyr_extract_int(idx);
                char errbuf[256];
                if(arrv.type != VAL_TYPED_ARRAY || !arrv.v.typed_array) {
                    snprintf(errbuf, sizeof(errbuf), "类型错误：OPC_SIZE_T_ARRAY_GET 需要 size_t 类型化数组，实际类型为 %s", val_typename(arrv.type));
                    runtime_error(errbuf);
                }
                TypedArray* tarr = arrv.v.typed_array;
                if(tarr->elem_type != VAL_SIZE_T) {
                    snprintf(errbuf, sizeof(errbuf), "类型错误：数组元素类型不匹配，期望 size_t，实际为 %s", val_typename(tarr->elem_type));
                    runtime_error(errbuf);
                }
                if(stidx < 0 || stidx >= tarr->len) {
                    snprintf(errbuf, sizeof(errbuf), "数组越界：索引 %d 超出范围 [0, %d)", stidx, tarr->len);
                    runtime_error(errbuf);
                }
                size_t val = ((size_t*)tarr->items)[stidx];
                SIZE_T_PUSH(val);
                break;
            }
            case OPC_SSIZE_T_ARRAY_LIT: {
                int n = in.b;
                if(in.a == 1) {
                    TypedArray* ta = (TypedArray*)gc_alloc(sizeof(TypedArray), VAL_TYPED_ARRAY);
                    ta->len = n;
                    ta->cap = n > 0 ? n : 1;
                    ta->elem_type = VAL_SSIZE_T;
                    ta->items = gc_alloc((size_t)ta->cap * sizeof(ssize_t), VAL_TYPED_ARRAY);
                    ssize_t* sstitems = (ssize_t*)ta->items;
                    for(int k = 0; k < n; k++) {
                        sstitems[k] = SSIZE_T_POP();
                    }
                    Value ret;
                    ret.type = VAL_TYPED_ARRAY;
                    ret.v.typed_array = ta;
                    stack[sp++] = ret;
                } else {
                    Value* elems = &stack[sp - n];
                    TypedArray* ta = (TypedArray*)gc_alloc(sizeof(TypedArray), VAL_TYPED_ARRAY);
                    ta->len = n;
                    ta->cap = n > 0 ? n : 1;
                    ta->elem_type = VAL_SSIZE_T;
                    ta->items = gc_alloc((size_t)ta->cap * sizeof(ssize_t), VAL_TYPED_ARRAY);
                    ssize_t* sstitems = (ssize_t*)ta->items;
                    for(int k = 0; k < n; k++) {
                        Value v = elems[k];
                        sstitems[k] = (v.type == VAL_SSIZE_T) ? (ssize_t)v.v.i : (ssize_t)lumyr_cast_int(v).v.i;
                    }
                    sp -= n;
                    Value ret;
                    ret.type = VAL_TYPED_ARRAY;
                    ret.v.typed_array = ta;
                    stack[sp++] = ret;
                }
                break;
            }
            case OPC_SSIZE_T_ARRAY_GET: {
                Value idx = stack[--sp];
                Value arrv = stack[--sp];
                int sstidx = (int)lumyr_extract_int(idx);
                char errbuf[256];
                if(arrv.type != VAL_TYPED_ARRAY || !arrv.v.typed_array) {
                    snprintf(errbuf, sizeof(errbuf), "类型错误：OPC_SSIZE_T_ARRAY_GET 需要 ssize_t 类型化数组，实际类型为 %s", val_typename(arrv.type));
                    runtime_error(errbuf);
                }
                TypedArray* tarr = arrv.v.typed_array;
                if(tarr->elem_type != VAL_SSIZE_T) {
                    snprintf(errbuf, sizeof(errbuf), "类型错误：数组元素类型不匹配，期望 ssize_t，实际为 %s", val_typename(tarr->elem_type));
                    runtime_error(errbuf);
                }
                if(sstidx < 0 || sstidx >= tarr->len) {
                    snprintf(errbuf, sizeof(errbuf), "数组越界：索引 %d 超出范围 [0, %d)", sstidx, tarr->len);
                    runtime_error(errbuf);
                }
                ssize_t val = ((ssize_t*)tarr->items)[sstidx];
                SSIZE_T_PUSH(val);
                break;
            }
            case OPC_LONG_DOUBLE_ARRAY_LIT: {
                int n = in.b;
                if(in.a == 1) {
                    TypedArray* ta = (TypedArray*)gc_alloc(sizeof(TypedArray), VAL_TYPED_ARRAY);
                    ta->len = n;
                    ta->cap = n > 0 ? n : 1;
                    ta->elem_type = VAL_LONG_DOUBLE;
                    ta->items = gc_alloc((size_t)ta->cap * sizeof(long double), VAL_TYPED_ARRAY);
                    long double* lditems = (long double*)ta->items;
                    for(int k = 0; k < n; k++) {
                        lditems[k] = LONG_DOUBLE_POP();
                    }
                    Value ret;
                    ret.type = VAL_TYPED_ARRAY;
                    ret.v.typed_array = ta;
                    stack[sp++] = ret;
                } else {
                    Value* elems = &stack[sp - n];
                    TypedArray* ta = (TypedArray*)gc_alloc(sizeof(TypedArray), VAL_TYPED_ARRAY);
                    ta->len = n;
                    ta->cap = n > 0 ? n : 1;
                    ta->elem_type = VAL_LONG_DOUBLE;
                    ta->items = gc_alloc((size_t)ta->cap * sizeof(long double), VAL_TYPED_ARRAY);
                    long double* lditems = (long double*)ta->items;
                    for(int k = 0; k < n; k++) {
                        Value v = elems[k];
                        lditems[k] = (v.type == VAL_LONG_DOUBLE) ? (long double)v.v.d : (long double)lumyr_cast_double(v).v.d;
                    }
                    sp -= n;
                    Value ret;
                    ret.type = VAL_TYPED_ARRAY;
                    ret.v.typed_array = ta;
                    stack[sp++] = ret;
                }
                break;
            }
            case OPC_LONG_DOUBLE_ARRAY_GET: {
                Value idx = stack[--sp];
                Value arrv = stack[--sp];
                int ldidx = (int)lumyr_extract_int(idx);
                char errbuf[256];
                if(arrv.type != VAL_TYPED_ARRAY || !arrv.v.typed_array) {
                    snprintf(errbuf, sizeof(errbuf), "类型错误：OPC_LONG_DOUBLE_ARRAY_GET 需要 long double 类型化数组，实际类型为 %s", val_typename(arrv.type));
                    runtime_error(errbuf);
                }
                TypedArray* tarr = arrv.v.typed_array;
                if(tarr->elem_type != VAL_LONG_DOUBLE) {
                    snprintf(errbuf, sizeof(errbuf), "类型错误：数组元素类型不匹配，期望 long double，实际为 %s", val_typename(tarr->elem_type));
                    runtime_error(errbuf);
                }
                if(ldidx < 0 || ldidx >= tarr->len) {
                    snprintf(errbuf, sizeof(errbuf), "数组越界：索引 %d 超出范围 [0, %d)", ldidx, tarr->len);
                    runtime_error(errbuf);
                }
                long double val = ((long double*)tarr->items)[ldidx];
                LONG_DOUBLE_PUSH(val);
                break;
            }
            case OPC_INDEX_GET: {
                Value idx = stack[--sp];
                Value c = stack[--sp];
                Value result;
                /* 判断是否是 class 结构体实例 */
                if(c.type == VAL_CLASS_PTR) {
                    /* 结构体实例：先从字段中查找，再从 vtable 方法中查找 */
                    const char* field_name = lumyr_str_cstr(&idx);
                    if(field_name) {
                        result = lumyr_class_instance_get_field(c, field_name);
                        if(result.type == VAL_NONE) {
                            /* 字段没找到，从 vtable 方法中查找 */
                            ClassInstance* inst = (ClassInstance*)c.v.struct_ptr;
                            ClassVTable* vt = inst->vtable;
                            if(vt) {
                                RuntimeFunc* method = NULL;
                                /* 先在当前类的方法表中查找 */
                                for(int mi = 0; mi < vt->nmethods; mi++) {
                                    if(vt->method_names && strcmp(vt->method_names[mi], field_name) == 0) {
                                        method = vt->methods[mi];
                                        break;
                                    }
                                }
                                /* 再在父类的方法表中查找（递归） */
                                if(!method && vt->parent) {
                                    ClassVTable* pvt = vt->parent;
                                    while(pvt && !method) {
                                        for(int mi = 0; mi < pvt->nmethods; mi++) {
                                            if(pvt->method_names && strcmp(pvt->method_names[mi], field_name) == 0) {
                                                method = pvt->methods[mi];
                                                break;
                                            }
                                        }
                                        pvt = pvt->parent;
                                    }
                                }
                                if(method) {
                                    /* vt->methods[i] 已经是 RuntimeFunc* 了，直接包装成 Value */
                                    result.type = VAL_FUNC;
                                    result.v.func.func_obj = method;
                                    result.v.func.ffi_func = NULL;
                                    result.v.func.is_ffi = 0;
                                } else {
                                    /* vtable 方法表为空，用红黑树封装好的方法 ir_func_table_lookup_class 查找 class 方法 */
                                    /* 方法名直接使用 field_name，因为 ir_func_table_lookup_class 使用 class_name 作为 scope */
                                    BytecodeFunc* method_bf = ir_func_table_lookup_class(vt->class_name, field_name);
                                    if(method_bf) {
                                        /* 把 BytecodeFunc* 包装成 RuntimeFunc* */
                                        RuntimeFunc* rf = (RuntimeFunc*)malloc(sizeof(RuntimeFunc));
                                        memset(rf, 0, sizeof(RuntimeFunc));
                                        rf->entry = vm_func_entry;
                                        rf->param_count = method_bf->param_cnt;
                                        rf->has_variadic = method_bf->has_variadic;
                                        rf->captures = NULL;
                                        rf->capture_count = -1;  /* 标记为 payload 函数 */
                                        /* 创建 InterpFuncPayload，存储 BytecodeFunc* */
                                        InterpFuncPayload* pl = (InterpFuncPayload*)malloc(sizeof(InterpFuncPayload));
                                        memset(pl, 0, sizeof(InterpFuncPayload));
                                        pl->body = NULL;
                                        pl->bytecode = method_bf;
                                        pl->param_names = method_bf->params;
                                        pl->param_cnt = method_bf->param_cnt;
                                        pl->has_variadic = method_bf->has_variadic;
                                        pl->default_vals = NULL;
                                        pl->has_default = NULL;
                                        pl->param_is_ref = method_bf->param_is_ref;
                                        pl->captured_names = NULL;
                                        pl->captured_cells = NULL;
                                        pl->captured_cell_count = 0;
                                        pl->is_generator = method_bf->is_generator;
                                        rf->captures = (Value*)pl;
                                        result.type = VAL_FUNC;
                                        result.v.func.func_obj = rf;
                                        result.v.func.ffi_func = NULL;
                                        result.v.func.is_ffi = 0;
                                    }
                                }
                            }
                        }
                    } else {
                        result = val_none();
                    }
                } else {
                    /* map 或其他类型：用旧的方式 */
                    result = lumyr_index_get(c, idx);
                    /* 解耦：不允许回退到全局符号表查找扩展方法
                       如果对象没有该属性，直接返回 VAL_NONE，由调用方处理 */
                }
                stack[sp++] = result;
                break;
            }
            case OPC_INDEX_SET: {
                Value val = stack[--sp];
                Value idx = stack[--sp];
                Value arr = stack[--sp];
                /* 判断是否是 class 结构体实例 */
                if(arr.type == VAL_CLASS_PTR) {
                    /* 结构体实例：用新的方式设置字段 */
                    const char* field_name = lumyr_str_cstr(&idx);
                    if(field_name) {
                        lumyr_class_instance_set_field(arr, field_name, val);
                    }
                    stack[sp++] = val;
                } else {
                    /* map 或其他类型：用旧的方式 */
                    stack[sp++] = lumyr_array_set(arr, idx, val);
                }
                break;
            }
            case OPC_LOAD_FIELD: {
                const char* vname = bf->syms[in.a];
                Value fname = bf->consts[in.b];
                _Bool fnd = 0;
                Value obj = stackframe_get(frame, vname, &fnd);
                if(!fnd) runtime_undefined("变量", vname);
                /* 判断是否是 class 结构体实例 */
                if(obj.type == VAL_CLASS_PTR) {
                    /* 结构体实例：先从字段中查找，再从 vtable 方法中查找 */
                    const char* field_name = lumyr_str_cstr(&fname);
                    if(field_name) {
                        Value field_val = lumyr_class_instance_get_field(obj, field_name);
                        if(field_val.type == VAL_NONE) {
                            /* 字段没找到，从 vtable 方法中查找 */
                            ClassInstance* inst = (ClassInstance*)obj.v.struct_ptr;
                            ClassVTable* vt = inst ? inst->vtable : NULL;
                            if(vt) {
                                RuntimeFunc* method = NULL;
                                /* 先在当前类的方法表中查找 */
                                for(int mi = 0; mi < vt->nmethods; mi++) {
                                    if(vt->method_names && strcmp(vt->method_names[mi], field_name) == 0) {
                                        method = vt->methods[mi];
                                        break;
                                    }
                                }
                                /* 再在父类的方法表中查找（递归） */
                                if(!method && vt->parent) {
                                    ClassVTable* pvt = vt->parent;
                                    while(pvt && !method) {
                                        for(int mi = 0; mi < pvt->nmethods; mi++) {
                                            if(pvt->method_names && strcmp(pvt->method_names[mi], field_name) == 0) {
                                                method = pvt->methods[mi];
                                                break;
                                            }
                                        }
                                        pvt = pvt->parent;
                                    }
                                }
                                if(method) {
                                    /* 把 RuntimeFunc* 包装成 Value */
                                    field_val.type = VAL_FUNC;
                                    field_val.v.func.func_obj = method;
                                    field_val.v.func.ffi_func = NULL;
                                    field_val.v.func.is_ffi = 0;
                                } else {
                                    /* vtable 方法表为空，用红黑树封装好的方法 ir_func_table_lookup_class 查找 class 方法 */
                                    BytecodeFunc* method_bf = ir_func_table_lookup_class(vt->class_name, field_name);
                                    if(method_bf) {
                                        /* 把 BytecodeFunc* 包装成 RuntimeFunc* */
                                        RuntimeFunc* rf = (RuntimeFunc*)malloc(sizeof(RuntimeFunc));
                                        memset(rf, 0, sizeof(RuntimeFunc));
                                        rf->entry = vm_func_entry;
                                        rf->param_count = method_bf->param_cnt;
                                        rf->has_variadic = method_bf->has_variadic;
                                        rf->captures = NULL;
                                        rf->capture_count = -1;
                                        InterpFuncPayload* pl = (InterpFuncPayload*)malloc(sizeof(InterpFuncPayload));
                                        memset(pl, 0, sizeof(InterpFuncPayload));
                                        pl->body = NULL;
                                        pl->bytecode = method_bf;
                                        pl->param_names = method_bf->params;
                                        pl->param_cnt = method_bf->param_cnt;
                                        pl->has_variadic = method_bf->has_variadic;
                                        pl->default_vals = NULL;
                                        pl->has_default = NULL;
                                        pl->param_is_ref = method_bf->param_is_ref;
                                        pl->captured_names = NULL;
                                        pl->captured_cells = NULL;
                                        pl->captured_cell_count = 0;
                                        pl->is_generator = method_bf->is_generator;
                                        rf->captures = (Value*)pl;
                                        field_val.type = VAL_FUNC;
                                        field_val.v.func.func_obj = rf;
                                        field_val.v.func.ffi_func = NULL;
                                        field_val.v.func.is_ffi = 0;
                                    }
                                }
                            }
                        }
                        stack[sp++] = field_val;
                    } else {
                        stack[sp++] = val_none();
                    }
                } else {
                    /* map 或其他类型：用旧的方式 */
                    stack[sp++] = lumyr_index_get(obj, fname);
                }
                break;
            }
            case OPC_STORE_FIELD: {
                Value val = stack[--sp];
                const char* vname = bf->syms[in.a];
                Value fname = bf->consts[in.b];
                _Bool fnd = 0;
                Value obj = stackframe_get(frame, vname, &fnd);
                if(!fnd) runtime_undefined("变量", vname);
                /* 判断是否是 class 结构体实例 */
                if(obj.type == VAL_CLASS_PTR) {
                    /* 结构体实例：用新的方式写入字段 */
                    const char* field_name = lumyr_str_cstr(&fname);
                    if(field_name) {
                        lumyr_class_instance_set_field(obj, field_name, val);
                    }
                } else {
                    /* map 或其他类型：用旧的方式 */
                    lumyr_array_set(obj, fname, val);
                }
                stack[sp++] = val;
                break;
            }
            case OPC_LOAD_STRUCT_PTR: {
                /* 加载 struct 变量的指针（用于方法 self 参数）；VM 中行为同 OPC_LOAD_VAR */
                const char* name = bf->syms[in.a];
                _Bool fnd = 0;
                Value vv = stackframe_get(frame, name, &fnd);
                if(!fnd) runtime_undefined("变量", name);
                stack[sp++] = vv;
                break;
            }
            case OPC_STORE_NESTED_FIELD: {
                Value val = stack[--sp];
                const char* vname = bf->syms[in.a];
                const char* combined = lumyr_str_cstr(&bf->consts[in.b]);
                _Bool fnd = 0;
                Value obj = stackframe_get(frame, vname, &fnd);
                if(!fnd) runtime_undefined("变量", vname);
                /* 解析组合字段名 "top_left.x" */
                char nested_fname[128], field_name[128];
                const char* dot = strchr(combined, '.');
                if(dot) {
                    size_t nlen = (size_t)(dot - combined);
                    if(nlen >= sizeof(nested_fname)) nlen = sizeof(nested_fname) - 1;
                    memcpy(nested_fname, combined, nlen);
                    nested_fname[nlen] = '\0';
                    strncpy(field_name, dot + 1, sizeof(field_name) - 1);
                    field_name[sizeof(field_name) - 1] = '\0';
                    /* 访问嵌套 struct 字段，然后写入字段值 */
                    Value nested = lumyr_index_get(obj, lumyr_make_string(nested_fname));
                    lumyr_array_set(nested, lumyr_make_string(field_name), val);
                }
                stack[sp++] = val;
                break;
            }
            case OPC_BUILTIN: {
                sp = vm_exec_builtin(in, stack, sp, frame, ctx);
                break;
            }
            case OPC_PRINT: {
                /* 多参数打印：in.a = 参数数量；从栈底到栈顶依次打印，最后换行并弹出所有参数 */
                int cnt = in.a;
                if(cnt <= 0) cnt = 1;  /* 向后兼容：旧代码可能用 0 表示单参数 */
                int base = sp - cnt;
                for(int i = 0; i < cnt; i++) {
                    if(i > 0) printf(" ");  /* 参数之间用空格分隔 */
                    lumyr_print_inline(stack[base + i]);
                }
                printf("\n");
                sp -= cnt;  /* 弹出所有参数 */
                break;
            }
            case OPC_PRINT_INT: {
                /* 从 int 栈弹出并打印（零开销，用于声明为 int 的变量） */
                int iv = INT_POP();
                printf("%d\n", iv);
                break;
            }
            case OPC_PRINT_DOUBLE: {
                /* 从 double 栈弹出并打印（零开销，用于声明为 double 的变量） */
                double dv = DOUBLE_POP();
                printf("%g\n", dv);
                break;
            }
            case OPC_PRINT_FLOAT: {
                /* 从 float 栈弹出并打印（零开销，用于声明为 float 的变量） */
                float fv = FLOAT_POP();
                printf("%g\n", (double)fv);
                break;
            }
            case OPC_PRINT_UINT: {
                /* 从 uint 栈弹出并打印（零开销，用于声明为 uint 的变量） */
                unsigned int uv = UINT_POP();
                printf("%u\n", uv);
                break;
            }
            case OPC_PRINT_BOOL: {
                /* 从 bool 栈弹出并打印（零开销，用于声明为 bool 的变量） */
                _Bool bv = BOOL_POP();
                printf("%s\n", bv ? "true" : "false");
                break;
            }
            case OPC_PRINT_CHAR: {
                /* 从 char 栈弹出并打印（零开销，用于声明为 char 的变量） */
                char cv = CHAR_POP();
                printf("%c\n", cv);
                break;
            }
            case OPC_PRINT_BYTE: {
                /* 从 byte 栈弹出并打印（零开销，用于声明为 byte 的变量） */
                unsigned char byv = BYTE_POP();
                printf("%d\n", (int)byv);
                break;
            }
            case OPC_PRINT_INT8: {
                /* 从 int8 栈弹出并打印（零开销，用于声明为 int8 的变量） */
                int8_t i8v = INT8_POP();
                printf("%d\n", (int)i8v);
                break;
            }
            case OPC_PRINT_INT16: {
                /* 从 int16 栈弹出并打印（零开销，用于声明为 int16 的变量） */
                int16_t i16v = INT16_POP();
                printf("%d\n", (int)i16v);
                break;
            }
            case OPC_PRINT_INT32: {
                /* 从 int32 栈弹出并打印（零开销，用于声明为 int32 的变量） */
                int32_t i32v = INT32_POP();
                printf("%d\n", (int)i32v);
                break;
            }
            case OPC_PRINT_INT64: {
                /* 从 int64 栈弹出并打印（零开销，用于声明为 int64 的变量） */
                int64_t i64v = INT64_POP();
                printf("%lld\n", (long long)i64v);
                break;
            }
            case OPC_PRINT_UINT8: {
                /* 从 uint8 栈弹出并打印（零开销，用于声明为 uint8 的变量） */
                uint8_t u8v = UINT8_POP();
                printf("%u\n", (unsigned int)u8v);
                break;
            }
            case OPC_PRINT_UINT16: {
                /* 从 uint16 栈弹出并打印（零开销，用于声明为 uint16 的变量） */
                uint16_t u16v = UINT16_POP();
                printf("%u\n", (unsigned int)u16v);
                break;
            }
            case OPC_PRINT_UINT32: {
                /* 从 uint32 栈弹出并打印（零开销，用于声明为 uint32 的变量） */
                uint32_t u32v = UINT32_POP();
                printf("%u\n", (unsigned int)u32v);
                break;
            }
            case OPC_PRINT_UINT64: {
                /* 从 uint64 栈弹出并打印（零开销，用于声明为 uint64 的变量） */
                uint64_t u64v = UINT64_POP();
                printf("%llu\n", (unsigned long long)u64v);
                break;
            }
            case OPC_PRINT_LONG: {
                /* 从 long 栈弹出并打印（零开销，用于声明为 long 的变量） */
                long lv = LONG_POP();
                printf("%ld\n", (long)lv);
                break;
            }
            case OPC_PRINT_ULONG: {
                /* 从 unsigned long 栈弹出并打印（零开销，用于声明为 unsigned long 的变量） */
                unsigned long ulv = ULONG_POP();
                printf("%lu\n", (unsigned long)ulv);
                break;
            }
            case OPC_PRINT_SIZE_T: {
                /* 从 size_t 栈弹出并打印（零开销，用于声明为 size_t 的变量） */
                size_t stv = SIZE_T_POP();
                printf("%zu\n", (size_t)stv);
                break;
            }
            case OPC_PRINT_SSIZE_T: {
                /* 从 ssize_t 栈弹出并打印（零开销，用于声明为 ssize_t 的变量） */
                ssize_t sstv = SSIZE_T_POP();
                printf("%zd\n", (ssize_t)sstv);
                break;
            }
            case OPC_PRINT_LONG_DOUBLE: {
                /* 从 long double 栈弹出并打印（零开销，用于声明为 long double 的变量） */
                long double ldv = LONG_DOUBLE_POP();
                printf("%Lf\n", (long double)ldv);
                break;
            }
            case OPC_TO_BOOL:
                stack[sp - 1] = lumyr_make_bool(lumyr_to_bool(stack[sp - 1]));
                break;
            case OPC_DUP:
                stack[sp] = stack[sp - 1];
                sp++;
                break;
            case OPC_POP:
                sp--;
                break;
            case OPC_TRY: {
                /* longjmp 后自动变量值未定义：catch 目标按层保存 */
                vm_ensure(vm_depth + 2);
                int d = vm_depth;
                vm_target[d] = in.a;
                vm_sp[d] = sp;
                vm_prev[d] = g_err_jmp;
                vm_tn[d] = g_trace_n;
                vm_fn[d] = vm_fin_n;
                g_err_jmp = &vm_jbs[d];
                if(setjmp(vm_jbs[d]) == 0) {
                    vm_depth = d + 1;
                } else {
                    /* longjmp 后局部变量值未定义：本层索引从 g_err_jmp 反推
                       （不依赖 vm_depth，因为嵌套 catch 中再次 throw 时 vm_depth 可能已被 GET_ERR 修改） */
                    int d2 = (int)(g_err_jmp - vm_jbs);
                    if(d2 < 0 || d2 >= vm_cap) d2 = vm_depth - 1;  /* fallback */
                    sp = vm_sp[d2];
                    vm_depth = d2 + 1;  /* catch 块与 try 块在同一层 try 保护区中 */
                    g_err_jmp = vm_prev[d2];
                    /* trace/fin 栈不在此截断：GET_ERR 用完整残留生成回溯后再截断 */
                    pc = vm_target[d2];
                }
                break;
            }
            case OPC_ENDTRY:
                if(vm_depth > 0) { vm_depth--; g_err_jmp = vm_prev[vm_depth]; }
                break;
            case OPC_GET_ERR: {
                /* 错误对象化：type/message + 调用栈回溯；然后截断残留到 TRY 层 */
                char* st = lumyr_build_stack_trace();
                stack[sp++] = lumyr_make_error(g_err_type, g_err_msg, st);
                free(st);
                /* OPC_TRY else 已将 vm_depth 设为 d+1（catch 块与 try 块同层），
                   TRY 层备份在索引 d 处，故用 vm_depth-1 索引 */
                int try_idx = vm_depth - 1;
                if(try_idx < 0) try_idx = 0;
                g_trace_n = vm_tn[try_idx];
                vm_fin_n = vm_fn[try_idx];
                break;
            }
            case OPC_THROW: {
                /* 弹1：包装成错误对象抛出（字符串→type Error；错误对象→原样；map→type/message 字段） */
                Value v = stack[--sp];
                const char* type = "Error";
                char* msg = NULL;
                if(v.type == VAL_ERROR) {
                    type = v.v.err.type ? v.v.err.type : "Error";
                    msg = strdup(v.v.err.message ? v.v.err.message : "");
                } else if(v.type == VAL_MAP) {
                    if(lumyr_map_has(v, lumyr_make_string("type"))) {
                        Value tv = lumyr_map_get(v, lumyr_make_string("type"));
                        if(tv.type == VAL_STRING) type = lumyr_str_cstr(&tv);
                    }
                    if(lumyr_map_has(v, lumyr_make_string("message"))) {
                        Value mv = lumyr_map_get(v, lumyr_make_string("message"));
                        if(mv.type == VAL_STRING) msg = strdup(lumyr_str_cstr(&mv));
                    }
                }
                if(!msg) msg = value_to_str(v);
                g_err_type_set(type);
                g_err_msg_set(msg);
                free(msg);
                if(g_err_jmp) longjmp(*g_err_jmp, 1);
                LOG_ERROR("Runtime Error: %s\n", g_err_msg);
                exit(EXIT_FAILURE);
            }
            case OPC_FIN_PUSH:
                vm_ensure(vm_fin_n + 2);
                vm_fin_act[vm_fin_n] = in.a;
                vm_fin_tgt[vm_fin_n] = in.b;
                vm_fin_dep[vm_fin_n] = vm_depth - 1;   /* try 前深度（异常入口 act=2 不使用） */
                vm_fin_n++;
                break;
            case OPC_FINISH: {
                if(vm_fin_n <= 0) runtime_error("finally 完成栈为空");
                int act = vm_fin_act[--vm_fin_n];
                if(act == 1 || act == 3 || act == 4) {
                    vm_depth = vm_fin_dep[vm_fin_n];   /* 退出 try 保护区，恢复层深度 */
                    g_err_jmp = vm_prev[vm_fin_dep[vm_fin_n]];  /* 还原 TRY 前 handler */
                    pc = vm_fin_tgt[vm_fin_n];
                } else if(act == 2) {
                    /* RETHROW：错误消息/类型仍在 g_err_msg/g_err_type，向上一层冒泡 */
                    if(g_err_jmp) longjmp(*g_err_jmp, 1);
                    LOG_ERROR("Runtime Error: %s\n", g_err_msg);
                    exit(EXIT_FAILURE);
                } else if(act == 5) {
                    /* RETURN：恢复函数返回 */
                    Value v = vm_pend_val;
                    vm_depth = saved_depth;
                    g_err_jmp = saved_gj;
                    gc_set_roots(old_gc_stack, old_gc_sp, old_gc_frame);
                    tls_vm_run_depth--;
                    if (tls_vm_run_depth == 0) {
                        gc_protect_push(v);
                        gc_unregister_thread_keep_protect();
                    } else {
                        if (!tls_skip_vm_unregister) gc_unregister_thread();
                        gc_protect_push(v);
                        gc_protect_pop();
                    }
                    free(stack);
                    return v;
                } else {
                    runtime_error("finally 完成动作未知");
                }
                break;
            }
            case OPC_PEND_RETURN: {
                /* 弹1（返回值）→ 压 RETURN 动作 → 跳 finally（b=0 则直接返回） */
                vm_pend_val = stack[--sp];
                vm_fin_act[vm_fin_n] = 5;
                vm_fin_tgt[vm_fin_n] = 0;
                vm_fin_n++;
                if(in.b) pc = in.b;
                break;
            }
            case OPC_JMP:
                pc = in.a;
                break;
            case OPC_JMP_IF_FALSE:
                if(!lumyr_to_bool(stack[--sp])) pc = in.a;
                break;
            case OPC_JMP_IF_TRUE:
                if(lumyr_to_bool(stack[--sp])) pc = in.a;
                break;
            case OPC_JMP_IF_NULL:
                if(stack[--sp].type == VAL_NONE) pc = in.a;
                break;
            case OPC_CLASS_NEW: {
                /* VM 模式下创建 class 实例：优先用结构体方式（虚表），如果虚表未注册则回退到 map 方式 */
                const char* class_name = bf->syms[in.a];
                int argc = in.b;
                /* 尝试用新的结构体方式创建实例 */
                Value obj = lumyr_class_instance_new(class_name);
                /* 用构造参数按顺序初始化字段 */
                if(argc > 0) {
                    TypeDef* td = class_lookup(class_name);
                    if(td && td->nprops > 0) {
                        int n = argc < td->nprops ? argc : td->nprops;
                        for(int i = 0; i < n; i++) {
                            Value arg = stack[sp - argc + i];
                            if(obj.type == VAL_CLASS_PTR) {
                                /* 结构体实例：用新的方式设置字段 */
                                lumyr_class_instance_set_field(obj, td->props[i], arg);
                            } else {
                                /* map 实例：用旧的方式设置字段 */
                                lumyr_map_set(&obj, lumyr_make_string(td->props[i]), arg);
                            }
                        }
                    }
                    sp -= argc;
                }
                stack[sp++] = obj;
                break;
            }
            case OPC_CALL: {
                const char* fname = bf->syms[in.a];
                int argc = in.b;
                LOG_PUSH_CALL("OPC_CALL fname=%s argc=%d", fname, argc);
                /* 特殊处理：lumyr_interface_cast 内置函数（接口类型转换） */
                if(strcmp(fname, "lumyr_interface_cast") == 0 && argc >= 2) {
                    Value obj = stack[sp - argc];
                    Value iface_val = stack[sp - argc + 1];
                    const char* iface_name = (iface_val.type == VAL_STRING) ? lumyr_str_cstr(&iface_val) : "";
                    _Bool impl = 0;
                    /* class 实例：通过 lumyr_obj_implements_interface 判断 */
                    if(obj.type == VAL_CLASS_PTR && obj.v.struct_ptr) {
                        impl = lumyr_obj_implements_interface(obj, iface_name);
                    }
                    if(impl) {
                        sp -= argc;
                        stack[sp++] = obj;
                    } else {
                        const char* type_name = "unknown";
                        if(obj.type == VAL_MAP) {
                            if(lumyr_map_has(obj, lumyr_make_string("__classname__"))) {
                                Value cn = lumyr_map_get(obj, lumyr_make_string("__classname__"));
                                if(cn.type == VAL_STRING) type_name = lumyr_str_cstr(&cn);
                            }
                        }
                        char msg[256];
                        snprintf(msg, sizeof(msg), "接口类型转换失败：类型 \"%s\" 未实现接口 \"%s\"", type_name, iface_name);
                        runtime_error(msg);
                    }
                    break;
                }
                /* __super_call_<当前类名>_<方法名>(self, args...)：调用父类方法（编译期静态绑定） */
                if(strncmp(fname, "__super_call_", 13) == 0 && argc >= 1) {
                    /* 解析函数名，获取当前类名和方法名 */
                    const char* rest = fname + 13;
                    char current_class_name[128] = {0};
                    char method_name[128] = {0};
                    const char* underscore = strchr(rest, '_');
                    if(underscore) {
                        int class_len = underscore - rest;
                        if(class_len > 0 && class_len < 127) {
                            strncpy(current_class_name, rest, class_len);
                            current_class_name[class_len] = '\0';
                        }
                        strncpy(method_name, underscore + 1, 127);
                        method_name[127] = '\0';
                    }
                    /* 根据当前类名查找父类 */
                    const char* parent_name = NULL;
                    if(current_class_name[0]) {
                        TypeDef* td = class_lookup(current_class_name);
                        if(td && td->parent) parent_name = td->parent;
                    }
                    Value self_val = stack[sp - argc];
                    /* 类型拆分后，class 实例使用 VAL_CLASS_PTR 类型 */
                    _Bool is_class_instance = (self_val.type == VAL_CLASS_PTR);
                    if(is_class_instance && parent_name && method_name[0]) {
                        void* rf = NULL;
                        /* 构造函数 __init__ 单独处理，使用 class_get_constructor_func */
                        if(strcmp(method_name, "__init__") == 0) {
                            rf = class_get_constructor_func(parent_name);
                        } else {
                            rf = class_find_method_func(parent_name, method_name);
                        }
                        if(rf) {
                            /* 调用父类方法：使用和 super_method_call 相同的方式 */
                            RuntimeFunc* rf_ptr = (RuntimeFunc*)rf;
                            Value* eval_args = &stack[sp - argc];  /* 从 self 开始 */
                            int call_argc = argc;
                            StackFrame* callee = stackframe_new(frame);
                            if(interp_func_is_payload(rf_ptr)) {
                                int pcnt = interp_func_param_cnt(rf_ptr);
                                for(int i = 0; i < pcnt; i++) {
                                    const char* pname = interp_func_param_name(rf_ptr, i);
                                    Value bound = (i < call_argc) ? eval_args[i] : val_none();
                                    stackframe_bind(callee, pname, bound);
                                }
                            }
                            closure_bind_cells(rf_ptr, callee);
                            RuntimeFunc* prev_rf = interp_set_current_rf(rf_ptr);
                            int saved_break = ctx->hit_break;
                            int saved_cont = ctx->hit_continue;
                            ctx->hit_break = 0;
                            ctx->hit_continue = 0;
                            g_trace_push(fname);
                            Value ret = rf_ptr->entry(call_argc, eval_args, ctx, callee);
                            if(g_trace_n > 0) g_trace_n--;
                            ctx->hit_break = saved_break;
                            ctx->hit_continue = saved_cont;
                            interp_set_current_rf(prev_rf);
                            stackframe_destroy(callee);
                            sp -= argc;
                            stack[sp++] = ret;
                            break;
                        }
                    }
                    runtime_error("super 调用失败：无法找到父类方法");
                }
                /* __super_ctor_<当前类名>(self, args...)：调用父类构造函数（编译期静态绑定） */
                if(strncmp(fname, "__super_ctor_", 13) == 0 && argc >= 1) {
                    /* 解析函数名，获取当前类名 */
                    const char* current_class_name = fname + 13;
                    /* 根据当前类名查找父类 */
                    const char* parent_name = NULL;
                    if(current_class_name[0]) {
                        TypeDef* td = class_lookup(current_class_name);
                        if(td && td->parent) parent_name = td->parent;
                    }
                    Value self_val = stack[sp - argc];
                    /* 类型拆分后，class 实例使用 VAL_CLASS_PTR 类型 */
                    _Bool is_class_instance2 = (self_val.type == VAL_CLASS_PTR);
                    if(is_class_instance2 && parent_name) {
                        /* 调用父类构造函数 */
                        void* rf = class_get_constructor_func(parent_name);
                        if(rf) {
                            RuntimeFunc* rf_ptr = (RuntimeFunc*)rf;
                            Value* eval_args = &stack[sp - argc];
                            int call_argc = argc;
                            StackFrame* callee = stackframe_new(frame);
                            if(interp_func_is_payload(rf_ptr)) {
                                int pcnt = interp_func_param_cnt(rf_ptr);
                                for(int i = 0; i < pcnt; i++) {
                                    const char* pname = interp_func_param_name(rf_ptr, i);
                                    Value bound = (i < call_argc) ? eval_args[i] : val_none();
                                    stackframe_bind(callee, pname, bound);
                                }
                            }
                            closure_bind_cells(rf_ptr, callee);
                            RuntimeFunc* prev_rf = interp_set_current_rf(rf_ptr);
                            int saved_break = ctx->hit_break;
                            int saved_cont = ctx->hit_continue;
                            ctx->hit_break = 0;
                            ctx->hit_continue = 0;
                            g_trace_push(fname);
                            Value ret = rf_ptr->entry(call_argc, eval_args, ctx, callee);
                            if(g_trace_n > 0) g_trace_n--;
                            ctx->hit_break = saved_break;
                            ctx->hit_continue = saved_cont;
                            interp_set_current_rf(prev_rf);
                            stackframe_destroy(callee);
                            sp -= argc;
                            stack[sp++] = ret;
                            break;
                        }
                    }
                    /* 如果没有自定义构造函数，直接返回 self */
                    sp -= argc;
                    stack[sp++] = self_val;
                    break;
                }
                /* super_method_call(method_name, current_class, self, args...)：调用父类方法 */
                if(strcmp(fname, "super_method_call") == 0 && argc >= 3) {
                    Value method_name_val = stack[sp - argc];
                    Value current_class_val = stack[sp - argc + 1];
                    Value self_val = stack[sp - argc + 2];
                    if(method_name_val.type == VAL_STRING && current_class_val.type == VAL_STRING &&
                       self_val.type == VAL_MAP) {
                        TypeDef* td = class_lookup(lumyr_str_cstr(&current_class_val));
                        if(td && td->parent) {
                            TypeDef* parent_td = class_lookup(td->parent);
                            if(parent_td) {
                                void* rf = NULL;
                                if(strcmp(lumyr_str_cstr(&method_name_val), "__init__") == 0) {
                                    rf = parent_td->constructor_func;
                                } else {
                                    rf = class_find_method_func(parent_td->name, lumyr_str_cstr(&method_name_val));
                                }
                                if(rf) {
                                    /* 调用父类方法：self 作为第一个参数，其他参数跟随 */
                                    Value func_val;
                                    func_val.type = VAL_FUNC;
                                    func_val.v.func.func_obj = (RuntimeFunc*)rf;
                                    func_val.v.func.ffi_func = NULL;
                                    func_val.v.func.is_ffi = 0;
                                    RuntimeFunc* rf_ptr = (RuntimeFunc*)rf;
                                    Value* eval_args = &stack[sp - argc + 2];  /* 跳过 method_name 和 current_class，从 self 开始 */
                                    int call_argc = argc - 2;
                                    StackFrame* callee = stackframe_new(frame);
                                    if(interp_func_is_payload(rf_ptr)) {
                                        int pcnt = interp_func_param_cnt(rf_ptr);
                                        for(int i = 0; i < pcnt; i++) {
                                            const char* pname = interp_func_param_name(rf_ptr, i);
                                            Value bound = (i < call_argc) ? eval_args[i] : val_none();
                                            stackframe_bind(callee, pname, bound);
                                        }
                                    }
                                    closure_bind_cells(rf_ptr, callee);
                                    RuntimeFunc* prev_rf = interp_set_current_rf(rf_ptr);
                                    int saved_break = ctx->hit_break;
                                    int saved_cont = ctx->hit_continue;
                                    ctx->hit_break = 0;
                                    ctx->hit_continue = 0;
                                    g_trace_push(fname);
                                    Value ret = rf_ptr->entry(call_argc, eval_args, ctx, callee);
                                    if(g_trace_n > 0) g_trace_n--;
                                    ctx->hit_break = saved_break;
                                    ctx->hit_continue = saved_cont;
                                    interp_set_current_rf(prev_rf);
                                    stackframe_destroy(callee);
                                    sp -= argc;
                                    stack[sp++] = ret;
                                    break;
                                }
                            }
                        }
                    }
                    /* 如果 super_method_call 失败，报错 */
                    /* 如果 super_method_call 失败，报错 */
                    char buf[256];
                    snprintf(buf, sizeof(buf), "super 调用失败：无法找到父类方法");
                    runtime_error(buf);
                }
                /* <类名>___init__(obj, args...)：调用 class 构造函数 */
                {
                    size_t flen = strlen(fname);
                    if(flen >= 9 && strcmp(fname + flen - 9, "___init__") == 0 && argc >= 1) {
                        Value self_val = stack[sp - argc];
                        const char* ctor_class_name = NULL;
                        if(self_val.type == VAL_MAP &&
                           lumyr_map_has(self_val, lumyr_make_string("__classname__"))) {
                            Value cn = lumyr_map_get(self_val, lumyr_make_string("__classname__"));
                            if(cn.type == VAL_STRING) ctor_class_name = lumyr_str_cstr(&cn);
                        } else if(self_val.type == VAL_CLASS_PTR) {
                            ClassInstance* inst = (ClassInstance*)self_val.v.struct_ptr;
                            if(inst && inst->vtable) ctor_class_name = inst->vtable->class_name;
                        }
                        if(ctor_class_name) {
                            void* rf = class_get_constructor_func(ctor_class_name);
                            if(rf) {
                                /* 调用构造函数：self 作为第一个参数 */
                                Value func_val;
                                func_val.type = VAL_FUNC;
                                func_val.v.func.func_obj = (RuntimeFunc*)rf;
                                func_val.v.func.ffi_func = NULL;
                                func_val.v.func.is_ffi = 0;
                                RuntimeFunc* rf_ptr = (RuntimeFunc*)rf;
                                Value* eval_args = &stack[sp - argc];
                                int call_argc = argc;
                                StackFrame* callee = stackframe_new(frame);
                                if(interp_func_is_payload(rf_ptr)) {
                                    int pcnt = interp_func_param_cnt(rf_ptr);
                                    for(int i = 0; i < pcnt; i++) {
                                        const char* pname = interp_func_param_name(rf_ptr, i);
                                        Value bound = (i < call_argc) ? eval_args[i] : val_none();
                                        stackframe_bind(callee, pname, bound);
                                    }
                                }
                                closure_bind_cells(rf_ptr, callee);
                                RuntimeFunc* prev_rf = interp_set_current_rf(rf_ptr);
                                int saved_break = ctx->hit_break;
                                int saved_cont = ctx->hit_continue;
                                ctx->hit_break = 0;
                                ctx->hit_continue = 0;
                                g_trace_push(fname);
                                Value ret = rf_ptr->entry(call_argc, eval_args, ctx, callee);
                                if(g_trace_n > 0) g_trace_n--;
                                ctx->hit_break = saved_break;
                                ctx->hit_continue = saved_cont;
                                interp_set_current_rf(prev_rf);
                                stackframe_destroy(callee);
                                sp -= argc;
                                /* 构造函数返回 self（如果返回 none，则返回 self） */
                                if(ret.type == VAL_NONE) {
                                    stack[sp++] = self_val;
                                } else {
                                    stack[sp++] = ret;
                                }
                                break;
                            }
                        }
                        /* 如果构造函数调用失败，报错 */
                        char buf[256];
                        snprintf(buf, sizeof(buf), "构造函数调用失败：无法找到构造函数");
                        runtime_error(buf);
                    }
                }
                // 0. 数组方法识别：当第一个参数是数组时，直接调用对应的运行时函数
                // 这样数组方法名（如 add、remove、clear）就不占用全局函数命名空间了
                if(argc > 0) {
                    Value arr_obj = stack[sp - argc];
                    if(arr_obj.type == VAL_ARRAY) {
                        _Bool is_array_method = 1;
                        if(strcmp(fname, "add") == 0) {
                            if(argc >= 2) {
                                Value v = stack[--sp];
                                lumyr_array_add(&stack[sp-1], v);
                            } else {
                                runtime_error("add() 需要至少 2 个参数");
                            }
                        } else if(strcmp(fname, "remove") == 0) {
                            if(argc >= 2) {
                                Value idx = stack[--sp];
                                lumyr_del(&stack[sp-1], idx);
                            } else {
                                runtime_error("remove() 需要至少 2 个参数");
                            }
                        } else if(strcmp(fname, "clear") == 0) {
                            lumyr_array_clear(&stack[sp-1]);
                        } else if(strcmp(fname, "indexOf") == 0) {
                            if(argc >= 2) {
                                Value x = stack[--sp];
                                Value arr = stack[--sp];
                                stack[sp++] = lumyr_index_of(arr, x);
                            } else {
                                runtime_error("indexOf() 需要至少 2 个参数");
                            }
                        } else if(strcmp(fname, "arr_get") == 0 || strcmp(fname, "get") == 0) {
                            if(argc >= 2) {
                                Value i = stack[--sp];
                                Value arr = stack[--sp];
                                stack[sp++] = lumyr_array_get_safe(arr, i);
                            } else {
                                runtime_error("get() 需要至少 2 个参数");
                            }
                        } else if(strcmp(fname, "set") == 0) {
                            if(argc >= 3) {
                                Value v = stack[--sp];
                                Value i = stack[--sp];
                                Value arr = stack[--sp];
                                stack[sp++] = lumyr_array_set_method(arr, i, v);
                            } else {
                                runtime_error("set() 需要至少 3 个参数");
                            }
                        } else if(strcmp(fname, "first") == 0) {
                            Value arr = stack[--sp];
                            stack[sp++] = lumyr_array_first(arr);
                        } else if(strcmp(fname, "last") == 0) {
                            Value arr = stack[--sp];
                            stack[sp++] = lumyr_array_last(arr);
                        } else if(strcmp(fname, "has") == 0) {
                            if(argc >= 2) {
                                Value k = stack[--sp];
                                Value m = stack[--sp];
                                stack[sp++] = lumyr_make_bool(lumyr_map_has(m, k));
                            } else {
                                runtime_error("has() 需要至少 2 个参数");
                            }
                        } else if(strcmp(fname, "flat") == 0) {
                            Value arr = stack[--sp];
                            int depth = (argc >= 2) ? (int)stack[--sp].v.i : 1;
                            stack[sp++] = lumyr_array_flat(arr, depth);
                        } else if(strcmp(fname, "qs") == 0) {
                            Value enc = val_none();
                            Value v;
                            if(argc >= 2) { enc = stack[--sp]; v = stack[--sp]; }
                            else { v = stack[--sp]; }
                            if(v.type == VAL_MAP || v.type == VAL_ARRAY) {
                                char* q = lumyr_qs_stringify_enc(v, enc);
                                stack[sp++] = lumyr_make_string(q);
                                free(q);
                            } else if(v.type == VAL_STRING) {
                                stack[sp++] = lumyr_qs_parse_enc(lumyr_str_cstr(&v), enc);
                            } else {
                                runtime_error("qs() 参数必须是字典/数组（序列化）或字符串（解析）");
                            }
                        } else if(strcmp(fname, "addAll") == 0) {
                            if(argc >= 2) {
                                Value b = stack[--sp];
                                lumyr_array_addall(&stack[sp-1], b);
                            } else {
                                runtime_error("addAll() 需要至少 2 个参数");
                            }
                        } else {
                            is_array_method = 0;
                        }
                        if(is_array_method) {
                            LOG_POP_CALL();
                            break;
                        }
                    }
                }
                // 1. 查函数：帧链 VAL_FUNC → 全局函数表 → 函数表（红黑树）→ class 方法表
                Value func_val;
                _Bool fnd = 0;
                Value gv = stackframe_get(frame, fname, &fnd);
                /* 解耦：当第一个参数是 class 实例时，只查找 class 方法，不查找全局函数
                   如果找不到 class 方法，就抛异常 */
                const char* class_name_for_call = NULL;
                if(argc > 0) {
                    Value obj_for_call = stack[sp - argc];
                    if(obj_for_call.type == VAL_CLASS_PTR) {
                        ClassInstance* inst_for_call = (ClassInstance*)obj_for_call.v.struct_ptr;
                        if(inst_for_call && inst_for_call->vtable) class_name_for_call = inst_for_call->vtable->class_name;
                    }
                }
                if(class_name_for_call) {
                    /* class 实例：跳过帧链和全局函数表查找，直接从函数表中查找 class 方法 */
                    func_val = val_none();
                    fnd = 0;
                    /* 从函数表（红黑树）中查找 class 方法 */
                    BytecodeFunc* bf_fn = ir_func_table_lookup_class(class_name_for_call, fname);
                    if(bf_fn) {
                        /* 把 BytecodeFunc* 包装成 RuntimeFunc* */
                        RuntimeFunc* rf_ptr = (RuntimeFunc*)malloc(sizeof(RuntimeFunc));
                        memset(rf_ptr, 0, sizeof(RuntimeFunc));
                        rf_ptr->entry = vm_func_entry;
                        rf_ptr->param_count = bf_fn->param_cnt;
                        rf_ptr->has_variadic = bf_fn->has_variadic;
                        rf_ptr->captures = NULL;
                        rf_ptr->capture_count = -1;
                        InterpFuncPayload* pl = (InterpFuncPayload*)malloc(sizeof(InterpFuncPayload));
                        memset(pl, 0, sizeof(InterpFuncPayload));
                        pl->body = NULL;
                        pl->bytecode = bf_fn;
                        pl->param_names = bf_fn->params;
                        pl->param_cnt = bf_fn->param_cnt;
                        pl->has_variadic = bf_fn->has_variadic;
                        pl->default_vals = NULL;
                        pl->has_default = NULL;
                        pl->param_is_ref = bf_fn->param_is_ref;
                        pl->captured_names = NULL;
                        pl->captured_cells = NULL;
                        pl->captured_cell_count = 0;
                        pl->is_generator = bf_fn->is_generator;
                        rf_ptr->captures = (Value*)pl;
                        func_val.type = VAL_FUNC;
                        func_val.v.func.func_obj = rf_ptr;
                        func_val.v.func.ffi_func = NULL;
                        func_val.v.func.is_ffi = 0;
                        fnd = 1;
                    } else {
                        /* 找不到 class 方法，抛异常 */
                        char msg[256];
                        snprintf(msg, sizeof(msg), "类 \"%s\" 没有方法 \"%s\"", class_name_for_call, fname);
                        runtime_error(msg);
                    }
                } else if(fnd && gv.type == VAL_FUNC) {
                    func_val = gv;
                } else if(sym_has(fname)) {
                    func_val = sym_get(fname);
                } else {
                    /* 从函数表（红黑树）中查找 */
                    BytecodeFunc* bf_fn = NULL;
                    const char* class_name = NULL;
                    if(argc > 0) {
                        /* 检查第一个参数是否是 class 实例 */
                        Value obj = stack[sp - argc];
                        if(obj.type == VAL_CLASS_PTR) {
                            ClassInstance* inst = (ClassInstance*)obj.v.struct_ptr;
                            if(inst && inst->vtable) class_name = inst->vtable->class_name;
                        }
                    }
                    if(class_name) {
                        /* 优先查找 class 方法（fname 可能已经是 <类名>_<方法名> 格式） */
                        bf_fn = ir_func_table_lookup_class(class_name, fname);
                    }
                    if(!bf_fn) {
                        /* 找不到 class 方法，再查找全局函数 */
                        bf_fn = ir_func_table_lookup(fname);
                    }
                    if(bf_fn) {
                        /* 把 BytecodeFunc* 包装成 RuntimeFunc* */
                        RuntimeFunc* rf_ptr = (RuntimeFunc*)malloc(sizeof(RuntimeFunc));
                        memset(rf_ptr, 0, sizeof(RuntimeFunc));
                        rf_ptr->entry = vm_func_entry;
                        rf_ptr->param_count = bf_fn->param_cnt;
                        rf_ptr->has_variadic = bf_fn->has_variadic;
                        rf_ptr->captures = NULL;
                        rf_ptr->capture_count = -1;
                        InterpFuncPayload* pl = (InterpFuncPayload*)malloc(sizeof(InterpFuncPayload));
                        memset(pl, 0, sizeof(InterpFuncPayload));
                        pl->body = NULL;
                        pl->bytecode = bf_fn;
                        pl->param_names = bf_fn->params;
                        pl->param_cnt = bf_fn->param_cnt;
                        pl->has_variadic = bf_fn->has_variadic;
                        pl->default_vals = NULL;
                        pl->has_default = NULL;
                        pl->param_is_ref = bf_fn->param_is_ref;
                        pl->captured_names = NULL;
                        pl->captured_cells = NULL;
                        pl->captured_cell_count = 0;
                        pl->is_generator = bf_fn->is_generator;
                        rf_ptr->captures = (Value*)pl;
                        func_val.type = VAL_FUNC;
                        func_val.v.func.func_obj = rf_ptr;
                        func_val.v.func.ffi_func = NULL;
                        func_val.v.func.is_ffi = 0;
                    } else if(argc > 0) {
                    /* 可能是 class 方法调用：检查第一个参数是否是 class 实例 */
                    Value obj = stack[sp - argc];
                    const char* class_name = NULL;
                    if(obj.type == VAL_CLASS_PTR) {
                        /* class 实例：从 vtable 中获取 class 名 */
                        ClassInstance* inst = (ClassInstance*)obj.v.struct_ptr;
                        if(inst && inst->vtable) class_name = inst->vtable->class_name;
                    }
                    if(class_name) {
                        /* fname 可能已经是 <类名>_<方法名> 格式，需要去掉类名前缀 */
                        const char* raw_method_name = fname;
                        size_t class_name_len = strlen(class_name);
                        if(strncmp(fname, class_name, class_name_len) == 0 &&
                           fname[class_name_len] == '_') {
                            raw_method_name = fname + class_name_len + 1;
                        }
                        /* 先尝试从全局符号表（红黑树）中查找，键是 (class_name, raw_method_name) */
                        if(sym_has_class(class_name, raw_method_name)) {
                            func_val = sym_get_class(class_name, raw_method_name);
                        } else {
                            /* 再尝试从 class 方法表中查找（递归查找父类方法） */
                            void* rf = class_find_method_func(class_name, raw_method_name);
                            if(rf) {
                                func_val.type = VAL_FUNC;
                                func_val.v.func.func_obj = (RuntimeFunc*)rf;
                                func_val.v.func.ffi_func = NULL;
                                func_val.v.func.is_ffi = 0;
                            } else {
                                /* 最后尝试从函数表（红黑树）中查找 */
                                BytecodeFunc* method_bf = ir_func_table_lookup_class(class_name, raw_method_name);
                                if(method_bf) {
                                    /* 把 BytecodeFunc* 包装成 RuntimeFunc* */
                                    RuntimeFunc* rf_ptr = (RuntimeFunc*)malloc(sizeof(RuntimeFunc));
                                    memset(rf_ptr, 0, sizeof(RuntimeFunc));
                                    rf_ptr->entry = vm_func_entry;
                                    rf_ptr->param_count = method_bf->param_cnt;
                                    rf_ptr->has_variadic = method_bf->has_variadic;
                                    rf_ptr->captures = NULL;
                                    rf_ptr->capture_count = -1;
                                    InterpFuncPayload* pl = (InterpFuncPayload*)malloc(sizeof(InterpFuncPayload));
                                    memset(pl, 0, sizeof(InterpFuncPayload));
                                    pl->body = NULL;
                                    pl->bytecode = method_bf;
                                    pl->param_names = method_bf->params;
                                    pl->param_cnt = method_bf->param_cnt;
                                    pl->has_variadic = method_bf->has_variadic;
                                    pl->default_vals = NULL;
                                    pl->has_default = NULL;
                                    pl->param_is_ref = method_bf->param_is_ref;
                                    pl->captured_names = NULL;
                                    pl->captured_cells = NULL;
                                    pl->captured_cell_count = 0;
                                    pl->is_generator = method_bf->is_generator;
                                    rf_ptr->captures = (Value*)pl;
                                    func_val.type = VAL_FUNC;
                                    func_val.v.func.func_obj = rf_ptr;
                                    func_val.v.func.ffi_func = NULL;
                                    func_val.v.func.is_ffi = 0;
                                } else {
                                    runtime_undefined("函数", fname);
                                }
                            }
                        }
                    } else {
                        runtime_undefined("函数", fname);
                    }
                    } else {
                        runtime_undefined("函数", fname);
                    }
                }
                if(func_val.type != VAL_FUNC) {
                    char buf[256];
                    snprintf(buf, sizeof(buf), "尝试调用非函数: %s", fname);
                    runtime_error(buf);
                }
                /* FFI 外部函数调用 */
                if(func_val.v.func.is_ffi && func_val.v.func.ffi_func) {
                    Value* eval_args = (sp > 0 && argc > 0) ? &stack[sp - argc] : NULL;
                    Value ret = lumyr_ffi_call(func_val.v.func.ffi_func, eval_args, argc);
                    sp -= argc;
                    stack[sp++] = ret;
                    break;
                }
                RuntimeFunc* rf = func_val.v.func.func_obj;
                // 2. 实参：栈顶 argc 个（指针指向 VM 栈，entry 内绑定完成前有效）
                Value* eval_args = (sp > 0) ? &stack[sp - argc] : NULL;
                // 3. 新帧 + 参数绑定（与 interp AST_CALL 一致）
                StackFrame* callee = stackframe_new(frame);
                if(interp_func_is_payload(rf)) {
                    int pcnt = interp_func_param_cnt(rf);
                    int i = 0;
                    for(; i < pcnt; i++) {
                        const char* pname = interp_func_param_name(rf, i);
                        Value bound = (i < argc) ? eval_args[i] : val_none();
                        /* struct 类型参数值传递：浅拷贝（C 语义）；self 参数和 ref 参数除外（引用语义） */
                        if(strcmp(pname, "self") != 0 && !interp_func_param_is_ref(rf, i) &&
                           bound.type == VAL_MAP &&
                           lumyr_map_has(bound, lumyr_make_string("__structname__"))) {
                            bound = lumyr_map_shallow_copy(bound);
                        }
                        stackframe_bind(callee, pname, bound);
                    }
                    if(interp_func_has_variadic(rf)) {
                        const char* vname = interp_func_param_name(rf, pcnt);
                        int rest = argc - i;
                        if(rest < 0) rest = 0;
                        Value arr = val_array(rest);
                        for(int k = 0; k < rest; k++) {
                            arr.v.array->items[k] = eval_args[i + k];
                        }
                        stackframe_bind(callee, vname, arr);
                    }
                }
                closure_bind_cells(rf, callee);
                // 4. 调用 entry：设置当前函数、隔离 break/continue、消费 return
                RuntimeFunc* prev_rf = interp_set_current_rf(rf);
                int saved_break = ctx->hit_break;
                int saved_cont = ctx->hit_continue;
                ctx->hit_break = 0;
                ctx->hit_continue = 0;
                /* 调用栈回溯：入栈函数名（longjmp 跳过 pop 由 GET_ERR 截断） */
                g_trace_push(fname);
                Value ret = rf->entry(argc, eval_args, ctx, callee);
                if(g_trace_n > 0) g_trace_n--;
                ctx->hit_break = saved_break;
                ctx->hit_continue = saved_cont;
                interp_set_current_rf(prev_rf);
                stackframe_destroy(callee);
                // 5. 弹出实参（不销毁，与 interp 口径一致），压入返回值
                sp -= argc;
                stack[sp++] = ret;
                break;
            }
            case OPC_CALLV: {
                // 动态调用链 f(1)(2)：栈顶 b 个为实参，其下一位是函数值
                int argc = in.b;
                Value func_val = (sp - argc - 1 >= 0) ? stack[sp - argc - 1] : val_none();
                if(func_val.type != VAL_FUNC) runtime_error("尝试调用非函数值");
                RuntimeFunc* rf = func_val.v.func.func_obj;
                Value* eval_args = (sp > 0) ? &stack[sp - argc] : NULL;
                StackFrame* callee = stackframe_new(frame);
                if(interp_func_is_payload(rf)) {
                    int pcnt = interp_func_param_cnt(rf);
                    int i = 0;
                    for(; i < pcnt; i++) {
                        const char* pname = interp_func_param_name(rf, i);
                        Value bound = (i < argc) ? eval_args[i] : val_none();
                        /* struct 类型参数值传递：浅拷贝（C 语义）；self 参数和 ref 参数除外（引用语义） */
                        if(strcmp(pname, "self") != 0 && !interp_func_param_is_ref(rf, i) &&
                           bound.type == VAL_MAP &&
                           lumyr_map_has(bound, lumyr_make_string("__structname__"))) {
                            bound = lumyr_map_shallow_copy(bound);
                        }
                        stackframe_bind(callee, pname, bound);
                    }
                    if(interp_func_has_variadic(rf)) {
                        const char* vname = interp_func_param_name(rf, pcnt);
                        int rest = argc - i;
                        if(rest < 0) rest = 0;
                        Value arr = val_array(rest);
                        for(int k = 0; k < rest; k++) {
                            arr.v.array->items[k] = eval_args[i + k];
                        }
                        stackframe_bind(callee, vname, arr);
                    }
                }
                closure_bind_cells(rf, callee);
                RuntimeFunc* prev_rf = interp_set_current_rf(rf);
                int saved_break = ctx->hit_break;
                int saved_cont = ctx->hit_continue;
                ctx->hit_break = 0;
                ctx->hit_continue = 0;
                Value ret = rf->entry(argc, eval_args, ctx, callee);
                ctx->hit_break = saved_break;
                ctx->hit_continue = saved_cont;
                interp_set_current_rf(prev_rf);
                stackframe_destroy(callee);
                sp -= argc + 1;   // 弹实参 + 函数值
                stack[sp++] = ret;
                break;
            }
            case OPC_YIELD: {
                /* 生成器 yield：保存状态，longjmp 返回到 generator_resume
                 * 恢复时，send_value 会被压入栈顶作为 yield 表达式的返回值 */
                if(!is_generator) {
                    LOG_ERROR("Runtime Error: yield 只能在生成器函数中使用\n");
                    exit(EXIT_FAILURE);
                }
                Value v = stack[--sp];
                /* 保存生成器状态 */
                gen_ctx->pc = pc;
                gen_ctx->sp = sp;
                gen_ctx->started = 1;
                /* 保存 try-catch 上下文（yield 时的状态） */
                generator_save_try_context(gen_ctx);
                /* 注册为 GC 根：暂停后 stack/frame 不再是当前 VM 根，
                 * 但内部仍持有 GC 对象引用，不注册会被错误回收导致堆破坏 */
                paused_gen_add(gen_ctx);
                /* 恢复外层 VM 状态 */
                vm_depth = saved_depth;
                g_err_jmp = saved_gj;
                vm_fin_n = saved_fin;
                gc_set_roots(old_gc_stack, old_gc_sp, old_gc_frame);
                tls_vm_run_depth--;
                if (!tls_skip_vm_unregister) gc_unregister_thread();
                /* 设置 yield 结果并 longjmp 返回到 generator_resume */
                s_gen_yield_result = v;
                lumyr_set_current_class(old_current_class);
                longjmp(gen_ctx->resume_point, 1);
            }
            case OPC_RETURN: {
                Value v = stack[--sp];
                /* 如果是生成器，标记结束 */
                if(is_generator) {
                    gen_ctx->finished = 1;
                    gen_ctx->pc = pc;
                    gen_ctx->sp = sp;
                }
                vm_depth = saved_depth;
                g_err_jmp = saved_gj;
                vm_fin_n = saved_fin;
                gc_set_roots(old_gc_stack, old_gc_sp, old_gc_frame);
                tls_vm_run_depth--;
                if (tls_vm_run_depth == 0) {
                    /* 最外层：先 protect_push(v) 注册 protect entry（head），
                     * 再 gc_unregister_thread_keep_protect 移除 VM entry（第二个）。
                     * 这样 protect 与 unregister 之间无窗口，v 始终有 GC 根保护。 */
                    gc_protect_push(v);
                    gc_unregister_thread_keep_protect();
                } else {
                    /* 嵌套调用：正常 unregister + 短暂 protect */
                    if (!tls_skip_vm_unregister) gc_unregister_thread();
                    gc_protect_push(v);
                    gc_protect_pop();
                }
                if(!is_generator) free(stack);
                lumyr_set_current_class(old_current_class);
                return v;
            }
            case OPC_RETURN_NIL:
                if(is_generator) {
                    gen_ctx->finished = 1;
                    gen_ctx->pc = pc;
                    gen_ctx->sp = sp;
                }
                vm_depth = saved_depth;
                g_err_jmp = saved_gj;
                vm_fin_n = saved_fin;
                gc_set_roots(old_gc_stack, old_gc_sp, old_gc_frame);
                tls_vm_run_depth--;
                if (tls_vm_run_depth > 0 || !tls_skip_vm_unregister) gc_unregister_thread();
                if(!is_generator) free(stack);
                lumyr_set_current_class(old_current_class);
                return val_none();
            case OPC_HALT:
                if(is_generator) {
                    gen_ctx->finished = 1;
                    gen_ctx->pc = pc;
                    gen_ctx->sp = sp;
                }
                vm_depth = saved_depth;
                g_err_jmp = saved_gj;
                vm_fin_n = saved_fin;
                gc_set_roots(old_gc_stack, old_gc_sp, old_gc_frame);
                tls_vm_run_depth--;
                if (tls_vm_run_depth > 0 || !tls_skip_vm_unregister) gc_unregister_thread();
                if(!is_generator) free(stack);
                lumyr_set_current_class(old_current_class);
                return val_none();
            default:
                runtime_error("vm: 未知指令");
        }
    }
}
