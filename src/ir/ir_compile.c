/* 编译期模块：ir_func_table/ir_func_count 为编译期状态，单线程编译设计；
 * 未来支持并发编译时需实例化（每编译任务一份），不影响运行时多线程。 */
// AST → 字节码 IR 编译器
// 遍历结构与 ast_typecheck.c / codegen.c 对齐（用户建议复用其递归结构）。
#include "ir_compile.h"
#include "lumyr_log.h"
#include "ir_cgen.h"
#include "ir_arith.h"
#include "lumyr_ffi.h"
#include "ast/ast_runtime_sym.h"
#include "ir_opt.h"
#include "ast/lumyr_types.h"
#include "ast/ast_types.h"
#include "ast/func_compile.h"
#include "ast/ast_interp.h"
#include "rbtree.h"
#include "symbol_table.h"
#include "lm_value.h"
#include "gc_runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 变量类型标记特殊值：1000 表示变量是 int 类型化数组（用于上下文感知类型推导） */
#define VAR_TYPE_INT_ARRAY 1000
/* 变量类型标记特殊值：1001 表示变量是 double 类型化数组（用于上下文感知类型推导） */
#define VAR_TYPE_DOUBLE_ARRAY 1001
/* 变量类型标记特殊值：1002 表示变量是 float 类型化数组（用于上下文感知类型推导） */
#define VAR_TYPE_FLOAT_ARRAY 1002
/* 变量类型标记特殊值：1003 表示变量是 uint 类型化数组（用于上下文感知类型推导） */
#define VAR_TYPE_UINT_ARRAY 1003
#define VAR_TYPE_BOOL_ARRAY 1004
#define VAR_TYPE_CHAR_ARRAY 1005
#define VAR_TYPE_BYTE_ARRAY 1006
#define VAR_TYPE_INT8_ARRAY 1007

// ---------------- 全局函数表 ----------------
// yacc 期注册每个函数（ir_compile_function），main.c 注册 main（ir_compile_main）。
// 动态扩容，无硬上限。
/* 统一符号表存储函数：键为 (file_name, class_name, method_name)，class_name 为 NULL 表示普通函数 */
static SymbolTable* ir_func_table = NULL;

void ir_func_table_reset(void)
{
    if(ir_func_table) symbol_table_destroy(ir_func_table);
    ir_func_table = symbol_table_create();
}

BytecodeFunc* ir_func_table_lookup(const char* name)
{
    if(!name || !ir_func_table) return NULL;
    /* 只查找 class_name 为 NULL 的普通函数，class 方法通过 ir_func_table_lookup_class 查找 */
    SymbolEntry* entry = symbol_table_find(ir_func_table, NULL, NULL, name);
    if(entry) return (BytecodeFunc*)entry->data;
    return NULL;
}

/* 按 class_name + method_name 查找 class 方法 */
BytecodeFunc* ir_func_table_lookup_class(const char* class_name, const char* method_name)
{
    if(!class_name || !method_name || !ir_func_table) return NULL;
    SymbolEntry* entry = symbol_table_find(ir_func_table, NULL, class_name, method_name);
    if(entry) return (BytecodeFunc*)entry->data;
    return NULL;
}

/* 查找任意函数（先查找普通函数，如果找不到，再按名字查找第一个匹配的 class 方法）
   用于 CC 模式的代码生成器，因为 CC 模式在处理 OPC_GETFUNC 时不知道函数是普通函数还是 class 方法 */
BytecodeFunc* ir_func_table_lookup_any(const char* name)
{
    if(!name || !ir_func_table) return NULL;
    /* 先查找普通函数 */
    SymbolEntry* entry = symbol_table_find(ir_func_table, NULL, NULL, name);
    if(entry) return (BytecodeFunc*)entry->data;
    /* 再按名字查找第一个匹配的 class 方法 */
    entry = symbol_table_find_by_name(ir_func_table, name);
    if(entry) return (BytecodeFunc*)entry->data;
    /* DEBUG: 打印 ir_func_table 中的所有函数 */
    {
        FILE* __dbg = fopen("debug_lookup_any.txt", "a");
        if(__dbg) {
            fprintf(__dbg, "[ir_func_table_lookup_any] 未找到函数: %s\n", name);
            fprintf(__dbg, "  ir_func_table 中的所有函数:\n");
            SymbolTable* __st = ir_func_table;
            /* 遍历红黑树，打印所有节点 */
            /* 简化实现：使用 symbol_table_foreach */
            fclose(__dbg);
        }
    }
    return NULL;
}

/* 遍历所有函数（统一符号表中序遍历） */
typedef struct {
    void (*callback)(const char*, const char*, void*, void*);
    void* user_data;
} ForeachWrapperData;

static void ir_func_table_foreach_wrapper(const char* file_name, const char* scope,
                                           const char* name, SymbolType type, void* data, void* user_data)
{
    (void)file_name;
    (void)type;
    ForeachWrapperData* wd = (ForeachWrapperData*)user_data;
    if(wd && wd->callback) {
        wd->callback(scope, name, data, wd->user_data);
    }
}

void ir_func_table_foreach(void (*callback)(const char* class_name, const char* method_name, void* data, void* user_data), void* user_data)
{
    if(ir_func_table) {
        ForeachWrapperData wd;
        wd.callback = callback;
        wd.user_data = user_data;
        symbol_table_foreach(ir_func_table, ir_func_table_foreach_wrapper, &wd);
    }
}

static void ir_func_table_add(BytecodeFunc* fn)
{
    if(!ir_func_table) ir_func_table = symbol_table_create();
    if(fn->name) {
        /* 跳过原始名字的构造函数注册：构造函数会被改名成 <类名>___init__ 后再注册 */
        size_t _name_len = strlen(fn->name);
        _Bool _is_raw_constructor = (_name_len == 8 && strcmp(fn->name, "__init__") == 0 && !fn->class_name);
        if(_is_raw_constructor) {
            return;
        }
        SymbolType sym_type = fn->class_name ? SYMBOL_METHOD : SYMBOL_FUNC;
        symbol_table_add(ir_func_table, NULL, fn->class_name, fn->name, sym_type, fn);
    }
}

// ---------------- 字符串常量缓存 ----------------
// 编译期全局缓存：相同字符串字面量只创建一次 Value，避免重复分配和 bf_const 临时对象泄漏。
// 长字符串（非 SSO）调用 gc_pin 钉住，防止编译期 GC 回收；短字符串内联在 Value 中无需钉住。
static Value* str_cache = NULL;
static char** str_cache_keys = NULL;
static int str_cache_cnt = 0;
static int str_cache_cap = 0;

void string_cache_reset(void)
{
    for(int i = 0; i < str_cache_cnt; i++) {
        free(str_cache_keys[i]);
    }
    free(str_cache);
    free(str_cache_keys);
    str_cache = NULL;
    str_cache_keys = NULL;
    str_cache_cnt = 0;
    str_cache_cap = 0;
}

static Value intern_string(const char* s)
{
    if(!s) s = "";
    for(int i = 0; i < str_cache_cnt; i++) {
        if(strcmp(str_cache_keys[i], s) == 0)
            return str_cache[i];
    }
    Value v = lumyr_make_string(s);
    if(str_cache_cnt >= str_cache_cap) {
        int newcap = str_cache_cap > 0 ? str_cache_cap * 2 : 64;
        Value* nv = (Value*)realloc(str_cache, (size_t)newcap * sizeof(Value));
        char** nk = (char**)realloc(str_cache_keys, (size_t)newcap * sizeof(char*));
        if(!nv || !nk) { LOG_ERROR("IR: string cache oom\n"); exit(EXIT_FAILURE); }
        str_cache = nv;
        str_cache_keys = nk;
        str_cache_cap = newcap;
    }
    str_cache_keys[str_cache_cnt] = strdup(s);
    str_cache[str_cache_cnt] = v;
    if(v.type == VAL_STRING && !v.str_inline && v.v.s) gc_pin(v.v.s);
    str_cache_cnt++;
    return v;
}

// ---------------- 编译上下文 ----------------





/* 控制层/ finally 行 扩容 helper */
static void ctx_ensure_layers(Ctx* c, int need)
{
    if(need <= c->layers_cap) return;
    int nc = c->layers_cap > 0 ? c->layers_cap * 2 : 32;
    Layer* nl = (Layer*)realloc(c->layers, (size_t)nc * sizeof(Layer));
    if(!nl) { LOG_ERROR("IR: 控制层扩容内存不足\n"); exit(EXIT_FAILURE); }
    c->layers = nl;
    c->layers_cap = nc;
}

static void ctx_ensure_fin_rows(Ctx* c, int need)
{
    if(need < c->fin_cap) return;
    int nc = c->fin_cap > 0 ? c->fin_cap * 2 : 32;
    int** np = (int**)realloc(c->fin_pend, (size_t)nc * sizeof(int*));
    if(!np) { LOG_ERROR("IR: finally 层扩容内存不足\n"); exit(EXIT_FAILURE); }
    c->fin_pend = np;
    int** nj = (int**)realloc(c->fin_jmp, (size_t)nc * sizeof(int*));
    if(!nj) { LOG_ERROR("IR: finally 层扩容内存不足\n"); exit(EXIT_FAILURE); }
    c->fin_jmp = nj;
    int* nn = (int*)realloc(c->fin_pend_n, (size_t)nc * sizeof(int));
    if(!nn) { LOG_ERROR("IR: finally 层扩容内存不足\n"); exit(EXIT_FAILURE); }
    c->fin_pend_n = nn;
    int* nm = (int*)realloc(c->fin_jmp_n, (size_t)nc * sizeof(int));
    if(!nm) { LOG_ERROR("IR: finally 层扩容内存不足\n"); exit(EXIT_FAILURE); }
    c->fin_jmp_n = nm;
    int* nc1 = (int*)realloc(c->fin_pend_cap, (size_t)nc * sizeof(int));
    if(!nc1) { LOG_ERROR("IR: finally 层扩容内存不足\n"); exit(EXIT_FAILURE); }
    c->fin_pend_cap = nc1;
    int* nc2 = (int*)realloc(c->fin_jmp_cap, (size_t)nc * sizeof(int));
    if(!nc2) { LOG_ERROR("IR: finally 层扩容内存不足\n"); exit(EXIT_FAILURE); }
    c->fin_jmp_cap = nc2;
    for(int i = c->fin_cap; i < nc; i++) {
        c->fin_pend[i] = NULL; c->fin_pend_n[i] = 0; c->fin_pend_cap[i] = 0;
        c->fin_jmp[i] = NULL;  c->fin_jmp_n[i] = 0;  c->fin_jmp_cap[i] = 0;
    }
    c->fin_cap = nc;
}

static void fin_pend_add(Ctx* c, int pos)
{
    if(c->fin_pend_n[c->fin_depth] >= c->fin_pend_cap[c->fin_depth]) {
        int nc = c->fin_pend_cap[c->fin_depth] > 0 ? c->fin_pend_cap[c->fin_depth] * 2 : 16;
        int* na = (int*)realloc(c->fin_pend[c->fin_depth], (size_t)nc * sizeof(int));
        if(!na) { LOG_ERROR("IR: finally 挂起表扩容内存不足\n"); exit(EXIT_FAILURE); }
        c->fin_pend[c->fin_depth] = na;
        c->fin_pend_cap[c->fin_depth] = nc;
    }
    c->fin_pend[c->fin_depth][c->fin_pend_n[c->fin_depth]++] = pos;
}

static void fin_jmp_add(Ctx* c, int pos)
{
    if(c->fin_jmp_n[c->fin_depth] >= c->fin_jmp_cap[c->fin_depth]) {
        int nc = c->fin_jmp_cap[c->fin_depth] > 0 ? c->fin_jmp_cap[c->fin_depth] * 2 : 16;
        int* na = (int*)realloc(c->fin_jmp[c->fin_depth], (size_t)nc * sizeof(int));
        if(!na) { LOG_ERROR("IR: finally 跳转表扩容内存不足\n"); exit(EXIT_FAILURE); }
        c->fin_jmp[c->fin_depth] = na;
        c->fin_jmp_cap[c->fin_depth] = nc;
    }
    c->fin_jmp[c->fin_depth][c->fin_jmp_n[c->fin_depth]++] = pos;
}









// ---------------- 控制层（break/continue） ----------------

static void layer_push(Ctx* c, int kind, int cont_target)
{
    ctx_ensure_layers(c, c->layer_depth + 1);
    Layer* l = &c->layers[c->layer_depth++];
    memset(l, 0, sizeof(Layer));
    l->kind = kind;
    l->cont_target = cont_target;
}

static Layer layer_pop(Ctx* c)
{
    return c->layers[--c->layer_depth];
}

static void layer_brk_fin_add(Ctx* c, int pos)
{
    Layer* l = &c->layers[c->layer_depth - 1];
    if(l->brk_fin_cnt >= l->brk_fin_cap) {
        l->brk_fin_cap = l->brk_fin_cap ? l->brk_fin_cap * 2 : 4;
        l->brk_fin = (int*)realloc(l->brk_fin, sizeof(int) * l->brk_fin_cap);
    }
    l->brk_fin[l->brk_fin_cnt++] = pos;
}
static void layer_cont_fin_add(Ctx* c, int pos)
{
    Layer* l = &c->layers[c->layer_depth - 1];
    if(l->cont_fin_cnt >= l->cont_fin_cap) {
        l->cont_fin_cap = l->cont_fin_cap ? l->cont_fin_cap * 2 : 4;
        l->cont_fin = (int*)realloc(l->cont_fin, sizeof(int) * l->cont_fin_cap);
    }
    l->cont_fin[l->cont_fin_cnt++] = pos;
}
static void layer_brk_add(Ctx* c, int pos)
{
    Layer* l = &c->layers[c->layer_depth - 1];
    if(l->brk_cnt >= l->brk_cap) {
        l->brk_cap = l->brk_cap ? l->brk_cap * 2 : 4;
        l->brk = (int*)realloc(l->brk, sizeof(int) * l->brk_cap);
    }
    l->brk[l->brk_cnt++] = pos;
}

static void layer_cont_add(Ctx* c, int pos)
{
    for(int i = c->layer_depth - 1; i >= 0; i--) {
        if(c->layers[i].kind == 0) {
            if(c->layers[i].cont_target >= 0) {
                bf_patch(c->fn, pos, c->layers[i].cont_target);
            } else {
                Layer* l = &c->layers[i];
                if(l->cont_cnt >= l->cont_cap) {
                    l->cont_cap = l->cont_cap ? l->cont_cap * 2 : 4;
                    l->cont = (int*)realloc(l->cont, sizeof(int) * l->cont_cap);
                }
                l->cont[l->cont_cnt++] = pos;
            }
            return;
        }
    }
    LOG_ERROR("IR: continue 不在循环内\n");
    exit(EXIT_FAILURE);
}

// ---------------- 表达式 / 语句编译 ----------------

/*
 * extract_int_literal_value: 提取整数字面量的值（包括负数字面量）
 * 返回值：1表示成功，0表示失败
 * 支持的节点类型：
 *   - AST_INT：正整数字面量
 *   - AST_UNARY（op=OP_UNARY_MINUS，child=AST_INT）：负整数字面量
 */
static int extract_int_literal_value(AstNode* expr, long long* out_val) {
    if (!expr || !out_val) return 0;
    if (expr->type == AST_INT) {
        *out_val = expr->u.inum;
        return 1;
    }
    if (expr->type == AST_UNARY && expr->u.uny.op == OP_UNARY_MINUS &&
        expr->u.uny.child && expr->u.uny.child->type == AST_INT) {
        *out_val = -expr->u.uny.child->u.inum;
        return 1;
    }
    return 0;
}

static void c_stmt(Ctx* c, AstNode* node);
void c_expr(Ctx* c, AstNode* node);

/* ===== type 构造（type Person { name, age } → Person("张三", 18) 编译为 map 字面量） ===== */
static int type_arg_count(AstNode* chain)
{
    if(!chain) return 0;
    if(chain->type == AST_SEQ) return type_arg_count(chain->u.seq.first) + type_arg_count(chain->u.seq.second);
    return 1;
}
static AstNode* type_arg_nth(AstNode* chain, int n, int* cur)
{
    if(!chain) return NULL;
    if(chain->type == AST_SEQ) {
        AstNode* r = type_arg_nth(chain->u.seq.first, n, cur);
        if(r) return r;
        return type_arg_nth(chain->u.seq.second, n, cur);
    }
    if((*cur)++ == n) return chain;
    return NULL;
}
static AstNode* type_wrap_cast(AstNode* e, ValueType vt)
{
    int kind;
    switch(vt) {
        case VAL_INT:    kind = CAST_INT;    break;
        case VAL_DOUBLE: kind = CAST_DOUBLE; break;
        case VAL_STRING: kind = CAST_STRING; break;
        case VAL_BOOL:   kind = CAST_BOOL;   break;
        case VAL_CHAR:   kind = CAST_CHAR;   break;
        case VAL_BYTE:   kind = CAST_BYTE;   break;
        default:         return e;  /* 未标注/自定义：不强转 */
    }
    return new_cast_node(kind, e);
}
/* 单参是否字面量（字符串/数字等）：字面量走位置构造（Tag("a") → {"name":"a"}），
   非字面量（map 字面量/变量/调用结果）走原样返回（Person(m) 标注场景） */
static int type_arg_is_literal(AstNode* m)
{
    if(!m) return 1;
    return m->type == AST_INT || m->type == AST_NUM || m->type == AST_STRING ||
           m->type == AST_BOOL || m->type == AST_CHAR || m->type == AST_ARRAY_LIT;
}
/* Person(a, b) → {"name": cast(a), "age": cast(b)}；
   Person(m) → m（单 map 参数原样，动态语言宽松语义） */
static AstNode* build_type_ctor(AstNode* call, TypeDef* t)
{
    AstNode* args = call->u.call.args;
    int argc = type_arg_count(args);
    AstNode* items = NULL;
    if(argc == 1 && t->nprops >= 1 && !type_arg_is_literal(type_arg_nth(args, 0, &(int){0}))) {
        /* 单参数非字面量：原样返回（map 泛型元素标注场景） */
        AstNode* m = type_arg_nth(args, 0, &(int){0});
        call->u.call.args = NULL;
        return m;
    }
    /* class 有自定义构造函数（__init__）：创建 C 结构体实例后调用构造函数 */
    if(t->is_class && t->constructor) {
        /* 1. 创建 C 结构体实例（AST_CLASS_NEW 节点，编译时生成 malloc 代码）
           有自定义构造函数时 argc=0，参数传递给构造函数 */
        AstNode* obj = ast_class_new(strdup(t->name), 0, NULL);
        /* 2. 调用 <类名>___init__(obj, args...)：构造函数名作为函数名，obj 作为第一个参数 */
        AstNode* ctor_args = ast_seq(obj, args);
        char* ctor_name = (char*)malloc(strlen(t->name) + 10);
        sprintf(ctor_name, "%s___init__", t->name);
        AstNode* ctor_call = ast_call(ctor_name, ctor_args);
        call->u.call.args = NULL;
        /* 3. 直接返回构造函数调用（构造函数修改 self 后返回 self） */
        return ctor_call;
    }
    /* 默认构造函数 */
    if(t->is_class) {
        /* class 类型：创建 C 结构体实例（AST_CLASS_NEW 节点，带参数）
           OPC_CLASS_NEW 会用参数初始化字段 */
        call->u.call.args = NULL;  /* 参数节点已移入 AST_CLASS_NEW，摘空原链防双 free */
        return ast_class_new(strdup(t->name), argc, args);
    }
    /* struct/type 类型：生成 map 字面量初始化所有属性 */
    /* 首项注入只读类名属性：__mapname__ / __structname__ / __classname__ = 类型名 */
    items = ast_seq(items, ast_map_entry(ast_string(strdup("__mapname__")),
                                         ast_string(strdup(t->name))));
    items = ast_seq(items, ast_map_entry(ast_string(strdup("__structname__")),
                                         ast_string(strdup(t->name))));
    int n = argc < t->nprops ? argc : t->nprops;
    for(int k = 0; k < n; k++) {
        int cur = 0;
        AstNode* a = type_arg_nth(args, k, &cur);
        AstNode* key = ast_string(strdup(t->props[k]));
        /* 嵌套 struct 字段：不进行类型转换，原样保留参数 */
        if(t->field_struct_names && t->field_struct_names[k]) {
            items = ast_seq(items, ast_map_entry(key, a));
        } else {
            items = ast_seq(items, ast_map_entry(key, type_wrap_cast(a, t->ptypes[k])));
        }
    }
    call->u.call.args = NULL;  /* 参数节点已移入 items 树，摘空原链防双 free */
    return ast_map_lit(items);
}

static void c_args_ref(Ctx* c, AstNode* args, int* argc, int* param_is_ref, int* ref_idx)
{
    if(!args) return;
    if(args->type != AST_SEQ) {
        /* ref 参数的变量引用：使用 OPC_LOAD_VAR_REF（struct 不转 Map） */
        if(args->type == AST_VAR && param_is_ref && *ref_idx >= 0 &&
           *ref_idx < 1024 && param_is_ref[*ref_idx]) {
            emit(c, OPC_LOAD_VAR_REF, bf_sym(c->fn, args->u.varname), 0);
        } else {
            c_expr(c, args);
        }
        (*argc)++;
        (*ref_idx)++;
        return;
    }
    c_args_ref(c, args->u.seq.first, argc, param_is_ref, ref_idx);
    c_args_ref(c, args->u.seq.second, argc, param_is_ref, ref_idx);
}

static void c_args(Ctx* c, AstNode* args, int* argc)
{
    int ref_idx = 0;
    c_args_ref(c, args, argc, NULL, &ref_idx);
}

// 检查变量是否声明为 int 类型
static int is_int_var(Ctx* c, AstNode* node) {
    if(!node || node->type != AST_VAR) return 0;
    int var_idx = bf_sym(c->fn, node->u.varname);
    if(var_idx < 0 || var_idx >= c->fn->sym_cnt) return 0;
    return (c->fn->var_type_tags && c->fn->var_type_tags[var_idx] == CAST_INT);
}

// 检查数组所有元素是否都是 int 类型（声明为 int 的变量或 int 类型化数组的元素访问）
static int all_int_vars(Ctx* c, AstNode* e) {
    if(!e) return 1;
    if(e->type == AST_SEQ) {
        return all_int_vars(c, e->u.seq.first) && all_int_vars(c, e->u.seq.second);
    }
    if(e->type == AST_INDEX) {
        /* 数组访问表达式：视为 int 候选，运行时 OPC_INT_ARRAY_GET 会检查是否是 int 类型化数组 */
        return 1;
    }
    return is_int_var(c, e);
}

// 编译 int 泛型数组元素：全部压入 int 栈（零检查零转换）
// 支持：声明为 int 的变量（OPC_LOAD_INT_VAR）、int 类型化数组元素访问（OPC_INT_ARRAY_GET）
static void compile_int_array_elems(Ctx* c, AstNode* e, int* n) {
    if(!e) return;
    if(e->type == AST_SEQ) {
        compile_int_array_elems(c, e->u.seq.first, n);
        compile_int_array_elems(c, e->u.seq.second, n);
        return;
    }
    if(e->type == AST_INDEX) {
        /* 数组访问表达式：编译 arr 和 idx，然后发射 OPC_INT_ARRAY_GET
           运行时会检查是否是 int 类型化数组，如果是直接读取 int 值压入 int 栈，零包装！
           如果不是，回退到普通数组访问（包装成 Value，然后需要转换） */
        AstNode* arr = e->u.index.arr;
        AstNode* idx = e->u.index.idx;
        c_expr(c, arr);
        c_expr(c, idx);
        emit(c, OPC_INT_ARRAY_GET, 0, 0);
        (*n)++;
        return;
    }
    /* 声明为 int 类型的变量：使用 OPC_LOAD_INT_VAR，直接压入 int 栈 */
    int var_idx = bf_sym(c->fn, e->u.varname);
    emit(c, OPC_LOAD_INT_VAR, var_idx, 0);
    (*n)++;
}

// 检查变量是否声明为 double 类型
// 检查变量是否是 double 类型化数组
static int is_double_typed_array_var(Ctx* c, const char* vname) {
    if(!c || !c->fn || !vname) return 0;
    int var_idx = bf_sym(c->fn, vname);
    if(var_idx < 0 || var_idx >= c->fn->sym_cnt) return 0;
    return (c->fn->var_type_tags && c->fn->var_type_tags[var_idx] == VAR_TYPE_DOUBLE_ARRAY);
}

// 检查变量是否是 float 类型化数组
static int is_float_typed_array_var(Ctx* c, const char* vname) {
    if(!c || !c->fn || !vname) return 0;
    int var_idx = bf_sym(c->fn, vname);
    if(var_idx < 0 || var_idx >= c->fn->sym_cnt) return 0;
    return (c->fn->var_type_tags && c->fn->var_type_tags[var_idx] == VAR_TYPE_FLOAT_ARRAY);
}

static int is_double_var(Ctx* c, AstNode* node) {
    if(!node || node->type != AST_VAR) return 0;
    int var_idx = bf_sym(c->fn, node->u.varname);
    if(var_idx < 0 || var_idx >= c->fn->sym_cnt) return 0;
    return (c->fn->var_type_tags && c->fn->var_type_tags[var_idx] == CAST_DOUBLE);
}

// 检查数组所有元素是否都是 double 类型（声明为 double 的变量或 double 类型化数组的元素访问）
static int all_double_vars(Ctx* c, AstNode* e) {
    if(!e) return 1;
    if(e->type == AST_SEQ) {
        return all_double_vars(c, e->u.seq.first) && all_double_vars(c, e->u.seq.second);
    }
    if(e->type == AST_INDEX) {
        /* 数组访问表达式：视为 double 候选，运行时 OPC_DOUBLE_ARRAY_GET 会检查是否是 double 类型化数组 */
        return 1;
    }
    return is_double_var(c, e);
}

// 编译 double 泛型数组元素：全部压入 double 栈（零检查零转换）
// 支持：声明为 double 的变量（OPC_LOAD_DOUBLE_VAR）、double 类型化数组元素访问（OPC_DOUBLE_ARRAY_GET）
static void compile_double_array_elems(Ctx* c, AstNode* e, int* n) {
    if(!e) return;
    if(e->type == AST_SEQ) {
        compile_double_array_elems(c, e->u.seq.first, n);
        compile_double_array_elems(c, e->u.seq.second, n);
        return;
    }
    if(e->type == AST_INDEX) {
        /* 数组访问表达式：编译 arr 和 idx，然后发射 OPC_DOUBLE_ARRAY_GET
           运行时会检查是否是 double 类型化数组，如果是直接读取 double 值压入 double 栈，零包装！
           如果不是，回退到普通数组访问（包装成 Value，然后需要转换） */
        AstNode* arr = e->u.index.arr;
        AstNode* idx = e->u.index.idx;
        c_expr(c, arr);
        c_expr(c, idx);
        emit(c, OPC_DOUBLE_ARRAY_GET, 0, 0);
        (*n)++;
        return;
    }
    /* 声明为 double 类型的变量：使用 OPC_LOAD_DOUBLE_VAR，直接压入 double 栈 */
    int var_idx = bf_sym(c->fn, e->u.varname);
    emit(c, OPC_LOAD_DOUBLE_VAR, var_idx, 0);
    (*n)++;
}

// 检查变量是否声明为 float 类型
static int is_float_var(Ctx* c, AstNode* node) {
    if(!node || node->type != AST_VAR) return 0;
    int var_idx = bf_sym(c->fn, node->u.varname);
    if(var_idx < 0 || var_idx >= c->fn->sym_cnt) return 0;
    return (c->fn->var_type_tags && c->fn->var_type_tags[var_idx] == CAST_FLOAT);
}

/* 表达式类型枚举（用于算术运算结果类型推断，编译期调用，零运行时开销）
   注意：数值大小不完全对应类型优先级，有符号/无符号混合运算需通过expr_type_promote函数处理
   类型提升规则参考C语言标准：浮点 > 整数，64位 > 32位 > 16位 > 8位 */


/* 位宽等级：8位=1, 16位=2, 32位=3, 64位=4, long=5 */
static int width_rank(ExprType t) {
    switch(t) {
        case EXPR_TYPE_BOOL: case EXPR_TYPE_CHAR: case EXPR_TYPE_INT8:
        case EXPR_TYPE_BYTE: case EXPR_TYPE_UINT8: return 1;
        case EXPR_TYPE_INT16: case EXPR_TYPE_UINT16: return 2;
        case EXPR_TYPE_INT: case EXPR_TYPE_UINT: case EXPR_TYPE_SIZE_T:
        case EXPR_TYPE_SSIZE_T: return 3;
        case EXPR_TYPE_INT64: case EXPR_TYPE_UINT64: case EXPR_TYPE_LONG_LONG: return 4;
        case EXPR_TYPE_LONG: case EXPR_TYPE_ULONG: return 5;
        default: return 0;
    }
}
static int is_unsigned_type(ExprType t) {
    switch(t) {
        case EXPR_TYPE_BYTE: case EXPR_TYPE_UINT8: case EXPR_TYPE_UINT16:
        case EXPR_TYPE_UINT: case EXPR_TYPE_UINT64: case EXPR_TYPE_ULONG:
        case EXPR_TYPE_SIZE_T: return 1;
        default: return 0;
    }
}

/* 类型提升辅助函数：根据C语言标准的常用算术转换规则，返回两个类型提升后的结果类型
   简化规则：浮点 > 整数，64位 > 32位 > 16位 > 8位，无符号 > 有符号（相同位宽时） */
static ExprType expr_type_promote(ExprType a, ExprType b) {
    if(a == EXPR_TYPE_NONE || b == EXPR_TYPE_NONE) return EXPR_TYPE_NONE;
    /* 浮点类型优先级最高 */
    int a_is_float = (a == EXPR_TYPE_FLOAT || a == EXPR_TYPE_DOUBLE || a == EXPR_TYPE_LONG_DOUBLE);
    int b_is_float = (b == EXPR_TYPE_FLOAT || b == EXPR_TYPE_DOUBLE || b == EXPR_TYPE_LONG_DOUBLE);
    if(a_is_float || b_is_float) {
        if(a == EXPR_TYPE_LONG_DOUBLE || b == EXPR_TYPE_LONG_DOUBLE) return EXPR_TYPE_LONG_DOUBLE;
        if(a == EXPR_TYPE_DOUBLE || b == EXPR_TYPE_DOUBLE) return EXPR_TYPE_DOUBLE;
        return EXPR_TYPE_FLOAT;
    }
    /* 整数类型：按位宽和符号判断 */
    int wa = width_rank(a), wb = width_rank(b);
    if(wa != wb) return wa > wb ? a : b;
    /* 相同位宽：无符号优先 */
    if(is_unsigned_type(a) != is_unsigned_type(b)) return is_unsigned_type(a) ? a : b;
    return a; /* 相同位宽相同符号，返回任意一个 */
}

/* 判断表达式的类型（用于算术运算结果类型推断，编译期调用，零运行时开销）
   返回ExprType枚举值 */
static ExprType get_expr_type(Ctx* c, AstNode* node) {
    if(!node || !c || !c->fn) return 0;
    if(node->type == AST_VAR) {
        int var_idx = bf_sym(c->fn, node->u.varname);
        if(var_idx < 0 || var_idx >= c->fn->sym_cnt) return 0;
        int tag = c->fn->var_type_tags ? c->fn->var_type_tags[var_idx] : -1;
        /* 有符号整数类型 */
        if(tag == CAST_INT8) return EXPR_TYPE_INT8;
        if(tag == CAST_INT16) return EXPR_TYPE_INT16;
        if(tag == CAST_SHORT) return EXPR_TYPE_SHORT;
        if(tag == CAST_INT || tag == CAST_INT32) return EXPR_TYPE_INT;
        if(tag == CAST_INT64) return EXPR_TYPE_INT64;
        if(tag == CAST_LONGLONG) return EXPR_TYPE_LONG_LONG;
        if(tag == CAST_LONG) return EXPR_TYPE_LONG;
        /* 无符号整数类型 */
        if(tag == CAST_BYTE || tag == CAST_UINT8) return EXPR_TYPE_UINT8;
        if(tag == CAST_UINT16) return EXPR_TYPE_UINT16;
        if(tag == CAST_UINT32) return EXPR_TYPE_UINT;
        if(tag == CAST_UINT64) return EXPR_TYPE_UINT64;
        if(tag == CAST_ULONG) return EXPR_TYPE_ULONG;
        /* 布尔和字符类型 */
        if(tag == CAST_BOOL) return EXPR_TYPE_BOOL;
        if(tag == CAST_CHAR) return EXPR_TYPE_CHAR;
        /* 大小类型 */
        if(tag == CAST_SIZE_T) return EXPR_TYPE_SIZE_T;
        if(tag == CAST_SSIZE_T) return EXPR_TYPE_SSIZE_T;
        /* 浮点类型 */
        if(tag == CAST_FLOAT) return EXPR_TYPE_FLOAT;
        if(tag == CAST_DOUBLE) return EXPR_TYPE_DOUBLE;
        if(tag == CAST_LONG_DOUBLE) return EXPR_TYPE_LONG_DOUBLE;
        return EXPR_TYPE_NONE;
    }
    if(node->type == AST_BINOP) {
        BinOp bop = node->u.bin.op;
        int is_arith = (bop == OP_ADD || bop == OP_SUB || bop == OP_MUL || bop == OP_DIV || bop == OP_MOD);
        if(!is_arith) return 0; /* 比较运算结果是bool，走通用路径 */
        ExprType left_type = get_expr_type(c, node->u.bin.left);
        ExprType right_type = get_expr_type(c, node->u.bin.right);
        if(left_type == EXPR_TYPE_NONE || right_type == EXPR_TYPE_NONE) return EXPR_TYPE_NONE;
        return expr_type_promote(left_type, right_type);
    }
    return 0;
}

// 检查数组所有元素是否都是 float 类型（声明为 float 的变量或 float 类型化数组的元素访问）
static int all_float_vars(Ctx* c, AstNode* e) {
    if(!e) return 1;
    if(e->type == AST_SEQ) {
        return all_float_vars(c, e->u.seq.first) && all_float_vars(c, e->u.seq.second);
    }
    if(e->type == AST_INDEX) {
        /* 数组访问表达式：视为 float 候选，运行时 OPC_FLOAT_ARRAY_GET 会检查是否是 float 类型化数组 */
        return 1;
    }
    return is_float_var(c, e);
}

// 编译 float 泛型数组元素：全部压入 float 栈（零检查零转换）
// 支持：声明为 float 的变量（OPC_LOAD_FLOAT_VAR）、float 类型化数组元素访问（OPC_FLOAT_ARRAY_GET）
static void compile_float_array_elems(Ctx* c, AstNode* e, int* n) {
    if(!e) return;
    if(e->type == AST_SEQ) {
        compile_float_array_elems(c, e->u.seq.first, n);
        compile_float_array_elems(c, e->u.seq.second, n);
        return;
    }
    if(e->type == AST_INDEX) {
        /* 数组访问表达式：编译 arr 和 idx，然后发射 OPC_FLOAT_ARRAY_GET */
        AstNode* arr = e->u.index.arr;
        AstNode* idx = e->u.index.idx;
        c_expr(c, arr);
        c_expr(c, idx);
        emit(c, OPC_FLOAT_ARRAY_GET, 0, 0);
        (*n)++;
        return;
    }
    /* 声明为 float 类型的变量：使用 OPC_LOAD_FLOAT_VAR，直接压入 float 栈 */
    int var_idx = bf_sym(c->fn, e->u.varname);
    emit(c, OPC_LOAD_FLOAT_VAR, var_idx, 0);
    (*n)++;
}

// 检查变量是否声明为 uint 类型
static int is_uint_var(Ctx* c, AstNode* node) {
    if(!node || node->type != AST_VAR) return 0;
    int var_idx = bf_sym(c->fn, node->u.varname);
    if(var_idx < 0 || var_idx >= c->fn->sym_cnt) return 0;
    return (c->fn->var_type_tags && c->fn->var_type_tags[var_idx] == CAST_UINT32);
}

// 检查数组所有元素是否都是 uint 类型（声明为 uint 的变量或 uint 类型化数组的元素访问）
static int all_uint_vars(Ctx* c, AstNode* e) {
    if(!e) return 1;
    if(e->type == AST_SEQ) {
        return all_uint_vars(c, e->u.seq.first) && all_uint_vars(c, e->u.seq.second);
    }
    if(e->type == AST_INDEX) {
        /* 数组访问表达式：视为 uint 候选，运行时 OPC_UINT_ARRAY_GET 会检查是否是 uint 类型化数组 */
        return 1;
    }
    return is_uint_var(c, e);
}

