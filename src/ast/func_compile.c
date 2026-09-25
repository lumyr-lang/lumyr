#include "func_compile.h"
#include "ast_interp.h"
#include "stackframe.h"
#include "lumyr_value.h"
#include <stdlib.h>
#include <string.h>
#include "ast_node.h"
#include "ir/ir_compile.h"
#include "ast_runtime_sym.h"
#include "ast_types.h"
#include "ir/vm.h"
#include "gc_runtime.h"

// ---- 全局函数 AST 表（用于 const fn 编译期求值查找）----
#define FUNC_AST_MAX 512
static struct { char* name; AstNode* ast; } g_func_ast_table[FUNC_AST_MAX];
static int g_func_ast_cnt = 0;

void func_ast_register(const char* name, AstNode* func_ast) {
    if(g_func_ast_cnt >= FUNC_AST_MAX) return;
    // 检查是否已注册（避免重复）
    for(int i = 0; i < g_func_ast_cnt; i++) {
        if(strcmp(g_func_ast_table[i].name, name) == 0) {
            g_func_ast_table[i].ast = func_ast;
            return;
        }
    }
    g_func_ast_table[g_func_ast_cnt].name = strdup(name);
    g_func_ast_table[g_func_ast_cnt].ast = func_ast;
    g_func_ast_cnt++;
}

AstNode* func_ast_lookup(const char* name) {
    for(int i = 0; i < g_func_ast_cnt; i++) {
        if(strcmp(g_func_ast_table[i].name, name) == 0)
            return g_func_ast_table[i].ast;
    }
    return NULL;
}

// ---- 当前被调函数：解释器entry入口处查询自身payload用 ----
// 调用点先set、entry入口立即读取到局部变量，之后嵌套调用不影响
// _Thread_local：多线程 VM 通道（thread 启动的线程各自调用函数）需要每线程隔离
static _Thread_local RuntimeFunc* s_current_rf = NULL;

RuntimeFunc* interp_set_current_rf(RuntimeFunc* rf)
{
    RuntimeFunc* old = s_current_rf;
    s_current_rf = rf;
    return old;
}

RuntimeFunc* interp_current_rf(void)
{
    return s_current_rf;
}

_Bool interp_func_is_payload(const RuntimeFunc* rf)
{
    return rf && rf->capture_count == -1;
}

int interp_func_param_cnt(const RuntimeFunc* rf)
{
    if(!interp_func_is_payload(rf)) return 0;
    InterpFuncPayload* pl = (InterpFuncPayload*)rf->captures;
    return pl->param_cnt;
}

_Bool interp_func_has_variadic(const RuntimeFunc* rf)
{
    if(!interp_func_is_payload(rf)) return 0;
    InterpFuncPayload* pl = (InterpFuncPayload*)rf->captures;
    return pl->has_variadic;
}

const char* interp_func_param_name(const RuntimeFunc* rf, int idx)
{
    if(!interp_func_is_payload(rf)) return NULL;
    InterpFuncPayload* pl = (InterpFuncPayload*)rf->captures;
    if(idx < 0 || idx >= pl->param_cnt + pl->has_variadic) return NULL;
    return pl->param_names[idx];
}

int interp_func_param_has_default(const RuntimeFunc* rf, int idx)
{
    if(!interp_func_is_payload(rf)) return 0;
    InterpFuncPayload* pl = (InterpFuncPayload*)rf->captures;
    if(idx < 0 || idx >= pl->param_cnt) return 0;
    return pl->has_default[idx];
}

int interp_func_param_is_ref(const RuntimeFunc* rf, int idx)
{
    if(!interp_func_is_payload(rf)) return 0;
    InterpFuncPayload* pl = (InterpFuncPayload*)rf->captures;
    if(idx < 0 || idx >= pl->param_cnt) return 0;
    return pl->param_is_ref[idx];
}

AstNode* interp_func_param_default(const RuntimeFunc* rf, int idx)
{
    if(!interp_func_is_payload(rf)) return NULL;
    InterpFuncPayload* pl = (InterpFuncPayload*)rf->captures;
    if(idx < 0 || idx >= pl->param_cnt) return NULL;
    return pl->default_vals[idx];
}

