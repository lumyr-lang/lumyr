/*
 * vm_exec_call.c - VM 函数调用指令
 * OPC_CALL：建帧 -> 逆序弹参/顺序绑定 -> 保存并切换执行状态 ->
 *           可重入执行 callee -> 恢复调用方 -> 按 callsite 处理返回值 -> 销毁帧
 * RETURN / RETURN_NIL 在 vm_exec.c 处理（只结束当前层）
 */
#include "vm_types.h"
#include "stack_manager.h"
#include "ast/stackframe.h"
#include "ir_compile.h"
#include "ir_arith.h"
#include "ir_types.h"
#include "ast/func_compile.h"
#include "vm_generator.h"
#include "lumyr_value_type.h"
#include "lm_value.h"
#include "lm_type.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 暂存一个已弹出的实参（跨 4 核心栈，按形参宽类型解释） */
typedef struct {
    ExprType et;
    int64_t i;
    double  d;
    void*   p;
    Value   v;
} ArgTmp;

/* 调用方需保存/恢复的执行状态 */
typedef struct {
    BytecodeFunc* fn;
    Instruction*  code;
    int           pc;
    StackFrame*   frame;
    ConstEntry*   const_pool;
    const char**  syms;
    int           const_cnt;
    int           sym_cnt;
} SavedState;

/* ========== 共用调用辅助 ========== */

/* pop_arg_slot：按 callee 形参 slot 的宽类型，从对应核心栈弹一个实参到 out */
static void pop_arg_slot(BytecodeFunc* callee, int slot, ArgTmp* out) {
    CastKind pck = (slot < callee->param_cnt && slot < callee->sym_cnt)
                   ? (CastKind)callee->var_type_tags[slot]
                   : CAST_NONE;
    ExprType et = castkind_to_exprtype(pck);
    out->et = et;
    switch (et) {
    case EXPR_TYPE_INT:    out->i = POP_INT64();  break;
    case EXPR_TYPE_DOUBLE: out->d = POP_DOUBLE(); break;
    case EXPR_TYPE_PTR:    out->p = POP_PTR();    break;
    default:               out->v = POP_VALUE();  break;
    }
}

/* vm_bind_only：建帧并按形参顺序绑定参数（ref 别名调用方槽位）。
 * 返回新建帧；调用方负责销毁（或由生成器接管）。 */
static StackFrame* vm_bind_only(BytecodeFunc* callee, CallSite* cs,
                                int argc, ArgTmp* args, StackFrame* parent_frame) {
    StackFrame* caller_frame = parent_frame;   /* ref 形参需引用调用方槽位 */
    StackFrame* new_frame = stackframe_new(parent_frame);
    int name_slots = callee->param_cnt + callee->has_variadic;
    for (int slot = 0; slot < argc; ++slot) {
        const char* pname = (slot < name_slots && callee->params[slot])
                            ? callee->params[slot] : "_";
        if (slot < callee->param_cnt && callee->param_is_ref
            && callee->param_is_ref[slot] && cs->arg_is_ref[slot]) {
            /* ref 形参：别名调用方槽位，丢弃栈上弹出的值 */
            stackframe_bind_ref(new_frame, pname, caller_frame, cs->arg_ref_slots[slot]);
            continue;
        }
        switch (args[slot].et) {
        case EXPR_TYPE_INT:
            stackframe_bind_int64(new_frame, pname, args[slot].i);
            break;
        case EXPR_TYPE_DOUBLE:
            stackframe_bind_double(new_frame, pname, args[slot].d);
            break;
        case EXPR_TYPE_PTR:
            stackframe_bind_ptr(new_frame, pname, args[slot].p);
            break;
        default:
            stackframe_bind(new_frame, pname, args[slot].v);
            break;
        }
    }
    return new_frame;
}

/* vm_bind_and_run：参数已弹出到 args[0..argc-1]。
 * 建帧按形参顺序绑定（ref 别名调用方槽位）→ 保存/切换执行状态 → 运行 callee →
 * 恢复状态 → 销毁帧。返回 loop 状态；ret 输出返回槽（detach 独立副本）。不处理返回值压栈。
 * 普通 CALL 与多态 CALL_METHOD 共用，保证建帧/状态切换语义单点维护。 */
