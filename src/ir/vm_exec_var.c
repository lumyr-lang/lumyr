/*
 * vm_exec_var.c - VM 变量存取指令
 * 4 核心栈设计：STACK_VALUE / INT64 / DOUBLE / PTR
 */
#include "vm_types.h"
#include "stack_manager.h"
#include "ast/stackframe.h"
#include "lm_value.h"

/* ref 辅助：把调用方 typed 存储 box 成 Value */
static Value ref_box(RefDesc* r) {
    switch((CastKind)r->type) {
    case CAST_INT: case CAST_INT8: case CAST_INT16: case CAST_INT32: case CAST_INT64:
    case CAST_LONGLONG: case CAST_LONG: case CAST_SHORT: case CAST_USHORT:
    case CAST_BOOL: case CAST_CHAR: case CAST_UCHAR: case CAST_BYTE: case CAST_ASCII:
    case CAST_UINT8: case CAST_UINT16: case CAST_UINT32: case CAST_UINT:
    case CAST_UINT64: case CAST_ULONG: case CAST_SIZE_T: case CAST_SSIZE_T:
        return lumyr_make_int64(*(int64_t*)r->ptr);
    case CAST_FLOAT: case CAST_DOUBLE: case CAST_LONG_DOUBLE:
        return lumyr_make_double(*(double*)r->ptr);
    case CAST_STRING: {
        Value v; v.type = VAL_STRING; v.str_inline = 0;
        v.v.s = *(char**)r->ptr;
        return v;
    }
    default:
        return *(Value*)r->ptr;
    }
}

/* ref 辅助：把 Value unbox 写入调用方 typed 存储 */
static void ref_unbox(RefDesc* r, Value v) {
    switch((CastKind)r->type) {
    case CAST_INT: case CAST_INT8: case CAST_INT16: case CAST_INT32: case CAST_INT64:
    case CAST_LONGLONG: case CAST_LONG: case CAST_SHORT: case CAST_USHORT:
    case CAST_BOOL: case CAST_CHAR: case CAST_UCHAR: case CAST_BYTE: case CAST_ASCII:
    case CAST_UINT8: case CAST_UINT16: case CAST_UINT32: case CAST_UINT:
    case CAST_UINT64: case CAST_ULONG: case CAST_SIZE_T: case CAST_SSIZE_T:
        if(v.type == VAL_INT64) *(int64_t*)r->ptr = v.v.i64;
        else if(v.type == VAL_INT) *(int64_t*)r->ptr = (int64_t)v.v.i;
        else if(v.type == VAL_DOUBLE) *(int64_t*)r->ptr = (int64_t)v.v.d;
        break;
    case CAST_FLOAT: case CAST_DOUBLE: case CAST_LONG_DOUBLE:
        if(v.type == VAL_DOUBLE) *(double*)r->ptr = v.v.d;
        else if(v.type == VAL_INT64) *(double*)r->ptr = (double)v.v.i64;
        else if(v.type == VAL_INT) *(double*)r->ptr = (double)v.v.i;
        break;
    case CAST_STRING:
        if(v.type == VAL_STRING) *(char**)r->ptr = v.str_inline ? strdup(v.v.sso.data) : v.v.s;
        break;
    default:
        *(Value*)r->ptr = v;
        break;
    }
}

/* 确保帧的槽位数组已分配到至少 need 个元素 */
static void frame_ensure_slots(StackFrame* f, int need) {
    if(!f) return;
    if(need <= f->cap) return;
    int oldcap = f->cap;
    int newcap = f->cap > 0 ? f->cap : 16;
    while(newcap < need) newcap *= 2;

    /* 分配 names */
    if(!f->names) f->names = calloc(newcap, sizeof(char*));
    else { f->names = realloc(f->names, newcap * sizeof(char*)); memset(f->names + oldcap, 0, (newcap - oldcap) * sizeof(char*)); }

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
    else { f->ptr_slots = realloc(f->ptr_slots, newcap * sizeof(void*)); memset(f->ptr_slots + oldcap, 0, (newcap - oldcap) * sizeof(void*)); }

    /* 分配 type_tags */
    if(!f->type_tags) { f->type_tags = calloc(newcap, sizeof(int)); memset(f->type_tags, -1, newcap * sizeof(int)); }
    else { f->type_tags = realloc(f->type_tags, newcap * sizeof(int)); memset(f->type_tags + oldcap, -1, (newcap - oldcap) * sizeof(int)); }

    /* 分配 refs（ref 引用描述符指针数组，必须 NULL 初始化） */
    if(!f->refs) f->refs = calloc(newcap, sizeof(RefDesc*));
    else { f->refs = realloc(f->refs, newcap * sizeof(RefDesc*)); memset(f->refs + oldcap, 0, (newcap - oldcap) * sizeof(RefDesc*)); }

    f->cap = newcap;
}

/* ===== 变量存取（VALUE 栈） ===== */