// ================= 闭包捕获侧表 =================
// 单线程编译期状态：lambda 内部名 -> 其捕获的外层局部变量名列表。
// 仅在 typecheck 阶段写入，IR/VM 阶段读取。
typedef struct {
    char* lambda_name;
    char** names;
    int count;
    int cap;
} LambdaCaptureEntry;

static LambdaCaptureEntry* g_lambda_caps = NULL;
static int g_lambda_caps_cnt = 0;
static int g_lambda_caps_cap = 0;

static LambdaCaptureEntry* lcap_find(const char* lambda_name)
{
    for(int i = 0; i < g_lambda_caps_cnt; i++)
        if(strcmp(g_lambda_caps[i].lambda_name, lambda_name) == 0)
            return &g_lambda_caps[i];
    return NULL;
}

void func_compile_set_lambda_captures(const char* lambda_name, const char* const* names, int count)
{
    LambdaCaptureEntry* e = lcap_find(lambda_name);
    if(!e) {
        if(g_lambda_caps_cnt >= g_lambda_caps_cap) {
            int nc = g_lambda_caps_cap > 0 ? g_lambda_caps_cap * 2 : 16;
            LambdaCaptureEntry* nt = (LambdaCaptureEntry*)realloc(g_lambda_caps, (size_t)nc * sizeof(LambdaCaptureEntry));
            if(!nt) { fprintf(stderr, "闭包捕获表扩容内存不足\n"); exit(EXIT_FAILURE); }
            g_lambda_caps = nt;
            g_lambda_caps_cap = nc;
        }
        e = &g_lambda_caps[g_lambda_caps_cnt++];
        e->lambda_name = strdup(lambda_name);
        e->names = NULL;
        e->count = 0;
        e->cap = 0;
    }
    // 释放旧列表（typecheck 每轮可能重跑）
    for(int i = 0; i < e->count; i++) free(e->names[i]);
    free(e->names);
    e->names = NULL;
    e->count = 0;
    e->cap = 0;
    for(int i = 0; i < count; i++) {
        if(e->count >= e->cap) {
            int nc = e->cap > 0 ? e->cap * 2 : 8;
            char** nn = (char**)realloc(e->names, (size_t)nc * sizeof(char*));
            if(!nn) { fprintf(stderr, "闭包捕获名表扩容内存不足\n"); exit(EXIT_FAILURE); }
            e->names = nn; e->cap = nc;
        }
        e->names[e->count++] = strdup(names[i]);
    }
}

int lambda_capture_count(const char* lambda_name)
{
    LambdaCaptureEntry* e = lcap_find(lambda_name);
    return e ? e->count : 0;
}

const char* lambda_capture_name(const char* lambda_name, int i)
{
    LambdaCaptureEntry* e = lcap_find(lambda_name);
    if(!e || i < 0 || i >= e->count) return NULL;
    return e->names[i];
}

// ================= 全局捕获槽侧表 =================
// lambda 捕获的变量若为顶层(main 帧)变量，运行时帧不维护变量名、无法沿链按名查找，
// 必须在 ir_compile_main 的 fixup 阶段（main 符号表就绪后）把
// <lambda 名, 捕获名> → main 帧槽位索引 + PTR 族 hint 登记到本表，
// vm_exec_mkclosure 在当前函数帧找不到该捕获时查表，从根帧槽位取值建 cell。
typedef struct {
    char* lname;
    char* cname;
    int slot;
    int hint;
} GlobalCapSlot;

static GlobalCapSlot* g_gcap_slots = NULL;
static int g_gcap_cnt = 0, g_gcap_cap = 0;