static int vm_bind_and_run(VMExecCtx* ctx, BytecodeFunc* callee, CallSite* cs,
                           int argc, ArgTmp* args, RetSlot* ret) {
    StackFrame* new_frame = vm_bind_only(callee, cs, argc, args, ctx->frame);

    /* 保存调用方执行状态 */
    SavedState save;
    save.fn         = ctx->fn;
    save.code       = ctx->code;
    save.pc         = ctx->pc;
    save.frame      = ctx->frame;
    save.const_pool = ctx->const_pool;
    save.syms       = ctx->syms;
    save.const_cnt  = ctx->const_cnt;
    save.sym_cnt    = ctx->sym_cnt;

    /* 切换到 callee 的字节码/帧/常量池/符号表 */
    ctx->fn         = callee;
    ctx->code       = callee->code;
    ctx->pc         = 0;
    ctx->frame      = new_frame;
    ctx->const_pool = callee->const_pool;
    ctx->syms       = (const char**)callee->syms;
    ctx->const_cnt  = callee->const_cnt;
    ctx->sym_cnt    = callee->sym_cnt;

    /* 可重入执行 callee（嵌套调用会再次进入 vm_exec_loop） */
    int status = vm_exec_loop(ctx, ret);

    /* 恢复调用方执行状态 */
    ctx->fn         = save.fn;
    ctx->code       = save.code;
    ctx->pc         = save.pc;
    ctx->frame      = save.frame;
    ctx->const_pool = save.const_pool;
    ctx->syms       = save.syms;
    ctx->const_cnt  = save.const_cnt;
    ctx->sym_cnt    = save.sym_cnt;

    /* 销毁 callee 帧（返回值已在 RETURN 处 detach 为独立副本） */
    stackframe_destroy(new_frame);
    return status;
}

/* push_call_result：表达式语境按 callsite 记录的返回栈压入返回值；语句语境丢弃 */
static void push_call_result(CallSite* cs, RetSlot ret) {
    if (!cs->keep_result) return;
    switch ((ExprType)cs->ret_stack) {
    case EXPR_TYPE_INT:    PUSH_INT64(ret.i); break;
    case EXPR_TYPE_DOUBLE: PUSH_DOUBLE(ret.d); break;
    case EXPR_TYPE_PTR:    PUSH_PTR(ret.p); break;
    default:               stack_vm_push(g_stack_mgr, STACK_VALUE, &ret.v); break;
    }
}

/* ========== 普通函数调用 ========== */
int vm_exec_call(VMExecCtx* ctx, Instruction* in) {
    int cs_idx = in->a;
    CallSite* cs = &ctx->fn->callsites[cs_idx];
    int argc = cs->argc;

    BytecodeFunc* callee = ir_func_table_lookup(cs->callee);
    if (!callee) {
        fprintf(stderr, "VM: 调用未定义函数 %s\n", cs->callee);
        return 0;
    }

    /* 逆序弹实参（栈顶是最后一个实参），暂存以便顺序绑定 */
    ArgTmp* args = (ArgTmp*)malloc(sizeof(ArgTmp) * (argc > 0 ? argc : 1));
    if (!args) { perror("vm_exec_call args"); return 0; }
    for (int slot = argc - 1; slot >= 0; --slot)
        pop_arg_slot(callee, slot, &args[slot]);

    /* 生成器函数：建帧绑定参数，创建生成器对象压栈（不进入 vm_exec_loop） */
    if (callee->is_generator) {
        StackFrame* new_frame = vm_bind_only(callee, cs, argc, args, ctx->frame);
        free(args);
        GeneratorObject* gen = generator_new_with_frame(callee, new_frame);
        if (!gen) {
            fprintf(stderr, "VM: 创建生成器失败 %s\n", cs->callee);
            stackframe_destroy(new_frame);
            return 0;
        }
        RetSlot ret;
        memset(&ret, 0, sizeof(ret));
        ret.et = EXPR_TYPE_NONE;
        ret.v.type = VAL_GENERATOR;
        ret.v.v.generator = gen;
        push_call_result(cs, ret);
        return 1;
    }

    RetSlot ret;
    int status = vm_bind_and_run(ctx, callee, cs, argc, args, &ret);
    free(args);

    /* 异常穿过本调用：不压返回值，继续向调用方传播 */
    if (status == VM_LOOP_UNWIND) return VM_LOOP_UNWIND;

    push_call_result(cs, ret);
    return 1;
}

/* ========== 多态方法调用 CALL_METHOD ==========
 * 栈布局（自底向上）：receiver(PTR), 用户实参 slot1..argc-1（栈顶为最后一个）
 * cs->argc 含 receiver；cs->callee 为静态定义内部名 <DefClass>__m__<method>。
 * 实参按静态签名弹栈（重写方法签名兼容）；执行目标按 receiver 实际类型方法表解析 → 多态。 */
