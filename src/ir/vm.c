/*
 * vm.c - VM 解释器主入口（4 核心栈设计重写）
 *
 * 栈设计：
 *   STACK_VALUE  - 通用 Value 栈（动态类型、对象、字符串堆指针）
 *   STACK_INT64  - 统一整数栈（所有整数类型、bool、char 都存 int64_t）
 *   STACK_DOUBLE - 统一浮点栈（float、double、long double 都存 double）
 *   STACK_PTR    - 指针栈（字符串 SSO 内联、对象指针、FFI 指针）
 */

#include "vm.h"
#include "stack_manager.h"
#include "lumyr_value_type.h"
#include "lumyr_value.h"
#include "lm_value.h"
#include "gc_runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>



/* ========== VM 执行上下文 ========== */
typedef struct {
    BytecodeFunc* fn;       /* 当前函数字节码 */
    Instruction* code;      /* 指令数组 */
    int pc;                 /* 指令指针 */
    StackFrame* frame;      /* 当前栈帧 */
    Value* consts;          /* 常量池 */
    const char** syms;      /* 符号表 */
    int const_cnt;
    int sym_cnt;
} VMExecCtx;

/* ========== 栈操作宏（4 核心栈） ========== */
#define VALUE_PUSH(v)  do { g_stack_mgr->stacks[STACK_VALUE] = (g_stack_mgr->stacks[STACK_VALUE]); /* TODO */ } while(0)

