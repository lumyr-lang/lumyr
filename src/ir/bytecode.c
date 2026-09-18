#include "bytecode.h"
#include "bc_stack.h"
#include "gc_runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

BytecodeFunc* bytecode_func_new(const char* name, int is_main)
{
    BytecodeFunc* fn = (BytecodeFunc*)calloc(1, sizeof(BytecodeFunc));
    if(!fn) { perror("bytecode_func_new"); exit(EXIT_FAILURE); }
    fn->name = name ? strdup(name) : NULL;
    fn->is_main = is_main;
    return fn;
}

void bytecode_func_free(BytecodeFunc* fn)
{
    if(!fn) return;
    free((void*)fn->name);
    free(fn->code);
    for(int i = 0; i < fn->sym_cnt; i++) free(fn->syms[i]);
    free(fn->syms);
    free(fn->const_pool);  /* 统一常量池 */
    for(int i = 0; i < fn->param_cnt + fn->has_variadic; i++) free(fn->params[i]);
    free(fn->params);
    free(fn->param_is_ref);
    free(fn->method_self_struct);
    free(fn->var_type_tags);
    if(fn->var_struct_names) {
        for(int i = 0; i < fn->sym_cnt; i++) free(fn->var_struct_names[i]);
        free(fn->var_struct_names);
    }
    free(fn);
}

int bf_sym(BytecodeFunc* fn, const char* name)
{
    for(int i = 0; i < fn->sym_cnt; i++) {
        if(strcmp(fn->syms[i], name) == 0) return i;
    }
    if(fn->sym_cnt >= fn->sym_cap) {
        int old_cap = fn->sym_cap;
        fn->sym_cap = fn->sym_cap ? fn->sym_cap * 2 : 16;
        fn->syms = (char**)realloc(fn->syms, sizeof(char*) * fn->sym_cap);
        if(!fn->syms) { perror("bf_sym"); exit(EXIT_FAILURE); }
        fn->var_type_tags = (int*)realloc(fn->var_type_tags, sizeof(int) * fn->sym_cap);
        if(!fn->var_type_tags) { perror("bf_sym var_type_tags"); exit(EXIT_FAILURE); }
        for(int i = old_cap; i < fn->sym_cap; i++) fn->var_type_tags[i] = -1;
        fn->var_struct_names = (char**)realloc(fn->var_struct_names, sizeof(char*) * fn->sym_cap);
        if(!fn->var_struct_names) { perror("bf_sym var_struct_names"); exit(EXIT_FAILURE); }
        for(int i = old_cap; i < fn->sym_cap; i++) fn->var_struct_names[i] = NULL;
    }
    fn->syms[fn->sym_cnt] = strdup(name);
    return fn->sym_cnt++;
}

static int const_equal(Value a, Value b)
{
    if(a.type != b.type) return 0;
    switch(a.type) {
        /* 有符号整数 */
        case VAL_INT:        return a.v.i == b.v.i;
        case VAL_INT8:       return a.v.i8 == b.v.i8;
        case VAL_INT16:      return a.v.i16 == b.v.i16;
        case VAL_SHORT:      return a.v.sh == b.v.sh;
        case VAL_INT32:      return a.v.i32 == b.v.i32;
        case VAL_INT64:      return a.v.i64 == b.v.i64;
        case VAL_LONG:       return a.v.l == b.v.l;
        case VAL_LONG_LONG:  return a.v.ll == b.v.ll;
        /* 无符号整数 */
        case VAL_UINT:       return a.v.ui == b.v.ui;
        case VAL_UINT8:      return a.v.u8 == b.v.u8;
        case VAL_UINT16:     return a.v.u16 == b.v.u16;
        case VAL_UINT32:     return a.v.u32 == b.v.u32;
        case VAL_UINT64:     return a.v.u64 == b.v.u64;
        case VAL_ULONG:      return a.v.ul == b.v.ul;
        case VAL_BYTE:       return a.v.by == b.v.by;
        case VAL_UCHAR:      return a.v.uc == b.v.uc;
        case VAL_USHORT:     return a.v.us == b.v.us;
        case VAL_SIZE_T:     return a.v.st == b.v.st;
        case VAL_SSIZE_T:    return a.v.sst == b.v.sst;
        /* 浮点 */
        case VAL_FLOAT:      return a.v.f == b.v.f;
        case VAL_DOUBLE:     return a.v.d == b.v.d;
        case VAL_LONG_DOUBLE: return a.v.ld == b.v.ld;
        /* 其他 */
        case VAL_BOOL:       return a.v.b == b.v.b;
        case VAL_CHAR:       return a.v.c == b.v.c;
        case VAL_STRING:     return strcmp(lumyr_str_cstr(&a), lumyr_str_cstr(&b)) == 0;
        default:             return 0;
    }
}