void func_compile_set_global_cap(const char* lambda_name, const char* cap_name,
                                 int main_slot, int hint)
{
    for(int i = 0; i < g_gcap_cnt; i++) {
        if(strcmp(g_gcap_slots[i].lname, lambda_name) == 0 &&
           strcmp(g_gcap_slots[i].cname, cap_name) == 0) {
            g_gcap_slots[i].slot = main_slot;
            g_gcap_slots[i].hint = hint;
            return;
        }
    }
    if(g_gcap_cnt >= g_gcap_cap) {
        g_gcap_cap = g_gcap_cap ? g_gcap_cap * 2 : 16;
        GlobalCapSlot* nt = (GlobalCapSlot*)realloc(g_gcap_slots,
                                     sizeof(GlobalCapSlot) * (size_t)g_gcap_cap);
        if(!nt) { fprintf(stderr, "全局捕获槽表扩容内存不足\n"); exit(EXIT_FAILURE); }
        g_gcap_slots = nt;
    }
    GlobalCapSlot* s = &g_gcap_slots[g_gcap_cnt++];
    s->lname = strdup(lambda_name);
    s->cname = strdup(cap_name);
    s->slot = main_slot;
    s->hint = hint;
}

int func_compile_get_global_cap(const char* lambda_name, const char* cap_name,
                                int* hint_out)
{
    for(int i = 0; i < g_gcap_cnt; i++) {
        if(strcmp(g_gcap_slots[i].lname, lambda_name) == 0 &&
           strcmp(g_gcap_slots[i].cname, cap_name) == 0) {
            if(hint_out) *hint_out = g_gcap_slots[i].hint;
            return g_gcap_slots[i].slot;
        }
    }
    return -1;
}

// 生成闭包实例：复制模板 payload，沿当前帧链装箱 free 变量
Value closure_make_instance(RuntimeFunc* template_rf, StackFrame* cur_frame)
{
    InterpFuncPayload* tpl = (InterpFuncPayload*)template_rf->captures;
    const char* lambda_name = tpl->bytecode ? tpl->bytecode->name : NULL;
    int ncap = lambda_name ? lambda_capture_count(lambda_name) : 0;

    gc_disable();   // 构造新对象期间防止 GC 回收中间结果（当前帧仍是 GC 根）

    RuntimeFunc* rf = (RuntimeFunc*)malloc(sizeof(RuntimeFunc));
    InterpFuncPayload* pl = (InterpFuncPayload*)malloc(sizeof(InterpFuncPayload));
    *pl = *tpl;                       // 浅拷贝共享 body/bytecode/param_names
    pl->captured_names = NULL;
    pl->captured_cells = NULL;
    pl->captured_cell_count = 0;

    if(ncap > 0) {
        pl->captured_names = (char**)malloc((size_t)ncap * sizeof(char*));
        pl->captured_cells = (Value**)malloc((size_t)ncap * sizeof(Value*));
        for(int i = 0; i < ncap; i++) {
            const char* nm = lambda_capture_name(lambda_name, i);
            Value* cell = stackframe_ensure_cell(cur_frame, nm);
            if(!cell) {
                fprintf(stderr, "Runtime Error: 闭包无法捕获未定义变量 %s\n", nm);
                exit(EXIT_FAILURE);
            }
            pl->captured_names[i] = strdup(nm);
            pl->captured_cells[i] = cell;
        }
        pl->captured_cell_count = ncap;
    }

    rf->entry = template_rf->entry;
    rf->param_count = template_rf->param_count;
    rf->has_variadic = template_rf->has_variadic;
    rf->captures = (Value*)pl;
    rf->capture_count = -1;

    gc_enable();

    Value v;
    memset(&v, 0, sizeof(v));
    v.type = VAL_FUNC;
    v.v.func.func_obj = rf;
    v.v.func.ffi_func = NULL;
    v.v.func.is_ffi = 0;
    return v;
}

void closure_bind_cells(const RuntimeFunc* rf, StackFrame* callee)
{
    if(!interp_func_is_payload(rf) || !callee) return;
    InterpFuncPayload* pl = (InterpFuncPayload*)rf->captures;
    for(int i = 0; i < pl->captured_cell_count; i++) {
        stackframe_add_cell(callee, pl->captured_names[i], pl->captured_cells[i]);
    }
}

