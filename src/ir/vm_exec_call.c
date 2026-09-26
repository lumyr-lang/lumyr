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
#include "ast/lumyr_types.h"
#include "vm_generator.h"
#include "lumyr_value_type.h"
#include "lumyr_value.h"
#include "lm_value.h"
#include "lm_type.h"
#include "vm_exec.h"
#include "lm_formdata.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 主线程 main 根帧（vm_exec_var.c 登记）：闭包捕获全局变量时必须从此帧读，
 * 工作线程沿自身帧链找到的根帧不含全局槽位 */
extern StackFrame* g_main_root_frame;

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

    /* 可重入执行 callee（嵌套调用会再次进入 vm_exec_guarded） */
    int status = vm_exec_guarded(ctx, ret);

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
    /* PEND_RETURN 路径返回值装箱在 ret.v：按 callsite 返回栈拆箱到 i/d/p */
    ret = vm_ret_slot_for_callstack(ret, cs->ret_stack);
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
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "VM: 调用未定义函数 %s / call to undefined function %s",
                 cs->callee, cs->callee);
        runtime_error(buf);
        return 0;   /* 不可达 */
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
        free(args);
        runtime_error("VM: 不能对 null 调用方法（方法接收者缺少类型信息）/ cannot call a method on null (receiver has no type info)");
        return 0;   /* 不可达 */
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

/* ========== 动态方法调用 CALL_METHODV ==========
 * owner 类型未知的方法调用统一入口（编译期无法区分 receiver 是用户实例还是
 * 原生容器）。此前经 INDEX+CALLV 兜底：实例走 bound method 尚可，但原生容器的
 * 内置方法名（map.get/arr.len 等）在 INDEX 阶段被当键查询而丢失方法语义。
 * 运行时按 receiver 实际类型分派：
 *   用户实例（VAL_STRUCT_PTR/VAL_CLASS_PTR）→ 方法表 bound 分派（用户方法优先）；
 *   map/formdata → 先查键（支持 map 存函数值的动态调用），miss 再按内置方法名
 *   经 builtin_dispatch 兜底（原生容器 len/get/... 的方法形式）；
 *   其余 → 标准类型错误。
 * a=callsite 下标（callee=方法名，argc 含 receiver）；VALUE 栈顶 argc 个 */