// 编译 uint 泛型数组元素：全部压入 uint 栈（零检查零转换）
// 支持：声明为 uint 的变量（OPC_LOAD_UINT_VAR）、uint 类型化数组元素访问（OPC_UINT_ARRAY_GET）
static void compile_uint_array_elems(Ctx* c, AstNode* e, int* n) {
    if(!e) return;
    if(e->type == AST_SEQ) {
        compile_uint_array_elems(c, e->u.seq.first, n);
        compile_uint_array_elems(c, e->u.seq.second, n);
        return;
    }
    if(e->type == AST_INDEX) {
        /* 数组访问表达式：编译 arr 和 idx，然后发射 OPC_UINT_ARRAY_GET */
        AstNode* arr = e->u.index.arr;
        AstNode* idx = e->u.index.idx;
        c_expr(c, arr);
        c_expr(c, idx);
        emit(c, OPC_UINT_ARRAY_GET, 0, 0);
        (*n)++;
        return;
    }
    /* 声明为 uint 类型的变量：使用 OPC_LOAD_UINT_VAR，直接压入 uint 栈 */
    int var_idx = bf_sym(c->fn, e->u.varname);
    emit(c, OPC_LOAD_UINT_VAR, var_idx, 0);
    (*n)++;
}

// 检查变量是否声明为 long long 类型
static int is_long_long_var(Ctx* c, AstNode* node) {
    if(!node || node->type != AST_VAR) return 0;
    int var_idx = bf_sym(c->fn, node->u.varname);
    if(var_idx < 0 || var_idx >= c->fn->sym_cnt) return 0;
    return (c->fn->var_type_tags && c->fn->var_type_tags[var_idx] == CAST_LONGLONG);
}

// 检查数组所有元素是否都是 long long 类型（声明为 long long 的变量或 long long 类型化数组的元素访问）
static int all_long_long_vars(Ctx* c, AstNode* e) {
    if(!e) return 1;
    if(e->type == AST_SEQ) {
        return all_long_long_vars(c, e->u.seq.first) && all_long_long_vars(c, e->u.seq.second);
    }
    if(e->type == AST_INDEX) {
        /* 数组访问表达式：视为 long long 候选，运行时 OPC_LONG_LONG_ARRAY_GET 会检查是否是 long long 类型化数组 */
        return 1;
    }
    return is_long_long_var(c, e);
}

// 编译 long long 泛型数组元素：全部压入 long long 栈（零检查零转换）
// 支持：声明为 long long 的变量（OPC_LOAD_LONG_LONG_VAR）、long long 类型化数组元素访问（OPC_LONG_LONG_ARRAY_GET）
static void compile_long_long_array_elems(Ctx* c, AstNode* e, int* n) {
    if(!e) return;
    if(e->type == AST_SEQ) {
        compile_long_long_array_elems(c, e->u.seq.first, n);
        compile_long_long_array_elems(c, e->u.seq.second, n);
        return;
    }
    if(e->type == AST_INDEX) {
        /* 数组访问表达式：编译 arr 和 idx，然后发射 OPC_LONG_LONG_ARRAY_GET */
        AstNode* arr = e->u.index.arr;
        AstNode* idx = e->u.index.idx;
        c_expr(c, arr);
        c_expr(c, idx);
        emit(c, OPC_LONG_LONG_ARRAY_GET, 0, 0);
        (*n)++;
        return;
    }
    /* 声明为 long long 类型的变量：使用 OPC_LOAD_LONG_LONG_VAR，直接压入 long long 栈 */
    int var_idx = bf_sym(c->fn, e->u.varname);
    emit(c, OPC_LOAD_LONG_LONG_VAR, var_idx, 0);
    (*n)++;
}

// 检查变量是否声明为 bool 类型
static int is_bool_var(Ctx* c, AstNode* node) {
    if(!node || node->type != AST_VAR) return 0;
    int var_idx = bf_sym(c->fn, node->u.varname);
    if(var_idx < 0 || var_idx >= c->fn->sym_cnt) return 0;
    return (c->fn->var_type_tags && c->fn->var_type_tags[var_idx] == CAST_BOOL);
}

// 检查数组所有元素是否都是 bool 类型
static int all_bool_vars(Ctx* c, AstNode* e) {
    if(!e) return 1;
    if(e->type == AST_SEQ) {
        return all_bool_vars(c, e->u.seq.first) && all_bool_vars(c, e->u.seq.second);
    }
    if(e->type == AST_INDEX) {
        return 1;
    }
    return is_bool_var(c, e);
}

// 编译 bool 泛型数组元素：全部压入 bool 栈
static void compile_bool_array_elems(Ctx* c, AstNode* e, int* n) {
    if(!e) return;
    if(e->type == AST_SEQ) {
        compile_bool_array_elems(c, e->u.seq.first, n);
        compile_bool_array_elems(c, e->u.seq.second, n);
        return;
    }
    if(e->type == AST_INDEX) {
        AstNode* arr = e->u.index.arr;
        AstNode* idx = e->u.index.idx;
        c_expr(c, arr);
        c_expr(c, idx);
        emit(c, OPC_BOOL_ARRAY_GET, 0, 0);
        (*n)++;
        return;
    }
    int var_idx = bf_sym(c->fn, e->u.varname);
    emit(c, OPC_LOAD_BOOL_VAR, var_idx, 0);
    (*n)++;
}

// 检查变量是否声明为 char 类型
static int is_char_var(Ctx* c, AstNode* node) {
    if(!node || node->type != AST_VAR) return 0;
    int var_idx = bf_sym(c->fn, node->u.varname);
    if(var_idx < 0 || var_idx >= c->fn->sym_cnt) return 0;
    return (c->fn->var_type_tags && c->fn->var_type_tags[var_idx] == CAST_CHAR);
}

// 检查数组所有元素是否都是 char 类型
static int all_char_vars(Ctx* c, AstNode* e) {
    if(!e) return 1;
    if(e->type == AST_SEQ) {
        return all_char_vars(c, e->u.seq.first) && all_char_vars(c, e->u.seq.second);
    }
    if(e->type == AST_INDEX) {
        return 1;
    }
    return is_char_var(c, e);
}

// 编译 char 泛型数组元素：全部压入 char 栈
static void compile_char_array_elems(Ctx* c, AstNode* e, int* n) {
    if(!e) return;
    if(e->type == AST_SEQ) {
        compile_char_array_elems(c, e->u.seq.first, n);
        compile_char_array_elems(c, e->u.seq.second, n);
        return;
    }
    if(e->type == AST_INDEX) {
        AstNode* arr = e->u.index.arr;
        AstNode* idx = e->u.index.idx;
        c_expr(c, arr);
        c_expr(c, idx);
        emit(c, OPC_CHAR_ARRAY_GET, 0, 0);
        (*n)++;
        return;
    }
    int var_idx = bf_sym(c->fn, e->u.varname);
    emit(c, OPC_LOAD_CHAR_VAR, var_idx, 0);
    (*n)++;
}

// 检查变量是否声明为 byte 类型
static int is_byte_var(Ctx* c, AstNode* node) {
    if(!node || node->type != AST_VAR) return 0;
    int var_idx = bf_sym(c->fn, node->u.varname);
    if(var_idx < 0 || var_idx >= c->fn->sym_cnt) return 0;
    return (c->fn->var_type_tags && c->fn->var_type_tags[var_idx] == CAST_BYTE);
}

// 检查数组所有元素是否都是 byte 类型
static int all_byte_vars(Ctx* c, AstNode* e) {
    if(!e) return 1;
    if(e->type == AST_SEQ) {
        return all_byte_vars(c, e->u.seq.first) && all_byte_vars(c, e->u.seq.second);
    }
    if(e->type == AST_INDEX) {
        return 1;
    }
    return is_byte_var(c, e);
}

// 编译 byte 泛型数组元素：全部压入 byte 栈
static void compile_byte_array_elems(Ctx* c, AstNode* e, int* n) {
    if(!e) return;
    if(e->type == AST_SEQ) {
        compile_byte_array_elems(c, e->u.seq.first, n);
        compile_byte_array_elems(c, e->u.seq.second, n);
        return;
    }
    if(e->type == AST_INDEX) {
        AstNode* arr = e->u.index.arr;
        AstNode* idx = e->u.index.idx;
        c_expr(c, arr);
        c_expr(c, idx);
        emit(c, OPC_BYTE_ARRAY_GET, 0, 0);
        (*n)++;
        return;
    }
    int var_idx = bf_sym(c->fn, e->u.varname);
    emit(c, OPC_LOAD_BYTE_VAR, var_idx, 0);
    (*n)++;
}

// 检测是否是 int8 类型变量
static int is_int8_var(Ctx* c, AstNode* node) {
    if(node->type != AST_VAR) return 0;
    int var_idx = bf_sym(c->fn, node->u.varname);
    if(var_idx < 0) return 0;
    return (c->fn->var_type_tags && c->fn->var_type_tags[var_idx] == CAST_INT8);
}

// 检测是否全部是 int8 类型变量或数组元素访问
static int all_int8_vars(Ctx* c, AstNode* e) {
    if(!e) return 1;
    if(e->type == AST_SEQ) {
        return all_int8_vars(c, e->u.seq.first) && all_int8_vars(c, e->u.seq.second);
    }
    if(e->type == AST_INDEX) {
        /* 数组访问表达式：视为 int8 候选，运行时 OPC_INT8_ARRAY_GET 会检查是否是 int8 类型化数组 */
        return 1;
    }
    return is_int8_var(c, e);
}

// 编译 int8 泛型数组元素：全部压入 int8 栈
static void compile_int8_array_elems(Ctx* c, AstNode* e, int* n) {
    if(!e) return;
    if(e->type == AST_SEQ) {
        compile_int8_array_elems(c, e->u.seq.first, n);
        compile_int8_array_elems(c, e->u.seq.second, n);
        return;
    }
    if(e->type == AST_INDEX) {
        AstNode* arr = e->u.index.arr;
        AstNode* idx = e->u.index.idx;
        c_expr(c, arr);
        c_expr(c, idx);
        emit(c, OPC_INT8_ARRAY_GET, 0, 0);
        (*n)++;
        return;
    }
    int var_idx = bf_sym(c->fn, e->u.varname);
    emit(c, OPC_LOAD_INT8_VAR, var_idx, 0);
    (*n)++;
}

// 检查变量是否声明为 int16 类型
static int is_int16_var(Ctx* c, AstNode* node) {
    if(node->type != AST_VAR) return 0;
    int var_idx = bf_sym(c->fn, node->u.varname);
    if(var_idx < 0) return 0;
    return (c->fn->var_type_tags && c->fn->var_type_tags[var_idx] == CAST_INT16);
}

// 检测是否全部是 int16 类型变量或数组元素访问
static int all_int16_vars(Ctx* c, AstNode* e) {
    if(!e) return 1;
    if(e->type == AST_SEQ) {
        return all_int16_vars(c, e->u.seq.first) && all_int16_vars(c, e->u.seq.second);
    }
    if(e->type == AST_INDEX) {
        /* 数组访问表达式：视为 int16 候选，运行时 OPC_INT16_ARRAY_GET 会检查是否是 int16 类型化数组 */
        return 1;
    }
    return is_int16_var(c, e);
}

// 编译 int16 泛型数组元素：全部压入 int16 栈
static void compile_int16_array_elems(Ctx* c, AstNode* e, int* n) {
    if(!e) return;
    if(e->type == AST_SEQ) {
        compile_int16_array_elems(c, e->u.seq.first, n);
        compile_int16_array_elems(c, e->u.seq.second, n);
        return;
    }
    if(e->type == AST_INDEX) {
        AstNode* arr = e->u.index.arr;
        AstNode* idx = e->u.index.idx;
        c_expr(c, arr);
        c_expr(c, idx);
        emit(c, OPC_INT16_ARRAY_GET, 0, 0);
        (*n)++;
        return;
    }
    int var_idx = bf_sym(c->fn, e->u.varname);
    emit(c, OPC_LOAD_INT16_VAR, var_idx, 0);
    (*n)++;
}

// 检查变量是否声明为 int32 类型
static int is_int32_var(Ctx* c, AstNode* node) {
    if(node->type != AST_VAR) return 0;
    int var_idx = bf_sym(c->fn, node->u.varname);
    if(var_idx < 0) return 0;
    return (c->fn->var_type_tags && c->fn->var_type_tags[var_idx] == CAST_INT32);
}

// 检测是否全部是 int32 类型变量或数组元素访问
static int all_int32_vars(Ctx* c, AstNode* e) {
    if(!e) return 1;
    if(e->type == AST_SEQ) {
        return all_int32_vars(c, e->u.seq.first) && all_int32_vars(c, e->u.seq.second);
    }
    if(e->type == AST_INDEX) {
        /* 数组访问表达式：视为 int32 候选，运行时 OPC_INT32_ARRAY_GET 会检查是否是 int32 类型化数组 */
        return 1;
    }
    return is_int32_var(c, e);
}

// 编译 int32 泛型数组元素：全部压入 int32 栈
static void compile_int32_array_elems(Ctx* c, AstNode* e, int* n) {
    if(!e) return;
    if(e->type == AST_SEQ) {
        compile_int32_array_elems(c, e->u.seq.first, n);
        compile_int32_array_elems(c, e->u.seq.second, n);
        return;
    }
    if(e->type == AST_INDEX) {
        AstNode* arr = e->u.index.arr;
        AstNode* idx = e->u.index.idx;
        c_expr(c, arr);
        c_expr(c, idx);
        emit(c, OPC_INT32_ARRAY_GET, 0, 0);
        (*n)++;
        return;
    }
    int var_idx = bf_sym(c->fn, e->u.varname);
    emit(c, OPC_LOAD_INT32_VAR, var_idx, 0);
    (*n)++;
}

// 检查变量是否声明为 int64 类型
static int is_int64_var(Ctx* c, AstNode* node) {
    if(node->type != AST_VAR) return 0;
    int var_idx = bf_sym(c->fn, node->u.varname);
    if(var_idx < 0) return 0;
    return (c->fn->var_type_tags && c->fn->var_type_tags[var_idx] == CAST_INT64);
}

// 检测是否全部是 int64 类型变量或数组元素访问
static int all_int64_vars(Ctx* c, AstNode* e) {
    if(!e) return 1;
    if(e->type == AST_SEQ) {
        return all_int64_vars(c, e->u.seq.first) && all_int64_vars(c, e->u.seq.second);
    }
    if(e->type == AST_INDEX) {
        /* 数组访问表达式：视为 int64 候选，运行时 OPC_INT64_ARRAY_GET 会检查是否是 int64 类型化数组 */
        return 1;
    }
    return is_int64_var(c, e);
}

// 编译 int64 泛型数组元素：全部压入 int64 栈
static void compile_int64_array_elems(Ctx* c, AstNode* e, int* n) {
    if(!e) return;
    if(e->type == AST_SEQ) {
        compile_int64_array_elems(c, e->u.seq.first, n);
        compile_int64_array_elems(c, e->u.seq.second, n);
        return;
    }
    if(e->type == AST_INDEX) {
        AstNode* arr = e->u.index.arr;
        AstNode* idx = e->u.index.idx;
        c_expr(c, arr);
        c_expr(c, idx);
        emit(c, OPC_INT64_ARRAY_GET, 0, 0);
        (*n)++;
        return;
    }
    int var_idx = bf_sym(c->fn, e->u.varname);
    emit(c, OPC_LOAD_INT64_VAR, var_idx, 0);
    (*n)++;
}

// 检查变量是否声明为 uint8 类型
static int is_uint8_var(Ctx* c, AstNode* node) {
    if(node->type != AST_VAR) return 0;
    int var_idx = bf_sym(c->fn, node->u.varname);
    if(var_idx < 0) return 0;
    return (c->fn->var_type_tags && c->fn->var_type_tags[var_idx] == CAST_UINT8);
}

// 检测是否全部是 uint8 类型变量或数组元素访问
static int all_uint8_vars(Ctx* c, AstNode* e) {
    if(!e) return 1;
    if(e->type == AST_SEQ) {
        return all_uint8_vars(c, e->u.seq.first) && all_uint8_vars(c, e->u.seq.second);
    }
    if(e->type == AST_INDEX) {
        /* 数组访问表达式：视为 uint8 候选，运行时 OPC_UINT8_ARRAY_GET 会检查是否是 uint8 类型化数组 */
        return 1;
    }
    return is_uint8_var(c, e);
}

// 编译 uint8 泛型数组元素：全部压入 uint8 栈
static void compile_uint8_array_elems(Ctx* c, AstNode* e, int* n) {
    if(!e) return;
    if(e->type == AST_SEQ) {
        compile_uint8_array_elems(c, e->u.seq.first, n);
        compile_uint8_array_elems(c, e->u.seq.second, n);
        return;
    }
    if(e->type == AST_INDEX) {
        AstNode* arr = e->u.index.arr;
        AstNode* idx = e->u.index.idx;
        c_expr(c, arr);
        c_expr(c, idx);
        emit(c, OPC_UINT8_ARRAY_GET, 0, 0);
        (*n)++;
        return;
    }
    int var_idx = bf_sym(c->fn, e->u.varname);
    emit(c, OPC_LOAD_UINT8_VAR, var_idx, 0);
    (*n)++;
}

// 检查变量是否声明为 uint16 类型
static int is_uint16_var(Ctx* c, AstNode* node) {
    if(node->type != AST_VAR) return 0;
    int var_idx = bf_sym(c->fn, node->u.varname);
    if(var_idx < 0) return 0;
    return (c->fn->var_type_tags && c->fn->var_type_tags[var_idx] == CAST_UINT16);
}

// 检测是否全部是 uint16 类型变量或数组元素访问
static int all_uint16_vars(Ctx* c, AstNode* e) {
    if(!e) return 1;
    if(e->type == AST_SEQ) {
        return all_uint16_vars(c, e->u.seq.first) && all_uint16_vars(c, e->u.seq.second);
    }
    if(e->type == AST_INDEX) {
        /* 数组访问表达式：视为 uint16 候选，运行时 OPC_UINT16_ARRAY_GET 会检查是否是 uint16 类型化数组 */
        return 1;
    }
    return is_uint16_var(c, e);
}

// 编译 uint16 泛型数组元素：全部压入 uint16 栈
static void compile_uint16_array_elems(Ctx* c, AstNode* e, int* n) {
    if(!e) return;
    if(e->type == AST_SEQ) {
        compile_uint16_array_elems(c, e->u.seq.first, n);
        compile_uint16_array_elems(c, e->u.seq.second, n);
        return;
    }
    if(e->type == AST_INDEX) {
        AstNode* arr = e->u.index.arr;
        AstNode* idx = e->u.index.idx;
        c_expr(c, arr);
        c_expr(c, idx);
        emit(c, OPC_UINT16_ARRAY_GET, 0, 0);
        (*n)++;
        return;
    }
    int var_idx = bf_sym(c->fn, e->u.varname);
    emit(c, OPC_LOAD_UINT16_VAR, var_idx, 0);
    (*n)++;
}

// 检查变量是否声明为 uint32 类型
static int is_uint32_var(Ctx* c, AstNode* node) {
    if(node->type != AST_VAR) return 0;
    int var_idx = bf_sym(c->fn, node->u.varname);
    if(var_idx < 0) return 0;
    return (c->fn->var_type_tags && c->fn->var_type_tags[var_idx] == CAST_UINT32);
}

// 检测是否全部是 uint32 类型变量或数组元素访问
static int all_uint32_vars(Ctx* c, AstNode* e) {
    if(!e) return 1;
    if(e->type == AST_SEQ) {
        return all_uint32_vars(c, e->u.seq.first) && all_uint32_vars(c, e->u.seq.second);
    }
    if(e->type == AST_INDEX) {
        /* 数组访问表达式：视为 uint32 候选，运行时 OPC_UINT32_ARRAY_GET 会检查是否是 uint32 类型化数组 */
        return 1;
    }
    return is_uint32_var(c, e);
}

// 编译 uint32 泛型数组元素：全部压入 uint32 栈
static void compile_uint32_array_elems(Ctx* c, AstNode* e, int* n) {
    if(!e) return;
    if(e->type == AST_SEQ) {
        compile_uint32_array_elems(c, e->u.seq.first, n);
        compile_uint32_array_elems(c, e->u.seq.second, n);
        return;
    }
    if(e->type == AST_INDEX) {
        AstNode* arr = e->u.index.arr;
        AstNode* idx = e->u.index.idx;
        c_expr(c, arr);
        c_expr(c, idx);
        emit(c, OPC_UINT32_ARRAY_GET, 0, 0);
        (*n)++;
        return;
    }
    int var_idx = bf_sym(c->fn, e->u.varname);
    emit(c, OPC_LOAD_UINT32_VAR, var_idx, 0);
    (*n)++;
}

// 检查变量是否声明为 uint64 类型
static int is_uint64_var(Ctx* c, AstNode* node) {
    if(node->type != AST_VAR) return 0;
    int var_idx = bf_sym(c->fn, node->u.varname);
    if(var_idx < 0) return 0;
    return (c->fn->var_type_tags && c->fn->var_type_tags[var_idx] == CAST_UINT64);
}

// 检测是否全部是 uint64 类型变量或数组元素访问
static int all_uint64_vars(Ctx* c, AstNode* e) {
    if(!e) return 1;
    if(e->type == AST_SEQ) {
        return all_uint64_vars(c, e->u.seq.first) && all_uint64_vars(c, e->u.seq.second);
    }
    if(e->type == AST_INDEX) {
        /* 数组访问表达式：视为 uint64 候选，运行时 OPC_UINT64_ARRAY_GET 会检查是否是 uint64 类型化数组 */
        return 1;
    }
    return is_uint64_var(c, e);
}

// 编译 uint64 泛型数组元素：全部压入 uint64 栈
static void compile_uint64_array_elems(Ctx* c, AstNode* e, int* n) {
    if(!e) return;
    if(e->type == AST_SEQ) {
        compile_uint64_array_elems(c, e->u.seq.first, n);
        compile_uint64_array_elems(c, e->u.seq.second, n);
        return;
    }
    if(e->type == AST_INDEX) {
        AstNode* arr = e->u.index.arr;
        AstNode* idx = e->u.index.idx;
        c_expr(c, arr);
        c_expr(c, idx);
        emit(c, OPC_UINT64_ARRAY_GET, 0, 0);
        (*n)++;
        return;
    }
    int var_idx = bf_sym(c->fn, e->u.varname);
    emit(c, OPC_LOAD_UINT64_VAR, var_idx, 0);
    (*n)++;
}

// 检查变量是否声明为 long 类型
static int is_long_var(Ctx* c, AstNode* node) {
    if(node->type != AST_VAR) return 0;
    int var_idx = bf_sym(c->fn, node->u.varname);
    if(var_idx < 0) return 0;
    return (c->fn->var_type_tags && c->fn->var_type_tags[var_idx] == CAST_LONG);
}

// 检测是否全部是 long 类型变量或数组元素访问
static int all_long_vars(Ctx* c, AstNode* e) {
    if(!e) return 1;
    if(e->type == AST_SEQ) {
        return all_long_vars(c, e->u.seq.first) && all_long_vars(c, e->u.seq.second);
    }
    if(e->type == AST_INDEX) {
        /* 数组访问表达式：视为 long 候选，运行时 OPC_LONG_ARRAY_GET 会检查是否是 long 类型化数组 */
        return 1;
    }
    return is_long_var(c, e);
}

// 编译 long 泛型数组元素：全部压入 long 栈
static void compile_long_array_elems(Ctx* c, AstNode* e, int* n) {
    if(!e) return;
    if(e->type == AST_SEQ) {
        compile_long_array_elems(c, e->u.seq.first, n);
        compile_long_array_elems(c, e->u.seq.second, n);
        return;
    }
    if(e->type == AST_INDEX) {
        AstNode* arr = e->u.index.arr;
        AstNode* idx = e->u.index.idx;
        c_expr(c, arr);
        c_expr(c, idx);
        emit(c, OPC_LONG_ARRAY_GET, 0, 0);
        (*n)++;
        return;
    }
    int var_idx = bf_sym(c->fn, e->u.varname);
    emit(c, OPC_LOAD_LONG_VAR, var_idx, 0);
    (*n)++;
}

// 检查变量是否声明为 unsigned long 类型
static int is_ulong_var(Ctx* c, AstNode* node) {
    if(node->type != AST_VAR) return 0;
    int var_idx = bf_sym(c->fn, node->u.varname);
    if(var_idx < 0) return 0;
    return (c->fn->var_type_tags && c->fn->var_type_tags[var_idx] == CAST_ULONG);
}

// 检测是否全部是 unsigned long 类型变量或数组元素访问
static int all_ulong_vars(Ctx* c, AstNode* e) {
    if(!e) return 1;
    if(e->type == AST_SEQ) {
        return all_ulong_vars(c, e->u.seq.first) && all_ulong_vars(c, e->u.seq.second);
    }
    if(e->type == AST_INDEX) {
        /* 数组访问表达式：视为 unsigned long 候选，运行时 OPC_ULONG_ARRAY_GET 会检查是否是 unsigned long 类型化数组 */
        return 1;
    }
    return is_ulong_var(c, e);
}

// 编译 unsigned long 泛型数组元素：全部压入 unsigned long 栈
static void compile_ulong_array_elems(Ctx* c, AstNode* e, int* n) {
    if(!e) return;
    if(e->type == AST_SEQ) {
        compile_ulong_array_elems(c, e->u.seq.first, n);
        compile_ulong_array_elems(c, e->u.seq.second, n);
        return;
    }
    if(e->type == AST_INDEX) {
        AstNode* arr = e->u.index.arr;
        AstNode* idx = e->u.index.idx;
        c_expr(c, arr);
        c_expr(c, idx);
        emit(c, OPC_ULONG_ARRAY_GET, 0, 0);
        (*n)++;
        return;
    }
    int var_idx = bf_sym(c->fn, e->u.varname);
    emit(c, OPC_LOAD_ULONG_VAR, var_idx, 0);
    (*n)++;
}

// 检查变量是否声明为 size_t 类型
static int is_size_t_var(Ctx* c, AstNode* node) {
    if(node->type != AST_VAR) return 0;
    int var_idx = bf_sym(c->fn, node->u.varname);
    if(var_idx < 0) return 0;
    return (c->fn->var_type_tags && c->fn->var_type_tags[var_idx] == CAST_SIZE_T);
}

// 检测是否全部是 size_t 类型变量或数组元素访问
static int all_size_t_vars(Ctx* c, AstNode* e) {
    if(!e) return 1;
    if(e->type == AST_SEQ) {
        return all_size_t_vars(c, e->u.seq.first) && all_size_t_vars(c, e->u.seq.second);
    }
    if(e->type == AST_INDEX) {
        /* 数组访问表达式：视为 size_t 候选，运行时 OPC_SIZE_T_ARRAY_GET 会检查是否是 size_t 类型化数组 */
        return 1;
    }
    return is_size_t_var(c, e);
}

// 编译 size_t 泛型数组元素：全部压入 size_t 栈
static void compile_size_t_array_elems(Ctx* c, AstNode* e, int* n) {
    if(!e) return;
    if(e->type == AST_SEQ) {
        compile_size_t_array_elems(c, e->u.seq.first, n);
        compile_size_t_array_elems(c, e->u.seq.second, n);
        return;
    }
    if(e->type == AST_INDEX) {
        AstNode* arr = e->u.index.arr;
        AstNode* idx = e->u.index.idx;
        c_expr(c, arr);
        c_expr(c, idx);
        emit(c, OPC_SIZE_T_ARRAY_GET, 0, 0);
        (*n)++;
        return;
    }
    int var_idx = bf_sym(c->fn, e->u.varname);
    emit(c, OPC_LOAD_SIZE_T_VAR, var_idx, 0);
    (*n)++;
}

// 检查变量是否声明为 ssize_t 类型
static int is_ssize_t_var(Ctx* c, AstNode* node) {
    if(node->type != AST_VAR) return 0;
    int var_idx = bf_sym(c->fn, node->u.varname);
    if(var_idx < 0) return 0;
    return (c->fn->var_type_tags && c->fn->var_type_tags[var_idx] == CAST_SSIZE_T);
}

// 检测是否全部是 ssize_t 类型变量或数组元素访问
static int all_ssize_t_vars(Ctx* c, AstNode* e) {
    if(!e) return 1;
    if(e->type == AST_SEQ) {
        return all_ssize_t_vars(c, e->u.seq.first) && all_ssize_t_vars(c, e->u.seq.second);
    }
    if(e->type == AST_INDEX) {
        /* 数组访问表达式：视为 ssize_t 候选，运行时 OPC_SSIZE_T_ARRAY_GET 会检查是否是 ssize_t 类型化数组 */
        return 1;
    }
    return is_ssize_t_var(c, e);
}

// 编译 ssize_t 泛型数组元素：全部压入 ssize_t 栈
static void compile_ssize_t_array_elems(Ctx* c, AstNode* e, int* n) {
    if(!e) return;
    if(e->type == AST_SEQ) {
        compile_ssize_t_array_elems(c, e->u.seq.first, n);
        compile_ssize_t_array_elems(c, e->u.seq.second, n);
        return;
    }
    if(e->type == AST_INDEX) {
        AstNode* arr = e->u.index.arr;
        AstNode* idx = e->u.index.idx;
        c_expr(c, arr);
        c_expr(c, idx);
        emit(c, OPC_SSIZE_T_ARRAY_GET, 0, 0);
        (*n)++;
        return;
    }
    int var_idx = bf_sym(c->fn, e->u.varname);
    emit(c, OPC_LOAD_SSIZE_T_VAR, var_idx, 0);
    (*n)++;
}

// 检查变量是否声明为 long double 类型
static int is_long_double_var(Ctx* c, AstNode* node) {
    if(node->type != AST_VAR) return 0;
    int var_idx = bf_sym(c->fn, node->u.varname);
    if(var_idx < 0) return 0;
    return (c->fn->var_type_tags && c->fn->var_type_tags[var_idx] == CAST_LONG_DOUBLE);
}

// 检测是否全部是 long double 类型变量或数组元素访问
static int all_long_double_vars(Ctx* c, AstNode* e) {
    if(!e) return 1;
    if(e->type == AST_SEQ) {
        return all_long_double_vars(c, e->u.seq.first) && all_long_double_vars(c, e->u.seq.second);
    }
    if(e->type == AST_INDEX) {
        /* 数组访问表达式：视为 long double 候选，运行时 OPC_LONG_DOUBLE_ARRAY_GET 会检查是否是 long double 类型化数组 */
        return 1;
    }
    return is_long_double_var(c, e);
}

// 编译 long double 泛型数组元素：全部压入 long double 栈
static void compile_long_double_array_elems(Ctx* c, AstNode* e, int* n) {
    if(!e) return;
    if(e->type == AST_SEQ) {
        compile_long_double_array_elems(c, e->u.seq.first, n);
        compile_long_double_array_elems(c, e->u.seq.second, n);
        return;
    }
    if(e->type == AST_INDEX) {
        AstNode* arr = e->u.index.arr;
        AstNode* idx = e->u.index.idx;
        c_expr(c, arr);
        c_expr(c, idx);
        emit(c, OPC_LONG_DOUBLE_ARRAY_GET, 0, 0);
        (*n)++;
        return;
    }
    int var_idx = bf_sym(c->fn, e->u.varname);
    emit(c, OPC_LOAD_LONG_DOUBLE_VAR, var_idx, 0);
    (*n)++;
}

// 递归检测 AST_SEQ 树中是否含 AST_SPREAD
static int has_spread_node(AstNode* e) {
    if(!e) return 0;
    if(e->type == AST_SPREAD) return 1;
    if(e->type == AST_SEQ) return has_spread_node(e->u.seq.first) || has_spread_node(e->u.seq.second);
    return 0;
}
// 递归编译数组字面量元素（含 spread）：栈顶保持为当前数组
static void compile_array_elems(Ctx* c, AstNode* e) {
    if(!e) return;
    if(e->type == AST_SEQ) {
        compile_array_elems(c, e->u.seq.first);
        compile_array_elems(c, e->u.seq.second);
        return;
    }
    if(e->type == AST_SPREAD) {
        c_expr(c, e->u.spread.expr);
        emit(c, OPC_BUILTIN, BUILTIN_ARRAY_ADDALL, 2);
    } else {
        c_expr(c, e);
        emit(c, OPC_BUILTIN, BUILTIN_ARRAY_ADD, 2);
    }
}
// 递归编译 map 字面量条目（含 spread）
// 约定：入口时栈顶为正在构建的 map；出口时栈顶仍为该 map
static void compile_map_entries_spread(Ctx* c, AstNode* e) {
    if(!e) return;
    if(e->type == AST_SEQ) {
        compile_map_entries_spread(c, e->u.seq.first);
        compile_map_entries_spread(c, e->u.seq.second);
        return;
    }
    if(e->type == AST_SPREAD) {
        c_expr(c, e->u.spread.expr);
        emit(c, OPC_BUILTIN, BUILTIN_ARRAY_ADDALL, 2);
    } else {
        // OPC_INDEX_SET 返回被设置的值而非 map，因此先 DUP map，
        // 设置后 POP 掉返回值，保留原 map 在栈顶
        emit(c, OPC_DUP, 0, 0);
        c_expr(c, e->u.map_entry.key);
        c_expr(c, e->u.map_entry.value);
        emit(c, OPC_INDEX_SET, 0, 0);
        emit(c, OPC_POP, 0, 0);
    }
}
// 字典字面量项递归展开：AST_SEQ 链 / AST_MAP_ENTRY 单节点
static void c_map_entries(Ctx* c, AstNode* e, int* n) {
    if(!e) return;
    if(e->type == AST_SEQ) {
        c_map_entries(c, e->u.seq.first, n);
        c_map_entries(c, e->u.seq.second, n);
        return;
    }
    c_expr(c, e->u.map_entry.key);
    c_expr(c, e->u.map_entry.value);
    (*n)++;
}

// ---------------- 常量折叠 ----------------
// 纯字面量表达式在编译期求值（调用运行时 lumyr_*，语义与执行期一致）。
// 除零不折叠（保留运行期错误行为）。

static int fold_lit(AstNode* node, Value* out)
{
    switch(node->type) {
        case AST_INT:    *out = lumyr_make_long_long(node->u.inum); return 1;  /* inum是long long，避免大数截断 */
        case AST_NUM:    *out = lumyr_make_double(node->u.num); return 1;
        case AST_BOOL:   *out = lumyr_make_bool(node->u.bval ? 1 : 0); return 1;
        case AST_CHAR:   *out = lumyr_make_char(node->u.ch); return 1;
        case AST_STRING: *out = lumyr_make_string(node->u.sval); return 1;
        default:         return 0;
    }
}

static int fold_const(Ctx* c, AstNode* node, Value* out)
{
    (void)c;
    switch(node->type) {
        case AST_INT: case AST_NUM: case AST_BOOL: case AST_CHAR: case AST_STRING:
            return fold_lit(node, out);
        case AST_BINOP: {
            Value l, r;
            if(!fold_const(c, node->u.bin.left, &l)) return 0;
            if(!fold_const(c, node->u.bin.right, &r)) return 0;
            switch(node->u.bin.op) {
                case OP_ADD: *out = lumyr_add(l, r); return 1;
                case OP_SUB: *out = lumyr_sub(l, r); return 1;
                case OP_MUL: *out = lumyr_mul(l, r); return 1;
                case OP_DIV:
                    if((r.type == VAL_INT && r.v.i != 0) || (r.type == VAL_DOUBLE && r.v.d != 0.0)) {
                        *out = lumyr_div(l, r);
                        return 1;
                    }
                    return 0;
                case OP_GT: *out = lumyr_gt(l, r); return 1;
                case OP_LT: *out = lumyr_lt(l, r); return 1;
                case OP_GE: *out = lumyr_ge(l, r); return 1;
                case OP_LE: *out = lumyr_le(l, r); return 1;
                case OP_EQ: *out = lumyr_eq(l, r); return 1;
                case OP_NE: *out = lumyr_ne(l, r); return 1;
                case OP_MOD:
                    if((r.type == VAL_INT && r.v.i != 0) || (r.type == VAL_DOUBLE && r.v.d != 0.0)) {
                        *out = lumyr_mod(l, r);
                        return 1;
                    }
                    return 0;
                case OP_LOGIC_AND:
                    *out = lumyr_make_bool(lumyr_to_bool(l) && lumyr_to_bool(r));
                    return 1;
                case OP_LOGIC_OR:
                    *out = lumyr_make_bool(lumyr_to_bool(l) || lumyr_to_bool(r));
                    return 1;
                default: return 0;
            }
        }
        case AST_UNARY: {
            Value v;
            if(!fold_const(c, node->u.uny.child, &v)) return 0;
            switch(node->u.uny.op) {
                case OP_UNARY_PLUS:  *out = lumyr_unary_plus(v); return 1;
                case OP_UNARY_MINUS: *out = lumyr_unary_minus(v); return 1;
                case OP_LOGIC_NOT:   *out = lumyr_logic_not(v); return 1;
                default: return 0;
            }
        }
        case AST_CAST: {
            Value v;
            if(!fold_const(c, node->u.cast.child, &v)) return 0;
            switch(node->u.cast.cast_type) {
                case CAST_INT:    *out = lumyr_cast_int(v); return 1;
                case CAST_DOUBLE: *out = lumyr_cast_double(v); return 1;
                case CAST_CHAR:   *out = lumyr_cast_char(v); return 1;
                case CAST_BOOL:   *out = lumyr_cast_bool(v); return 1;
                case CAST_STRING: *out = lumyr_cast_string(v); return 1;
                case CAST_ASCII:  *out = lumyr_cast_ascii(v); return 1;
                case CAST_BYTE:   *out = lumyr_cast_byte(v); return 1;
                case CAST_INT8:   *out = lumyr_cast_int8(v); return 1;
                case CAST_INT16:  *out = lumyr_cast_int16(v); return 1;
                case CAST_INT32:  *out = lumyr_cast_int32(v); return 1;
                case CAST_INT64:  *out = lumyr_cast_int64(v); return 1;
                case CAST_UINT8:  *out = lumyr_cast_uint8(v); return 1;
                case CAST_UINT16: *out = lumyr_cast_uint16(v); return 1;
                case CAST_UINT32: *out = lumyr_cast_uint32(v); return 1;
                case CAST_UINT64: *out = lumyr_cast_uint64(v); return 1;
                case CAST_LONG: *out = lumyr_cast_long(v); return 1;
                case CAST_LONGLONG: *out = lumyr_cast_longlong(v); return 1;
                case CAST_FLOAT: *out = lumyr_cast_float(v); return 1;
                default: return 0;
            }
        }
        default:
            return 0;
    }
}