/* 添加 int64 到大常量池，返回索引 */
int bf_add_i64_const(BytecodeFunc* fn, int64_t val) {
    /* 去重检查 */
    for(int i = 0; i < fn->const_cnt; i++) {
        if(fn->const_pool[i].type == CONST_INT64 && fn->const_pool[i].i64 == val) return i;
    }
    if(fn->const_cnt >= fn->const_cap) {
        fn->const_cap = fn->const_cap ? fn->const_cap * 2 : 16;
        fn->const_pool = (ConstEntry*)realloc(fn->const_pool, sizeof(ConstEntry) * fn->const_cap);
        if(!fn->const_pool) { perror("bf_add_i64_const"); exit(EXIT_FAILURE); }
    }
    fn->const_pool[fn->const_cnt].type = CONST_INT64;
    fn->const_pool[fn->const_cnt].i64 = val;
    return fn->const_cnt++;
}

/* 添加 double 到大常量池，返回索引 */
int bf_add_double_const(BytecodeFunc* fn, double val) {
    /* 去重检查 */
    for(int i = 0; i < fn->const_cnt; i++) {
        if(fn->const_pool[i].type == CONST_DOUBLE && fn->const_pool[i].d == val) return i;
    }
    if(fn->const_cnt >= fn->const_cap) {
        fn->const_cap = fn->const_cap ? fn->const_cap * 2 : 16;
        fn->const_pool = (ConstEntry*)realloc(fn->const_pool, sizeof(ConstEntry) * fn->const_cap);
        if(!fn->const_pool) { perror("bf_add_double_const"); exit(EXIT_FAILURE); }
    }
    fn->const_pool[fn->const_cnt].type = CONST_DOUBLE;
    fn->const_pool[fn->const_cnt].d = val;
    return fn->const_cnt++;
}

/* 添加字符串到大常量池，返回索引 */
int bf_add_str_const(BytecodeFunc* fn, const char* s) {
    /* 去重检查 */
    for(int i = 0; i < fn->const_cnt; i++) {
        if(fn->const_pool[i].type == CONST_STRING && strcmp(fn->const_pool[i].s, s) == 0) return i;
    }
    if(fn->const_cnt >= fn->const_cap) {
        fn->const_cap = fn->const_cap ? fn->const_cap * 2 : 16;
        fn->const_pool = (ConstEntry*)realloc(fn->const_pool, sizeof(ConstEntry) * fn->const_cap);
        if(!fn->const_pool) { perror("bf_add_str_const"); exit(EXIT_FAILURE); }
    }
    fn->const_pool[fn->const_cnt].type = CONST_STRING;
    fn->const_pool[fn->const_cnt].s = strdup(s);  /* 拷贝字符串，防止 AST 释放后指针失效 */
    /* 钉住字符串 */
    gc_pin((void*)fn->const_pool[fn->const_cnt].s);
    return fn->const_cnt++;
}