int vm_exec_call_method(VMExecCtx* ctx, Instruction* in) {
    int cs_idx = in->a;
    CallSite* cs = &ctx->fn->callsites[cs_idx];
    int argc = cs->argc;
    if (argc < 1) {
        fprintf(stderr, "VM: CALL_METHOD 缺少 receiver\n");
        return 0;
    }

    /* 静态签名 callee：用于用户实参的栈类型解析 */
    BytecodeFunc* static_fn = ir_func_table_lookup(cs->callee);
    if (!static_fn) {
        fprintf(stderr, "VM: 方法静态签名未注册 %s\n", cs->callee);
        return 0;
    }

    /* 1. 逆序弹用户实参 slot argc-1..1（receiver 留栈底最后弹） */
    ArgTmp* args = (ArgTmp*)calloc((size_t)argc, sizeof(ArgTmp));
    if (!args) { perror("vm_exec_call_method args"); return 0; }
    for (int slot = argc - 1; slot >= 1; --slot)
        pop_arg_slot(static_fn, slot, &args[slot]);

    /* 2. 弹 receiver（PTR），作为 slot 0（self） */
    void* recv_ptr = POP_PTR();
    args[0].et = EXPR_TYPE_PTR;
    args[0].p = recv_ptr;

    /* 3. 实例偏移 0 取 RuntimeTypeInfo → 按方法名查实际 RuntimeFunc */
    RuntimeTypeInfo* ri = recv_ptr ? *(RuntimeTypeInfo**)recv_ptr : NULL;
    if (!ri) {
        fprintf(stderr, "VM: 方法接收者缺少类型信息\n");
        free(args);
        return 0;
    }

    /* 方法名：从静态内部名解析 "__m__" 后缀 */
    const char* msep = strstr(cs->callee, "__m__");
    const char* mname = msep ? msep + 5 : NULL;
    RuntimeFunc* actual_rf = mname ? lumyr_type_find_method(ri, mname) : NULL;

    /* 取实际 BytecodeFunc（解释器方法）；FFI/原生方法当前不走本路径 */
    BytecodeFunc* actual_fn = NULL;
    if (actual_rf && interp_func_is_payload(actual_rf)) {
        InterpFuncPayload* pl = (InterpFuncPayload*)actual_rf->captures;
        actual_fn = pl->bytecode;
    }
    if (!actual_fn) {
        fprintf(stderr, "VM: 类型 \"%s\" 未找到方法 \"%s\" 的可执行实现\n",
                ri->name ? ri->name : "?", mname ? mname : "?");
        free(args);
        return 0;
    }

    /* 4. 建帧绑定（slot0=self, slot1+=实参）并执行实际方法 */
    RetSlot ret;
    int status = vm_bind_and_run(ctx, actual_fn, cs, argc, args, &ret);
    free(args);

    if (status == VM_LOOP_UNWIND) return VM_LOOP_UNWIND;

    /* 5. 按 callsite 静态返回类型压栈 */
    push_call_result(cs, ret);
    return 1;
}

/* ========== 获取函数值（裸函数名引用） ========== */
int vm_exec_getfunc(VMExecCtx* ctx, Instruction* in) {
    const char* name = ctx->syms[in->a];
    RuntimeFunc* rf = (RuntimeFunc*)calloc(1, sizeof(RuntimeFunc));
    if(!rf) { perror("vm_exec_getfunc"); return 0; }
    rf->name = strdup(name ? name : "");
    rf->captures = NULL;
    rf->capture_count = 0;
    Value v;
    v.type = VAL_FUNC;
    v.v.func.func_obj = rf;
    v.v.func.ffi_func = NULL;
    v.v.func.is_ffi = 0;
    stack_vm_push(g_stack_mgr, STACK_VALUE, &v);
    return 1;
}

/* 辅助：在 BytecodeFunc 符号表中按名查槽位下标 */
static int bf_find_slot(BytecodeFunc* fn, const char* name) {
    if(!fn || !name) return -1;
    for(int i = 0; i < fn->sym_cnt; i++) {
        if(fn->syms[i] && strcmp(fn->syms[i], name) == 0) return i;
    }
    return -1;
}