/* 递归编译 print 的参数列表（左嵌套 AST_SEQ: seq(seq(a,b),c)），返回参数数量 */
static int c_print_args_recursive(Ctx* c, AstNode* args)
{
    if(!args) return 0;
    if(args->type != AST_SEQ) {
        c_expr(c, args);
        return 1;
    }
    int n1 = c_print_args_recursive(c, args->u.seq.first);
    int n2 = c_print_args_recursive(c, args->u.seq.second);
    return n1 + n2;
}

static int c_print_args(Ctx* c, AstNode* args)
{
    return c_print_args_recursive(c, args);
}

/* 混合类型算术运算的类型提升辅助函数：根据源类型和目标类型发射对应的转换指令
   类型编号：1=int, 2=uint, 3=float, 4=double, 5=long long */
/* 根据目标类型和 Value，发射对应的专用常量加载指令
   所有数据类型都直接压入对应的专用栈，不经过 Value 栈，实现零开销 */
static void emit_typed_const(Ctx* c, CastKind cast_type, Value v) {
    /* 根据 Value 的实际类型读取联合体中对应字段，避免跨字段误读 */
    switch(cast_type) {
        /* 整数类型：a=常量值 */
        case CAST_INT:
            emit(c, OPC_PUSH_INT_CONST, (int)v.v.i, 0);
            break;
        case CAST_INT32:
            emit(c, OPC_PUSH_INT_CONST, (int32_t)v.v.i32, 0);
            break;
        case CAST_UINT32:
            emit(c, OPC_PUSH_UINT32_CONST, (uint32_t)v.v.u32, 0);
            break;
        case CAST_UINT:
            emit(c, OPC_PUSH_UINT32_CONST, (uint32_t)v.v.ui, 0);
            break;
        case CAST_INT8:
            emit(c, OPC_PUSH_INT8_CONST, (int8_t)v.v.i8, 0);
            break;
        case CAST_INT16:
            emit(c, OPC_PUSH_INT16_CONST, (int16_t)v.v.i16, 0);
            break;
        case CAST_SHORT:
            emit(c, OPC_PUSH_INT16_CONST, (int16_t)v.v.sh, 0);
            break;
        case CAST_UINT8:
            emit(c, OPC_PUSH_UINT8_CONST, (uint8_t)v.v.u8, 0);
            break;
        case CAST_UCHAR:
            emit(c, OPC_PUSH_UINT8_CONST, (uint8_t)v.v.uc, 0);
            break;
        case CAST_UINT16:
            emit(c, OPC_PUSH_UINT16_CONST, (uint16_t)v.v.u16, 0);
            break;
        case CAST_USHORT:
            emit(c, OPC_PUSH_UINT16_CONST, (uint16_t)v.v.us, 0);
            break;
        case CAST_BYTE:
            emit(c, OPC_PUSH_BYTE_CONST, (uint8_t)v.v.by, 0);
            break;
        case CAST_CHAR:
            emit(c, OPC_PUSH_CHAR_CONST, (char)v.v.c, 0);
            break;
        case CAST_BOOL:
            emit(c, OPC_PUSH_BOOL_CONST, v.v.b ? 1 : 0, 0);
            break;
        /* 64位整数类型：a=低32位, b=高32位 */
        case CAST_INT64: {
            int64_t i64v = v.v.i64;  /* lumyr_make_int64 存在 v.v.i64 */
            emit(c, OPC_PUSH_INT64_CONST, (int)(uint32_t)i64v, (int)(uint32_t)(i64v >> 32));
            break;
        }
        case CAST_LONGLONG: {
            long long llv = v.v.ll;  /* lumyr_make_long_long 存在 v.v.ll */
            emit(c, OPC_PUSH_LONG_LONG_CONST, (int)(uint32_t)llv, (int)(uint32_t)(llv >> 32));
            break;
        }
        case CAST_LONG: {
            long lv = v.v.l;
            emit(c, OPC_PUSH_LONG_CONST, (int)(lv & 0xFFFFFFFF), (int)((lv >> 32) & 0xFFFFFFFF));
            break;
        }
        case CAST_UINT64: {
            unsigned long long ullv = v.v.u64;  /* lumyr_make_uint64 存在 v.v.u64 */
            emit(c, OPC_PUSH_UINT64_CONST, (int)(ullv & 0xFFFFFFFF), (int)((ullv >> 32) & 0xFFFFFFFF));
            break;
        }
        case CAST_ULONG: {
            unsigned long long ullv = (unsigned long long)v.v.ul;  /* lumyr_make_ulong 存在 v.v.ul */
            emit(c, OPC_PUSH_UINT64_CONST, (int)(ullv & 0xFFFFFFFF), (int)((ullv >> 32) & 0xFFFFFFFF));
            break;
        }
        case CAST_SIZE_T: {
            size_t stv = v.v.st;
            emit(c, OPC_PUSH_SIZE_T_CONST, (int)(stv & 0xFFFFFFFF), (int)((stv >> 32) & 0xFFFFFFFF));
            break;
        }
        case CAST_SSIZE_T: {
            ssize_t sstv = v.v.sst;
            emit(c, OPC_PUSH_SSIZE_T_CONST, (int)(sstv & 0xFFFFFFFF), (int)((sstv >> 32) & 0xFFFFFFFF));
            break;
        }
        /* 浮点类型：a=常量池下标 */
        case CAST_FLOAT: {
            int const_idx = bf_const(c->fn, v);
            emit(c, OPC_PUSH_FLOAT_CONST, const_idx, 0);
            break;
        }
        case CAST_DOUBLE: {
            int const_idx = bf_const(c->fn, v);
            emit(c, OPC_PUSH_DOUBLE_CONST, const_idx, 0);
            break;
        }
        /* long double：a=低32位, b=高32位 */
        case CAST_LONG_DOUBLE: {
            long double ldv = v.v.ld;
            unsigned long long bits = 0;
            memcpy(&bits, &ldv, sizeof(unsigned long long));
            emit(c, OPC_PUSH_LONG_DOUBLE_CONST, (int)(bits & 0xFFFFFFFF), (int)((bits >> 32) & 0xFFFFFFFF));
            break;
        }
        /* 其他类型：默认使用 OPC_LOAD_CONST */
        default:
            emit(c, OPC_LOAD_CONST, bf_const(c->fn, v), 0);
            break;
    }
}

/* 根据 ExprType 类型返回对应的变量加载指令 */
static OpCode get_load_var_opcode(ExprType expr_type) {
    switch(expr_type) {
        case EXPR_TYPE_BOOL: return OPC_LOAD_BOOL_VAR;
        case EXPR_TYPE_CHAR: return OPC_LOAD_CHAR_VAR;
        case EXPR_TYPE_INT8: return OPC_LOAD_INT8_VAR;
        case EXPR_TYPE_INT16: return OPC_LOAD_INT16_VAR;
        case EXPR_TYPE_SHORT: return OPC_LOAD_SHORT_VAR;
        case EXPR_TYPE_INT: return OPC_LOAD_INT_VAR;
        case EXPR_TYPE_INT64: return OPC_LOAD_INT64_VAR;
        case EXPR_TYPE_LONG_LONG: return OPC_LOAD_LONG_LONG_VAR;
        case EXPR_TYPE_LONG: return OPC_LOAD_LONG_VAR;
        case EXPR_TYPE_BYTE: return OPC_LOAD_BYTE_VAR;
        case EXPR_TYPE_UINT8: return OPC_LOAD_UINT8_VAR;
        case EXPR_TYPE_UINT16: return OPC_LOAD_UINT16_VAR;
        case EXPR_TYPE_UINT: return OPC_LOAD_UINT_VAR;
        case EXPR_TYPE_UINT64: return OPC_LOAD_UINT64_VAR;
        case EXPR_TYPE_ULONG: return OPC_LOAD_ULONG_VAR;
        case EXPR_TYPE_SIZE_T: return OPC_LOAD_SIZE_T_VAR;
        case EXPR_TYPE_SSIZE_T: return OPC_LOAD_SSIZE_T_VAR;
        case EXPR_TYPE_FLOAT: return OPC_LOAD_FLOAT_VAR;
        case EXPR_TYPE_DOUBLE: return OPC_LOAD_DOUBLE_VAR;
        case EXPR_TYPE_LONG_DOUBLE: return OPC_LOAD_LONG_DOUBLE_VAR;
        default: return OPC_LOAD_VAR;
    }
}

/* 根据 ExprType 类型返回对应的 TO_VALUE 指令 */
static OpCode get_to_value_opcode(ExprType expr_type) {
    switch(expr_type) {
        /* 整数类型 */
        case EXPR_TYPE_BOOL: return OPC_BOOL_TO_VALUE;
        case EXPR_TYPE_CHAR: return OPC_CHAR_TO_VALUE;
        case EXPR_TYPE_INT8: return OPC_INT8_TO_VALUE;
        case EXPR_TYPE_INT16: return OPC_INT16_TO_VALUE;
        case EXPR_TYPE_SHORT: return OPC_SHORT_TO_VALUE;
        case EXPR_TYPE_INT: return OPC_INT_TO_VALUE;
        case EXPR_TYPE_INT64: return OPC_INT64_TO_VALUE;
        case EXPR_TYPE_LONG_LONG: return OPC_LONG_LONG_TO_VALUE;
        case EXPR_TYPE_LONG: return OPC_LONG_TO_VALUE;
        /* 无符号整数类型 */
        case EXPR_TYPE_BYTE: return OPC_BYTE_TO_VALUE;
        case EXPR_TYPE_UINT8: return OPC_UINT8_TO_VALUE;
        case EXPR_TYPE_UINT16: return OPC_UINT16_TO_VALUE;
        case EXPR_TYPE_UINT: return OPC_UINT_TO_VALUE;
        case EXPR_TYPE_UINT64: return OPC_UINT64_TO_VALUE;
        case EXPR_TYPE_ULONG: return OPC_ULONG_TO_VALUE;
        case EXPR_TYPE_SIZE_T: return OPC_SIZE_T_TO_VALUE;
        case EXPR_TYPE_SSIZE_T: return OPC_SSIZE_T_TO_VALUE;
        /* 浮点类型 */
        case EXPR_TYPE_FLOAT: return OPC_FLOAT_TO_VALUE;
        case EXPR_TYPE_DOUBLE: return OPC_DOUBLE_TO_VALUE;
        case EXPR_TYPE_LONG_DOUBLE: return OPC_LONG_DOUBLE_TO_VALUE;
        default: return OPC_NOP; /* EXPR_TYPE_NONE 等无专用栈的类型 */
    }
}

static void emit_mixed_type_promote(Ctx* c, ExprType src_type, ExprType dst_type) {
    if(src_type == dst_type) return;
    
    /* 先处理有专用转换指令的类型 */
    if(src_type == EXPR_TYPE_INT) {
        if(dst_type == EXPR_TYPE_UINT) { emit(c, OPC_INT_TO_UINT, 0, 0); return; }
        if(dst_type == EXPR_TYPE_FLOAT) { emit(c, OPC_INT_TO_FLOAT, 0, 0); return; }
        if(dst_type == EXPR_TYPE_DOUBLE) { emit(c, OPC_INT_TO_DOUBLE, 0, 0); return; }
        if(dst_type == EXPR_TYPE_LONG_LONG) { emit(c, OPC_INT_TO_LONG_LONG, 0, 0); return; }
        if(dst_type == EXPR_TYPE_LONG_DOUBLE) { emit(c, OPC_INT_TO_LONG_DOUBLE, 0, 0); return; }
    }
    if(src_type == EXPR_TYPE_UINT) {
        if(dst_type == EXPR_TYPE_INT) { emit(c, OPC_UINT_TO_INT, 0, 0); return; }
        if(dst_type == EXPR_TYPE_FLOAT) { emit(c, OPC_UINT_TO_FLOAT, 0, 0); return; }
        if(dst_type == EXPR_TYPE_DOUBLE) { emit(c, OPC_UINT_TO_DOUBLE, 0, 0); return; }
        if(dst_type == EXPR_TYPE_LONG_LONG) { emit(c, OPC_UINT_TO_LONG_LONG, 0, 0); return; }
        if(dst_type == EXPR_TYPE_LONG_DOUBLE) { emit(c, OPC_UINT_TO_LONG_DOUBLE, 0, 0); return; }
    }
    if(src_type == EXPR_TYPE_FLOAT) {
        if(dst_type == EXPR_TYPE_DOUBLE) { emit(c, OPC_FLOAT_TO_DOUBLE, 0, 0); return; }
        if(dst_type == EXPR_TYPE_LONG_LONG) { emit(c, OPC_FLOAT_TO_LONG_LONG, 0, 0); return; }
        if(dst_type == EXPR_TYPE_LONG_DOUBLE) { emit(c, OPC_FLOAT_TO_LONG_DOUBLE, 0, 0); return; }
    }
    if(src_type == EXPR_TYPE_DOUBLE) {
        if(dst_type == EXPR_TYPE_LONG_LONG) { emit(c, OPC_DOUBLE_TO_LONG_LONG, 0, 0); return; }
        if(dst_type == EXPR_TYPE_LONG_DOUBLE) { emit(c, OPC_DOUBLE_TO_LONG_DOUBLE, 0, 0); return; }
    }
    if(src_type == EXPR_TYPE_LONG_LONG) {
        if(dst_type == EXPR_TYPE_FLOAT) { emit(c, OPC_LONG_LONG_TO_FLOAT, 0, 0); return; }
        if(dst_type == EXPR_TYPE_DOUBLE) { emit(c, OPC_LONG_LONG_TO_DOUBLE, 0, 0); return; }
        if(dst_type == EXPR_TYPE_LONG_DOUBLE) { emit(c, OPC_LONG_LONG_TO_LONG_DOUBLE, 0, 0); return; }
    }
    
    /* 对于没有专用转换指令的类型，使用通用路径：
       先从源类型专用栈转换到 Value 栈，然后再从 Value 栈转换到目标类型专用栈
       注意：这需要目标类型也有专用栈和 TO_VALUE 指令，否则走完全通用路径 */
    OpCode src_to_value = get_to_value_opcode(src_type);
    if(src_to_value != OPC_NOP) {
        emit(c, src_to_value, 0, 0);
        /* 从 Value 栈转换到目标类型专用栈需要专用指令，暂时不支持
           后续可以添加 VALUE_TO_* 系列指令 */
    }
    /* 如果没有专用转换指令，暂时不做转换，保持在源类型专用栈中
       后续可以通过 Value 栈中转，或者添加更多专用转换指令 */
}