void lumyr_interp_scan_captures(const RuntimeFunc* rf, void (*mark)(Value))
{
    if(!rf || !interp_func_is_payload(rf)) return;
    InterpFuncPayload* pl = (InterpFuncPayload*)rf->captures;
    for(int i = 0; i < pl->captured_cell_count; i++) {
        if(pl->captured_cells[i]) {
            mark(*pl->captured_cells[i]);
        }
    }
}

RuntimeFunc* compile_func_from_ast_with_class(AstNode* func_def_ast, const char* class_name)
{
    if(func_def_ast->type != AST_FUNC_DEF) return NULL;

    // 注册到全局函数 AST 表（用于 const fn 编译期求值查找）
    func_ast_register(func_def_ast->u.func_def.name, func_def_ast);

    // 1. 解析参数链表 AST_PARAM，拷贝参数名，**只拷贝字符串，不存AST指针**
    int normal_cnt = 0;
    int has_var = 0;
    AstNode* p = func_def_ast->u.func_def.params;
    while(p) {
        if(p->u.param.is_ellipsis) {
            has_var = 1;
        } else {
            normal_cnt ++;
        }
        p = p->u.param.next;
    }

    // 分配解释器负载（不修改RuntimeFunc原有结构体！）
    InterpFuncPayload* payload = (InterpFuncPayload*)calloc(1, sizeof(InterpFuncPayload));
    payload->param_cnt = normal_cnt;
    payload->has_variadic = has_var;
    payload->param_names = malloc(sizeof(char*)*(normal_cnt + (has_var?1:0)));
    payload->default_vals = calloc(normal_cnt, sizeof(AstNode*));
    payload->has_default = calloc(normal_cnt, sizeof(int));
    payload->param_is_ref = calloc(normal_cnt, sizeof(int));
    payload->is_generator = func_def_ast->u.func_def.is_generator;

    // 拷贝参数名字
    p = func_def_ast->u.func_def.params;
    int idx = 0;
    while(p) {
        payload->param_names[idx] = strdup(p->u.param.name);
        if(!p->u.param.is_ellipsis && p->u.param.default_val) {
            payload->default_vals[idx] = p->u.param.default_val;
            payload->has_default[idx] = 1;
        }
        if(!p->u.param.is_ellipsis && p->u.param.is_ref) {
            payload->param_is_ref[idx] = 1;
        }
        idx++;
        p = p->u.param.next;
    }
    // 编译函数体为字节码 IR（VM 执行；不再直接求值 AST）
    payload->body = func_def_ast->u.func_def.body;
    payload->bytecode = ir_compile_function(func_def_ast->u.func_def.name, func_def_ast->u.func_def.params, func_def_ast->u.func_def.body,
                                            func_def_ast->u.func_def.is_generator, class_name,
                                            func_def_ast->u.func_def.ret_type_name);

    // 构造RuntimeFunc，原有字段一个不动
    RuntimeFunc* rf = malloc(sizeof(RuntimeFunc));
    rf->entry = vm_func_entry;              // VM 执行字节码
    rf->param_count = normal_cnt;
    rf->has_variadic = has_var;
    rf->captures = NULL;
    rf->capture_count = 0;

    // trick：RuntimeFunc没有payload字段，用captures临时存payload指针（或者包装一层wrapper，不修改原有结构体）
    // 【重要】生产环境建议外层包一层wrapper，这里为不改结构体，用captures指针存payload，仅解释器模式使用；编译模式完全不用。
    rf->captures = (Value*)payload;
    rf->capture_count = -1; // 标记这是解释器payload，不是真实捕获变量

    return rf;
}


RuntimeFunc* compile_func_from_ast(AstNode* func_def_ast)
{
    return compile_func_from_ast_with_class(func_def_ast, NULL);
}

/* 编译 class 静态方法。静态方法名在 parse 期已被扁平化为 <Class>_<method>
 * 并按全局函数注册（main 等按该 flat 名静态解析调用），故这里**不能**再用
 * compile_func_from_ast_with_class——它会把注册名二次 mangle 成
 * <Class>__m__<Class>_<method>，导致调用点按 flat 名查不到、退化为动态调用。
 * 只需在 flat 字节码上回填 fn->class_name，使方法体执行期间访问控制上下文
 * 标记属主类（同类内访问 private 成员）。 */