/* 从调用方帧的 typed 槽位读值并 box 成 Value */
static Value frame_slot_to_value(StackFrame* f, BytecodeFunc* fn, int slot) {
    Value v;
    if(slot < 0 || slot >= f->cap) { v.type = VAL_NONE; return v; }
    /* 捕获变量以 ref 别名绑定：值存储在 ref->ptr 指向的 cell 中 */
    if(slot < f->cnt && f->refs && f->refs[slot] && f->refs[slot]->ptr) {
        return *(Value*)f->refs[slot]->ptr;
    }
    int tag = (slot < fn->sym_cnt) ? fn->var_type_tags[slot] : -1;
    switch((CastKind)tag) {
    case CAST_INT: case CAST_INT8: case CAST_INT16: case CAST_INT32: case CAST_INT64:
    case CAST_LONGLONG: case CAST_LONG: case CAST_SHORT: case CAST_USHORT:
    case CAST_BOOL: case CAST_CHAR: case CAST_UCHAR: case CAST_BYTE: case CAST_ASCII:
    case CAST_UINT8: case CAST_UINT16: case CAST_UINT32: case CAST_UINT:
    case CAST_UINT64: case CAST_ULONG: case CAST_SIZE_T: case CAST_SSIZE_T:
        return lumyr_make_int64(f->int_slots[slot]);
    case CAST_FLOAT: case CAST_DOUBLE: case CAST_LONG_DOUBLE:
        return lumyr_make_double(f->flt_slots[slot]);
    case CAST_STRING: {
        v.type = VAL_STRING; v.str_inline = 0; v.v.s = (char*)f->ptr_slots[slot];
        return v;
    }
    default:
        return f->vals[slot];
    }
}

/* ========== 创建闭包（捕获外层变量） ========== */
int vm_exec_mkclosure(VMExecCtx* ctx, Instruction* in) {
    const char* lname = ctx->syms[in->a];
    int ncap = lambda_capture_count(lname);
    RuntimeFunc* rf = (RuntimeFunc*)calloc(1, sizeof(RuntimeFunc));
    if(!rf) { perror("vm_exec_mkclosure"); return 0; }
    rf->name = strdup(lname ? lname : "");
    rf->capture_count = ncap;
    if(ncap > 0) {
        Value** cells = (Value**)calloc((size_t)ncap, sizeof(Value*));
        if(!cells) { perror("mkclosure cells"); free(rf); return 0; }
        for(int i = 0; i < ncap; i++) {
            const char* cname = lambda_capture_name(lname, i);
            /* 优先复用帧中已有的 cell */
            Value** cellp = stackframe_find_cell(ctx->frame, cname);
            Value* cell = cellp ? *cellp : NULL;
            if(!cell) {
                int slot = bf_find_slot(ctx->fn, cname);
                if(slot < 0) {
                    fprintf(stderr, "VM: 闭包无法捕获未定义变量 %s\n", cname ? cname : "?");
                    free(cells); free(rf); return 0;
                }
                cell = (Value*)malloc(sizeof(Value));
                if(!cell) { perror("mkclosure cell"); free(cells); free(rf); return 0; }
                *cell = frame_slot_to_value(ctx->frame, ctx->fn, slot);
                stackframe_add_cell(ctx->frame, cname, cell);
            }
            cells[i] = cell;
        }
        rf->captures = (Value*)cells;
    } else {
        rf->captures = NULL;
    }
    Value v;
    v.type = VAL_FUNC;
    v.v.func.func_obj = rf;
    v.v.func.ffi_func = NULL;
    v.v.func.is_ffi = 0;
    stack_vm_push(g_stack_mgr, STACK_VALUE, &v);
    return 1;
}

/* ========== 动态方法绑定 ==========
 * obj 为 struct/class 实例，mname 为方法名：构造 VAL_FUNC bound method（携带实例指针）。
 * 用于 map/array 等动态取出的实例的 obj.m(args) 调用（编译期无法静态知类型）。
 * 返回 1=成功写入 *out；0=非实例/无此解释器方法（调用方可继续按字段缺失处理） */