void c_expr(Ctx* c, AstNode* node)
{
    if(!node) { emit(c, OPC_LOAD_CONST, bf_const(c->fn, val_none()), 0); return; }
    switch(node->type) {
        case AST_INT:
            emit(c, OPC_LOAD_CONST, bf_const(c->fn, lumyr_make_int(node->u.inum)), 0);
            break;
        case AST_NUM:
            emit(c, OPC_LOAD_CONST, bf_const(c->fn, lumyr_make_double(node->u.num)), 0);
            break;
        case AST_BOOL:
            emit(c, OPC_LOAD_CONST, bf_const(c->fn, lumyr_make_bool(node->u.bval ? 1 : 0)), 0);
            break;
        case AST_NONE:
            emit(c, OPC_LOAD_CONST, bf_const(c->fn, val_none()), 0);
            break;
        case AST_CHAR:
            emit(c, OPC_LOAD_CONST, bf_const(c->fn, lumyr_make_char(node->u.ch)), 0);
            break;
        case AST_STRING:
            emit(c, OPC_LOAD_CONST, bf_const(c->fn, intern_string(node->u.sval)), 0);
            break;
        case AST_VAR:
            emit(c, OPC_LOAD_VAR, bf_sym(c->fn, node->u.varname), 0);
            break;
        case AST_FUNCREF:
            // 真函数（全局函数表）→ 取函数值；否则为存函数值的变量 → 读变量
            if(ir_func_table_lookup(node->u.varname))
                emit(c, OPC_GETFUNC, bf_sym(c->fn, node->u.varname), 0);
            else
                emit(c, OPC_LOAD_VAR, bf_sym(c->fn, node->u.varname), 0);
            break;
        case AST_FUNC_DEF:
            // 匿名函数表达式：函数已由 yacc 期注册（compile_func_from_ast → IR 函数表），
            // 表达式求值 = 压入函数值（内部名 _lambda_N）
            if(strncmp(node->u.func_def.name, "_lambda_", 8) == 0) {
                // 有捕获变量：运行时装箱生成闭包实例；无捕获：直接取全局共享函数值
                if(lambda_capture_count(node->u.func_def.name) > 0)
                    emit(c, OPC_MKCLOSURE, bf_sym(c->fn, node->u.func_def.name), 0);
                else
                    emit(c, OPC_GETFUNC, bf_sym(c->fn, node->u.func_def.name), 0);
            } else {
                emit(c, OPC_LOAD_CONST, bf_const(c->fn, val_none()), 0);
            }
            break;
        case AST_ASSIGN: {
            int var_idx = bf_sym(c->fn, node->u.assign.varname);
            /* 记录变量类型标记：如果赋值是 <type>expr，则记录类型标记；
               如果赋值没有类型标注，则清除之前的类型标记（回退到动态 Value 类型）。
               注意：<type>[...] 和 <type>{...} 中的 type 是元素/值类型，不是变量类型，不设置标记 */
            if(node->u.assign.expr && node->u.assign.expr->type == AST_TYPE_ANNOTATION) {
                AstNode* inner = node->u.assign.expr->u.type_annotation.expr;
                if(inner && inner->type != AST_ARRAY_LIT && inner->type != AST_MAP_LIT) {
                    c->fn->var_type_tags[var_idx] = node->u.assign.expr->u.type_annotation.cast_type;
                } else if(inner && inner->type == AST_ARRAY_LIT &&
                          node->u.assign.expr->u.type_annotation.cast_type == CAST_INT) {
                    /* <int>[...] 形式：变量是 int 类型化数组，设置特殊标记用于上下文感知类型推导 */
                    c->fn->var_type_tags[var_idx] = VAR_TYPE_INT_ARRAY;
                } else if(inner && inner->type == AST_ARRAY_LIT &&
                          node->u.assign.expr->u.type_annotation.cast_type == CAST_DOUBLE) {
                    /* <double>[...] 形式：变量是 double 类型化数组，设置特殊标记用于上下文感知类型推导 */
                    c->fn->var_type_tags[var_idx] = VAR_TYPE_DOUBLE_ARRAY;
                } else if(inner && inner->type == AST_ARRAY_LIT &&
                          node->u.assign.expr->u.type_annotation.cast_type == CAST_FLOAT) {
                    /* <float>[...] 形式：变量是 float 类型化数组，设置特殊标记用于上下文感知类型推导 */
                    c->fn->var_type_tags[var_idx] = VAR_TYPE_FLOAT_ARRAY;
                } else if(inner && inner->type == AST_ARRAY_LIT &&
                          node->u.assign.expr->u.type_annotation.cast_type == CAST_UINT32) {
                    /* <uint32>[...] 形式：变量是 uint 类型化数组，设置特殊标记用于上下文感知类型推导 */
                    c->fn->var_type_tags[var_idx] = VAR_TYPE_UINT_ARRAY;
                } else if(inner && inner->type == AST_ARRAY_LIT &&
                          node->u.assign.expr->u.type_annotation.cast_type == CAST_BOOL) {
                    /* <bool>[...] 形式：变量是 bool 类型化数组，设置特殊标记用于上下文感知类型推导 */
                    c->fn->var_type_tags[var_idx] = VAR_TYPE_BOOL_ARRAY;
                } else if(inner && inner->type == AST_ARRAY_LIT &&
                          node->u.assign.expr->u.type_annotation.cast_type == CAST_CHAR) {
                    /* <char>[...] 形式：变量是 char 类型化数组，设置特殊标记用于上下文感知类型推导 */
                    c->fn->var_type_tags[var_idx] = VAR_TYPE_CHAR_ARRAY;
                } else if(inner && inner->type == AST_ARRAY_LIT &&
                          node->u.assign.expr->u.type_annotation.cast_type == CAST_BYTE) {
                    /* <byte>[...] 形式：变量是 byte 类型化数组，设置特殊标记用于上下文感知类型推导 */
                    c->fn->var_type_tags[var_idx] = VAR_TYPE_BYTE_ARRAY;
                } else {
                    /* 其他数组/map字面量的类型标注不设置变量类型标记 */
                    c->fn->var_type_tags[var_idx] = -1;
                }
            } else if(node->u.assign.expr && node->u.assign.expr->type == AST_ARRAY_LIT) {
                /* <type>[...] 形式在语法分析阶段被特殊处理成 AST_ARRAY_LIT，
                   元素类型存储在 elem_type 字段中（使用 ValueType 枚举） */
                int elem_type = node->u.assign.expr->u.array_lit.elem_type;
                if(elem_type == VAL_INT) {
                    c->fn->var_type_tags[var_idx] = VAR_TYPE_INT_ARRAY;
                } else if(elem_type == VAL_DOUBLE) {
                    c->fn->var_type_tags[var_idx] = VAR_TYPE_DOUBLE_ARRAY;
                } else if(elem_type == VAL_FLOAT) {
                    c->fn->var_type_tags[var_idx] = VAR_TYPE_FLOAT_ARRAY;
                } else if(elem_type == VAL_UINT32) {
                    c->fn->var_type_tags[var_idx] = VAR_TYPE_UINT_ARRAY;
                } else if(elem_type == VAL_BOOL) {
                    c->fn->var_type_tags[var_idx] = VAR_TYPE_BOOL_ARRAY;
                } else if(elem_type == VAL_CHAR) {
                    c->fn->var_type_tags[var_idx] = VAR_TYPE_CHAR_ARRAY;
                } else if(elem_type == VAL_BYTE) {
                    c->fn->var_type_tags[var_idx] = VAR_TYPE_BYTE_ARRAY;
                } else {
                    c->fn->var_type_tags[var_idx] = -1;
                }
            } else if(node->u.assign.expr && node->u.assign.expr->type == AST_INTERFACE_ANNOTATION) {
                /* 接口类型标注：<Printable>expr → 变量是接口引用类型 */
                const char* iface_name = node->u.assign.expr->u.interface_annotation.interface_name;
                if(iface_name) {
                    if(c->fn->var_struct_names[var_idx]) {
                        free(c->fn->var_struct_names[var_idx]);
                    }
                    size_t flen = strlen(iface_name);
                    char* marked_name = (char*)malloc(flen + 11);
                    snprintf(marked_name, flen + 11, "interface:%s", iface_name);
                    c->fn->var_struct_names[var_idx] = marked_name;
                }
                c->fn->var_type_tags[var_idx] = -1;
            } else {
                /* 无类型标注的赋值：清除之前的类型标记，回退到动态 Value 类型 */
                c->fn->var_type_tags[var_idx] = -1;
                /* 检测 struct 构造调用：Point(10, 20) → 变量是 Point 类型 */
                if(node->u.assign.expr && node->u.assign.expr->type == AST_CALL) {
                    const char* fname = node->u.assign.expr->u.call.name;
                    if(fname && struct_lookup(fname)) {
                        /* 释放旧的 struct 类型名（如果有） */
                        if(c->fn->var_struct_names[var_idx]) {
                            free(c->fn->var_struct_names[var_idx]);
                        }
                        c->fn->var_struct_names[var_idx] = strdup(fname);
                    }
                    /* 检测 class 构造调用：Animal("Cat", 3) → 变量是 Animal class 类型；
                       或者 Animal___init__(obj, args...) → 变量也是 Animal class 类型 */
                    else if(fname) {
                        TypeDef* td = type_lookup(fname);
                        if(td && td->is_class) {
                            /* Animal("Cat", 3) 形式：直接检测 class 类型 */
                            if(c->fn->var_struct_names[var_idx]) {
                                free(c->fn->var_struct_names[var_idx]);
                            }
                            size_t flen = strlen(fname);
                            char* marked_name = (char*)malloc(flen + 7);
                            snprintf(marked_name, flen + 7, "class:%s", fname);
                            c->fn->var_struct_names[var_idx] = marked_name;
                        } else {
                            /* Animal___init__(obj, args...) 形式：检测 ___init__ 后缀 */
                            size_t flen = strlen(fname);
                            if(flen >= 9 && strcmp(fname + flen - 9, "___init__") == 0) {
                                char* class_name = (char*)malloc(flen - 8);
                                strncpy(class_name, fname, flen - 9);
                                class_name[flen - 9] = '\0';
                                TypeDef* td2 = type_lookup(class_name);
                                if(td2 && td2->is_class) {
                                    if(c->fn->var_struct_names[var_idx]) {
                                        free(c->fn->var_struct_names[var_idx]);
                                    }
                                    char* marked_name = (char*)malloc(flen);
                                    snprintf(marked_name, flen, "class:%s", class_name);
                                    c->fn->var_struct_names[var_idx] = marked_name;
                                }
                                free(class_name);
                            }
                        }
                    }
                }
                /* 检测右侧是 struct 类型变量：copy = original → 推断 copy 也是 struct 类型 */
                else if(node->u.assign.expr && node->u.assign.expr->type == AST_VAR) {
                    const char* rhs_name = node->u.assign.expr->u.varname;
                    int rhs_idx = bf_sym(c->fn, rhs_name);
                    if(c->fn->var_struct_names && c->fn->var_struct_names[rhs_idx]) {
                        if(c->fn->var_struct_names[var_idx]) {
                            free(c->fn->var_struct_names[var_idx]);
                        }
                        c->fn->var_struct_names[var_idx] = strdup(c->fn->var_struct_names[rhs_idx]);
                    }
                }
            }
            /* 优化0ld：赋值为 <long double>字面量 形式时，使用 OPC_PUSH_LONG_DOUBLE_CONST + OPC_STORE_LONG_DOUBLE_VAR
               零包装零重复提取，直接把字面量值压入 long double 栈并存储到 long double 变量
               注意：Windows平台下long double是128位(16字节)，但指令只能传递64位，
               所以先转换为double(64位)，传递double的位模式，VM中再提升为long double */
            if(node->u.assign.expr && node->u.assign.expr->type == AST_TYPE_ANNOTATION &&
               node->u.assign.expr->u.type_annotation.cast_type == CAST_LONG_DOUBLE &&
               node->u.assign.expr->u.type_annotation.expr &&
               node->u.assign.expr->u.type_annotation.expr->type == AST_NUM) {
                double d_val = (double)node->u.assign.expr->u.type_annotation.expr->u.num;
                /* 将 double 的位模式复制到 uint64_t，然后拆分为低32位和高32位 */
                uint64_t bits = 0;
                memcpy(&bits, &d_val, sizeof(double));
                emit(c, OPC_PUSH_LONG_DOUBLE_CONST, (int)(bits & 0xFFFFFFFF), (int)((bits >> 32) & 0xFFFFFFFF));
                emit(c, OPC_STORE_LONG_DOUBLE_VAR, var_idx, 0);
                c->fn->var_type_tags[var_idx] = CAST_LONG_DOUBLE;
            }
            /* 优化0：赋值为 <int>字面量 形式时，使用 OPC_PUSH_INT_CONST + OPC_STORE_INT_VAR
               零包装零重复提取，直接把字面量值压入 int 栈并存储到 int 变量
               避免创建 Value 再提取的开销
               支持正整数字面量和负整数字面量（AST_UNARY + OP_UNARY_MINUS） */
            else if(node->u.assign.expr && node->u.assign.expr->type == AST_TYPE_ANNOTATION &&
               node->u.assign.expr->u.type_annotation.cast_type == CAST_INT &&
               node->u.assign.expr->u.type_annotation.expr) {
                long long literal_val;
                if (extract_int_literal_value(node->u.assign.expr->u.type_annotation.expr, &literal_val)) {
                    emit(c, OPC_PUSH_INT_CONST, (int)literal_val, 0);
                    emit(c, OPC_STORE_INT_VAR, var_idx, 0);
                    c->fn->var_type_tags[var_idx] = CAST_INT;
                } else {
                    c_expr(c, node->u.assign.expr);
                    emit(c, OPC_STORE_VAR, var_idx, 0);
                }
            }
            /* 优化0u：赋值为 <uint>字面量 形式时，使用 OPC_PUSH_UINT_CONST + OPC_STORE_UINT_VAR
               零包装零重复提取，直接把字面量值压入 uint 栈并存储到 uint 变量
               避免创建 Value 再提取的开销 */
            else if(node->u.assign.expr && node->u.assign.expr->type == AST_TYPE_ANNOTATION &&
               node->u.assign.expr->u.type_annotation.cast_type == CAST_UINT32 &&
               node->u.assign.expr->u.type_annotation.expr) {
                long long literal_val;
                if (extract_int_literal_value(node->u.assign.expr->u.type_annotation.expr, &literal_val)) {
                    emit(c, OPC_PUSH_UINT32_CONST, (uint32_t)literal_val, 0);
                    emit(c, OPC_STORE_UINT32_VAR, var_idx, 0);
                    c->fn->var_type_tags[var_idx] = CAST_UINT32;
                } else {
                    c_expr(c, node->u.assign.expr);
                    emit(c, OPC_STORE_VAR, var_idx, 0);
                }
            }
            /* 优化0uint：赋值为 <uint>字面量 形式时，使用 OPC_PUSH_UINT_CONST + OPC_STORE_UINT_VAR */
            else if(node->u.assign.expr && node->u.assign.expr->type == AST_TYPE_ANNOTATION &&
               node->u.assign.expr->u.type_annotation.cast_type == CAST_UINT &&
               node->u.assign.expr->u.type_annotation.expr) {
                long long literal_val;
                if (extract_int_literal_value(node->u.assign.expr->u.type_annotation.expr, &literal_val)) {
                    emit(c, OPC_PUSH_UINT_CONST, (unsigned int)literal_val, 0);
                    emit(c, OPC_STORE_UINT_VAR, var_idx, 0);
                    c->fn->var_type_tags[var_idx] = CAST_UINT;
                } else {
                    c_expr(c, node->u.assign.expr);
                    emit(c, OPC_STORE_VAR, var_idx, 0);
                }
            }
            /* 优化0b：赋值为 <bool>表达式 形式时，使用专用路径
               如果是bool字面量，使用 OPC_PUSH_BOOL_CONST + OPC_STORE_BOOL_VAR
               否则编译表达式后从Value栈提取bool值，使用 OPC_STORE_BOOL_VAR */
            else if(node->u.assign.expr && node->u.assign.expr->type == AST_TYPE_ANNOTATION &&
               node->u.assign.expr->u.type_annotation.cast_type == CAST_BOOL &&
               node->u.assign.expr->u.type_annotation.expr) {
                AstNode* inner_expr = node->u.assign.expr->u.type_annotation.expr;
                if(inner_expr->type == AST_BOOL) {
                    /* bool字面量：直接压入bool栈，零检查零转换 */
                    int literal_val = inner_expr->u.bval ? 1 : 0;
                    emit(c, OPC_PUSH_BOOL_CONST, literal_val, 0);
                } else if(inner_expr->type == AST_INT) {
                    /* 整数字面量：非零为true，零为false */
                    int literal_val = inner_expr->u.inum ? 1 : 0;
                    emit(c, OPC_PUSH_BOOL_CONST, literal_val, 0);
                } else {
                    /* 其他表达式：编译表达式压入Value栈，然后转换为bool压入bool栈 */
                    c_expr(c, inner_expr);
                    emit(c, OPC_TO_BOOL, 0, 0);
                }
                /* OPC_STORE_BOOL_VAR：从 bool 栈弹出，存储到 bool_vals，零重复提取 */
                emit(c, OPC_STORE_BOOL_VAR, var_idx, 0);
                /* 记录变量类型标记为 bool */
                c->fn->var_type_tags[var_idx] = CAST_BOOL;
            }
            /* 优化0ll：赋值为 <long long>字面量 形式时，使用 OPC_PUSH_LONG_LONG_CONST + OPC_STORE_LONG_LONG_VAR
               零包装零重复提取，直接把字面量值压入 long long 栈并存储到 long long 变量
               避免创建 Value 再提取的开销
               支持正整数字面量和负整数字面量（AST_UNARY + OP_UNARY_MINUS） */
            else if(node->u.assign.expr && node->u.assign.expr->type == AST_TYPE_ANNOTATION &&
               node->u.assign.expr->u.type_annotation.cast_type == CAST_LONGLONG &&
               node->u.assign.expr->u.type_annotation.expr) {
                long long literal_val;
                if (extract_int_literal_value(node->u.assign.expr->u.type_annotation.expr, &literal_val)) {
                    emit(c, OPC_PUSH_LONG_LONG_CONST, (int)(uint32_t)literal_val, (int)(uint32_t)(literal_val >> 32));
                    emit(c, OPC_STORE_LONG_LONG_VAR, var_idx, 0);
                    c->fn->var_type_tags[var_idx] = CAST_LONGLONG;
                } else {
                    c_expr(c, node->u.assign.expr);
                    emit(c, OPC_STORE_VAR, var_idx, 0);
                }
            }
            /* 优化0c：赋值为 <char>字面量 形式时，使用 OPC_PUSH_CHAR_CONST + OPC_STORE_CHAR_VAR
               零包装零重复提取，直接把字面量值压入 char 栈并存储到 char 变量
               避免创建 Value 再提取的开销 */
            else if(node->u.assign.expr && node->u.assign.expr->type == AST_TYPE_ANNOTATION &&
               node->u.assign.expr->u.type_annotation.cast_type == CAST_CHAR &&
               node->u.assign.expr->u.type_annotation.expr &&
               (node->u.assign.expr->u.type_annotation.expr->type == AST_CHAR ||
                node->u.assign.expr->u.type_annotation.expr->type == AST_INT)) {
                AstNode* inner_expr = node->u.assign.expr->u.type_annotation.expr;
                char literal_val = (inner_expr->type == AST_CHAR) ?
                    (char)inner_expr->u.ch : (char)(unsigned char)inner_expr->u.inum;
                /* OPC_PUSH_CHAR_CONST：直接把常量值压入 char 栈，零检查零转换 */
                emit(c, OPC_PUSH_CHAR_CONST, (int)literal_val, 0);
                /* OPC_STORE_CHAR_VAR：从 char 栈弹出，存储到 char_vals，零重复提取 */
                emit(c, OPC_STORE_CHAR_VAR, var_idx, 0);
                /* 记录变量类型标记为 char */
                c->fn->var_type_tags[var_idx] = CAST_CHAR;
            }
            /* 优化0byte：赋值为 <byte>字面量 形式时，使用 OPC_PUSH_BYTE_CONST + OPC_STORE_BYTE_VAR
               零包装零重复提取，直接把字面量值压入 byte 栈并存储到 byte 变量
               避免创建 Value 再提取的开销 */
            else if(node->u.assign.expr && node->u.assign.expr->type == AST_TYPE_ANNOTATION &&
               node->u.assign.expr->u.type_annotation.cast_type == CAST_BYTE &&
               node->u.assign.expr->u.type_annotation.expr) {
                long long literal_val;
                if (extract_int_literal_value(node->u.assign.expr->u.type_annotation.expr, &literal_val)) {
                    emit(c, OPC_PUSH_BYTE_CONST, (unsigned char)literal_val, 0);
                    emit(c, OPC_STORE_BYTE_VAR, var_idx, 0);
                    c->fn->var_type_tags[var_idx] = CAST_BYTE;
                } else {
                    c_expr(c, node->u.assign.expr);
                    emit(c, OPC_STORE_VAR, var_idx, 0);
                }
            }
            /* 优化0int8：赋值为 <int8>字面量 形式时，使用 OPC_PUSH_INT8_CONST + OPC_STORE_INT8_VAR
               支持正整数字面量和负整数字面量（AST_UNARY + OP_UNARY_MINUS） */
            else if(node->u.assign.expr && node->u.assign.expr->type == AST_TYPE_ANNOTATION &&
               node->u.assign.expr->u.type_annotation.cast_type == CAST_INT8 &&
               node->u.assign.expr->u.type_annotation.expr) {
                long long literal_val;
                if (extract_int_literal_value(node->u.assign.expr->u.type_annotation.expr, &literal_val)) {
                    emit(c, OPC_PUSH_INT8_CONST, (int)literal_val, 0);
                    emit(c, OPC_STORE_INT8_VAR, var_idx, 0);
                    c->fn->var_type_tags[var_idx] = CAST_INT8;
                } else {
                    c_expr(c, node->u.assign.expr);
                    emit(c, OPC_STORE_VAR, var_idx, 0);
                }
            }
            /* 优化0int16：赋值为 <int16>字面量 形式时，使用 OPC_PUSH_INT16_CONST + OPC_STORE_INT16_VAR
               支持正整数字面量和负整数字面量（AST_UNARY + OP_UNARY_MINUS） */
            else if(node->u.assign.expr && node->u.assign.expr->type == AST_TYPE_ANNOTATION &&
               node->u.assign.expr->u.type_annotation.cast_type == CAST_INT16 &&
               node->u.assign.expr->u.type_annotation.expr) {
                long long literal_val;
                if (extract_int_literal_value(node->u.assign.expr->u.type_annotation.expr, &literal_val)) {
                    emit(c, OPC_PUSH_INT16_CONST, (int)literal_val, 0);
                    emit(c, OPC_STORE_INT16_VAR, var_idx, 0);
                    c->fn->var_type_tags[var_idx] = CAST_INT16;
                } else {
                    c_expr(c, node->u.assign.expr);
                    emit(c, OPC_STORE_VAR, var_idx, 0);
                }
            }
            /* 优化0short：赋值为 <short>字面量 形式时，使用 OPC_PUSH_SHORT_CONST + OPC_STORE_SHORT_VAR */
            else if(node->u.assign.expr && node->u.assign.expr->type == AST_TYPE_ANNOTATION &&
               node->u.assign.expr->u.type_annotation.cast_type == CAST_SHORT &&
               node->u.assign.expr->u.type_annotation.expr) {
                long long literal_val;
                if (extract_int_literal_value(node->u.assign.expr->u.type_annotation.expr, &literal_val)) {
                    emit(c, OPC_PUSH_SHORT_CONST, (int)literal_val, 0);
                    emit(c, OPC_STORE_SHORT_VAR, var_idx, 0);
                    c->fn->var_type_tags[var_idx] = CAST_SHORT;
                } else {
                    c_expr(c, node->u.assign.expr);
                    emit(c, OPC_STORE_VAR, var_idx, 0);
                }
            }
            /* 优化0int32：赋值为 <int32>字面量 形式时，使用 OPC_PUSH_INT32_CONST + OPC_STORE_INT32_VAR
               支持正整数字面量和负整数字面量（AST_UNARY + OP_UNARY_MINUS） */
            else if(node->u.assign.expr && node->u.assign.expr->type == AST_TYPE_ANNOTATION &&
               node->u.assign.expr->u.type_annotation.cast_type == CAST_INT32 &&
               node->u.assign.expr->u.type_annotation.expr) {
                long long literal_val;
                if (extract_int_literal_value(node->u.assign.expr->u.type_annotation.expr, &literal_val)) {
                    emit(c, OPC_PUSH_INT32_CONST, (int)literal_val, 0);
                    emit(c, OPC_STORE_INT32_VAR, var_idx, 0);
                    c->fn->var_type_tags[var_idx] = CAST_INT32;
                } else {
                    c_expr(c, node->u.assign.expr);
                    emit(c, OPC_STORE_VAR, var_idx, 0);
                }
            }
            /* 优化0int64：赋值为 <int64>字面量 形式时，使用 OPC_PUSH_INT64_CONST + OPC_STORE_INT64_VAR
               64位值合并：in.a低32位 + in.b高32位，in.b强制转换为unsigned int避免符号扩展
               支持正整数字面量和负整数字面量（AST_UNARY + OP_UNARY_MINUS） */
            else if(node->u.assign.expr && node->u.assign.expr->type == AST_TYPE_ANNOTATION &&
               node->u.assign.expr->u.type_annotation.cast_type == CAST_INT64 &&
               node->u.assign.expr->u.type_annotation.expr) {
                long long literal_val;
                if (extract_int_literal_value(node->u.assign.expr->u.type_annotation.expr, &literal_val)) {
                    emit(c, OPC_PUSH_INT64_CONST, (int)(uint32_t)literal_val, (int)(uint32_t)(literal_val >> 32));
                    emit(c, OPC_STORE_INT64_VAR, var_idx, 0);
                    c->fn->var_type_tags[var_idx] = CAST_INT64;
                } else {
                    c_expr(c, node->u.assign.expr);
                    emit(c, OPC_STORE_VAR, var_idx, 0);
                }
            }
            /* 优化0uint8：赋值为 <uint8>字面量 形式时，使用 OPC_PUSH_UINT8_CONST + OPC_STORE_UINT8_VAR */
            else if(node->u.assign.expr && node->u.assign.expr->type == AST_TYPE_ANNOTATION &&
               (node->u.assign.expr->u.type_annotation.cast_type == CAST_UINT8 ||
                node->u.assign.expr->u.type_annotation.cast_type == CAST_UCHAR) &&
               node->u.assign.expr->u.type_annotation.expr) {
                long long literal_val;
                if (extract_int_literal_value(node->u.assign.expr->u.type_annotation.expr, &literal_val)) {
                    emit(c, OPC_PUSH_UINT8_CONST, (uint8_t)literal_val, 0);
                    emit(c, OPC_STORE_UINT8_VAR, var_idx, 0);
                    c->fn->var_type_tags[var_idx] = node->u.assign.expr->u.type_annotation.cast_type;
                } else {
                    c_expr(c, node->u.assign.expr);
                    emit(c, OPC_STORE_VAR, var_idx, 0);
                }
            }
            /* 优化0uint16：赋值为 <uint16>字面量 形式时，使用 OPC_PUSH_UINT16_CONST + OPC_STORE_UINT16_VAR */
            else if(node->u.assign.expr && node->u.assign.expr->type == AST_TYPE_ANNOTATION &&
               (node->u.assign.expr->u.type_annotation.cast_type == CAST_UINT16 ||
                node->u.assign.expr->u.type_annotation.cast_type == CAST_USHORT) &&
               node->u.assign.expr->u.type_annotation.expr) {
                long long literal_val;
                if (extract_int_literal_value(node->u.assign.expr->u.type_annotation.expr, &literal_val)) {
                    emit(c, OPC_PUSH_UINT16_CONST, (uint16_t)literal_val, 0);
                    emit(c, OPC_STORE_UINT16_VAR, var_idx, 0);
                    c->fn->var_type_tags[var_idx] = node->u.assign.expr->u.type_annotation.cast_type;
                } else {
                    c_expr(c, node->u.assign.expr);
                    emit(c, OPC_STORE_VAR, var_idx, 0);
                }
            }
            /* 优化0uint64：赋值为 <uint64>字面量 形式时，使用 OPC_PUSH_UINT64_CONST + OPC_STORE_UINT64_VAR
               64位值合并：in.a低32位 + in.b高32位 */
            else if(node->u.assign.expr && node->u.assign.expr->type == AST_TYPE_ANNOTATION &&
               node->u.assign.expr->u.type_annotation.cast_type == CAST_UINT64 &&
               node->u.assign.expr->u.type_annotation.expr) {
                long long literal_val;
                if (extract_int_literal_value(node->u.assign.expr->u.type_annotation.expr, &literal_val)) {
                    uint64_t uv = (uint64_t)literal_val;
                    emit(c, OPC_PUSH_UINT64_CONST, (int)(uint32_t)uv, (int)(uint32_t)(uv >> 32));
                    emit(c, OPC_STORE_UINT64_VAR, var_idx, 0);
                    c->fn->var_type_tags[var_idx] = CAST_UINT64;
                } else {
                    c_expr(c, node->u.assign.expr);
                    emit(c, OPC_STORE_VAR, var_idx, 0);
                }
            }
            /* 优化0long：赋值为 <long>字面量 形式时，使用 OPC_PUSH_LONG_CONST + OPC_STORE_LONG_VAR
               支持正整数字面量和负整数字面量（AST_UNARY + OP_UNARY_MINUS） */
            else if(node->u.assign.expr && node->u.assign.expr->type == AST_TYPE_ANNOTATION &&
               node->u.assign.expr->u.type_annotation.cast_type == CAST_LONG &&
               node->u.assign.expr->u.type_annotation.expr) {
                long long literal_val;
                if (extract_int_literal_value(node->u.assign.expr->u.type_annotation.expr, &literal_val)) {
                    emit(c, OPC_PUSH_LONG_CONST, (int)literal_val, 0);
                    emit(c, OPC_STORE_LONG_VAR, var_idx, 0);
                    c->fn->var_type_tags[var_idx] = CAST_LONG;
                } else {
                    c_expr(c, node->u.assign.expr);
                    emit(c, OPC_STORE_VAR, var_idx, 0);
                }
            }
            /* 优化0ulong：赋值为 <ulong>字面量 形式时，使用 OPC_PUSH_ULONG_CONST + OPC_STORE_ULONG_VAR */
            else if(node->u.assign.expr && node->u.assign.expr->type == AST_TYPE_ANNOTATION &&
               node->u.assign.expr->u.type_annotation.cast_type == CAST_ULONG &&
               node->u.assign.expr->u.type_annotation.expr) {
                long long literal_val;
                if (extract_int_literal_value(node->u.assign.expr->u.type_annotation.expr, &literal_val)) {
                    emit(c, OPC_PUSH_ULONG_CONST, (int)(unsigned long)literal_val, 0);
                    emit(c, OPC_STORE_ULONG_VAR, var_idx, 0);
                    c->fn->var_type_tags[var_idx] = CAST_ULONG;
                } else {
                    c_expr(c, node->u.assign.expr);
                    emit(c, OPC_STORE_VAR, var_idx, 0);
                }
            }
            /* 优化0size_t：赋值为 <size_t>字面量 形式时，使用 OPC_PUSH_SIZE_T_CONST + OPC_STORE_SIZE_T_VAR */
            else if(node->u.assign.expr && node->u.assign.expr->type == AST_TYPE_ANNOTATION &&
               node->u.assign.expr->u.type_annotation.cast_type == CAST_SIZE_T &&
               node->u.assign.expr->u.type_annotation.expr) {
                long long literal_val;
                if (extract_int_literal_value(node->u.assign.expr->u.type_annotation.expr, &literal_val)) {
                    emit(c, OPC_PUSH_SIZE_T_CONST, (int)(size_t)literal_val, 0);
                    emit(c, OPC_STORE_SIZE_T_VAR, var_idx, 0);
                    c->fn->var_type_tags[var_idx] = CAST_SIZE_T;
                } else {
                    c_expr(c, node->u.assign.expr);
                    emit(c, OPC_STORE_VAR, var_idx, 0);
                }
            }
            /* 优化0ssize_t：赋值为 <ssize_t>字面量 形式时，使用 OPC_PUSH_SSIZE_T_CONST + OPC_STORE_SSIZE_T_VAR
               支持正整数字面量和负整数字面量（AST_UNARY + OP_UNARY_MINUS） */
            else if(node->u.assign.expr && node->u.assign.expr->type == AST_TYPE_ANNOTATION &&
               node->u.assign.expr->u.type_annotation.cast_type == CAST_SSIZE_T &&
               node->u.assign.expr->u.type_annotation.expr) {
                long long literal_val;
                if (extract_int_literal_value(node->u.assign.expr->u.type_annotation.expr, &literal_val)) {
                    emit(c, OPC_PUSH_SSIZE_T_CONST, (int)literal_val, 0);
                    emit(c, OPC_STORE_SSIZE_T_VAR, var_idx, 0);
                    c->fn->var_type_tags[var_idx] = CAST_SSIZE_T;
                } else {
                    c_expr(c, node->u.assign.expr);
                    emit(c, OPC_STORE_VAR, var_idx, 0);
                }
            }
            /* 优化1：赋值为 <int>arr[idx] 形式时，使用 OPC_INT_ARRAY_GET + OPC_STORE_INT_VAR
               零包装零重复提取，直接从 int 类型化数组读取并存储到 int 变量 */
            else if(node->u.assign.expr && node->u.assign.expr->type == AST_TYPE_ANNOTATION &&
               node->u.assign.expr->u.type_annotation.cast_type == CAST_INT &&
               node->u.assign.expr->u.type_annotation.expr &&
               node->u.assign.expr->u.type_annotation.expr->type == AST_INDEX) {
                AstNode* index_node = node->u.assign.expr->u.type_annotation.expr;
                AstNode* arr = index_node->u.index.arr;
                AstNode* idx = index_node->u.index.idx;
                /* 编译 arr 和 idx（压入 Value 栈） */
                c_expr(c, arr);
                c_expr(c, idx);
                /* OPC_INT_ARRAY_GET：直接读取 int 值，压入 int 栈，零包装 */
                emit(c, OPC_INT_ARRAY_GET, 0, 0);
                /* OPC_STORE_INT_VAR：从 int 栈弹出，存储到 int_vals，零重复提取 */
                emit(c, OPC_STORE_INT_VAR, var_idx, 0);
                /* 记录变量类型标记为 int */
                c->fn->var_type_tags[var_idx] = CAST_INT;
            }
            /* 优化1b：赋值为 <double>arr[idx] 形式时，使用 OPC_DOUBLE_ARRAY_GET + OPC_STORE_DOUBLE_VAR
               零包装零重复提取，直接从 double 类型化数组读取并存储到 double 变量 */
            else if(node->u.assign.expr && node->u.assign.expr->type == AST_TYPE_ANNOTATION &&
               node->u.assign.expr->u.type_annotation.cast_type == CAST_DOUBLE &&
               node->u.assign.expr->u.type_annotation.expr &&
               node->u.assign.expr->u.type_annotation.expr->type == AST_INDEX) {
                AstNode* index_node = node->u.assign.expr->u.type_annotation.expr;
                AstNode* arr = index_node->u.index.arr;
                AstNode* idx = index_node->u.index.idx;
                /* 编译 arr 和 idx（压入 Value 栈） */
                c_expr(c, arr);
                c_expr(c, idx);
                /* OPC_DOUBLE_ARRAY_GET：直接读取 double 值，压入 double 栈，零包装 */
                emit(c, OPC_DOUBLE_ARRAY_GET, 0, 0);
                /* OPC_STORE_DOUBLE_VAR：从 double 栈弹出，存储到 double_vals，零重复提取 */
                emit(c, OPC_STORE_DOUBLE_VAR, var_idx, 0);
                /* 记录变量类型标记为 double */
                c->fn->var_type_tags[var_idx] = 1; /* CAST_DOUBLE */
            }
            /* 优化1c：赋值为 <float>arr[idx] 形式时，使用 OPC_FLOAT_ARRAY_GET + OPC_STORE_FLOAT_VAR
               零包装零重复提取，直接从 float 类型化数组读取并存储到 float 变量 */
            else if(node->u.assign.expr && node->u.assign.expr->type == AST_TYPE_ANNOTATION &&
               node->u.assign.expr->u.type_annotation.cast_type == CAST_FLOAT &&
               node->u.assign.expr->u.type_annotation.expr &&
               node->u.assign.expr->u.type_annotation.expr->type == AST_INDEX) {
                AstNode* index_node = node->u.assign.expr->u.type_annotation.expr;
                AstNode* arr = index_node->u.index.arr;
                AstNode* idx = index_node->u.index.idx;
                /* 编译 arr 和 idx（压入 Value 栈） */
                c_expr(c, arr);
                c_expr(c, idx);
                /* OPC_FLOAT_ARRAY_GET：直接读取 float 值，压入 float 栈，零包装 */
                emit(c, OPC_FLOAT_ARRAY_GET, 0, 0);
                /* OPC_STORE_FLOAT_VAR：从 float 栈弹出，存储到 float_vals，零重复提取 */
                emit(c, OPC_STORE_FLOAT_VAR, var_idx, 0);
                /* 记录变量类型标记为 float */
                c->fn->var_type_tags[var_idx] = CAST_FLOAT;
            }
            /* 优化1d：赋值为 <uint32>arr[idx] 形式时，使用 OPC_UINT_ARRAY_GET + OPC_STORE_UINT_VAR
               零包装零重复提取，直接从 uint 类型化数组读取并存储到 uint 变量 */
            else if(node->u.assign.expr && node->u.assign.expr->type == AST_TYPE_ANNOTATION &&
               node->u.assign.expr->u.type_annotation.cast_type == CAST_UINT32 &&
               node->u.assign.expr->u.type_annotation.expr &&
               node->u.assign.expr->u.type_annotation.expr->type == AST_INDEX) {
                AstNode* index_node = node->u.assign.expr->u.type_annotation.expr;
                AstNode* arr = index_node->u.index.arr;
                AstNode* idx = index_node->u.index.idx;
                /* 编译 arr 和 idx（压入 Value 栈） */
                c_expr(c, arr);
                c_expr(c, idx);
                /* OPC_UINT_ARRAY_GET：直接读取 uint 值，压入 uint 栈，零包装 */
                emit(c, OPC_UINT_ARRAY_GET, 0, 0);
                /* OPC_STORE_UINT_VAR：从 uint 栈弹出，存储到 uint_vals，零重复提取 */
                emit(c, OPC_STORE_UINT_VAR, var_idx, 0);
                /* 记录变量类型标记为 uint */
                c->fn->var_type_tags[var_idx] = CAST_UINT32;
            }
            /* 优化0b：赋值为 <bool>表达式 形式时，使用专用路径
               如果是bool字面量，使用 OPC_PUSH_BOOL_CONST + OPC_STORE_BOOL_VAR
               否则编译表达式后从Value栈提取bool值，使用 OPC_STORE_BOOL_VAR */
            else if(node->u.assign.expr && node->u.assign.expr->type == AST_TYPE_ANNOTATION &&
               node->u.assign.expr->u.type_annotation.cast_type == CAST_BOOL &&
               node->u.assign.expr->u.type_annotation.expr) {
                AstNode* inner_expr = node->u.assign.expr->u.type_annotation.expr;
                if(inner_expr->type == AST_BOOL) {
                    /* bool字面量：直接压入bool栈，零检查零转换 */
                    int literal_val = inner_expr->u.bval ? 1 : 0;
                    emit(c, OPC_PUSH_BOOL_CONST, literal_val, 0);
                } else if(inner_expr->type == AST_INT) {
                    /* 整数字面量：非零为true，零为false */
                    int literal_val = inner_expr->u.inum ? 1 : 0;
                    emit(c, OPC_PUSH_BOOL_CONST, literal_val, 0);
                } else {
                    /* 其他表达式：编译表达式压入Value栈，然后转换为bool压入bool栈 */
                    c_expr(c, inner_expr);
                    emit(c, OPC_TO_BOOL, 0, 0);
                }
                /* OPC_STORE_BOOL_VAR：从 bool 栈弹出，存储到 bool_vals，零重复提取 */
                emit(c, OPC_STORE_BOOL_VAR, var_idx, 0);
                /* 记录变量类型标记为 bool */
                c->fn->var_type_tags[var_idx] = CAST_BOOL;
            }
            /* 优化2：上下文感知 - 赋值为 arr[idx] 且 arr 是 int 类型化数组时，自动感知为 int 类型
               即使左侧变量没有显式声明 <int>，也自动推导为 int 类型，并使用优化路径 */
            else if(node->u.assign.expr && node->u.assign.expr->type == AST_INDEX &&
                    node->u.assign.expr->u.index.arr &&
                    node->u.assign.expr->u.index.arr->type == AST_VAR) {
                const char* arr_name = node->u.assign.expr->u.index.arr->u.varname;
                int arr_idx = bf_sym(c->fn, arr_name);
                /* 检查数组变量是否标记为 int 类型化数组 */
                if(arr_idx >= 0 && c->fn->var_type_tags &&
                   c->fn->var_type_tags[arr_idx] == VAR_TYPE_INT_ARRAY) {
                    AstNode* arr = node->u.assign.expr->u.index.arr;
                    AstNode* idx = node->u.assign.expr->u.index.idx;
                    /* 编译 arr 和 idx（压入 Value 栈） */
                    c_expr(c, arr);
                    c_expr(c, idx);
                    /* OPC_INT_ARRAY_GET：直接读取 int 值，压入 int 栈，零包装 */
                    emit(c, OPC_INT_ARRAY_GET, 0, 0);
                    /* OPC_STORE_INT_VAR：从 int 栈弹出，存储到 int_vals，零重复提取 */
                    emit(c, OPC_STORE_INT_VAR, var_idx, 0);
                    /* 上下文感知：自动将左侧变量标记为 int 类型 */
                    c->fn->var_type_tags[var_idx] = CAST_INT;
                }
                /* 检查数组变量是否标记为 double 类型化数组 */
                else if(arr_idx >= 0 && c->fn->var_type_tags &&
                   c->fn->var_type_tags[arr_idx] == VAR_TYPE_DOUBLE_ARRAY) {
                    AstNode* arr = node->u.assign.expr->u.index.arr;
                    AstNode* idx = node->u.assign.expr->u.index.idx;
                    /* 编译 arr 和 idx（压入 Value 栈） */
                    c_expr(c, arr);
                    c_expr(c, idx);
                    /* OPC_DOUBLE_ARRAY_GET：直接读取 double 值，压入 double 栈，零包装 */
                    emit(c, OPC_DOUBLE_ARRAY_GET, 0, 0);
                    /* OPC_STORE_DOUBLE_VAR：从 double 栈弹出，存储到 double_vals，零重复提取 */
                    emit(c, OPC_STORE_DOUBLE_VAR, var_idx, 0);
                    /* 上下文感知：自动将左侧变量标记为 double 类型 */
                    c->fn->var_type_tags[var_idx] = 1; /* CAST_DOUBLE */
                }
                /* 检查数组变量是否标记为 float 类型化数组 */
                else if(arr_idx >= 0 && c->fn->var_type_tags &&
                   c->fn->var_type_tags[arr_idx] == VAR_TYPE_FLOAT_ARRAY) {
                    AstNode* arr = node->u.assign.expr->u.index.arr;
                    AstNode* idx = node->u.assign.expr->u.index.idx;
                    /* 编译 arr 和 idx（压入 Value 栈） */
                    c_expr(c, arr);
                    c_expr(c, idx);
                    /* OPC_FLOAT_ARRAY_GET：直接读取 float 值，压入 float 栈，零包装 */
                    emit(c, OPC_FLOAT_ARRAY_GET, 0, 0);
                    /* OPC_STORE_FLOAT_VAR：从 float 栈弹出，存储到 float_vals，零重复提取 */
                    emit(c, OPC_STORE_FLOAT_VAR, var_idx, 0);
                    /* 上下文感知：自动将左侧变量标记为 float 类型 */
                    c->fn->var_type_tags[var_idx] = CAST_FLOAT;
                }
                /* 检查数组变量是否标记为 uint 类型化数组 */
                else if(arr_idx >= 0 && c->fn->var_type_tags &&
                   c->fn->var_type_tags[arr_idx] == VAR_TYPE_UINT_ARRAY) {
                    AstNode* arr = node->u.assign.expr->u.index.arr;
                    AstNode* idx = node->u.assign.expr->u.index.idx;
                    /* 编译 arr 和 idx（压入 Value 栈） */
                    c_expr(c, arr);
                    c_expr(c, idx);
                    /* OPC_UINT_ARRAY_GET：直接读取 uint 值，压入 uint 栈，零包装 */
                    emit(c, OPC_UINT_ARRAY_GET, 0, 0);
                    /* OPC_STORE_UINT_VAR：从 uint 栈弹出，存储到 uint_vals，零重复提取 */
                    emit(c, OPC_STORE_UINT_VAR, var_idx, 0);
                    /* 上下文感知：自动将左侧变量标记为 uint 类型 */
                    c->fn->var_type_tags[var_idx] = CAST_UINT32;
                } else {
                    c_expr(c, node->u.assign.expr);
                    emit(c, OPC_STORE_VAR, var_idx, 0);
                }
            }
            /* 优化3.5：上下文感知 - 赋值为算术运算结果 b = a + c
               根据算术运算的结果类型自动推导左侧变量类型，并使用专用存储指令，零转换开销 */
            else if(node->u.assign.expr && node->u.assign.expr->type == AST_BINOP) {
                AstNode* binop = node->u.assign.expr;
                BinOp bop = binop->u.bin.op;
                int is_arith = (bop == OP_ADD || bop == OP_SUB || bop == OP_MUL || bop == OP_DIV || bop == OP_MOD);
                /* 只处理算术运算，比较运算的结果是bool类型，走通用路径 */
                if(is_arith) {
                    int left_is_int = is_int_var(c, binop->u.bin.left);
                    int right_is_int = is_int_var(c, binop->u.bin.right);
                    int left_is_uint = is_uint_var(c, binop->u.bin.left);
                    int right_is_uint = is_uint_var(c, binop->u.bin.right);
                    int left_is_double = is_double_var(c, binop->u.bin.left);
                    int right_is_double = is_double_var(c, binop->u.bin.right);
                    int left_is_float = is_float_var(c, binop->u.bin.left);
                    int right_is_float = is_float_var(c, binop->u.bin.right);
                    ExprType result_type = get_expr_type(c, binop);
                    if(result_type == EXPR_TYPE_INT) {
                        /* int类型算术运算：结果在int专用栈中，直接使用OPC_STORE_INT_VAR */
                        c_expr(c, binop);
                        emit(c, OPC_STORE_INT_VAR, var_idx, 0);
                        c->fn->var_type_tags[var_idx] = CAST_INT;
                    } else if(result_type == EXPR_TYPE_UINT) {
                        /* uint类型算术运算：结果在uint专用栈中，直接使用OPC_STORE_UINT_VAR */
                        c_expr(c, binop);
                        emit(c, OPC_STORE_UINT_VAR, var_idx, 0);
                        c->fn->var_type_tags[var_idx] = CAST_UINT32;
                    } else if(result_type == EXPR_TYPE_FLOAT) {
                        /* float类型算术运算：结果在float专用栈中，直接使用OPC_STORE_FLOAT_VAR */
                        c_expr(c, binop);
                        emit(c, OPC_STORE_FLOAT_VAR, var_idx, 0);
                        c->fn->var_type_tags[var_idx] = CAST_FLOAT;
                    } else if(result_type == EXPR_TYPE_DOUBLE) {
                        /* double类型算术运算：结果在double专用栈中，直接使用OPC_STORE_DOUBLE_VAR */
                        c_expr(c, binop);
                        emit(c, OPC_STORE_DOUBLE_VAR, var_idx, 0);
                        c->fn->var_type_tags[var_idx] = 1; /* CAST_DOUBLE */
                    } else if(result_type == EXPR_TYPE_LONG_LONG) {
                        /* long long类型算术运算：结果在long long专用栈中，直接使用OPC_STORE_LONG_LONG_VAR */
                        c_expr(c, binop);
                        emit(c, OPC_STORE_LONG_LONG_VAR, var_idx, 0);
                        c->fn->var_type_tags[var_idx] = CAST_LONGLONG;
                    } else if(result_type == EXPR_TYPE_LONG_DOUBLE) {
                        /* long double类型算术运算：结果在long double专用栈中，直接使用OPC_STORE_LONG_DOUBLE_VAR */
                        c_expr(c, binop);
                        emit(c, OPC_STORE_LONG_DOUBLE_VAR, var_idx, 0);
                        c->fn->var_type_tags[var_idx] = CAST_LONG_DOUBLE;
                    } else if(result_type == EXPR_TYPE_INT8) {
                        /* int8类型算术运算：结果在int8专用栈中，直接使用OPC_STORE_INT8_VAR */
                        c_expr(c, binop);
                        emit(c, OPC_STORE_INT8_VAR, var_idx, 0);
                        c->fn->var_type_tags[var_idx] = CAST_INT8;
                    } else if(result_type == EXPR_TYPE_INT16) {
                        c_expr(c, binop);
                        emit(c, OPC_STORE_INT16_VAR, var_idx, 0);
                        c->fn->var_type_tags[var_idx] = CAST_INT16;
                    } else if(result_type == EXPR_TYPE_INT) {
                        c_expr(c, binop);
                        emit(c, OPC_STORE_INT32_VAR, var_idx, 0);
                        c->fn->var_type_tags[var_idx] = CAST_INT32;
                    } else if(result_type == EXPR_TYPE_INT64) {
                        c_expr(c, binop);
                        emit(c, OPC_STORE_INT64_VAR, var_idx, 0);
                        c->fn->var_type_tags[var_idx] = CAST_INT64;
                    } else {
                        /* 无法推断结果类型，走通用路径 */
                        c_expr(c, node->u.assign.expr);
                        emit(c, OPC_STORE_VAR, var_idx, 0);
                    }
                } else {
                    /* 比较运算等：结果是bool类型，走通用路径 */
                    c_expr(c, node->u.assign.expr);
                    emit(c, OPC_STORE_VAR, var_idx, 0);
                }
            }
            /* 优化3：上下文感知 - 赋值为变量 b = a 且 a 是 int 类型时，自动感知为 int 类型
               即使左侧变量没有显式声明 <int>，也自动推导为 int 类型，并使用优化路径 */
            else if(node->u.assign.expr && node->u.assign.expr->type == AST_VAR) {
                const char* rhs_name = node->u.assign.expr->u.varname;
                int rhs_idx = bf_sym(c->fn, rhs_name);
                /* 检查右侧变量是否标记为 int 类型（CAST_INT = 2） */
                if(rhs_idx >= 0 && c->fn->var_type_tags &&
                   c->fn->var_type_tags[rhs_idx] == CAST_INT) {
                    /* OPC_LOAD_INT_VAR：直接从 int_vals 读取，零提取 */
                    emit(c, OPC_LOAD_INT_VAR, rhs_idx, 0);
                    /* OPC_STORE_INT_VAR：从 int 栈弹出，存储到 int_vals，零重复提取 */
                    emit(c, OPC_STORE_INT_VAR, var_idx, 0);
                    /* 上下文感知：自动将左侧变量标记为 int 类型 */
                    c->fn->var_type_tags[var_idx] = CAST_INT;
                }
                /* 检查右侧变量是否标记为 double 类型（CAST_DOUBLE = 1） */
                else if(rhs_idx >= 0 && c->fn->var_type_tags &&
                   c->fn->var_type_tags[rhs_idx] == CAST_DOUBLE) {
                    /* OPC_LOAD_DOUBLE_VAR：直接从 double_vals 读取，零提取 */
                    emit(c, OPC_LOAD_DOUBLE_VAR, rhs_idx, 0);
                    /* OPC_STORE_DOUBLE_VAR：从 double 栈弹出，存储到 double_vals，零重复提取 */
                    emit(c, OPC_STORE_DOUBLE_VAR, var_idx, 0);
                    /* 上下文感知：自动将左侧变量标记为 double 类型 */
                    c->fn->var_type_tags[var_idx] = 1; /* CAST_DOUBLE */
                }
                /* 检查右侧变量是否标记为 float 类型（CAST_FLOAT） */
                else if(rhs_idx >= 0 && c->fn->var_type_tags &&
                   c->fn->var_type_tags[rhs_idx] == CAST_FLOAT) {
                    /* OPC_LOAD_FLOAT_VAR：直接从 float_vals 读取，零提取 */
                    emit(c, OPC_LOAD_FLOAT_VAR, rhs_idx, 0);
                    /* OPC_STORE_FLOAT_VAR：从 float 栈弹出，存储到 float_vals，零重复提取 */
                    emit(c, OPC_STORE_FLOAT_VAR, var_idx, 0);
                    /* 上下文感知：自动将左侧变量标记为 float 类型 */
                    c->fn->var_type_tags[var_idx] = CAST_FLOAT;
                }
                /* 检查右侧变量是否标记为 uint 类型（CAST_UINT32） */
                else if(rhs_idx >= 0 && c->fn->var_type_tags &&
                   c->fn->var_type_tags[rhs_idx] == CAST_UINT32) {
                    /* OPC_LOAD_UINT_VAR：直接从 uint_vals 读取，零提取 */
                    emit(c, OPC_LOAD_UINT_VAR, rhs_idx, 0);
                    /* OPC_STORE_UINT_VAR：从 uint 栈弹出，存储到 uint_vals，零重复提取 */
                    emit(c, OPC_STORE_UINT_VAR, var_idx, 0);
                    /* 上下文感知：自动将左侧变量标记为 uint 类型 */
                    c->fn->var_type_tags[var_idx] = CAST_UINT32;
                }
                /* 检查右侧变量是否标记为 long long 类型（CAST_LONGLONG） */
                else if(rhs_idx >= 0 && c->fn->var_type_tags &&
                   c->fn->var_type_tags[rhs_idx] == CAST_LONGLONG) {
                    /* OPC_LOAD_LONG_LONG_VAR：直接从 longlong_vals 读取，零提取 */
                    emit(c, OPC_LOAD_LONG_LONG_VAR, rhs_idx, 0);
                    /* OPC_STORE_LONG_LONG_VAR：从 long long 栈弹出，存储到 longlong_vals，零重复提取 */
                    emit(c, OPC_STORE_LONG_LONG_VAR, var_idx, 0);
                    /* 上下文感知：自动将左侧变量标记为 long long 类型 */
                    c->fn->var_type_tags[var_idx] = CAST_LONGLONG;
                }
                /* 检查右侧变量是否标记为 long double 类型（CAST_LONG_DOUBLE） */
                else if(rhs_idx >= 0 && c->fn->var_type_tags &&
                   c->fn->var_type_tags[rhs_idx] == CAST_LONG_DOUBLE) {
                    /* OPC_LOAD_LONG_DOUBLE_VAR：直接从 longdouble_vals 读取，零提取 */
                    emit(c, OPC_LOAD_LONG_DOUBLE_VAR, rhs_idx, 0);
                    /* OPC_STORE_LONG_DOUBLE_VAR：从 long double 栈弹出，存储到 longdouble_vals，零重复提取 */
                    emit(c, OPC_STORE_LONG_DOUBLE_VAR, var_idx, 0);
                    /* 上下文感知：自动将左侧变量标记为 long double 类型 */
                    c->fn->var_type_tags[var_idx] = CAST_LONG_DOUBLE;
                }
                /* 检查右侧变量是否标记为 int8 类型（CAST_INT8） */
                else if(rhs_idx >= 0 && c->fn->var_type_tags &&
                   c->fn->var_type_tags[rhs_idx] == CAST_INT8) {
                    emit(c, OPC_LOAD_INT8_VAR, rhs_idx, 0);
                    emit(c, OPC_STORE_INT8_VAR, var_idx, 0);
                    c->fn->var_type_tags[var_idx] = CAST_INT8;
                }
                /* 检查右侧变量是否标记为 int16 类型（CAST_INT16） */
                else if(rhs_idx >= 0 && c->fn->var_type_tags &&
                   c->fn->var_type_tags[rhs_idx] == CAST_INT16) {
                    emit(c, OPC_LOAD_INT16_VAR, rhs_idx, 0);
                    emit(c, OPC_STORE_INT16_VAR, var_idx, 0);
                    c->fn->var_type_tags[var_idx] = CAST_INT16;
                }
                /* 检查右侧变量是否标记为 int32 类型（CAST_INT32） */
                else if(rhs_idx >= 0 && c->fn->var_type_tags &&
                   c->fn->var_type_tags[rhs_idx] == CAST_INT32) {
                    emit(c, OPC_LOAD_INT32_VAR, rhs_idx, 0);
                    emit(c, OPC_STORE_INT32_VAR, var_idx, 0);
                    c->fn->var_type_tags[var_idx] = CAST_INT32;
                }
                /* 检查右侧变量是否标记为 int64 类型（CAST_INT64） */
                else if(rhs_idx >= 0 && c->fn->var_type_tags &&
                   c->fn->var_type_tags[rhs_idx] == CAST_INT64) {
                    emit(c, OPC_LOAD_INT64_VAR, rhs_idx, 0);
                    emit(c, OPC_STORE_INT64_VAR, var_idx, 0);
                    c->fn->var_type_tags[var_idx] = CAST_INT64;
                }
                /* 检查右侧变量是否标记为 uint8 类型（CAST_UINT8） */
                else if(rhs_idx >= 0 && c->fn->var_type_tags &&
                   c->fn->var_type_tags[rhs_idx] == CAST_UINT8) {
                    emit(c, OPC_LOAD_UINT8_VAR, rhs_idx, 0);
                    emit(c, OPC_STORE_UINT8_VAR, var_idx, 0);
                    c->fn->var_type_tags[var_idx] = CAST_UINT8;
                }
                /* 检查右侧变量是否标记为 uint16 类型（CAST_UINT16） */
                else if(rhs_idx >= 0 && c->fn->var_type_tags &&
                   c->fn->var_type_tags[rhs_idx] == CAST_UINT16) {
                    emit(c, OPC_LOAD_UINT16_VAR, rhs_idx, 0);
                    emit(c, OPC_STORE_UINT16_VAR, var_idx, 0);
                    c->fn->var_type_tags[var_idx] = CAST_UINT16;
                }
                /* 检查右侧变量是否标记为 uint64 类型（CAST_UINT64） */
                else if(rhs_idx >= 0 && c->fn->var_type_tags &&
                   c->fn->var_type_tags[rhs_idx] == CAST_UINT64) {
                    emit(c, OPC_LOAD_UINT64_VAR, rhs_idx, 0);
                    emit(c, OPC_STORE_UINT64_VAR, var_idx, 0);
                    c->fn->var_type_tags[var_idx] = CAST_UINT64;
                }
                /* 检查右侧变量是否标记为 long 类型（CAST_LONG） */
                else if(rhs_idx >= 0 && c->fn->var_type_tags &&
                   c->fn->var_type_tags[rhs_idx] == CAST_LONG) {
                    emit(c, OPC_LOAD_LONG_VAR, rhs_idx, 0);
                    emit(c, OPC_STORE_LONG_VAR, var_idx, 0);
                    c->fn->var_type_tags[var_idx] = CAST_LONG;
                }
                /* 检查右侧变量是否标记为 ulong 类型（CAST_ULONG） */
                else if(rhs_idx >= 0 && c->fn->var_type_tags &&
                   c->fn->var_type_tags[rhs_idx] == CAST_ULONG) {
                    emit(c, OPC_LOAD_ULONG_VAR, rhs_idx, 0);
                    emit(c, OPC_STORE_ULONG_VAR, var_idx, 0);
                    c->fn->var_type_tags[var_idx] = CAST_ULONG;
                }
                /* 检查右侧变量是否标记为 size_t 类型（CAST_SIZE_T） */
                else if(rhs_idx >= 0 && c->fn->var_type_tags &&
                   c->fn->var_type_tags[rhs_idx] == CAST_SIZE_T) {
                    emit(c, OPC_LOAD_SIZE_T_VAR, rhs_idx, 0);
                    emit(c, OPC_STORE_SIZE_T_VAR, var_idx, 0);
                    c->fn->var_type_tags[var_idx] = CAST_SIZE_T;
                }
                /* 检查右侧变量是否标记为 ssize_t 类型（CAST_SSIZE_T） */
                else if(rhs_idx >= 0 && c->fn->var_type_tags &&
                   c->fn->var_type_tags[rhs_idx] == CAST_SSIZE_T) {
                    emit(c, OPC_LOAD_SSIZE_T_VAR, rhs_idx, 0);
                    emit(c, OPC_STORE_SSIZE_T_VAR, var_idx, 0);
                    c->fn->var_type_tags[var_idx] = CAST_SSIZE_T;
                }
                /* 检查右侧变量是否标记为 bool 类型（CAST_BOOL） */
                else if(rhs_idx >= 0 && c->fn->var_type_tags &&
                   c->fn->var_type_tags[rhs_idx] == CAST_BOOL) {
                    emit(c, OPC_LOAD_BOOL_VAR, rhs_idx, 0);
                    emit(c, OPC_STORE_BOOL_VAR, var_idx, 0);
                    c->fn->var_type_tags[var_idx] = CAST_BOOL;
                }
                /* 检查右侧变量是否标记为 char 类型（CAST_CHAR） */
                else if(rhs_idx >= 0 && c->fn->var_type_tags &&
                   c->fn->var_type_tags[rhs_idx] == CAST_CHAR) {
                    emit(c, OPC_LOAD_CHAR_VAR, rhs_idx, 0);
                    emit(c, OPC_STORE_CHAR_VAR, var_idx, 0);
                    c->fn->var_type_tags[var_idx] = CAST_CHAR;
                }
                /* 检查右侧变量是否标记为 byte 类型（CAST_BYTE） */
                else if(rhs_idx >= 0 && c->fn->var_type_tags &&
                   c->fn->var_type_tags[rhs_idx] == CAST_BYTE) {
                    emit(c, OPC_LOAD_BYTE_VAR, rhs_idx, 0);
                    emit(c, OPC_STORE_BYTE_VAR, var_idx, 0);
                    c->fn->var_type_tags[var_idx] = CAST_BYTE;
                } else {
                    /* 通用赋值路径：根据右操作数类型选择对应的存储指令
                       如果右操作数是已知类型的运算结果（在专用栈中），使用专用存储指令
                       否则使用通用的 OPC_STORE_VAR（从 Value 栈读取） */
                    ExprType rhs_expr_type = arith_get_expr_type(c, node->u.assign.expr);
                    c_expr(c, node->u.assign.expr);
                    switch(rhs_expr_type) {
                        case EXPR_TYPE_INT: emit(c, OPC_STORE_INT_VAR, var_idx, 0); c->fn->var_type_tags[var_idx] = CAST_INT; break;
                        case EXPR_TYPE_UINT: emit(c, OPC_STORE_UINT_VAR, var_idx, 0); c->fn->var_type_tags[var_idx] = CAST_UINT32; break;
                        case EXPR_TYPE_FLOAT: emit(c, OPC_STORE_FLOAT_VAR, var_idx, 0); c->fn->var_type_tags[var_idx] = CAST_FLOAT; break;
                        case EXPR_TYPE_DOUBLE: emit(c, OPC_STORE_DOUBLE_VAR, var_idx, 0); c->fn->var_type_tags[var_idx] = 1 /* CAST_DOUBLE */; break;
                        case EXPR_TYPE_LONG_LONG: emit(c, OPC_STORE_LONG_LONG_VAR, var_idx, 0); c->fn->var_type_tags[var_idx] = CAST_LONGLONG; break;
                        case EXPR_TYPE_LONG_DOUBLE: emit(c, OPC_STORE_LONG_DOUBLE_VAR, var_idx, 0); c->fn->var_type_tags[var_idx] = CAST_LONG_DOUBLE; break;
                        default: emit(c, OPC_STORE_VAR, var_idx, 0); break;
                    }
                }
            } else {
                /* 通用赋值路径：根据右操作数类型选择对应的存储指令 */
                ExprType rhs_expr_type2 = arith_get_expr_type(c, node->u.assign.expr);
                c_expr(c, node->u.assign.expr);
                switch(rhs_expr_type2) {
                    case EXPR_TYPE_INT: emit(c, OPC_STORE_INT_VAR, var_idx, 0); c->fn->var_type_tags[var_idx] = CAST_INT; break;
                    case EXPR_TYPE_UINT: emit(c, OPC_STORE_UINT_VAR, var_idx, 0); c->fn->var_type_tags[var_idx] = CAST_UINT32; break;
                    case EXPR_TYPE_FLOAT: emit(c, OPC_STORE_FLOAT_VAR, var_idx, 0); c->fn->var_type_tags[var_idx] = CAST_FLOAT; break;
                    case EXPR_TYPE_DOUBLE: emit(c, OPC_STORE_DOUBLE_VAR, var_idx, 0); c->fn->var_type_tags[var_idx] = 1 /* CAST_DOUBLE */; break;
                    case EXPR_TYPE_LONG_LONG: emit(c, OPC_STORE_LONG_LONG_VAR, var_idx, 0); c->fn->var_type_tags[var_idx] = CAST_LONGLONG; break;
                    case EXPR_TYPE_LONG_DOUBLE: emit(c, OPC_STORE_LONG_DOUBLE_VAR, var_idx, 0); c->fn->var_type_tags[var_idx] = CAST_LONG_DOUBLE; break;
                    default: emit(c, OPC_STORE_VAR, var_idx, 0); break;
                }
            }
                        break;
        }
        case AST_BINOP: {
            Value fv;
            if(fold_const(c, node, &fv)) {
                emit(c, OPC_LOAD_CONST, bf_const(c->fn, fv), 0);
                break;
            }
            BinOp bop = node->u.bin.op;
            if(bop == OP_LOGIC_AND || bop == OP_LOGIC_OR) {
                // 短路求值：&& 假跳过右；|| 真跳过右
                c_expr(c, node->u.bin.left);
                OpCode jop = (bop == OP_LOGIC_AND) ? OPC_JMP_IF_FALSE : OPC_JMP_IF_TRUE;
                int jskip = bf_emit_here(c->fn, jop, -1, 0);   // 弹左值
                c_expr(c, node->u.bin.right);
                emit(c, OPC_TO_BOOL, 0, 0);                    // 右值 → bool
                int jend = bf_emit_here(c->fn, OPC_JMP, -1, 0);
                bf_patch(c->fn, jskip, c->fn->code_len);       // 短路路径：
                emit(c, OPC_LOAD_CONST,
                     bf_const(c->fn, lumyr_make_bool(bop == OP_LOGIC_OR)), 0);
                bf_patch(c->fn, jend, c->fn->code_len);
                break;
            }
            /* 优化：int 类型专用算术/比较运算指令（零检查零转换零 Value 开销）
               如果左右操作数都是声明为 int 的变量，使用 OPC_INT_ADD 等专用指令，
               直接从 int 专用栈弹出两个 int，运算后结果压回 int 专用栈，完全不涉及 Value 栈 */
            int left_is_int = is_int_var(c, node->u.bin.left);
            int right_is_int = is_int_var(c, node->u.bin.right);
            int is_int_arith = (bop == OP_ADD || bop == OP_SUB || bop == OP_MUL || bop == OP_DIV || bop == OP_MOD);
            int is_int_cmp = (bop == OP_GT || bop == OP_LT || bop == OP_GE || bop == OP_LE || bop == OP_EQ || bop == OP_NE);
            int left_is_uint = is_uint_var(c, node->u.bin.left);
            int right_is_uint = is_uint_var(c, node->u.bin.right);
            int is_uint_arith = (bop == OP_ADD || bop == OP_SUB || bop == OP_MUL || bop == OP_DIV || bop == OP_MOD);
            int is_uint_cmp = (bop == OP_GT || bop == OP_LT || bop == OP_GE || bop == OP_LE || bop == OP_EQ || bop == OP_NE);
            int left_is_double = is_double_var(c, node->u.bin.left);
            int right_is_double = is_double_var(c, node->u.bin.right);
            int is_double_arith = (bop == OP_ADD || bop == OP_SUB || bop == OP_MUL || bop == OP_DIV);
            int is_double_cmp = (bop == OP_GT || bop == OP_LT || bop == OP_GE || bop == OP_LE || bop == OP_EQ || bop == OP_NE);
            int left_is_float = is_float_var(c, node->u.bin.left);
            int right_is_float = is_float_var(c, node->u.bin.right);
            int is_float_arith = (bop == OP_ADD || bop == OP_SUB || bop == OP_MUL || bop == OP_DIV);
            int is_float_cmp = (bop == OP_GT || bop == OP_LT || bop == OP_GE || bop == OP_LE || bop == OP_EQ || bop == OP_NE);
            int left_is_long_long = is_long_long_var(c, node->u.bin.left);
            int right_is_long_long = is_long_long_var(c, node->u.bin.right);
            int is_long_long_arith = (bop == OP_ADD || bop == OP_SUB || bop == OP_MUL || bop == OP_DIV || bop == OP_MOD);
            int is_long_long_cmp = (bop == OP_GT || bop == OP_LT || bop == OP_GE || bop == OP_LE || bop == OP_EQ || bop == OP_NE);
            int left_is_long_double = is_long_double_var(c, node->u.bin.left);
            int right_is_long_double = is_long_double_var(c, node->u.bin.right);
            int is_long_double_arith = (bop == OP_ADD || bop == OP_SUB || bop == OP_MUL || bop == OP_DIV);
            int is_long_double_cmp = (bop == OP_GT || bop == OP_LT || bop == OP_GE || bop == OP_LE || bop == OP_EQ || bop == OP_NE);
            int left_is_int8 = is_int8_var(c, node->u.bin.left);
            int right_is_int8 = is_int8_var(c, node->u.bin.right);
            int is_int8_arith = (bop == OP_ADD || bop == OP_SUB || bop == OP_MUL || bop == OP_DIV || bop == OP_MOD);
            int is_int8_cmp = (bop == OP_GT || bop == OP_LT || bop == OP_GE || bop == OP_LE || bop == OP_EQ || bop == OP_NE);
            int left_is_int16 = is_int16_var(c, node->u.bin.left);
            int right_is_int16 = is_int16_var(c, node->u.bin.right);
            int is_int16_arith = (bop == OP_ADD || bop == OP_SUB || bop == OP_MUL || bop == OP_DIV || bop == OP_MOD);
            int is_int16_cmp = (bop == OP_GT || bop == OP_LT || bop == OP_GE || bop == OP_LE || bop == OP_EQ || bop == OP_NE);
            int left_is_int32 = is_int32_var(c, node->u.bin.left);
            int right_is_int32 = is_int32_var(c, node->u.bin.right);
            int is_int32_arith = (bop == OP_ADD || bop == OP_SUB || bop == OP_MUL || bop == OP_DIV || bop == OP_MOD);
            int is_int32_cmp = (bop == OP_GT || bop == OP_LT || bop == OP_GE || bop == OP_LE || bop == OP_EQ || bop == OP_NE);
            int left_is_int64 = is_int64_var(c, node->u.bin.left);
            int right_is_int64 = is_int64_var(c, node->u.bin.right);
            int is_int64_arith = (bop == OP_ADD || bop == OP_SUB || bop == OP_MUL || bop == OP_DIV || bop == OP_MOD);
            int is_int64_cmp = (bop == OP_GT || bop == OP_LT || bop == OP_GE || bop == OP_LE || bop == OP_EQ || bop == OP_NE);
            if(left_is_int && right_is_int && (is_int_arith || is_int_cmp)) {
                /* 编译左右操作数（使用 int 专用路径，压入 int 栈） */
                int left_idx = bf_sym(c->fn, node->u.bin.left->u.varname);
                int right_idx = bf_sym(c->fn, node->u.bin.right->u.varname);
                emit(c, OPC_LOAD_INT_VAR, left_idx, 0);
                emit(c, OPC_LOAD_INT_VAR, right_idx, 0);
                if(is_int_arith) {
                    /* 生成 int 专用算术运算指令 */
                    static const OpCode int_arith_map[] = {
                        [OP_ADD] = OPC_INT_ADD, [OP_SUB] = OPC_INT_SUB, [OP_MUL] = OPC_INT_MUL,
                        [OP_DIV] = OPC_INT_DIV, [OP_MOD] = OPC_INT_MOD,
                    };
                    emit(c, int_arith_map[bop], 0, 0);
                    /* 结果保持在 int 专用栈中，后续操作通过上下文感知来处理
                       （赋值给 int 变量时使用 OPC_STORE_INT_VAR，print 时使用 OPC_PRINT_INT 等） */
                } else {
                    /* 生成 int 专用比较运算指令，比较结果(bool)直接压入 Value 栈 */
                    static const OpCode int_cmp_map[] = {
                        [OP_GT] = OPC_INT_GT, [OP_LT] = OPC_INT_LT, [OP_GE] = OPC_INT_GE,
                        [OP_LE] = OPC_INT_LE, [OP_EQ] = OPC_INT_EQ, [OP_NE] = OPC_INT_NE,
                    };
                    emit(c, int_cmp_map[bop], 0, 0);
                }
            } else if(left_is_uint && right_is_uint && (is_uint_arith || is_uint_cmp)) {
                /* 优化：uint 类型专用算术/比较运算指令（零检查零转换零 Value 开销）
                   如果左右操作数都是声明为 uint 的变量，使用 OPC_UINT_ADD 等专用指令，
                   直接从 uint 专用栈弹出两个 uint，运算后结果压回 uint 专用栈，完全不涉及 Value 栈 */
                int left_idx = bf_sym(c->fn, node->u.bin.left->u.varname);
                int right_idx = bf_sym(c->fn, node->u.bin.right->u.varname);
                emit(c, OPC_LOAD_UINT_VAR, left_idx, 0);
                emit(c, OPC_LOAD_UINT_VAR, right_idx, 0);
                if(is_uint_arith) {
                    /* 生成 uint 专用算术运算指令 */
                    static const OpCode uint_arith_map[] = {
                        [OP_ADD] = OPC_UINT_ADD, [OP_SUB] = OPC_UINT_SUB, [OP_MUL] = OPC_UINT_MUL,
                        [OP_DIV] = OPC_UINT_DIV, [OP_MOD] = OPC_UINT_MOD,
                    };
                    emit(c, uint_arith_map[bop], 0, 0);
                    /* 结果保持在 uint 专用栈中，后续操作通过上下文感知来处理
                       （赋值给 uint 变量时使用 OPC_STORE_UINT_VAR，print 时使用 OPC_PRINT_UINT 等） */
                } else {
                    /* 生成 uint 专用比较运算指令，比较结果(bool)直接压入 Value 栈 */
                    static const OpCode uint_cmp_map[] = {
                        [OP_GT] = OPC_UINT_GT, [OP_LT] = OPC_UINT_LT, [OP_GE] = OPC_UINT_GE,
                        [OP_LE] = OPC_UINT_LE, [OP_EQ] = OPC_UINT_EQ, [OP_NE] = OPC_UINT_NE,
                    };
                    emit(c, uint_cmp_map[bop], 0, 0);
                }
            } else if(left_is_long_long && right_is_long_long && (is_long_long_arith || is_long_long_cmp)) {
                /* 优化：long long 类型专用算术/比较运算指令（零检查零转换零 Value 开销）
                   如果左右操作数都是声明为 long long 的变量，使用 OPC_LONG_LONG_ADD 等专用指令，
                   直接从 long long 专用栈弹出两个 long long，运算后结果压回 long long 专用栈，完全不涉及 Value 栈 */
                int left_idx = bf_sym(c->fn, node->u.bin.left->u.varname);
                int right_idx = bf_sym(c->fn, node->u.bin.right->u.varname);
                emit(c, OPC_LOAD_LONG_LONG_VAR, left_idx, 0);
                emit(c, OPC_LOAD_LONG_LONG_VAR, right_idx, 0);
                if(is_long_long_arith) {
                    /* 生成 long long 专用算术运算指令 */
                    static const OpCode long_long_arith_map[] = {
                        [OP_ADD] = OPC_LONG_LONG_ADD, [OP_SUB] = OPC_LONG_LONG_SUB, [OP_MUL] = OPC_LONG_LONG_MUL,
                        [OP_DIV] = OPC_LONG_LONG_DIV, [OP_MOD] = OPC_LONG_LONG_MOD,
                    };
                    emit(c, long_long_arith_map[bop], 0, 0);
                    /* 结果保持在 long long 专用栈中，后续操作通过上下文感知来处理
                       （赋值给 long long 变量时使用 OPC_STORE_LONG_LONG_VAR，print 时使用 OPC_PRINT_LONG_LONG 等） */
                } else {
                    /* 生成 long long 专用比较运算指令，比较结果(bool)直接压入 Value 栈 */
                    static const OpCode long_long_cmp_map[] = {
                        [OP_GT] = OPC_LONG_LONG_GT, [OP_LT] = OPC_LONG_LONG_LT, [OP_GE] = OPC_LONG_LONG_GE,
                        [OP_LE] = OPC_LONG_LONG_LE, [OP_EQ] = OPC_LONG_LONG_EQ, [OP_NE] = OPC_LONG_LONG_NE,
                    };
                    emit(c, long_long_cmp_map[bop], 0, 0);
                }
            } else if(left_is_double && right_is_double && (is_double_arith || is_double_cmp)) {
                /* 优化：double 类型专用算术/比较运算指令（零检查零转换零 Value 开销）
                   如果左右操作数都是声明为 double 的变量，使用 OPC_DOUBLE_ADD 等专用指令，
                   直接从 double 专用栈弹出两个 double，运算后结果压回 double 专用栈，完全不涉及 Value 栈 */
                int left_idx = bf_sym(c->fn, node->u.bin.left->u.varname);
                int right_idx = bf_sym(c->fn, node->u.bin.right->u.varname);
                emit(c, OPC_LOAD_DOUBLE_VAR, left_idx, 0);
                emit(c, OPC_LOAD_DOUBLE_VAR, right_idx, 0);
                if(is_double_arith) {
                    /* 生成 double 专用算术运算指令（注意：double 没有取模运算） */
                    static const OpCode double_arith_map[] = {
                        [OP_ADD] = OPC_DOUBLE_ADD, [OP_SUB] = OPC_DOUBLE_SUB, [OP_MUL] = OPC_DOUBLE_MUL,
                        [OP_DIV] = OPC_DOUBLE_DIV,
                    };
                    emit(c, double_arith_map[bop], 0, 0);
                    /* 结果保持在 double 专用栈中，后续操作通过上下文感知来处理
                       （赋值给 double 变量时使用 OPC_STORE_DOUBLE_VAR，print 时使用 OPC_PRINT_DOUBLE 等） */
                } else {
                    /* 生成 double 专用比较运算指令，比较结果(bool)直接压入 Value 栈 */
                    static const OpCode double_cmp_map[] = {
                        [OP_GT] = OPC_DOUBLE_GT, [OP_LT] = OPC_DOUBLE_LT, [OP_GE] = OPC_DOUBLE_GE,
                        [OP_LE] = OPC_DOUBLE_LE, [OP_EQ] = OPC_DOUBLE_EQ, [OP_NE] = OPC_DOUBLE_NE,
                    };
                    emit(c, double_cmp_map[bop], 0, 0);
                }
            }
            /* float 类型专用算术/比较运算指令（零检查零转换零 Value 开销） */
            else if(left_is_float && right_is_float && (is_float_arith || is_float_cmp)) {
                int left_idx = bf_sym(c->fn, node->u.bin.left->u.varname);
                int right_idx = bf_sym(c->fn, node->u.bin.right->u.varname);
                emit(c, OPC_LOAD_FLOAT_VAR, left_idx, 0);
                emit(c, OPC_LOAD_FLOAT_VAR, right_idx, 0);
                if(is_float_arith) {
                    static const OpCode float_arith_map[] = {
                        [OP_ADD] = OPC_FLOAT_ADD, [OP_SUB] = OPC_FLOAT_SUB, [OP_MUL] = OPC_FLOAT_MUL,
                        [OP_DIV] = OPC_FLOAT_DIV,
                    };
                    emit(c, float_arith_map[bop], 0, 0);
                } else {
                    static const OpCode float_cmp_map[] = {
                        [OP_GT] = OPC_FLOAT_GT, [OP_LT] = OPC_FLOAT_LT, [OP_GE] = OPC_FLOAT_GE,
                        [OP_LE] = OPC_FLOAT_LE, [OP_EQ] = OPC_FLOAT_EQ, [OP_NE] = OPC_FLOAT_NE,
                    };
                    emit(c, float_cmp_map[bop], 0, 0);
                }
            }
            /* long double 类型专用算术/比较运算指令（零检查零转换零 Value 开销） */
            else if(left_is_long_double && right_is_long_double && (is_long_double_arith || is_long_double_cmp)) {
                int left_idx = bf_sym(c->fn, node->u.bin.left->u.varname);
                int right_idx = bf_sym(c->fn, node->u.bin.right->u.varname);
                emit(c, OPC_LOAD_LONG_DOUBLE_VAR, left_idx, 0);
                emit(c, OPC_LOAD_LONG_DOUBLE_VAR, right_idx, 0);
                if(is_long_double_arith) {
                    static const OpCode long_double_arith_map[] = {
                        [OP_ADD] = OPC_LONG_DOUBLE_ADD, [OP_SUB] = OPC_LONG_DOUBLE_SUB, [OP_MUL] = OPC_LONG_DOUBLE_MUL,
                        [OP_DIV] = OPC_LONG_DOUBLE_DIV,
                    };
                    emit(c, long_double_arith_map[bop], 0, 0);
                } else {
                    static const OpCode long_double_cmp_map[] = {
                        [OP_GT] = OPC_LONG_DOUBLE_GT, [OP_LT] = OPC_LONG_DOUBLE_LT, [OP_GE] = OPC_LONG_DOUBLE_GE,
                        [OP_LE] = OPC_LONG_DOUBLE_LE, [OP_EQ] = OPC_LONG_DOUBLE_EQ, [OP_NE] = OPC_LONG_DOUBLE_NE,
                    };
                    emit(c, long_double_cmp_map[bop], 0, 0);
                }
            }
            /* int8 类型专用算术/比较运算指令（零检查零转换零 Value 开销） */
            else if(left_is_int8 && right_is_int8 && (is_int8_arith || is_int8_cmp)) {
                int left_idx = bf_sym(c->fn, node->u.bin.left->u.varname);
                int right_idx = bf_sym(c->fn, node->u.bin.right->u.varname);
                emit(c, OPC_LOAD_INT8_VAR, left_idx, 0);
                emit(c, OPC_LOAD_INT8_VAR, right_idx, 0);
                if(is_int8_arith) {
                    static const OpCode int8_arith_map[] = {
                        [OP_ADD] = OPC_INT8_ADD, [OP_SUB] = OPC_INT8_SUB, [OP_MUL] = OPC_INT8_MUL,
                        [OP_DIV] = OPC_INT8_DIV, [OP_MOD] = OPC_INT8_MOD,
                    };
                    emit(c, int8_arith_map[bop], 0, 0);
                } else {
                    static const OpCode int8_cmp_map[] = {
                        [OP_GT] = OPC_INT8_GT, [OP_LT] = OPC_INT8_LT, [OP_GE] = OPC_INT8_GE,
                        [OP_LE] = OPC_INT8_LE, [OP_EQ] = OPC_INT8_EQ, [OP_NE] = OPC_INT8_NE,
                    };
                    emit(c, int8_cmp_map[bop], 0, 0);
                }
            }
            /* int16 类型专用算术/比较运算指令 */
            else if(left_is_int16 && right_is_int16 && (is_int16_arith || is_int16_cmp)) {
                int left_idx = bf_sym(c->fn, node->u.bin.left->u.varname);
                int right_idx = bf_sym(c->fn, node->u.bin.right->u.varname);
                emit(c, OPC_LOAD_INT16_VAR, left_idx, 0);
                emit(c, OPC_LOAD_INT16_VAR, right_idx, 0);
                if(is_int16_arith) {
                    static const OpCode int16_arith_map[] = {
                        [OP_ADD] = OPC_INT16_ADD, [OP_SUB] = OPC_INT16_SUB, [OP_MUL] = OPC_INT16_MUL,
                        [OP_DIV] = OPC_INT16_DIV, [OP_MOD] = OPC_INT16_MOD,
                    };
                    emit(c, int16_arith_map[bop], 0, 0);
                } else {
                    static const OpCode int16_cmp_map[] = {
                        [OP_GT] = OPC_INT16_GT, [OP_LT] = OPC_INT16_LT, [OP_GE] = OPC_INT16_GE,
                        [OP_LE] = OPC_INT16_LE, [OP_EQ] = OPC_INT16_EQ, [OP_NE] = OPC_INT16_NE,
                    };
                    emit(c, int16_cmp_map[bop], 0, 0);
                }
            }
            /* int32 类型专用算术/比较运算指令 */
            else if(left_is_int32 && right_is_int32 && (is_int32_arith || is_int32_cmp)) {
                int left_idx = bf_sym(c->fn, node->u.bin.left->u.varname);
                int right_idx = bf_sym(c->fn, node->u.bin.right->u.varname);
                emit(c, OPC_LOAD_INT32_VAR, left_idx, 0);
                emit(c, OPC_LOAD_INT32_VAR, right_idx, 0);
                if(is_int32_arith) {
                    static const OpCode int32_arith_map[] = {
                        [OP_ADD] = OPC_INT32_ADD, [OP_SUB] = OPC_INT32_SUB, [OP_MUL] = OPC_INT32_MUL,
                        [OP_DIV] = OPC_INT32_DIV, [OP_MOD] = OPC_INT32_MOD,
                    };
                    emit(c, int32_arith_map[bop], 0, 0);
                } else {
                    static const OpCode int32_cmp_map[] = {
                        [OP_GT] = OPC_INT32_GT, [OP_LT] = OPC_INT32_LT, [OP_GE] = OPC_INT32_GE,
                        [OP_LE] = OPC_INT32_LE, [OP_EQ] = OPC_INT32_EQ, [OP_NE] = OPC_INT32_NE,
                    };
                    emit(c, int32_cmp_map[bop], 0, 0);
                }
            }
            /* int64 类型专用算术/比较运算指令 */
            else if(left_is_int64 && right_is_int64 && (is_int64_arith || is_int64_cmp)) {
                int left_idx = bf_sym(c->fn, node->u.bin.left->u.varname);
                int right_idx = bf_sym(c->fn, node->u.bin.right->u.varname);
                emit(c, OPC_LOAD_INT64_VAR, left_idx, 0);
                emit(c, OPC_LOAD_INT64_VAR, right_idx, 0);
                if(is_int64_arith) {
                    static const OpCode int64_arith_map[] = {
                        [OP_ADD] = OPC_INT64_ADD, [OP_SUB] = OPC_INT64_SUB, [OP_MUL] = OPC_INT64_MUL,
                        [OP_DIV] = OPC_INT64_DIV, [OP_MOD] = OPC_INT64_MOD,
                    };
                    emit(c, int64_arith_map[bop], 0, 0);
                } else {
                    static const OpCode int64_cmp_map[] = {
                        [OP_GT] = OPC_INT64_GT, [OP_LT] = OPC_INT64_LT, [OP_GE] = OPC_INT64_GE,
                        [OP_LE] = OPC_INT64_LE, [OP_EQ] = OPC_INT64_EQ, [OP_NE] = OPC_INT64_NE,
                    };
                    emit(c, int64_cmp_map[bop], 0, 0);
                }
            }
            /* 混合类型算术运算（类型提升，零包装零Value开销）
               类型提升规则：int/uint -> float -> double
               先把小类型转换为大类型，然后执行大类型的算术运算
               只要有一个操作数是Value类型才走通用，其他的都走专用 */
            else if(is_int_arith || is_uint_arith || is_float_arith || is_double_arith || is_long_long_arith) {
                int left_idx = -1, right_idx = -1;
                /* 使用 arith_get_expr_type 获取左右操作数的类型（支持嵌套表达式）
                   直接使用 ExprType 枚举，支持所有数据类型
                   只要有一个操作数是Value类型才走通用，其他的都走专用 */
                ExprType left_type = arith_get_expr_type(c, node->u.bin.left);
                ExprType right_type = arith_get_expr_type(c, node->u.bin.right);
                /* 如果是变量引用，获取变量索引 */
                if(node->u.bin.left->type == AST_VAR && left_type > 0) {
                    left_idx = bf_sym(c->fn, node->u.bin.left->u.varname);
                }
                if(node->u.bin.right->type == AST_VAR && right_type > 0) {
                    right_idx = bf_sym(c->fn, node->u.bin.right->u.varname);
                }
                /* 只有当左右操作数都是已知类型时，才使用混合类型优化 */
                if(left_type > 0 && right_type > 0) {
                    /* short 类型走通用路径：没有专用算术运算指令，强行优化会导致栈不匹配 */
                    if(left_type == EXPR_TYPE_SHORT || right_type == EXPR_TYPE_SHORT) {
                        c_expr(c, node->u.bin.left);
                        c_expr(c, node->u.bin.right);
                        static const OpCode map[] = {
                            [OP_ADD] = OPC_ADD, [OP_SUB] = OPC_SUB, [OP_MUL] = OPC_MUL, [OP_DIV] = OPC_DIV,
                            [OP_MOD] = OPC_MOD,
                        };
                        emit(c, map[bop], 0, 0);
                        break;
                    }
                    fprintf(stderr, "[DEBUG MIXED] left_type=%d, right_type=%d\n", left_type, right_type);
                    /* 类型提升规则：参考C语言标准的常用算术转换
                       1. long double 优先级最高
                       2. double 次之
                       3. float 再次之
                       4. 整数类型按位宽和符号性提升
                       ExprType 枚举的数值大小对应类型优先级，直接取较大值即可 */
                    ExprType result_type = (left_type > right_type) ? left_type : right_type;
                    /* 特殊处理：如果有一个是浮点类型，结果取浮点类型中优先级较高的 */
                    int left_is_float = (left_type >= EXPR_TYPE_FLOAT && left_type <= EXPR_TYPE_LONG_DOUBLE);
                    int right_is_float = (right_type >= EXPR_TYPE_FLOAT && right_type <= EXPR_TYPE_LONG_DOUBLE);
                    if(left_is_float || right_is_float) {
                        result_type = (left_type > right_type) ? left_type : right_type;
                    }
                    fprintf(stderr, "[DEBUG MIXED] result_type=%d\n", result_type);
                    /* 加载左操作数到对应专用栈
                       对于变量引用使用 OPC_LOAD_*_VAR，对于嵌套表达式直接递归编译（结果已在专用栈） */
                    if(node->u.bin.left->type == AST_VAR) {
                        emit(c, get_load_var_opcode(left_type), left_idx, 0);
                    } else {
                        /* 嵌套表达式：直接递归编译，结果已在专用栈中 */
                        c_expr(c, node->u.bin.left);
                    }
                    /* 左操作数类型提升 */
                    emit_mixed_type_promote(c, left_type, result_type);
                    /* 加载右操作数到对应专用栈
                       对于变量引用使用 OPC_LOAD_*_VAR，对于嵌套表达式直接递归编译（结果已在专用栈） */
                    if(node->u.bin.right->type == AST_VAR) {
                        emit(c, get_load_var_opcode(right_type), right_idx, 0);
                    } else {
                        /* 嵌套表达式：直接递归编译，结果已在专用栈中 */
                        c_expr(c, node->u.bin.right);
                    }
                    /* 右操作数类型提升 */
                    emit_mixed_type_promote(c, right_type, result_type);
                    /* 执行大类型的算术运算（结果在大类型专用栈中）
                       使用 ExprType 枚举判断类型，支持所有数据类型 */
                    if(result_type == EXPR_TYPE_INT) {
                        static const OpCode int_arith_map[] = {
                            [OP_ADD] = OPC_INT_ADD, [OP_SUB] = OPC_INT_SUB, [OP_MUL] = OPC_INT_MUL,
                            [OP_DIV] = OPC_INT_DIV, [OP_MOD] = OPC_INT_MOD,
                        };
                        emit(c, int_arith_map[bop], 0, 0);
                    } else if(result_type == EXPR_TYPE_UINT) {
                        static const OpCode uint_arith_map[] = {
                            [OP_ADD] = OPC_UINT_ADD, [OP_SUB] = OPC_UINT_SUB, [OP_MUL] = OPC_UINT_MUL,
                            [OP_DIV] = OPC_UINT_DIV, [OP_MOD] = OPC_UINT_MOD,
                        };
                        emit(c, uint_arith_map[bop], 0, 0);
                    } else if(result_type == EXPR_TYPE_FLOAT) {
                        static const OpCode float_arith_map[] = {
                            [OP_ADD] = OPC_FLOAT_ADD, [OP_SUB] = OPC_FLOAT_SUB, [OP_MUL] = OPC_FLOAT_MUL,
                            [OP_DIV] = OPC_FLOAT_DIV,
                        };
                        emit(c, float_arith_map[bop], 0, 0);
                    } else if(result_type == EXPR_TYPE_DOUBLE) {
                        static const OpCode double_arith_map[] = {
                            [OP_ADD] = OPC_DOUBLE_ADD, [OP_SUB] = OPC_DOUBLE_SUB, [OP_MUL] = OPC_DOUBLE_MUL,
                            [OP_DIV] = OPC_DOUBLE_DIV,
                        };
                        emit(c, double_arith_map[bop], 0, 0);
                    } else if(result_type == EXPR_TYPE_LONG_LONG) {
                        static const OpCode long_long_arith_map[] = {
                            [OP_ADD] = OPC_LONG_LONG_ADD, [OP_SUB] = OPC_LONG_LONG_SUB, [OP_MUL] = OPC_LONG_LONG_MUL,
                            [OP_DIV] = OPC_LONG_LONG_DIV, [OP_MOD] = OPC_LONG_LONG_MOD,
                        };
                        emit(c, long_long_arith_map[bop], 0, 0);
                    } else if(result_type == EXPR_TYPE_LONG_DOUBLE) {
                        static const OpCode long_double_arith_map[] = {
                            [OP_ADD] = OPC_LONG_DOUBLE_ADD, [OP_SUB] = OPC_LONG_DOUBLE_SUB, [OP_MUL] = OPC_LONG_DOUBLE_MUL,
                            [OP_DIV] = OPC_LONG_DOUBLE_DIV,
                        };
                        emit(c, long_double_arith_map[bop], 0, 0);
                    }
                    /* 结果保持在大类型专用栈中，后续操作通过上下文感知处理
                       （赋值时用 OPC_STORE_FLOAT_VAR/OPC_STORE_DOUBLE_VAR，print 时用 OPC_PRINT_FLOAT/OPC_PRINT_DOUBLE） */
                    break;
                }
                /* 左右操作数不都是已知类型，走通用路径 */
                c_expr(c, node->u.bin.left);
                c_expr(c, node->u.bin.right);
                static const OpCode map[] = {
                    [OP_ADD] = OPC_ADD, [OP_SUB] = OPC_SUB, [OP_MUL] = OPC_MUL, [OP_DIV] = OPC_DIV,
                    [OP_MOD] = OPC_MOD,
                };
                emit(c, map[bop], 0, 0);
                break;
            } else {
                /* 通用路径：编译左右操作数，生成通用指令 */
                c_expr(c, node->u.bin.left);
                c_expr(c, node->u.bin.right);
                static const OpCode map[] = {
                    [OP_ADD] = OPC_ADD, [OP_SUB] = OPC_SUB, [OP_MUL] = OPC_MUL, [OP_DIV] = OPC_DIV,
                    [OP_MOD] = OPC_MOD,
                    [OP_GT] = OPC_GT, [OP_LT] = OPC_LT, [OP_GE] = OPC_GE, [OP_LE] = OPC_LE,
                    [OP_EQ] = OPC_EQ, [OP_NE] = OPC_NE, [OP_IMPLEMENTS] = OPC_IMPLEMENTS,
                };
                emit(c, map[bop], 0, 0);
            }
            break;
        }
        case AST_UNARY: {
            AstNode* kid = node->u.uny.child;
            switch(node->u.uny.op) {
                case OP_PRE_INC:   emit(c, OPC_PRE_INC, bf_sym(c->fn, kid->u.varname), 0); break;
                case OP_POST_INC:  emit(c, OPC_POST_INC, bf_sym(c->fn, kid->u.varname), 0); break;
                case OP_PRE_DEC:   emit(c, OPC_PRE_DEC, bf_sym(c->fn, kid->u.varname), 0); break;
                case OP_POST_DEC:  emit(c, OPC_POST_DEC, bf_sym(c->fn, kid->u.varname), 0); break;
                case OP_UNARY_PLUS: {
                    Value fv;
                    if(fold_const(c, node, &fv)) { emit(c, OPC_LOAD_CONST, bf_const(c->fn, fv), 0); break; }
                    c_expr(c, kid); emit(c, OPC_POS, 0, 0); break;
                }
                case OP_UNARY_MINUS: {
                    Value fv;
                    if(fold_const(c, node, &fv)) { emit(c, OPC_LOAD_CONST, bf_const(c->fn, fv), 0); break; }
                    c_expr(c, kid); emit(c, OPC_NEG, 0, 0); break;
                }
                case OP_LOGIC_NOT: {
                    Value fv;
                    if(fold_const(c, node, &fv)) { emit(c, OPC_LOAD_CONST, bf_const(c->fn, fv), 0); break; }
                    c_expr(c, kid); emit(c, OPC_LOGIC_NOT, 0, 0); break;
                }
                default: break;
            }
            break;
        }
        case AST_CAST: {
            Value fv;
            if(fold_const(c, node, &fv)) {
                emit(c, OPC_LOAD_CONST, bf_const(c->fn, fv), 0);
                break;
            }
            c_expr(c, node->u.cast.child);
            static const OpCode cmap[] = {
                [CAST_INT] = OPC_CAST_INT, [CAST_DOUBLE] = OPC_CAST_DOUBLE,
                [CAST_CHAR] = OPC_CAST_CHAR, [CAST_BOOL] = OPC_CAST_BOOL,
                [CAST_STRING] = OPC_CAST_STRING, [CAST_ASCII] = OPC_CAST_ASCII,
                [CAST_BYTE] = OPC_CAST_BYTE,
                [CAST_INT8] = OPC_CAST_INT8, [CAST_INT16] = OPC_CAST_INT16,
                [CAST_INT32] = OPC_CAST_INT32, [CAST_INT64] = OPC_CAST_INT64,
                [CAST_UINT8] = OPC_CAST_UINT8, [CAST_UINT16] = OPC_CAST_UINT16,
                [CAST_UINT32] = OPC_CAST_UINT32, [CAST_UINT64] = OPC_CAST_UINT64,
                [CAST_LONG] = OPC_CAST_LONG, [CAST_LONGLONG] = OPC_CAST_LONGLONG,
                [CAST_FLOAT] = OPC_CAST_FLOAT,
                /* 补充 C 标准类型：复用现有操作码（运行时统一 long long/double 存储） */
                [CAST_ULONG] = OPC_CAST_UINT64,
                [CAST_UCHAR] = OPC_CAST_UINT8,
                [CAST_SHORT] = OPC_CAST_INT16,
                [CAST_USHORT] = OPC_CAST_UINT16,
                [CAST_SIZE_T] = OPC_CAST_UINT64,
                [CAST_SSIZE_T] = OPC_CAST_INT64,
                [CAST_LONG_DOUBLE] = OPC_CAST_DOUBLE,
                [CAST_PTR] = OPC_CAST_INT64,
            };
            /* CAST_VOID 特殊处理：直接返回 none */
            if(node->u.cast.cast_type == CAST_VOID) {
                emit(c, OPC_POP, 0, 0);
                emit(c, OPC_LOAD_CONST, bf_const(c->fn, val_none()), 0);
            } else {
                emit(c, cmap[node->u.cast.cast_type], 0, 0);
            }
            break;
        }
        case AST_INTERFACE_ANNOTATION: {
            /* 接口类型标注 <Interface>expr：给变量打接口引用类型标记
               只需要编译内部的表达式即可（不需要做类型转换） */
            c_expr(c, node->u.interface_annotation.expr);
            break;
        }
        case AST_TYPE_ANNOTATION: {
            /* 类型标注 <type>expr：给值打类型标记（等价 C 的类型声明）
               编译期常量：直接按目标类型构造，构造函数自身做 C 风格截断/扩展 */
            Value fv;
            if(fold_const(c, node->u.type_annotation.expr, &fv)) {
                CastKind ct = node->u.type_annotation.cast_type;
                long long llv = lumyr_extract_ll(fv);
                unsigned long long ullv = (unsigned long long)llv;
                double dv = value_as_number(fv);
                switch(ct) {
                    case CAST_VOID:       fv = val_none(); break;
                    case CAST_BOOL:       fv = lumyr_make_bool(llv != 0); break;
                    case CAST_CHAR:       fv = lumyr_make_char((char)(unsigned char)llv); break;
                    case CAST_BYTE:       fv = lumyr_make_byte((unsigned char)llv); break;
                    case CAST_INT8:       fv = lumyr_make_int8((int8_t)llv); break;
                    case CAST_INT16:      fv = lumyr_make_int16((int16_t)llv); break;
                    case CAST_SHORT:      fv = lumyr_make_short((int16_t)llv); break;
                    case CAST_INT32:      fv = lumyr_make_int32((int32_t)llv); break;
                    case CAST_INT:        fv = lumyr_make_int((int)llv); break;
                    case CAST_INT64:      fv = lumyr_make_int64(llv); break;
                    case CAST_LONGLONG:   fv = lumyr_make_long_long(llv); break;
                    case CAST_LONG:       fv = lumyr_make_long((long)llv); break;
                    case CAST_UINT8:      fv = lumyr_make_uint8((uint8_t)ullv); break;
                    case CAST_UCHAR:      fv = lumyr_make_uchar((unsigned char)ullv); break;
                    case CAST_UINT16:     fv = lumyr_make_uint16((uint16_t)ullv); break;
                    case CAST_USHORT:     fv = lumyr_make_ushort((unsigned short)ullv); break;
                    case CAST_UINT32:     fv = lumyr_make_uint32((uint32_t)ullv); break;
                    case CAST_UINT:       fv = lumyr_make_uint((unsigned int)ullv); break;
                    case CAST_UINT64:     fv = lumyr_make_uint64(ullv); break;
                    case CAST_ULONG:      fv = lumyr_make_ulong((unsigned long)ullv); break;
                    case CAST_SIZE_T:     fv = lumyr_make_size_t((size_t)ullv); break;
                    case CAST_SSIZE_T:    fv = lumyr_make_ssize_t((ssize_t)llv); break;
                    case CAST_FLOAT:      fv = lumyr_make_float((float)dv); break;
                    case CAST_DOUBLE:     fv = lumyr_make_double(dv); break;
                    case CAST_LONG_DOUBLE: fv = lumyr_make_long_double((long double)dv); break;
                    case CAST_PTR:        fv = lumyr_make_int64(llv); break;
                    default:              fv = lumyr_make_int(llv); break;
                }
                /* 非赋值上下文：用通用 LOAD_CONST 压入 Value 栈，安全兼容所有消费方
                   专用栈零开销优化由赋值路径单独处理 */
                emit(c, OPC_LOAD_CONST, bf_const(c->fn, fv), 0);
                break;
            }
            /* Direct conversion between dedicated stacks */
            ExprType inner_expr_type = arith_get_expr_type(c, node->u.type_annotation.expr);
            CastKind target_cast_type = node->u.type_annotation.cast_type;
            fprintf(stderr, "[DEBUG TYPE_ANNOT] inner_expr_type=%d, target_cast_type=%d\n", inner_expr_type, target_cast_type);
            c_expr(c, node->u.type_annotation.expr);
            /* 如果内部表达式是已知类型，直接从专用栈转换到目标类型专用栈 */
            if(inner_expr_type != EXPR_TYPE_NONE && inner_expr_type != EXPR_TYPE_BOOL && inner_expr_type != EXPR_TYPE_CHAR) {
                /* 目标类型映射到 ExprType */
                int target_expr_type = EXPR_TYPE_NONE;
                switch(target_cast_type) {
                    case CAST_INT: case CAST_INT32: target_expr_type = EXPR_TYPE_INT; break;
                    case CAST_UINT32: target_expr_type = EXPR_TYPE_UINT; break;
                    case CAST_FLOAT: target_expr_type = EXPR_TYPE_FLOAT; break;
                    case CAST_DOUBLE: target_expr_type = EXPR_TYPE_DOUBLE; break;
                    case CAST_LONGLONG: case CAST_INT64: case CAST_LONG: target_expr_type = EXPR_TYPE_LONG_LONG; break;
                    case CAST_LONG_DOUBLE: target_expr_type = EXPR_TYPE_LONG_DOUBLE; break;
                    default: target_expr_type = EXPR_TYPE_NONE; break;
                }
                /* 如果目标类型也是已知类型，直接在专用栈之间转换
                   按照用户设计思路：计算结果用什么栈，取决于接收方声明的什么类型
                   直接在专用栈之间转换，不需要中间转换到 Value 栈 */
                if(target_expr_type != EXPR_TYPE_NONE) {
                    if(inner_expr_type != target_expr_type) {
                        /* 使用专用栈之间的直接转换指令 */
                        if(inner_expr_type == EXPR_TYPE_INT && target_expr_type == EXPR_TYPE_DOUBLE) emit(c, OPC_INT_TO_DOUBLE, 0, 0);
                        else if(inner_expr_type == EXPR_TYPE_INT && target_expr_type == EXPR_TYPE_FLOAT) emit(c, OPC_INT_TO_FLOAT, 0, 0);
                        else if(inner_expr_type == EXPR_TYPE_INT && target_expr_type == EXPR_TYPE_LONG_LONG) emit(c, OPC_INT_TO_LONG_LONG, 0, 0);
                        else if(inner_expr_type == EXPR_TYPE_INT && target_expr_type == EXPR_TYPE_UINT) emit(c, OPC_INT_TO_UINT, 0, 0);
                        else if(inner_expr_type == EXPR_TYPE_UINT && target_expr_type == EXPR_TYPE_DOUBLE) emit(c, OPC_UINT_TO_DOUBLE, 0, 0);
                        else if(inner_expr_type == EXPR_TYPE_UINT && target_expr_type == EXPR_TYPE_FLOAT) emit(c, OPC_UINT_TO_FLOAT, 0, 0);
                        else if(inner_expr_type == EXPR_TYPE_UINT && target_expr_type == EXPR_TYPE_LONG_LONG) emit(c, OPC_UINT_TO_LONG_LONG, 0, 0);
                        else if(inner_expr_type == EXPR_TYPE_UINT && target_expr_type == EXPR_TYPE_INT) emit(c, OPC_UINT_TO_INT, 0, 0);
                        else if(inner_expr_type == EXPR_TYPE_FLOAT && target_expr_type == EXPR_TYPE_DOUBLE) emit(c, OPC_FLOAT_TO_DOUBLE, 0, 0);
                        else if(inner_expr_type == EXPR_TYPE_FLOAT && target_expr_type == EXPR_TYPE_LONG_LONG) emit(c, OPC_FLOAT_TO_LONG_LONG, 0, 0);
                        else if(inner_expr_type == EXPR_TYPE_DOUBLE && target_expr_type == EXPR_TYPE_LONG_LONG) emit(c, OPC_DOUBLE_TO_LONG_LONG, 0, 0);
                        else if(inner_expr_type == EXPR_TYPE_LONG_LONG && target_expr_type == EXPR_TYPE_DOUBLE) emit(c, OPC_LONG_LONG_TO_DOUBLE, 0, 0);
                        else if(inner_expr_type == EXPR_TYPE_LONG_LONG && target_expr_type == EXPR_TYPE_FLOAT) emit(c, OPC_LONG_LONG_TO_FLOAT, 0, 0);
                        /* 转换到 long double 的专用指令 */
                        else if(inner_expr_type == EXPR_TYPE_INT && target_expr_type == EXPR_TYPE_LONG_DOUBLE) emit(c, OPC_INT_TO_LONG_DOUBLE, 0, 0);
                        else if(inner_expr_type == EXPR_TYPE_UINT && target_expr_type == EXPR_TYPE_LONG_DOUBLE) emit(c, OPC_UINT_TO_LONG_DOUBLE, 0, 0);
                        else if(inner_expr_type == EXPR_TYPE_FLOAT && target_expr_type == EXPR_TYPE_LONG_DOUBLE) emit(c, OPC_FLOAT_TO_LONG_DOUBLE, 0, 0);
                        else if(inner_expr_type == EXPR_TYPE_DOUBLE && target_expr_type == EXPR_TYPE_LONG_DOUBLE) emit(c, OPC_DOUBLE_TO_LONG_DOUBLE, 0, 0);
                        else if(inner_expr_type == EXPR_TYPE_LONG_LONG && target_expr_type == EXPR_TYPE_LONG_DOUBLE) emit(c, OPC_LONG_LONG_TO_LONG_DOUBLE, 0, 0);
                        else {
                            /* 没有直接转换指令，先转换到 Value 栈，然后再转换 */
                            switch(inner_expr_type) {
                                case EXPR_TYPE_INT: emit(c, OPC_INT_TO_VALUE, 0, 0); break;
                                case EXPR_TYPE_UINT: emit(c, OPC_UINT_TO_VALUE, 0, 0); break;
                                case EXPR_TYPE_FLOAT: emit(c, OPC_FLOAT_TO_VALUE, 0, 0); break;
                                case EXPR_TYPE_DOUBLE: emit(c, OPC_DOUBLE_TO_VALUE, 0, 0); break;
                                case EXPR_TYPE_LONG_LONG: emit(c, OPC_LONG_LONG_TO_VALUE, 0, 0); break;
                                default: break;
                            }
                        }
                    }
                    /* 转换完成后，结果在目标类型专用栈中，需要转换到 Value 栈供后续使用
                       注意：long double 没有 TO_VALUE 指令，因为 long double 比较特殊，
                       需要先转换为 double，然后再转换为 Value */
                    switch(target_expr_type) {
                        case EXPR_TYPE_INT: emit(c, OPC_INT_TO_VALUE, 0, 0); break;
                        case EXPR_TYPE_UINT: emit(c, OPC_UINT_TO_VALUE, 0, 0); break;
                        case EXPR_TYPE_FLOAT: emit(c, OPC_FLOAT_TO_VALUE, 0, 0); break;
                        case EXPR_TYPE_DOUBLE: emit(c, OPC_DOUBLE_TO_VALUE, 0, 0); break;
                        case EXPR_TYPE_LONG_LONG: emit(c, OPC_LONG_LONG_TO_VALUE, 0, 0); break;
                        case EXPR_TYPE_LONG_DOUBLE:
                            /* long double 比较特殊，保持在 long double 专用栈中
                               后续赋值给 long double 变量时直接从 long double 专用栈存储
                               print long double 时直接从 long double 专用栈打印 */
                            break;
                        default: break;
                    }
                } else {
                    /* 目标类型不是已知类型，先转换到 Value 栈，然后再转换 */
                    switch(inner_expr_type) {
                        case EXPR_TYPE_INT: emit(c, OPC_INT_TO_VALUE, 0, 0); break;
                        case EXPR_TYPE_UINT: emit(c, OPC_UINT_TO_VALUE, 0, 0); break;
                        case EXPR_TYPE_FLOAT: emit(c, OPC_FLOAT_TO_VALUE, 0, 0); break;
                        case EXPR_TYPE_DOUBLE: emit(c, OPC_DOUBLE_TO_VALUE, 0, 0); break;
                        case EXPR_TYPE_LONG_LONG: emit(c, OPC_LONG_LONG_TO_VALUE, 0, 0); break;
                        default: break;
                    }
                }
            }
            static const OpCode cmap[] = {
                [CAST_INT] = OPC_CAST_INT, [CAST_DOUBLE] = OPC_CAST_DOUBLE,
                [CAST_CHAR] = OPC_CAST_CHAR, [CAST_BOOL] = OPC_CAST_BOOL,
                [CAST_INT8] = OPC_CAST_INT8, [CAST_INT16] = OPC_CAST_INT16,
                [CAST_INT32] = OPC_CAST_INT32, [CAST_INT64] = OPC_CAST_INT64,
                [CAST_UINT8] = OPC_CAST_UINT8, [CAST_UINT16] = OPC_CAST_UINT16,
                [CAST_UINT32] = OPC_CAST_UINT32, [CAST_UINT64] = OPC_CAST_UINT64,
                [CAST_LONG] = OPC_CAST_INT64, [CAST_LONGLONG] = OPC_CAST_INT64,
                [CAST_FLOAT] = OPC_CAST_FLOAT,
                [CAST_ULONG] = OPC_CAST_UINT64, [CAST_UCHAR] = OPC_CAST_UINT8,
                [CAST_SHORT] = OPC_CAST_INT16, [CAST_USHORT] = OPC_CAST_UINT16,
                [CAST_SIZE_T] = OPC_CAST_UINT64, [CAST_SSIZE_T] = OPC_CAST_INT64,
                [CAST_LONG_DOUBLE] = OPC_CAST_DOUBLE, [CAST_PTR] = OPC_CAST_INT64,
            };
            if(node->u.type_annotation.cast_type == CAST_VOID) {
                emit(c, OPC_POP, 0, 0);
                emit(c, OPC_LOAD_CONST, bf_const(c->fn, val_none()), 0);
            } else {
                emit(c, cmap[node->u.type_annotation.cast_type], 0, 0);
            }
            break;
        }
        case AST_TERNARY: {
            c_expr(c, node->u.ternary.cond);
            int jf = emit_here(c, OPC_JMP_IF_FALSE, 0, 0);
            c_expr(c, node->u.ternary.true_expr);
            int je = emit_here(c, OPC_JMP, 0, 0);
            patch_to(c, jf);
            c_expr(c, node->u.ternary.false_expr);
            patch_to(c, je);
            break;
        }
        case AST_CALL: {
            // ---- const fn 编译期求值 ----
            AstNode* func_ast = func_ast_lookup(node->u.call.name);
            if(func_ast && func_ast->u.func_def.is_const) {
                // 检查所有参数是否都是编译期常量
                _Bool all_const = 1;
                AstNode* arg = node->u.call.args;
                while(arg) {
                    if(arg->type != AST_INT && arg->type != AST_NUM &&
                       arg->type != AST_BOOL && arg->type != AST_STRING &&
                       arg->type != AST_CHAR && arg->type != AST_NONE) {
                        all_const = 0;
                        break;
                    }
                    arg = arg->u.seq.second;
                }
                if(all_const) {
                    // 编译期求值：用解释执行引擎求值函数调用
                    Value cv = ast_eval(node);
                    emit(c, OPC_LOAD_CONST, bf_const(c->fn, cv), 0);
                    break;
                }
            }
            TypeDef* td = type_lookup(node->u.call.name);
            if(td) {
                /* 类型构造调用：Person(a, b) → map 字面量（属性按序强转） */
                AstNode* ml = build_type_ctor(node, td);
                c_expr(c, ml);
                ast_free(ml);
                break;
            }
            /* 引用语义：修改型内置直接原地修改，无需自动 DUP+STORE 回变量 */
            int argc = 0;
            /* 方法调用：self 参数如果是 struct 变量，用 OPC_LOAD_STRUCT_PTR 传递指针 */
            const char* _actual_call_name = node->u.call.name;
            BytecodeFunc* _callee = ir_func_table_lookup(_actual_call_name);
            if(_callee && _callee->is_method && node->u.call.args) {
                /* 参数列表可能是 AST_SEQ（多参数）或直接是参数值（单参数） */
                AstNode* _first_arg = node->u.call.args;
                AstNode* _rest_args = NULL;
                if(_first_arg->type == AST_SEQ) {
                    _rest_args = _first_arg->u.seq.second;
                    _first_arg = _first_arg->u.seq.first;
                }
                if(_first_arg->type == AST_VAR) {
                    const char* _self_name = _first_arg->u.varname;
                    int _self_idx = bf_sym(c->fn, _self_name);
                    if(c->fn->var_struct_names && c->fn->var_struct_names[_self_idx]) {
                        emit(c, OPC_LOAD_STRUCT_PTR, _self_idx, 0);
                        argc = 1;
                        c_args(c, _rest_args, &argc);
                    } else {
                        c_args(c, node->u.call.args, &argc);
                    }
                } else {
                    c_args(c, node->u.call.args, &argc);
                }
            } else {
                /* 查找被调用函数，获取 param_is_ref 数组 */
                BytecodeFunc* _callee_fn = ir_func_table_lookup(_actual_call_name);
                int* _call_param_is_ref = (_callee_fn && _callee_fn->param_is_ref) ? _callee_fn->param_is_ref : NULL;
                int _call_ref_idx = 0;
                c_args_ref(c, node->u.call.args, &argc, _call_param_is_ref, &_call_ref_idx);
            }
            // 用户函数优先；否则内置函数（len/type/input/range/substr）
            static const char* bnames[BUILTIN_COUNT] = {"len", "type", "input", "range", "substr", "toupper", "tolower", "split", "del", "insert", "floor", "ceil", "abs", "sqrt", "max", "min", "join", "contains", "repeat", "replace", "sum", "avg", "format", "sort", "reverse", "map", "filter", "reduce", "strip", "startswith", "endswith", "read_file", "write_file", "file_exists", "keys", "values", "thread", "thread_join", "mutex", "rmutex", "rwlock", "spinlock", "lock", "unlock", "trylock", "rdlock", "wrlock", "tryrdlock", "trywrlock", "condvar", "cond_wait", "cond_wait_timeout", "cond_signal", "cond_broadcast", "threadlocal_get", "threadlocal_set", "get", "post", "put", "delete", "head", "patch", "json", "stringify", NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, "bytes", "str", "encode", "decode", "encodeURL", "decodeURL", "md5", "encodeBase64", "decodeBase64", "regex_match", "regex_search", "regex_replace", "now", "timestamp", "timestamp_ms", "sleep", "date", "time", "datetime", "format_time", "debug", "info", "warn", "error", "fatal", "gc_count", "gc_bytes", "gc_collect", "gc_stw_ns", "next", "send", "receive", "close", "GenThrow", "chain", "zip", "skip", "take", "enumerate"};
            int bid = -1;
            if(!ir_func_table_lookup(_actual_call_name)) {
                for(int k = 0; k < BUILTIN_COUNT; k++) {
                    if(bnames[k] && strcmp(node->u.call.name, bnames[k]) == 0) { bid = k; break; }
                }
            }
            if(bid >= 0) {
                emit(c, OPC_BUILTIN, bid, argc);
            }
            else emit(c, OPC_CALL, bf_sym(c->fn, _actual_call_name), argc);
            break;
        }
        case AST_DYN_CALL: {
            int argc = 0;
            c_expr(c, node->u.dyn_call.callee);   // 压函数值
            c_args(c, node->u.dyn_call.args, &argc);
            emit(c, OPC_CALLV, 0, argc);
            break;
        }
        case AST_SAFE_CALL: {
            // 安全调用：用 OPC_JMP_IF_NULL 判断是否为 null
            // 1. 压入 obj
            c_expr(c, node->u.safe_call.obj);
            // 2. 复制 obj（用于判断和返回）
            emit(c, OPC_DUP, 0, 0);
            // 3. 如果为 null，跳转到 null 分支
            int jnull = emit_here(c, OPC_JMP_IF_NULL, 0, 0);
            // 4. obj 非 null：弹出副本，执行方法调用或属性访问
            emit(c, OPC_POP, 0, 0);
            if(node->u.safe_call.args != NULL) {
                // 安全方法调用：obj?.method(args) → method(obj, args)
                c_expr(c, node->u.safe_call.obj);
                int argc = 0;
                c_args(c, node->u.safe_call.args, &argc);
                // 检查是否为内置函数
                static const char* bnames[BUILTIN_COUNT] = {"len", "type", "input", "range", "substr", "toupper", "tolower", "split", "del", "insert", "floor", "ceil", "abs", "sqrt", "max", "min", "join", "contains", "repeat", "replace", "sum", "avg", "format", "sort", "reverse", "map", "filter", "reduce", "strip", "startswith", "endswith", "read_file", "write_file", "file_exists", "keys", "values", "thread", "thread_join", "mutex", "rmutex", "rwlock", "spinlock", "lock", "unlock", "trylock", "rdlock", "wrlock", "tryrdlock", "trywrlock", "condvar", "cond_wait", "cond_wait_timeout", "cond_signal", "cond_broadcast", "threadlocal_get", "threadlocal_set", "get", "post", "put", "delete", "head", "patch", "json", "stringify", NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, "bytes", "str", "encode", "decode", "encodeURL", "decodeURL", "md5", "encodeBase64", "decodeBase64", "regex_match", "regex_search", "regex_replace", "now", "timestamp", "timestamp_ms", "sleep", "date", "time", "datetime", "format_time", "debug", "info", "warn", "error", "fatal", "gc_count", "gc_bytes", "gc_collect", "gc_stw_ns", "next", "send", "receive", "close", "GenThrow", "chain", "zip", "skip", "take", "enumerate"};
                int bid = -1;
                if(!ir_func_table_lookup(node->u.safe_call.method)) {
                    for(int k = 0; k < BUILTIN_COUNT; k++) {
                        if(bnames[k] && strcmp(node->u.safe_call.method, bnames[k]) == 0) { bid = k; break; }
                    }
                }
                if(bid >= 0) {
                    emit(c, OPC_BUILTIN, bid, argc + 1);
                } else {
                    emit(c, OPC_CALL, bf_sym(c->fn, node->u.safe_call.method), argc + 1);
                }
            } else {
                // 安全属性访问：obj?.property → obj["property"]
                c_expr(c, node->u.safe_call.obj);
                emit(c, OPC_LOAD_CONST, bf_const(c->fn, lumyr_make_string(node->u.safe_call.method)), 0);
                emit(c, OPC_INDEX_GET, 0, 0);
            }
            // 5. 跳转到结束
            int jend = emit_here(c, OPC_JMP, 0, 0);
            // 6. null 分支：栈顶是 obj（null），直接返回
            patch_to(c, jnull);
            patch_to(c, jend);
            break;
        }
        case AST_NULL_COALESCE: {
            // 空值合并：left ?? right → 如果 left 为 null，返回 right；否则返回 left
            // 简化模式：非 null 分支直接返回栈顶的 left 副本
            c_expr(c, node->u.null_coalesce.left);
            emit(c, OPC_DUP, 0, 0);  // 复制 left，栈：[left, left]
            int jnull = emit_here(c, OPC_JMP_IF_NULL, 0, 0);  // 弹出栈顶，如果为 null 跳转，栈：[left]
            // 非 null 分支：栈顶是 left 副本，直接跳转到结束
            int jend = emit_here(c, OPC_JMP, 0, 0);
            // null 分支：栈顶是 left，弹出后返回 right
            patch_to(c, jnull);
            emit(c, OPC_POP, 0, 0);  // 弹出 left
            c_expr(c, node->u.null_coalesce.right);  // 求值 right
            patch_to(c, jend);
            break;
        }
        case AST_INDEX: {
            AstNode* arr = node->u.index.arr;
            AstNode* idx = node->u.index.idx;
            if(arr && arr->type == AST_VAR && idx && idx->type == AST_STRING) {
                const char* vname = arr->u.varname;
                const char* fname = idx->u.sval;
                int var_idx = bf_sym(c->fn, vname);
                /* 方法 self 参数：从参数列表查找 self 的 constraint（不依赖 is_method，因为 recompile 可能丢失） */
                const char* sname = NULL;
                if(strcmp(vname, "self") == 0) {
                    if(c->fn->method_self_struct) {
                        sname = c->fn->method_self_struct;
                    } else if(c->fn->var_struct_names && c->fn->var_struct_names[var_idx]) {
                        sname = c->fn->var_struct_names[var_idx];
                    }
                } else if(c->fn->var_struct_names && c->fn->var_struct_names[var_idx]) {
                    sname = c->fn->var_struct_names[var_idx];
                }
                if(sname) {
                    /* 特殊属性 __mapname__ / __structname__：直接加载类名字符串，不需要访问实例 */
                    if(strcmp(fname, "__mapname__") == 0 || strcmp(fname, "__structname__") == 0) {
                        int cname_idx = bf_const(c->fn, make_string(sname));
                        emit(c, OPC_LOAD_CONST, cname_idx, 0);
                        break;
                    }
                    /* 方法 self 参数：直接生成 OPC_LOAD_FIELD，不依赖 struct_lookup
                       （parse 期 struct 可能还没注册，但 method_self_struct 已有类型名） */
                    int is_field = 0;
                    if(strcmp(vname, "self") == 0 && c->fn->method_self_struct) {
                        is_field = 1; /* 方法内 self 字段访问，直接信任 */
                    } else {
                        /* 支持 struct 和 class 两种类型：class 类型的 sname 以 "class:" 前缀 */
                        TypeDef* td = NULL;
                        if(strncmp(sname, "class:", 6) == 0) {
                            td = type_lookup(sname + 6);
                        } else {
                            td = struct_lookup(sname);
                        }
                        if(td) {
                            for(int fi = 0; fi < td->nprops; fi++) {
                                if(strcmp(td->props[fi], fname) == 0) { is_field = 1; break; }
                            }
                        }
                    }
                    if(is_field) {
                        int field_idx = bf_const(c->fn, make_string(fname));
                        emit(c, OPC_LOAD_FIELD, var_idx, field_idx);
                        break;
                    }
                }
            }
            /* 优化：double 类型化数组元素访问
               如果数组是声明为 double 的类型化数组，使用 OPC_DOUBLE_ARRAY_GET 指令，
               直接读取 double 值，压入 double 栈，零包装零转换 */
            if(arr && arr->type == AST_VAR && is_double_typed_array_var(c, arr->u.varname)) {
                c_expr(c, arr);
                c_expr(c, idx);
                emit(c, OPC_DOUBLE_ARRAY_GET, 0, 0);
            } else if(arr && arr->type == AST_VAR && is_float_typed_array_var(c, arr->u.varname)) {
                /* 优化：float 类型化数组元素访问
                   如果数组是声明为 float 的类型化数组，使用 OPC_FLOAT_ARRAY_GET 指令，
                   直接读取 float 值，压入 float 栈，零包装零转换 */
                c_expr(c, arr);
                c_expr(c, idx);
                emit(c, OPC_FLOAT_ARRAY_GET, 0, 0);
            } else {
                c_expr(c, arr);
                c_expr(c, idx);
                emit(c, OPC_INDEX_GET, 0, 0);
            }
            break;
        }
        case AST_INDEX_ASSIGN: {
            AstNode* arr = node->u.index_assign.arr;
            AstNode* idx = node->u.index_assign.idx;
            /* 嵌套 struct 字段赋值：r.top_left.x = 100 */
            if(arr && arr->type == AST_INDEX && arr->u.index.arr && arr->u.index.arr->type == AST_VAR &&
               arr->u.index.idx && arr->u.index.idx->type == AST_STRING &&
               idx && idx->type == AST_STRING) {
                const char* vname = arr->u.index.arr->u.varname;
                const char* nested_fname = arr->u.index.idx->u.sval;
                const char* fname = idx->u.sval;
                int var_idx = bf_sym(c->fn, vname);
                if(c->fn->var_struct_names && c->fn->var_struct_names[var_idx]) {
                    const char* sname = c->fn->var_struct_names[var_idx];
                    TypeDef* td = struct_lookup(sname);
                    int is_nested_field = 0;
                    if(td && td->field_struct_names) {
                        for(int fi = 0; fi < td->nprops; fi++) {
                            if(strcmp(td->props[fi], nested_fname) == 0 && td->field_struct_names[fi]) {
                                is_nested_field = 1;
                                break;
                            }
                        }
                    }
                    if(is_nested_field) {
                        char combined[256];
                        snprintf(combined, sizeof(combined), "%s.%s", nested_fname, fname);
                        int field_idx = bf_const(c->fn, make_string(combined));
                        c_expr(c, node->u.index_assign.value);
                        emit(c, OPC_STORE_NESTED_FIELD, var_idx, field_idx);
                        break;
                    }
                }
            }
            if(arr && arr->type == AST_VAR && idx && idx->type == AST_STRING) {
                const char* vname = arr->u.varname;
                const char* fname = idx->u.sval;
                int var_idx = bf_sym(c->fn, vname);
                /* 方法 self 参数：直接生成 OPC_STORE_FIELD，不依赖 struct_lookup
                   （parse 期 struct 可能还没注册，但 method_self_struct 已有类型名） */
                int is_field = 0;
                if(strcmp(vname, "self") == 0 && c->fn->method_self_struct) {
                    is_field = 1;
                } else if(c->fn->var_struct_names && c->fn->var_struct_names[var_idx]) {
                    const char* sname = c->fn->var_struct_names[var_idx];
                    TypeDef* td = struct_lookup(sname);
                    if(td) {
                        for(int fi = 0; fi < td->nprops; fi++) {
                            if(strcmp(td->props[fi], fname) == 0) { is_field = 1; break; }
                        }
                    }
                }
                if(is_field) {
                    int field_idx = bf_const(c->fn, make_string(fname));
                    c_expr(c, node->u.index_assign.value);
                    emit(c, OPC_STORE_FIELD, var_idx, field_idx);
                    break;
                }
            }
            /* 优化：int 类型化数组元素赋值（零转换开销）
               当数组是 int 类型化数组，且赋值的值是 int 类型（字面量或变量）时，
               使用 OPC_INT_ARRAY_SET 专用指令，直接从 int 专用栈弹出值写入数组 */
            if(arr && arr->type == AST_VAR) {
                const char* arr_name = arr->u.varname;
                int arr_idx = bf_sym(c->fn, arr_name);
                if(arr_idx >= 0 && c->fn->var_type_tags &&
                   c->fn->var_type_tags[arr_idx] == VAR_TYPE_INT_ARRAY) {
                    AstNode* val_node = node->u.index_assign.value;
                    int is_int_val = 0;
                    int int_literal_val = 0;
                    int int_var_idx = -1;
                    /* 检查赋值的值是否是 int 类型 */
                    if(val_node && val_node->type == AST_INT) {
                        /* int 字面量 */
                        is_int_val = 1;
                        int_literal_val = (int)val_node->u.inum;
                    } else if(val_node && val_node->type == AST_VAR) {
                        /* int 变量 */
                        int val_idx = bf_sym(c->fn, val_node->u.varname);
                        if(val_idx >= 0 && c->fn->var_type_tags &&
                           c->fn->var_type_tags[val_idx] == CAST_INT) {
                            is_int_val = 1;
                            int_var_idx = val_idx;
                        }
                    }
                    if(is_int_val) {
                        /* 编译数组和索引（压入 Value 栈） */
                        c_expr(c, arr);
                        c_expr(c, idx);
                        /* 编译赋值的值（压入 int 专用栈） */
                        if(val_node->type == AST_INT) {
                            /* int 字面量：直接压入 int 栈，零检查零转换 */
                            emit(c, OPC_PUSH_INT_CONST, int_literal_val, 0);
                        } else {
                            /* int 变量：从栈帧的 int_vals 数组读取，压入 int 栈 */
                            emit(c, OPC_LOAD_INT_VAR, int_var_idx, 0);
                        }
                        /* 生成 int 类型化数组元素赋值专用指令 */
                        emit(c, OPC_INT_ARRAY_SET, 0, 0);
                        break;
                    }
                }
            }
            /* 优化：uint 类型化数组元素赋值（零转换开销）
               当数组是 uint 类型化数组，且赋值的值是 uint 类型（字面量或变量）时，
               使用 OPC_UINT_ARRAY_SET 专用指令，直接从 uint 专用栈弹出值写入数组 */
            if(arr && arr->type == AST_VAR) {
                const char* arr_name = arr->u.varname;
                int arr_idx = bf_sym(c->fn, arr_name);
                if(arr_idx >= 0 && c->fn->var_type_tags &&
                   c->fn->var_type_tags[arr_idx] == VAR_TYPE_UINT_ARRAY) {
                    AstNode* val_node = node->u.index_assign.value;
                    int is_uint_val = 0;
                    unsigned int uint_literal_val = 0;
                    int uint_var_idx = -1;
                    /* 检查赋值的值是否是 uint 类型 */
                    if(val_node && val_node->type == AST_INT) {
                        /* uint 字面量 */
                        is_uint_val = 1;
                        uint_literal_val = (unsigned int)val_node->u.inum;
                    } else if(val_node && val_node->type == AST_VAR) {
                        /* uint 变量 */
                        int val_idx = bf_sym(c->fn, val_node->u.varname);
                        if(val_idx >= 0 && c->fn->var_type_tags &&
                           c->fn->var_type_tags[val_idx] == CAST_UINT32) {
                            is_uint_val = 1;
                            uint_var_idx = val_idx;
                        }
                    }
                    if(is_uint_val) {
                        /* 编译数组和索引（压入 Value 栈） */
                        c_expr(c, arr);
                        c_expr(c, idx);
                        /* 编译赋值的值（压入 uint 专用栈） */
                        if(val_node->type == AST_INT) {
                            /* uint 字面量：直接压入 uint 栈，零检查零转换 */
                            emit(c, OPC_PUSH_UINT_CONST, (int)uint_literal_val, 0);
                        } else {
                            /* uint 变量：从栈帧的 uint_vals 数组读取，压入 uint 栈 */
                            emit(c, OPC_LOAD_UINT_VAR, uint_var_idx, 0);
                        }
                        /* 生成 uint 类型化数组元素赋值专用指令 */
                        emit(c, OPC_UINT_ARRAY_SET, 0, 0);
                        break;
                    }
                }
            }
            c_expr(c, arr);
            c_expr(c, idx);
            c_expr(c, node->u.index_assign.value);
            emit(c, OPC_INDEX_SET, 0, 0);
            break;
        }
        case AST_ARRAY_LIT: {
            int elem_type = node->u.array_lit.elem_type;
            if(!has_spread_node(node->u.array_lit.elems)) {
                int n = 0;
                if(elem_type == VAL_INT && all_int_vars(c, node->u.array_lit.elems)) {
                    /* 所有元素都是声明为 int 类型的变量：使用 OPC_LOAD_INT_VAR 压入 int 栈，
                       OPC_INT_ARRAY_LIT(a=1) 从 int 栈读取，实现零检查零转换 */
                    compile_int_array_elems(c, node->u.array_lit.elems, &n);
                    emit(c, OPC_INT_ARRAY_LIT, 1, n);  /* a=1: 从 int 栈读取 */
                } else if(elem_type == VAL_DOUBLE && all_double_vars(c, node->u.array_lit.elems)) {
                    /* 所有元素都是声明为 double 类型的变量：使用 OPC_LOAD_DOUBLE_VAR 压入 double 栈，
                       OPC_DOUBLE_ARRAY_LIT(a=1) 从 double 栈读取，实现零检查零转换 */
                    compile_double_array_elems(c, node->u.array_lit.elems, &n);
                    emit(c, OPC_DOUBLE_ARRAY_LIT, 1, n);  /* a=1: 从 double 栈读取 */
                } else if(elem_type == VAL_FLOAT && all_float_vars(c, node->u.array_lit.elems)) {
                    /* 所有元素都是声明为 float 类型的变量：使用 OPC_LOAD_FLOAT_VAR 压入 float 栈，
                       OPC_FLOAT_ARRAY_LIT(a=1) 从 float 栈读取，实现零检查零转换 */
                    compile_float_array_elems(c, node->u.array_lit.elems, &n);
                    emit(c, OPC_FLOAT_ARRAY_LIT, 1, n);  /* a=1: 从 float 栈读取 */
                } else if(elem_type == VAL_UINT32 && all_uint_vars(c, node->u.array_lit.elems)) {
                    /* 所有元素都是声明为 uint 类型的变量：使用 OPC_LOAD_UINT_VAR 压入 uint 栈，
                       OPC_UINT_ARRAY_LIT(a=1) 从 uint 栈读取，实现零检查零转换 */
                    compile_uint_array_elems(c, node->u.array_lit.elems, &n);
                    emit(c, OPC_UINT_ARRAY_LIT, 1, n);  /* a=1: 从 uint 栈读取 */
                } else if(elem_type == VAL_BOOL && all_bool_vars(c, node->u.array_lit.elems)) {
                    /* 所有元素都是声明为 bool 类型的变量：使用 OPC_LOAD_BOOL_VAR 压入 bool 栈，
                       OPC_BOOL_ARRAY_LIT(a=1) 从 bool 栈读取，实现零检查零转换 */
                    compile_bool_array_elems(c, node->u.array_lit.elems, &n);
                    emit(c, OPC_BOOL_ARRAY_LIT, 1, n);  /* a=1: 从 bool 栈读取 */
                } else if(elem_type == VAL_CHAR && all_char_vars(c, node->u.array_lit.elems)) {
                    /* 所有元素都是声明为 char 类型的变量：使用 OPC_LOAD_CHAR_VAR 压入 char 栈，
                       OPC_CHAR_ARRAY_LIT(a=1) 从 char 栈读取，实现零检查零转换 */
                    compile_char_array_elems(c, node->u.array_lit.elems, &n);
                    emit(c, OPC_CHAR_ARRAY_LIT, 1, n);  /* a=1: 从 char 栈读取 */
                } else if(elem_type == VAL_BYTE && all_byte_vars(c, node->u.array_lit.elems)) {
                    /* 所有元素都是声明为 byte 类型的变量：使用 OPC_LOAD_BYTE_VAR 压入 byte 栈，
                       OPC_BYTE_ARRAY_LIT(a=1) 从 byte 栈读取，实现零检查零转换 */
                    compile_byte_array_elems(c, node->u.array_lit.elems, &n);
                    emit(c, OPC_BYTE_ARRAY_LIT, 1, n);  /* a=1: 从 byte 栈读取 */
                } else if(elem_type == VAL_INT8 && all_int8_vars(c, node->u.array_lit.elems)) {
                    /* 所有元素都是声明为 int8 类型的变量：使用 OPC_LOAD_INT8_VAR 压入 int8 栈，
                       OPC_INT8_ARRAY_LIT(a=1) 从 int8 栈读取，实现零检查零转换 */
                    compile_int8_array_elems(c, node->u.array_lit.elems, &n);
                    emit(c, OPC_INT8_ARRAY_LIT, 1, n);  /* a=1: 从 int8 栈读取 */
                } else if(elem_type == VAL_INT16 && all_int16_vars(c, node->u.array_lit.elems)) {
                    /* 所有元素都是声明为 int16 类型的变量：使用 OPC_LOAD_INT16_VAR 压入 int16 栈，
                       OPC_INT16_ARRAY_LIT(a=1) 从 int16 栈读取，实现零检查零转换 */
                    compile_int16_array_elems(c, node->u.array_lit.elems, &n);
                    emit(c, OPC_INT16_ARRAY_LIT, 1, n);  /* a=1: 从 int16 栈读取 */
                } else if(elem_type == VAL_INT32 && all_int32_vars(c, node->u.array_lit.elems)) {
                    /* 所有元素都是声明为 int32 类型的变量：使用 OPC_LOAD_INT32_VAR 压入 int32 栈，
                       OPC_INT32_ARRAY_LIT(a=1) 从 int32 栈读取，实现零检查零转换 */
                    compile_int32_array_elems(c, node->u.array_lit.elems, &n);
                    emit(c, OPC_INT32_ARRAY_LIT, 1, n);  /* a=1: 从 int32 栈读取 */
                } else if(elem_type == VAL_INT64 && all_int64_vars(c, node->u.array_lit.elems)) {
                    /* 所有元素都是声明为 int64 类型的变量：使用 OPC_LOAD_INT64_VAR 压入 int64 栈，
                       OPC_INT64_ARRAY_LIT(a=1) 从 int64 栈读取，实现零检查零转换 */
                    compile_int64_array_elems(c, node->u.array_lit.elems, &n);
                    emit(c, OPC_INT64_ARRAY_LIT, 1, n);  /* a=1: 从 int64 栈读取 */
                } else if(elem_type == VAL_UINT8 && all_uint8_vars(c, node->u.array_lit.elems)) {
                    /* 所有元素都是声明为 uint8 类型的变量：使用 OPC_LOAD_UINT8_VAR 压入 uint8 栈，
                       OPC_UINT8_ARRAY_LIT(a=1) 从 uint8 栈读取，实现零检查零转换 */
                    compile_uint8_array_elems(c, node->u.array_lit.elems, &n);
                    emit(c, OPC_UINT8_ARRAY_LIT, 1, n);  /* a=1: 从 uint8 栈读取 */
                } else if(elem_type == VAL_UINT16 && all_uint16_vars(c, node->u.array_lit.elems)) {
                    /* 所有元素都是声明为 uint16 类型的变量：使用 OPC_LOAD_UINT16_VAR 压入 uint16 栈，
                       OPC_UINT16_ARRAY_LIT(a=1) 从 uint16 栈读取，实现零检查零转换 */
                    compile_uint16_array_elems(c, node->u.array_lit.elems, &n);
                    emit(c, OPC_UINT16_ARRAY_LIT, 1, n);  /* a=1: 从 uint16 栈读取 */
                } else if(elem_type == VAL_UINT32 && all_uint32_vars(c, node->u.array_lit.elems)) {
                    /* 所有元素都是声明为 uint32 类型的变量：使用 OPC_LOAD_UINT32_VAR 压入 uint32 栈，
                       OPC_UINT32_ARRAY_LIT(a=1) 从 uint32 栈读取，实现零检查零转换 */
                    compile_uint32_array_elems(c, node->u.array_lit.elems, &n);
                    emit(c, OPC_UINT32_ARRAY_LIT, 1, n);  /* a=1: 从 uint32 栈读取 */
                } else if(elem_type == VAL_UINT64 && all_uint64_vars(c, node->u.array_lit.elems)) {
                    /* 所有元素都是声明为 uint64 类型的变量：使用 OPC_LOAD_UINT64_VAR 压入 uint64 栈，
                       OPC_UINT64_ARRAY_LIT(a=1) 从 uint64 栈读取，实现零检查零转换 */
                    compile_uint64_array_elems(c, node->u.array_lit.elems, &n);
                    emit(c, OPC_UINT64_ARRAY_LIT, 1, n);  /* a=1: 从 uint64 栈读取 */
                } else if(elem_type == VAL_LONG && all_long_vars(c, node->u.array_lit.elems)) {
                    /* 所有元素都是声明为 long 类型的变量：使用 OPC_LOAD_LONG_VAR 压入 long 栈，
                       OPC_LONG_ARRAY_LIT(a=1) 从 long 栈读取，实现零检查零转换 */
                    compile_long_array_elems(c, node->u.array_lit.elems, &n);
                    emit(c, OPC_LONG_ARRAY_LIT, 1, n);  /* a=1: 从 long 栈读取 */
                } else if(elem_type == VAL_ULONG && all_ulong_vars(c, node->u.array_lit.elems)) {
                    /* 所有元素都是声明为 unsigned long 类型的变量：使用 OPC_LOAD_ULONG_VAR 压入 unsigned long 栈，
                       OPC_ULONG_ARRAY_LIT(a=1) 从 unsigned long 栈读取，实现零检查零转换 */
                    compile_ulong_array_elems(c, node->u.array_lit.elems, &n);
                    emit(c, OPC_ULONG_ARRAY_LIT, 1, n);  /* a=1: 从 unsigned long 栈读取 */
                } else if(elem_type == VAL_SIZE_T && all_size_t_vars(c, node->u.array_lit.elems)) {
                    /* 所有元素都是声明为 size_t 类型的变量：使用 OPC_LOAD_SIZE_T_VAR 压入 size_t 栈，
                       OPC_SIZE_T_ARRAY_LIT(a=1) 从 size_t 栈读取，实现零检查零转换 */
                    compile_size_t_array_elems(c, node->u.array_lit.elems, &n);
                    emit(c, OPC_SIZE_T_ARRAY_LIT, 1, n);  /* a=1: 从 size_t 栈读取 */
                } else if(elem_type == VAL_SSIZE_T && all_ssize_t_vars(c, node->u.array_lit.elems)) {
                    /* 所有元素都是声明为 ssize_t 类型的变量：使用 OPC_LOAD_SSIZE_T_VAR 压入 ssize_t 栈，
                       OPC_SSIZE_T_ARRAY_LIT(a=1) 从 ssize_t 栈读取，实现零检查零转换 */
                    compile_ssize_t_array_elems(c, node->u.array_lit.elems, &n);
                    emit(c, OPC_SSIZE_T_ARRAY_LIT, 1, n);  /* a=1: 从 ssize_t 栈读取 */
                } else if(elem_type == VAL_LONG_DOUBLE && all_long_double_vars(c, node->u.array_lit.elems)) {
                    /* 所有元素都是声明为 long double 类型的变量：使用 OPC_LOAD_LONG_DOUBLE_VAR 压入 long double 栈，
                       OPC_LONG_DOUBLE_ARRAY_LIT(a=1) 从 long double 栈读取，实现零检查零转换 */
                    compile_long_double_array_elems(c, node->u.array_lit.elems, &n);
                    emit(c, OPC_LONG_DOUBLE_ARRAY_LIT, 1, n);  /* a=1: 从 long double 栈读取 */
                } else {
                    /* 混合场景：使用普通 c_args 编译压入 Value 栈，
                       OPC_INT_ARRAY_LIT(a=0)/OPC_DOUBLE_ARRAY_LIT(a=0)/OPC_FLOAT_ARRAY_LIT(a=0)/OPC_UINT_ARRAY_LIT(a=0) 从 Value 栈读取，内联类型转换 */
                    c_args(c, node->u.array_lit.elems, &n);
                    if(elem_type == VAL_INT) {
                        emit(c, OPC_INT_ARRAY_LIT, 0, n);  /* a=0: 从 Value 栈读取 */
                    } else if(elem_type == VAL_DOUBLE) {
                        emit(c, OPC_DOUBLE_ARRAY_LIT, 0, n);  /* a=0: 从 Value 栈读取 */
                    } else if(elem_type == VAL_FLOAT) {
                        emit(c, OPC_FLOAT_ARRAY_LIT, 0, n);  /* a=0: 从 Value 栈读取 */
                    } else if(elem_type == VAL_UINT32) {
                        emit(c, OPC_UINT_ARRAY_LIT, 0, n);  /* a=0: 从 Value 栈读取 */
                    } else if(elem_type == VAL_BOOL) {
                        emit(c, OPC_BOOL_ARRAY_LIT, 0, n);  /* a=0: 从 Value 栈读取 */
                    } else if(elem_type == VAL_CHAR) {
                        emit(c, OPC_CHAR_ARRAY_LIT, 0, n);  /* a=0: 从 Value 栈读取 */
                    } else if(elem_type == VAL_BYTE) {
                        emit(c, OPC_BYTE_ARRAY_LIT, 0, n);  /* a=0: 从 Value 栈读取 */
                    } else if(elem_type == VAL_INT8) {
                        emit(c, OPC_INT8_ARRAY_LIT, 0, n);  /* a=0: 从 Value 栈读取 */
                    } else if(elem_type == VAL_INT16) {
                        emit(c, OPC_INT16_ARRAY_LIT, 0, n);  /* a=0: 从 Value 栈读取 */
                    } else if(elem_type == VAL_INT32) {
                        emit(c, OPC_INT32_ARRAY_LIT, 0, n);  /* a=0: 从 Value 栈读取 */
                    } else if(elem_type == VAL_INT64) {
                        emit(c, OPC_INT64_ARRAY_LIT, 0, n);  /* a=0: 从 Value 栈读取 */
                    } else if(elem_type == VAL_UINT8) {
                        emit(c, OPC_UINT8_ARRAY_LIT, 0, n);  /* a=0: 从 Value 栈读取 */
                    } else if(elem_type == VAL_UINT16) {
                        emit(c, OPC_UINT16_ARRAY_LIT, 0, n);  /* a=0: 从 Value 栈读取 */
                    } else if(elem_type == VAL_UINT32) {
                        emit(c, OPC_UINT32_ARRAY_LIT, 0, n);  /* a=0: 从 Value 栈读取 */
                    } else if(elem_type == VAL_UINT64) {
                        emit(c, OPC_UINT64_ARRAY_LIT, 0, n);  /* a=0: 从 Value 栈读取 */
                    } else if(elem_type == VAL_LONG) {
                        emit(c, OPC_LONG_ARRAY_LIT, 0, n);  /* a=0: 从 Value 栈读取 */
                    } else if(elem_type == VAL_ULONG) {
                        emit(c, OPC_ULONG_ARRAY_LIT, 0, n);  /* a=0: 从 Value 栈读取 */
                    } else if(elem_type == VAL_SIZE_T) {
                        emit(c, OPC_SIZE_T_ARRAY_LIT, 0, n);  /* a=0: 从 Value 栈读取 */
                    } else if(elem_type == VAL_SSIZE_T) {
                        emit(c, OPC_SSIZE_T_ARRAY_LIT, 0, n);  /* a=0: 从 Value 栈读取 */
                    } else if(elem_type == VAL_LONG_DOUBLE) {
                        emit(c, OPC_LONG_DOUBLE_ARRAY_LIT, 0, n);  /* a=0: 从 Value 栈读取 */
                    } else {
                        emit(c, OPC_ARRAY_LIT, elem_type, n);
                    }
                }
            } else {
                if(elem_type == VAL_INT) {
                    emit(c, OPC_INT_ARRAY_LIT, 0, 0);  /* a=0: 从 Value 栈读取 */
                } else if(elem_type == VAL_DOUBLE) {
                    emit(c, OPC_DOUBLE_ARRAY_LIT, 0, 0);  /* a=0: 从 Value 栈读取 */
                } else if(elem_type == VAL_FLOAT) {
                    emit(c, OPC_FLOAT_ARRAY_LIT, 0, 0);  /* a=0: 从 Value 栈读取 */
                } else if(elem_type == VAL_UINT32) {
                    emit(c, OPC_UINT_ARRAY_LIT, 0, 0);  /* a=0: 从 Value 栈读取 */
                } else if(elem_type == VAL_BOOL) {
                    emit(c, OPC_BOOL_ARRAY_LIT, 0, 0);  /* a=0: 从 Value 栈读取 */
                } else if(elem_type == VAL_CHAR) {
                    emit(c, OPC_CHAR_ARRAY_LIT, 0, 0);  /* a=0: 从 Value 栈读取 */
                } else if(elem_type == VAL_BYTE) {
                    emit(c, OPC_BYTE_ARRAY_LIT, 0, 0);  /* a=0: 从 Value 栈读取 */
                } else if(elem_type == VAL_INT8) {
                    emit(c, OPC_INT8_ARRAY_LIT, 0, 0);  /* a=0: 从 Value 栈读取 */
                } else if(elem_type == VAL_INT16) {
                    emit(c, OPC_INT16_ARRAY_LIT, 0, 0);  /* a=0: 从 Value 栈读取 */
                } else if(elem_type == VAL_INT32) {
                    emit(c, OPC_INT32_ARRAY_LIT, 0, 0);  /* a=0: 从 Value 栈读取 */
                } else if(elem_type == VAL_INT64) {
                    emit(c, OPC_INT64_ARRAY_LIT, 0, 0);  /* a=0: 从 Value 栈读取 */
                } else if(elem_type == VAL_UINT8) {
                    emit(c, OPC_UINT8_ARRAY_LIT, 0, 0);  /* a=0: 从 Value 栈读取 */
                } else if(elem_type == VAL_UINT16) {
                    emit(c, OPC_UINT16_ARRAY_LIT, 0, 0);  /* a=0: 从 Value 栈读取 */
                } else if(elem_type == VAL_UINT32) {
                    emit(c, OPC_UINT32_ARRAY_LIT, 0, 0);  /* a=0: 从 Value 栈读取 */
                } else if(elem_type == VAL_UINT64) {
                    emit(c, OPC_UINT64_ARRAY_LIT, 0, 0);  /* a=0: 从 Value 栈读取 */
                } else if(elem_type == VAL_LONG) {
                    emit(c, OPC_LONG_ARRAY_LIT, 0, 0);  /* a=0: 从 Value 栈读取 */
                } else if(elem_type == VAL_ULONG) {
                    emit(c, OPC_ULONG_ARRAY_LIT, 0, 0);  /* a=0: 从 Value 栈读取 */
                } else if(elem_type == VAL_SIZE_T) {
                    emit(c, OPC_SIZE_T_ARRAY_LIT, 0, 0);  /* a=0: 从 Value 栈读取 */
                } else if(elem_type == VAL_SSIZE_T) {
                    emit(c, OPC_SSIZE_T_ARRAY_LIT, 0, 0);  /* a=0: 从 Value 栈读取 */
                } else if(elem_type == VAL_LONG_DOUBLE) {
                    emit(c, OPC_LONG_DOUBLE_ARRAY_LIT, 0, 0);  /* a=0: 从 Value 栈读取 */
                } else {
                    emit(c, OPC_ARRAY_LIT, elem_type, 0);
                }
                compile_array_elems(c, node->u.array_lit.elems);
            }
            break;
        }
        case AST_MAP_LIT: {
            if(!has_spread_node(node->u.map_lit.entries)) {
                int n = 0;
                c_map_entries(c, node->u.map_lit.entries, &n);
                emit(c, OPC_MAP_LIT, 0, n);
            } else {
                emit(c, OPC_MAP_LIT, 0, 0);
                compile_map_entries_spread(c, node->u.map_lit.entries);
            }
            break;
        }
        case AST_CLASS_NEW: {
            /* 创建 class 实例（C 结构体）：先编译参数压栈，然后发射 OPC_CLASS_NEW */
            int argc = node->u.class_new.argc;
            if(argc > 0 && node->u.class_new.args) {
                c_args(c, node->u.class_new.args, &(int){0});
            }
            emit(c, OPC_CLASS_NEW, bf_sym(c->fn, node->u.class_new.class_name), argc);
            break;
        }
        case AST_PRINT: {
            /* 优化：如果 print 只有一个参数，而且是一个有类型标记的变量，使用专用打印指令（零开销） */
            AstNode* single_arg = NULL;
            if(node->u.print.args && node->u.print.args->type != AST_SEQ) {
                single_arg = node->u.print.args;  /* 单个参数 */
            }
            if(single_arg && single_arg->type == AST_VAR) {
                int var_idx = bf_sym(c->fn, single_arg->u.varname);
                if(c->fn->var_type_tags && var_idx >= 0 && var_idx < c->fn->sym_cnt) {
                    int tag = c->fn->var_type_tags[var_idx];
                    if(tag == CAST_INT) {
                        emit(c, OPC_LOAD_INT_VAR, var_idx, 0);
                        emit(c, OPC_PRINT_INT, 0, 0);
                        break;
                    } else if(tag == CAST_DOUBLE) {
                        emit(c, OPC_LOAD_DOUBLE_VAR, var_idx, 0);
                        emit(c, OPC_PRINT_DOUBLE, 0, 0);
                        break;
                    } else if(tag == CAST_FLOAT) {
                        emit(c, OPC_LOAD_FLOAT_VAR, var_idx, 0);
                        emit(c, OPC_PRINT_FLOAT, 0, 0);
                        break;
                    } else if(tag == CAST_UINT32) {
                        emit(c, OPC_LOAD_UINT_VAR, var_idx, 0);
                        emit(c, OPC_PRINT_UINT, 0, 0);
                        break;
                    } else if(tag == CAST_LONGLONG) {
                        emit(c, OPC_LOAD_LONG_LONG_VAR, var_idx, 0);
                        emit(c, OPC_PRINT_LONG_LONG, 0, 0);
                        break;
                    } else if(tag == CAST_LONG_DOUBLE) {
                        emit(c, OPC_LOAD_LONG_DOUBLE_VAR, var_idx, 0);
                        emit(c, OPC_PRINT_LONG_DOUBLE, 0, 0);
                        break;
                    } else if(tag == CAST_BOOL) {
                        emit(c, OPC_LOAD_BOOL_VAR, var_idx, 0);
                        emit(c, OPC_PRINT_BOOL, 0, 0);
                        break;
                    } else if(tag == CAST_CHAR) {
                        emit(c, OPC_LOAD_CHAR_VAR, var_idx, 0);
                        emit(c, OPC_PRINT_CHAR, 0, 0);
                        break;
                    } else if(tag == CAST_BYTE) {
                        emit(c, OPC_LOAD_BYTE_VAR, var_idx, 0);
                        emit(c, OPC_PRINT_BYTE, 0, 0);
                        break;
                    } else if(tag == CAST_INT8) {
                        emit(c, OPC_LOAD_INT8_VAR, var_idx, 0);
                        emit(c, OPC_PRINT_INT8, 0, 0);
                        break;
                    } else if(tag == CAST_INT16) {
                        emit(c, OPC_LOAD_INT16_VAR, var_idx, 0);
                        emit(c, OPC_PRINT_INT16, 0, 0);
                        break;
                    } else if(tag == CAST_SHORT) {
                        emit(c, OPC_LOAD_SHORT_VAR, var_idx, 0);
                        emit(c, OPC_PRINT_SHORT, 0, 0);
                        break;
                    } else if(tag == CAST_INT32) {
                        emit(c, OPC_LOAD_INT32_VAR, var_idx, 0);
                        emit(c, OPC_PRINT_INT32, 0, 0);
                        break;
                    } else if(tag == CAST_INT64) {
                        emit(c, OPC_LOAD_INT64_VAR, var_idx, 0);
                        emit(c, OPC_PRINT_INT64, 0, 0);
                        break;
                    } else if(tag == CAST_UINT8) {
                        emit(c, OPC_LOAD_UINT8_VAR, var_idx, 0);
                        emit(c, OPC_PRINT_UINT8, 0, 0);
                        break;
                    } else if(tag == CAST_UINT16) {
                        emit(c, OPC_LOAD_UINT16_VAR, var_idx, 0);
                        emit(c, OPC_PRINT_UINT16, 0, 0);
                        break;
                    } else if(tag == CAST_UINT32) {
                        emit(c, OPC_LOAD_UINT32_VAR, var_idx, 0);
                        emit(c, OPC_PRINT_UINT32, 0, 0);
                        break;
                    } else if(tag == CAST_UINT64) {
                        emit(c, OPC_LOAD_UINT64_VAR, var_idx, 0);
                        emit(c, OPC_PRINT_UINT64, 0, 0);
                        break;
                    } else if(tag == CAST_LONG) {
                        emit(c, OPC_LOAD_LONG_VAR, var_idx, 0);
                        emit(c, OPC_PRINT_LONG, 0, 0);
                        break;
                    } else if(tag == CAST_ULONG) {
                        emit(c, OPC_LOAD_ULONG_VAR, var_idx, 0);
                        emit(c, OPC_PRINT_ULONG, 0, 0);
                        break;
                    } else if(tag == CAST_SIZE_T) {
                        emit(c, OPC_LOAD_SIZE_T_VAR, var_idx, 0);
                        emit(c, OPC_PRINT_SIZE_T, 0, 0);
                        break;
                    } else if(tag == CAST_SSIZE_T) {
                        emit(c, OPC_LOAD_SSIZE_T_VAR, var_idx, 0);
                        emit(c, OPC_PRINT_SSIZE_T, 0, 0);
                        break;
                    } else if(tag == CAST_LONG_DOUBLE) {
                        emit(c, OPC_LOAD_LONG_DOUBLE_VAR, var_idx, 0);
                        emit(c, OPC_PRINT_LONG_DOUBLE, 0, 0);
                        break;
                    }
                }
            }
            /* 普通打印：多参数或非类型化变量 */
            int cnt = c_print_args(c, node->u.print.args);
            emit(c, OPC_PRINT, cnt, 0);
            break;
        }
        case AST_SEQ:
            c_expr(c, node->u.seq.first);
            emit(c, OPC_POP, 0, 0);
            c_expr(c, node->u.seq.second);
            break;
        // 语句型节点出现在表达式位置（边缘）：求值后压 0（与解释器 make_int(0) 一致）
        case AST_IF:
        case AST_IF_CHAIN:
        case AST_BLOCK:
        case AST_WHILE:
        case AST_DO_WHILE:
        case AST_FOR:
        case AST_SWITCH:
            c_stmt(c, node);
            emit(c, OPC_LOAD_CONST, bf_const(c->fn, lumyr_make_int(0)), 0);
            break;
        case AST_RETURN:
            if(node->u.ret.ret_val) c_expr(c, node->u.ret.ret_val);
            else emit(c, OPC_LOAD_CONST, bf_const(c->fn, val_none()), 0);
            break;
        case AST_YIELD:
            if(node->u.yieldnode.value) c_expr(c, node->u.yieldnode.value);
            else emit(c, OPC_LOAD_CONST, bf_const(c->fn, val_none()), 0);
            emit(c, OPC_YIELD, 0, 0);
            break;
        case AST_DESTRUCT: {
            c_expr(c, node->u.destruct.rhs);
            for(int i = 0; i < node->u.destruct.count; i++) {
                emit(c, OPC_DUP, 0, 0);
                emit(c, OPC_LOAD_CONST, bf_const(c->fn, lumyr_make_int(i)), 0);
                emit(c, OPC_INDEX_GET, 0, 0);
                emit(c, OPC_STORE_VAR, bf_sym(c->fn, node->u.destruct.names[i]), 0);
                emit(c, OPC_POP, 0, 0);
            }
            emit(c, OPC_POP, 0, 0);
            emit(c, OPC_LOAD_CONST, bf_const(c->fn, val_none()), 0);
            break;
        }
        case AST_SPREAD:
            c_expr(c, node->u.spread.expr);
            break;
        default:
            emit(c, OPC_LOAD_CONST, bf_const(c->fn, val_none()), 0);
            break;
    }
}

