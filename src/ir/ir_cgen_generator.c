/*
 * ir_cgen_generator.c - 生成器函数 C 代码生成（状态机重写）
 *
 * 将 gen func 转换为：
 *   1. 状态机结构体（保存所有局部变量、栈、状态）
 *   2. 创建函数（分配结构体并初始化参数）
 *   3. next() 函数（switch 状态 + goto 恢复执行）
 *   4. OPC_YIELD 处保存状态并返回
 *
 * 零成本抽象：编译期状态机重写，运行时无 VM 开销
 */
#include "ir_cgen_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 生成器状态计数 */
int g_gen_yield_count = 0;
int g_is_generator = 0;  /* 当前是否在生成器函数中 */

/* 当前生成器的 try-catch 块 catch 标签列表（用于 GenThrow 时直接 goto catch 块，
   避免 longjmp 到旧栈帧的未定义行为） */
static int s_gen_try_labels[256];
static int s_gen_try_label_count = 0;

void gen_set_try_labels(int* labels, int count) {
    s_gen_try_label_count = count;
    for(int i = 0; i < count && i < 256; i++) {
        s_gen_try_labels[i] = labels[i];
    }
}

/*
 * 统计函数中 OPC_YIELD 的数量
 */
int count_yields(const BytecodeFunc* fn) {
    int cnt = 0;
    for(int i = 0; i < fn->code_len; i++) {
        if(fn->code[i].op == OPC_YIELD) cnt++;
    }
    return cnt;
}

/*
 * 生成生成器状态机结构体定义
 */
void emit_gen_struct(BytecodeFunc* fn) {
    int maxd = bc_analyze_stack(fn, NULL, 0);
    int total = fn->param_cnt + (fn->has_variadic ? 1 : 0);

    fprintf(out, "/* 生成器状态机结构体：%s */\n", fn->name);
    fprintf(out, "struct lumyr_gen_%s {\n", fn->name);
    fprintf(out, "    Value (*next)(void*, Value);  /* next() 函数指针（通用接口，包装生成器可调用） */\n");
    fprintf(out, "    int __state;          /* 执行状态：0=初始, 1..n=yield点, -1=结束（与包装生成器布局一致） */\n");
    fprintf(out, "    Value __send_val;     /* send() 发送的值，receive() 返回 */\n");
    fprintf(out, "    Value __pending_exc;  /* 待抛出的异常（GenThrow() 设置，恢复时抛出） */\n");
    fprintf(out, "    int __has_pending_exc; /* 是否有待抛出的异常 */\n");
    fprintf(out, "    int __saved_depth;     /* 暂停时的 try-catch 嵌套深度 */\n");
    fprintf(out, "    jmp_buf* __saved_err_jmp; /* 暂停时的错误跳转缓冲区 */\n");
    fprintf(out, "    int __saved_fin_n;     /* 暂停时的 finally 嵌套深度 */\n");
    fprintf(out, "    int __saved_trace_n;   /* 暂停时的调用栈深度 */\n");
    fprintf(out, "    jmp_buf __saved_jb;    /* 暂停时最内层 setjmp 缓冲区内容 */\n");
    fprintf(out, "    int __saved_catch_tgt; /* 暂停时最内层 try-catch 块的 catch 标签编号 */\n");
    fprintf(out, "    int __sp;             /* 操作数栈指针 */\n");
    fprintf(out, "    Value __stk[%d];      /* 操作数栈 */\n", maxd + 2);

    /* 参数 */
    for(int i = 0; i < total; i++) {
        if(fn->params[i]) {
            fprintf(out, "    Value lmloc_%s;       /* 参数 */\n", fn->params[i]);
        }
    }

    /* 局部变量 */
    for(int i = 0; i < fn_locals.count; i++) {
        fprintf(out, "    Value lmloc_%s;       /* 局部变量 */\n", fn_locals.names[i]);
    }

    fprintf(out, "};\n\n");
}

/*
 * 生成生成器创建函数
 */
