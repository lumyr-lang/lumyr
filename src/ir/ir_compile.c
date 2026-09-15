/* 编译期模块：ir_func_table/ir_func_count 为编译期状态，单线程编译设计；
 * 未来支持并发编译时需实例化（每编译任务一份），不影响运行时多线程。 */
// AST → 字节码 IR 编译器
// 遍历结构与 ast_typecheck.c / codegen.c 对齐（用户建议复用其递归结构）。
#include "ir_compile.h"
#include "lumyr_log.h"
#include "ir_cgen.h"
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

typedef struct {
    int kind;              // 0=循环 1=switch
    int* brk; int brk_cnt, brk_cap;    // 未定 break 跳转位置（JMP.a）
    int* cont; int cont_cnt, cont_cap; // 未定 continue 跳转位置（循环，JMP.a）
    int* brk_fin; int brk_fin_cnt, brk_fin_cap;   // try-finally 内 break 的 FIN_PUSH 位置（patch b）
    int* cont_fin; int cont_fin_cnt, cont_fin_cap; // try-finally 内 continue 的 FIN_PUSH 位置（patch b）
    int cont_target;       // 已知 continue 目标（while 的 cond 开头）或 -1
} Layer;

typedef struct {
    BytecodeFunc* fn;
    Layer* layers;           // 动态：循环/switch 嵌套无硬上限
    int layer_depth;
    int layers_cap;
    /* finally 上下文：fin_depth>0 表示当前编译位置在 try-finally 内；
       fin_pend[depth][*] = body/catch 中 PEND_RETURN(b=0) 的位置，fstart 确定后统一 patch b
       行/列均动态扩容，try-finally 嵌套与每层挂起数无硬上限 */
    int** fin_pend;
    int* fin_pend_n;
    int* fin_pend_cap;
    int** fin_jmp;            // try-finally 内 break/continue 的 JMP 位置（patch a=fstart）
    int* fin_jmp_n;
    int* fin_jmp_cap;
    int fin_depth;
    int fin_cap;
} Ctx;

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

static int here(Ctx* c) { return c->fn->code_len; }

static void emit(Ctx* c, OpCode op, int a, int b) { bf_emit(c->fn, op, a, b); }

static int emit_here(Ctx* c, OpCode op, int a, int b) { return bf_emit_here(c->fn, op, a, b); }

static void patch_to(Ctx* c, int pos) { bf_patch(c->fn, pos, here(c)); }

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

static void c_stmt(Ctx* c, AstNode* node);
static void c_expr(Ctx* c, AstNode* node);

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
        case AST_INT:    *out = lumyr_make_int(node->u.inum); return 1;
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