static void c_stmt(Ctx* c, AstNode* node)
{
    if(!node) return;
    switch(node->type) {
        case AST_SEQ:
            c_stmt(c, node->u.seq.first);
            c_stmt(c, node->u.seq.second);
            break;
        case AST_BLOCK:
            c_stmt(c, node->u.block.stmts);
            break;
        // 表达式语句：求值后丢弃结果
        case AST_ASSIGN:
        case AST_BINOP:
        case AST_UNARY:
        case AST_CAST:
        case AST_TYPE_ANNOTATION:
        case AST_TERNARY:
        case AST_CALL:
        case AST_DYN_CALL:
        case AST_VAR:
        case AST_INT:
        case AST_NUM:
        case AST_BOOL:
        case AST_CHAR:
        case AST_STRING:
        case AST_NONE:
        case AST_YIELD:
                        c_expr(c, node);
                        emit(c, OPC_POP, 0, 0);
                        break;
        case AST_INDEX:
        case AST_INDEX_ASSIGN:
        case AST_ARRAY_LIT:
        case AST_MAP_LIT:
        case AST_DESTRUCT:
        case AST_SPREAD:
            c_expr(c, node);
            emit(c, OPC_POP, 0, 0);
            break;
        case AST_PRINT: {
            /* 优化：如果 print 只有一个参数，而且是一个数组访问表达式，
               并且数组是 float/double 类型化数组，使用专用打印指令（零开销） */
            AstNode* single_arg = NULL;
            if(node->u.print.args && node->u.print.args->type != AST_SEQ) {
                single_arg = node->u.print.args;  /* 单个参数 */
            }
            if(single_arg && single_arg->type == AST_INDEX) {
                AstNode* arr = single_arg->u.index.arr;
                AstNode* idx = single_arg->u.index.idx;
                if(arr && arr->type == AST_VAR) {
                    if(is_double_typed_array_var(c, arr->u.varname)) {
                        c_expr(c, arr);
                        c_expr(c, idx);
                        emit(c, OPC_DOUBLE_ARRAY_GET, 0, 0);
                        emit(c, OPC_PRINT_DOUBLE, 0, 0);
                        break;
                    } else if(is_float_typed_array_var(c, arr->u.varname)) {
                        c_expr(c, arr);
                        c_expr(c, idx);
                        emit(c, OPC_FLOAT_ARRAY_GET, 0, 0);
                        emit(c, OPC_PRINT_FLOAT, 0, 0);
                        break;
                    }
                }
            }
            /* 优化：如果 print 只有一个参数，而且是一个算术运算表达式，
               并且左右操作数都是对应类型的变量，使用专用打印指令（零开销） */
            else if(single_arg && single_arg->type == AST_BINOP) {
                BinOp bop = single_arg->u.bin.op;
                int is_arith = (bop == OP_ADD || bop == OP_SUB || bop == OP_MUL || bop == OP_DIV || bop == OP_MOD);
                if(is_arith) {
                    int left_is_int = is_int_var(c, single_arg->u.bin.left);
                    int right_is_int = is_int_var(c, single_arg->u.bin.right);
                    int left_is_uint = is_uint_var(c, single_arg->u.bin.left);
                    int right_is_uint = is_uint_var(c, single_arg->u.bin.right);
                    int left_is_double = is_double_var(c, single_arg->u.bin.left);
                    int right_is_double = is_double_var(c, single_arg->u.bin.right);
                    int left_is_float = is_float_var(c, single_arg->u.bin.left);
                    int right_is_float = is_float_var(c, single_arg->u.bin.right);
                    ExprType result_type = get_expr_type(c, single_arg);
                    if(result_type == EXPR_TYPE_INT) {
                        c_expr(c, single_arg);
                        emit(c, OPC_PRINT_INT, 0, 0);
                        break;
                    } else if(result_type == EXPR_TYPE_UINT) {
                        c_expr(c, single_arg);
                        emit(c, OPC_PRINT_UINT, 0, 0);
                        break;
                    } else if(result_type == EXPR_TYPE_FLOAT) {
                        c_expr(c, single_arg);
                        emit(c, OPC_PRINT_FLOAT, 0, 0);
                        break;
                    } else if(result_type == EXPR_TYPE_DOUBLE) {
                        c_expr(c, single_arg);
                        emit(c, OPC_PRINT_DOUBLE, 0, 0);
                        break;
                    } else if(result_type == EXPR_TYPE_LONG_LONG) {
                        /* long long 类型算术运算结果：直接生成 OPC_PRINT_LONG_LONG */
                        c_expr(c, single_arg);
                        emit(c, OPC_PRINT_LONG_LONG, 0, 0);
                        break;
                    } else if(result_type == EXPR_TYPE_LONG_DOUBLE) {
                        /* long double 类型算术运算结果：直接生成 OPC_PRINT_LONG_DOUBLE */
                        c_expr(c, single_arg);
                        emit(c, OPC_PRINT_LONG_DOUBLE, 0, 0);
                        break;
                    } else if(result_type == EXPR_TYPE_INT8) {
                        c_expr(c, single_arg);
                        emit(c, OPC_PRINT_INT8, 0, 0);
                        break;
                    } else if(result_type == EXPR_TYPE_INT16) {
                        c_expr(c, single_arg);
                        emit(c, OPC_PRINT_INT16, 0, 0);
                        break;
                    } else if(result_type == EXPR_TYPE_INT) {
                        c_expr(c, single_arg);
                        emit(c, OPC_PRINT_INT32, 0, 0);
                        break;
                    } else if(result_type == EXPR_TYPE_INT64) {
                        c_expr(c, single_arg);
                        emit(c, OPC_PRINT_INT64, 0, 0);
                        break;
                    }
                }
            }
            /* 优化：如果 print 只有一个参数，而且是一个有类型标记的变量，使用专用打印指令（零开销） */
            if(single_arg && single_arg->type == AST_VAR) {
                int var_idx = bf_sym(c->fn, single_arg->u.varname);
                if(c->fn->var_type_tags && var_idx >= 0 && var_idx < c->fn->sym_cnt) {
                    int tag = c->fn->var_type_tags[var_idx];
                    if(tag == CAST_INT) {
                        emit(c, OPC_LOAD_INT_VAR, var_idx, 0);
                        emit(c, OPC_PRINT_INT, 0, 0);
                        break;
                    } else if(tag == CAST_DOUBLE) {
                        emit(c, OPC_LOAD_DOUBLE_VAR, var_idx, 0);
                        emit(c, OPC_PRINT_DOUBLE, 0, 0);
                        break;
                    } else if(tag == CAST_FLOAT) {
                        emit(c, OPC_LOAD_FLOAT_VAR, var_idx, 0);
                        emit(c, OPC_PRINT_FLOAT, 0, 0);
                        break;
                    } else if(tag == CAST_UINT32) {
                        emit(c, OPC_LOAD_UINT_VAR, var_idx, 0);
                        emit(c, OPC_PRINT_UINT, 0, 0);
                        break;
                    } else if(tag == CAST_LONGLONG) {
                        emit(c, OPC_LOAD_LONG_LONG_VAR, var_idx, 0);
                        emit(c, OPC_PRINT_LONG_LONG, 0, 0);
                        break;
                    } else if(tag == CAST_LONG_DOUBLE) {
                        emit(c, OPC_LOAD_LONG_DOUBLE_VAR, var_idx, 0);
                        emit(c, OPC_PRINT_LONG_DOUBLE, 0, 0);
                        break;
                    } else if(tag == CAST_BOOL) {
                        emit(c, OPC_LOAD_BOOL_VAR, var_idx, 0);
                        emit(c, OPC_PRINT_BOOL, 0, 0);
                        break;
                    } else if(tag == CAST_CHAR) {
                        emit(c, OPC_LOAD_CHAR_VAR, var_idx, 0);
                        emit(c, OPC_PRINT_CHAR, 0, 0);
                        break;
                    } else if(tag == CAST_BYTE) {
                        emit(c, OPC_LOAD_BYTE_VAR, var_idx, 0);
                        emit(c, OPC_PRINT_BYTE, 0, 0);
                        break;
                    } else if(tag == CAST_INT8) {
                        emit(c, OPC_LOAD_INT8_VAR, var_idx, 0);
                        emit(c, OPC_PRINT_INT8, 0, 0);
                        break;
                    } else if(tag == CAST_INT16) {
                        emit(c, OPC_LOAD_INT16_VAR, var_idx, 0);
                        emit(c, OPC_PRINT_INT16, 0, 0);
                        break;
                    } else if(tag == CAST_INT32) {
                        emit(c, OPC_LOAD_INT32_VAR, var_idx, 0);
                        emit(c, OPC_PRINT_INT32, 0, 0);
                        break;
                    } else if(tag == CAST_INT64) {
                        emit(c, OPC_LOAD_INT64_VAR, var_idx, 0);
                        emit(c, OPC_PRINT_INT64, 0, 0);
                        break;
                    } else if(tag == CAST_UINT8) {
                        emit(c, OPC_LOAD_UINT8_VAR, var_idx, 0);
                        emit(c, OPC_PRINT_UINT8, 0, 0);
                        break;
                    } else if(tag == CAST_UINT16) {
                        emit(c, OPC_LOAD_UINT16_VAR, var_idx, 0);
                        emit(c, OPC_PRINT_UINT16, 0, 0);
                        break;
                    } else if(tag == CAST_UINT64) {
                        emit(c, OPC_LOAD_UINT64_VAR, var_idx, 0);
                        emit(c, OPC_PRINT_UINT64, 0, 0);
                        break;
                    } else if(tag == CAST_LONG) {
                        emit(c, OPC_LOAD_LONG_VAR, var_idx, 0);
                        emit(c, OPC_PRINT_LONG, 0, 0);
                        break;
                    } else if(tag == CAST_ULONG) {
                        emit(c, OPC_LOAD_ULONG_VAR, var_idx, 0);
                        emit(c, OPC_PRINT_ULONG, 0, 0);
                        break;
                    } else if(tag == CAST_SIZE_T) {
                        emit(c, OPC_LOAD_SIZE_T_VAR, var_idx, 0);
                        emit(c, OPC_PRINT_SIZE_T, 0, 0);
                        break;
                    } else if(tag == CAST_SSIZE_T) {
                        emit(c, OPC_LOAD_SSIZE_T_VAR, var_idx, 0);
                        emit(c, OPC_PRINT_SSIZE_T, 0, 0);
                        break;
                    } else if(tag == CAST_LONG_DOUBLE) {
                        emit(c, OPC_LOAD_LONG_DOUBLE_VAR, var_idx, 0);
                        emit(c, OPC_PRINT_LONG_DOUBLE, 0, 0);
                        break;
                    }
                }
            }
            int cnt = c_print_args(c, node->u.print.args);
            emit(c, OPC_PRINT, cnt, 0);
            /* OPC_PRINT 会弹出所有参数，不需要额外的 OPC_POP */
            break;
        }
        case AST_IF: {
            c_expr(c, node->u.ifnode.cond);
            int jf = emit_here(c, OPC_JMP_IF_FALSE, 0, 0);
            c_stmt(c, node->u.ifnode.then_stmt);
            if(node->u.ifnode.else_stmt || node->u.ifnode.elif_chain) {
                int je = emit_here(c, OPC_JMP, 0, 0);
                patch_to(c, jf);
                c_stmt(c, node->u.ifnode.elif_chain);
                c_stmt(c, node->u.ifnode.else_stmt);
                patch_to(c, je);
            } else {
                patch_to(c, jf);
            }
            break;
        }
        case AST_IF_CHAIN: {
            // if-elif-else 链：逐个条件判断（分支数动态，无硬上限）
            int* end_jumps = NULL; int jump_cnt = 0, jump_cap = 0;
            c_expr(c, node->u.if_chain.cond);
            int jf = emit_here(c, OPC_JMP_IF_FALSE, 0, 0);
            c_stmt(c, node->u.if_chain.if_body);
            if(jump_cnt >= jump_cap) {
                int nc = jump_cap > 0 ? jump_cap * 2 : 8;
                int* nj = (int*)realloc(end_jumps, (size_t)nc * sizeof(int));
                if(!nj) { LOG_ERROR("IR: if-chain 分支表扩容内存不足\n"); exit(EXIT_FAILURE); }
                end_jumps = nj; jump_cap = nc;
            }
            end_jumps[jump_cnt++] = emit_here(c, OPC_JMP, 0, 0);
            patch_to(c, jf);

            AstNode* p = node->u.if_chain.elif_list;
            while(p) {
                c_expr(c, p->u.elif.cond);
                int jf2 = emit_here(c, OPC_JMP_IF_FALSE, 0, 0);
                c_stmt(c, p->u.elif.body);
                if(jump_cnt >= jump_cap) {
                    int nc = jump_cap > 0 ? jump_cap * 2 : 8;
                    int* nj = (int*)realloc(end_jumps, (size_t)nc * sizeof(int));
                    if(!nj) { LOG_ERROR("IR: if-chain 分支表扩容内存不足\n"); exit(EXIT_FAILURE); }
                    end_jumps = nj; jump_cap = nc;
                }
                end_jumps[jump_cnt++] = emit_here(c, OPC_JMP, 0, 0);
                patch_to(c, jf2);
                p = p->u.elif.next;
            }
            if(node->u.if_chain.else_body) c_stmt(c, node->u.if_chain.else_body);
            for(int i = 0; i < jump_cnt; i++) patch_to(c, end_jumps[i]);
            free(end_jumps);
            break;
        }
        case AST_TRY: {
            /* try { body } [catch (Type e) { handler }]... [finally { fbody }]
               支持多个 catch 块和按类型捕获。 */
            int has_fin = (node->u.trynode.finally_body != NULL);
            int has_catch = (node->u.trynode.catch_var != NULL);
            /* 仅在多 catch 或有类型 catch 时走新分支；无类型单个 catch 保持旧逻辑 */
            int use_multi = 0;
            if(node->u.trynode.catch_count > 1) use_multi = 1;
            else if(node->u.trynode.catch_count == 1 && node->u.trynode.catches != NULL
                    && node->u.trynode.catches[0].type != NULL) use_multi = 1;

            if(use_multi && !has_fin) {
                /* 多 catch 块，无 finally - 简化版：先 STORE_VAR，再类型检查 */
                int ccnt = node->u.trynode.catch_count;
                int* jmismatch = (int*)calloc((size_t)ccnt, sizeof(int));
                int* jdone = (int*)calloc((size_t)ccnt, sizeof(int));
                int mismatch_cnt = 0, done_cnt = 0;

                int jtry = here(c);
                emit(c, OPC_TRY, 0, 0);
                c_stmt(c, node->u.trynode.body);
                int jendtry = here(c);
                emit(c, OPC_ENDTRY, 0, 0);
                int jskip = here(c);
                emit(c, OPC_JMP, 0, 0);

                int cstart = here(c);
                bf_patch(c->fn, jtry, cstart);
                bf_patch(c->fn, jendtry, cstart);

                for(int ci = 0; ci < ccnt; ci++) {
                    CatchClause* cc = &node->u.trynode.catches[ci];
                    /* 修补上一个 catch 的不匹配跳转到当前位置 */
                    for(int k = 0; k < mismatch_cnt; k++)
                        bf_patch(c->fn, jmismatch[k], here(c));
                    mismatch_cnt = 0;

                    /* 获取异常对象并存入变量 */
                    emit(c, OPC_GET_ERR, 0, 0);
                    emit(c, OPC_STORE_VAR, bf_sym(c->fn, cc->var), 0);
                    emit(c, OPC_POP, 0, 0);

                    /* 类型检查：加载变量，获取 type 字段，比较 */
                    if(cc->type != NULL) {
                        emit(c, OPC_LOAD_VAR, bf_sym(c->fn, cc->var), 0);
                        emit(c, OPC_LOAD_CONST, bf_const(c->fn, lumyr_make_string("type")), 0);
                        emit(c, OPC_INDEX_GET, 0, 0);
                        emit(c, OPC_LOAD_CONST, bf_const(c->fn, lumyr_make_string(cc->type)), 0);
                        emit(c, OPC_EQ, 0, 0);
                        jmismatch[mismatch_cnt++] = here(c);
                        emit(c, OPC_JMP_IF_FALSE, 0, 0);
                    }

                    /* catch body */
                    c_stmt(c, cc->body);
                    jdone[done_cnt++] = here(c);
                    emit(c, OPC_JMP, 0, 0);
                }

                /* 所有 catch 都不匹配：重新抛出 */
                for(int k = 0; k < mismatch_cnt; k++)
                    bf_patch(c->fn, jmismatch[k], here(c));
                emit(c, OPC_GET_ERR, 0, 0);
                emit(c, OPC_THROW, 0, 0);

                /* 结束位置：修补 jskip 和所有 jdone */
                int end = here(c);
                patch_to(c, jskip);
                for(int k = 0; k < done_cnt; k++)
                    bf_patch(c->fn, jdone[k], end);

                free(jmismatch);
                free(jdone);
                break;
            }
            int jtry = here(c);
            int jf2_patch = -1;
            emit(c, OPC_TRY, 0, 0);
            if(has_fin) { c->fin_depth++; ctx_ensure_fin_rows(c, c->fin_depth + 1); c->fin_pend_n[c->fin_depth] = 0; }
            c_stmt(c, node->u.trynode.body);
            if(has_fin) {
                int jnp = here(c);
                emit(c, OPC_FIN_PUSH, 1, 0);       // 正常完成 → JMP end
                int jf1 = here(c);
                emit(c, OPC_JMP, 0, 0);
                int cstart = here(c);
                bf_patch(c->fn, jtry, cstart);     // 错误恢复 → catch
                if(has_catch) {
                    emit(c, OPC_GET_ERR, 0, 0);
                    emit(c, OPC_STORE_VAR, bf_sym(c->fn, node->u.trynode.catch_var), 0);
                    emit(c, OPC_POP, 0, 0);        // 丢弃表达式值
                    c_stmt(c, node->u.trynode.catch_body);
                    int jnp2 = here(c);
                    emit(c, OPC_FIN_PUSH, 1, 0);   // catch 处理完 → JMP end
                    jf2_patch = jnp2;
                } else {
                    emit(c, OPC_FIN_PUSH, 2, 0);   // 无 catch → RETHROW
                }
                int jf2 = here(c);
                emit(c, OPC_JMP, 0, 0);
                int fstart = here(c);
                bf_patch(c->fn, jf1, fstart);
                bf_patch(c->fn, jf2, fstart);
                bf_patch_b(c->fn, jtry, fstart);   // TRY.b = finally 起始
                /* body/catch 内 return 挂起的 PEND_RETURN.b = fstart；
                   break/continue 的 JMP.a = fstart */
                for(int pi = 0; pi < c->fin_pend_n[c->fin_depth]; pi++)
                    bf_patch_b(c->fn, c->fin_pend[c->fin_depth][pi], fstart);
                for(int ji = 0; ji < c->fin_jmp_n[c->fin_depth]; ji++)
                    bf_patch(c->fn, c->fin_jmp[c->fin_depth][ji], fstart);
                c->fin_pend_n[c->fin_depth] = 0;
                c->fin_jmp_n[c->fin_depth] = 0;
                /* 已消费，fbody 内 return/break 不再挂起（finally 体直接用完成动作） */
                /* finally 体：内部 return 直接返回（不挂起，避免自跳） */
                c->fin_depth--;
                c_stmt(c, node->u.trynode.finally_body);
                c->fin_depth++;
                int end = here(c);
                emit(c, OPC_FINISH, 0, 0);
                end = here(c);                      // end = FINISH 之后（FINISH 弹出动作后跳此处）
                bf_patch_b(c->fn, jnp, end);        // FIN_PUSH(1).b = end（a=1 保持）
                if(jf2_patch >= 0) bf_patch_b(c->fn, jf2_patch, end);  // catch 的 FIN_PUSH(1).b = end
                if(c->fin_depth > 0) c->fin_depth--;
            } else {
                int jend = here(c);
                emit(c, OPC_ENDTRY, 0, 0);
                int jskip = here(c);
                emit(c, OPC_JMP, 0, 0);
                int cstart = here(c);
                bf_patch(c->fn, jtry, cstart);     // 错误恢复 → catch
                bf_patch(c->fn, jend, cstart);     // ENDTRY 的 a（C 端 else 分支 goto）
                emit(c, OPC_GET_ERR, 0, 0);
                emit(c, OPC_STORE_VAR, bf_sym(c->fn, node->u.trynode.catch_var), 0);
                emit(c, OPC_POP, 0, 0);            // 丢弃 STORE 压回的表达式值，catch 尾 sp 平衡
                c_stmt(c, node->u.trynode.catch_body);
                /* 异常路径不需要 ENDTRY：OPC_TRY else 已将 vm_depth 设为 d（TRY 前深度），
                   GET_ERR 不修改 vm_depth，catch_body 结束后 vm_depth 已正确为 d */
                patch_to(c, jskip);
            }
            break;
        }
        case AST_THROW:
            /* throw expr：求值 → 弹栈顶包装成错误对象抛出 */
            c_expr(c, node->u.thrownode.expr);
            emit(c, OPC_THROW, 0, 0);
            break;
        case AST_ELIF:
            // 仅由 IF_CHAIN 直接遍历，不独立出现
            break;
        case AST_WHILE: {
            int l_cond = here(c);
            c_expr(c, node->u.while_node.cond);
            int jf = emit_here(c, OPC_JMP_IF_FALSE, 0, 0);
            layer_push(c, 0, l_cond);          // continue → 回 cond
            c_stmt(c, node->u.while_node.body);
            Layer l = layer_pop(c);
            emit(c, OPC_JMP, l_cond, 0);
            int l_end = here(c);
            bf_patch(c->fn, jf, l_end);
            for(int i = 0; i < l.brk_cnt; i++) bf_patch(c->fn, l.brk[i], l_end);
            for(int i = 0; i < l.brk_fin_cnt; i++) bf_patch_b(c->fn, l.brk_fin[i], l_end);
            /* try-finally 内 continue 的 FIN_PUSH.b 回填到 while 条件（此前遗漏导致跳 pc=0 死循环） */
            for(int i = 0; i < l.cont_fin_cnt; i++) bf_patch_b(c->fn, l.cont_fin[i], l_cond);
            free(l.brk); free(l.cont);
            break;
        }
        case AST_DO_WHILE: {
            int l_body = here(c);
            layer_push(c, 0, -1);              /* continue 目标未知（cond 前才知道） */
            c_stmt(c, node->u.while_node.body);
            Layer l = layer_pop(c);
            int l_cond = here(c);              /* continue 跳到这里（cond 检查前） */
            for(int i = 0; i < l.cont_cnt; i++) bf_patch(c->fn, l.cont[i], l_cond);
            for(int i = 0; i < l.cont_fin_cnt; i++) bf_patch_b(c->fn, l.cont_fin[i], l_cond);
            c_expr(c, node->u.while_node.cond);
            emit(c, OPC_JMP_IF_TRUE, l_body, 0);  /* cond 为真则跳回 body */
            int l_end = here(c);
            for(int i = 0; i < l.brk_cnt; i++) bf_patch(c->fn, l.brk[i], l_end);
            for(int i = 0; i < l.brk_fin_cnt; i++) bf_patch_b(c->fn, l.brk_fin[i], l_end);
            free(l.brk); free(l.cont);
            break;
        }
        case AST_FOR: {
            if(node->u.for_node.init) c_stmt(c, node->u.for_node.init);
            int l_cond = here(c);
            int jf = -1;
            if(node->u.for_node.cond) {
                c_expr(c, node->u.for_node.cond);
                jf = emit_here(c, OPC_JMP_IF_FALSE, 0, 0);
            }
            layer_push(c, 0, -1);              // continue 目标未知（update 后才知道）
            c_stmt(c, node->u.for_node.body);
            Layer l = layer_pop(c);
            int l_cont = here(c);              // continue 跳到这里（update 前）
            for(int i = 0; i < l.cont_cnt; i++) bf_patch(c->fn, l.cont[i], l_cont);
            for(int i = 0; i < l.cont_fin_cnt; i++) bf_patch_b(c->fn, l.cont_fin[i], l_cont);
            if(node->u.for_node.update) c_stmt(c, node->u.for_node.update);
            emit(c, OPC_JMP, l_cond, 0);
            int l_end = here(c);
            if(jf >= 0) bf_patch(c->fn, jf, l_end);
            for(int i = 0; i < l.brk_cnt; i++) bf_patch(c->fn, l.brk[i], l_end);
            for(int i = 0; i < l.brk_fin_cnt; i++) bf_patch_b(c->fn, l.brk_fin[i], l_end);
            for(int i = 0; i < l.brk_fin_cnt; i++) bf_patch_b(c->fn, l.brk_fin[i], l_end);
            free(l.brk); free(l.cont);
            break;
        }
        case AST_SWITCH: {
            c_expr(c, node->u.sw.cond);        // 栈顶 sw_val（保留）
            layer_push(c, 1, -1);
            int* end_jumps = NULL; int jump_cnt = 0, jump_cap = 0;
            AstNode* cp = node->u.sw.cases;
            int first_case = 1;
            int last_jf = -1;                  // 上一个非 default case 的未中跳转
            while(cp) {
                // 上一 case 未中 → 跳到当前 case 开头（patch 后重置，避免重复 patch 覆盖目标）
                if(!first_case) {
                    if(last_jf >= 0) patch_to(c, last_jf);
                    last_jf = -1;
                }
                if(cp->u.cs.is_default) {
                    emit(c, OPC_POP, 0, 0);      // 丢弃 sw_val
                    c_stmt(c, cp->u.cs.body);
                    if(jump_cnt >= jump_cap) {
                        int nc = jump_cap > 0 ? jump_cap * 2 : 8;
                        int* nj = (int*)realloc(end_jumps, (size_t)nc * sizeof(int));
                        if(!nj) { LOG_ERROR("IR: switch 分支表扩容内存不足\n"); exit(EXIT_FAILURE); }
                        end_jumps = nj; jump_cap = nc;
                    }
                    end_jumps[jump_cnt++] = emit_here(c, OPC_JMP, 0, 0);
                } else if(cp->u.cs.is_type_match) {
                    // 类型匹配：type(sw_val) == "typename"
                    emit(c, OPC_DUP, 0, 0);
                    emit(c, OPC_BUILTIN, BUILTIN_TYPE, 1);  // type(sw_val)
                    const char* type_names[] = {"none","int","double","bool","char","string","func","array","map","error","byte"};
                    int tidx = cp->u.cs.match_type;
                    if(tidx < 0 || tidx > 11) tidx = 0;
                    int cidx = bf_const(c->fn, lumyr_make_string(type_names[tidx]));
                    emit(c, OPC_LOAD_CONST, cidx, 0);
                    emit(c, OPC_EQ, 0, 0);
                    last_jf = emit_here(c, OPC_JMP_IF_FALSE, 0, 0);
                    emit(c, OPC_POP, 0, 0);      // 命中：丢弃 sw_val
                    c_stmt(c, cp->u.cs.body);
                } else if(cp->u.cs.bind_var != NULL) {
                    /* 模式绑定：case x [if cond]: */
                    /* STORE_VAR 弹值写变量后原值压回，栈顶仍为 sw_val */
                    emit(c, OPC_STORE_VAR, bf_sym(c->fn, cp->u.cs.bind_var), 0);
                    if(cp->u.cs.guard != NULL) {
                        /* 有守卫：保留 sw_val 在栈底，guard 结果在栈顶 */
                        c_expr(c, cp->u.cs.guard);
                        last_jf = emit_here(c, OPC_JMP_IF_FALSE, 0, 0);
                        /* 命中：丢弃 sw_val */
                        emit(c, OPC_POP, 0, 0);
                    } else {
                        /* 无守卫：丢弃 sw_val */
                        emit(c, OPC_POP, 0, 0);
                    }
                    c_stmt(c, cp->u.cs.body);
                    if(jump_cnt >= jump_cap) {
                        int nc = jump_cap > 0 ? jump_cap * 2 : 8;
                        int* nj = (int*)realloc(end_jumps, (size_t)nc * sizeof(int));
                        if(!nj) { LOG_ERROR("IR: switch branch table oom\n"); exit(EXIT_FAILURE); }
                        end_jumps = nj; jump_cap = nc;
                    }
                    end_jumps[jump_cnt++] = emit_here(c, OPC_JMP, 0, 0);
                } else {
                    emit(c, OPC_DUP, 0, 0);
                    c_expr(c, cp->u.cs.const_val);
                    emit(c, OPC_EQ, 0, 0);
                    last_jf = emit_here(c, OPC_JMP_IF_FALSE, 0, 0);
                    emit(c, OPC_POP, 0, 0);      // 命中：丢弃 sw_val
                    c_stmt(c, cp->u.cs.body);
                    if(jump_cnt >= jump_cap) {
                        int nc = jump_cap > 0 ? jump_cap * 2 : 8;
                        int* nj = (int*)realloc(end_jumps, (size_t)nc * sizeof(int));
                        if(!nj) { LOG_ERROR("IR: switch 分支表扩容内存不足\n"); exit(EXIT_FAILURE); }
                        end_jumps = nj; jump_cap = nc;
                    }
                    end_jumps[jump_cnt++] = emit_here(c, OPC_JMP, 0, 0);
                }
                first_case = 0;
                cp = cp->u.cs.next;
            }
            // 最后一个 case 非 default 且未中 → 跳到循环后（丢弃 sw_val）
            if(last_jf >= 0) patch_to(c, last_jf);
            emit(c, OPC_POP, 0, 0);              // 未命中：丢弃 sw_val
            Layer l = layer_pop(c);
            int l_end = here(c);
            for(int i = 0; i < jump_cnt; i++) bf_patch(c->fn, end_jumps[i], l_end);
            free(end_jumps);
            for(int i = 0; i < l.brk_cnt; i++) bf_patch(c->fn, l.brk[i], l_end);
            for(int i = 0; i < l.brk_fin_cnt; i++) bf_patch_b(c->fn, l.brk_fin[i], l_end);
            for(int i = 0; i < l.brk_fin_cnt; i++) bf_patch_b(c->fn, l.brk_fin[i], l_end);
            free(l.brk); free(l.cont);
            break;
        }
        case AST_CASE:
            // 仅由 SWITCH 直接遍历，不独立出现
            break;
        case AST_BREAK: {
            if(c->layer_depth <= 0) {
                LOG_ERROR("IR: break 不在循环/switch 内\n");
                exit(EXIT_FAILURE);
            }
            if(c->fin_depth > 0) {
                /* try-finally 内 break：压 BREAK 完成动作（b=循环出口，循环层 patch），
                   再直接 JMP 到 finally（fstart，fin 层 patch），避免正常路径的 FIN_PUSH(1) 覆盖 */
                int pos = emit_here(c, OPC_FIN_PUSH, 3, 0);
                layer_brk_fin_add(c, pos);
                int jp = emit_here(c, OPC_JMP, 0, 0);
                fin_jmp_add(c, jp);
                break;
            }
            int pos = emit_here(c, OPC_JMP, 0, 0);
            layer_brk_add(c, pos);
            break;
        }
        case AST_CONTINUE: {
            if(c->fin_depth > 0) {
                /* try-finally 内 continue：压 CONT 完成动作（b=continue 目标，循环层 patch） */
                int pos = emit_here(c, OPC_FIN_PUSH, 4, 0);
                layer_cont_fin_add(c, pos);
                int jp = emit_here(c, OPC_JMP, 0, 0);
                fin_jmp_add(c, jp);
                break;
            }
            int pos = emit_here(c, OPC_JMP, 0, 0);
            layer_cont_add(c, pos);
            break;
        }
        case AST_RETURN:
            if(c->fin_depth > 0) {
                /* try-finally 内 return：挂起返回值，先跑 finally */
                if(node->u.ret.ret_val) c_expr(c, node->u.ret.ret_val);
                else emit(c, OPC_LOAD_CONST, bf_const(c->fn, val_none()), 0);
                int ppos = here(c);
                emit(c, OPC_PEND_RETURN, 0, 0);   // b 由 AST_TRY 结束处 patch 为 fstart
                fin_pend_add(c, ppos);
                break;
            }
            if(node->u.ret.ret_val) {
                c_expr(c, node->u.ret.ret_val);
                emit(c, OPC_RETURN, 0, 0);
            } else {
                emit(c, OPC_RETURN_NIL, 0, 0);
            }
            break;
        case AST_FUNC_DEF:
            // 函数定义已由 yacc 期注册；顶层/函数体内的定义节点不产生指令
            break;
        case AST_EXTERN_FUNC: {
            /* FFI 外部函数声明：编译阶段创建 FFIFunc 对象，注册到全局符号表，
               同时添加到编译通道的 extern 声明列表中 */
            ValueType ret_type = lumyr_ffi_type_from_name(node->u.extern_func.ret_type_name);
            int param_count = 0;
            AstNode* p = node->u.extern_func.params;
            while(p) { param_count++; p = p->u.param.next; }
            ValueType* param_types = NULL;
            int* cgen_param_types = NULL;
            if(param_count > 0) {
                param_types = (ValueType*)malloc((size_t)param_count * sizeof(ValueType));
                cgen_param_types = (int*)malloc((size_t)param_count * sizeof(int));
                p = node->u.extern_func.params;
                for(int i = 0; i < param_count && p; i++) {
                    param_types[i] = lumyr_ffi_type_from_name(p->u.param.constraint);
                    cgen_param_types[i] = (int)param_types[i];
                    p = p->u.param.next;
                }
            }
            FFIFunc* ffi = lumyr_ffi_func_create(node->u.extern_func.name,
                                                    node->u.extern_func.libname,
                                                    ret_type, param_types, param_count);
            free(param_types);
            Value fv;
            fv.type = VAL_FUNC;
            fv.v.func.func_obj = NULL;
            fv.v.func.ffi_func = ffi;
            fv.v.func.is_ffi = 1;
            sym_set(node->u.extern_func.name, fv);
            /* 添加到编译通道的 extern 声明列表 */
            ffi_decl_add(node->u.extern_func.name, node->u.extern_func.libname,
                         (int)ret_type, cgen_param_types, param_count);
            free(cgen_param_types);
            break;
        }
        default:
            break;
    }
}