void emit_gen_create(BytecodeFunc* fn) {
    int total = fn->param_cnt + (fn->has_variadic ? 1 : 0);
    int has_caps = lambda_has_captures(fn->name);

    fprintf(out, "/* 创建生成器实例 */\n");
    fprintf(out, "static lumyr_gen_%s* lumyr_gen_%s_create(", fn->name, fn->name);
    if(has_caps) fprintf(out, "Value** __caps");
    for(int i = 0; i < total; i++) {
        if(has_caps || i) fprintf(out, ", ");
        fprintf(out, "Value lmloc_%s", fn->params[i]);
    }
    fprintf(out, ")\n{\n");
    fprintf(out, "    lumyr_gen_%s* g = (lumyr_gen_%s*)malloc(sizeof(lumyr_gen_%s));\n",
            fn->name, fn->name, fn->name);
    fprintf(out, "    memset(g, 0, sizeof(lumyr_gen_%s));\n", fn->name);
    fprintf(out, "    g->next = lumyr_gen_%s_next;\n", fn->name);
    fprintf(out, "    g->__state = 0;\n");
    fprintf(out, "    g->__sp = 0;\n");

    /* 初始化参数 */
    for(int i = 0; i < total; i++) {
        if(fn->params[i]) {
            fprintf(out, "    g->lmloc_%s = lmloc_%s;\n", fn->params[i], fn->params[i]);
        }
    }

    fprintf(out, "    return g;\n");
    fprintf(out, "}\n\n");
}

/*
 * 生成生成器 next() 函数的开头（switch 状态 + goto 标签）
 */
void emit_gen_next_header(BytecodeFunc* fn) {
    int nyields = count_yields(fn);
    int maxd = bc_analyze_stack(fn, NULL, 0);

    /* 收集本函数内 FIN_PUSH 的目标（finally/循环结束 label），与普通函数一致 */
    fin_lab_cnt = 0;
    for(int i = 0; i < fn->code_len; i++) {
        Instruction in = fn->code[i];
        if(in.op == OPC_FIN_PUSH && in.b) fin_lab_idx_of(in.b);
    }

    fprintf(out, "/* 生成器 next() 函数：恢复执行并返回 yield 值 */\n");
    fprintf(out, "static Value lumyr_gen_%s_next(void* __gptr, Value __send_val)\n{\n",
            fn->name);
    fprintf(out, "    lumyr_gen_%s* g = (lumyr_gen_%s*)__gptr;\n", fn->name, fn->name);
    fprintf(out, "    if(g->__state == -1) return val_none();\n");

    /* 设置生成器上下文：保存 send 值，标记在生成器内部 */
    fprintf(out, "    __g_gen_send_val = __send_val;\n");
    fprintf(out, "    __g_gen_in_generator = 1;\n");

    /* 从状态机恢复栈和栈指针 */
    fprintf(out, "    Value* __stk = g->__stk;\n");
    fprintf(out, "    int __sp = g->__sp;\n");

    /* finally 标签数组（与普通函数一致，OPC_FINISH 中 goto *__g_fin_labs[...] 需要） */
    if(fin_lab_cnt > 0) {
        fprintf(out, "    static void* __g_fin_labs[%d] = { ", fin_lab_cnt);
        for(int k = 0; k < fin_lab_cnt; k++)
            fprintf(out, "&&L%d%s", fin_lab_pcs[k], (k + 1 < fin_lab_cnt) ? ", " : "");
        fprintf(out, " };\n");
    }

    /* 函数边界保存 */
    fprintf(out, "    int __g_d0 = __g_depth; jmp_buf* __g_gj0 = g_err_jmp; int __g_fin0 = __g_fin_n;\n");
    fprintf(out, "    g_trace_push(\"%s\");\n", fn->name);

    /* 状态 switch：跳转到对应 yield 点 */
    if(nyields > 0) {
        fprintf(out, "    switch(g->__state) {\n");
        fprintf(out, "        case 0: goto __gen_start;\n");
        for(int i = 1; i <= nyields; i++) {
            fprintf(out, "        case %d: goto __gen_yield_%d;\n", i, i);
        }
        fprintf(out, "        default: return val_none();\n");
        fprintf(out, "    }\n");
    }

    fprintf(out, "__gen_start:\n");
    fprintf(out, "    (void)__send_val;\n");
}

/*
 * 生成生成器 next() 函数的结尾
 */
void emit_gen_next_footer(BytecodeFunc* fn) {
    fprintf(out, "__gen_end:\n");
    fprintf(out, "    __g_gen_in_generator = 0;  /* 离开生成器上下文 */\n");
    fprintf(out, "    g->__state = -1;  /* 标记为已结束 */\n");
    fprintf(out, "    return val_none();\n");
    fprintf(out, "}\n\n");
}

/*
 * 生成 OPC_YIELD 指令的 C 代码
 * 保存状态，返回栈顶值
 */
