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
    case CAST_INT: case CAST_INT_INFER: case CAST_INT8: case CAST_INT16: case CAST_INT32: case CAST_INT64:
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
    default: {
        /* CAST_NONE cell 不变量：字符串必须保持非内联表示。
         * mkclosure 会在定义帧为被捕获变量挂 CAST_NONE cell ref，同帧的 typed
         * PTR 存取（vm_exec_var_load/store_ptr 的 CAST_NONE 分支）直接读
         * Value.v.struct_ptr；若 cell 被动态写入 SSO 内联串，typed 读会把内联
         * 字节误当指针（字符串拼接时 strlen 解引用非法地址 SEGV）。
         * 动态读写方读到 VAL_STRING 非内联，语义完全等价。 */
        Value wv = v;
        if(wv.type == VAL_STRING && wv.str_inline) {
            wv.str_inline = 0;
            wv.v.s = strdup(wv.v.sso.data);
        }
        *(Value*)r->ptr = wv;
        break;
    }
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

/* 主线程 main 根帧：全局变量的唯一存储位置。工作线程自身根帧 parent=NULL，
 * 若沿本线程帧链找"根帧"会误把线程根帧当 main 帧（全局槽不存在→读空）。
 * 主线程启动时由 vm_set_main_root_frame 登记；所有线程共享此指针。 */
StackFrame* g_main_root_frame = NULL;
void vm_set_main_root_frame(StackFrame* f) { g_main_root_frame = f; }

/* LOAD_GLOBAL：函数体内读顶层(main 根帧)变量。
 * in->a = 根帧槽位索引（ir_compile_main 末尾 fixup 已按名字解析）；
 * in->b = PTR 族精确类型提示（3 string / 4 bigint / 5 decimal / 6 bitdecimal / 7 裸 ptr），
 *         仅在根帧槽位运行时为 PTR 存储（type_tags==CAST_STRING）时用于消歧。
 * 运行时按根帧槽位当前存储族装箱：INT 族→VAL_INT64、DOUBLE 族→VAL_DOUBLE、
 * PTR 族→按 b 装箱 ptr_slots、其余→vals[a] 原样（动态变量/容器/实例）。 */
int vm_exec_var_load_global(VMExecCtx* ctx, Instruction* in) {
    int idx = in->a;
    /* 优先主线程 main 帧（工作线程读全局）；未登记时（极少数早期路径）
     * 退回沿本线程帧链找根帧，保持旧行为可用 */
    StackFrame* root = g_main_root_frame;
    if(!root) {
        root = ctx->frame;
        while(root && root->parent) root = root->parent;
    }
    if(!root) {
        Value v = val_none();
        stack_vm_push(g_stack_mgr, STACK_VALUE, &v);
        return 1;
    }
    frame_ensure_slots(root, idx + 1);
    int rtag = (idx < root->cap) ? root->type_tags[idx] : -1;
    Value v;
    memset(&v, 0, sizeof(v));
    switch(rtag) {
    case CAST_INT:
        /* 所有整数/布尔/字符统一 int64 宽槽 */
        v = lumyr_make_int64(root->int_slots[idx]);
        break;
    case CAST_DOUBLE:
        v = lumyr_make_double(root->flt_slots[idx]);
        break;
    /* PTR 族：帧标签已携带精确类型，按标签装箱。
     * CAST_STRING 仍兼容编译期 fixup 提示（in.b：bigint/decimal/裸ptr） */
    case CAST_CLASS_PTR:
        v.type = VAL_CLASS_PTR; v.v.struct_ptr = root->ptr_slots[idx]; break;
    case CAST_STRUCT_PTR:
        v.type = VAL_STRUCT_PTR; v.v.struct_ptr = root->ptr_slots[idx]; break;
    case CAST_BIGINT:
        v.type = VAL_BIGINT; v.v.bigint = root->ptr_slots[idx]; break;
    case CAST_DECIMAL:
        v.type = VAL_DECIMAL; v.v.decimal = root->ptr_slots[idx]; break;
    case CAST_BITDECIMAL:
        v.type = VAL_BITDECIMAL; v.v.bitdecimal = root->ptr_slots[idx]; break;
    case CAST_PTR:
        v.type = VAL_PTR; v.v.struct_ptr = root->ptr_slots[idx]; break;
    case CAST_STRING: {
        void* p = root->ptr_slots[idx];
        switch(in->b) {
        case 4:  v.type = VAL_BIGINT;     v.v.bigint = p; break;
        case 5:  v.type = VAL_DECIMAL;    v.v.decimal = p; break;
        case 6:  v.type = VAL_BITDECIMAL; v.v.bitdecimal = p; break;
        case 7:  v.type = VAL_PTR;        v.v.struct_ptr = p; break;
        default: v.type = VAL_STRING;     v.str_inline = 0; v.v.s = (char*)p; break;
        }
        break;
    }
    default:
        /* 动态变量（容器/函数/null 等）：vals 原样 */
        v = root->vals[idx];
        break;
    }
    stack_vm_push(g_stack_mgr, STACK_VALUE, &v);
    return 1;
}