static void c_expr(Ctx* c, AstNode* node)
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
                          node->u.assign.expr->u.type_annotation.cast_type == 2 /* CAST_INT */) {
                    /* <int>[...] 形式：变量是 int 类型化数组，设置特殊标记用于上下文感知类型推导 */
                    c->fn->var_type_tags[var_idx] = VAR_TYPE_INT_ARRAY;
                } else if(inner && inner->type == AST_ARRAY_LIT &&
                          node->u.assign.expr->u.type_annotation.cast_type == 1 /* CAST_DOUBLE */) {
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
            /* 优化1：赋值为 <int>arr[idx] 形式时，使用 OPC_INT_ARRAY_GET + OPC_STORE_INT_VAR
               零包装零重复提取，直接从 int 类型化数组读取并存储到 int 变量 */
            if(node->u.assign.expr && node->u.assign.expr->type == AST_TYPE_ANNOTATION &&
               node->u.assign.expr->u.type_annotation.cast_type == 2 /* CAST_INT */ &&
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
                c->fn->var_type_tags[var_idx] = 2; /* CAST_INT */
            }
            /* 优化1b：赋值为 <double>arr[idx] 形式时，使用 OPC_DOUBLE_ARRAY_GET + OPC_STORE_DOUBLE_VAR
               零包装零重复提取，直接从 double 类型化数组读取并存储到 double 变量 */
            else if(node->u.assign.expr && node->u.assign.expr->type == AST_TYPE_ANNOTATION &&
               node->u.assign.expr->u.type_annotation.cast_type == 1 /* CAST_DOUBLE */ &&
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
                    c->fn->var_type_tags[var_idx] = 2; /* CAST_INT */
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
            /* 优化3：上下文感知 - 赋值为变量 b = a 且 a 是 int 类型时，自动感知为 int 类型
               即使左侧变量没有显式声明 <int>，也自动推导为 int 类型，并使用优化路径 */
            else if(node->u.assign.expr && node->u.assign.expr->type == AST_VAR) {
                const char* rhs_name = node->u.assign.expr->u.varname;
                int rhs_idx = bf_sym(c->fn, rhs_name);
                /* 检查右侧变量是否标记为 int 类型（CAST_INT = 2） */
                if(rhs_idx >= 0 && c->fn->var_type_tags &&
                   c->fn->var_type_tags[rhs_idx] == 2 /* CAST_INT */) {
                    /* OPC_LOAD_INT_VAR：直接从 int_vals 读取，零提取 */
                    emit(c, OPC_LOAD_INT_VAR, rhs_idx, 0);
                    /* OPC_STORE_INT_VAR：从 int 栈弹出，存储到 int_vals，零重复提取 */
                    emit(c, OPC_STORE_INT_VAR, var_idx, 0);
                    /* 上下文感知：自动将左侧变量标记为 int 类型 */
                    c->fn->var_type_tags[var_idx] = 2; /* CAST_INT */
                }
                /* 检查右侧变量是否标记为 double 类型（CAST_DOUBLE = 1） */
                else if(rhs_idx >= 0 && c->fn->var_type_tags &&
                   c->fn->var_type_tags[rhs_idx] == 1 /* CAST_DOUBLE */) {
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
                    c_expr(c, node->u.assign.expr);
                    emit(c, OPC_STORE_VAR, var_idx, 0);
                }
            } else {
                c_expr(c, node->u.assign.expr);
                emit(c, OPC_STORE_VAR, var_idx, 0);
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
            c_expr(c, node->u.bin.left);
            c_expr(c, node->u.bin.right);
            static const OpCode map[] = {
                [OP_ADD] = OPC_ADD, [OP_SUB] = OPC_SUB, [OP_MUL] = OPC_MUL, [OP_DIV] = OPC_DIV,
                [OP_MOD] = OPC_MOD,
                [OP_GT] = OPC_GT, [OP_LT] = OPC_LT, [OP_GE] = OPC_GE, [OP_LE] = OPC_LE,
                [OP_EQ] = OPC_EQ, [OP_NE] = OPC_NE, [OP_IMPLEMENTS] = OPC_IMPLEMENTS,
            };
            emit(c, map[bop], 0, 0);
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
               基础版本：复用 OPC_CAST 指令，按标注类型转换值
               后续扩展：变量级类型标记传播，编译期零开销优化 */
            Value fv;
            if(fold_const(c, node->u.type_annotation.expr, &fv)) {
                /* 编译期常量：按标注类型折叠，做 C 风格截断/扩展 */
                int ct = node->u.type_annotation.cast_type;
                if(ct == CAST_VOID) {
                    fv = val_none();
                } else if(ct == CAST_DOUBLE || ct == CAST_FLOAT || ct == CAST_LONG_DOUBLE) {
                    double dv = (fv.type == VAL_DOUBLE) ? fv.v.d : (double)fv.v.i;
                    if(ct == CAST_FLOAT) dv = (float)dv;
                    fv = lumyr_make_double(dv);
                } else {
                    long long iv = (fv.type == VAL_INT) ? fv.v.i : (long long)fv.v.d;
                    switch(ct) {
                        case CAST_INT8: iv = (long long)(int8_t)iv; break;
                        case CAST_INT16: case CAST_SHORT: iv = (long long)(int16_t)iv; break;
                        case CAST_INT32: case CAST_INT: iv = (long long)(int32_t)iv; break;
                        case CAST_UINT8: case CAST_UCHAR: case CAST_BYTE: iv = (long long)(uint8_t)(unsigned long long)iv; break;
                        case CAST_UINT16: case CAST_USHORT: iv = (long long)(uint16_t)(unsigned long long)iv; break;
                        case CAST_UINT32: iv = (long long)(uint32_t)(unsigned long long)iv; break;
                        case CAST_CHAR: iv = (long long)(unsigned char)iv; break;
                        case CAST_BOOL: iv = iv ? 1 : 0; break;
                        /* int64/long/long long/uint64/ulong/size_t/ssize_t/ptr：不截断 */
                        default: break;
                    }
                    fv = lumyr_make_int(iv);
                }
                emit(c, OPC_LOAD_CONST, bf_const(c->fn, fv), 0);
                break;
            }
            c_expr(c, node->u.type_annotation.expr);
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
            c_expr(c, arr);
            c_expr(c, idx);
            emit(c, OPC_INDEX_GET, 0, 0);
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
