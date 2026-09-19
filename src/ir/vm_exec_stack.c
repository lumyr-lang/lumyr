/*
 * vm_exec_stack.c - VM 栈操作指令
 * 4 核心栈设计：STACK_VALUE / INT64 / DOUBLE / PTR
 */
#include "vm_types.h"
#include "stack_manager.h"
#include "lumyr_value.h"

/* ===== 栈操作 ===== */

/* POP：弹出 VALUE 栈顶 */
int vm_exec_stack_pop(VMExecCtx* ctx, Instruction* in) {
    Value val;
    stack_vm_pop(g_stack_mgr, STACK_VALUE, &val);
    return 1;
}

/* DUP：复制 VALUE 栈顶 */
int vm_exec_stack_dup(VMExecCtx* ctx, Instruction* in) {
    Value val;
    stack_vm_pop(g_stack_mgr, STACK_VALUE, &val);
    stack_vm_push(g_stack_mgr, STACK_VALUE, &val);
    stack_vm_push(g_stack_mgr, STACK_VALUE, &val);
    return 1;
}

/* ARRAY_LIT：弹 b 个 VALUE 栈顶元素（栈顶为最后一个），构造数组并压入 VALUE 栈。
 * 元素顺序保持实参左至右：弹栈逆序，回填 items[i] 时倒序放置。 */
int vm_exec_array_lit(VMExecCtx* ctx, Instruction* in) {
    (void)ctx;
    int n = in->b;
    Value arr = val_array(n);
    if (n > 0) {
        for (int i = n - 1; i >= 0; --i) {
            Value v;
            stack_vm_pop(g_stack_mgr, STACK_VALUE, &v);
            arr.v.array->items[i] = v;
        }
    }
    stack_vm_push(g_stack_mgr, STACK_VALUE, &arr);
    return 1;
}

/* 把 Value 下标转为 int64（支持 VAL_INT/INT64/LONG_LONG 等） */
static int64_t value_to_index(Value v) {
    switch(v.type) {
    case VAL_INT:       return (int64_t)v.v.i;
    case VAL_INT64:     return v.v.i64;
    case VAL_LONG_LONG: return (int64_t)v.v.ll;
    case VAL_LONG:      return (int64_t)v.v.l;
    case VAL_INT32:     return (int64_t)v.v.i32;
    case VAL_INT16:     return (int64_t)v.v.i16;
    case VAL_INT8:      return (int64_t)v.v.i8;
    case VAL_SHORT:     return (int64_t)v.v.sh;
    case VAL_UINT:      return (int64_t)v.v.ui;
    case VAL_UINT64:    return (int64_t)v.v.u64;
    case VAL_UINT32:    return (int64_t)v.v.u32;
    case VAL_DOUBLE:    return (int64_t)v.v.d;
    default:            return 0;
    }
}

/* INDEX_GET：弹 idx、arr（VALUE 栈），取 arr[idx]，压元素（越界/非数组→none） */
int vm_exec_index_get(VMExecCtx* ctx, Instruction* in) {
    (void)ctx; (void)in;
    Value idx; stack_vm_pop(g_stack_mgr, STACK_VALUE, &idx);
    Value arr; stack_vm_pop(g_stack_mgr, STACK_VALUE, &arr);
    Value r = val_none();
    if(arr.type == VAL_ARRAY) {
        int64_t i = value_to_index(idx);
        if(i >= 0 && i < (int64_t)arr.v.array->len)
            r = arr.v.array->items[i];
    }
    stack_vm_push(g_stack_mgr, STACK_VALUE, &r);
    return 1;
}
