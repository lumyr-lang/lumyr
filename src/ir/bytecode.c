#include "bytecode.h"
#include "bc_stack.h"
#include "gc_runtime.h"
#include "vm_exec.h"
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
    free((void*)fn->table_key);  /* 重载唯一键（malloc），需释放 */
    free(fn->code);
    for(int i = 0; i < fn->sym_cnt; i++) free(fn->syms[i]);
    free(fn->syms);
    symhash_reset(&fn->sym_idx);
    free(fn->const_pool);  /* 统一常量池 */
    for(int i = 0; i < fn->param_cnt + fn->has_variadic; i++) free(fn->params[i]);
    free(fn->params);
    free(fn->param_is_ref);
    free(fn->method_self_struct);
    free(fn->class_name);
    free(fn->ret_type_name);
    free(fn->var_type_tags);
    if(fn->var_struct_names) {
        for(int i = 0; i < fn->sym_cnt; i++) free(fn->var_struct_names[i]);
        free(fn->var_struct_names);
    }
    if(fn->callsites) {
        for(int i = 0; i < fn->callsite_cnt; i++) {
            free(fn->callsites[i].callee);
            free(fn->callsites[i].arg_is_ref);
        }
        free(fn->callsites);
    }
    free(fn);
}

/* ===== SymHash 符号哈希（开放寻址，load factor < 0.5） ===== */

unsigned symhash_code(const char* s) {
    /* FNV-1a 32 位 */
    unsigned h = 2166136261u;
    while(s && *s) { h ^= (unsigned char)*s++; h *= 16777619u; }
    return h;
}

int symhash_lookup(const SymHash* h, char* const* names, const char* name) {
    if(!h->tab || h->cap == 0) return -1;
    int mask = h->cap - 1;
    int p = (int)(symhash_code(name) & (unsigned)mask);
    while(h->tab[p] != -1) {
        int idx = h->tab[p];
        if(strcmp(names[idx], name) == 0) return idx;
        p = (p + 1) & mask;
    }
    return -1;
}

void symhash_insert(SymHash* h, char* const* names, int cnt, int idx) {
    int need = cnt * 2;
    if(h->cap < need) {
        /* 容量不足：扩容（2 幂）并全量 rehash [0..cnt)，含新 idx */
        int nc = h->cap > 0 ? h->cap : 16;
        while(nc < need) nc *= 2;
        int* t = (int*)malloc((size_t)nc * sizeof(int));
        if(!t) { perror("symhash_insert"); exit(EXIT_FAILURE); }
        for(int i = 0; i < nc; i++) t[i] = -1;
        for(int i = 0; i < cnt; i++) {
            int p = (int)(symhash_code(names[i]) & (unsigned)(nc - 1));
            while(t[p] != -1) p = (p + 1) & (nc - 1);
            t[p] = i;
        }
        free(h->tab);
        h->tab = t; h->cap = nc;
        return;
    }
    /* 容量足够：表已覆盖 [0..cnt-1)，增量放置新下标 idx（=cnt-1） */
    int mask = h->cap - 1;
    int p = (int)(symhash_code(names[idx]) & (unsigned)mask);
    while(h->tab[p] != -1) p = (p + 1) & mask;
    h->tab[p] = idx;
}

void symhash_reset(SymHash* h) {
    free(h->tab);
    h->tab = NULL; h->cap = 0;
}

int bf_sym(BytecodeFunc* fn, const char* name)
{
    int hit = symhash_lookup(&fn->sym_idx, fn->syms, name);
    if(hit >= 0) return hit;

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
    int newidx = fn->sym_cnt;
    fn->syms[newidx] = strdup(name);
    fn->sym_cnt++;
    /* 登记哈希（names[newidx] 已就绪） */
    symhash_insert(&fn->sym_idx, fn->syms, fn->sym_cnt, newidx);
    return newidx;
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
    /* strdup 分配的内存不在 GC 管理范围内，不会被 GC 回收，不需要 gc_pin */
    return fn->const_cnt++;
}

/* 添加 uint64 到大常量池（用于 RuntimeTypeInfo* 指针存储），返回索引
 * 注意：指针值不参与去重，每次注册都新增条目（不同实例可能恰好共用同一 RuntimeTypeInfo*） */