/* LOAD_VAR：从帧槽位加载 Value 到 VALUE 栈；ref 槽 box 调用方存储 */
int vm_exec_var_load(VMExecCtx* ctx, Instruction* in) {
    int idx = in->a;
    frame_ensure_slots(ctx->frame, idx + 1);
    StackFrame* f = ctx->frame;
    if(f->refs && f->refs[idx]) {
        Value v = ref_box(f->refs[idx]);
        stack_vm_push(g_stack_mgr, STACK_VALUE, &v);
    } else {
        stack_vm_push(g_stack_mgr, STACK_VALUE, &f->vals[idx]);
    }
    return 1;
}

/* STORE_VAR：从 VALUE 栈弹值存储到帧槽位；ref 槽 unbox 到调用方存储 */
int vm_exec_var_store(VMExecCtx* ctx, Instruction* in) {
    int idx = in->a;
    frame_ensure_slots(ctx->frame, idx + 1);
    StackFrame* f = ctx->frame;
    Value* stk = (Value*)g_stack_mgr->stacks[STACK_VALUE];
    Value v = stk[--g_stack_mgr->sp[STACK_VALUE]];
    if(f->refs && f->refs[idx]) {
        ref_unbox(f->refs[idx], v);
    } else {
        f->vals[idx] = v;
        f->type_tags[idx] = (uint8_t)CAST_NONE;
    }
    return 1;
}

/* ===== 变量存取（INT64 栈） ===== */

/* LOAD_INT64_VAR：从帧 int_slots 加载到 INT64 栈；ref 槽读调用方 int 存储 */
int vm_exec_var_load_int64(VMExecCtx* ctx, Instruction* in) {
    int idx = in->a;
    StackFrame* f = ctx->frame;
    int64_t v = (f->refs && f->refs[idx]) ? *(int64_t*)f->refs[idx]->ptr : f->int_slots[idx];
    stack_vm_push(g_stack_mgr, STACK_INT64, &v);
    return 1;
}

/* STORE_INT64_VAR：从 INT64 栈弹值存储到帧 int_slots；ref 槽写调用方 int 存储 */
int vm_exec_var_store_int64(VMExecCtx* ctx, Instruction* in) {
    int idx = in->a;
    frame_ensure_slots(ctx->frame, idx + 1);
    StackFrame* f = ctx->frame;
    int sp = --g_stack_mgr->sp[STACK_INT64];
    int64_t v = ((int64_t*)g_stack_mgr->stacks[STACK_INT64])[sp];
    if(f->refs && f->refs[idx]) *(int64_t*)f->refs[idx]->ptr = v;
    else { f->int_slots[idx] = v; f->type_tags[idx] = (uint8_t)CAST_INT; }
    return 1;
}

/* ===== 变量存取（DOUBLE 栈） ===== */

/* LOAD_DOUBLE_VAR：从帧 flt_slots 加载到 DOUBLE 栈；ref 槽读调用方 double 存储 */
int vm_exec_var_load_double(VMExecCtx* ctx, Instruction* in) {
    int idx = in->a;
    StackFrame* f = ctx->frame;
    double v = (f->refs && f->refs[idx]) ? *(double*)f->refs[idx]->ptr : f->flt_slots[idx];
    stack_vm_push(g_stack_mgr, STACK_DOUBLE, &v);
    return 1;
}

/* STORE_DOUBLE_VAR：从 DOUBLE 栈弹值存储到帧 flt_slots；ref 槽写调用方 double 存储 */
int vm_exec_var_store_double(VMExecCtx* ctx, Instruction* in) {
    int idx = in->a;
    frame_ensure_slots(ctx->frame, idx + 1);
    StackFrame* f = ctx->frame;
    int sp = --g_stack_mgr->sp[STACK_DOUBLE];
    double v = ((double*)g_stack_mgr->stacks[STACK_DOUBLE])[sp];
    if(f->refs && f->refs[idx]) *(double*)f->refs[idx]->ptr = v;
    else { f->flt_slots[idx] = v; f->type_tags[idx] = (uint8_t)CAST_DOUBLE; }
    return 1;
}

/* ===== 变量存取（PTR 栈） ===== */

/* LOAD_PTR_VAR：从帧 ptr_slots 加载到 PTR 栈；ref 槽读调用方 ptr 存储 */
int vm_exec_var_load_ptr(VMExecCtx* ctx, Instruction* in) {
    int idx = in->a;
    StackFrame* f = ctx->frame;
    void* v = (f->refs && f->refs[idx]) ? *(void**)f->refs[idx]->ptr : f->ptr_slots[idx];
    stack_vm_push(g_stack_mgr, STACK_PTR, &v);
    return 1;
}

/* STORE_PTR_VAR：从 PTR 栈弹值存储到帧 ptr_slots；ref 槽写调用方 ptr 存储 */
int vm_exec_var_store_ptr(VMExecCtx* ctx, Instruction* in) {
    int idx = in->a;
    frame_ensure_slots(ctx->frame, idx + 1);
    StackFrame* f = ctx->frame;
    int sp = --g_stack_mgr->sp[STACK_PTR];
    void* v = ((void**)g_stack_mgr->stacks[STACK_PTR])[sp];
    if(f->refs && f->refs[idx]) *(void**)f->refs[idx]->ptr = v;
    else { f->ptr_slots[idx] = v; f->type_tags[idx] = (uint8_t)CAST_STRING; }
    return 1;
}
