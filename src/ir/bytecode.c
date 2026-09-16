#include "bytecode.h"
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
        case VAL_INT:    return a.v.i == b.v.i;
        case VAL_DOUBLE: return a.v.d == b.v.d;
        case VAL_BOOL:   return a.v.b == b.v.b;
        case VAL_CHAR:   return a.v.c == b.v.c;
        case VAL_STRING: return strcmp(lumyr_str_cstr(&a), lumyr_str_cstr(&b)) == 0;
        default:         return 0;
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

// ---------------- 静态栈深度分析 ----------------

// 指令对栈的净变化（执行一条指令前后 sp 差）
static int op_stack_delta(BytecodeFunc* fn, Instruction in)
{
    switch(in.op) {
        case OPC_LOAD_CONST:
        case OPC_LOAD_VAR:
        case OPC_LOAD_VAR_REF:
        case OPC_GETFUNC:
        case OPC_MKCLOSURE:
        case OPC_PRE_INC: case OPC_POST_INC: case OPC_PRE_DEC: case OPC_POST_DEC:
        case OPC_DUP:
            return +1;
        case OPC_LOAD_INT_VAR:
            return 0;                        /* 压入 int 栈，不改变 Value 栈深度 */
        case OPC_STORE_INT_VAR:
            return +1;                       /* 从 int 栈弹出 int，包装成 Value 压回（赋值表达式有返回值） */
        case OPC_PUSH_INT_CONST:
            return 0;                        /* 压入 int 栈，不改变 Value 栈深度 */
        case OPC_PUSH_UINT_CONST:
            return 0;                        /* 压入 uint 栈，不改变 Value 栈深度 */
        case OPC_PUSH_BOOL_CONST:
            return 0;                        /* 压入 bool 栈，不改变 Value 栈深度 */
        case OPC_PUSH_CHAR_CONST:
            return 0;                        /* 压入 char 栈，不改变 Value 栈深度 */
        case OPC_PUSH_BYTE_CONST:
            return 0;                        /* 压入 byte 栈，不改变 Value 栈深度 */
        case OPC_PUSH_INT8_CONST:
        case OPC_PUSH_INT16_CONST:
        case OPC_PUSH_INT32_CONST:
        case OPC_PUSH_INT64_CONST:
        case OPC_PUSH_UINT8_CONST:
        case OPC_PUSH_UINT16_CONST:
        case OPC_PUSH_UINT64_CONST:
        case OPC_PUSH_LONG_CONST:
        case OPC_PUSH_ULONG_CONST:
        case OPC_PUSH_SIZE_T_CONST:
        case OPC_PUSH_SSIZE_T_CONST:
            return 0;                        /* 压入专用栈，不改变 Value 栈深度 */
        /* 新的专用指令（各类型专用栈，不改变 Value 栈深度） */
        case OPC_LOAD_INT8_VAR: case OPC_LOAD_INT16_VAR: case OPC_LOAD_INT32_VAR: case OPC_LOAD_INT64_VAR:
        case OPC_LOAD_UINT8_VAR: case OPC_LOAD_UINT16_VAR: case OPC_LOAD_UINT32_VAR: case OPC_LOAD_UINT64_VAR:
        case OPC_LOAD_LONG_VAR: case OPC_LOAD_ULONG_VAR:
        case OPC_LOAD_BOOL_VAR: case OPC_LOAD_CHAR_VAR: case OPC_LOAD_BYTE_VAR:
        case OPC_LOAD_FLOAT_VAR:
        case OPC_LOAD_UINT_VAR:
        case OPC_LOAD_SIZE_T_VAR: case OPC_LOAD_SSIZE_T_VAR:
        case OPC_LOAD_LONG_DOUBLE_VAR:
            return 0;                        /* 压入专用栈，不改变 Value 栈深度 */
        case OPC_STORE_INT8_VAR: case OPC_STORE_INT16_VAR: case OPC_STORE_INT32_VAR: case OPC_STORE_INT64_VAR:
        case OPC_STORE_UINT8_VAR: case OPC_STORE_UINT16_VAR: case OPC_STORE_UINT32_VAR: case OPC_STORE_UINT64_VAR:
        case OPC_STORE_LONG_VAR: case OPC_STORE_ULONG_VAR:
        case OPC_STORE_BOOL_VAR: case OPC_STORE_CHAR_VAR: case OPC_STORE_BYTE_VAR:
        case OPC_STORE_FLOAT_VAR:
        case OPC_STORE_UINT_VAR:
        case OPC_STORE_SIZE_T_VAR: case OPC_STORE_SSIZE_T_VAR:
        case OPC_STORE_LONG_DOUBLE_VAR:
            return +1;                       /* 从专用栈弹出，包装成 Value 压回（赋值表达式有返回值） */
        case OPC_PRINT_INT8: case OPC_PRINT_INT16: case OPC_PRINT_INT32: case OPC_PRINT_INT64:
        case OPC_PRINT_UINT8: case OPC_PRINT_UINT16: case OPC_PRINT_UINT32: case OPC_PRINT_UINT64:
        case OPC_PRINT_LONG: case OPC_PRINT_ULONG:
        case OPC_PRINT_BOOL: case OPC_PRINT_CHAR: case OPC_PRINT_BYTE:
            return 0;                        /* 从专用栈弹出并打印，不改变Value栈深度 */
        case OPC_YIELD:
            return 0;                        /* 生成器yield，栈不变 */
        case OPC_INT_ADD: case OPC_INT_SUB: case OPC_INT_MUL: case OPC_INT_DIV: case OPC_INT_MOD:
            return 0;                        /* 从 int 栈弹2压1，不改变 Value 栈深度（零开销算术运算） */
        case OPC_INT_TO_VALUE:
            return +1;                       /* 从 int 栈弹出1个，包装成 Value 压入 Value 栈（+1） */
        case OPC_INT_GT: case OPC_INT_LT: case OPC_INT_GE: case OPC_INT_LE: case OPC_INT_EQ: case OPC_INT_NE:
            return +1;                       /* 从 int 栈弹出2个，比较结果(bool)压入 Value 栈（+1） */
        case OPC_INT_ARRAY_SET:
            return -1;                       /* 从 Value 栈弹出数组和索引(2个)，压入被设置的值(1个)，Value栈变化-1；从 int 栈弹出值(1个) */
        case OPC_UINT_ADD: case OPC_UINT_SUB: case OPC_UINT_MUL: case OPC_UINT_DIV: case OPC_UINT_MOD:
            return 0;                        /* 从 uint 栈弹2压1，不改变 Value 栈深度（零开销算术运算） */
        case OPC_UINT_TO_VALUE:
            return +1;                       /* 从 uint 栈弹出1个，包装成 Value 压入 Value 栈（+1） */
        case OPC_UINT_GT: case OPC_UINT_LT: case OPC_UINT_GE: case OPC_UINT_LE: case OPC_UINT_EQ: case OPC_UINT_NE:
            return +1;                       /* 从 uint 栈弹出2个，比较结果(bool)压入 Value 栈（+1） */
        case OPC_UINT_ARRAY_SET:
            return -1;                       /* 从 Value 栈弹出数组和索引(2个)，压入被设置的值(1个)，Value栈变化-1；从 uint 栈弹出值(1个) */
        /* 类型转换指令：专用栈之间的转换，不改变 Value 栈深度（零包装零Value开销） */
        case OPC_INT_TO_FLOAT:
        case OPC_INT_TO_DOUBLE:
        case OPC_UINT_TO_FLOAT:
        case OPC_UINT_TO_DOUBLE:
        case OPC_FLOAT_TO_DOUBLE:
            return 0;                        /* 从一个专用栈弹出1个，转换后压入另一个专用栈，不改变 Value 栈深度 */
        /* long long 类型专用指令栈深度计算 */
        case OPC_PUSH_LONG_LONG_CONST:
        case OPC_LOAD_LONG_LONG_VAR:
            return 0;                        /* 压入 long long 栈，不改变 Value 栈深度 */
        case OPC_STORE_LONG_LONG_VAR:
            return +1;                       /* 从 long long 栈弹出，包装成 Value 压回（赋值表达式有返回值） */
        case OPC_LONG_LONG_ADD: case OPC_LONG_LONG_SUB: case OPC_LONG_LONG_MUL: case OPC_LONG_LONG_DIV: case OPC_LONG_LONG_MOD:
            return 0;                        /* 从 long long 栈弹2压1，不改变 Value 栈深度（零开销算术运算） */
        case OPC_LONG_LONG_TO_VALUE:
            return +1;                       /* 从 long long 栈弹出1个，包装成 Value 压入 Value 栈（+1） */
        case OPC_LONG_LONG_GT: case OPC_LONG_LONG_LT: case OPC_LONG_LONG_GE: case OPC_LONG_LONG_LE: case OPC_LONG_LONG_EQ: case OPC_LONG_LONG_NE:
            return +1;                       /* 从 long long 栈弹出2个，比较结果(bool)压入 Value 栈（+1） */
        case OPC_LONG_LONG_ARRAY_SET:
            return -1;                       /* 从 Value 栈弹出数组和索引(2个)，压入被设置的值(1个)，Value栈变化-1 */
        case OPC_LONG_LONG_ARRAY_LIT:
            return +1;                       /* 弹 b 个元素，压入1个数组 Value，Value栈变化+1-b */
        case OPC_LONG_LONG_ARRAY_GET:
            return 0;                        /* 从 Value 栈弹出数组和索引(2个)，压入 long long 栈(1个)，Value栈变化-2+1=-1？不对，应该是0因为结果在专用栈 */
        case OPC_PRINT_LONG_LONG:
            return 0;                        /* 从 long long 栈弹出并打印，Value 栈不变 */
        case OPC_POP:
        case OPC_PEND_RETURN:
        case OPC_THROW:
            return -1;
        case OPC_ADD: case OPC_SUB: case OPC_MUL: case OPC_DIV: case OPC_MOD:
        case OPC_GT: case OPC_LT: case OPC_GE: case OPC_LE: case OPC_EQ: case OPC_NE: case OPC_IMPLEMENTS:
            return -1;                       // 弹2压1
        case OPC_NEG: case OPC_POS:
        case OPC_LOGIC_NOT:
        case OPC_CAST_INT: case OPC_CAST_DOUBLE: case OPC_CAST_CHAR:
        case OPC_CAST_BOOL: case OPC_CAST_STRING: case OPC_CAST_ASCII:
        case OPC_CAST_BYTE:
        case OPC_CAST_INT8: case OPC_CAST_INT16: case OPC_CAST_INT32: case OPC_CAST_INT64:
        case OPC_CAST_UINT8: case OPC_CAST_UINT16: case OPC_CAST_UINT32: case OPC_CAST_UINT64:
        case OPC_CAST_LONG: case OPC_CAST_LONGLONG: case OPC_CAST_FLOAT:
            return 0;                        // 弹1压1
        case OPC_TRY:
        case OPC_ENDTRY:
        case OPC_FIN_PUSH:
        case OPC_FINISH:
            return 0;                        // 栈不变
        case OPC_GET_ERR:
            return 1;                        // 压 1 错误消息
        case OPC_BUILTIN:
            return -in.b + 1;                // 弹 b 实参，压 1 结果
        case OPC_ARRAY_LIT:
            return -in.b + 1;                // 弹 b 元素，压 1 数组
        case OPC_INT_ARRAY_LIT:
            if(in.a == 1) return +1;          /* 从 int 栈读取 b 元素，压 1 数组（Value 栈净变化 +1） */
            return -in.b + 1;                  /* 从 Value 栈读取 b 元素，压 1 数组 */
        case OPC_INT_ARRAY_GET:
            return -2;                          /* 弹 arr,idx（2个Value栈元素），压入 int 栈（Value栈净变化-2） */
        case OPC_LOAD_DOUBLE_VAR:
        case OPC_PUSH_DOUBLE_CONST:
            return 0;                        /* 压入 double 栈，不改变 Value 栈深度 */
        case OPC_STORE_DOUBLE_VAR:
            return +1;                       /* 从 double 栈弹出 double，包装成 Value 压回（赋值表达式有返回值） */
        case OPC_DOUBLE_ADD: case OPC_DOUBLE_SUB: case OPC_DOUBLE_MUL: case OPC_DOUBLE_DIV:
            return 0;                        /* 从 double 栈弹2压1，不改变 Value 栈深度（零开销算术运算） */
        case OPC_DOUBLE_TO_VALUE:
            return +1;                       /* 从 double 栈弹出1个，包装成 Value 压入 Value 栈（+1） */
        case OPC_DOUBLE_GT: case OPC_DOUBLE_LT: case OPC_DOUBLE_GE: case OPC_DOUBLE_LE: case OPC_DOUBLE_EQ: case OPC_DOUBLE_NE:
            return +1;                       /* 从 double 栈弹出2个，比较结果(bool)压入 Value 栈（+1） */
        case OPC_DOUBLE_ARRAY_SET:
            return -1;                       /* 从 Value 栈弹出数组和索引(2个)，压入被设置的值(1个)，Value栈变化-1；从 double 栈弹出值(1个) */
        case OPC_DOUBLE_ARRAY_LIT:
            if(in.a == 1) return +1;          /* 从 double 栈读取 b 元素，压 1 数组（Value 栈净变化 +1） */
            return -in.b + 1;                  /* 从 Value 栈读取 b 元素，压 1 数组 */
        case OPC_DOUBLE_ARRAY_GET:
            return -2;                          /* 弹 arr,idx（2个Value栈元素），压入 double 栈（Value栈净变化-2） */
        case OPC_PUSH_FLOAT_CONST:
            return 0;                        /* 压入 float 栈，不改变 Value 栈深度 */
        case OPC_FLOAT_ADD: case OPC_FLOAT_SUB: case OPC_FLOAT_MUL: case OPC_FLOAT_DIV:
            return 0;                        /* 从 float 栈弹2压1，不改变 Value 栈深度（零开销算术运算） */
        case OPC_FLOAT_TO_VALUE:
            return +1;                       /* 从 float 栈弹出1个，包装成 Value 压入 Value 栈（+1） */
        case OPC_FLOAT_GT: case OPC_FLOAT_LT: case OPC_FLOAT_GE: case OPC_FLOAT_LE: case OPC_FLOAT_EQ: case OPC_FLOAT_NE:
            return +1;                       /* 从 float 栈弹出2个，比较结果(bool)压入 Value 栈（+1） */
        case OPC_FLOAT_ARRAY_SET:
            return -1;                       /* 从 Value 栈弹出数组和索引(2个)，压入被设置的值(1个)，Value栈变化-1；从 float 栈弹出值(1个) */
        case OPC_INT8_ARRAY_LIT:
            if(in.a == 1) return +1;
            return -in.b + 1;
        case OPC_INT8_ARRAY_GET:
            return -2;
        case OPC_INT16_ARRAY_LIT:
            if(in.a == 1) return +1;
            return -in.b + 1;
        case OPC_INT16_ARRAY_GET:
            return -2;
        case OPC_INT32_ARRAY_LIT:
            if(in.a == 1) return +1;
            return -in.b + 1;
        case OPC_INT32_ARRAY_GET:
            return -2;
        case OPC_INT64_ARRAY_LIT:
            if(in.a == 1) return +1;
            return -in.b + 1;
        case OPC_INT64_ARRAY_GET:
            return -2;
        case OPC_UINT8_ARRAY_LIT:
            if(in.a == 1) return +1;
            return -in.b + 1;
        case OPC_UINT8_ARRAY_GET:
            return -2;
        case OPC_UINT16_ARRAY_LIT:
            if(in.a == 1) return +1;
            return -in.b + 1;
        case OPC_UINT16_ARRAY_GET:
            return -2;
        case OPC_UINT32_ARRAY_LIT:
            if(in.a == 1) return +1;
            return -in.b + 1;
        case OPC_UINT32_ARRAY_GET:
            return -2;
        case OPC_UINT64_ARRAY_LIT:
            if(in.a == 1) return +1;
            return -in.b + 1;
        case OPC_UINT64_ARRAY_GET:
            return -2;
        case OPC_LONG_ARRAY_LIT:
            if(in.a == 1) return +1;
            return -in.b + 1;
        case OPC_LONG_ARRAY_GET:
            return -2;
        case OPC_ULONG_ARRAY_LIT:
            if(in.a == 1) return +1;
            return -in.b + 1;
        case OPC_ULONG_ARRAY_GET:
            return -2;
        case OPC_SIZE_T_ARRAY_LIT:
            if(in.a == 1) return +1;
            return -in.b + 1;
        case OPC_SIZE_T_ARRAY_GET:
            return -2;
        case OPC_SSIZE_T_ARRAY_LIT:
            if(in.a == 1) return +1;
            return -in.b + 1;
        case OPC_SSIZE_T_ARRAY_GET:
            return -2;
        case OPC_LONG_DOUBLE_ARRAY_LIT:
            if(in.a == 1) return +1;
            return -in.b + 1;
        case OPC_LONG_DOUBLE_ARRAY_GET:
            return -2;
        case OPC_MAP_LIT:
            return -2 * in.b + 1;            // 弹 2b 键值，压 1 字典
        case OPC_CLASS_NEW:
            return 1;                        // 压 1 class 实例
        case OPC_INDEX_GET:
            return -1;                       // 弹2压1
        case OPC_INDEX_SET:
            return -2;                       // 弹3压1
        case OPC_LOAD_FIELD:
            return 1;                        // 压1（字段值）
        case OPC_STORE_FIELD:
            return 0;                        // 弹1压1（表达式值）
        case OPC_LOAD_STRUCT_PTR:
            return 1;                        // 压1（struct 指针）
        case OPC_STORE_NESTED_FIELD:
            return 0;                        // 弹1压1（表达式值）
        case OPC_STORE_VAR:
            return 0;                        // 弹1压1
        case OPC_PRINT:
            return -(in.a > 0 ? in.a : 1);  /* 多参数打印：弹出所有参数（向后兼容：a<=0 时弹1） */
        case OPC_PRINT_INT:
        case OPC_PRINT_UINT:
        case OPC_PRINT_FLOAT:
        case OPC_PRINT_DOUBLE:
        case OPC_PRINT_SIZE_T:
        case OPC_PRINT_SSIZE_T:
        case OPC_PRINT_LONG_DOUBLE:
            return 0;  /* 从专用栈弹出值，不改变 Value 栈深度 */
        case OPC_TO_BOOL:
        case OPC_JMP:
        case OPC_RETURN_NIL:
        case OPC_HALT:
        case OPC_NOP:
            return 0;
        case OPC_JMP_IF_FALSE:
        case OPC_JMP_IF_TRUE:
        case OPC_JMP_IF_NULL:
            return -1;                       // 弹条件
        case OPC_CALL:
            return -in.b + 1;                // 弹 argc 实参，压 1 返回值
        case OPC_CALLV:
            return -in.b;                    // 弹 argc 实参 + 函数值，压 1 返回值
        case OPC_RETURN:
            return -1;                       // 弹返回值
    }
    return 0;
}