void bf_emit(BytecodeFunc* fn, OpCode op, int a, int b)
{
    if(fn->code_len >= fn->code_cap) {
        fn->code_cap = fn->code_cap ? fn->code_cap * 2 : 32;
        fn->code = (Instruction*)realloc(fn->code, sizeof(Instruction) * fn->code_cap);
        if(!fn->code) { perror("bf_emit"); exit(EXIT_FAILURE); }
    }
    fn->code[fn->code_len].op = op;
    fn->code[fn->code_len].a = a;
    fn->code[fn->code_len].b = b;
    fn->code_len++;
}

int bf_emit_here(BytecodeFunc* fn, OpCode op, int a, int b)
{
    int pos = fn->code_len;
    bf_emit(fn, op, a, b);
    return pos;
}

void bf_patch(BytecodeFunc* fn, int pos, int target)
{
    if(pos < 0 || pos >= fn->code_len) {
        fprintf(stderr, "bf_patch: 越界 pos=%d len=%d\n", pos, fn->code_len);
        exit(EXIT_FAILURE);
    }
    fn->code[pos].a = target;
}

void bf_patch_b(BytecodeFunc* fn, int pos, int target)
{
    if(pos < 0 || pos >= fn->code_len) {
        fprintf(stderr, "bf_patch_b: 越界 pos=%d len=%d\n", pos, fn->code_len);
        exit(EXIT_FAILURE);
    }
    fn->code[pos].b = target;
}

int bc_analyze_stack(BytecodeFunc* fn, int* depth_out, int depth_cap)
{
    if(!fn || fn->code_len == 0) return 0;
    int n = fn->code_len;
    StackDelta* d = (StackDelta*)malloc(sizeof(StackDelta) * n);
    if(!d) { perror("bc_analyze_stack"); exit(EXIT_FAILURE); }
    for(int i = 0; i < n; i++) {
        memset(&d[i], 0, sizeof(StackDelta));
        d[i].value = -1;
    }
    d[0].value = 0;

    /* 深度合并：4 核心栈设计 */
    #define STACK_MERGE(target_idx, src) do { \
        StackDelta* _t = &d[target_idx]; \
        const StackDelta* _s = &(src); \
        if(_t->value < _s->value || _t->int64 < _s->int64 || \
           _t->double_stk < _s->double_stk || _t->ptr < _s->ptr) { \
            *_t = *_s; changed = 1; \
        } \
    } while(0)

    /* 检查所有栈是否下溢 */
    #define STACK_CHECK_UNDERFLOW(nd, pc) do { \
        if((nd).value < 0 || (nd).int64 < 0 || \
           (nd).double_stk < 0 || (nd).ptr < 0) { \
            fprintf(stderr, "IR 栈深分析: 指令 %d 栈下溢（Value=%d int64=%d double=%d ptr=%d）——IR 生成错误\n", \
                    (pc), (nd).value, (nd).int64, (nd).double_stk, (nd).ptr); \
            free(d); return -1; \
        } \
    } while(0)

    int changed = 1;
    while(changed) {
        changed = 0;
        for(int i = 0; i < n; i++) {
            if(d[i].value < 0) continue;
            Instruction in = fn->code[i];
            StackDelta delta = op_stack_delta(fn, in);
            StackDelta nd;
            nd.value       = d[i].value + delta.value;
            nd.int64       = d[i].int64 + delta.int64;
            nd.double_stk  = d[i].double_stk + delta.double_stk;
            nd.ptr         = d[i].ptr + delta.ptr;

            STACK_CHECK_UNDERFLOW(nd, i);

            if(in.op == OPC_JMP || in.op == OPC_JMP_IF_FALSE || in.op == OPC_JMP_IF_TRUE || in.op == OPC_JMP_IF_NULL) {
                if(in.a >= 0 && in.a < n) STACK_MERGE(in.a, nd);
            }
            if(in.op == OPC_TRY) {
                if(in.a > 0 && in.a < n) STACK_MERGE(in.a, nd);
                if(in.b > 0 && in.b < n) STACK_MERGE(in.b, nd);
            }
            if(in.op == OPC_FIN_PUSH || in.op == OPC_PEND_RETURN) {
                if(in.b > 0 && in.b < n) STACK_MERGE(in.b, nd);
            }
            if(in.op != OPC_RETURN && in.op != OPC_RETURN_NIL && in.op != OPC_HALT &&
               in.op != OPC_JMP) {
                if(i + 1 < n) STACK_MERGE(i + 1, nd);
            }
        }
    }

    #undef STACK_MERGE
    #undef STACK_CHECK_UNDERFLOW

    int maxd = 0;
    for(int i = 0; i < n; i++) {
        int depth = (d[i].value < 0) ? 0 : d[i].value;
        if(depth_out && i < depth_cap) depth_out[i] = depth;
        int peak = depth + op_stack_push(fn->code[i].op);
        if(peak > maxd) maxd = peak;
    }
    free(d);
    return maxd;
}

