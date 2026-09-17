#include "bytecode.h"
#include "bytecode_stack.h"
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
    for(int i = 0; i < fn->const_cnt; i++) val_destroy(&fn->consts[i]);
    free(fn->consts);
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

int bf_const(BytecodeFunc* fn, Value v)
{
    for(int i = 0; i < fn->const_cnt; i++) {
        if(const_equal(fn->consts[i], v)) return i;
    }
    if(fn->const_cnt >= fn->const_cap) {
        fn->const_cap = fn->const_cap ? fn->const_cap * 2 : 16;
        fn->consts = (Value*)realloc(fn->consts, sizeof(Value) * fn->const_cap);
        if(!fn->consts) { perror("bf_const"); exit(EXIT_FAILURE); }
    }
    fn->consts[fn->const_cnt] = val_clone(&v);   // 常量池浅拷贝持有（GC 引用语义）
    /* 钉住字符串常量：常量表不是 GC 根，需防止被 sweep；内联字符串无需钉住 */
    if (v.type == VAL_STRING && !v.str_inline && v.v.s) gc_pin(v.v.s);
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
    /* value=-1 表示不可达；其他栈深度初始化为 0 */
    for(int i = 0; i < n; i++) {
        memset(&d[i], 0, sizeof(StackDelta));
        d[i].value = -1;
    }
    d[0].value = 0;

    /* 深度合并：如果目标点任一栈深度比当前记录大，则更新 */
    #define STACK_MERGE(target_idx, src) do { \
        StackDelta* _t = &d[target_idx]; \
        const StackDelta* _s = &(src); \
        if(_t->value < _s->value || _t->int_stack < _s->int_stack || \
           _t->double_stack < _s->double_stack || _t->float_stack < _s->float_stack || \
           _t->uint_stack < _s->uint_stack || _t->bool_stack < _s->bool_stack || \
           _t->char_stack < _s->char_stack || _t->byte_stack < _s->byte_stack || \
           _t->short_stack < _s->short_stack || _t->int8_stack < _s->int8_stack || \
           _t->int16_stack < _s->int16_stack || _t->int32_stack < _s->int32_stack || \
           _t->int64_stack < _s->int64_stack || _t->uint8_stack < _s->uint8_stack || \
           _t->uint16_stack < _s->uint16_stack || _t->uint32_stack < _s->uint32_stack || \
           _t->uint64_stack < _s->uint64_stack || _t->long_stack < _s->long_stack || \
           _t->ulong_stack < _s->ulong_stack || _t->size_t_stack < _s->size_t_stack || \
           _t->ssize_t_stack < _s->ssize_t_stack || \
           _t->long_double_stack < _s->long_double_stack || \
           _t->long_long_stack < _s->long_long_stack) { \
            *_t = *_s; changed = 1; \
        } \
    } while(0)

    /* 检查所有栈是否下溢 */
    #define STACK_CHECK_UNDERFLOW(nd, pc) do { \
        if((nd).value < 0 || (nd).int_stack < 0 || (nd).double_stack < 0 || \
           (nd).float_stack < 0 || (nd).uint_stack < 0 || (nd).bool_stack < 0 || \
           (nd).char_stack < 0 || (nd).byte_stack < 0 || (nd).short_stack < 0 || \
           (nd).int8_stack < 0 || (nd).int16_stack < 0 || (nd).int32_stack < 0 || \
           (nd).int64_stack < 0 || (nd).uint8_stack < 0 || (nd).uint16_stack < 0 || \
           (nd).uint32_stack < 0 || (nd).uint64_stack < 0 || (nd).long_stack < 0 || \
           (nd).ulong_stack < 0 || (nd).size_t_stack < 0 || (nd).ssize_t_stack < 0 || \
           (nd).long_double_stack < 0 || (nd).long_long_stack < 0) { \
            fprintf(stderr, "IR 栈深分析: 指令 %d 栈下溢（Value=%d int=%d double=%d float=%d uint=%d）——IR 生成错误\n", \
                    (pc), (nd).value, (nd).int_stack, (nd).double_stack, (nd).float_stack, (nd).uint_stack); \
            free(d); return -1; \
        } \
    } while(0)

    // 数据流迭代：顺序后继 + 跳转后继，直到收敛
    int changed = 1;
    while(changed) {
        changed = 0;
        for(int i = 0; i < n; i++) {
            if(d[i].value < 0) continue;
            Instruction in = fn->code[i];
            StackDelta delta = op_stack_delta(fn, in);
            StackDelta nd;
            nd.value          = d[i].value + delta.value;
            nd.int_stack      = d[i].int_stack + delta.int_stack;
            nd.double_stack   = d[i].double_stack + delta.double_stack;
            nd.float_stack    = d[i].float_stack + delta.float_stack;
            nd.uint_stack     = d[i].uint_stack + delta.uint_stack;
            nd.bool_stack     = d[i].bool_stack + delta.bool_stack;
            nd.char_stack     = d[i].char_stack + delta.char_stack;
            nd.byte_stack     = d[i].byte_stack + delta.byte_stack;
            nd.short_stack    = d[i].short_stack + delta.short_stack;
            nd.int8_stack     = d[i].int8_stack + delta.int8_stack;
            nd.int16_stack    = d[i].int16_stack + delta.int16_stack;
            nd.int32_stack    = d[i].int32_stack + delta.int32_stack;
            nd.int64_stack    = d[i].int64_stack + delta.int64_stack;
            nd.uint8_stack    = d[i].uint8_stack + delta.uint8_stack;
            nd.uint16_stack   = d[i].uint16_stack + delta.uint16_stack;
            nd.uint32_stack   = d[i].uint32_stack + delta.uint32_stack;
            nd.uint64_stack   = d[i].uint64_stack + delta.uint64_stack;
            nd.long_stack     = d[i].long_stack + delta.long_stack;
            nd.ulong_stack    = d[i].ulong_stack + delta.ulong_stack;
            nd.size_t_stack   = d[i].size_t_stack + delta.size_t_stack;
            nd.ssize_t_stack  = d[i].ssize_t_stack + delta.ssize_t_stack;
            nd.long_double_stack = d[i].long_double_stack + delta.long_double_stack;
            nd.long_long_stack   = d[i].long_long_stack + delta.long_long_stack;

            STACK_CHECK_UNDERFLOW(nd, i);

            if(in.op == OPC_JMP || in.op == OPC_JMP_IF_FALSE || in.op == OPC_JMP_IF_TRUE || in.op == OPC_JMP_IF_NULL) {
                if(in.a >= 0 && in.a < n) STACK_MERGE(in.a, nd);
            }
            /* try/catch/finally 的非跳转式目标 */
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
        int depth = (d[i].value < 0) ? 0 : d[i].value;   // 不可达指令深度记 0
        if(depth_out && i < depth_cap) depth_out[i] = depth;
        int peak = depth + op_stack_push(fn->code[i].op);
        if(peak > maxd) maxd = peak;
    }
    free(d);
    return maxd;
}