// ---------------- 入口 ----------------

static void compile_params(BytecodeFunc* fn, AstNode* params)
{
    int idx = 0;
    AstNode* p = params;
    /* 先统计普通参数个数，分配 param_is_ref 数组 */
    int normal_cnt = 0;
    AstNode* pp = params;
    while(pp) {
        if(!pp->u.param.is_ellipsis) normal_cnt++;
        pp = pp->u.param.next;
    }
    fn->param_is_ref = (int*)calloc(normal_cnt, sizeof(int));
    int normal_idx = 0;
    while(p) {
        if(p->u.param.is_ellipsis) fn->has_variadic = 1;
        fn->params = (char**)realloc(fn->params, sizeof(char*) * (idx + 1));
        fn->params[idx++] = strdup(p->u.param.name);
        /* 记录 ref 参数（只记录普通参数，可变参数不记录） */
        if(!p->u.param.is_ellipsis && p->u.param.is_ref) {
            fn->param_is_ref[normal_idx] = 1;
        }
        if(!p->u.param.is_ellipsis) normal_idx++;
        /* 参数有类型标注时，添加到 var_struct_names（不检查是否已注册，parse 期可能还没注册） */
        if(p->u.param.name && p->u.param.constraint) {
            int pidx = bf_sym(fn, p->u.param.name);
            if(fn->var_struct_names) {
                fn->var_struct_names[pidx] = strdup(p->u.param.constraint);
            }
        }
        p = p->u.param.next;
    }
    fn->param_cnt = idx - (fn->has_variadic ? 1 : 0);
}