const char* opc_name(OpCode op)
{
    switch(op) {
        case OPC_NOP: return "NOP";
        case OPC_POP: return "POP";
        case OPC_DUP: return "DUP";
        case OPC_TO_BOOL: return "TO_BOOL";
        case OPC_LOAD_CONST: return "LOAD_CONST";
        case OPC_PUSH_INT64_CONST: return "PUSH_INT64_CONST";
        case OPC_PUSH_DOUBLE_CONST: return "PUSH_DOUBLE_CONST";
        case OPC_PUSH_PTR_CONST: return "PUSH_PTR_CONST";
        case OPC_LOAD_VAR: return "LOAD_VAR";
        case OPC_STORE_VAR: return "STORE_VAR";
        case OPC_LOAD_INT64_VAR: return "LOAD_INT64_VAR";
        case OPC_STORE_INT64_VAR: return "STORE_INT64_VAR";
        case OPC_LOAD_DOUBLE_VAR: return "LOAD_DOUBLE_VAR";
        case OPC_STORE_DOUBLE_VAR: return "STORE_DOUBLE_VAR";
        case OPC_LOAD_PTR_VAR: return "LOAD_PTR_VAR";
        case OPC_STORE_PTR_VAR: return "STORE_PTR_VAR";
        case OPC_LOAD_VAR_REF: return "LOAD_VAR_REF";
        case OPC_ADD: return "ADD";
        case OPC_SUB: return "SUB";
        case OPC_MUL: return "MUL";
        case OPC_DIV: return "DIV";
        case OPC_MOD: return "MOD";
        case OPC_GT: return "GT";
        case OPC_LT: return "LT";
        case OPC_GE: return "GE";
        case OPC_LE: return "LE";
        case OPC_EQ: return "EQ";
        case OPC_NE: return "NE";
        case OPC_IMPLEMENTS: return "IMPLEMENTS";
        case OPC_NEG: return "NEG";
        case OPC_POS: return "POS";
        case OPC_LOGIC_NOT: return "LOGIC_NOT";
        case OPC_PRE_INC: return "PRE_INC";
        case OPC_POST_INC: return "POST_INC";
        case OPC_PRE_DEC: return "PRE_DEC";
        case OPC_POST_DEC: return "POST_DEC";
        case OPC_INT64_ADD: return "INT64_ADD";
        case OPC_INT64_SUB: return "INT64_SUB";
        case OPC_INT64_MUL: return "INT64_MUL";
        case OPC_INT64_DIV: return "INT64_DIV";
        case OPC_INT64_MOD: return "INT64_MOD";
        case OPC_INT64_GT: return "INT64_GT";
        case OPC_INT64_LT: return "INT64_LT";
        case OPC_INT64_GE: return "INT64_GE";
        case OPC_INT64_LE: return "INT64_LE";
        case OPC_INT64_EQ: return "INT64_EQ";
        case OPC_INT64_NE: return "INT64_NE";
        case OPC_INT64_TO_VALUE: return "INT64_TO_VALUE";
        case OPC_DOUBLE_ADD: return "DOUBLE_ADD";
        case OPC_DOUBLE_SUB: return "DOUBLE_SUB";
        case OPC_DOUBLE_MUL: return "DOUBLE_MUL";
        case OPC_DOUBLE_DIV: return "DOUBLE_DIV";
        case OPC_DOUBLE_GT: return "DOUBLE_GT";
        case OPC_DOUBLE_LT: return "DOUBLE_LT";
        case OPC_DOUBLE_GE: return "DOUBLE_GE";
        case OPC_DOUBLE_LE: return "DOUBLE_LE";
        case OPC_DOUBLE_EQ: return "DOUBLE_EQ";
        case OPC_DOUBLE_NE: return "DOUBLE_NE";
        case OPC_DOUBLE_TO_VALUE: return "DOUBLE_TO_VALUE";
        case OPC_INT64_TO_DOUBLE: return "INT64_TO_DOUBLE";
        case OPC_DOUBLE_TO_INT64: return "DOUBLE_TO_INT64";
        case OPC_INT64_TO_PTR: return "INT64_TO_PTR";
        case OPC_PTR_TO_INT64: return "PTR_TO_INT64";
        case OPC_CAST_INT: return "CAST_INT";
        case OPC_CAST_DOUBLE: return "CAST_DOUBLE";
        case OPC_CAST_CHAR: return "CAST_CHAR";
        case OPC_CAST_BOOL: return "CAST_BOOL";
        case OPC_CAST_STRING: return "CAST_STRING";
        case OPC_CAST_ASCII: return "CAST_ASCII";
        case OPC_CAST_BYTE: return "CAST_BYTE";
        case OPC_CAST_INT8: return "CAST_INT8";
        case OPC_CAST_INT16: return "CAST_INT16";
        case OPC_CAST_INT32: return "CAST_INT32";
        case OPC_CAST_INT64: return "CAST_INT64";
        case OPC_CAST_UINT8: return "CAST_UINT8";
        case OPC_CAST_UINT16: return "CAST_UINT16";
        case OPC_CAST_UINT32: return "CAST_UINT32";
        case OPC_CAST_UINT64: return "CAST_UINT64";
        case OPC_CAST_LONG: return "CAST_LONG";
        case OPC_CAST_LONGLONG: return "CAST_LONGLONG";
        case OPC_CAST_FLOAT: return "CAST_FLOAT";
        case OPC_ARRAY_LIT: return "ARRAY_LIT";
        case OPC_INT64_ARRAY_LIT: return "INT64_ARRAY_LIT";
        case OPC_DOUBLE_ARRAY_LIT: return "DOUBLE_ARRAY_LIT";
        case OPC_PTR_ARRAY_LIT: return "PTR_ARRAY_LIT";
        case OPC_MAP_LIT: return "MAP_LIT";
        case OPC_INDEX_GET: return "INDEX_GET";
        case OPC_INDEX_SET: return "INDEX_SET";
        case OPC_INT64_INDEX_SET: return "INT64_INDEX_SET";
        case OPC_DOUBLE_INDEX_SET: return "DOUBLE_INDEX_SET";
        case OPC_LOAD_FIELD: return "LOAD_FIELD";
        case OPC_STORE_FIELD: return "STORE_FIELD";
        case OPC_STORE_NESTED_FIELD: return "STORE_NESTED_FIELD";
        case OPC_LOAD_STRUCT_PTR: return "LOAD_STRUCT_PTR";
        case OPC_BUILTIN: return "BUILTIN";
        case OPC_PRINT: return "PRINT";
        case OPC_PRINT_INT64: return "PRINT_INT64";
        case OPC_PRINT_DOUBLE: return "PRINT_DOUBLE";
        case OPC_PRINT_PTR: return "PRINT_PTR";
        case OPC_TRY: return "TRY";
        case OPC_ENDTRY: return "ENDTRY";
        case OPC_GET_ERR: return "GET_ERR";
        case OPC_THROW: return "THROW";
        case OPC_FIN_PUSH: return "FIN_PUSH";
        case OPC_FINISH: return "FINISH";
        case OPC_PEND_RETURN: return "PEND_RETURN";
        case OPC_JMP: return "JMP";
        case OPC_JMP_IF_FALSE: return "JMP_IF_FALSE";
        case OPC_JMP_IF_TRUE: return "JMP_IF_TRUE";
        case OPC_JMP_IF_NULL: return "JMP_IF_NULL";
        case OPC_GETFUNC: return "GETFUNC";
        case OPC_CALL: return "CALL";
        case OPC_CALLV: return "CALLV";
        case OPC_MKCLOSURE: return "MKCLOSURE";
        case OPC_RETURN: return "RETURN";
        case OPC_RETURN_NIL: return "RETURN_NIL";
        case OPC_YIELD: return "YIELD";
        case OPC_CLASS_NEW: return "CLASS_NEW";
        case OPC_HALT: return "HALT";
        default: return "UNKNOWN";
    }
}