int vm_exec_call_method_dyn(VMExecCtx* ctx, const Instruction* in) {
    int cs_idx = in->a;
    CallSite* cs = &ctx->fn->callsites[cs_idx];
    int argc = cs->argc;              /* 含 receiver */
    const char* mname = cs->callee;
    if (argc < 1 || !mname) {
        fprintf(stderr, "VM: CALL_METHODV 缺少 receiver 或方法名\n");
        return 0;
    }

    Value* argv = (Value*)malloc(sizeof(Value) * (size_t)argc);
    if (!argv) { perror("vm_exec_call_method_dyn"); return 0; }
    for (int i = argc - 1; i >= 0; --i)
        stack_vm_pop(g_stack_mgr, STACK_VALUE, &argv[i]);
    Value recv = argv[0];

    /* 1. 用户实例：方法表分派（bound method，self 自动占 slot0） */
    if ((recv.type == VAL_STRUCT_PTR || recv.type == VAL_CLASS_PTR) &&
        recv.v.struct_ptr) {
        Value fv;
        if (!vm_make_bound_method(recv, mname, &fv)) {
            Value tn = lumyr_type(recv);
            const char* tns = lumyr_str_cstr(&tn);
            char buf[512];
            snprintf(buf, sizeof(buf),
                     "运行时错误: 类型 %s 没有方法 \"%s\" 的可执行实现 / runtime error: type %s has no executable implementation of method \"%s\"",
                     tns ? tns : "?", mname, tns ? tns : "?", mname);
            free(argv);
            runtime_error(buf);
            return 0;   /* 不可达 */
        }
        Value outv;
        int status = vm_call_func_value(ctx, fv, argc - 1, &argv[1], &outv);
        free(argv);
        /* 与 vm_exec_callv 同构：rc==1（成功）压结果；
         * 不可用 VM_LOOP_UNWIND(1) 特判——它与成功值 1 冲突，
         * 误判会跳过压栈导致调用方拿到空栈（.size 返回 null 的根因） */
        if (status == 1 && cs->keep_result)
            stack_vm_push(g_stack_mgr, STACK_VALUE, &outv);
        return status;
    }

    /* 2. map/formdata：先查键（容器可存函数值，字段调用优先于内置） */
    if (recv.type == VAL_MAP || recv.type == VAL_FORMDATA) {
        Value fv;
        memset(&fv, 0, sizeof(fv));
        if (recv.type == VAL_MAP) {
            Value key = lumyr_make_string(mname);
            fv = lumyr_map_get(recv, key);
        } else {
            fv = lumyr_formdata_get_by_name(recv, lumyr_make_string(mname));
        }
        if (fv.type == VAL_FUNC && fv.v.func.func_obj) {
            Value outv;
            int status = vm_call_func_value(ctx, fv, argc - 1, &argv[1], &outv);
            free(argv);
            /* 与 vm_exec_callv 同构：rc==1（成功）压结果；
             * vm_call_func_value 的 UNWIND 也返回 1，无法区分，统一按成功压栈 */
            if (status == 1 && cs->keep_result)
                stack_vm_push(g_stack_mgr, STACK_VALUE, &outv);
            return status;
        }
    }

    /* 3. 内置方法兜底：数组/字符串/类型化数组/map 等所有内置类型的动态方法调用
     * （编译期"用户方法优先"豁免内置截胡后落到本指令的内置名统一在此分派）
     * is_method=1：argv[0]=receiver，argc-1 个实参。
     * 返回 1=成功（压结果）、0=失败；与 vm_exec_callv 同构，
     * 不可用 VM_LOOP_UNWIND(1) 特判（与成功值 1 冲突） */
    int bid = builtin_id_by_name(mname);
    if (bid >= 0) {
        Value outv;
        int status = builtin_dispatch(ctx, bid, argv, argc - 1, &outv, 1);
        free(argv);
        if (status == 1 && cs->keep_result)
            stack_vm_push(g_stack_mgr, STACK_VALUE, &outv);
        return status;
    }

    /* 4. 其余：标准类型错误（经 runtime_error 走协作式 throw，try 可捕获） */
    Value tn = lumyr_type(recv);
    const char* tns = lumyr_str_cstr(&tn);
    char buf[512];
    snprintf(buf, sizeof(buf),
             "运行时错误: 类型 %s 不支持方法 .%s / runtime error: type %s does not support method .%s",
             tns ? tns : "?", mname, tns ? tns : "?", mname);
    free(argv);
    runtime_error(buf);
    return 0;   /* 不可达 */
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

/* 按帧自身 type_tags 从指定槽位读值并 box 成 Value。
 * 用途：闭包捕获——从当前函数帧槽位、或根帧(main)槽位取值建 cell。
 * 帧槽位纯索引工作，运行时帧不维护变量名。hint 用于 PTR 族消歧
 * （CAST_STRING 存储的 string/bigint/decimal/bitdecimal/ptr，编译期 fixup
 * 从 main 变量精确标记取得）：0=string，4=bigint，5=decimal，6=bitdecimal，7=ptr。 */
static Value frame_slot_value_at(StackFrame* f, int slot, int ptrHint) {
    Value v;
    v.type = VAL_NONE; v.v.i = 0;
    if(!f || slot < 0 || slot >= f->cap) { v.type = VAL_NONE; return v; }
    /* ref 别名槽 */
    if(slot < f->cnt && f->refs && f->refs[slot] && f->refs[slot]->ptr)
        return *(Value*)f->refs[slot]->ptr;
    int tag = f->type_tags[slot];
    switch((CastKind)tag) {
    case CAST_INT: case CAST_INT_INFER: case CAST_INT8: case CAST_INT16: case CAST_INT32: case CAST_INT64:
    case CAST_LONGLONG: case CAST_LONG: case CAST_SHORT: case CAST_USHORT:
    case CAST_BOOL: case CAST_CHAR: case CAST_UCHAR: case CAST_BYTE: case CAST_ASCII:
    case CAST_UINT8: case CAST_UINT16: case CAST_UINT32: case CAST_UINT:
    case CAST_UINT64: case CAST_ULONG: case CAST_SIZE_T: case CAST_SSIZE_T:
        return lumyr_make_int64(f->int_slots[slot]);
    case CAST_FLOAT: case CAST_DOUBLE: case CAST_LONG_DOUBLE:
        return lumyr_make_double(f->flt_slots[slot]);
    case CAST_STRING: {
        void* p = f->ptr_slots[slot];
        switch(ptrHint) {
        case 4:  v.type = VAL_BIGINT;     v.v.bigint = p; return v;
        case 5:  v.type = VAL_DECIMAL;    v.v.decimal = p; return v;
        case 6:  v.type = VAL_BITDECIMAL; v.v.bitdecimal = p; return v;
        case 7:  v.type = VAL_PTR;        v.v.struct_ptr = p; return v;
        default: v.type = VAL_STRING; v.str_inline = 0; v.v.s = (char*)p; return v;
        }
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
                int localSlot = -1;   /* >=0：捕获自当前函数帧局部槽 */
                Value boxed;
                if(slot >= 0) {
                    localSlot = slot;
                    stackframe_ensure_slots(ctx->frame, slot + 1);
                    CastKind ltag = (CastKind)((slot < ctx->fn->sym_cnt)
                                      ? ctx->fn->var_type_tags[slot] : -1);
                    int lhint = 0;
                    if(ltag == CAST_BIGINT) lhint = 4;
                    else if(ltag == CAST_DECIMAL) lhint = 5;
                    else if(ltag == CAST_BITDECIMAL) lhint = 6;
                    else if(ltag == CAST_PTR) lhint = 7;
                    boxed = frame_slot_value_at(ctx->frame, slot, lhint);
                    /* PTR 族槽的帧 vals 镜像由 bind_ptr 写成通用 VAL_PTR；但闭包内
                     * 字段访问 (INDEX_GET) 只认精确的 VAL_STRUCT_PTR/VAL_CLASS_PTR，
                     * VAL_PTR 落空返回 null（lambda 捕获 self 后 self.base 读空）。
                     * 按编译期 ltag 把 VAL_PTR 重构为精确 Value 类型（裸指针同址）。
                     * 漏修正则 string/bigint/decimal 形参捕获后变空值：
                     * union 槽指针在但类型错，拼接/算术按非字符串处理丢弃。 */
                    if(boxed.type == VAL_PTR) {
                        if(ltag == CAST_STRUCT_PTR) boxed.type = VAL_STRUCT_PTR;
                        else if(ltag == CAST_CLASS_PTR) boxed.type = VAL_CLASS_PTR;
                        else if(ltag == CAST_STRING) {
                            boxed.type = VAL_STRING;
                            boxed.str_inline = 0;
                        }
                        else if(ltag == CAST_BIGINT) boxed.type = VAL_BIGINT;
                        else if(ltag == CAST_DECIMAL) boxed.type = VAL_DECIMAL;
                        else if(ltag == CAST_BITDECIMAL) boxed.type = VAL_BITDECIMAL;
                    }
                } else {
                    /* 当前函数帧无此变量：查全局捕获槽侧表（lambda 捕获的顶层变量），
                     * 从根帧(main)槽位按编译期 hint 取值建 cell；侧表无则真未定义。
                     * 不能运行时按名沿链找——运行时帧不维护变量名。 */
                    int ghint = 0;
                    int gslot = func_compile_get_global_cap(lname, cname, &ghint);
                    if(gslot >= 0) {
                        /* 从主线程 main 帧取全局捕获（工作线程根帧无此槽）；
                         * 未登记时退回沿本线程帧链，兼容旧路径 */
                        StackFrame* rootFrame = g_main_root_frame;
                        if(!rootFrame) {
                            rootFrame = ctx->frame;
                            while(rootFrame && rootFrame->parent) rootFrame = rootFrame->parent;
                        }
                        boxed = frame_slot_value_at(rootFrame, gslot, ghint);
                    } else {
                        fprintf(stderr, "VM: 闭包无法捕获未定义变量 %s\n", cname ? cname : "?");
                        free(cells); free(rf); return 0;
                    }
                }
                cell = (Value*)malloc(sizeof(Value));
                if(!cell) { perror("mkclosure cell"); free(cells); free(rf); return 0; }
                *cell = boxed;
                stackframe_add_cell(ctx->frame, cname, cell);
                if(localSlot >= 0) {
                    /* 当前帧同名槽挂为 cell 的 ref 别名：随后的 STORE_VAR（如递归
                     * lambda 自引用赋值）写入 cell，lambda 内 refs 也指向同一 cell，
                     * 闭包才能读到定义完成后的自身（letrec 语义）。
                     * 全局侧表路径不在根帧挂——不改变顶层变量存储。 */
                    if(!ctx->frame->refs[localSlot]) {
                        RefDesc* rd = (RefDesc*)malloc(sizeof(RefDesc));
                        if(rd) {
                            rd->ptr = cell;
                            rd->type = (int)CAST_NONE;
                            ctx->frame->refs[localSlot] = rd;
                        }
                    }
                }
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
/* 评估字面量默认值 AST → Value（仅简单字面量：string/int/double/bool/char/null）。
 * 复杂表达式返回 NONE，由调用方自行处理。
 * 用途：动态调用（OPC_CALLV）时，当实参数少于形参数，用默认值填补缺失形参 */
static Value eval_literal_default(AstNode* dflt) {
    if(!dflt) { Value v; v.type = VAL_NONE; v.v.i = 0; return v; }
    switch(dflt->type) {
    case AST_INT:    return lumyr_make_int64(dflt->u.inum);
    case AST_NUM:    return lumyr_make_double(dflt->u.num);
    case AST_BOOL:  return lumyr_make_bool(dflt->u.bval);
    case AST_CHAR:   return lumyr_make_int64((int64_t)dflt->u.ch);
    case AST_STRING: return lumyr_make_string(dflt->u.sval);
    case AST_NONE: { Value v; v.type = VAL_NONE; v.v.i = 0; return v; }
    default: break;
    }
    Value v; v.type = VAL_NONE; v.v.i = 0;
    return v;
}

/* 默认值求值辅助：数值提取（int/bool/char 归一 int64；double 单列） */
static int dv_num(Value v, int64_t* pi, double* pd, int* is_int) {
    switch(v.type) {
    case VAL_INT64:  *pi = v.v.i64; *pd = (double)v.v.i64; *is_int = 1; return 1;
    case VAL_INT:    *pi = v.v.i;   *pd = (double)v.v.i;   *is_int = 1; return 1;
    case VAL_BOOL:   *pi = v.v.b ? 1 : 0; *pd = *pi ? 1.0 : 0.0; *is_int = 1; return 1;
    case VAL_CHAR:   *pi = (int64_t)(unsigned char)v.v.c; *pd = (double)*pi; *is_int = 1; return 1;
    case VAL_DOUBLE: *pd = v.v.d; *pi = (int64_t)v.v.d; *is_int = 0; return 1;
    default: return 0;
    }
}
static int dv_truthy(Value v) {
    switch(v.type) {
    case VAL_INT64:  return v.v.i64 != 0;
    case VAL_INT:    return v.v.i != 0;
    case VAL_BOOL:   return v.v.b != 0;
    case VAL_CHAR:   return v.v.c != 0;
    case VAL_DOUBLE: return v.v.d != 0.0;
    case VAL_NONE:   return 0;
    default:         return 1;   /* 字符串/容器等引用类型非空即真 */
    }
}

/* 动态调用默认参数表达式求值（调用点编译期未知 callee，运行时在 callee 帧上下文求值）。
 * 支持：字面量 / 变量（闭包捕获 cell 优先=词法作用域，其次沿帧链）/ 一元 / 二元算术与比较。
 * 返回 1=成功写 *out；0=表达式超出支持范围（调用方按无兜底原则报错中止）。 */
static int eval_default_expr(AstNode* e, StackFrame* frame, RuntimeFunc* rf, Value* out) {
    if(!e) { *out = val_none(); return 1; }
    switch(e->type) {
    case AST_INT: case AST_NUM: case AST_BOOL:
    case AST_CHAR: case AST_STRING: case AST_NONE:
        *out = eval_literal_default(e);
        return 1;
    case AST_VAR: {
        const char* name = e->u.varname;
        /* 闭包捕获优先（词法作用域）；cells 与捕获名表一一对应 */
        if(rf && rf->name && rf->captures && rf->capture_count > 0) {
            Value** cells = (Value**)rf->captures;
            int ncap = lambda_capture_count(rf->name);
            for(int i = 0; i < ncap && i < rf->capture_count; i++) {
                const char* cn = lambda_capture_name(rf->name, i);
                if(cn && cells[i] && strcmp(cn, name) == 0) { *out = *cells[i]; return 1; }
            }
        }
        _Bool found = 0;
        Value v = stackframe_get(frame, name, &found);
        if(found) { *out = v; return 1; }
        return 0;
    }
    case AST_UNARY: {
        Value a;
        if(!eval_default_expr(e->u.uny.child, frame, rf, &a)) return 0;
        int64_t ai; double ad; int aint;
        switch(e->u.uny.op) {
        case OP_UNARY_MINUS: case OP_UNARY_PLUS: {
            if(!dv_num(a, &ai, &ad, &aint)) return 0;
            int neg = (e->u.uny.op == OP_UNARY_MINUS);
            if(aint) *out = lumyr_make_int64(neg ? -ai : ai);
            else     *out = lumyr_make_double(neg ? -ad : ad);
            return 1;
        }
        case OP_LOGIC_NOT:
            *out = lumyr_make_bool(!dv_truthy(a));
            return 1;
        default: return 0;
        }
    }
    case AST_BINOP: {
        Value a, b;
        if(!eval_default_expr(e->u.bin.left, frame, rf, &a)) return 0;
        if(!eval_default_expr(e->u.bin.right, frame, rf, &b)) return 0;
        BinOp op = e->u.bin.op;
        if(op == OP_LOGIC_AND || op == OP_LOGIC_OR) {
            int r = (op == OP_LOGIC_AND) ? (dv_truthy(a) && dv_truthy(b))
                                         : (dv_truthy(a) || dv_truthy(b));
            *out = lumyr_make_bool(r);
            return 1;
        }
        int64_t ai, bi; double ad, bd; int aint, bint;
        if(!dv_num(a, &ai, &ad, &aint) || !dv_num(b, &bi, &bd, &bint)) return 0;
        switch(op) {
        case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: case OP_MOD: {
            if(aint && bint) {
                int64_t r = 0;
                switch(op) {
                case OP_ADD: r = ai + bi; break;
                case OP_SUB: r = ai - bi; break;
                case OP_MUL: r = ai * bi; break;
                case OP_DIV: if(bi == 0) return 0; r = ai / bi; break;  /* int/int 整数除法 */
                case OP_MOD: if(bi == 0) return 0; r = ai % bi; break;
                default: return 0;
                }
                *out = lumyr_make_int64(r);
            } else {
                if(op == OP_MOD) return 0;   /* 浮点不支持取模 */
                double r = 0;
                switch(op) {
                case OP_ADD: r = ad + bd; break;
                case OP_SUB: r = ad - bd; break;
                case OP_MUL: r = ad * bd; break;
                case OP_DIV: r = ad / bd; break;
                default: return 0;
                }
                *out = lumyr_make_double(r);
            }
            return 1;
        }
        case OP_GT: case OP_LT: case OP_GE: case OP_LE: {
            int r = 0;
            if(aint && bint) {
                switch(op) {
                case OP_GT: r = ai >  bi; break;
                case OP_LT: r = ai <  bi; break;
                case OP_GE: r = ai >= bi; break;
                default:    r = ai <= bi; break;
                }
            } else {
                switch(op) {
                case OP_GT: r = ad >  bd; break;
                case OP_LT: r = ad <  bd; break;
                case OP_GE: r = ad >= bd; break;
                default:    r = ad <= bd; break;
                }
            }
            *out = lumyr_make_bool(r);
            return 1;
        }
        case OP_EQ: case OP_NE: {
            int eq = (aint && bint) ? (ai == bi) : (ad == bd);
            *out = lumyr_make_bool((op == OP_EQ) ? eq : !eq);
            return 1;
        }
        default: return 0;
        }
    }
    default: return 0;
    }
}

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
            else if(args[slot].type == VAL_BOOL) iv = args[slot].v.b ? 1 : 0;
            else if(args[slot].type == VAL_CHAR) iv = (int64_t)(unsigned char)args[slot].v.c;
            else if(args[slot].type == VAL_DOUBLE) iv = (int64_t)args[slot].v.d;
            stackframe_bind_int64(new_frame, pname, iv);
            break;
        }
        case EXPR_TYPE_DOUBLE: {
            double dv = 0;
            if(args[slot].type == VAL_DOUBLE) dv = args[slot].v.d;
            else if(args[slot].type == VAL_INT64) dv = (double)args[slot].v.i64;
            else if(args[slot].type == VAL_INT) dv = (double)args[slot].v.i;
            else if(args[slot].type == VAL_BOOL) dv = args[slot].v.b ? 1.0 : 0.0;
            else if(args[slot].type == VAL_CHAR) dv = (double)(unsigned char)args[slot].v.c;
            stackframe_bind_double(new_frame, pname, dv);
            break;
        }
        case EXPR_TYPE_PTR: {
            /* PTR 类型形参（string/bigint/decimal/struct/class 等）：
             * 从 Value 中提取裸指针，绑定到 ptr_slots（LOAD_PTR_VAR 读取）。
             * 与 OPC_CALL 路径的 stackframe_bind_ptr 对齐，否则函数体内
             * LOAD_PTR_VAR 会读到 NULL 导致空值/段错误。
             * 不 strdup：形参生命周期内，原始 Value（常量池/调用方变量）仍存活 */
            void* p = NULL;
            switch(args[slot].type) {
            case VAL_PTR: case VAL_STRUCT_PTR: case VAL_CLASS_PTR:
                p = args[slot].v.struct_ptr; break;
            case VAL_BIGINT:     p = args[slot].v.bigint; break;
            case VAL_DECIMAL:    p = args[slot].v.decimal; break;
            case VAL_BITDECIMAL: p = args[slot].v.bitdecimal; break;
            case VAL_ARRAY:      p = args[slot].v.array; break;
            case VAL_TYPED_ARRAY: p = args[slot].v.typed_array; break;
            case VAL_MAP:        p = args[slot].v.map; break;
            case VAL_STRING:
                p = args[slot].str_inline ? (void*)args[slot].v.sso.data
                                          : (void*)args[slot].v.s;
                break;
            case VAL_INT64:  p = (void*)(intptr_t)args[slot].v.i64; break;
            case VAL_INT:    p = (void*)(intptr_t)args[slot].v.i; break;
            default: p = NULL; break;
            }
            stackframe_bind_ptr(new_frame, pname, p);
            break;
        }
        default:
            stackframe_bind(new_frame, pname, args[slot]);
            break;
        }
    }

    /* 默认参数填补：实参数 < 形参数时，从 AST 求值默认值并绑定。
     * 支持字面量/变量（含闭包捕获 cell）/一元/二元算术比较表达式；
     * 超出支持范围的表达式按无兜底原则报错中止，不静默绑 0。
     * 这使 arrow/lambda 函数的默认参数在动态调用时也能生效 */
    if(argc + slot_off < callee->param_cnt) {
        AstNode* def_ast = func_ast_lookup(fname);
        if(def_ast && def_ast->u.func_def.params) {
            AstNode* p = def_ast->u.func_def.params;
            for(int i = 0; i < slot_off && p; i++) p = p->u.param.next;
            for(int i = 0; i < argc && p; i++) p = p->u.param.next;
            for(int slot = argc + slot_off; slot < callee->param_cnt && p; slot++, p = p->u.param.next) {
                if(!p->u.param.default_val) continue;
                Value dv;
                if(!eval_default_expr(p->u.param.default_val, new_frame, fv.v.func.func_obj, &dv)) {
                    fprintf(stderr, "VM: 动态调用 %s 的默认参数表达式不支持运行时求值 / unsupported default argument expression in dynamic call\n",
                            fname);
                    stackframe_destroy(new_frame);
                    return 0;
                }
                int fslot = slot;
                const char* pname = (fslot < name_slots && callee->params[fslot])
                                    ? callee->params[fslot] : "_";
                CastKind pck = (fslot < callee->param_cnt && fslot < callee->sym_cnt)
                               ? (CastKind)callee->var_type_tags[fslot]
                               : CAST_NONE;
                ExprType et = castkind_to_exprtype(pck);
                switch(et) {
                case EXPR_TYPE_INT: {
                    int64_t iv = 0;
                    if(dv.type == VAL_INT64) iv = dv.v.i64;
                    else if(dv.type == VAL_INT) iv = (int64_t)dv.v.i;
                    else if(dv.type == VAL_BOOL) iv = dv.v.b ? 1 : 0;
                    else if(dv.type == VAL_DOUBLE) iv = (int64_t)dv.v.d;
                    iv = ir_int_truncate(iv, pck);   /* 按形参硬类型截断 */
                    stackframe_bind_int64(new_frame, pname, iv);
                    break;
                }
                case EXPR_TYPE_DOUBLE: {
                    double dval = 0;
                    if(dv.type == VAL_DOUBLE) dval = dv.v.d;
                    else if(dv.type == VAL_INT64) dval = (double)dv.v.i64;
                    else if(dv.type == VAL_INT) dval = (double)dv.v.i;
                    stackframe_bind_double(new_frame, pname, dval);
                    break;
                }
                case EXPR_TYPE_PTR: {
                    void* p_val = NULL;
                    if(dv.type == VAL_STRING)
                        p_val = dv.str_inline ? (void*)dv.v.sso.data : (void*)dv.v.s;
                    else if(dv.type == VAL_PTR || dv.type == VAL_STRUCT_PTR || dv.type == VAL_CLASS_PTR)
                        p_val = dv.v.struct_ptr;
                    stackframe_bind_ptr(new_frame, pname, p_val);
                    break;
                }
                default:
                    stackframe_bind(new_frame, pname, dv);
                    break;
                }
            }
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

    /* 生成器箭头函数：参数/闭包已绑定到 new_frame，创建 GeneratorObject 接管该帧，
     * 包装为 VAL_GENERATOR Value 返回，不进入 vm_exec_loop（与 OPC_CALL 生成器路径一致） */
    if(callee->is_generator) {
        GeneratorObject* gen = generator_new_with_frame(callee, new_frame);
        if(!gen) {
            fprintf(stderr, "VM: 创建箭头生成器失败 %s\n", fname);
            stackframe_destroy(new_frame);
            return 0;
        }
        out->type = VAL_GENERATOR;
        out->v.generator = gen;
        return 1;
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
    int status = vm_exec_guarded(ctx, &ret);

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