int bf_add_u64_const(BytecodeFunc* fn, uint64_t val) {
    if(fn->const_cnt >= fn->const_cap) {
        fn->const_cap = fn->const_cap ? fn->const_cap * 2 : 16;
        fn->const_pool = (ConstEntry*)realloc(fn->const_pool, sizeof(ConstEntry) * fn->const_cap);
        if(!fn->const_pool) { perror("bf_add_u64_const"); exit(EXIT_FAILURE); }
    }
    fn->const_pool[fn->const_cnt].type = CONST_UINT64;
    fn->const_pool[fn->const_cnt].u64 = val;
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

/* 新增一个调用点，返回其在 fn->callsites 中的下标（OPC_CALL.a） */
int bf_add_callsite(BytecodeFunc* fn, const char* callee, int argc, int keep_result, int ret_stack)
{
    if(fn->callsite_cnt >= fn->callsite_cap) {
        fn->callsite_cap = fn->callsite_cap ? fn->callsite_cap * 2 : 8;
        fn->callsites = (CallSite*)realloc(fn->callsites, sizeof(CallSite) * fn->callsite_cap);
        if(!fn->callsites) { perror("bf_add_callsite"); exit(EXIT_FAILURE); }
    }
    CallSite* cs = &fn->callsites[fn->callsite_cnt];
    cs->callee = strdup(callee);
    cs->argc = argc;
    cs->arg_is_ref = (int*)calloc(argc > 0 ? argc : 1, sizeof(int));
    if(!cs->arg_is_ref) { perror("bf_add_callsite arg_is_ref"); exit(EXIT_FAILURE); }
    cs->arg_ref_slots = (int*)malloc((argc > 0 ? argc : 1) * sizeof(int));
    if(!cs->arg_ref_slots) { perror("bf_add_callsite arg_ref_slots"); exit(EXIT_FAILURE); }
    for(int i = 0; i < (argc > 0 ? argc : 1); i++) cs->arg_ref_slots[i] = -1;
    cs->keep_result = keep_result ? 1 : 0;
    cs->ret_stack = ret_stack;
    return fn->callsite_cnt++;
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
        case OPC_PUSH_CONST_IDX: return "PUSH_CONST_IDX";
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
        case OPC_INT64_BAND: return "INT64_BAND";
        case OPC_INT64_BOR: return "INT64_BOR";
        case OPC_INT64_BXOR: return "INT64_BXOR";
        case OPC_INT64_BNOT: return "INT64_BNOT";
        case OPC_INT64_TRUNC: return "INT64_TRUNC";
        case OPC_INT64_SHL: return "INT64_SHL";
        case OPC_INT64_SHR: return "INT64_SHR";
        case OPC_INT64_POW: return "INT64_POW";
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
        case OPC_DOUBLE_POW: return "DOUBLE_POW";
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
        case OPC_TYPED_BYTES: return "TYPED_BYTES";
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
        case OPC_CALL_METHOD: return "CALL_METHOD";
        case OPC_HALT: return "HALT";
        case OPC_VADD: return "VADD";
        case OPC_VSUB: return "VSUB";
        case OPC_VMUL: return "VMUL";
        case OPC_VDIV: return "VDIV";
        case OPC_VMOD: return "VMOD";
        case OPC_VPOW: return "VPOW";
        case OPC_VNEG: return "VNEG";
        case OPC_VGT: return "VGT";
        case OPC_VLT: return "VLT";
        case OPC_VGE: return "VGE";
        case OPC_VLE: return "VLE";
        case OPC_VEQ: return "VEQ";
        case OPC_VNE: return "VNE";
        case OPC_VBAND: return "VBAND";
        case OPC_VBOR: return "VBOR";
        case OPC_VBXOR: return "VBXOR";
        case OPC_VBNOT: return "VBNOT";
        case OPC_VSHL: return "VSHL";
        case OPC_VSHR: return "VSHR";
        case OPC_JMP_IF_TRUE_V: return "JMP_IF_TRUE_V";
        case OPC_JMP_IF_FALSE_V: return "JMP_IF_FALSE_V";
        case OPC_BOX_INT64:  return "BOX_INT64";
        case OPC_BOX_DOUBLE: return "BOX_DOUBLE";
        case OPC_BOX_PTR:    return "BOX_PTR";
        case OPC_ASSERT_NONNULL: return "ASSERT_NONNULL";
        case OPC_CATCH_MATCH:return "CATCH_MATCH";
        case OPC_PUSH_NONE:  return "PUSH_NONE";
        case OPC_UNBOX_INT64:  return "UNBOX_INT64";
        case OPC_UNBOX_DOUBLE: return "UNBOX_DOUBLE";
        case OPC_UNBOX_PTR:    return "UNBOX_PTR";
        case OPC_STR_TO_INT64:  return "STR_TO_INT64";
        case OPC_STR_TO_DOUBLE: return "STR_TO_DOUBLE";
        case OPC_PUSH_INT_VAL:   return "PUSH_INT_VAL";
        case OPC_PUSH_CONST_VAL: return "PUSH_CONST_VAL";
        case OPC_CALL_METHODV:  return "CALL_METHODV";
        case OPC_GENERIC_BIND:  return "GENERIC_BIND";
        default: return "UNKNOWN";
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
            case OPC_CALL: {
                const char* cn = "?";
                int keep = 1;
                if(in.a >= 0 && in.a < fn->callsite_cnt) {
                    cn = fn->callsites[in.a].callee;
                    keep = fn->callsites[in.a].keep_result;
                }
                snprintf(txt, sizeof(txt), "CALL %s argc=%d%s",
                         cn, in.b, keep ? "" : " discard");
                break;
            }
            case OPC_CALLV:
                snprintf(txt, sizeof(txt), "CALLV argc=%d", in.b);
                break;
            case OPC_ARRAY_LIT:
                snprintf(txt, sizeof(txt), "ARRAY_LIT n=%d", in.b);
                break;
            case OPC_BUILTIN:
                snprintf(txt, sizeof(txt), "BUILTIN %s argc=%d", builtin_id_name(in.a), in.b);
                break;
            case OPC_CALL_BUILTIN_METHOD:
                snprintf(txt, sizeof(txt), "BUILTIN_METHOD %s argc=%d", builtin_id_name(in.a), in.b);
                break;
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