// 指令执行瞬间的额外栈高（压栈动作造成的峰值超出进入深度部分）
static int op_stack_push(OpCode op)
{
    switch(op) {
        case OPC_LOAD_CONST:
        case OPC_LOAD_VAR:
        case OPC_LOAD_VAR_REF:
        case OPC_GETFUNC:
        case OPC_MKCLOSURE:
        case OPC_PRE_INC: case OPC_POST_INC: case OPC_PRE_DEC: case OPC_POST_DEC:
        case OPC_DUP:
        case OPC_LOAD_FIELD:
        case OPC_INT_ARRAY_LIT:
        case OPC_DOUBLE_ARRAY_LIT:
        case OPC_FLOAT_ARRAY_LIT:
        case OPC_UINT_ARRAY_LIT:
        case OPC_BOOL_ARRAY_LIT:
        case OPC_CHAR_ARRAY_LIT:
        case OPC_BYTE_ARRAY_LIT:
            return 1;
        default:
            return 0;
    }
}

int bc_analyze_stack(BytecodeFunc* fn, int* depth_out, int depth_cap)
{
    if(!fn || fn->code_len == 0) return 0;
    int n = fn->code_len;
    int* d = (int*)malloc(sizeof(int) * n);
    if(!d) { perror("bc_analyze_stack"); exit(EXIT_FAILURE); }
    for(int i = 0; i < n; i++) d[i] = -1;
    d[0] = 0;

    // 数据流迭代：顺序后继 + 跳转后继，直到收敛
    int changed = 1;
    while(changed) {
        changed = 0;
        for(int i = 0; i < n; i++) {
            if(d[i] < 0) continue;
            Instruction in = fn->code[i];
            int nd = d[i] + op_stack_delta(fn, in);
            if(nd < 0) {
                fprintf(stderr, "IR 栈深分析: 指令 %d 栈下溢（深度 %d）——IR 生成错误\n", i, nd);
                free(d);
                return -1;
            }
            if(in.op == OPC_JMP || in.op == OPC_JMP_IF_FALSE || in.op == OPC_JMP_IF_TRUE || in.op == OPC_JMP_IF_NULL) {
                if(in.a >= 0 && in.a < n && d[in.a] < nd) { d[in.a] = nd; changed = 1; }
            }
            /* try/catch/finally 的非跳转式目标：TRY.a=catch 入口（异常路径 sp 恢复后）、
               TRY.b/FIN_PUSH.b/PEND_RETURN.b=finally 或完成动作目标。
               a/b 用 0 作"无目标"哨兵，必须 >0 才算后继，否则会把 pc=0 当成目标
               形成 d[0]→...→TRY→d[0] 的正反馈环导致分析不收敛 */
            if(in.op == OPC_TRY) {
                if(in.a > 0 && in.a < n && d[in.a] < nd) { d[in.a] = nd; changed = 1; }
                if(in.b > 0 && in.b < n && d[in.b] < nd) { d[in.b] = nd; changed = 1; }
            }
            if(in.op == OPC_FIN_PUSH || in.op == OPC_PEND_RETURN) {
                if(in.b > 0 && in.b < n && d[in.b] < nd) { d[in.b] = nd; changed = 1; }
            }
            if(in.op != OPC_RETURN && in.op != OPC_RETURN_NIL && in.op != OPC_HALT &&
               in.op != OPC_JMP) {
                if(i + 1 < n && d[i + 1] < nd) { d[i + 1] = nd; changed = 1; }
            }
        }
    }

    int maxd = 0;
    for(int i = 0; i < n; i++) {
        int depth = (d[i] < 0) ? 0 : d[i];   // 不可达指令深度记 0
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
        case OPC_INT_TO_FLOAT: return "INT_TO_FLOAT";
        case OPC_INT_TO_DOUBLE: return "INT_TO_DOUBLE";
        case OPC_UINT_TO_FLOAT: return "UINT_TO_FLOAT";
        case OPC_UINT_TO_DOUBLE: return "UINT_TO_DOUBLE";
        case OPC_FLOAT_TO_DOUBLE: return "FLOAT_TO_DOUBLE";
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
        case OPC_PUSH_INT32_CONST: return "PUSH_INT32_CONST";
        case OPC_PUSH_INT64_CONST: return "PUSH_INT64_CONST";
        case OPC_PUSH_UINT8_CONST: return "PUSH_UINT8_CONST";
        case OPC_PUSH_UINT16_CONST: return "PUSH_UINT16_CONST";
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
        case OPC_PRINT_INT16: return "PRINT_INT16";
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
        /* 跳转指令 */
        case OPC_JMP_IF_NULL: return "JMP_IF_NULL";
    }
    return "?";
}

static void const_to_text(Value v, char* buf, int cap)
{
    switch(v.type) {
        case VAL_INT:    snprintf(buf, cap, "%lld", v.v.i); break;
        case VAL_DOUBLE: snprintf(buf, cap, "%.17g", v.v.d); break;
        case VAL_BOOL:   snprintf(buf, cap, "%s", v.v.b ? "true" : "false"); break;
        case VAL_CHAR:   snprintf(buf, cap, "'%c'", v.v.c); break;
        case VAL_STRING: snprintf(buf, cap, "\"%s\"", lumyr_str_cstr(&v)); break;
        default:         snprintf(buf, cap, "nil"); break;
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