/* 方法 self 类型标记查找上下文 */
typedef struct {
    const char* func_name;
    size_t func_len;
    char* class_name;
} MethodSelfLookupCtx;

/* type_foreach 回调：查找函数名匹配的 class 类型 */
static void method_self_lookup_cb(const char* name, TypeDef* td, void* user_data)
{
    MethodSelfLookupCtx* ctx = (MethodSelfLookupCtx*)user_data;
    if(ctx->class_name || !td->is_class || !name) return;
    size_t tlen = strlen(name);
    if(ctx->func_len > tlen + 1 && strncmp(ctx->func_name, name, tlen) == 0 && ctx->func_name[tlen] == '_') {
        ctx->class_name = strdup(name);
    }
}

BytecodeFunc* ir_compile_function(const char* name, AstNode* params, AstNode* body, int is_generator, const char* class_name)
{
    BytecodeFunc* fn = bytecode_func_new(name, 0);
    fn->is_generator = is_generator;
    if(class_name) fn->class_name = strdup(class_name);
    compile_params(fn, params);
    /* 方法标记：第一个参数名是 "self" 时，标记为方法，self 传递 struct 指针 */
    if(params && params->u.param.name && strcmp(params->u.param.name, "self") == 0) {
        fn->is_method = 1;
        const char* self_type = params->u.param.constraint;
        /* 构造函数和普通方法：从函数名中提取 class 名，自动给 self 打上 class 类型标记 */
        if(!self_type && name) {
            size_t nlen = strlen(name);
            char* extracted_class_name = NULL;
            /* 先尝试构造函数：函数名以 ___init__ 结尾 */
            if(nlen >= 9 && strcmp(name + nlen - 9, "___init__") == 0) {
                /* 提取 class 名：去掉 ___init__ 后缀 */
                extracted_class_name = (char*)malloc(nlen - 8);
                strncpy(extracted_class_name, name, nlen - 9);
                extracted_class_name[nlen - 9] = '\0';
            } else {
                /* 普通方法：遍历所有 class 类型，看看函数名是否以 <类名>_ 开头 */
                MethodSelfLookupCtx ctx = { name, nlen, NULL };
                type_foreach(method_self_lookup_cb, &ctx);
                extracted_class_name = ctx.class_name;
            }
            if(extracted_class_name) {
                TypeDef* td = type_lookup(extracted_class_name);
                if(td && td->is_class) {
                    /* 用 class: 前缀标记这是 class 类型 */
                    size_t marked_len = strlen(extracted_class_name) + 7; /* "class:" + 类名 + \0 */
                    char* marked_name = (char*)malloc(marked_len);
                    snprintf(marked_name, marked_len, "class:%s", extracted_class_name);
                    self_type = marked_name;
                    fn->method_self_struct = marked_name;
                    /* 设置 fn->class_name 字段，用于红黑树查找 class 方法 */
                    if(!fn->class_name) fn->class_name = strdup(extracted_class_name);
                }
                free(extracted_class_name);
            }
        }
        if(self_type) {
            if(!fn->method_self_struct) {
                fn->method_self_struct = strdup(self_type);
            }
            /* 把 self 参数添加到 var_struct_names，让 self.x 访问生成 OPC_LOAD_FIELD */
            int self_idx = bf_sym(fn, "self");
            if(fn->var_struct_names) {
                fn->var_struct_names[self_idx] = strdup(self_type);
            }
        }
    }

    /* 把参数添加到符号表中（确保 self 等参数在编译函数体之前就存在于符号表中） */
    for(int pi = 0; pi < fn->param_cnt; pi++) {
        if(fn->params && fn->params[pi]) {
            bf_sym(fn, fn->params[pi]);
        }
    }

    Ctx c = { .fn = fn, .layer_depth = 0 };
    c_stmt(&c, body);
    emit(&c, OPC_RETURN_NIL, 0, 0);   // fallthrough 默认返回 nil

    ir_func_table_add(fn);
    return fn;
}