/* ========== 主执行循环 ========== */
static Value vm_execute(VMExecCtx* ctx) {
    Instruction* code = ctx->code;
    int pc = 0;
    Value result = val_none();

    /* 初始化全局栈管理器 */
    if (!g_stack_mgr) {
        stack_global_init(256);
    }


    while (pc < ctx->fn->code_len) {
        Instruction in = code[pc++];
        switch (in.op) {
        case OPC_NOP:
            break;

        /* ===== 栈操作 ===== */
        case OPC_POP:
            g_stack_mgr->sp[STACK_VALUE]--;
            break;

        case OPC_DUP: {
            int sp = g_stack_mgr->sp[STACK_VALUE];
            Value* stk = (Value*)g_stack_mgr->stacks[STACK_VALUE];
            stk[sp] = stk[sp - 1];
            g_stack_mgr->sp[STACK_VALUE]++;
            break;
        }

        /* ===== 常量加载 ===== */
        case OPC_LOAD_CONST: {
            int idx = in.a;
            Value* stk = (Value*)g_stack_mgr->stacks[STACK_VALUE];
            int sp = g_stack_mgr->sp[STACK_VALUE]++;
            stk[sp] = ctx->consts[idx];
            break;
        }

        case OPC_PUSH_INT64_CONST: {
            int64_t val = ((int64_t)in.a) | ((int64_t)in.b << 32);
            int sp = g_stack_mgr->sp[STACK_INT64]++;
            ((int64_t*)g_stack_mgr->stacks[STACK_INT64])[sp] = val;
            break;
        }

        case OPC_PUSH_DOUBLE_CONST: {
            uint64_t bits = ((uint64_t)in.a) | ((uint64_t)in.b << 32);
            double val;
            memcpy(&val, &bits, sizeof(double));
            int sp = g_stack_mgr->sp[STACK_DOUBLE]++;
            ((double*)g_stack_mgr->stacks[STACK_DOUBLE])[sp] = val;
            break;
        }

        case OPC_PUSH_PTR_CONST: {
            void* val = (void*)(intptr_t)in.a;
            int sp = g_stack_mgr->sp[STACK_PTR]++;
            ((void**)g_stack_mgr->stacks[STACK_PTR])[sp] = val;
            break;
        }

        /* ===== 变量加载/存储 ===== */
        case OPC_LOAD_VAR: {
            int idx = in.a;
            Value* stk = (Value*)g_stack_mgr->stacks[STACK_VALUE];
            int sp = g_stack_mgr->sp[STACK_VALUE]++;
            stk[sp] = ctx->frame->vals[idx];
            break;
        }

        case OPC_STORE_VAR: {
            int idx = in.a;
            Value* stk = (Value*)g_stack_mgr->stacks[STACK_VALUE];
            Value val = stk[--g_stack_mgr->sp[STACK_VALUE]];
            ctx->frame->vals[idx] = val;
            /* 压回（表达式值） */
            stk[g_stack_mgr->sp[STACK_VALUE]++] = val;
            break;
        }

        case OPC_LOAD_INT64_VAR: {
            int idx = in.a;
            int sp = g_stack_mgr->sp[STACK_INT64]++;
            ((int64_t*)g_stack_mgr->stacks[STACK_INT64])[sp] = ctx->frame->int_slots[idx];
            break;
        }

        case OPC_STORE_INT64_VAR: {
            int idx = in.a;
            int sp = --g_stack_mgr->sp[STACK_INT64];
            ctx->frame->int_slots[idx] = ((int64_t*)g_stack_mgr->stacks[STACK_INT64])[sp];
            break;
        }

        case OPC_LOAD_DOUBLE_VAR: {
            int idx = in.a;
            int sp = g_stack_mgr->sp[STACK_DOUBLE]++;
            ((double*)g_stack_mgr->stacks[STACK_DOUBLE])[sp] = ctx->frame->flt_slots[idx];
            break;
        }

        case OPC_STORE_DOUBLE_VAR: {
            int idx = in.a;
            int sp = --g_stack_mgr->sp[STACK_DOUBLE];
            ctx->frame->flt_slots[idx] = ((double*)g_stack_mgr->stacks[STACK_DOUBLE])[sp];
            break;
        }

        /* ===== int64 栈算术运算 ===== */
        case OPC_INT64_ADD: {
            int sp = g_stack_mgr->sp[STACK_INT64];
            int64_t* stk = (int64_t*)g_stack_mgr->stacks[STACK_INT64];
            int64_t b = stk[--sp];
            int64_t a = stk[--sp];
            stk[sp++] = a + b;
            g_stack_mgr->sp[STACK_INT64] = sp;
            break;
        }

        case OPC_INT64_SUB: {
            int sp = g_stack_mgr->sp[STACK_INT64];
            int64_t* stk = (int64_t*)g_stack_mgr->stacks[STACK_INT64];
            int64_t b = stk[--sp];
            int64_t a = stk[--sp];
            stk[sp++] = a - b;
            g_stack_mgr->sp[STACK_INT64] = sp;
            break;
        }

        case OPC_INT64_MUL: {
            int sp = g_stack_mgr->sp[STACK_INT64];
            int64_t* stk = (int64_t*)g_stack_mgr->stacks[STACK_INT64];
            int64_t b = stk[--sp];
            int64_t a = stk[--sp];
            stk[sp++] = a * b;
            g_stack_mgr->sp[STACK_INT64] = sp;
            break;
        }

        case OPC_INT64_DIV: {
            int sp = g_stack_mgr->sp[STACK_INT64];
            int64_t* stk = (int64_t*)g_stack_mgr->stacks[STACK_INT64];
            int64_t b = stk[--sp];
            int64_t a = stk[--sp];
            stk[sp++] = a / b;
            g_stack_mgr->sp[STACK_INT64] = sp;
            break;
        }

        case OPC_INT64_MOD: {
            int sp = g_stack_mgr->sp[STACK_INT64];
            int64_t* stk = (int64_t*)g_stack_mgr->stacks[STACK_INT64];
            int64_t b = stk[--sp];
            int64_t a = stk[--sp];
            stk[sp++] = a % b;
            g_stack_mgr->sp[STACK_INT64] = sp;
            break;
        }

        /* ===== int64 栈比较运算 ===== */
        case OPC_INT64_GT: {
            int sp = g_stack_mgr->sp[STACK_INT64];
            int64_t* stk = (int64_t*)g_stack_mgr->stacks[STACK_INT64];
            int64_t b = stk[--sp];
            int64_t a = stk[--sp];
            g_stack_mgr->sp[STACK_INT64] = sp;
            Value* vstk = (Value*)g_stack_mgr->stacks[STACK_VALUE];
            vstk[g_stack_mgr->sp[STACK_VALUE]++] = lumyr_make_bool(a > b);
            break;
        }

        case OPC_INT64_LT: {
            int sp = g_stack_mgr->sp[STACK_INT64];
            int64_t* stk = (int64_t*)g_stack_mgr->stacks[STACK_INT64];
            int64_t b = stk[--sp];
            int64_t a = stk[--sp];
            g_stack_mgr->sp[STACK_INT64] = sp;
            Value* vstk = (Value*)g_stack_mgr->stacks[STACK_VALUE];
            vstk[g_stack_mgr->sp[STACK_VALUE]++] = lumyr_make_bool(a < b);
            break;
        }

        case OPC_INT64_EQ: {
            int sp = g_stack_mgr->sp[STACK_INT64];
            int64_t* stk = (int64_t*)g_stack_mgr->stacks[STACK_INT64];
            int64_t b = stk[--sp];
            int64_t a = stk[--sp];
            g_stack_mgr->sp[STACK_INT64] = sp;
            Value* vstk = (Value*)g_stack_mgr->stacks[STACK_VALUE];
            vstk[g_stack_mgr->sp[STACK_VALUE]++] = lumyr_make_bool(a == b);
            break;
        }

        /* ===== double 栈算术运算 ===== */
        case OPC_DOUBLE_ADD: {
            int sp = g_stack_mgr->sp[STACK_DOUBLE];
            double* stk = (double*)g_stack_mgr->stacks[STACK_DOUBLE];
            double b = stk[--sp];
            double a = stk[--sp];
            stk[sp++] = a + b;
            g_stack_mgr->sp[STACK_DOUBLE] = sp;
            break;
        }

        case OPC_DOUBLE_SUB: {
            int sp = g_stack_mgr->sp[STACK_DOUBLE];
            double* stk = (double*)g_stack_mgr->stacks[STACK_DOUBLE];
            double b = stk[--sp];
            double a = stk[--sp];
            stk[sp++] = a - b;
            g_stack_mgr->sp[STACK_DOUBLE] = sp;
            break;
        }

        case OPC_DOUBLE_MUL: {
            int sp = g_stack_mgr->sp[STACK_DOUBLE];
            double* stk = (double*)g_stack_mgr->stacks[STACK_DOUBLE];
            double b = stk[--sp];
            double a = stk[--sp];
            stk[sp++] = a * b;
            g_stack_mgr->sp[STACK_DOUBLE] = sp;
            break;
        }

        case OPC_DOUBLE_DIV: {
            int sp = g_stack_mgr->sp[STACK_DOUBLE];
            double* stk = (double*)g_stack_mgr->stacks[STACK_DOUBLE];
            double b = stk[--sp];
            double a = stk[--sp];
            stk[sp++] = a / b;
            g_stack_mgr->sp[STACK_DOUBLE] = sp;
            break;
        }

        /* ===== double 栈比较运算 ===== */
        case OPC_DOUBLE_GT: {
            int sp = g_stack_mgr->sp[STACK_DOUBLE];
            double* stk = (double*)g_stack_mgr->stacks[STACK_DOUBLE];
            double b = stk[--sp];
            double a = stk[--sp];
            g_stack_mgr->sp[STACK_DOUBLE] = sp;
            Value* vstk = (Value*)g_stack_mgr->stacks[STACK_VALUE];
            vstk[g_stack_mgr->sp[STACK_VALUE]++] = lumyr_make_bool(a > b);
            break;
        }

        case OPC_DOUBLE_LT: {
            int sp = g_stack_mgr->sp[STACK_DOUBLE];
            double* stk = (double*)g_stack_mgr->stacks[STACK_DOUBLE];
            double b = stk[--sp];
            double a = stk[--sp];
            g_stack_mgr->sp[STACK_DOUBLE] = sp;
            Value* vstk = (Value*)g_stack_mgr->stacks[STACK_VALUE];
            vstk[g_stack_mgr->sp[STACK_VALUE]++] = lumyr_make_bool(a < b);
            break;
        }

        /* ===== 栈间转换 ===== */
        case OPC_INT64_TO_DOUBLE: {
            int sp = --g_stack_mgr->sp[STACK_INT64];
            int64_t val = ((int64_t*)g_stack_mgr->stacks[STACK_INT64])[sp];
            int dsp = g_stack_mgr->sp[STACK_DOUBLE]++;
            ((double*)g_stack_mgr->stacks[STACK_DOUBLE])[dsp] = (double)val;
            break;
        }

        case OPC_DOUBLE_TO_INT64: {
            int sp = --g_stack_mgr->sp[STACK_DOUBLE];
            double val = ((double*)g_stack_mgr->stacks[STACK_DOUBLE])[sp];
            int isp = g_stack_mgr->sp[STACK_INT64]++;
            ((int64_t*)g_stack_mgr->stacks[STACK_INT64])[isp] = (int64_t)val;
            break;
        }

        case OPC_INT64_TO_VALUE: {
            int sp = --g_stack_mgr->sp[STACK_INT64];
            int64_t val = ((int64_t*)g_stack_mgr->stacks[STACK_INT64])[sp];
            Value* vstk = (Value*)g_stack_mgr->stacks[STACK_VALUE];
            vstk[g_stack_mgr->sp[STACK_VALUE]++] = lumyr_make_int((int)val);
            break;
        }

        case OPC_DOUBLE_TO_VALUE: {
            int sp = --g_stack_mgr->sp[STACK_DOUBLE];
            double val = ((double*)g_stack_mgr->stacks[STACK_DOUBLE])[sp];
            Value* vstk = (Value*)g_stack_mgr->stacks[STACK_VALUE];
            vstk[g_stack_mgr->sp[STACK_VALUE]++] = lumyr_make_double(val);
            break;
        }

        /* ===== 控制流 ===== */
        case OPC_JMP:
            pc = in.a;
            break;

        case OPC_JMP_IF_FALSE: {
            Value* stk = (Value*)g_stack_mgr->stacks[STACK_VALUE];
            Value cond = stk[--g_stack_mgr->sp[STACK_VALUE]];
            if (!lumyr_to_bool(cond)) {
                pc = in.a;
            }
            break;
        }

        case OPC_JMP_IF_TRUE: {
            Value* stk = (Value*)g_stack_mgr->stacks[STACK_VALUE];
            Value cond = stk[--g_stack_mgr->sp[STACK_VALUE]];
            if (lumyr_to_bool(cond)) {
                pc = in.a;
            }
            break;
        }

        /* ===== 打印 ===== */
        case OPC_PRINT: {
            Value* stk = (Value*)g_stack_mgr->stacks[STACK_VALUE];
            Value val = stk[g_stack_mgr->sp[STACK_VALUE] - 1];
            lumyr_print(val);
            break;
        }

        case OPC_PRINT_INT64: {
            int sp = --g_stack_mgr->sp[STACK_INT64];
            int64_t val = ((int64_t*)g_stack_mgr->stacks[STACK_INT64])[sp];
            printf("%lld\n", (long long)val);
            break;
        }

        case OPC_PRINT_DOUBLE: {
            int sp = --g_stack_mgr->sp[STACK_DOUBLE];
            double val = ((double*)g_stack_mgr->stacks[STACK_DOUBLE])[sp];
            printf("%g\n", val);
            break;
        }

        /* ===== 返回 ===== */
        case OPC_RETURN: {
            Value* stk = (Value*)g_stack_mgr->stacks[STACK_VALUE];
            result = stk[--g_stack_mgr->sp[STACK_VALUE]];
            goto done;
        }

        case OPC_RETURN_NIL:
            goto done;

        case OPC_HALT:
            goto done;

        default:
            fprintf(stderr, "VM: 未实现指令 %d (pc=%d)\n", (int)in.op, pc - 1);
            goto done;
        }
    }

done:
    return result;
}