void emit_gen_yield(BytecodeFunc* fn, int yield_id) {
    fprintf(out, "    /* YIELD #%d：保存状态并返回 */\n", yield_id);
    fprintf(out, "    __g_gen_in_generator = 0;  /* 暂停时离开生成器上下文 */\n");
    fprintf(out, "    g->__state = %d;\n", yield_id);
    fprintf(out, "    g->__sp = __sp;\n");
    fprintf(out, "    g->__saved_depth = __g_depth;\n");
    fprintf(out, "    g->__saved_err_jmp = g_err_jmp;\n");
    fprintf(out, "    g->__saved_fin_n = __g_fin_n;\n");
    fprintf(out, "    g->__saved_trace_n = g_trace_n;\n");
    fprintf(out, "    if(__g_depth > 0) memcpy(g->__saved_jb, __g_jbs[__g_depth - 1], sizeof(jmp_buf));\n");
    fprintf(out, "    g->__saved_catch_tgt = (__g_depth > 0) ? __g_tgt[__g_depth - 1] : -1;\n");
    fprintf(out, "    return __stk[__sp - 1];\n");
    fprintf(out, "__gen_yield_%d:\n", yield_id);
    /* 从 yield 恢复：先恢复 try-catch 上下文（g_err_jmp 等），否则异常无法被生成器内部 catch */
    fprintf(out, "    __g_depth = g->__saved_depth;\n");
    fprintf(out, "    /* 只有当生成器内部有 try-catch 时才恢复 g_err_jmp；否则保持调用者的 g_err_jmp */\n");
    fprintf(out, "    if(g->__saved_depth > 0) g_err_jmp = g->__saved_err_jmp;\n");
    fprintf(out, "    __g_fin_n = g->__saved_fin_n;\n");
    fprintf(out, "    g_trace_n = g->__saved_trace_n;\n");
    fprintf(out, "    if(__g_depth > 0) memcpy(__g_jbs[__g_depth - 1], g->__saved_jb, sizeof(jmp_buf));\n");
    /* 从 yield 恢复：先检查是否有待抛出的异常（GenThrow），有则立即抛出 */
    fprintf(out, "    if(g->__has_pending_exc) {\n");
    fprintf(out, "        g->__has_pending_exc = 0;\n");
    fprintf(out, "        Value __v = g->__pending_exc;\n");
    fprintf(out, "        const char* __type = \"Error\"; char* __msg = NULL;\n");
    fprintf(out, "        if(__v.type == VAL_ERROR) { __type = __v.v.err.type ? __v.v.err.type : \"Error\"; __msg = strdup(__v.v.err.message ? __v.v.err.message : \"\"); }\n");
    fprintf(out, "        else if(__v.type == VAL_MAP) {\n");
    fprintf(out, "            if(lumyr_map_has(__v, lumyr_make_string(\"type\"))) { Value __tv = lumyr_map_get(__v, lumyr_make_string(\"type\")); if(__tv.type == VAL_STRING) __type = lumyr_str_cstr(&__tv); }\n");
    fprintf(out, "            if(lumyr_map_has(__v, lumyr_make_string(\"message\"))) { Value __mv = lumyr_map_get(__v, lumyr_make_string(\"message\")); if(__mv.type == VAL_STRING) __msg = strdup(lumyr_str_cstr(&__mv)); }\n");
    fprintf(out, "        }\n");
    fprintf(out, "        if(!__msg) __msg = value_to_str(__v);\n");
    fprintf(out, "        g_err_type_set(__type); g_err_msg_set(__msg); free(__msg);\n");
    fprintf(out, "        if(g->__saved_catch_tgt >= 0) {\n");
    fprintf(out, "            /* 生成器内部有 try-catch：直接跳转到 catch 块，避免 longjmp 到旧栈帧 */\n");
    fprintf(out, "            g->__state = -1;\n");
    fprintf(out, "            int __d2 = (int)(g_err_jmp - __g_jbs);\n");
    fprintf(out, "            if(__d2 < 0) __d2 = g->__saved_depth - 1;\n");
    fprintf(out, "            __sp = __g_sp0[__d2]; __g_depth = __d2; g_err_jmp = __g_prev[__d2];\n");
    fprintf(out, "            switch(g->__saved_catch_tgt) {\n");
    for(int ti = 0; ti < s_gen_try_label_count; ti++) {
        fprintf(out, "                case %d: goto L%d;\n", s_gen_try_labels[ti], s_gen_try_labels[ti]);
    }
    fprintf(out, "                default: break;\n");
    fprintf(out, "            }\n");
    fprintf(out, "        } else {\n");
    fprintf(out, "            /* 生成器内部没有 try-catch：g_err_jmp 已经是调用者的值（恢复时未覆盖），直接 longjmp */\n");
    fprintf(out, "            g->__state = -1;\n");
    fprintf(out, "            __g_depth = g->__saved_depth;\n");
    fprintf(out, "            __g_fin_n = g->__saved_fin_n; g_trace_n = g->__saved_trace_n;\n");
    fprintf(out, "            if(g_err_jmp) longjmp(*g_err_jmp, 1);\n");
    fprintf(out, "        }\n");
    fprintf(out, "        fprintf(stderr, \"Runtime Error: %%s\\n\", g_err_msg); exit(EXIT_FAILURE);\n");
    fprintf(out, "    }\n");
    /* 从 yield 恢复：如果有 send 值，压入栈 */
    fprintf(out, "    if(__send_val.type != VAL_NONE) { __stk[__sp++] = __send_val; }\n");
}