int vm_make_bound_method(Value obj, const char* mname, Value* out) {
    if((obj.type != VAL_STRUCT_PTR && obj.type != VAL_CLASS_PTR) ||
       !obj.v.struct_ptr || !mname) return 0;
    RuntimeTypeInfo* ri = *(RuntimeTypeInfo**)obj.v.struct_ptr;
    if(!ri) return 0;
    RuntimeFunc* rf = lumyr_type_find_method(ri, mname);
    if(!rf || !interp_func_is_payload(rf)) return 0;
    InterpFuncPayload* pl = (InterpFuncPayload*)rf->captures;
    if(!pl || !pl->bytecode || !pl->bytecode->name) return 0;

    RuntimeFunc* bf = (RuntimeFunc*)calloc(1, sizeof(RuntimeFunc));
    if(!bf) return 0;
    bf->name = strdup(pl->bytecode->name);
    bf->bound_self = obj.v.struct_ptr;
    /* 保留闭包捕获（method RuntimeFunc 的 captures 即 InterpFuncPayload） */
    bf->captures = rf->captures;
    bf->capture_count = rf->capture_count;

    memset(out, 0, sizeof(*out));
    out->type = VAL_FUNC;
    out->v.func.func_obj = bf;
    out->v.func.ffi_func = NULL;
    out->v.func.is_ffi = 0;
    return 1;
}

/* ========== 函数值调用内核（OPC_CALLV 与 map/filter/reduce 高阶内置共用） ==========
 * 校验函数值 → 查全局函数表 → 新建帧按形参类型 unbox 绑定 → 闭包捕获 ref 别名 →
 * 保存/切换执行状态 → 执行 → 恢复 → 返回值 box 到 *out。
 * 返回 1=成功；0=失败（已打印错误）；VM_LOOP_UNWIND=异常穿过本调用 */