static void const_to_text(Value v, char* buf, int cap)
{
    switch(v.type) {
        /* 有符号整数 */
        case VAL_INT:        snprintf(buf, cap, "%lld", v.v.i); break;
        case VAL_INT8:       snprintf(buf, cap, "%d", (int)v.v.i8); break;
        case VAL_INT16:      snprintf(buf, cap, "%d", (int)v.v.i16); break;
        case VAL_SHORT:      snprintf(buf, cap, "%d", (int)v.v.sh); break;
        case VAL_INT32:      snprintf(buf, cap, "%d", (int)v.v.i32); break;
        case VAL_INT64:      snprintf(buf, cap, "%lld", v.v.i64); break;
        case VAL_LONG:       snprintf(buf, cap, "%ld", v.v.l); break;
        case VAL_LONG_LONG:  snprintf(buf, cap, "%lld", v.v.ll); break;
        /* 无符号整数 */
        case VAL_UINT:       snprintf(buf, cap, "%u", (unsigned int)v.v.ui); break;
        case VAL_UINT8:      snprintf(buf, cap, "%u", (unsigned int)v.v.u8); break;
        case VAL_UINT16:     snprintf(buf, cap, "%u", (unsigned int)v.v.u16); break;
        case VAL_UINT32:     snprintf(buf, cap, "%u", v.v.u32); break;
        case VAL_UINT64:     snprintf(buf, cap, "%llu", v.v.u64); break;
        case VAL_ULONG:      snprintf(buf, cap, "%lu", v.v.ul); break;
        case VAL_BYTE:       snprintf(buf, cap, "%u", (unsigned int)v.v.by); break;
        case VAL_UCHAR:      snprintf(buf, cap, "%u", (unsigned int)v.v.uc); break;
        case VAL_USHORT:     snprintf(buf, cap, "%u", (unsigned int)v.v.us); break;
        case VAL_SIZE_T:     snprintf(buf, cap, "%zu", v.v.st); break;
        case VAL_SSIZE_T:    snprintf(buf, cap, "%zd", v.v.sst); break;
        /* 浮点 */
        case VAL_FLOAT:      snprintf(buf, cap, "%g", (double)v.v.f); break;
        case VAL_DOUBLE:     snprintf(buf, cap, "%.17g", v.v.d); break;
        case VAL_LONG_DOUBLE: snprintf(buf, cap, "%Lg", v.v.ld); break;
        /* 其他 */
        case VAL_BOOL:       snprintf(buf, cap, "%s", v.v.b ? "true" : "false"); break;
        case VAL_CHAR:       snprintf(buf, cap, "'%c'", v.v.c); break;
        case VAL_STRING:     snprintf(buf, cap, "\"%s\"", lumyr_str_cstr(&v)); break;
        default:             snprintf(buf, cap, "nil"); break;
    }
}

