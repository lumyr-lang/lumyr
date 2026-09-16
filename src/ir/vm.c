// 字节码 VM 执行器
// 指令语义与 ast_interp.c 对齐（栈帧链变量、调用绑定、return 深拷贝、break/continue 编译期跳转）。
#include "vm_types.h"
#include "vm_try_context.h"
#include "vm_internal.h"
#include "vm_exec.h"
#include "vm_generator.h"
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
_Thread_local int tls_vm_run_depth = 0;
_Thread_local int tls_skip_vm_unregister = 0;
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
#define LONG_LONG_PUSH(val) do { stack_global_ensure(STACK_LONG_LONG, 1); ((long long*)stack_global_get_stack(STACK_LONG_LONG))[(*stack_global_get_sp(STACK_LONG_LONG))++] = (val); } while(0)
#define LONG_LONG_POP() (((long long*)stack_global_get_stack(STACK_LONG_LONG))[--(*stack_global_get_sp(STACK_LONG_LONG))])


/* ========== 生成器支持 ========== */






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





/* 当前线程的全局帧（主线程 = vm_run_main 的 top；线程体启动时从 data 继承并写入本线程 TLS） */
_Thread_local StackFrame* s_global_frame = NULL;

// 线程体（VM 通道）：线程内执行 RuntimeFunc，与 vm_call_rf 语义一致
void vm_thread_body(ThreadLaunch* t)
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
Value vm_call_rf(RuntimeFunc* rf, Value* args, int argc, StackFrame* parent, EvalCtx* ctx)
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
int try_operator_overload(const char* op_name, Value l, Value r,
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