/* LOAD_VAR：从帧槽位加载 Value 到 VALUE 栈；ref 槽 box 调用方存储 */
int vm_exec_var_load(VMExecCtx* ctx, Instruction* in) {
    int idx = in->a;
    frame_ensure_slots(ctx->frame, idx + 1);
    StackFrame* f = ctx->frame;
    if(f->refs && f->refs[idx]) {
        Value v = ref_box(f->refs[idx]);
        stack_vm_push(g_stack_mgr, STACK_VALUE, &v);
    } else {
        Value v = f->vals[idx];
        stack_vm_push(g_stack_mgr, STACK_VALUE, &v);
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
    int64_t v;
    if(f->refs && f->refs[idx]) {
        RefDesc* r = f->refs[idx];
        if(r->type == CAST_NONE) v = lumyr_extract_ll(*(Value*)r->ptr);  /* mkclosure 的 Value-cell */
        else v = *(int64_t*)r->ptr;                                       /* typed 引用存储 */
    } else v = f->int_slots[idx];
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
    if(f->refs && f->refs[idx]) {
        RefDesc* r = f->refs[idx];
        if(r->type == CAST_NONE) *(Value*)r->ptr = lumyr_make_int64(v);
        else *(int64_t*)r->ptr = v;
    }
    else { f->int_slots[idx] = v; f->type_tags[idx] = (uint8_t)CAST_INT; }
    return 1;
}

/* ===== 变量存取（DOUBLE 栈） ===== */

/* LOAD_DOUBLE_VAR：从帧 flt_slots 加载到 DOUBLE 栈；ref 槽读调用方 double 存储 */
int vm_exec_var_load_double(VMExecCtx* ctx, Instruction* in) {
    int idx = in->a;
    StackFrame* f = ctx->frame;
    double v;
    if(f->refs && f->refs[idx]) {
        RefDesc* r = f->refs[idx];
        if(r->type == CAST_NONE) v = lumyr_extract_double(*(Value*)r->ptr);
        else v = *(double*)r->ptr;
    } else v = f->flt_slots[idx];
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
    if(f->refs && f->refs[idx]) {
        RefDesc* r = f->refs[idx];
        if(r->type == CAST_NONE) *(Value*)r->ptr = lumyr_make_double(v);
        else *(double*)r->ptr = v;
    }
    else { f->flt_slots[idx] = v; f->type_tags[idx] = (uint8_t)CAST_DOUBLE; }
    return 1;
}

/* ===== 变量存取（PTR 栈） ===== */

/* LOAD_PTR_VAR：从帧 ptr_slots 加载到 PTR 栈；ref 槽读调用方 ptr 存储 */
int vm_exec_var_load_ptr(VMExecCtx* ctx, Instruction* in) {
    int idx = in->a;
    StackFrame* f = ctx->frame;
    void* v;
    if(f->refs && f->refs[idx]) {
        RefDesc* r = f->refs[idx];
        if(r->type == CAST_NONE) v = ((Value*)r->ptr)->v.struct_ptr;
        else v = *(void**)r->ptr;
    } else v = f->ptr_slots[idx];
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
    if(f->refs && f->refs[idx]) {
        RefDesc* r = f->refs[idx];
        if(r->type == CAST_NONE) ((Value*)r->ptr)->v.struct_ptr = v;  /* 保留 cell Value 类型 */
        else *(void**)r->ptr = v;
    }
    else {
        /* b=右值精确 CastKind（class/struct/string/bigint...）；
         * 旧字节码 b=0 时退回 CAST_STRING（绝大多数 PTR 变量是字符串） */
        f->ptr_slots[idx] = v;
        f->type_tags[idx] = (uint8_t)(in->b > 0 ? in->b : CAST_STRING);
    }
    return 1;
}