int vm_call_func_value(VMExecCtx* ctx, Value fv, int argc, Value* args, Value* out) {
    *out = val_none();
    if(fv.type != VAL_FUNC || !fv.v.func.func_obj || !fv.v.func.func_obj->name) {
        fprintf(stderr, "VM: 动态调用的值不是函数\n");
        return 0;
    }
    const char* fname = fv.v.func.func_obj->name;
    BytecodeFunc* callee = ir_func_table_lookup(fname);
    if(!callee) {
        fprintf(stderr, "VM: 动态调用未定义函数 %s\n", fname);
        return 0;
    }

    /* 新建帧，按形参类型 unbox 绑定 */
    StackFrame* new_frame = stackframe_new(ctx->frame);
    int name_slots = callee->param_cnt + callee->has_variadic;

    /* bound method：实例自动占 slot 0（self），用户实参整体后移一槽 */
    RuntimeFunc* rf0 = fv.v.func.func_obj;
    int slot_off = 0;
    if(rf0->bound_self) {
        void* bself = rf0->bound_self;
        RuntimeTypeInfo* bri = *(RuntimeTypeInfo**)bself;
        Value sv; memset(&sv, 0, sizeof(sv));
        sv.v.struct_ptr = bself;
        sv.type = (bri && bri->kind == TYPE_KIND_STRUCT) ? VAL_STRUCT_PTR : VAL_CLASS_PTR;
        const char* spname = (callee->param_cnt > 0 && callee->params[0]) ? callee->params[0] : "self";
        CastKind spck = (callee->param_cnt > 0 && callee->sym_cnt > 0)
                        ? (CastKind)callee->var_type_tags[0] : CAST_NONE;
        ExprType set = castkind_to_exprtype(spck);
        if(set == EXPR_TYPE_INT) {
            stackframe_bind_int64(new_frame, spname, (int64_t)(uintptr_t)bself);
        } else if(set == EXPR_TYPE_DOUBLE) {
            stackframe_bind_double(new_frame, spname, 0.0);
        } else {
            /* PTR self：必须写 ptr_slots（LOAD_PTR_VAR 读它）；
             * stackframe_bind 只写 vals 不写 ptr_slots，self 会变 NULL */
            stackframe_bind_ptr(new_frame, spname, bself);
        }
        slot_off = 1;
    }

    for(int slot = 0; slot < argc; ++slot) {
        int fslot = slot + slot_off;
        const char* pname = (fslot < name_slots && callee->params[fslot])
                            ? callee->params[fslot] : "_";
        CastKind pck = (fslot < callee->param_cnt && fslot < callee->sym_cnt)
                       ? (CastKind)callee->var_type_tags[fslot]
                       : CAST_NONE;
        ExprType et = castkind_to_exprtype(pck);
        switch(et) {
        case EXPR_TYPE_INT: {
            int64_t iv = 0;
            if(args[slot].type == VAL_INT64) iv = args[slot].v.i64;
            else if(args[slot].type == VAL_INT) iv = (int64_t)args[slot].v.i;
            else if(args[slot].type == VAL_DOUBLE) iv = (int64_t)args[slot].v.d;
            stackframe_bind_int64(new_frame, pname, iv);
            break;
        }
        case EXPR_TYPE_DOUBLE: {
            double dv = 0;
            if(args[slot].type == VAL_DOUBLE) dv = args[slot].v.d;
            else if(args[slot].type == VAL_INT64) dv = (double)args[slot].v.i64;
            else if(args[slot].type == VAL_INT) dv = (double)args[slot].v.i;
            stackframe_bind_double(new_frame, pname, dv);
            break;
        }
        default:
            stackframe_bind(new_frame, pname, args[slot]);
            break;
        }
    }

    /* 闭包：把捕获的 cell 绑定到 callee 帧对应槽位（通过 ref 别名） */
    RuntimeFunc* rf = fv.v.func.func_obj;
    if(rf->capture_count > 0 && rf->captures) {
        Value** cells = (Value**)rf->captures;
        const char* lname = rf->name;
        int ncap = lambda_capture_count(lname);
        for(int i = 0; i < ncap && i < rf->capture_count; i++) {
            const char* cname = lambda_capture_name(lname, i);
            int cslot = bf_find_slot(callee, cname);
            if(cslot >= 0 && cells[i]) {
                /* 确保槽位已分配（绑定一个占位 NONE），再用 ref 别名覆盖 */
                Value none; none.type = VAL_NONE; none.v.i = 0;
                stackframe_bind(new_frame, cname, none);
                RefDesc* rd = (RefDesc*)malloc(sizeof(RefDesc));
                if(rd) {
                    rd->ptr = cells[i];
                    rd->type = (int)CAST_NONE;
                    new_frame->refs[cslot] = rd;
                }
            }
        }
    }

    /* 保存/切换执行状态 */
    SavedState save;
    save.fn = ctx->fn; save.code = ctx->code; save.pc = ctx->pc; save.frame = ctx->frame;
    save.const_pool = ctx->const_pool; save.syms = ctx->syms;
    save.const_cnt = ctx->const_cnt; save.sym_cnt = ctx->sym_cnt;

    ctx->fn = callee; ctx->code = callee->code; ctx->pc = 0; ctx->frame = new_frame;
    ctx->const_pool = callee->const_pool; ctx->syms = (const char**)callee->syms;
    ctx->const_cnt = callee->const_cnt; ctx->sym_cnt = callee->sym_cnt;

    RetSlot ret;
    int status = vm_exec_loop(ctx, &ret);

    ctx->fn = save.fn; ctx->code = save.code; ctx->pc = save.pc; ctx->frame = save.frame;
    ctx->const_pool = save.const_pool; ctx->syms = save.syms;
    ctx->const_cnt = save.const_cnt; ctx->sym_cnt = save.sym_cnt;

    stackframe_destroy(new_frame);

    if(status == VM_LOOP_UNWIND) return VM_LOOP_UNWIND;

    /* 返回值统一 box */
    switch((ExprType)ret.et) {
    case EXPR_TYPE_INT:    *out = lumyr_make_int64(ret.i); break;
    case EXPR_TYPE_DOUBLE: *out = lumyr_make_double(ret.d); break;
    case EXPR_TYPE_PTR:
        out->type = VAL_STRING; out->str_inline = 0; out->v.s = (char*)ret.p;
        break;
    default: *out = ret.v; break;
    }
    return 1;
}

/* ========== 动态调用 CALLV ==========
 * 栈布局（自底向上）：函数值, arg0, arg1, ..., arg_{argc-1}（argc-1 在栈顶）
 * in->b = argc。所有实参均为 VALUE（动态调用）。
 */
int vm_exec_callv(VMExecCtx* ctx, Instruction* in) {
    int argc = in->b;

    /* 逆序弹 argc 个实参（均为 VALUE） */
    Value* args = (Value*)malloc(sizeof(Value) * (argc > 0 ? argc : 1));
    if(!args) { perror("vm_exec_callv args"); return 0; }
    for(int slot = argc - 1; slot >= 0; --slot) {
        stack_vm_pop(g_stack_mgr, STACK_VALUE, &args[slot]);
    }

    /* 弹函数值，交内核执行 */
    Value fv;
    stack_vm_pop(g_stack_mgr, STACK_VALUE, &fv);
    Value rv;
    int rc = vm_call_func_value(ctx, fv, argc, args, &rv);
    free(args);
    if(rc == 1) stack_vm_push(g_stack_mgr, STACK_VALUE, &rv);
    return rc;
}