// 重编译已注册函数：函数体字节码在 parse 期生成，早于 typecheck 的
// AST_VAR→AST_FUNCREF 转换；typecheck 后对受影响函数重编译（原位替换，
// 保持函数表顺序与 CALL 指令的 index 绑定不变）。
BytecodeFunc* ir_func_table_recompile(const char* name, AstNode* params, AstNode* body)
{
    /* 先查找旧函数，保存方法标记 */
    BytecodeFunc* old_fn = ir_func_table_lookup(name);
    int old_is_method = 0;
    char* old_method_self_struct = NULL;
    char* old_class_name = NULL;
    if(old_fn) {
        old_is_method = old_fn->is_method;
        if(old_fn->method_self_struct) {
            old_method_self_struct = strdup(old_fn->method_self_struct);
        }
        if(old_fn->class_name) {
            old_class_name = strdup(old_fn->class_name);
        }
    }
    BytecodeFunc* nb = ir_compile_function(name, params, body, 0, old_fn ? old_fn->class_name : NULL); // 内部 add 到红黑树
    /* 恢复方法标记 */
    if(old_is_method && !nb->is_method) {
        nb->is_method = old_is_method;
        if(old_method_self_struct && !nb->method_self_struct) {
            nb->method_self_struct = old_method_self_struct;
            old_method_self_struct = NULL; /* 所有权转移 */
        }
    }
    /* 恢复 class_name 字段（用于 CC 模式方法命名） */
    if(old_class_name) {
        if(nb->class_name) free(nb->class_name);
        nb->class_name = old_class_name;
        old_class_name = NULL; /* 所有权转移 */
    }
    free(old_method_self_struct);
    free(old_class_name);
    /* 注意：红黑树不支持原位替换，旧函数会留在树中（内存由 GC 或后续清理处理）
       新函数已通过 ir_compile_function -> ir_func_table_add 插入到红黑树中
       由于红黑树的键是 (class_name, method_name)，新函数会覆盖旧函数的查找结果 */
    return nb;
}

/* 优化回调函数：用于红黑树遍历优化所有函数 */
static void optimize_cb(const char* class_name, const char* method_name, void* data, void* user_data)
{
    (void)class_name; (void)method_name; (void)user_data;
    ir_optimize((BytecodeFunc*)data);
}

BytecodeFunc* ir_compile_main(AstNode* root)
{
    BytecodeFunc* fn = bytecode_func_new(NULL, 1);
    Ctx c = { .fn = fn, .layer_depth = 0 };
    c_stmt(&c, root);
            emit(&c, OPC_HALT, 0, 0);
    
    /* 优化 pass：常量折叠。在所有 BytecodeFunc 生成完毕后、返回前，
       对 main 与函数表中每个函数统一做一遍 IR peephole 优化。
       VM 执行 / -S 反汇编 / -c 代码生成三条通道共用此 IR，故双通道一致。 */
    /* 用红黑树遍历优化所有函数 */
        ir_func_table_foreach(optimize_cb, NULL);
        ir_optimize(fn);
    
    /* 编译完成：清理字符串常量缓存（长字符串已 gc_pin，由 GC 回收） */
    string_cache_reset();

    return fn;
}