/* ========== 主入口 ========== */
Value vm_run_main(BytecodeFunc* main_fn) {
    fprintf(stdout, "[VM] vm_run_main entered\n");
    if (!main_fn) {
        fprintf(stdout, "[VM] main_fn is NULL\n");
        return val_none();
    }

    VMExecCtx ctx = {0};
    ctx.fn = main_fn;
    ctx.code = main_fn->code;
    ctx.consts = main_fn->consts;
    ctx.syms = main_fn->syms;
    ctx.const_cnt = main_fn->const_cnt;
    ctx.sym_cnt = main_fn->sym_cnt;

    /* 创建顶层栈帧 */
    StackFrame* frame = calloc(1, sizeof(StackFrame));
    frame->cap = 16;
    frame->names = calloc(frame->cap, sizeof(char*));
    frame->vals = calloc(frame->cap, sizeof(Value));
    frame->int_slots = calloc(frame->cap, sizeof(int64_t));
    frame->flt_slots = calloc(frame->cap, sizeof(double));
    ctx.frame = frame;

    /* 注册 GC 根 */
    gc_set_roots((Value*)g_stack_mgr->stacks[STACK_VALUE], &g_stack_mgr->sp[STACK_VALUE], frame);

    fprintf(stdout, "[VM] code_len=%d const_cnt=%d sym_cnt=%d\n", 
            ctx.fn->code_len, ctx.const_cnt, ctx.sym_cnt);
    Value result = vm_execute(&ctx);

    /* 清理 */
    free(frame->names);
    free(frame->vals);
    free(frame->int_slots);
    free(frame->flt_slots);
    free(frame);

    return result;
}

Value vm_func_entry(int arg_cnt, const Value* args, EvalCtx* ctx, StackFrame* frame) {
    (void)arg_cnt; (void)args; (void)ctx; (void)frame;
    return val_none();
}