// ---------------- 反汇编（-S） ----------------

static const char* opc_name(OpCode op)
{
    switch(op) {
        case OPC_NOP: return "NOP";
        case OPC_LOAD_CONST: return "LOAD_CONST";
        case OPC_GETFUNC: return "GETFUNC";
        case OPC_MKCLOSURE: return "MKCLOSURE";
        case OPC_LOAD_VAR: return "LOAD_VAR";
        case OPC_STORE_VAR: return "STORE_VAR";
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
        case OPC_MAP_LIT: return "MAP_LIT";
        case OPC_CLASS_NEW: return "CLASS_NEW";
        case OPC_INDEX_GET: return "INDEX_GET";
        case OPC_INDEX_SET: return "INDEX_SET";
        case OPC_BUILTIN: return "BUILTIN";
        case OPC_PRINT: return "PRINT";
        case OPC_TO_BOOL: return "TO_BOOL";
        case OPC_DUP: return "DUP";
        case OPC_POP: return "POP";
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
        case OPC_CALL: return "CALL";
        case OPC_CALLV: return "CALLV";
        case OPC_RETURN: return "RETURN";
        case OPC_RETURN_NIL: return "RETURN_NIL";
        case OPC_HALT: return "HALT";
        /* int专用指令 */
        case OPC_LOAD_INT_VAR: return "LOAD_INT_VAR";
        case OPC_STORE_INT_VAR: return "STORE_INT_VAR";
        case OPC_PUSH_INT_CONST: return "PUSH_INT_CONST";
        case OPC_INT_ADD: return "INT_ADD";
        case OPC_INT_SUB: return "INT_SUB";
        case OPC_INT_MUL: return "INT_MUL";
        case OPC_INT_DIV: return "INT_DIV";
        case OPC_INT_MOD: return "INT_MOD";
        case OPC_INT_TO_VALUE: return "INT_TO_VALUE";
        case OPC_SHORT_TO_VALUE: return "SHORT_TO_VALUE";
        case OPC_INT_GT: return "INT_GT";
        case OPC_INT_LT: return "INT_LT";
        case OPC_INT_GE: return "INT_GE";
        case OPC_INT_LE: return "INT_LE";
        case OPC_INT_EQ: return "INT_EQ";
        case OPC_INT_NE: return "INT_NE";
        case OPC_INT_ARRAY_SET: return "INT_ARRAY_SET";
        case OPC_INT_ARRAY_LIT: return "INT_ARRAY_LIT";
        case OPC_INT_ARRAY_GET: return "INT_ARRAY_GET";
        case OPC_PRINT_INT: return "PRINT_INT";
        /* double专用指令 */
        case OPC_PUSH_DOUBLE_CONST: return "PUSH_DOUBLE_CONST";
        case OPC_DOUBLE_ADD: return "DOUBLE_ADD";
        case OPC_DOUBLE_SUB: return "DOUBLE_SUB";
        case OPC_DOUBLE_MUL: return "DOUBLE_MUL";
        case OPC_DOUBLE_DIV: return "DOUBLE_DIV";
        case OPC_DOUBLE_TO_VALUE: return "DOUBLE_TO_VALUE";
        case OPC_DOUBLE_GT: return "DOUBLE_GT";
        case OPC_DOUBLE_LT: return "DOUBLE_LT";
        case OPC_DOUBLE_GE: return "DOUBLE_GE";
        case OPC_DOUBLE_LE: return "DOUBLE_LE";
        case OPC_DOUBLE_EQ: return "DOUBLE_EQ";
        case OPC_DOUBLE_NE: return "DOUBLE_NE";
        case OPC_DOUBLE_ARRAY_SET: return "DOUBLE_ARRAY_SET";
        /* uint专用指令 */
        case OPC_LOAD_UINT_VAR: return "LOAD_UINT_VAR";
        case OPC_STORE_UINT_VAR: return "STORE_UINT_VAR";
        case OPC_PUSH_UINT_CONST: return "PUSH_UINT_CONST";
        case OPC_UINT_ADD: return "UINT_ADD";
        case OPC_UINT_SUB: return "UINT_SUB";
        case OPC_UINT_MUL: return "UINT_MUL";
        case OPC_UINT_DIV: return "UINT_DIV";
        case OPC_UINT_MOD: return "UINT_MOD";
        case OPC_UINT_TO_VALUE: return "UINT_TO_VALUE";
        case OPC_UINT_GT: return "UINT_GT";
        case OPC_UINT_LT: return "UINT_LT";
        case OPC_UINT_GE: return "UINT_GE";
        case OPC_UINT_LE: return "UINT_LE";
        case OPC_UINT_EQ: return "UINT_EQ";
        case OPC_UINT_NE: return "UINT_NE";
        case OPC_UINT_ARRAY_SET: return "UINT_ARRAY_SET";
        case OPC_INT_TO_UINT: return "INT_TO_UINT";
        case OPC_UINT_TO_INT: return "UINT_TO_INT";
        case OPC_INT_TO_FLOAT: return "INT_TO_FLOAT";
        case OPC_INT_TO_DOUBLE: return "INT_TO_DOUBLE";
        case OPC_UINT_TO_FLOAT: return "UINT_TO_FLOAT";
        case OPC_UINT_TO_DOUBLE: return "UINT_TO_DOUBLE";
        case OPC_FLOAT_TO_DOUBLE: return "FLOAT_TO_DOUBLE";
        case OPC_INT_TO_LONG_LONG: return "INT_TO_LONG_LONG";
        case OPC_UINT_TO_LONG_LONG: return "UINT_TO_LONG_LONG";
        case OPC_FLOAT_TO_LONG_LONG: return "FLOAT_TO_LONG_LONG";
        case OPC_DOUBLE_TO_LONG_LONG: return "DOUBLE_TO_LONG_LONG";
        case OPC_LONG_LONG_TO_FLOAT: return "LONG_LONG_TO_FLOAT";
        case OPC_LONG_LONG_TO_DOUBLE: return "LONG_LONG_TO_DOUBLE";
        /* 转换到 long double 的专用指令名称 */
        case OPC_INT_TO_LONG_DOUBLE: return "INT_TO_LONG_DOUBLE";
        case OPC_UINT_TO_LONG_DOUBLE: return "UINT_TO_LONG_DOUBLE";
        case OPC_FLOAT_TO_LONG_DOUBLE: return "FLOAT_TO_LONG_DOUBLE";
        case OPC_DOUBLE_TO_LONG_DOUBLE: return "DOUBLE_TO_LONG_DOUBLE";
        case OPC_LONG_LONG_TO_LONG_DOUBLE: return "LONG_LONG_TO_LONG_DOUBLE";
        /* long long 类型专用指令名称 */
        case OPC_PUSH_LONG_LONG_CONST: return "PUSH_LONG_LONG_CONST";
        case OPC_LOAD_LONG_LONG_VAR: return "LOAD_LONG_LONG_VAR";
        case OPC_STORE_LONG_LONG_VAR: return "STORE_LONG_LONG_VAR";
        case OPC_LONG_LONG_ADD: return "LONG_LONG_ADD";
        case OPC_LONG_LONG_SUB: return "LONG_LONG_SUB";
        case OPC_LONG_LONG_MUL: return "LONG_LONG_MUL";
        case OPC_LONG_LONG_DIV: return "LONG_LONG_DIV";
        case OPC_LONG_LONG_MOD: return "LONG_LONG_MOD";
        case OPC_LONG_LONG_TO_VALUE: return "LONG_LONG_TO_VALUE";
        case OPC_LONG_LONG_GT: return "LONG_LONG_GT";
        case OPC_LONG_LONG_LT: return "LONG_LONG_LT";
        case OPC_LONG_LONG_GE: return "LONG_LONG_GE";
        case OPC_LONG_LONG_LE: return "LONG_LONG_LE";
        case OPC_LONG_LONG_EQ: return "LONG_LONG_EQ";
        case OPC_LONG_LONG_NE: return "LONG_LONG_NE";
        case OPC_LONG_LONG_ARRAY_SET: return "LONG_LONG_ARRAY_SET";
        case OPC_LONG_LONG_ARRAY_LIT: return "LONG_LONG_ARRAY_LIT";
        case OPC_LONG_LONG_ARRAY_GET: return "LONG_LONG_ARRAY_GET";
        case OPC_PRINT_LONG_LONG: return "PRINT_LONG_LONG";
        case OPC_PRINT_UINT: return "PRINT_UINT";
        /* double专用指令 */
        case OPC_LOAD_DOUBLE_VAR: return "LOAD_DOUBLE_VAR";
        case OPC_STORE_DOUBLE_VAR: return "STORE_DOUBLE_VAR";
        case OPC_DOUBLE_ARRAY_LIT: return "DOUBLE_ARRAY_LIT";
        case OPC_DOUBLE_ARRAY_GET: return "DOUBLE_ARRAY_GET";
        case OPC_PRINT_DOUBLE: return "PRINT_DOUBLE";
        /* float专用指令 */
        case OPC_LOAD_FLOAT_VAR: return "LOAD_FLOAT_VAR";
        case OPC_STORE_FLOAT_VAR: return "STORE_FLOAT_VAR";
        case OPC_PRINT_FLOAT: return "PRINT_FLOAT";
        case OPC_PUSH_FLOAT_CONST: return "PUSH_FLOAT_CONST";
        case OPC_FLOAT_ADD: return "FLOAT_ADD";
        case OPC_FLOAT_SUB: return "FLOAT_SUB";
        case OPC_FLOAT_MUL: return "FLOAT_MUL";
        case OPC_FLOAT_DIV: return "FLOAT_DIV";
        case OPC_FLOAT_TO_VALUE: return "FLOAT_TO_VALUE";
        case OPC_FLOAT_GT: return "FLOAT_GT";
        case OPC_FLOAT_LT: return "FLOAT_LT";
        case OPC_FLOAT_GE: return "FLOAT_GE";
        case OPC_FLOAT_LE: return "FLOAT_LE";
        case OPC_FLOAT_EQ: return "FLOAT_EQ";
        case OPC_FLOAT_NE: return "FLOAT_NE";
        case OPC_FLOAT_ARRAY_SET: return "FLOAT_ARRAY_SET";
        /* bool专用指令 */
        case OPC_PUSH_BOOL_CONST: return "PUSH_BOOL_CONST";
        case OPC_LOAD_BOOL_VAR: return "LOAD_BOOL_VAR";
        case OPC_STORE_BOOL_VAR: return "STORE_BOOL_VAR";
        case OPC_PRINT_BOOL: return "PRINT_BOOL";
        case OPC_PUSH_CHAR_CONST: return "PUSH_CHAR_CONST";
        case OPC_PUSH_BYTE_CONST: return "PUSH_BYTE_CONST";
        case OPC_PUSH_INT8_CONST: return "PUSH_INT8_CONST";
        case OPC_PUSH_INT16_CONST: return "PUSH_INT16_CONST";
        case OPC_PUSH_SHORT_CONST: return "PUSH_SHORT_CONST";
        case OPC_PUSH_INT32_CONST: return "PUSH_INT32_CONST";
        case OPC_PUSH_INT64_CONST: return "PUSH_INT64_CONST";
        case OPC_PUSH_UINT8_CONST: return "PUSH_UINT8_CONST";
        case OPC_PUSH_UINT16_CONST: return "PUSH_UINT16_CONST";
        case OPC_PUSH_UINT32_CONST: return "PUSH_UINT32_CONST";
        case OPC_PUSH_UINT64_CONST: return "PUSH_UINT64_CONST";
        case OPC_PUSH_LONG_CONST: return "PUSH_LONG_CONST";
        case OPC_PUSH_ULONG_CONST: return "PUSH_ULONG_CONST";
        case OPC_PUSH_SIZE_T_CONST: return "PUSH_SIZE_T_CONST";
        case OPC_PUSH_SSIZE_T_CONST: return "PUSH_SSIZE_T_CONST";
        /* char专用指令 */
        case OPC_LOAD_CHAR_VAR: return "LOAD_CHAR_VAR";
        case OPC_STORE_CHAR_VAR: return "STORE_CHAR_VAR";
        case OPC_PRINT_CHAR: return "PRINT_CHAR";
        /* byte专用指令 */
        case OPC_LOAD_BYTE_VAR: return "LOAD_BYTE_VAR";
        case OPC_STORE_BYTE_VAR: return "STORE_BYTE_VAR";
        case OPC_PRINT_BYTE: return "PRINT_BYTE";
        /* int8/16/32/64专用指令 */
        case OPC_LOAD_INT8_VAR: return "LOAD_INT8_VAR";
        case OPC_STORE_INT8_VAR: return "STORE_INT8_VAR";
        case OPC_PRINT_INT8: return "PRINT_INT8";
        case OPC_LOAD_INT16_VAR: return "LOAD_INT16_VAR";
        case OPC_STORE_INT16_VAR: return "STORE_INT16_VAR";
        case OPC_LOAD_SHORT_VAR: return "LOAD_SHORT_VAR";
        case OPC_STORE_SHORT_VAR: return "STORE_SHORT_VAR";
        case OPC_PRINT_INT16: return "PRINT_INT16";
        case OPC_PRINT_SHORT: return "PRINT_SHORT";
        case OPC_LOAD_INT32_VAR: return "LOAD_INT32_VAR";
        case OPC_STORE_INT32_VAR: return "STORE_INT32_VAR";
        case OPC_PRINT_INT32: return "PRINT_INT32";
        case OPC_LOAD_INT64_VAR: return "LOAD_INT64_VAR";
        case OPC_STORE_INT64_VAR: return "STORE_INT64_VAR";
        case OPC_PRINT_INT64: return "PRINT_INT64";
        /* uint8/16/32/64专用指令 */
        case OPC_LOAD_UINT8_VAR: return "LOAD_UINT8_VAR";
        case OPC_STORE_UINT8_VAR: return "STORE_UINT8_VAR";
        case OPC_PRINT_UINT8: return "PRINT_UINT8";
        case OPC_LOAD_UINT16_VAR: return "LOAD_UINT16_VAR";
        case OPC_STORE_UINT16_VAR: return "STORE_UINT16_VAR";
        case OPC_PRINT_UINT16: return "PRINT_UINT16";
        case OPC_LOAD_UINT32_VAR: return "LOAD_UINT32_VAR";
        case OPC_STORE_UINT32_VAR: return "STORE_UINT32_VAR";
        case OPC_PRINT_UINT32: return "PRINT_UINT32";
        case OPC_LOAD_UINT64_VAR: return "LOAD_UINT64_VAR";
        case OPC_STORE_UINT64_VAR: return "STORE_UINT64_VAR";
        case OPC_PRINT_UINT64: return "PRINT_UINT64";
        /* long/ulong专用指令 */
        case OPC_LOAD_LONG_VAR: return "LOAD_LONG_VAR";
        case OPC_STORE_LONG_VAR: return "STORE_LONG_VAR";
        case OPC_PRINT_LONG: return "PRINT_LONG";
        case OPC_LOAD_ULONG_VAR: return "LOAD_ULONG_VAR";
        case OPC_STORE_ULONG_VAR: return "STORE_ULONG_VAR";
        /* size_t/ssize_t专用指令 */
        case OPC_LOAD_SIZE_T_VAR: return "LOAD_SIZE_T_VAR";
        case OPC_STORE_SIZE_T_VAR: return "STORE_SIZE_T_VAR";
        case OPC_LOAD_SSIZE_T_VAR: return "LOAD_SSIZE_T_VAR";
        case OPC_STORE_SSIZE_T_VAR: return "STORE_SSIZE_T_VAR";
        /* long double专用指令 */
        case OPC_LOAD_LONG_DOUBLE_VAR: return "LOAD_LONG_DOUBLE_VAR";
        case OPC_STORE_LONG_DOUBLE_VAR: return "STORE_LONG_DOUBLE_VAR";
        /* 生成器指令 */
        case OPC_YIELD: return "YIELD";
        /* 其他指令 */
        case OPC_LOAD_VAR_REF: return "LOAD_VAR_REF";
        case OPC_FLOAT_ARRAY_LIT: return "FLOAT_ARRAY_LIT";
        case OPC_FLOAT_ARRAY_GET: return "FLOAT_ARRAY_GET";
        case OPC_UINT_ARRAY_LIT: return "UINT_ARRAY_LIT";
        case OPC_UINT_ARRAY_GET: return "UINT_ARRAY_GET";
        case OPC_BOOL_ARRAY_LIT: return "BOOL_ARRAY_LIT";
        case OPC_BOOL_ARRAY_GET: return "BOOL_ARRAY_GET";
        case OPC_CHAR_ARRAY_LIT: return "CHAR_ARRAY_LIT";
        case OPC_CHAR_ARRAY_GET: return "CHAR_ARRAY_GET";
        case OPC_BYTE_ARRAY_LIT: return "BYTE_ARRAY_LIT";
        case OPC_BYTE_ARRAY_GET: return "BYTE_ARRAY_GET";
        case OPC_LOAD_FIELD: return "LOAD_FIELD";
        case OPC_STORE_FIELD: return "STORE_FIELD";
        case OPC_STORE_NESTED_FIELD: return "STORE_NESTED_FIELD";
        case OPC_LOAD_STRUCT_PTR: return "LOAD_STRUCT_PTR";
        /* int8/16/32/64数组指令 */
        case OPC_INT8_ARRAY_LIT: return "INT8_ARRAY_LIT";
        case OPC_INT8_ARRAY_GET: return "INT8_ARRAY_GET";
        case OPC_INT16_ARRAY_LIT: return "INT16_ARRAY_LIT";
        case OPC_INT16_ARRAY_GET: return "INT16_ARRAY_GET";
        case OPC_INT32_ARRAY_LIT: return "INT32_ARRAY_LIT";
        case OPC_INT32_ARRAY_GET: return "INT32_ARRAY_GET";
        case OPC_INT64_ARRAY_LIT: return "INT64_ARRAY_LIT";
        case OPC_INT64_ARRAY_GET: return "INT64_ARRAY_GET";
        /* uint8/16/32/64数组指令 */
        case OPC_UINT8_ARRAY_LIT: return "UINT8_ARRAY_LIT";
        case OPC_UINT8_ARRAY_GET: return "UINT8_ARRAY_GET";
        case OPC_UINT16_ARRAY_LIT: return "UINT16_ARRAY_LIT";
        case OPC_UINT16_ARRAY_GET: return "UINT16_ARRAY_GET";
        case OPC_UINT32_ARRAY_LIT: return "UINT32_ARRAY_LIT";
        case OPC_UINT32_ARRAY_GET: return "UINT32_ARRAY_GET";
        case OPC_UINT64_ARRAY_LIT: return "UINT64_ARRAY_LIT";
        case OPC_UINT64_ARRAY_GET: return "UINT64_ARRAY_GET";
        /* long/ulong数组指令 */
        case OPC_LONG_ARRAY_LIT: return "LONG_ARRAY_LIT";
        case OPC_LONG_ARRAY_GET: return "LONG_ARRAY_GET";
        case OPC_ULONG_ARRAY_LIT: return "ULONG_ARRAY_LIT";
        case OPC_ULONG_ARRAY_GET: return "ULONG_ARRAY_GET";
        /* size_t/ssize_t数组指令 */
        case OPC_SIZE_T_ARRAY_LIT: return "SIZE_T_ARRAY_LIT";
        case OPC_SIZE_T_ARRAY_GET: return "SIZE_T_ARRAY_GET";
        case OPC_SSIZE_T_ARRAY_LIT: return "SSIZE_T_ARRAY_LIT";
        case OPC_SSIZE_T_ARRAY_GET: return "SSIZE_T_ARRAY_GET";
        /* long double数组指令 */
        case OPC_LONG_DOUBLE_ARRAY_LIT: return "LONG_DOUBLE_ARRAY_LIT";
        case OPC_LONG_DOUBLE_ARRAY_GET: return "LONG_DOUBLE_ARRAY_GET";
        /* 剩余PRINT指令 */
        case OPC_PRINT_ULONG: return "PRINT_ULONG";
        case OPC_PRINT_SIZE_T: return "PRINT_SIZE_T";
        case OPC_PRINT_SSIZE_T: return "PRINT_SSIZE_T";
        case OPC_PRINT_LONG_DOUBLE: return "PRINT_LONG_DOUBLE";
        case OPC_PUSH_LONG_DOUBLE_CONST: return "PUSH_LONG_DOUBLE_CONST";
        case OPC_LONG_DOUBLE_ADD: return "LONG_DOUBLE_ADD";
        case OPC_LONG_DOUBLE_SUB: return "LONG_DOUBLE_SUB";
        case OPC_LONG_DOUBLE_MUL: return "LONG_DOUBLE_MUL";
        case OPC_LONG_DOUBLE_DIV: return "LONG_DOUBLE_DIV";
        case OPC_LONG_DOUBLE_GT: return "LONG_DOUBLE_GT";
        case OPC_LONG_DOUBLE_LT: return "LONG_DOUBLE_LT";
        case OPC_LONG_DOUBLE_GE: return "LONG_DOUBLE_GE";
        case OPC_LONG_DOUBLE_LE: return "LONG_DOUBLE_LE";
        case OPC_LONG_DOUBLE_EQ: return "LONG_DOUBLE_EQ";
        case OPC_LONG_DOUBLE_NE: return "LONG_DOUBLE_NE";
        /* ===== int8 类型专用算术/比较运算指令 ===== */
        case OPC_INT8_ADD: return "INT8_ADD";
        case OPC_INT8_SUB: return "INT8_SUB";
        case OPC_INT8_MUL: return "INT8_MUL";
        case OPC_INT8_DIV: return "INT8_DIV";
        case OPC_INT8_MOD: return "INT8_MOD";
        case OPC_INT8_GT: return "INT8_GT";
        case OPC_INT8_LT: return "INT8_LT";
        case OPC_INT8_GE: return "INT8_GE";
        case OPC_INT8_LE: return "INT8_LE";
        case OPC_INT8_EQ: return "INT8_EQ";
        case OPC_INT8_NE: return "INT8_NE";
        /* ===== int16 类型专用算术/比较运算指令 ===== */
        case OPC_INT16_ADD: return "INT16_ADD";
        case OPC_INT16_SUB: return "INT16_SUB";
        case OPC_INT16_MUL: return "INT16_MUL";
        case OPC_INT16_DIV: return "INT16_DIV";
        case OPC_INT16_MOD: return "INT16_MOD";
        case OPC_INT16_GT: return "INT16_GT";
        case OPC_INT16_LT: return "INT16_LT";
        case OPC_INT16_GE: return "INT16_GE";
        case OPC_INT16_LE: return "INT16_LE";
        case OPC_INT16_EQ: return "INT16_EQ";
        case OPC_INT16_NE: return "INT16_NE";
        case OPC_SHORT_ADD: return "SHORT_ADD";
        case OPC_SHORT_SUB: return "SHORT_SUB";
        case OPC_SHORT_MUL: return "SHORT_MUL";
        case OPC_SHORT_DIV: return "SHORT_DIV";
        case OPC_SHORT_MOD: return "SHORT_MOD";
        case OPC_SHORT_GT: return "SHORT_GT";
        case OPC_SHORT_LT: return "SHORT_LT";
        case OPC_SHORT_GE: return "SHORT_GE";
        case OPC_SHORT_LE: return "SHORT_LE";
        case OPC_SHORT_EQ: return "SHORT_EQ";
        case OPC_SHORT_NE: return "SHORT_NE";
        /* ===== int32 类型专用算术/比较运算指令 ===== */
        case OPC_INT32_ADD: return "INT32_ADD";
        case OPC_INT32_SUB: return "INT32_SUB";
        case OPC_INT32_MUL: return "INT32_MUL";
        case OPC_INT32_DIV: return "INT32_DIV";
        case OPC_INT32_MOD: return "INT32_MOD";
        case OPC_INT32_GT: return "INT32_GT";
        case OPC_INT32_LT: return "INT32_LT";
        case OPC_INT32_GE: return "INT32_GE";
        case OPC_INT32_LE: return "INT32_LE";
        case OPC_INT32_EQ: return "INT32_EQ";
        case OPC_INT32_NE: return "INT32_NE";
        /* ===== int64 类型专用算术/比较运算指令 ===== */
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
        /* 跳转指令 */
        case OPC_JMP_IF_NULL: return "JMP_IF_NULL";
    }
    return "?";
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
                const_to_text(fn->consts[in.a], cb, sizeof(cb));
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