RuntimeFunc* compile_static_func_from_ast(AstNode* func_def_ast, const char* class_name)
{
    RuntimeFunc* rf = compile_func_from_ast(func_def_ast);   /* flat 名，class_name=NULL */
    if(rf && class_name) {
        InterpFuncPayload* pl = (InterpFuncPayload*)rf->captures;
        if(pl && pl->bytecode) {
            if(pl->bytecode->class_name) free(pl->bytecode->class_name);
            pl->bytecode->class_name = strdup(class_name);
        }
    }
    return rf;
}

// typecheck 把 AST_VAR（函数名）就地转成 AST_FUNCREF 后，重新编译该函数的
// 字节码并替换（parse 期生成的旧字节码里函数名引用还是 LOAD_VAR）。
void func_compile_recompile(AstNode* def)
{
    if(!def || def->type != AST_FUNC_DEF) return;
    const char* name = def->u.func_def.name;
    BytecodeFunc* nb = ir_func_table_recompile(name, def->u.func_def.params, def->u.func_def.body,
                                               def->u.func_def.is_generator, NULL,
                                               def->u.func_def.ret_type_name);
    Value fv = sym_get(name);
    if(fv.type == VAL_FUNC) {
        RuntimeFunc* rf = (RuntimeFunc*)fv.v.func.func_obj;
        InterpFuncPayload* pl = (InterpFuncPayload*)rf->captures;
        /* 旧字节码已由 ir_func_table_recompile 释放（原位替换），这里只更新指针 */
        if(pl) pl->bytecode = nb;
    }
}

/* 重编译 struct/class 方法（方法在函数表用内部名 <owner>__m__<method>）。
 * 更新字节码后，同步更新 TypeDef 方法表中 RuntimeFunc payload 的 bytecode 指针，
 * 否则运行时分派仍持有已释放的旧字节码（use-after-free）。
 * 调用须晚于方法体内嵌套 lambda 的重编译（func_compile_recompile）。 */
void func_compile_recompile_method(const char* owner, AstNode* def)
{
    if(!owner || !def || def->type != AST_FUNC_DEF) return;
    const char* mname = def->u.func_def.name;
    BytecodeFunc* nb = ir_func_table_recompile(mname, def->u.func_def.params,
                                               def->u.func_def.body,
                                               def->u.func_def.is_generator,
                                               owner,
                                               def->u.func_def.ret_type_name);
    TypeDef* td = struct_lookup(owner);
    if(!td) td = class_lookup(owner);
    if(!td) return;
    for(int i = 0; i < td->nmethods; i++) {
        if(!td->method_names[i] || strcmp(td->method_names[i], mname) != 0) continue;
        RuntimeFunc* rf = (RuntimeFunc*)td->method_funcs[i];
        if(interp_func_is_payload(rf)) {
            InterpFuncPayload* pl = (InterpFuncPayload*)rf->captures;
            if(pl) pl->bytecode = nb;
        }
    }
}

void runtime_func_destroy(RuntimeFunc* f)
{
    if(!f) return;
    if(f->capture_count == -1) {
        InterpFuncPayload* pl = (InterpFuncPayload*)f->captures;
        for(int i=0;i<pl->param_cnt + pl->has_variadic;i++) {
            free(pl->param_names[i]);
        }
        free(pl->param_names);
        free(pl->default_vals);
        free(pl->has_default);
        for(int i = 0; i < pl->captured_cell_count; i++) {
            free(pl->captured_names[i]);
            free(pl->captured_cells[i]);   // cell 由该闭包实例独占释放
        }
        free(pl->captured_names);
        free(pl->captured_cells);
        bytecode_func_free(pl->bytecode);
        free(pl);
    } else {
        for(int i=0;i<f->capture_count;i++) {
            val_destroy(&f->captures[i]);
        }
        free(f->captures);
    }
    free(f);
}

