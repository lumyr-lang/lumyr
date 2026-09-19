/*
 * vm_exec_io.c - VM 输入输出指令
 * 4 核心栈设计：STACK_VALUE / INT64 / DOUBLE / PTR
 */
#include "vm_types.h"
#include "stack_manager.h"
#include "lumyr_value_type.h"
#include "lm_value.h"
#include "lm_bigint.h"
#include "lm_decimal.h"
#include <stdio.h>
#include <stdlib.h>

/* ===== 打印 ===== */

/* PRINT：从 VALUE 栈弹值打印 */
int vm_exec_io_print(VMExecCtx* ctx, Instruction* in) {
    static int print_count = 0;
    print_count++;
    Value* stk = (Value*)g_stack_mgr->stacks[STACK_VALUE];
    int sp = --g_stack_mgr->sp[STACK_VALUE];
    lumyr_print(stk[sp]);
    return 1;
}

/* PRINT_INT64：从 INT64 栈弹值打印（a 字段存 CastKind） */
int vm_exec_io_print_int64(VMExecCtx* ctx, Instruction* in) {
    static int print_int64_count = 0;
    print_int64_count++;
    int sp = --g_stack_mgr->sp[STACK_INT64];
    int64_t val = ((int64_t*)g_stack_mgr->stacks[STACK_INT64])[sp];
    CastKind ct = (CastKind)in->a;
    switch(ct) {
        case CAST_UINT8:
        case CAST_UINT16:
        case CAST_UINT32:
        case CAST_UINT:
        case CAST_UINT64:
        case CAST_ULONG:
        case CAST_USHORT:
        case CAST_UCHAR:
        case CAST_BYTE:
        case CAST_SIZE_T:
            printf("%llu\n", (unsigned long long)val);
            break;
        case CAST_CHAR:
            printf("%c\n", (char)val);
            break;
        case CAST_BOOL:
            printf("%s\n", val ? "true" : "false");
            break;
        default:
            printf("%lld\n", (long long)val);
            break;
    }
    return 1;
}

/* PRINT_DOUBLE：从 DOUBLE 栈弹值打印 */
int vm_exec_io_print_double(VMExecCtx* ctx, Instruction* in) {
    static int print_double_count = 0;
    print_double_count++;
    int sp = --g_stack_mgr->sp[STACK_DOUBLE];
    double val = ((double*)g_stack_mgr->stacks[STACK_DOUBLE])[sp];
    printf("%g\n", val);
    return 1;
}

/* PRINT_PTR：从 PTR 栈弹值打印（字符串指针） */
int vm_exec_io_print_ptr(VMExecCtx* ctx, Instruction* in) {
    static int print_ptr_count = 0;
    print_ptr_count++;
    void* val;
    stack_vm_pop(g_stack_mgr, STACK_PTR, &val);
    if(val) {
        printf("%s\n", (char*)val);
    } else {
        printf("(null)\n");
    }
    return 1;
}

/* PRINT_BIGINT：从 PTR 栈弹 bigint 对象打印 */
int vm_exec_io_print_bigint(VMExecCtx* ctx, Instruction* in) {
    void* val;
    stack_vm_pop(g_stack_mgr, STACK_PTR, &val);
    BigInt* bi = (BigInt*)val;
    if(bi) {
        char* s = lumyr_bigint_to_string(bi);
        printf("%s\n", s);
        free(s);
    } else {
        printf("(null)\n");
    }
    return 1;
}

/* PRINT_DECIMAL：从 PTR 栈弹 decimal 对象打印 */
int vm_exec_io_print_decimal(VMExecCtx* ctx, Instruction* in) {
    void* val;
    stack_vm_pop(g_stack_mgr, STACK_PTR, &val);
    Decimal* d = (Decimal*)val;
    if(d) {
        char* s = lumyr_decimal_to_string(d);
        printf("%s\n", s);
        free(s);
    } else {
        printf("(null)\n");
    }
    return 1;
}
