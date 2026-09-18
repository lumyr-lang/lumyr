/*
 * vm_exec_var.c - VM 变量存取指令
 * 4 核心栈设计：STACK_VALUE / INT64 / DOUBLE / PTR
 */
#include "vm_types.h"
#include "stack_manager.h"
#include "ast/stackframe.h"

/* 确保帧的槽位数组已分配到至少 need 个元素 */
static void frame_ensure_slots(StackFrame* f, int need) {
    if(!f) return;
    if(need <= f->cap) return;
    int newcap = f->cap > 0 ? f->cap : 16;
    while(newcap < need) newcap *= 2;

    /* 分配 names */
    if(!f->names) f->names = calloc(newcap, sizeof(char*));
    else f->names = realloc(f->names, newcap * sizeof(char*));

    /* 分配 vals */
    if(!f->vals) f->vals = calloc(newcap, sizeof(Value));
    else f->vals = realloc(f->vals, newcap * sizeof(Value));

    /* 分配 int_slots */
    if(!f->int_slots) f->int_slots = calloc(newcap, sizeof(int64_t));
    else f->int_slots = realloc(f->int_slots, newcap * sizeof(int64_t));

    /* 分配 flt_slots */
    if(!f->flt_slots) f->flt_slots = calloc(newcap, sizeof(double));
    else f->flt_slots = realloc(f->flt_slots, newcap * sizeof(double));

    /* 分配 ptr_slots */
    if(!f->ptr_slots) f->ptr_slots = calloc(newcap, sizeof(void*));
    else f->ptr_slots = realloc(f->ptr_slots, newcap * sizeof(void*));

    /* 分配 type_tags */
    if(!f->type_tags) f->type_tags = calloc(newcap, sizeof(int));
    else f->type_tags = realloc(f->type_tags, newcap * sizeof(int));

    f->cap = newcap;
}

/* ===== 变量存取（VALUE 栈） ===== */

/* LOAD_VAR：从帧槽位加载 Value 到 VALUE 栈 */
int vm_exec_var_load(VMExecCtx* ctx, Instruction* in) {
    int idx = in->a;
    stack_vm_push(g_stack_mgr, STACK_VALUE, &ctx->frame->vals[idx]);
    return 1;
}

/* STORE_VAR：从 VALUE 栈弹值存储到帧槽位 */
int vm_exec_var_store(VMExecCtx* ctx, Instruction* in) {
    int idx = in->a;
    Value val;
    if (stack_vm_pop(g_stack_mgr, STACK_VALUE, &val) < 0) {
        fprintf(stderr, "VM Error: VALUE stack underflow at STORE_VAR (idx=%d)\n", idx);
        return -1;
    }
    ctx->frame->vals[idx] = val;
    return 1;
}

/* ===== 变量存取（INT64 栈） ===== */

/* LOAD_INT64_VAR：从帧 int_slots 加载到 INT64 栈 */
int vm_exec_var_load_int64(VMExecCtx* ctx, Instruction* in) {
    int idx = in->a;
    stack_vm_push(g_stack_mgr, STACK_INT64, &ctx->frame->int_slots[idx]);
    return 1;
}

/* STORE_INT64_VAR：从 INT64 栈弹值存储到帧 int_slots */
int vm_exec_var_store_int64(VMExecCtx* ctx, Instruction* in) {
    int idx = in->a;
    frame_ensure_slots(ctx->frame, idx + 1);
    int64_t val;
    stack_vm_pop(g_stack_mgr, STACK_INT64, &val);
    ctx->frame->int_slots[idx] = val;
    return 1;
}

/* ===== 变量存取（DOUBLE 栈） ===== */

/* LOAD_DOUBLE_VAR：从帧 flt_slots 加载到 DOUBLE 栈 */
int vm_exec_var_load_double(VMExecCtx* ctx, Instruction* in) {
    int idx = in->a;
    stack_vm_push(g_stack_mgr, STACK_DOUBLE, &ctx->frame->flt_slots[idx]);
    return 1;
}

/* STORE_DOUBLE_VAR：从 DOUBLE 栈弹值存储到帧 flt_slots */
int vm_exec_var_store_double(VMExecCtx* ctx, Instruction* in) {
    int idx = in->a;
    frame_ensure_slots(ctx->frame, idx + 1);
    double val;
    stack_vm_pop(g_stack_mgr, STACK_DOUBLE, &val);
    ctx->frame->flt_slots[idx] = val;
    return 1;
}

/* ===== 变量存取（PTR 栈） ===== */

/* LOAD_PTR_VAR：从帧 ptr_slots 加载到 PTR 栈 */
int vm_exec_var_load_ptr(VMExecCtx* ctx, Instruction* in) {
    int idx = in->a;
    stack_vm_push(g_stack_mgr, STACK_PTR, &ctx->frame->ptr_slots[idx]);
    return 1;
}

/* STORE_PTR_VAR：从 PTR 栈弹值存储到帧 ptr_slots */
int vm_exec_var_store_ptr(VMExecCtx* ctx, Instruction* in) {
    int idx = in->a;
    frame_ensure_slots(ctx->frame, idx + 1);
    void* val;
    stack_vm_pop(g_stack_mgr, STACK_PTR, &val);
    ctx->frame->ptr_slots[idx] = val;
    return 1;
}
