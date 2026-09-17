/*
 * VM 执行模块实现
 *
 * 本模块提供 VM 的核心执行功能，包括字节码指令的执行和内置函数的执行。
 */

#include "vm_exec.h"
#include "vm_macros.h"
#include "vm_internal.h"
#include "vm_generator.h"
#include "vm_try_context.h"
#include "vm_types.h"
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
#include <string.h>
#include <setjmp.h>



Value vm_run(BytecodeFunc* bf, StackFrame* frame, EvalCtx* ctx)
{
    /* 访问修饰符检查：保存旧的当前类名，设置为当前函数所属的类名 */
    const char* old_current_class = lumyr_get_current_class();
    lumyr_set_current_class(bf->class_name);
    /* 生成器上下文恢复：如果 s_current_gen 不为 NULL，从生成器对象恢复状态 */
    GeneratorObject* gen_ctx = s_current_gen;
    int is_generator = (gen_ctx != NULL);
    // 静态栈深度分析：精确分配执行栈（动态，无硬上限），并校验 IR 栈平衡
    int maxd = bc_analyze_stack(bf, NULL, 0);
        if(maxd < 0) {
        fprintf(stderr, "[DEBUG VM] bc_analyze_stack returned negative, exiting\n");
        exit(EXIT_FAILURE);   // 已打印下溢位置
    }
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
        if(pc >= bf->code_len) {
                        break;
        }
        Instruction in = bf->code[pc++];
        fprintf(stderr, "[VM] pc=%d, op=%d, a=%d, b=%d, sp=%d, value_sp=%d, int_sp=%d, double_sp=%d, ld_sp=%d\n", 
                pc-1, (int)in.op, in.a, in.b, sp,
                (g_stack_mgr ? *stack_global_get_sp(STACK_VALUE) : -1),
                (g_stack_mgr ? *stack_global_get_sp(STACK_INT) : -1),
                (g_stack_mgr ? *stack_global_get_sp(STACK_DOUBLE) : -1),
                (g_stack_mgr ? *stack_global_get_sp(STACK_LONG_DOUBLE) : -1));
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
            case OPC_PUSH_UINT_CONST: {
                /* uint 常量：直接把常量值压入 uint 栈，零检查零转换
                   用于 <uint>42 字面量赋值等场景，避免创建 Value 再提取的开销 */
                UINT_PUSH((unsigned int)in.a);
                break;
            }
            case OPC_PUSH_UINT32_CONST: {
                /* uint32 常量：直接把常量值压入 uint32 栈，零检查零转换
                   用于 <uint32>42 字面量赋值等场景，避免创建 Value 再提取的开销 */
                UINT32_PUSH((uint32_t)in.a);
                break;
            }
            case OPC_INT_ADD: {
                /* int 加法：直接从 int 栈弹出两个 int，相加，结果压回 int 栈
                   零检查零转换零 Value 开销，完全不涉及 Value 栈 */
                int b = INT_POP();
                int a = INT_POP();
                INT_PUSH(a + b);
                break;
            }
            case OPC_INT_SUB: {
                /* int 减法：直接从 int 栈弹出两个 int，相减，结果压回 int 栈 */
                int b = INT_POP();
                int a = INT_POP();
                INT_PUSH(a - b);
                break;
            }
            case OPC_INT_MUL: {
                /* int 乘法：直接从 int 栈弹出两个 int，相乘，结果压回 int 栈 */
                int b = INT_POP();
                int a = INT_POP();
                INT_PUSH(a * b);
                break;
            }
            case OPC_INT_DIV: {
                /* int 除法：直接从 int 栈弹出两个 int，相除，结果压回 int 栈
                   需要检查除零 */
                int b = INT_POP();
                int a = INT_POP();
                if(b == 0) runtime_error("除零错误：int 除法除数为零");
                INT_PUSH(a / b);
                break;
            }
            case OPC_INT_MOD: {
                /* int 取模：直接从 int 栈弹出两个 int，取模，结果压回 int 栈
                   需要检查除零 */
                int b = INT_POP();
                int a = INT_POP();
                if(b == 0) runtime_error("除零错误：int 取模除数为零");
                INT_PUSH(a % b);
                break;
            }
            case OPC_INT_TO_VALUE: {
                /* 把 int 专用栈顶的 int 值包装成 Value，压入 Value 栈
                   用于兼容赋值等通用逻辑（赋值给普通变量时需要从 Value 栈弹出值） */
                int iv = INT_POP();
                stack[sp++] = lumyr_make_int((long long)iv);
                break;
            }
            case OPC_SHORT_TO_VALUE: {
                /* 把 short 专用栈顶的 short 值包装成 Value，压入 Value 栈 */
                short sv = SHORT_POP();
                stack[sp++] = lumyr_make_short(sv);
                break;
            }
            case OPC_INT_GT: {
                /* int 大于比较：直接从 int 栈弹出两个 int，比较后结果(bool)压入 Value 栈 */
                int b = INT_POP();
                int a = INT_POP();
                stack[sp++] = lumyr_make_bool(a > b);
                break;
            }
            case OPC_INT_LT: {
                /* int 小于比较 */
                int b = INT_POP();
                int a = INT_POP();
                stack[sp++] = lumyr_make_bool(a < b);
                break;
            }
            case OPC_INT_GE: {
                /* int 大于等于比较 */
                int b = INT_POP();
                int a = INT_POP();
                stack[sp++] = lumyr_make_bool(a >= b);
                break;
            }
            case OPC_INT_LE: {
                /* int 小于等于比较 */
                int b = INT_POP();
                int a = INT_POP();
                stack[sp++] = lumyr_make_bool(a <= b);
                break;
            }
            case OPC_INT_EQ: {
                /* int 等于比较 */
                int b = INT_POP();
                int a = INT_POP();
                stack[sp++] = lumyr_make_bool(a == b);
                break;
            }
            case OPC_INT_NE: {
                /* int 不等于比较 */
                int b = INT_POP();
                int a = INT_POP();
                stack[sp++] = lumyr_make_bool(a != b);
                break;
            }
            case OPC_INT_ARRAY_SET: {
                /* int 类型化数组元素赋值：从 int 专用栈弹出值，从 Value 栈弹出索引和数组，
                   直接写入 int 类型化数组，零转换开销 */
                int val = INT_POP();
                Value idx = stack[--sp];
                Value arr = stack[--sp];
                if(arr.type == VAL_TYPED_ARRAY && arr.v.typed_array &&
                   arr.v.typed_array->elem_type == VAL_INT) {
                    TypedArray* tarr = arr.v.typed_array;
                    long long i = array_index_of(idx);
                    if(i < 0 || i >= tarr->len) {
                        char buf[128];
                        snprintf(buf, sizeof(buf), "int类型化数组下标越界: %lld (长度 %d)", i, tarr->len);
                        runtime_error(buf);
                    }
                    ((int*)tarr->items)[i] = val;
                } else {
                    runtime_error("OPC_INT_ARRAY_SET: 数组不是int类型化数组");
                }
                /* 把被设置的值包装成 Value，压入 Value 栈（用于表达式值） */
                stack[sp++] = lumyr_make_int((long long)val);
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
            case OPC_PUSH_DOUBLE_CONST: {
                /* double 常量：从 a=低32位, b=高32位 重组 double 值，直接压入 double 栈
                   用于 <double>3.14 字面量赋值等场景，避免创建 Value 再提取的开销 */
                uint64_t bits = ((uint64_t)(uint32_t)in.b << 32) | (uint32_t)in.a;
                double dv;
                memcpy(&dv, &bits, sizeof(double));
                DOUBLE_PUSH(dv);
                break;
            }
            case OPC_DOUBLE_ADD: {
                /* double 加法：直接从 double 栈弹出两个 double，相加，结果压回 double 栈
                   零检查零转换零 Value 开销，完全不涉及 Value 栈 */
                double b = DOUBLE_POP();
                double a = DOUBLE_POP();
                DOUBLE_PUSH(a + b);
                break;
            }
            case OPC_DOUBLE_SUB: {
                /* double 减法：直接从 double 栈弹出两个 double，相减，结果压回 double 栈 */
                double b = DOUBLE_POP();
                double a = DOUBLE_POP();
                DOUBLE_PUSH(a - b);
                break;
            }
            case OPC_DOUBLE_MUL: {
                /* double 乘法：直接从 double 栈弹出两个 double，相乘，结果压回 double 栈 */
                double b = DOUBLE_POP();
                double a = DOUBLE_POP();
                DOUBLE_PUSH(a * b);
                break;
            }
            case OPC_DOUBLE_DIV: {
                /* double 除法：直接从 double 栈弹出两个 double，相除，结果压回 double 栈
                   需要检查除零 */
                double b = DOUBLE_POP();
                double a = DOUBLE_POP();
                if(b == 0.0) runtime_error("除零错误：double 除法除数为零");
                DOUBLE_PUSH(a / b);
                break;
            }
            case OPC_DOUBLE_TO_VALUE: {
                /* 把 double 专用栈顶的 double 值包装成 Value，压入 Value 栈
                   用于兼容赋值等通用逻辑（赋值给普通变量时需要从 Value 栈弹出值） */
                double dv = DOUBLE_POP();
                stack[sp++] = lumyr_make_double(dv);
                break;
            }
            case OPC_DOUBLE_GT: {
                /* double 大于比较：直接从 double 栈弹出两个 double，比较后结果(bool)压入 Value 栈 */
                double b = DOUBLE_POP();
                double a = DOUBLE_POP();
                stack[sp++] = lumyr_make_bool(a > b);
                break;
            }
            case OPC_DOUBLE_LT: {
                /* double 小于比较：直接从 double 栈弹出两个 double，比较后结果(bool)压入 Value 栈 */
                double b = DOUBLE_POP();
                double a = DOUBLE_POP();
                stack[sp++] = lumyr_make_bool(a < b);
                break;
            }
            case OPC_DOUBLE_GE: {
                /* double 大于等于比较：直接从 double 栈弹出两个 double，比较后结果(bool)压入 Value 栈 */
                double b = DOUBLE_POP();
                double a = DOUBLE_POP();
                stack[sp++] = lumyr_make_bool(a >= b);
                break;
            }
            case OPC_DOUBLE_LE: {
                /* double 小于等于比较：直接从 double 栈弹出两个 double，比较后结果(bool)压入 Value 栈 */
                double b = DOUBLE_POP();
                double a = DOUBLE_POP();
                stack[sp++] = lumyr_make_bool(a <= b);
                break;
            }
            case OPC_DOUBLE_EQ: {
                /* double 等于比较：直接从 double 栈弹出两个 double，比较后结果(bool)压入 Value 栈 */
                double b = DOUBLE_POP();
                double a = DOUBLE_POP();
                stack[sp++] = lumyr_make_bool(a == b);
                break;
            }
            case OPC_DOUBLE_NE: {
                /* double 不等于比较：直接从 double 栈弹出两个 double，比较后结果(bool)压入 Value 栈 */
                double b = DOUBLE_POP();
                double a = DOUBLE_POP();
                stack[sp++] = lumyr_make_bool(a != b);
                break;
            }
            case OPC_DOUBLE_ARRAY_SET: {
                /* double 类型化数组元素赋值：从 Value 栈弹出数组和索引，从 double 栈弹出值，写入数组
                   零转换，直接写入 double 类型化数组 */
                double val = DOUBLE_POP();
                Value idx = stack[--sp];
                Value arr = stack[--sp];
                if(arr.type == VAL_TYPED_ARRAY && arr.v.typed_array->elem_type == VAL_DOUBLE) {
                    TypedArray* tarr = arr.v.typed_array;
                    long long i = array_index_of(idx);
                    ((double*)tarr->items)[i] = val;
                } else {
                    runtime_error("类型错误：期望 double 类型化数组");
                }
                stack[sp++] = lumyr_make_double(val);
                break;
            }
            case OPC_PUSH_FLOAT_CONST: {
                /* float 常量：从 a=位模式 重组 float 值，直接压入 float 栈
                   用于 <float>3.14 字面量赋值等场景，避免创建 Value 再提取的开销 */
                uint32_t bits = (uint32_t)in.a;
                float fv;
                memcpy(&fv, &bits, sizeof(float));
                FLOAT_PUSH(fv);
                break;
            }
            case OPC_FLOAT_ADD: {
                /* float 加法：直接从 float 栈弹出两个 float，相加，结果压回 float 栈
                   零检查零转换零 Value 开销，完全不涉及 Value 栈 */
                float b = FLOAT_POP();
                float a = FLOAT_POP();
                fprintf(stderr, "[DEBUG VM] FLOAT_ADD: a=%f, b=%f, result=%f\n", (double)a, (double)b, (double)(a + b));
                FLOAT_PUSH(a + b);
                break;
            }
            case OPC_FLOAT_SUB: {
                /* float 减法 */
                float b = FLOAT_POP();
                float a = FLOAT_POP();
                FLOAT_PUSH(a - b);
                break;
            }
            case OPC_FLOAT_MUL: {
                /* float 乘法 */
                float b = FLOAT_POP();
                float a = FLOAT_POP();
                FLOAT_PUSH(a * b);
                break;
            }
            case OPC_FLOAT_DIV: {
                /* float 除法，需要检查除零 */
                float b = FLOAT_POP();
                float a = FLOAT_POP();
                if(b == 0.0f) runtime_error("除零错误：float 除法除数为零");
                FLOAT_PUSH(a / b);
                break;
            }
            case OPC_FLOAT_TO_VALUE: {
                /* 把 float 专用栈顶的 float 值包装成 Value，压入 Value 栈
                   用于兼容赋值等通用逻辑 */
                float fv = FLOAT_POP();
                stack[sp++] = lumyr_make_float(fv);
                break;
            }
            case OPC_FLOAT_GT: {
                /* float 大于比较：直接从 float 栈弹出两个 float，比较后结果(bool)压入 Value 栈 */
                float b = FLOAT_POP();
                float a = FLOAT_POP();
                stack[sp++] = lumyr_make_bool(a > b);
                break;
            }
            case OPC_FLOAT_LT: {
                /* float 小于比较 */
                float b = FLOAT_POP();
                float a = FLOAT_POP();
                stack[sp++] = lumyr_make_bool(a < b);
                break;
            }
            case OPC_FLOAT_GE: {
                /* float 大于等于比较 */
                float b = FLOAT_POP();
                float a = FLOAT_POP();
                stack[sp++] = lumyr_make_bool(a >= b);
                break;
            }
            case OPC_FLOAT_LE: {
                /* float 小于等于比较 */
                float b = FLOAT_POP();
                float a = FLOAT_POP();
                stack[sp++] = lumyr_make_bool(a <= b);
                break;
            }
            case OPC_FLOAT_EQ: {
                /* float 等于比较 */
                float b = FLOAT_POP();
                float a = FLOAT_POP();
                stack[sp++] = lumyr_make_bool(a == b);
                break;
            }
            case OPC_FLOAT_NE: {
                /* float 不等于比较 */
                float b = FLOAT_POP();
                float a = FLOAT_POP();
                stack[sp++] = lumyr_make_bool(a != b);
                break;
            }
            case OPC_FLOAT_ARRAY_SET: {
                /* float 类型化数组元素赋值：从 Value 栈弹出数组和索引，从 float 栈弹出值，写入数组
                   零转换，直接写入 float 类型化数组 */
                float val = FLOAT_POP();
                Value idx = stack[--sp];
                Value arr = stack[--sp];
                if(arr.type == VAL_TYPED_ARRAY && arr.v.typed_array->elem_type == VAL_FLOAT) {
                    TypedArray* tarr = arr.v.typed_array;
                    long long i = array_index_of(idx);
                    ((float*)tarr->items)[i] = val;
                } else {
                    runtime_error("类型错误：期望 float 类型化数组");
                }
                stack[sp++] = lumyr_make_float(val);
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
            case OPC_PUSH_BOOL_CONST: {
                /* bool常量零开销压栈：直接把常量值压入bool专用栈，不创建Value */
                BOOL_PUSH(in.a ? 1 : 0);
                break;
            }
            case OPC_PUSH_CHAR_CONST: {
                /* char常量零开销压栈：直接把常量值压入char专用栈，不创建Value */
                CHAR_PUSH((char)in.a);
                break;
            }
            case OPC_PUSH_BYTE_CONST: {
                /* byte常量零开销压栈：直接把常量值压入byte专用栈，不创建Value */
                BYTE_PUSH((unsigned char)in.a);
                break;
            }
            case OPC_PUSH_INT8_CONST: {
                /* int8常量零开销压栈：直接把常量值压入int8专用栈，不创建Value */
                INT8_PUSH((int8_t)in.a);
                break;
            }
            case OPC_PUSH_INT16_CONST: {
                /* int16常量零开销压栈：直接把常量值压入int16专用栈，不创建Value */
                INT16_PUSH((int16_t)in.a);
                break;
            }
            case OPC_PUSH_SHORT_CONST: {
                /* short常量零开销压栈：直接把常量值压入short专用栈，不创建Value */
                SHORT_PUSH((short)in.a);
                break;
            }
            case OPC_PUSH_INT32_CONST: {
                /* int32常量零开销压栈：直接把常量值压入int32专用栈，不创建Value */
                INT32_PUSH((int32_t)in.a);
                break;
            }
            case OPC_PUSH_INT64_CONST: {
                /* int64常量零开销压栈：直接把常量值压入int64专用栈，不创建Value
                   64位值合并：in.a低32位 + in.b高32位，in.b强制转换为unsigned int避免符号扩展 */
                int64_t i64val = (int64_t)((uint32_t)in.a) | ((int64_t)(uint32_t)in.b << 32);
                INT64_PUSH(i64val);
                break;
            }
            case OPC_PUSH_UINT8_CONST: {
                UINT8_PUSH((uint8_t)in.a);
                break;
            }
            case OPC_PUSH_UINT16_CONST: {
                UINT16_PUSH((uint16_t)in.a);
                break;
            }
            case OPC_PUSH_UINT64_CONST: {
                uint64_t u64val = (uint64_t)((uint32_t)in.a) | ((uint64_t)(uint32_t)in.b << 32);
                UINT64_PUSH(u64val);
                break;
            }
            case OPC_PUSH_LONG_CONST: {
                LONG_PUSH((long)in.a);
                break;
            }
            case OPC_PUSH_ULONG_CONST: {
                ULONG_PUSH((unsigned long)in.a);
                break;
            }
            case OPC_PUSH_SIZE_T_CONST: {
                SIZE_T_PUSH((size_t)in.a);
                break;
            }
            case OPC_PUSH_SSIZE_T_CONST: {
                SSIZE_T_PUSH((ssize_t)in.a);
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
            case OPC_LOAD_SHORT_VAR: {
                const char* name = bf->syms[in.a];
                _Bool fnd = 0;
                short sv = stackframe_get_short(frame, name, &fnd);
                if(!fnd) runtime_undefined("变量", name);
                SHORT_PUSH(sv);
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
            case OPC_STORE_SHORT_VAR: {
                const char* name = bf->syms[in.a];
                short sv = SHORT_POP();
                stackframe_bind_short(frame, name, sv);
                Value ret;
                ret.type = VAL_SHORT;
                ret.v.sh = sv;
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
                                 /* 先查询变量类型（CastKind），直接操作专用数组，零转换开销 */
                                 int ttag = stackframe_get_type_tag(frame, n);
                                 Value __nv;
                                 switch(ttag) {
                                     case CAST_INT: {
                                         int* p = stackframe_get_int_ptr(frame, n);
                                         if(!p) { /* 回退到 Value 路径 */ goto inc_val_pre; }
                                         int_inc(p);
                                         __nv = lumyr_make_int(*p);
                                         break;
                                     }
                                     case CAST_INT8: {
                                         int8_t* p = stackframe_get_int8_ptr(frame, n);
                                         if(!p) goto inc_val_pre;
                                         int8_inc(p);
                                         __nv = lumyr_make_int8(*p);
                                         break;
                                     }
                                     case CAST_INT16: {
                                         int16_t* p = stackframe_get_int16_ptr(frame, n);
                                         if(!p) goto inc_val_pre;
                                         int16_inc(p);
                                         __nv = lumyr_make_int16(*p);
                                         break;
                                     }
                                     /* CAST_SHORT 与 CAST_INT16 共用 int16_vals */
                                     case CAST_INT32: {
                                         int32_t* p = stackframe_get_int32_ptr(frame, n);
                                         if(!p) goto inc_val_pre;
                                         int32_inc(p);
                                         __nv = lumyr_make_int32(*p);
                                         break;
                                     }
                                     case CAST_INT64: {
                                         int64_t* p = stackframe_get_int64_ptr(frame, n);
                                         if(!p) goto inc_val_pre;
                                         int64_inc(p);
                                         __nv = lumyr_make_int64(*p);
                                         break;
                                     }
                                     case CAST_UINT: {
                                         unsigned int* p = stackframe_get_uint_ptr(frame, n);
                                         if(!p) goto inc_val_pre;
                                         uint_inc(p);
                                         __nv = lumyr_make_uint(*p);
                                         break;
                                     }
                                     case CAST_UINT8: {
                                         uint8_t* p = stackframe_get_uint8_ptr(frame, n);
                                         if(!p) goto inc_val_pre;
                                         uint8_inc(p);
                                         __nv = lumyr_make_uint8(*p);
                                         break;
                                     }
                                     case CAST_UINT16: {
                                         uint16_t* p = stackframe_get_uint16_ptr(frame, n);
                                         if(!p) goto inc_val_pre;
                                         uint16_inc(p);
                                         __nv = lumyr_make_uint16(*p);
                                         break;
                                     }
                                     case CAST_UINT32: {
                                         uint32_t* p = stackframe_get_uint32_ptr(frame, n);
                                         if(!p) goto inc_val_pre;
                                         uint32_inc(p);
                                         __nv = lumyr_make_uint32(*p);
                                         break;
                                     }
                                     case CAST_UINT64: {
                                         uint64_t* p = stackframe_get_uint64_ptr(frame, n);
                                         if(!p) goto inc_val_pre;
                                         uint64_inc(p);
                                         __nv = lumyr_make_uint64(*p);
                                         break;
                                     }
                                     default: goto inc_val_pre;
                                 }
                                 stack[sp++] = __nv; break;
                                 inc_val_pre: {
                                     Value __old = stackframe_get(frame, n, &fnd);
                                     if(!fnd) runtime_undefined("变量", n);
                                     __nv = lumyr_pre_inc(&__old);
                                     stackframe_bind(frame, n, __nv);
                                     stack[sp++] = __nv;
                                 } break; }
            case OPC_POST_INC: { const char* n = bf->syms[in.a]; _Bool fnd = 0;
                                 /* 先查询变量类型（CastKind），直接操作专用数组，零转换开销 */
                                 int ttag = stackframe_get_type_tag(frame, n);
                                 Value __old_v;
                                 switch(ttag) {
                                     case CAST_INT: {
                                         int* p = stackframe_get_int_ptr(frame, n);
                                         if(!p) goto inc_val_post;
                                         int v = *p;
                                         int_inc(p);
                                         __old_v = lumyr_make_int(v);
                                         break;
                                     }
                                     case CAST_INT8: {
                                         int8_t* p = stackframe_get_int8_ptr(frame, n);
                                         if(!p) goto inc_val_post;
                                         int8_t v = *p;
                                         int8_inc(p);
                                         __old_v = lumyr_make_int8(v);
                                         break;
                                     }
                                     case CAST_INT16: {
                                         int16_t* p = stackframe_get_int16_ptr(frame, n);
                                         if(!p) goto inc_val_post;
                                         int16_t v = *p;
                                         int16_inc(p);
                                         __old_v = lumyr_make_int16(v);
                                         break;
                                     }
                                     case CAST_INT32: {
                                         int32_t* p = stackframe_get_int32_ptr(frame, n);
                                         if(!p) goto inc_val_post;
                                         int32_t v = *p;
                                         int32_inc(p);
                                         __old_v = lumyr_make_int32(v);
                                         break;
                                     }
                                     case CAST_INT64: {
                                         int64_t* p = stackframe_get_int64_ptr(frame, n);
                                         if(!p) goto inc_val_post;
                                         int64_t v = *p;
                                         int64_inc(p);
                                         __old_v = lumyr_make_int64(v);
                                         break;
                                     }
                                     case CAST_UINT: {
                                         unsigned int* p = stackframe_get_uint_ptr(frame, n);
                                         if(!p) goto inc_val_post;
                                         unsigned int v = *p;
                                         uint_inc(p);
                                         __old_v = lumyr_make_uint(v);
                                         break;
                                     }
                                     case CAST_UINT8: {
                                         uint8_t* p = stackframe_get_uint8_ptr(frame, n);
                                         if(!p) goto inc_val_post;
                                         uint8_t v = *p;
                                         uint8_inc(p);
                                         __old_v = lumyr_make_uint8(v);
                                         break;
                                     }
                                     case CAST_UINT16: {
                                         uint16_t* p = stackframe_get_uint16_ptr(frame, n);
                                         if(!p) goto inc_val_post;
                                         uint16_t v = *p;
                                         uint16_inc(p);
                                         __old_v = lumyr_make_uint16(v);
                                         break;
                                     }
                                     case CAST_UINT32: {
                                         uint32_t* p = stackframe_get_uint32_ptr(frame, n);
                                         if(!p) goto inc_val_post;
                                         uint32_t v = *p;
                                         uint32_inc(p);
                                         __old_v = lumyr_make_uint32(v);
                                         break;
                                     }
                                     case CAST_UINT64: {
                                         uint64_t* p = stackframe_get_uint64_ptr(frame, n);
                                         if(!p) goto inc_val_post;
                                         uint64_t v = *p;
                                         uint64_inc(p);
                                         __old_v = lumyr_make_uint64(v);
                                         break;
                                     }
                                     default: goto inc_val_post;
                                 }
                                 stack[sp++] = __old_v; break;
                                 inc_val_post: {
                                     Value __old = stackframe_get(frame, n, &fnd);
                                     if(!fnd) runtime_undefined("变量", n);
                                     Value __nv = lumyr_post_inc(&__old);
                                     stackframe_bind(frame, n, __old);
                                     stack[sp++] = __nv;
                                 } break; }               // 返回值 = 旧值
            case OPC_PRE_DEC:  { const char* n = bf->syms[in.a]; _Bool fnd = 0;
                                 /* 先查询变量类型（CastKind），直接操作专用数组，零转换开销 */
                                 int ttag = stackframe_get_type_tag(frame, n);
                                 Value __nv;
                                 switch(ttag) {
                                     case CAST_INT: {
                                         int* p = stackframe_get_int_ptr(frame, n);
                                         if(!p) goto dec_val_pre;
                                         int_dec(p);
                                         __nv = lumyr_make_int(*p);
                                         break;
                                     }
                                     case CAST_INT8: {
                                         int8_t* p = stackframe_get_int8_ptr(frame, n);
                                         if(!p) goto dec_val_pre;
                                         int8_dec(p);
                                         __nv = lumyr_make_int8(*p);
                                         break;
                                     }
                                     case CAST_INT16: {
                                         int16_t* p = stackframe_get_int16_ptr(frame, n);
                                         if(!p) goto dec_val_pre;
                                         int16_dec(p);
                                         __nv = lumyr_make_int16(*p);
                                         break;
                                     }
                                     case CAST_INT32: {
                                         int32_t* p = stackframe_get_int32_ptr(frame, n);
                                         if(!p) goto dec_val_pre;
                                         int32_dec(p);
                                         __nv = lumyr_make_int32(*p);
                                         break;
                                     }
                                     case CAST_INT64: {
                                         int64_t* p = stackframe_get_int64_ptr(frame, n);
                                         if(!p) goto dec_val_pre;
                                         int64_dec(p);
                                         __nv = lumyr_make_int64(*p);
                                         break;
                                     }
                                     case CAST_UINT: {
                                         unsigned int* p = stackframe_get_uint_ptr(frame, n);
                                         if(!p) goto dec_val_pre;
                                         uint_dec(p);
                                         __nv = lumyr_make_uint(*p);
                                         break;
                                     }
                                     case CAST_UINT8: {
                                         uint8_t* p = stackframe_get_uint8_ptr(frame, n);
                                         if(!p) goto dec_val_pre;
                                         uint8_dec(p);
                                         __nv = lumyr_make_uint8(*p);
                                         break;
                                     }
                                     case CAST_UINT16: {
                                         uint16_t* p = stackframe_get_uint16_ptr(frame, n);
                                         if(!p) goto dec_val_pre;
                                         uint16_dec(p);
                                         __nv = lumyr_make_uint16(*p);
                                         break;
                                     }
                                     case CAST_UINT32: {
                                         uint32_t* p = stackframe_get_uint32_ptr(frame, n);
                                         if(!p) goto dec_val_pre;
                                         uint32_dec(p);
                                         __nv = lumyr_make_uint32(*p);
                                         break;
                                     }
                                     case CAST_UINT64: {
                                         uint64_t* p = stackframe_get_uint64_ptr(frame, n);
                                         if(!p) goto dec_val_pre;
                                         uint64_dec(p);
                                         __nv = lumyr_make_uint64(*p);
                                         break;
                                     }
                                     default: goto dec_val_pre;
                                 }
                                 stack[sp++] = __nv; break;
                                 dec_val_pre: {
                                     Value __old = stackframe_get(frame, n, &fnd);
                                     if(!fnd) runtime_undefined("变量", n);
                                     __nv = lumyr_pre_dec(&__old);
                                     stackframe_bind(frame, n, __nv);
                                     stack[sp++] = __nv;
                                 } break; }
            case OPC_POST_DEC: { const char* n = bf->syms[in.a]; _Bool fnd = 0;
                                 /* 先查询变量类型（CastKind），直接操作专用数组，零转换开销 */
                                 int ttag = stackframe_get_type_tag(frame, n);
                                 Value __old_v;
                                 switch(ttag) {
                                     case CAST_INT: {
                                         int* p = stackframe_get_int_ptr(frame, n);
                                         if(!p) goto dec_val_post;
                                         int v = *p;
                                         int_dec(p);
                                         __old_v = lumyr_make_int(v);
                                         break;
                                     }
                                     case CAST_INT8: {
                                         int8_t* p = stackframe_get_int8_ptr(frame, n);
                                         if(!p) goto dec_val_post;
                                         int8_t v = *p;
                                         int8_dec(p);
                                         __old_v = lumyr_make_int8(v);
                                         break;
                                     }
                                     case CAST_INT16: {
                                         int16_t* p = stackframe_get_int16_ptr(frame, n);
                                         if(!p) goto dec_val_post;
                                         int16_t v = *p;
                                         int16_dec(p);
                                         __old_v = lumyr_make_int16(v);
                                         break;
                                     }
                                     case CAST_INT32: {
                                         int32_t* p = stackframe_get_int32_ptr(frame, n);
                                         if(!p) goto dec_val_post;
                                         int32_t v = *p;
                                         int32_dec(p);
                                         __old_v = lumyr_make_int32(v);
                                         break;
                                     }
                                     case CAST_INT64: {
                                         int64_t* p = stackframe_get_int64_ptr(frame, n);
                                         if(!p) goto dec_val_post;
                                         int64_t v = *p;
                                         int64_dec(p);
                                         __old_v = lumyr_make_int64(v);
                                         break;
                                     }
                                     case CAST_UINT: {
                                         unsigned int* p = stackframe_get_uint_ptr(frame, n);
                                         if(!p) goto dec_val_post;
                                         unsigned int v = *p;
                                         uint_dec(p);
                                         __old_v = lumyr_make_uint(v);
                                         break;
                                     }
                                     case CAST_UINT8: {
                                         uint8_t* p = stackframe_get_uint8_ptr(frame, n);
                                         if(!p) goto dec_val_post;
                                         uint8_t v = *p;
                                         uint8_dec(p);
                                         __old_v = lumyr_make_uint8(v);
                                         break;
                                     }
                                     case CAST_UINT16: {
                                         uint16_t* p = stackframe_get_uint16_ptr(frame, n);
                                         if(!p) goto dec_val_post;
                                         uint16_t v = *p;
                                         uint16_dec(p);
                                         __old_v = lumyr_make_uint16(v);
                                         break;
                                     }
                                     case CAST_UINT32: {
                                         uint32_t* p = stackframe_get_uint32_ptr(frame, n);
                                         if(!p) goto dec_val_post;
                                         uint32_t v = *p;
                                         uint32_dec(p);
                                         __old_v = lumyr_make_uint32(v);
                                         break;
                                     }
                                     case CAST_UINT64: {
                                         uint64_t* p = stackframe_get_uint64_ptr(frame, n);
                                         if(!p) goto dec_val_post;
                                         uint64_t v = *p;
                                         uint64_dec(p);
                                         __old_v = lumyr_make_uint64(v);
                                         break;
                                     }
                                     default: goto dec_val_post;
                                 }
                                 stack[sp++] = __old_v; break;
                                 dec_val_post: {
                                     Value __old = stackframe_get(frame, n, &fnd);
                                     if(!fnd) runtime_undefined("变量", n);
                                     Value __nv = lumyr_post_dec(&__old);
                                     stackframe_bind(frame, n, __old);
                                     stack[sp++] = __nv;
                                 } break; }               // 返回值 = 旧值
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
            case OPC_UINT_ADD: {
                /* uint 加法：直接从 uint 栈弹出两个 uint，相加后结果压回 uint 栈，零检查零转换零 Value 开销 */
                unsigned int b = UINT_POP();
                unsigned int a = UINT_POP();
                fprintf(stderr, "[DEBUG VM] UINT_ADD: a=%u, b=%u, result=%u\n", a, b, a + b);
                UINT_PUSH(a + b);
                break;
            }
            case OPC_UINT_SUB: {
                /* uint 减法 */
                unsigned int b = UINT_POP();
                unsigned int a = UINT_POP();
                UINT_PUSH(a - b);
                break;
            }
            case OPC_UINT_MUL: {
                /* uint 乘法 */
                unsigned int b = UINT_POP();
                unsigned int a = UINT_POP();
                UINT_PUSH(a * b);
                break;
            }
            case OPC_UINT_DIV: {
                /* uint 除法：带除零检查 */
                unsigned int b = UINT_POP();
                unsigned int a = UINT_POP();
                if(b == 0) runtime_error("除零错误");
                UINT_PUSH(a / b);
                break;
            }
            case OPC_UINT_MOD: {
                /* uint 取模：带除零检查 */
                unsigned int b = UINT_POP();
                unsigned int a = UINT_POP();
                if(b == 0) runtime_error("除零错误");
                UINT_PUSH(a % b);
                break;
            }
            case OPC_UINT_TO_VALUE: {
                /* 把 uint 专用栈顶的 uint 值包装成 Value，压入 Value 栈
                   用于兼容赋值等通用逻辑（赋值给普通变量时需要从 Value 栈弹出值） */
                unsigned int uv = UINT_POP();
                stack[sp++] = lumyr_make_uint32(uv);
                break;
            }
            case OPC_UINT_GT: {
                /* uint 大于比较：直接从 uint 栈弹出两个 uint，比较后结果(bool)压入 Value 栈 */
                unsigned int b = UINT_POP();
                unsigned int a = UINT_POP();
                stack[sp++] = lumyr_make_bool(a > b);
                break;
            }
            case OPC_UINT_LT: {
                /* uint 小于比较 */
                unsigned int b = UINT_POP();
                unsigned int a = UINT_POP();
                stack[sp++] = lumyr_make_bool(a < b);
                break;
            }
            case OPC_UINT_GE: {
                /* uint 大于等于比较 */
                unsigned int b = UINT_POP();
                unsigned int a = UINT_POP();
                stack[sp++] = lumyr_make_bool(a >= b);
                break;
            }
            case OPC_UINT_LE: {
                /* uint 小于等于比较 */
                unsigned int b = UINT_POP();
                unsigned int a = UINT_POP();
                stack[sp++] = lumyr_make_bool(a <= b);
                break;
            }
            case OPC_UINT_EQ: {
                /* uint 等于比较 */
                unsigned int b = UINT_POP();
                unsigned int a = UINT_POP();
                stack[sp++] = lumyr_make_bool(a == b);
                break;
            }
            case OPC_UINT_NE: {
                /* uint 不等于比较 */
                unsigned int b = UINT_POP();
                unsigned int a = UINT_POP();
                stack[sp++] = lumyr_make_bool(a != b);
                break;
            }
            case OPC_UINT_ARRAY_SET: {
                /* uint 类型化数组元素赋值：从 uint 专用栈弹出值，从 Value 栈弹出索引和数组，
                   直接写入 uint 类型化数组，零转换开销 */
                unsigned int val = UINT_POP();
                Value idx = stack[--sp];
                Value arr = stack[--sp];
                if(arr.type == VAL_TYPED_ARRAY && arr.v.typed_array &&
                   arr.v.typed_array->elem_type == VAL_UINT32) {
                    TypedArray* tarr = arr.v.typed_array;
                    long long i = array_index_of(idx);
                    if(i < 0 || i >= tarr->len) {
                        char buf[128];
                        snprintf(buf, sizeof(buf), "uint typed array index out of bounds: %lld (len %d)", i, tarr->len);
                        runtime_error(buf);
                    }
                    ((unsigned int*)tarr->items)[i] = val;
                } else {
                    runtime_error("OPC_UINT_ARRAY_SET: array is not uint typed array");
                }
                /* 把被设置的值包装成 Value，压入 Value 栈（用于表达式值） */
                stack[sp++] = lumyr_make_uint32(val);
                break;
            }
            /* 类型转换指令：专用栈之间的转换，零包装零Value开销 */
            /* ========== long long 类型专用指令 VM 处理 ========== */
            case OPC_PUSH_LONG_LONG_CONST: {
                /* 合并低32位(in.a)和高32位(in.b)为64位long long值
                   注意：in.b也要强制转换为unsigned int，避免符号扩展 */
                long long llval = ((long long)(unsigned int)in.a) | (((long long)(unsigned int)in.b) << 32);
                                LONG_LONG_PUSH(llval);
                break;
            }
            case OPC_LOAD_LONG_LONG_VAR: {
                const char* name = bf->syms[in.a];
                _Bool fnd = 0;
                long long llv = stackframe_get_long_long(frame, name, &fnd);
                                if(!fnd) runtime_undefined("variable", name);
                LONG_LONG_PUSH(llv);
                break;
            }
            case OPC_STORE_LONG_LONG_VAR: {
                const char* name = bf->syms[in.a];
                long long val = LONG_LONG_POP();
                                stackframe_bind_long_long(frame, name, val);
                stack[sp++] = lumyr_make_long_long(val);
                break;
            }
            case OPC_LONG_LONG_ADD: {
                long long b = LONG_LONG_POP();
                long long a = LONG_LONG_POP();
                LONG_LONG_PUSH(a + b);
                break;
            }
            case OPC_LONG_LONG_SUB: {
                long long b = LONG_LONG_POP();
                long long a = LONG_LONG_POP();
                LONG_LONG_PUSH(a - b);
                break;
            }
            case OPC_LONG_LONG_MUL: {
                long long b = LONG_LONG_POP();
                long long a = LONG_LONG_POP();
                LONG_LONG_PUSH(a * b);
                break;
            }
            case OPC_LONG_LONG_DIV: {
                long long b = LONG_LONG_POP();
                long long a = LONG_LONG_POP();
                if(b == 0) runtime_error("除零错误");
                LONG_LONG_PUSH(a / b);
                break;
            }
            case OPC_LONG_LONG_MOD: {
                long long b = LONG_LONG_POP();
                long long a = LONG_LONG_POP();
                if(b == 0) runtime_error("除零错误");
                LONG_LONG_PUSH(a % b);
                break;
            }
            case OPC_LONG_LONG_TO_VALUE: {
                long long llv = LONG_LONG_POP();
                stack[sp++] = lumyr_make_long_long(llv);
                break;
            }
            case OPC_LONG_LONG_GT: {
                long long b = LONG_LONG_POP();
                long long a = LONG_LONG_POP();
                stack[sp++] = lumyr_make_bool(a > b);
                break;
            }
            case OPC_LONG_LONG_LT: {
                long long b = LONG_LONG_POP();
                long long a = LONG_LONG_POP();
                stack[sp++] = lumyr_make_bool(a < b);
                break;
            }
            case OPC_LONG_LONG_GE: {
                long long b = LONG_LONG_POP();
                long long a = LONG_LONG_POP();
                stack[sp++] = lumyr_make_bool(a >= b);
                break;
            }
            case OPC_LONG_LONG_LE: {
                long long b = LONG_LONG_POP();
                long long a = LONG_LONG_POP();
                stack[sp++] = lumyr_make_bool(a <= b);
                break;
            }
            case OPC_LONG_LONG_EQ: {
                long long b = LONG_LONG_POP();
                long long a = LONG_LONG_POP();
                stack[sp++] = lumyr_make_bool(a == b);
                break;
            }
            case OPC_LONG_LONG_NE: {
                long long b = LONG_LONG_POP();
                long long a = LONG_LONG_POP();
                stack[sp++] = lumyr_make_bool(a != b);
                break;
            }
            case OPC_PRINT_LONG_LONG: {
                long long llv = LONG_LONG_POP();
                                printf("%lld\n", llv);
                break;
            }
            case OPC_INT_TO_UINT: {
                unsigned int uv = (unsigned int)INT_POP();
                fprintf(stderr, "[DEBUG VM] INT_TO_UINT: int_val=%d, uint_val=%u\n", (int)uv, uv);
                UINT_PUSH(uv);
                break;
            }
            case OPC_UINT_TO_INT: {
                int iv = (int)UINT_POP();
                INT_PUSH(iv);
                break;
            }
            case OPC_INT_TO_FLOAT: {
                /* 从 int 栈弹出一个 int，转换为 float，压入 float 栈 */
                int iv = INT_POP();
                FLOAT_PUSH((float)iv);
                break;
            }
            case OPC_INT_TO_DOUBLE: {
                /* 从 int 栈弹出一个 int，转换为 double，压入 double 栈 */
                int iv = INT_POP();
                DOUBLE_PUSH((double)iv);
                break;
            }
            case OPC_UINT_TO_FLOAT: {
                /* 从 uint 栈弹出一个 uint，转换为 float，压入 float 栈 */
                unsigned int uv = UINT_POP();
                FLOAT_PUSH((float)uv);
                break;
            }
            case OPC_UINT_TO_DOUBLE: {
                /* 从 uint 栈弹出一个 uint，转换为 double，压入 double 栈 */
                unsigned int uv = UINT_POP();
                DOUBLE_PUSH((double)uv);
                break;
            }
            case OPC_FLOAT_TO_DOUBLE: {
                /* 从 float 栈弹出一个 float，转换为 double，压入 double 栈 */
                float fv = FLOAT_POP();
                DOUBLE_PUSH((double)fv);
                break;
            }
            case OPC_INT_TO_LONG_LONG: {
                long long llv = (long long)INT_POP();
                LONG_LONG_PUSH(llv);
                break;
            }
            case OPC_UINT_TO_LONG_LONG: {
                long long llv = (long long)UINT_POP();
                LONG_LONG_PUSH(llv);
                break;
            }
            case OPC_FLOAT_TO_LONG_LONG: {
                long long llv = (long long)FLOAT_POP();
                LONG_LONG_PUSH(llv);
                break;
            }
            case OPC_DOUBLE_TO_LONG_LONG: {
                long long llv = (long long)DOUBLE_POP();
                LONG_LONG_PUSH(llv);
                break;
            }
            case OPC_LONG_LONG_TO_FLOAT: {
                long long llv = LONG_LONG_POP();
                float fv = (float)llv;
                fprintf(stderr, "[DEBUG VM] LONG_LONG_TO_FLOAT: ll_val=%lld, float_val=%f\n", llv, (double)fv);
                FLOAT_PUSH(fv);
                break;
            }
            case OPC_LONG_LONG_TO_DOUBLE: {
                double dv = (double)LONG_LONG_POP();
                DOUBLE_PUSH(dv);
                break;
            }
            /* 转换到 long double 的专用指令 */
            case OPC_INT_TO_LONG_DOUBLE: {
                int iv = INT_POP();
                long double ldv = (long double)iv;
                fprintf(stderr, "[DEBUG LD] INT_TO_LONG_DOUBLE: int_val=%d, ld_val=%Lf\n", iv, (long double)ldv);
                LONG_DOUBLE_PUSH(ldv);
                fprintf(stderr, "[DEBUG LD] after push, sp=%d\n", *stack_global_get_sp(STACK_LONG_DOUBLE));
                break;
            }
            case OPC_UINT_TO_LONG_DOUBLE: {
                unsigned int uv = UINT_POP();
                long double ldv = (long double)uv;
                fprintf(stderr, "[DEBUG LD] UINT_TO_LONG_DOUBLE: uint_val=%u, ld_val=%Lf\n", uv, (long double)ldv);
                LONG_DOUBLE_PUSH(ldv);
                fprintf(stderr, "[DEBUG LD] after push, sp=%d\n", *stack_global_get_sp(STACK_LONG_DOUBLE));
                break;
            }
            case OPC_FLOAT_TO_LONG_DOUBLE: {
                float fv = FLOAT_POP();
                long double ldv = (long double)fv;
                fprintf(stderr, "[DEBUG LD] FLOAT_TO_LONG_DOUBLE: float_val=%f, ld_val=%Lf\n", (double)fv, (long double)ldv);
                LONG_DOUBLE_PUSH(ldv);
                fprintf(stderr, "[DEBUG LD] after push, sp=%d\n", *stack_global_get_sp(STACK_LONG_DOUBLE));
                break;
            }
            case OPC_DOUBLE_TO_LONG_DOUBLE: {
                double dv = DOUBLE_POP();
                long double ldv = (long double)dv;
                fprintf(stderr, "[DEBUG LD] DOUBLE_TO_LONG_DOUBLE: double_val=%f, ld_val=%Lf, sizeof(ld)=%d\n", dv, (long double)ldv, (int)sizeof(long double));
                LONG_DOUBLE_PUSH(ldv);
                fprintf(stderr, "[DEBUG LD] after push, sp=%d\n", *stack_global_get_sp(STACK_LONG_DOUBLE));
                break;
            }
            case OPC_LONG_LONG_TO_LONG_DOUBLE: {
                long long llv = LONG_LONG_POP();
                long double ldv = (long double)llv;
                fprintf(stderr, "[DEBUG LD] LONG_LONG_TO_LONG_DOUBLE: ll_val=%lld, ld_val=%Lf\n", llv, (long double)ldv);
                LONG_DOUBLE_PUSH(ldv);
                fprintf(stderr, "[DEBUG LD] after push, sp=%d\n", *stack_global_get_sp(STACK_LONG_DOUBLE));
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
            case OPC_PRINT_SHORT: {
                /* 从 short 栈弹出并打印（零开销，用于声明为 short 的变量） */
                short sv = SHORT_POP();
                printf("%d\n", (int)sv);
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
            case OPC_PUSH_LONG_DOUBLE_CONST: {
                /* long double常量压栈：in.a低32位 + in.b高32位，合并为64位double值，再转换为long double
                   注意：Windows平台下long double是128位(16字节)，但指令只能传递64位，
                   所以先解析为double(64位)，再提升为long double，损失部分高精度但保证正确性 */
                uint64_t bits = (uint64_t)(uint32_t)in.a | ((uint64_t)(uint32_t)in.b << 32);
                double d = 0.0;
                memcpy(&d, &bits, sizeof(double));
                long double ld = (long double)d;
                LONG_DOUBLE_PUSH(ld);
                break;
            }
            case OPC_LONG_DOUBLE_ADD: {
                long double b = LONG_DOUBLE_POP();
                long double a = LONG_DOUBLE_POP();
                LONG_DOUBLE_PUSH(a + b);
                break;
            }
            case OPC_LONG_DOUBLE_SUB: {
                long double b = LONG_DOUBLE_POP();
                long double a = LONG_DOUBLE_POP();
                LONG_DOUBLE_PUSH(a - b);
                break;
            }
            case OPC_LONG_DOUBLE_MUL: {
                long double b = LONG_DOUBLE_POP();
                long double a = LONG_DOUBLE_POP();
                LONG_DOUBLE_PUSH(a * b);
                break;
            }
            case OPC_LONG_DOUBLE_DIV: {
                long double b = LONG_DOUBLE_POP();
                long double a = LONG_DOUBLE_POP();
                LONG_DOUBLE_PUSH(a / b);
                break;
            }
            case OPC_LONG_DOUBLE_GT: {
                long double b = LONG_DOUBLE_POP();
                long double a = LONG_DOUBLE_POP();
                stack[sp++] = lumyr_make_bool(a > b);
                break;
            }
            case OPC_LONG_DOUBLE_LT: {
                long double b = LONG_DOUBLE_POP();
                long double a = LONG_DOUBLE_POP();
                stack[sp++] = lumyr_make_bool(a < b);
                break;
            }
            case OPC_LONG_DOUBLE_GE: {
                long double b = LONG_DOUBLE_POP();
                long double a = LONG_DOUBLE_POP();
                stack[sp++] = lumyr_make_bool(a >= b);
                break;
            }
            case OPC_LONG_DOUBLE_LE: {
                long double b = LONG_DOUBLE_POP();
                long double a = LONG_DOUBLE_POP();
                stack[sp++] = lumyr_make_bool(a <= b);
                break;
            }
            case OPC_LONG_DOUBLE_EQ: {
                long double b = LONG_DOUBLE_POP();
                long double a = LONG_DOUBLE_POP();
                stack[sp++] = lumyr_make_bool(a == b);
                break;
            }
            case OPC_LONG_DOUBLE_NE: {
                long double b = LONG_DOUBLE_POP();
                long double a = LONG_DOUBLE_POP();
                stack[sp++] = lumyr_make_bool(a != b);
                break;
            }
            /* ===== int8 类型专用算术/比较运算指令 ===== */
            case OPC_INT8_ADD: {
                int8_t b = INT8_POP();
                int8_t a = INT8_POP();
                INT8_PUSH(a + b);
                break;
            }
            case OPC_INT8_SUB: {
                int8_t b = INT8_POP();
                int8_t a = INT8_POP();
                INT8_PUSH(a - b);
                break;
            }
            case OPC_INT8_MUL: {
                int8_t b = INT8_POP();
                int8_t a = INT8_POP();
                INT8_PUSH(a * b);
                break;
            }
            case OPC_INT8_DIV: {
                int8_t b = INT8_POP();
                int8_t a = INT8_POP();
                if(b == 0) runtime_error("division by zero: int8 division");
                INT8_PUSH(a / b);
                break;
            }
            case OPC_INT8_MOD: {
                int8_t b = INT8_POP();
                int8_t a = INT8_POP();
                if(b == 0) runtime_error("division by zero: int8 modulo");
                INT8_PUSH(a % b);
                break;
            }
            case OPC_INT8_GT: {
                int8_t b = INT8_POP();
                int8_t a = INT8_POP();
                stack[sp++] = lumyr_make_bool(a > b);
                break;
            }
            case OPC_INT8_LT: {
                int8_t b = INT8_POP();
                int8_t a = INT8_POP();
                stack[sp++] = lumyr_make_bool(a < b);
                break;
            }
            case OPC_INT8_GE: {
                int8_t b = INT8_POP();
                int8_t a = INT8_POP();
                stack[sp++] = lumyr_make_bool(a >= b);
                break;
            }
            case OPC_INT8_LE: {
                int8_t b = INT8_POP();
                int8_t a = INT8_POP();
                stack[sp++] = lumyr_make_bool(a <= b);
                break;
            }
            case OPC_INT8_EQ: {
                int8_t b = INT8_POP();
                int8_t a = INT8_POP();
                stack[sp++] = lumyr_make_bool(a == b);
                break;
            }
            case OPC_INT8_NE: {
                int8_t b = INT8_POP();
                int8_t a = INT8_POP();
                stack[sp++] = lumyr_make_bool(a != b);
                break;
            }
            /* ===== int16 类型专用算术/比较运算指令 ===== */
            case OPC_INT16_ADD: {
                int16_t b = INT16_POP();
                int16_t a = INT16_POP();
                INT16_PUSH(a + b);
                break;
            }
            case OPC_INT16_SUB: {
                int16_t b = INT16_POP();
                int16_t a = INT16_POP();
                INT16_PUSH(a - b);
                break;
            }
            case OPC_INT16_MUL: {
                int16_t b = INT16_POP();
                int16_t a = INT16_POP();
                INT16_PUSH(a * b);
                break;
            }
            case OPC_INT16_DIV: {
                int16_t b = INT16_POP();
                int16_t a = INT16_POP();
                if(b == 0) runtime_error("division by zero: int16 division");
                INT16_PUSH(a / b);
                break;
            }
            case OPC_INT16_MOD: {
                int16_t b = INT16_POP();
                int16_t a = INT16_POP();
                if(b == 0) runtime_error("division by zero: int16 modulo");
                INT16_PUSH(a % b);
                break;
            }
            case OPC_INT16_GT: {
                int16_t b = INT16_POP();
                int16_t a = INT16_POP();
                stack[sp++] = lumyr_make_bool(a > b);
                break;
            }
            case OPC_INT16_LT: {
                int16_t b = INT16_POP();
                int16_t a = INT16_POP();
                stack[sp++] = lumyr_make_bool(a < b);
                break;
            }
            case OPC_INT16_GE: {
                int16_t b = INT16_POP();
                int16_t a = INT16_POP();
                stack[sp++] = lumyr_make_bool(a >= b);
                break;
            }
            case OPC_INT16_LE: {
                int16_t b = INT16_POP();
                int16_t a = INT16_POP();
                stack[sp++] = lumyr_make_bool(a <= b);
                break;
            }
            case OPC_INT16_EQ: {
                int16_t b = INT16_POP();
                int16_t a = INT16_POP();
                stack[sp++] = lumyr_make_bool(a == b);
                break;
            }
            case OPC_INT16_NE: {
                int16_t b = INT16_POP();
                int16_t a = INT16_POP();
                stack[sp++] = lumyr_make_bool(a != b);
                break;
            }
            /* ===== short 类型专用算术/比较运算指令 ===== */
            case OPC_SHORT_ADD: {
                short b = SHORT_POP();
                short a = SHORT_POP();
                SHORT_PUSH(a + b);
                break;
            }
            case OPC_SHORT_SUB: {
                short b = SHORT_POP();
                short a = SHORT_POP();
                SHORT_PUSH(a - b);
                break;
            }
            case OPC_SHORT_MUL: {
                short b = SHORT_POP();
                short a = SHORT_POP();
                SHORT_PUSH(a * b);
                break;
            }
            case OPC_SHORT_DIV: {
                short b = SHORT_POP();
                short a = SHORT_POP();
                SHORT_PUSH(a / b);
                break;
            }
            case OPC_SHORT_MOD: {
                short b = SHORT_POP();
                short a = SHORT_POP();
                SHORT_PUSH(a % b);
                break;
            }
            case OPC_SHORT_GT: {
                short b = SHORT_POP();
                short a = SHORT_POP();
                stack[sp++] = lumyr_make_bool(a > b);
                break;
            }
            case OPC_SHORT_LT: {
                short b = SHORT_POP();
                short a = SHORT_POP();
                stack[sp++] = lumyr_make_bool(a < b);
                break;
            }
            case OPC_SHORT_GE: {
                short b = SHORT_POP();
                short a = SHORT_POP();
                stack[sp++] = lumyr_make_bool(a >= b);
                break;
            }
            case OPC_SHORT_LE: {
                short b = SHORT_POP();
                short a = SHORT_POP();
                stack[sp++] = lumyr_make_bool(a <= b);
                break;
            }
            case OPC_SHORT_EQ: {
                short b = SHORT_POP();
                short a = SHORT_POP();
                stack[sp++] = lumyr_make_bool(a == b);
                break;
            }
            case OPC_SHORT_NE: {
                short b = SHORT_POP();
                short a = SHORT_POP();
                stack[sp++] = lumyr_make_bool(a != b);
                break;
            }
            /* ===== int32 类型专用算术/比较运算指令 ===== */
            case OPC_INT32_ADD: {
                int32_t b = INT32_POP();
                int32_t a = INT32_POP();
                INT32_PUSH(a + b);
                break;
            }
            case OPC_INT32_SUB: {
                int32_t b = INT32_POP();
                int32_t a = INT32_POP();
                INT32_PUSH(a - b);
                break;
            }
            case OPC_INT32_MUL: {
                int32_t b = INT32_POP();
                int32_t a = INT32_POP();
                INT32_PUSH(a * b);
                break;
            }
            case OPC_INT32_DIV: {
                int32_t b = INT32_POP();
                int32_t a = INT32_POP();
                if(b == 0) runtime_error("division by zero: int32 division");
                INT32_PUSH(a / b);
                break;
            }
            case OPC_INT32_MOD: {
                int32_t b = INT32_POP();
                int32_t a = INT32_POP();
                if(b == 0) runtime_error("division by zero: int32 modulo");
                INT32_PUSH(a % b);
                break;
            }
            case OPC_INT32_GT: {
                int32_t b = INT32_POP();
                int32_t a = INT32_POP();
                stack[sp++] = lumyr_make_bool(a > b);
                break;
            }
            case OPC_INT32_LT: {
                int32_t b = INT32_POP();
                int32_t a = INT32_POP();
                stack[sp++] = lumyr_make_bool(a < b);
                break;
            }
            case OPC_INT32_GE: {
                int32_t b = INT32_POP();
                int32_t a = INT32_POP();
                stack[sp++] = lumyr_make_bool(a >= b);
                break;
            }
            case OPC_INT32_LE: {
                int32_t b = INT32_POP();
                int32_t a = INT32_POP();
                stack[sp++] = lumyr_make_bool(a <= b);
                break;
            }
            case OPC_INT32_EQ: {
                int32_t b = INT32_POP();
                int32_t a = INT32_POP();
                stack[sp++] = lumyr_make_bool(a == b);
                break;
            }
            case OPC_INT32_NE: {
                int32_t b = INT32_POP();
                int32_t a = INT32_POP();
                stack[sp++] = lumyr_make_bool(a != b);
                break;
            }
            /* ===== int64 类型专用算术/比较运算指令 ===== */
            case OPC_INT64_ADD: {
                int64_t b = INT64_POP();
                int64_t a = INT64_POP();
                INT64_PUSH(a + b);
                break;
            }
            case OPC_INT64_SUB: {
                int64_t b = INT64_POP();
                int64_t a = INT64_POP();
                INT64_PUSH(a - b);
                break;
            }
            case OPC_INT64_MUL: {
                int64_t b = INT64_POP();
                int64_t a = INT64_POP();
                INT64_PUSH(a * b);
                break;
            }
            case OPC_INT64_DIV: {
                int64_t b = INT64_POP();
                int64_t a = INT64_POP();
                if(b == 0) runtime_error("division by zero: int64 division");
                INT64_PUSH(a / b);
                break;
            }
            case OPC_INT64_MOD: {
                int64_t b = INT64_POP();
                int64_t a = INT64_POP();
                if(b == 0) runtime_error("division by zero: int64 modulo");
                INT64_PUSH(a % b);
                break;
            }
            case OPC_INT64_GT: {
                int64_t b = INT64_POP();
                int64_t a = INT64_POP();
                stack[sp++] = lumyr_make_bool(a > b);
                break;
            }
            case OPC_INT64_LT: {
                int64_t b = INT64_POP();
                int64_t a = INT64_POP();
                stack[sp++] = lumyr_make_bool(a < b);
                break;
            }
            case OPC_INT64_GE: {
                int64_t b = INT64_POP();
                int64_t a = INT64_POP();
                stack[sp++] = lumyr_make_bool(a >= b);
                break;
            }
            case OPC_INT64_LE: {
                int64_t b = INT64_POP();
                int64_t a = INT64_POP();
                stack[sp++] = lumyr_make_bool(a <= b);
                break;
            }
            case OPC_INT64_EQ: {
                int64_t b = INT64_POP();
                int64_t a = INT64_POP();
                stack[sp++] = lumyr_make_bool(a == b);
                break;
            }
            case OPC_INT64_NE: {
                int64_t b = INT64_POP();
                int64_t a = INT64_POP();
                stack[sp++] = lumyr_make_bool(a != b);
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