void bc_disasm(FILE* out, BytecodeFunc* fn)
{
    int* depth = (int*)malloc(sizeof(int) * (fn->code_len ? fn->code_len : 1));
    int maxd = bc_analyze_stack(fn, depth, fn->code_len);

    fprintf(out, "%s (code_len=%d, max_stack=%d):\n",
            fn->name ? fn->name : "<main>", fn->code_len, maxd);
    if(fn->param_cnt > 0 || fn->has_variadic) {
        fprintf(out, "  ; params:");
        for(int i = 0; i < fn->param_cnt + fn->has_variadic; i++) {
            fprintf(out, " %s", fn->params[i]);
        }
        fprintf(out, "%s\n", fn->has_variadic ? " ..." : "");
    }

    for(int i = 0; i < fn->code_len; i++) {
        Instruction in = fn->code[i];
        char txt[256];
        switch(in.op) {
            case OPC_LOAD_CONST: {
                char cb[128];
                /* 从统一常量池加载 */
                if(in.a < fn->const_cnt) {
                    ConstEntry* e = &fn->const_pool[in.a];
                    switch(e->type) {
                        case CONST_INT64: snprintf(cb, sizeof(cb), "%lld", (long long)e->i64); break;
                        case CONST_UINT64: snprintf(cb, sizeof(cb), "%llu", (unsigned long long)e->u64); break;
                        case CONST_DOUBLE: snprintf(cb, sizeof(cb), "%g", e->d); break;
                        case CONST_STRING: snprintf(cb, sizeof(cb), "\"%s\"", e->s); break;
                        default: snprintf(cb, sizeof(cb), "?"); break;
                    }
                } else {
                    snprintf(cb, sizeof(cb), "?");
                }
                snprintf(txt, sizeof(txt), "%s %d ; %s", opc_name(in.op), in.a, cb);
                break;
            }
            case OPC_LOAD_VAR:
            case OPC_STORE_VAR:
            case OPC_PRE_INC: case OPC_POST_INC: case OPC_PRE_DEC: case OPC_POST_DEC:
                snprintf(txt, sizeof(txt), "%s %s", opc_name(in.op),
                         (in.a >= 0 && in.a < fn->sym_cnt) ? fn->syms[in.a] : "?");
                break;
            case OPC_JMP:
            case OPC_JMP_IF_FALSE:
            case OPC_JMP_IF_TRUE:
            case OPC_JMP_IF_NULL:
            case OPC_TRY:
            case OPC_ENDTRY:
                snprintf(txt, sizeof(txt), "%s L%d", opc_name(in.op), in.a);
                break;
            case OPC_FIN_PUSH:
                snprintf(txt, sizeof(txt), "FIN_PUSH act=%d tgt=L%d", in.a, in.b);
                break;
            case OPC_CALL:
                snprintf(txt, sizeof(txt), "CALL %s argc=%d",
                         (in.a >= 0 && in.a < fn->sym_cnt) ? fn->syms[in.a] : "?", in.b);
                break;
            case OPC_CALLV:
                snprintf(txt, sizeof(txt), "CALLV argc=%d", in.b);
                break;
            case OPC_ARRAY_LIT:
                snprintf(txt, sizeof(txt), "ARRAY_LIT n=%d", in.b);
                break;
            case OPC_BUILTIN: {
                static const char* bname[] = {"len", "type", "input", "range", "substr", "toupper", "tolower", "split", "del", "insert", "floor", "ceil", "abs", "sqrt", "max", "min", "join", "contains", "repeat", "replace", "sum", "avg", "format", "sort", "reverse", "map", "filter", "reduce", "strip", "startswith", "endswith"};
                const char* bn = (in.a >= 0 && in.a < 31) ? bname[in.a] : "?";
                snprintf(txt, sizeof(txt), "BUILTIN %s argc=%d", bn, in.b);
                break;
            }
            default:
                snprintf(txt, sizeof(txt), "%s", opc_name(in.op));
                break;
        }
        int d = (i < fn->code_len) ? depth[i] : 0;
        fprintf(out, "  %4d: [%2d] %-20s\n", i, d, txt);
    }
    free(depth);
    fputc('\n', out);
}