/*
 * 生成包装生成器（组合操作）的运行时支持代码
 */
void emit_gen_wrapper_support(void) {
    fprintf(out, "/* ===== 生成器组合操作运行时支持 ===== */\n");
    fprintf(out, "typedef enum { WRAP_NONE=0, WRAP_MAP=1, WRAP_FILTER=2, WRAP_SKIP=3, WRAP_TAKE=4, WRAP_ENUMERATE=5, WRAP_CHAIN=6, WRAP_ZIP=7 } WrapType;\n");
    fprintf(out, "typedef struct lumyr_gen_wrap {\n");
    fprintf(out, "    Value (*next)(void*, Value);\n");
    fprintf(out, "    int __state;\n");
    fprintf(out, "    int is_wrapped;\n");
    fprintf(out, "    int wrap_type;\n");
    fprintf(out, "    void* wrapped_gen;\n");
    fprintf(out, "    void* wrapped_gen2;\n");
    fprintf(out, "    Value wrap_fn;\n");
    fprintf(out, "    int wrap_arg;\n");
    fprintf(out, "    int wrap_index;\n");
    fprintf(out, "} lumyr_gen_wrap;\n\n");

    fprintf(out, "static Value lumyr_gen_wrap_next(void* __gptr, Value __send_val) {\n");
    fprintf(out, "    lumyr_gen_wrap* g = (lumyr_gen_wrap*)__gptr;\n");
    fprintf(out, "    if(g->__state == -1) return val_none();\n");
    fprintf(out, "    Value (*wnext)(void*, Value) = *(Value(**)(void*,Value))g->wrapped_gen;\n");
    fprintf(out, "    switch(g->wrap_type) {\n");

    fprintf(out, "        case WRAP_MAP: {\n");
    fprintf(out, "            Value v = wnext(g->wrapped_gen, val_none());\n");
    fprintf(out, "            if(v.type == VAL_NONE) { g->__state = -1; return val_none(); }\n");
    fprintf(out, "            Value args[1]; args[0] = v;\n");
    fprintf(out, "            Value (*fn)(Value*,int) = (Value(*)(Value*,int))((RuntimeFunc*)g->wrap_fn.v.func.func_obj)->entry;\n");
    fprintf(out, "            return fn(args, 1);\n");
    fprintf(out, "        }\n");

    fprintf(out, "        case WRAP_FILTER: {\n");
    fprintf(out, "            while(1) {\n");
    fprintf(out, "                Value v = wnext(g->wrapped_gen, val_none());\n");
    fprintf(out, "                if(v.type == VAL_NONE) { g->__state = -1; return val_none(); }\n");
    fprintf(out, "                Value args[1]; args[0] = v;\n");
    fprintf(out, "                Value (*fn)(Value*,int) = (Value(*)(Value*,int))((RuntimeFunc*)g->wrap_fn.v.func.func_obj)->entry;\n");
    fprintf(out, "                Value r = fn(args, 1);\n");
    fprintf(out, "                if(lumyr_to_bool(r)) return v;\n");
    fprintf(out, "            }\n");
    fprintf(out, "        }\n");

    fprintf(out, "        case WRAP_SKIP: {\n");
    fprintf(out, "            while(g->wrap_index < g->wrap_arg) {\n");
    fprintf(out, "                Value v = wnext(g->wrapped_gen, val_none());\n");
    fprintf(out, "                if(v.type == VAL_NONE) { g->__state = -1; return val_none(); }\n");
    fprintf(out, "                g->wrap_index++;\n");
    fprintf(out, "            }\n");
    fprintf(out, "            Value v = wnext(g->wrapped_gen, val_none());\n");
    fprintf(out, "            if(v.type == VAL_NONE) { g->__state = -1; return val_none(); }\n");
    fprintf(out, "            return v;\n");
    fprintf(out, "        }\n");

    fprintf(out, "        case WRAP_TAKE: {\n");
    fprintf(out, "            if(g->wrap_index >= g->wrap_arg) { g->__state = -1; return val_none(); }\n");
    fprintf(out, "            Value v = wnext(g->wrapped_gen, val_none());\n");
    fprintf(out, "            if(v.type == VAL_NONE) { g->__state = -1; return val_none(); }\n");
    fprintf(out, "            g->wrap_index++;\n");
    fprintf(out, "            return v;\n");
    fprintf(out, "        }\n");

    fprintf(out, "        case WRAP_ENUMERATE: {\n");
    fprintf(out, "            Value v = wnext(g->wrapped_gen, val_none());\n");
    fprintf(out, "            if(v.type == VAL_NONE) { g->__state = -1; return val_none(); }\n");
    fprintf(out, "            Value arr = val_array(2);\n");
    fprintf(out, "            arr.v.array->items[0] = lumyr_make_int(g->wrap_index);\n");
    fprintf(out, "            arr.v.array->items[1] = v;\n");
    fprintf(out, "            g->wrap_index++;\n");
    fprintf(out, "            return arr;\n");
    fprintf(out, "        }\n");

    fprintf(out, "        case WRAP_CHAIN: {\n");
    fprintf(out, "            if(g->wrap_index == 0) {\n");
    fprintf(out, "                Value v = wnext(g->wrapped_gen, val_none());\n");
    fprintf(out, "                if(v.type != VAL_NONE) return v;\n");
    fprintf(out, "                g->wrap_index = 1;\n");
    fprintf(out, "            }\n");
    fprintf(out, "            Value (*wnext2)(void*,Value) = *(Value(**)(void*,Value))g->wrapped_gen2;\n");
    fprintf(out, "            Value v = wnext2(g->wrapped_gen2, val_none());\n");
    fprintf(out, "            if(v.type == VAL_NONE) { g->__state = -1; return val_none(); }\n");
    fprintf(out, "            return v;\n");
    fprintf(out, "        }\n");

    fprintf(out, "        case WRAP_ZIP: {\n");
    fprintf(out, "            Value (*wnext2)(void*,Value) = *(Value(**)(void*,Value))g->wrapped_gen2;\n");
    fprintf(out, "            Value v1 = wnext(g->wrapped_gen, val_none());\n");
    fprintf(out, "            Value v2 = wnext2(g->wrapped_gen2, val_none());\n");
    fprintf(out, "            if(v1.type == VAL_NONE || v2.type == VAL_NONE) { g->__state = -1; return val_none(); }\n");
    fprintf(out, "            Value arr = val_array(2);\n");
    fprintf(out, "            arr.v.array->items[0] = v1;\n");
    fprintf(out, "            arr.v.array->items[1] = v2;\n");
    fprintf(out, "            return arr;\n");
    fprintf(out, "        }\n");

    fprintf(out, "        default: g->__state = -1; return val_none();\n");
    fprintf(out, "    }\n");
    fprintf(out, "}\n\n");

    fprintf(out, "static Value lumyr_wrap_create(int wtype, Value g1, Value g2, Value fn, int arg) {\n");
    fprintf(out, "    if(g1.type != VAL_GENERATOR) runtime_error(\"First argument of combine operation must be generator\");\n");
    fprintf(out, "    lumyr_gen_wrap* wg = (lumyr_gen_wrap*)calloc(1, sizeof(lumyr_gen_wrap));\n");
    fprintf(out, "    wg->next = lumyr_gen_wrap_next;\n");
    fprintf(out, "    wg->is_wrapped = 1;\n");
    fprintf(out, "    wg->wrap_type = wtype;\n");
    fprintf(out, "    wg->wrapped_gen = g1.v.generator;\n");
    fprintf(out, "    if(g2.type == VAL_GENERATOR) wg->wrapped_gen2 = g2.v.generator;\n");
    fprintf(out, "    wg->wrap_fn = fn;\n");
    fprintf(out, "    wg->wrap_arg = arg;\n");
    fprintf(out, "    wg->wrap_index = 0;\n");
    fprintf(out, "    Value gv; gv.type = VAL_GENERATOR; gv.v.generator = (void*)wg;\n");
    fprintf(out, "    return gv;\n");
    fprintf(out, "}\n\n");
    fprintf(out, "/* ===== 生成器组合操作支持结束 ===== */\n\n");
}
