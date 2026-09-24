/* 编译期模块：func_depth/in_lambda/lambda_locals_cnt/g_global_vars_cnt 为单线程编译状态，
 * 未来并发编译需实例化。 */
#include "ast_typecheck.h"
#include "lumyr_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ast_symtab.h"
#include "ast_runtime_sym.h"
#include "func_compile.h"
#include "ast_types.h"
#include "ast_node.h"
#include "lm_type.h"


// ---------------- 作用域快照 ----------------
// static_sym 是全局单表；检查函数体前保存、检查后恢复，
// 使函数参数/局部变量不泄漏到顶层，且参数能遮蔽同名全局。

/* 作用域快照：保存当前作用域层级，检查函数体后恢复。 */
static void sym_save(void) {
    static_sym_save();
}

static void sym_restore(void) {
    static_sym_restore();
}

// 函数体递归深度：>0 表示正在检查某函数体，其内的嵌套 func 定义跳过
/* typecheck 把函数名引用（AST_VAR→AST_FUNCREF）就地转换后，记录受影响的函数定义，
 * 结束阶段重编译其字节码（parse 期生成的字节码里函数名引用还是 LOAD_VAR）。 */
static AstNode* g_cur_func_def = NULL;
static AstNode** g_recompile = NULL;
static int g_recompile_cnt = 0;
static int g_recompile_cap = 0;

/* struct/class 方法重编译表：方法节点不在程序 AST 树中，单独记录属主以便重编译。
 * 方法内嵌套的 lambda/arrow 仍进 g_recompile（它们是普通函数）。 */
typedef struct {
    char* owner;          /* struct/class 名 */
    AstNode* node;        /* 方法 AST_FUNC_DEF 节点 */
} MethodRecomp;
static MethodRecomp* g_method_recomp = NULL;
static int g_method_recomp_cnt = 0;
static int g_method_recomp_cap = 0;
/* 当前正在 typecheck 的方法属主（NULL=普通函数上下文） */
static const char* g_method_owner = NULL;
/* 当前是否正在检查构造函数（const 字段在构造函数内允许首次赋值/初始化） */
static int g_in_ctor = 0;

static void method_recomp_add(const char* owner, AstNode* node) {
    for(int i = 0; i < g_method_recomp_cnt; i++)
        if(g_method_recomp[i].node == node) return;  /* 去重 */
    if(g_method_recomp_cnt >= g_method_recomp_cap) {
        int nc = g_method_recomp_cap > 0 ? g_method_recomp_cap * 2 : 16;
        MethodRecomp* nt = (MethodRecomp*)realloc(g_method_recomp, (size_t)nc * sizeof(MethodRecomp));
        if(!nt) { LOG_ERROR("方法重编译表扩容内存不足\n"); exit(EXIT_FAILURE); }
        g_method_recomp = nt; g_method_recomp_cap = nc;
    }
    g_method_recomp[g_method_recomp_cnt].owner = strdup(owner);
    g_method_recomp[g_method_recomp_cnt].node = node;
    g_method_recomp_cnt++;
}

/* ============== 变量持有类型（owner）追踪：用于字段访问控制 ============== */
typedef struct { char* var; char* owner; } VarOwner;
static VarOwner* g_vo = NULL;
static int g_vo_cnt = 0, g_vo_cap = 0;

static void vo_reset(void) {
    for(int i = 0; i < g_vo_cnt; i++) { free(g_vo[i].var); free(g_vo[i].owner); }
    free(g_vo); g_vo = NULL; g_vo_cnt = g_vo_cap = 0;
}
/* 作用域回滚：释放 base 之后登记的项（函数/方法返回时调用） */
static void vo_rollback(int base) {
    for(int i = g_vo_cnt - 1; i >= base; i--) { free(g_vo[i].var); free(g_vo[i].owner); }
    g_vo_cnt = base;
}
/* 记录变量持有的自定义类型名（同作用域内同名变量多次赋值会追加多条，查询从最近取） */
static void vo_set(const char* var, const char* owner) {
    if(!var || !owner) return;
    if(g_vo_cnt >= g_vo_cap) {
        g_vo_cap = g_vo_cap ? g_vo_cap * 2 : 32;
        g_vo = (VarOwner*)realloc(g_vo, (size_t)g_vo_cap * sizeof(VarOwner));
    }
    g_vo[g_vo_cnt].var = strdup(var);
    g_vo[g_vo_cnt].owner = strdup(owner);
    g_vo_cnt++;
}
/* 从最近作用域向后查变量持有类型；无则 NULL */
static const char* vo_get(const char* var) {
    for(int i = g_vo_cnt - 1; i >= 0; i--)
        if(strcmp(g_vo[i].var, var) == 0) return g_vo[i].owner;
    return NULL;
}

/* 在某类型字段表中按名查找 FieldInfo（含父类扁平化字段） */
static FieldInfo* tc_find_field(const char* owner, const char* fname) {
    TypeDef* td = type_lookup(owner);
    if(!td || !td->runtime_info) return NULL;
    RuntimeTypeInfo* info = td->runtime_info;
    for(int i = 0; i < info->nfields; i++)
        if(strcmp(info->fields[i].name, fname) == 0) return &info->fields[i];
    return NULL;
}

/* 推断表达式持有的自定义类型名（返回借用指针，不 strdup；不可推断返回 NULL）。
 * 动态/不可静态追踪的形态返回 NULL，访问控制对这些情形保守放行（避免误报）。 */
static const char* tc_owner(AstNode* node) {
    if(!node) return NULL;
    if(node->type == AST_CLASS_NEW) return node->u.class_new.class_name;
    if(node->type == AST_VAR) {
        const char* vn = node->u.varname;
        if(strcmp(vn, "self") == 0) return g_method_owner;
        if(strcmp(vn, "super") == 0) {
            if(!g_method_owner) return NULL;
            TypeDef* td = class_lookup(g_method_owner);
            return td ? td->parent : NULL;
        }
        return vo_get(vn);
    }
    if(node->type == AST_INDEX) {
        /* base.field：字段若持有自定义类型，FieldInfo.type_name 给出其类型 */
        const char* base = tc_owner(node->u.index.arr);
        if(base && node->u.index.idx->type == AST_STRING) {
            FieldInfo* fi = tc_find_field(base, node->u.index.idx->u.sval);
            if(fi) return fi->type_name;
        }
    }
    return NULL;
}

/* 当前上下文是否允许访问 owner 类中 access 级别的成员 */
static int tc_access_ok(const char* owner, int access) {
    if(access == ACCESS_PUBLIC) return 1;
    const char* ctx = g_method_owner;
    if(!ctx) return 0;                        /* 外部上下文：private/protected 均拒绝 */
    if(strcmp(ctx, owner) == 0) return 1;     /* 同类：private/protected 允许 */
    if(access == ACCESS_PROTECTED) {
        /* ctx 沿继承链是否派生自 owner */
        TypeDef* td = class_lookup(ctx);
        while(td && td->parent) {
            if(strcmp(td->parent, owner) == 0) return 1;
            td = class_lookup(td->parent);
        }
    }
    return 0;
}

/* ======================================================================
 * 编译期常量折叠（const func）
 *
 * 语义：
 *  - const func 为纯函数：体内只允许 const 局部声明与 return；表达式只能由
 *    参数、常量、纯算术/比较/逻辑运算及其他 const func 调用组成；
 *  - 在 const 声明的初始化表达式中调用 const func 时，编译期直接求值，并把
 *    该初始化表达式就地改写为结果字面量（常量折叠）。
 * ====================================================================== */
typedef enum {
    CEK_NONE = 0, CEK_INT, CEK_FLOAT, CEK_BOOL, CEK_STRING
} CEKind;
typedef struct {
    CEKind kind;
    long long i;
    double d;
    const char* s;   /* 借用：指向 AST 字面量，不释放 */
} CEVal;
static void ce_set_none(CEVal* v) { v->kind = CEK_NONE; v->i = 0; v->d = 0; v->s = NULL; }

/* const func 注册表（顶层 const func：name → AST_FUNC_DEF） */
typedef struct { char* name; AstNode* node; } ConstFuncEntry;
static ConstFuncEntry* g_cf = NULL;
static int g_cf_cnt = 0, g_cf_cap = 0;
static AstNode* cf_lookup(const char* name) {
    for(int i = 0; i < g_cf_cnt; i++)
        if(strcmp(g_cf[i].name, name) == 0) return g_cf[i].node;
    return NULL;
}
static void cf_register(const char* name, AstNode* node) {
    if(cf_lookup(name)) return;
    if(g_cf_cnt >= g_cf_cap) {
        int nc = g_cf_cap ? g_cf_cap * 2 : 16;
        ConstFuncEntry* nt = (ConstFuncEntry*)realloc(g_cf, (size_t)nc * sizeof(ConstFuncEntry));
        if(!nt) { LOG_ERROR("const func 表扩容内存不足\n"); exit(EXIT_FAILURE); }
        g_cf = nt; g_cf_cap = nc;
    }
    g_cf[g_cf_cnt].name = strdup(name);
    g_cf[g_cf_cnt].node = node;
    g_cf_cnt++;
}

/* 已折叠的顶层 const 变量（name → CEVal），供后续 const/const func 引用 */
typedef struct { char* name; CEVal v; } ConstVarEntry;
static ConstVarEntry* g_cv = NULL;
static int g_cv_cnt = 0, g_cv_cap = 0;
static int cv_get(const char* name, CEVal* out) {
    for(int i = g_cv_cnt - 1; i >= 0; i--)
        if(strcmp(g_cv[i].name, name) == 0) { *out = g_cv[i].v; return 1; }
    return 0;
}
static void cv_put(const char* name, CEVal v) {
    if(g_cv_cnt >= g_cv_cap) {
        int nc = g_cv_cap ? g_cv_cap * 2 : 16;
        ConstVarEntry* nt = (ConstVarEntry*)realloc(g_cv, (size_t)nc * sizeof(ConstVarEntry));
        if(!nt) { LOG_ERROR("const 变量表扩容内存不足\n"); exit(EXIT_FAILURE); }
        g_cv = nt; g_cv_cap = nc;
    }
    g_cv[g_cv_cnt].name = strdup(name);
    g_cv[g_cv_cnt].v = v;
    g_cv_cnt++;
}

/* 参数 / 局部 const 绑定（求值 const func body 期间生效，返回时回滚） */
typedef struct { char* name; CEVal v; } CEBind;
static CEBind* g_ceb = NULL;
static int g_ceb_cnt = 0, g_ceb_cap = 0;
static void ceb_rollback(int base) {
    for(int i = g_ceb_cnt - 1; i >= base; i--) free(g_ceb[i].name);
    g_ceb_cnt = base;
}
static int ceb_get(const char* name, CEVal* out) {
    for(int i = g_ceb_cnt - 1; i >= 0; i--)
        if(strcmp(g_ceb[i].name, name) == 0) { *out = g_ceb[i].v; return 1; }
    return 0;
}
static void ceb_put(const char* name, CEVal v) {
    if(g_ceb_cnt >= g_ceb_cap) {
        int nc = g_ceb_cap ? g_ceb_cap * 2 : 16;
        CEBind* nt = (CEBind*)realloc(g_ceb, (size_t)nc * sizeof(CEBind));
        if(!nt) { LOG_ERROR("绑定表扩容内存不足\n"); exit(EXIT_FAILURE); }
        g_ceb = nt; g_ceb_cap = nc;
    }
    g_ceb[g_ceb_cnt].name = strdup(name);
    g_ceb[g_ceb_cnt].v = v;
    g_ceb_cnt++;
}

static int ce_is_num(const CEVal* v) { return v->kind == CEK_INT || v->kind == CEK_FLOAT; }
static double ce_as_double(const CEVal* v) { return v->kind == CEK_FLOAT ? v->d : (double)v->i; }

/* 二元运算常量求值；类型不支持返回 0 */
static int ce_binop(BinOp op, const CEVal* a, const CEVal* b, CEVal* out) {
    ce_set_none(out);
    switch(op) {
        case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: case OP_MOD: {
            if(!ce_is_num(a) || !ce_is_num(b)) return 0;
            if(a->kind == CEK_FLOAT || b->kind == CEK_FLOAT) {
                if(op == OP_MOD) return 0;   /* 浮点不支持取模 */
                double x = ce_as_double(a), y = ce_as_double(b), r = 0;
                switch(op) {
                    case OP_ADD: r = x + y; break;
                    case OP_SUB: r = x - y; break;
                    case OP_MUL: r = x * y; break;
                    case OP_DIV: r = x / y; break;
                    default: return 0;
                }
                out->kind = CEK_FLOAT; out->d = r;
            } else {
                long long x = a->i, y = b->i, r = 0;
                switch(op) {
                    case OP_ADD: r = x + y; break;
                    case OP_SUB: r = x - y; break;
                    case OP_MUL: r = x * y; break;
                    case OP_DIV: r = x / y; break;
                    case OP_MOD: r = x % y; break;
                    default: return 0;
                }
                out->kind = CEK_INT; out->i = r;
            }
            return 1;
        }
        case OP_GT: case OP_LT: case OP_GE: case OP_LE: {
            if(!ce_is_num(a) || !ce_is_num(b)) return 0;
            double x = ce_as_double(a), y = ce_as_double(b); int r = 0;
            switch(op) {
                case OP_GT: r = x > y; break;
                case OP_LT: r = x < y; break;
                case OP_GE: r = x >= y; break;
                case OP_LE: r = x <= y; break;
                default: return 0;
            }
            out->kind = CEK_BOOL; out->i = r; return 1;
        }
        case OP_EQ: case OP_NE: {
            int eq = 0;
            if(ce_is_num(a) && ce_is_num(b)) eq = ce_as_double(a) == ce_as_double(b);
            else if(a->kind == CEK_STRING && b->kind == CEK_STRING) eq = strcmp(a->s, b->s) == 0;
            else if(a->kind == CEK_BOOL && b->kind == CEK_BOOL) eq = a->i == b->i;
            else return 0;
            out->kind = CEK_BOOL; out->i = (op == OP_EQ) ? eq : !eq; return 1;
        }
        case OP_LOGIC_AND: case OP_LOGIC_OR: {
            if(a->kind != CEK_BOOL || b->kind != CEK_BOOL) return 0;
            int r = (op == OP_LOGIC_AND) ? (a->i && b->i) : (a->i || b->i);
            out->kind = CEK_BOOL; out->i = r; return 1;
        }
        default: return 0;
    }
}
/* 一元运算常量求值 */
static int ce_unary(BinOp op, const CEVal* a, CEVal* out) {
    ce_set_none(out);
    if(op == OP_UNARY_MINUS || op == OP_UNARY_PLUS) {
        if(!ce_is_num(a)) return 0;
        if(a->kind == CEK_FLOAT) { out->kind = CEK_FLOAT; out->d = op == OP_UNARY_MINUS ? -a->d : a->d; }
        else { out->kind = CEK_INT; out->i = op == OP_UNARY_MINUS ? -a->i : a->i; }
        return 1;
    }
    if(op == OP_LOGIC_NOT) {
        if(a->kind != CEK_BOOL) return 0;
        out->kind = CEK_BOOL; out->i = !a->i; return 1;
    }
    return 0;
}

static int ce_eval(AstNode* node, CEVal* out);   /* 前向声明（调用与求值互相递归） */

/* 实参链（AST_SEQ 左嵌套：first 在前）逐个求值到 vals[]；任一非常量返回 -1 */
static int ce_eval_args(AstNode* args, CEVal* vals, int max) {
    if(!args) return 0;
    if(args->type == AST_SEQ) {
        int n1 = ce_eval_args(args->u.seq.first, vals, max);
        if(n1 < 0) return -1;
        int n2 = ce_eval_args(args->u.seq.second, vals + n1, max - n1);
        if(n2 < 0) return -1;
        return n1 + n2;
    }
    if(max <= 0) return -1;
    if(!ce_eval(args, vals)) return -1;
    return 1;
}

/* const func body 语句求值：只允许 const 局部声明与 return；返回 return 表达式值 */
static int ce_body_stmts(AstNode* s, CEVal* ret, int* got_ret) {
    if(!s) return 1;
    if(s->type == AST_SEQ)
        return ce_body_stmts(s->u.seq.first, ret, got_ret) &
               ce_body_stmts(s->u.seq.second, ret, got_ret);
    if(s->type == AST_RETURN) {
        if(!s->u.ret.ret_val) return 0;
        if(!ce_eval(s->u.ret.ret_val, ret)) return 0;
        *got_ret = 1; return 1;
    }
    if(s->type == AST_ASSIGN) {
        if(!s->u.assign.is_const) return 0;
        CEVal v;
        if(!ce_eval(s->u.assign.expr, &v)) return 0;
        ceb_put(s->u.assign.varname, v);
        return 1;
    }
    return 0;   /* 非纯语句 */
}

/* 调用 const func：求值实参、绑定形参、求 body */
static int ce_call_func(AstNode* fn, AstNode* call_args, CEVal* out) {
    if(!fn || fn->type != AST_FUNC_DEF || !fn->u.func_def.is_const) return 0;
    CEVal avals[32];
    int nargs = ce_eval_args(call_args, avals, 32);
    if(nargs < 0) return 0;
    int base = g_ceb_cnt;
    AstNode* pp = fn->u.func_def.params;
    int pi = 0;
    while(pp && pi < nargs) {
        ceb_put(pp->u.param.name, avals[pi]);
        pp = pp->u.param.next; pi++;
    }
    AstNode* stmts = fn->u.func_def.body ? fn->u.func_def.body->u.block.stmts : NULL;
    CEVal ret; ce_set_none(&ret);
    int got_ret = 0;
    int ok = ce_body_stmts(stmts, &ret, &got_ret);
    ceb_rollback(base);
    if(!ok || !got_ret) return 0;
    *out = ret; return 1;
}

/* 表达式编译期求值；不可静态求值返回 0 */
static int ce_eval(AstNode* node, CEVal* out) {
    ce_set_none(out);
    if(!node) return 0;
    switch(node->type) {
        case AST_INT:    out->kind = CEK_INT;   out->i = node->u.inum; return 1;
        case AST_NUM:    out->kind = CEK_FLOAT; out->d = node->u.num;  return 1;
        case AST_BOOL:   out->kind = CEK_BOOL;  out->i = node->u.bval; return 1;
        case AST_STRING: out->kind = CEK_STRING; out->s = node->u.sval; return 1;
        case AST_BINOP: {
            CEVal a, b;
            if(!ce_eval(node->u.bin.left, &a)) return 0;
            if(!ce_eval(node->u.bin.right, &b)) return 0;
            return ce_binop(node->u.bin.op, &a, &b, out);
        }
        case AST_UNARY: {
            CEVal a;
            if(!ce_eval(node->u.uny.child, &a)) return 0;
            return ce_unary(node->u.uny.op, &a, out);
        }
        case AST_TERNARY: {
            CEVal c;
            if(!ce_eval(node->u.ternary.cond, &c) || c.kind != CEK_BOOL) return 0;
            return ce_eval(c.i ? node->u.ternary.true_expr : node->u.ternary.false_expr, out);
        }
        case AST_VAR:
            if(ceb_get(node->u.varname, out)) return 1;   /* 参数 / 局部 const */
            if(cv_get(node->u.varname, out)) return 1;     /* 顶层已折叠 const */
            return 0;
        case AST_CALL: {
            AstNode* fn = cf_lookup(node->u.call.name);
            if(!fn) return 0;
            return ce_call_func(fn, node->u.call.args, out);
        }
        default: return 0;
    }
}

/* ---- const func 纯函数约束校验（不依赖调用，定义即检查） ---- */
/* 表达式纯度：AST_VAR 引用宽松放行（参数/const/全局引用，折叠成败由求值决定），
 * AST_CALL 只能调用 const func；方法调用/字段访问/new 等涉及对象状态，判不纯。 */
static int ce_pure_expr(AstNode* node) {
    if(!node) return 1;
    switch(node->type) {
        case AST_INT: case AST_NUM: case AST_BOOL: case AST_STRING: return 1;
        case AST_VAR: return 1;
        case AST_BINOP: return ce_pure_expr(node->u.bin.left) && ce_pure_expr(node->u.bin.right);
        case AST_UNARY: return ce_pure_expr(node->u.uny.child);
        case AST_TERNARY: return ce_pure_expr(node->u.ternary.cond) &&
                              ce_pure_expr(node->u.ternary.true_expr) &&
                              ce_pure_expr(node->u.ternary.false_expr);
        case AST_CALL: {
            AstNode* fn = cf_lookup(node->u.call.name);
            if(!fn || !fn->u.func_def.is_const) return 0;
            if(!node->u.call.args) return 1;
            if(node->u.call.args->type != AST_SEQ) return ce_pure_expr(node->u.call.args);
            return ce_pure_expr(node->u.call.args->u.seq.first) &&
                   ce_pure_expr(node->u.call.args->u.seq.second);
        }
        default: return 0;
    }
}
static int ce_check_stmts(AstNode* s, int* has_ret) {
    if(!s) return 1;
    if(s->type == AST_SEQ)
        return ce_check_stmts(s->u.seq.first, has_ret) &
               ce_check_stmts(s->u.seq.second, has_ret);
    if(s->type == AST_RETURN) {
        *has_ret = 1;
        return s->u.ret.ret_val ? ce_pure_expr(s->u.ret.ret_val) : 1;
    }
    if(s->type == AST_ASSIGN)
        return s->u.assign.is_const && ce_pure_expr(s->u.assign.expr);
    return 0;
}
/* 校验 const func：body 只含 const 声明/return，且必须有 return */
static int ce_validate_func(AstNode* fn) {
    AstNode* stmts = fn->u.func_def.body ? fn->u.func_def.body->u.block.stmts : NULL;
    int has_ret = 0;
    return ce_check_stmts(stmts, &has_ret) && has_ret;
}

/* 折叠结果 → 结果字面量节点 */
static AstNode* ce_make_literal(CEVal v) {
    switch(v.kind) {
        case CEK_INT:   return ast_int(v.i);
        case CEK_FLOAT: return ast_num(v.d);
        case CEK_BOOL:  return ast_bool(v.i);
        case CEK_STRING: return ast_string(v.s);
        default: return NULL;
    }
}

static int func_depth = 0;
static int g_collect_err = 0;   // 顶层收集阶段错误（函数重复定义等）
/* 匿名函数捕获：lambda 只能访问 参数 + 全局变量 + 自身局部；
 * 引用到的外层函数局部变量记录为"捕获变量"，运行时装箱为闭包 cell。 */
static int in_lambda = 0;
static AstNode* g_lambda_params = NULL;
static char** lambda_locals = NULL;
static int lambda_locals_cnt = 0;
static int lambda_locals_cap = 0;
static char** g_global_vars = NULL;
static int g_global_vars_cnt = 0;
static int g_global_vars_cap = 0;

// 捕获列表栈：每层 lambda 一个，记录其引用的外层局部变量名（引用语义）
typedef struct { char** names; int cnt; int cap; } CaptList;
static CaptList* g_cap_stack = NULL;
static int g_cap_depth = 0;
static int g_cap_cap = 0;

static void cap_push(void) {
    if(g_cap_depth >= g_cap_cap) {
        int nc = g_cap_cap > 0 ? g_cap_cap * 2 : 8;
        CaptList* nt = (CaptList*)realloc(g_cap_stack, (size_t)nc * sizeof(CaptList));
        if(!nt) { LOG_ERROR("捕获栈扩容内存不足\n"); exit(EXIT_FAILURE); }
        g_cap_stack = nt; g_cap_cap = nc;
    }
    g_cap_stack[g_cap_depth].names = NULL;
    g_cap_stack[g_cap_depth].cnt = 0;
    g_cap_stack[g_cap_depth].cap = 0;
    g_cap_depth++;
}

static void cap_add(const char* name) {
    CaptList* L = &g_cap_stack[g_cap_depth - 1];
    for(int i = 0; i < L->cnt; i++) if(strcmp(L->names[i], name) == 0) return;
    if(L->cnt >= L->cap) {
        int nc = L->cap > 0 ? L->cap * 2 : 8;
        char** nn = (char**)realloc(L->names, (size_t)nc * sizeof(char*));
        if(!nn) { LOG_ERROR("捕获名表扩容内存不足\n"); exit(EXIT_FAILURE); }
        L->names = nn; L->cap = nc;
    }
    L->names[L->cnt++] = strdup(name);
}

static int is_global_var(const char* n);
static int is_lambda_local(const char* n);
static int is_lambda_param(const char* n);

// 弹出当前 lambda 的捕获列表并登记到侧表，返回捕获个数。
// 关键：内层 lambda 捕获的变量，若不是当前（外层）lambda 的形参/局部/全局，
// 则外层 lambda 也必须捕获该变量（闭包捕获向上传播），否则运行时无法传递。
static int cap_pop_record(const char* lambda_name) {
    g_cap_depth--;
    CaptList* L = &g_cap_stack[g_cap_depth];
    func_compile_set_lambda_captures(lambda_name, (const char* const*)L->names, L->cnt);
    int n = L->cnt;
    /* 向上传播：把本层捕获中不属于外层作用域的变量加入外层捕获表 */
    if(g_cap_depth > 0) {
        for(int i = 0; i < L->cnt; i++) {
            const char* cn = L->names[i];
            if(!is_lambda_param(cn) && !is_lambda_local(cn)) {
                /* 全局变量也传播：外层 lambda 同样需要捕获，
                 * 否则外层无该槽位，内层捕获时 bf_find_slot 找不到 */
                cap_add(cn);
            }
        }
    }
    for(int i = 0; i < L->cnt; i++) free(L->names[i]);
    free(L->names);
    L->names = NULL; L->cnt = 0; L->cap = 0;
    return n;
}

static int is_global_var(const char* n) {
    if(strcmp(n, "logging") == 0) return 1;  // 预定义全局对象：logging.debug/info/warn/error/fatal
    for(int i = 0; i < g_global_vars_cnt; i++)
        if(strcmp(g_global_vars[i], n) == 0) return 1;
    return 0;
}
static int is_lambda_local(const char* n) {
    for(int i = 0; i < lambda_locals_cnt; i++)
        if(strcmp(lambda_locals[i], n) == 0) return 1;
    return 0;
}
static int is_lambda_param(const char* n) {
    for(AstNode* p = g_lambda_params; p; p = p->u.param.next)
        if(strcmp(p->u.param.name, n) == 0) return 1;
    return 0;
}

// ---------------- 阶段1：顶层收集 ----------------
// 遍历顶层（不深入函数体）：登记全部函数名 + 顶层赋值变量，
// 使函数前向引用、函数体读取全局变量在阶段2都能通过。

/* 重载签名表：记录顶层函数“名字#普通形参数#形参类型序列”，签名完全相同才算重复定义；
 * 同名不同签名视为重载，允许。 */
static char** g_func_sigs = NULL;
static int g_func_sig_n = 0, g_func_sig_cap = 0;

static char* make_func_sig(AstNode* node) {
    const char* fname = node->u.func_def.name;
    int ncnt = 0;
    for(AstNode* p = node->u.func_def.params; p; p = p->u.param.next)
        if(!p->u.param.is_ellipsis) ncnt++;
    size_t cap = strlen(fname) + 32;
    for(AstNode* p = node->u.func_def.params; p; p = p->u.param.next)
        cap += p->u.param.constraint ? strlen(p->u.param.constraint) + 2 : 3;
    char* s = (char*)malloc(cap);
    size_t w = 0;
    w += (size_t)snprintf(s + w, cap - w, "%s#%d#", fname, ncnt);
    for(AstNode* p = node->u.func_def.params; p; p = p->u.param.next) {
        if(p->u.param.is_ellipsis) continue;
        const char* c = p->u.param.constraint;
        w += (size_t)snprintf(s + w, cap - w, "%s,", c ? c : (p->u.param.is_nullable ? "n" : "d"));
    }
    return s;
}

static void collect_top_level(AstNode* node) {
    if (!node) return;
    switch (node->type) {
        case AST_ASSIGN:
            static_sym_put(node->u.assign.varname, VAL_NONE);
            collect_top_level(node->u.assign.expr);
            if(g_global_vars_cnt >= g_global_vars_cap) {
                int nc = g_global_vars_cap > 0 ? g_global_vars_cap * 2 : 64;
                char** nt = (char**)realloc(g_global_vars, (size_t)nc * sizeof(char*));
                if(!nt) { LOG_ERROR("全局变量表扩容内存不足\n"); exit(EXIT_FAILURE); }
                g_global_vars = nt; g_global_vars_cap = nc;
            }
            g_global_vars[g_global_vars_cnt++] = strdup(node->u.assign.varname);
            break;
        case AST_FUNC_DEF: {
            // 重复定义判定：仅当签名（形参个数 + 形参声明类型）完全相同才报错；
            // 同名不同签名为函数重载，允许。lambda/arrow 与 class 方法不走此表。
            if(strncmp(node->u.func_def.name, "_lambda_", 8) != 0 &&
               strncmp(node->u.func_def.name, "_arrow_", 7) != 0 &&
               !node->u.func_def.is_class_method) {
                char* sig = make_func_sig(node);
                int dup = 0;
                for(int i = 0; i < g_func_sig_n; i++)
                    if(strcmp(g_func_sigs[i], sig) == 0) { dup = 1; break; }
                if(dup) {
                    LOG_ERROR("语义错误(第%d行)：函数 \"%s\" 签名重复定义\n",
                            node->line, node->u.func_def.name);
                    g_collect_err = 1;
                } else {
                    if(g_func_sig_n >= g_func_sig_cap) {
                        g_func_sig_cap = g_func_sig_cap ? g_func_sig_cap * 2 : 32;
                        g_func_sigs = (char**)realloc(g_func_sigs,
                                         sizeof(char*) * (size_t)g_func_sig_cap);
                    }
                    g_func_sigs[g_func_sig_n++] = sig;
                }
            }
            // 登记函数名（支持前向引用）；不深入函数体；class 方法也登记（用于方法调用查找）
            if(!node->u.func_def.is_class_method) {
                static_sym_put(node->u.func_def.name, VAL_FUNC);
            }
            /* const func：登记到 const func 表（供常量折叠查找其 AST） */
            if(node->u.func_def.is_const) cf_register(node->u.func_def.name, node);
            break;
        }
        case AST_EXTERN_FUNC: {
            // FFI 外部函数声明：登记函数名（支持前向引用）
            static_sym_put(node->u.extern_func.name, VAL_FUNC);
            break;
        }
        case AST_BINOP:
            collect_top_level(node->u.bin.left);
            collect_top_level(node->u.bin.right);
            break;
        case AST_UNARY:
            collect_top_level(node->u.uny.child);
            break;
        case AST_CAST:
            collect_top_level(node->u.cast.child);
            break;
        case AST_INDEX:
            collect_top_level(node->u.index.arr);
            collect_top_level(node->u.index.idx);
            break;
        case AST_INDEX_ASSIGN:
            collect_top_level(node->u.index_assign.arr);
            collect_top_level(node->u.index_assign.idx);
            collect_top_level(node->u.index_assign.value);
            break;
        case AST_ARRAY_LIT:
            collect_top_level(node->u.array_lit.elems);
            break;
        case AST_MAP_LIT:
            collect_top_level(node->u.map_lit.entries);
            break;
        case AST_COMP_LIST:
            collect_top_level(node->u.comp.expr);
            collect_top_level(node->u.comp.var);
            collect_top_level(node->u.comp.iter);
            if(node->u.comp.cond) collect_top_level(node->u.comp.cond);
            break;
        case AST_COMP_MAP:
            collect_top_level(node->u.comp.expr);
            collect_top_level(node->u.comp.value);
            collect_top_level(node->u.comp.var);
            collect_top_level(node->u.comp.iter);
            if(node->u.comp.cond) collect_top_level(node->u.comp.cond);
            break;
        case AST_TRY:
            collect_top_level(node->u.trynode.body);
            collect_top_level(node->u.trynode.catch_body);
            collect_top_level(node->u.trynode.finally_body);
            break;
        case AST_THROW:
            collect_top_level(node->u.thrownode.expr);
            break;
        case AST_DESTRUCT:
            for(int i = 0; i < node->u.destruct.count; i++) {
                static_sym_put(node->u.destruct.names[i], VAL_NONE);
                if(g_global_vars_cnt >= g_global_vars_cap) {
                    int nc = g_global_vars_cap > 0 ? g_global_vars_cap * 2 : 64;
                    char** nt = (char**)realloc(g_global_vars, (size_t)nc * sizeof(char*));
                    if(!nt) { LOG_ERROR("全局变量表扩容内存不足\n"); exit(EXIT_FAILURE); }
                    g_global_vars = nt; g_global_vars_cap = nc;
                }
                g_global_vars[g_global_vars_cnt++] = strdup(node->u.destruct.names[i]);
            }
            collect_top_level(node->u.destruct.rhs);
            break;
        case AST_SPREAD:
            collect_top_level(node->u.spread.expr);
            break;
        case AST_MAP_ENTRY:
            collect_top_level(node->u.map_entry.key);
            collect_top_level(node->u.map_entry.value);
            break;
        case AST_TERNARY:
            collect_top_level(node->u.ternary.cond);
            collect_top_level(node->u.ternary.true_expr);
            collect_top_level(node->u.ternary.false_expr);
            break;
        case AST_PRINT:
            collect_top_level(node->u.print.args);
            break;
        case AST_SEQ: {
            /* 顶层语句经左结合 SEQ 链承载，N 万语句时递归 N 层会栈溢出。
               沿链迭代下沉，只对叶子语句调用 collect（叶子内部递归深度有限，
               不再随顶层语句数增长）。 */
            AstNode* p = node;
            while(p && p->type == AST_SEQ) {
                collect_top_level(p->u.seq.second);
                p = p->u.seq.first;
            }
            if(p) collect_top_level(p);
            break;
        }
        case AST_BLOCK:
            collect_top_level(node->u.block.stmts);
            break;
        case AST_IF:
            collect_top_level(node->u.ifnode.cond);
            collect_top_level(node->u.ifnode.then_stmt);
            collect_top_level(node->u.ifnode.elif_chain);
            collect_top_level(node->u.ifnode.else_stmt);
            break;
        case AST_IF_CHAIN:
            collect_top_level(node->u.if_chain.cond);
            collect_top_level(node->u.if_chain.if_body);
            {
                AstNode* p = node->u.if_chain.elif_list;
                while (p) {
                    collect_top_level(p->u.elif.cond);
                    collect_top_level(p->u.elif.body);
                    p = p->u.elif.next;
                }
            }
            collect_top_level(node->u.if_chain.else_body);
            break;
        case AST_ELIF:
            collect_top_level(node->u.elif.cond);
            collect_top_level(node->u.elif.body);
            break;
        case AST_FOR:
            collect_top_level(node->u.for_node.init);
            collect_top_level(node->u.for_node.cond);
            collect_top_level(node->u.for_node.update);
            collect_top_level(node->u.for_node.body);
            break;
        case AST_WHILE:
            collect_top_level(node->u.while_node.cond);
            collect_top_level(node->u.while_node.body);
            break;
        case AST_DO_WHILE:
            collect_top_level(node->u.while_node.body);
            collect_top_level(node->u.while_node.cond);
            break;
        case AST_SWITCH:
            collect_top_level(node->u.sw.cond);
            {
                AstNode* cp = node->u.sw.cases;
                while (cp) {
                    if (cp->u.cs.const_val) collect_top_level(cp->u.cs.const_val);
                    collect_top_level(cp->u.cs.body);
                    cp = cp->u.cs.next;
                }
            }
            break;
        case AST_CASE:
            if (node->u.cs.const_val) collect_top_level(node->u.cs.const_val);
            collect_top_level(node->u.cs.body);
            break;
        case AST_RETURN:
            collect_top_level(node->u.ret.ret_val);
            break;
        case AST_CALL: {
            AstNode* a = node->u.call.args;
            if (a) {
                if (a->type != AST_SEQ) collect_top_level(a);
                else {
                    collect_top_level(a->u.seq.first);
                    collect_top_level(a->u.seq.second);
                }
            }
            break;
        }
        default:
            break;
    }
}

// 实参链表是左嵌套 AST_SEQ 链，递归展开逐个检查
static int typecheck_arg_count(AstNode* chain)
{
    if(!chain) return 0;
    if(chain->type == AST_SEQ) return typecheck_arg_count(chain->u.seq.first) + typecheck_arg_count(chain->u.seq.second);
    return 1;
}
/* 实参树中是否含命名实参（AST_ASSIGN：f(x=expr)）。
 * 含命名时默认值按“位置尾部补齐”会错位，跳过该填充，改由 compile_user_call 按槽处理。 */
static int args_have_named(AstNode* chain)
{
    if(!chain) return 0;
    if(chain->type == AST_SEQ)
        return args_have_named(chain->u.seq.first) || args_have_named(chain->u.seq.second);
    return chain->type == AST_ASSIGN;
}
static int typecheck_call_args(AstNode* args) {
    if (!args) return 0;
    if (args->type != AST_SEQ) {
        return typecheck_expr(args);
    }
    return typecheck_call_args(args->u.seq.first) | typecheck_call_args(args->u.seq.second);
}

/* struct/class 方法不在程序 AST 树中（方法节点存于 TypeDef），遍历所有类型，
 * 对每个方法运行 typecheck 以分析其体内嵌套 lambda/arrow 的捕获；
 * 嵌套函数进入 g_recompile，方法本身进入方法重编译表。 */
static void method_typecheck_cb(const char* name, TypeDef* td, void* user_data) {
    int* acc = (int*)user_data;
    if(!td || !(td->is_struct || td->is_class)) return;
    /* 构造函数单独存于 td->constructor，同样需要 typecheck（否则其内访问控制/const 漏检） */
    if(td->constructor && td->constructor->type == AST_FUNC_DEF) {
        g_method_owner = name;
        *acc |= typecheck_expr(td->constructor);
        g_method_owner = NULL;
    }
    for(int i = 0; i < td->nmethods; i++) {
        AstNode* m = td->method_nodes[i];
        if(!m || m->type != AST_FUNC_DEF) continue;
        g_method_owner = name;
        *acc |= typecheck_expr(m);
        g_method_owner = NULL;
    }
}

static int typecheck_type_methods(void) {
    int acc = 0;
    type_foreach(method_typecheck_cb, &acc);
    return acc;
}

int ast_typecheck(AstNode* node)
{
    static_sym_reset();
    vo_reset();
    func_depth = 0;
    g_collect_err = 0;
    if(!node) return 0;
    // 阶段1：顶层收集（函数名 + 全局变量），支持前向引用/函数体读全局
    collect_top_level(node);
    // 阶段2：全面检查（含函数体递归）
    int err = g_collect_err | typecheck_expr(node);
    // 阶段2b：struct/class 方法补 typecheck（分析方法内嵌套 lambda/arrow 捕获）
    if(!err) err |= typecheck_type_methods();
    // 阶段3：重编译。先处理普通函数/嵌套 lambda，再处理方法
    // （方法编译时其体内嵌套 lambda 须已是带捕获的最新版本）
    if(!err) {
        for(int i = 0; i < g_recompile_cnt; i++)
            func_compile_recompile(g_recompile[i]);
        for(int i = 0; i < g_method_recomp_cnt; i++)
            func_compile_recompile_method(g_method_recomp[i].owner,
                                          g_method_recomp[i].node);
    }
    return err;
}

// 实参个数：AST_SEQ 二叉链递归计数
static int count_args(AstNode* args)
{
    if(!args) return 0;
    if(args->type != AST_SEQ) return 1;
    return count_args(args->u.seq.first) + count_args(args->u.seq.second);
}


/*
 * 类型检查：函数调用
 * 返回错误标志（0=无错误，1=有错误）
 */
static int typecheck_call(AstNode* node)
{
    int err = 0;
    /* 静态成员方法（Class_method 全局名）访问控制：
     * class_static_member_lookup 命中且为 private/protected 时按当前上下文检查 */
    {
        const char* sm_owner = NULL;
        int sm_access = 0;
        if(class_static_member_lookup(node->u.call.name, &sm_owner, &sm_access) &&
           !tc_access_ok(sm_owner, sm_access)) {
            LOG_ERROR("语义错误(第%d行)：静态方法 '%s' 为 %s，当前上下文不可调用\n",
                      node->line, node->u.call.name,
                      sm_access == ACCESS_PRIVATE ? "private" : "protected");
            err = 1;
        }
    }
            // 默认参数填充：如果实参不足，用函数定义中的默认值表达式填充。
            // 含命名实参时跳过（位置补齐会错位），由 compile_user_call 按形参槽取默认值。
            {
                if(sym_has(node->u.call.name) && !args_have_named(node->u.call.args)) {
                Value fv = sym_get(node->u.call.name);
                if(fv.type == VAL_FUNC) {
                    RuntimeFunc* rf = (RuntimeFunc*)fv.v.func.func_obj;
                    if(interp_func_is_payload(rf)) {
                        int nargs = typecheck_arg_count(node->u.call.args);
                        int pcount = interp_func_param_cnt(rf);
                        // 从缺失的第一个参数开始，逐个填充默认值
                        for(int pi = nargs; pi < pcount; pi++) {
                            if(interp_func_param_has_default(rf, pi)) {
                                AstNode* dv = interp_func_param_default(rf, pi);
                                if(dv) {
                                    // 深拷贝默认值表达式，避免与函数定义共享节点导致重复释放
                                    AstNode* dv_copy = ast_clone_node(dv);
                                    node->u.call.args = ast_arg_append(node->u.call.args, dv_copy);
                                }
                            }
                        }
                    }
                }
                }
            }
            // 实参逐个检查（含嵌套调用）
            err |= typecheck_call_args(node->u.call.args);
            // 函数名：已定义函数 或 赋过函数值的变量 均可（与解释器一致）；
            // 内置函数白名单：len/type/input/range/substr（用户函数同名时用户优先）
            ValueType t;
            if(type_lookup(node->u.call.name) != NULL) {
                /* type 构造调用：参数个数 == 属性数（或单 map 原样）。
                   函数体先于 typecheck 被 ir 编译（yacc 动作 compile_func_from_ast），
                   构造展开会摘空 args——args 为空时跳过（已展开，运行时宽松处理） */
                int nargs = typecheck_arg_count(node->u.call.args);
                if(nargs > 0) {
                    int nprops = type_lookup(node->u.call.name)->nprops;
                    if(!(nargs == nprops || nargs == 1)) {
                        LOG_ERROR("语义错误(第%d行)：类型构造参数个数错误：需要 %d 个（或单个 map），实际 %d 个\n",
                                node->line, nprops, nargs);
                    }
                }
            } else if(!static_sym_get(node->u.call.name, &t)) {
                static const struct { const char* name; int min; int max; } builtins[] = {
                    {"len", 1, 1}, {"type", 1, 1}, {"input", 0, 0}, {"range", 1, 3}, {"substr", 3, 3},
                    {"toupper", 1, 1}, {"tolower", 1, 1}, {"split", 2, 2}, {"del", 2, 2}, {"insert", 3, 3},
                    {"floor", 1, 1}, {"ceil", 1, 1}, {"abs", 1, 1}, {"sqrt", 1, 1},
                    {"sin", 1, 1}, {"cos", 1, 1}, {"tan", 1, 1},
                    {"asin", 1, 1}, {"acos", 1, 1}, {"atan", 1, 1}, {"atan2", 2, 2},
                    {"log", 1, 1}, {"log10", 1, 1}, {"log2", 1, 1},
                    {"exp", 1, 1}, {"pow", 2, 2},
                    {"round", 1, 1}, {"cbrt", 1, 1}, {"hypot", 2, 2},
                    {"sign", 1, 1}, {"degrees", 1, 1}, {"radians", 1, 1}, {"trunc", 1, 1},
                    {"random", 0, 0},
                    {"max", 1, -1}, {"min", 1, -1}, {"join", 2, 2}, {"contains", 2, 2},
                    {"repeat", 2, 2}, {"replace", 3, 3}, {"sum", 1, 1}, {"avg", 1, 1},
                    {"format", 1, -1}, {"sort", 1, 1}, {"reverse", 1, 1},
                    {"map", 2, 2}, {"filter", 2, 2}, {"reduce", 3, 3},
                    {"strip", 1, 1}, {"startswith", 2, 2}, {"endswith", 2, 2},
                    {"readFile", 1, 1}, {"writeFile", 2, 2}, {"fileExists", 1, 1},
                    {"keys", 1, 1}, {"values", 1, 1},
                    {"thread", 1, -1}, {"thread_join", 1, 1},
                    {"mutex", 0, 0}, {"rmutex", 0, 0}, {"rwlock", 0, 0}, {"spinlock", 0, 0},
                    {"lock", 1, 1}, {"unlock", 1, 1}, {"trylock", 1, 1},
                    {"rdlock", 1, 1}, {"wrlock", 1, 1},
                    {"tryrdlock", 1, 1}, {"trywrlock", 1, 1},
                    {"condvar", 0, 0}, {"cond_wait", 2, 2}, {"cond_wait_timeout", 3, 3}, {"cond_signal", 1, 1}, {"cond_broadcast", 1, 1},
                    {"threadlocal_get", 1, 1}, {"threadlocal_set", 2, 2},
                    {"get", 1, 3}, {"post", 1, 3}, {"put", 1, 3}, {"delete", 1, 3}, {"head", 1, 3}, {"patch", 1, 3},
                    {"http_get", 1, 2}, {"http_post", 1, 2}, {"http_put", 1, 2},
                    {"http_delete", 1, 2}, {"http_head", 1, 2}, {"http_patch", 1, 2},

                    {"add", 2, 3}, {"remove", 2, 2}, {"clear", 1, 1},
                    {"arr_get", 2, 2}, {"indexOf", 2, 2}, {"set", 0, 32}, {"first", 1, 1}, {"last", 1, 1}, {"has", 0, 32},
                    {"flat", 1, 2}, {"qs", 1, 2}, {"addAll", 2, 2}, {"bytes", 1, 32}, {"str", 1, 2},
                    {"json", 1, 2}, {"stringify", 1, 2},
                    {"encode", 1, 2}, {"decode", 1, 2},
                    {"encodeURL", 1, 1}, {"decodeURL", 1, 1},
                    {"md5", 1, 1}, {"encodeBase64", 1, 1}, {"decodeBase64", 1, 1},
                    {"regexMatch", 2, 2}, {"regexSearch", 2, 2}, {"regexReplace", 3, 3},
                    {"now", 0, 0}, {"timestamp", 0, 0}, {"timestamp_ms", 0, 0},
                    {"sleep", 1, 1}, {"date", 1, 4}, {"time", 1, 6}, {"datetime", 1, 8}, {"timedelta", 1, 2},
                    {"today", 0, 0},
                    {"year", 1, 1}, {"month", 1, 1}, {"day", 1, 1}, {"hour", 1, 1},
                    {"minute", 1, 1}, {"second", 1, 1}, {"weekday", 1, 1}, {"yearday", 1, 1},
                    {"days", 1, 1}, {"seconds", 1, 1}, {"totalSeconds", 1, 1},
                    {"formatDate", 2, 2}, {"diff", 2, 2},
                    {"tuple", 0, 32}, {"complex", 1, 2},
                    {"calendar", 1, 3},
                    {"firstDate", 1, 1}, {"lastDate", 1, 1},
                    {"file", 1, 2}, {"folder", 1, 1},
                    /* socket 构造：大写为主用名（可收可选 config map），小写为别名 */
                    {"TcpSocket", 0, 1}, {"tcpSocket", 0, 1},
                    {"UdpSocket", 0, 1}, {"udpSocket", 0, 1},
                    {"UnixSocket", 0, 1}, {"unixSocket", 0, 1},
                    {"UnixDgramSocket", 0, 1}, {"unixDgramSocket", 0, 1},
                    {"readAll", 1, 1}, {"readLines", 1, 3}, {"readLine", 2, 2},
                    {"writeAll", 2, 2}, {"writeLine", 3, 3}, {"insertLine", 3, 3}, {"writeLines", 2, 2},
                    {"append", 2, 2}, {"appendLine", 2, 2},
                    {"flush", 1, 1}, {"delete", 1, 1},
                    {"readBytes", 1, 1}, {"writeBytes", 2, 2},
                    {"truncate", 2, 2}, {"renameTo", 2, 2},
                    {"list", 1, 1}, {"files", 1, 1}, {"dirs", 1, 1},
                    {"create", 1, 1}, {"remove", 1, 1}, {"walk", 1, 1},
                    {"copyTo", 2, 2}, {"moveTo", 2, 2}, {"glob", 2, 2},
                    {"fromHex", 1, 1}, {"conjugate", 1, 1}, {"union", 2, 2}, {"intersect", 2, 2}, {"hex", 1, 1}, {"toStr", 1, 1},
                    {"format_time", 1, 2},
                    {"debug", 1, 2}, {"info", 1, 2}, {"warn", 1, 2}, {"error", 1, 2}, {"fatal", 1, 2},
                    {"gc_count", 0, 0}, {"gc_bytes", 0, 0}, {"gc_collect", 0, 0}, {"gc_stw_ns", 0, 0}, {"next", 1, 1}, {"send", 2, 2}, {"receive", 0, 0}, {"close", 1, 1}, {"GenThrow", 2, 2}, {"chain", 2, 2}, {"zip", 2, 2}, {"skip", 2, 2}, {"take", 2, 2}, {"enumerate", 1, 1}, {"next", 1, 1},
                };
                int found = 0;
                int nbuiltins = (int)(sizeof(builtins) / sizeof(builtins[0]));
                for(int k = 0; k < nbuiltins; k++) {
                    if(strcmp(node->u.call.name, builtins[k].name) == 0) {
                        found = 1;
                        int nargs = count_args(node->u.call.args);
                        int bad = (nargs < builtins[k].min) ||
                                  (builtins[k].max >= 0 && nargs > builtins[k].max);
                        if(bad) {
                            if(builtins[k].max >= 0 && builtins[k].min == builtins[k].max)
                                LOG_ERROR("语义错误(第%d行)：%s() 需要 %d 个实参（给了 %d 个）\n", node->line,
                                        builtins[k].name, builtins[k].min, nargs);
                            else if(builtins[k].max < 0)
                                LOG_ERROR("语义错误(第%d行)：%s() 需要至少 %d 个实参（给了 %d 个）\n", node->line,
                                        builtins[k].name, builtins[k].min, nargs);
                            else
                                LOG_ERROR("语义错误(第%d行)：%s() 需要 %d 到 %d 个实参（给了 %d 个）\n", node->line,
                                        builtins[k].name, builtins[k].min, builtins[k].max, nargs);
                            err = 1;
                        }
                        break;
                    }
                }
                if(!found) {
                    /* 方法调用（如 a.speak() 转换为 speak(a)）可能是 class 方法，
                       不在全局符号表中，运行时根据对象类型动态查找，所以编译期不报错 */
                    int nargs = typecheck_arg_count(node->u.call.args);
                    if(nargs == 0) {
                        /* 无参数的函数调用，不可能是方法调用，报错
                           但是静态方法的函数名是 <类名>_<方法名>，包含 _，跳过检查 */
                        if(strchr(node->u.call.name, '_') == NULL) {
                            LOG_ERROR("语义错误(第%d行)：调用未定义函数 %s\n", node->line, node->u.call.name);
                            err = 1;
                        }
                    }
                    /* 有参数的函数调用，可能是方法调用，不报错，运行时查找 */
                }
            } else if(t != VAL_NONE && t != VAL_FUNC) {
                LOG_ERROR("语义错误(第%d行)：%s 不是函数\n", node->line, node->u.call.name);
                err = 1;
            }
            node->val_type = VAL_NONE;
    return err;
}

int typecheck_expr(AstNode* node)
{
    if(!node) return 0;
    int err = 0;
    switch(node->type) {
        case AST_INT:
            node->val_type = VAL_INT;
            break;
        case AST_NUM:
            node->val_type = VAL_DOUBLE;
            break;
        case AST_BOOL:
            node->val_type = VAL_BOOL;
            break;
        case AST_NONE:
            node->val_type = VAL_NONE;
            break;
        case AST_STRING:
            node->val_type = VAL_STRING;
            break;
        case AST_CHAR:
            node->val_type = VAL_CHAR;
            break;
        case AST_VAR:{
            if(in_lambda && !is_lambda_param(node->u.varname) &&
               !is_lambda_local(node->u.varname)) {
                // 引用外层作用域变量 → 记录为当前 lambda 的捕获变量（运行时装箱）
                // 注：全局变量也需捕获——VM 通道无独立的全局加载机制，
                // 不捕获则 c_find_var 返回 -1 → 误 emit LOAD_VAR slot0（读到形参），
                // 导致 lambda 内读全局变量不可靠。捕获后经 cell 别名绑到 lambda 槽位。
                cap_add(node->u.varname);
            }
            ValueType t;
            if(static_sym_get(node->u.varname, &t)) {
                /* 静态成员（Class_prop / Class_method 全局名）访问控制：
                 * 引用命中 class_static_member 表且为 private/protected 时检查上下文 */
                {
                    const char* sm_owner = NULL;
                    int sm_access = 0;
                    if(class_static_member_lookup(node->u.varname, &sm_owner, &sm_access) &&
                       !tc_access_ok(sm_owner, sm_access)) {
                        LOG_ERROR("语义错误(第%d行)：静态成员 '%s' 为 %s，当前上下文不可访问\n",
                                  node->line, node->u.varname,
                                  sm_access == ACCESS_PRIVATE ? "private" : "protected");
                        err = 1;
                    }
                }
                if(t == VAL_FUNC) {
                    // 函数名引用：就地转 AST_FUNCREF（函数作为值）
                    node->type = AST_FUNCREF;
                    node->val_type = VAL_FUNC;
                    // 若发生在函数体内，该函数的 parse 期字节码需重编译
                    if(g_cur_func_def) {
                        int dup = 0;
                        for(int i = 0; i < g_recompile_cnt; i++)
                            if(g_recompile[i] == g_cur_func_def) { dup = 1; break; }
                        if(!dup) {
                            if(g_recompile_cnt >= g_recompile_cap) {
                                int nc = g_recompile_cap > 0 ? g_recompile_cap * 2 : 16;
                                AstNode** nt = (AstNode**)realloc(g_recompile, (size_t)nc * sizeof(AstNode*));
                                if(!nt) { LOG_ERROR("重编译表扩容内存不足\n"); exit(EXIT_FAILURE); }
                                g_recompile = nt; g_recompile_cap = nc;
                            }
                            g_recompile[g_recompile_cnt++] = g_cur_func_def;
                        }
                    }
                } else {
                    node->val_type = t;
                }
            } else if(strcmp(node->u.varname, "super") == 0 && g_method_owner) {
                /* super：class 子类方法中的父类接收者（运行时 ir_compile 对 super 走
                 * LOAD_FIELD 快路径）。仅当当前类型确有父类时合法；val_type=NONE
                 * 使 super.x 的下标类型校验通过（AST_INDEX 白名单含 NONE）。 */
                TypeDef* sup_td = struct_lookup(g_method_owner);
                if(!sup_td) sup_td = class_lookup(g_method_owner);
                if(sup_td && sup_td->parent) {
                    node->val_type = VAL_NONE;
                } else {
                    LOG_ERROR("语义错误(第%d行)：使用未定义变量 %s\n", node->line, node->u.varname);
                    node->val_type = VAL_NONE;
                    err = 1;
                }
            } else {
                LOG_ERROR("语义错误(第%d行)：使用未定义变量 %s\n", node->line, node->u.varname);
                node->val_type = VAL_NONE;
                err = 1;
            }
            break;
        }
        case AST_FUNCREF:
            node->val_type = VAL_FUNC;
            break;
        case AST_UNARY: {
            AstNode* kid = node->u.uny.child;
            BinOp op = node->u.uny.op;
            err |= typecheck_expr(kid);
            if(op == OP_PRE_INC || op == OP_POST_INC || op == OP_PRE_DEC || op == OP_POST_DEC) {
                if(kid->type != AST_VAR) {
                    LOG_ERROR("语义错误：++/-- 的操作数必须是变量\n");
                    return -1;
                }
            }
            node->val_type = kid->val_type;
            break;
        }
        case AST_BINOP:{
            err |= typecheck_expr(node->u.bin.left);
            err |= typecheck_expr(node->u.bin.right);
            ValueType tl = node->u.bin.left->val_type;
            ValueType tr = node->u.bin.right->val_type;
            int left_unknown = (tl == VAL_NONE);
            int right_unknown = (tr == VAL_NONE);
            int left_is_str = (tl == VAL_STRING);
            int right_is_str = (tr == VAL_STRING);
            int left_is_num = (tl == VAL_INT || tl == VAL_DOUBLE || tl == VAL_CHAR || tl == VAL_BYTE || tl == VAL_BOOL);
            int right_is_num = (tr == VAL_INT || tr == VAL_DOUBLE || tr == VAL_CHAR || tr == VAL_BYTE || tr == VAL_BOOL);
            BinOp op = node->u.bin.op;
            if(op == OP_ADD) {
                if(left_unknown || right_unknown) {
                    node->val_type = VAL_NONE;
                } else if(left_is_str || right_is_str) {
                    /* 与运行时一致：bool 走 INT64 数值算术（非字符串拼接） */
                    node->val_type = VAL_STRING;
                } else if(left_is_num && right_is_num) {
                    if(tl == VAL_DOUBLE || tr == VAL_DOUBLE)
                        node->val_type = VAL_DOUBLE;
                    else
                        node->val_type = VAL_INT;
                } else if(tl == VAL_ARRAY && tr == VAL_ARRAY) {
                    /* 数组拼接：[a] + [b] → 新数组 */
                    node->val_type = VAL_ARRAY;
                } else {
                    LOG_ERROR("语义错误：不支持 %s + %s\n", valtype_to_cstr(tl), valtype_to_cstr(tr));
                    err = 1;
                }
            } else if(op == OP_EQ || op == OP_NE) {
                if(left_unknown || right_unknown) {
                    node->val_type = VAL_BOOL;
                } else {
                    int ok = 0;
                    if(tl == tr) { ok = 1; }
                    /* 数值族（int/char/byte/bool/double）跨类型比较：运行时经
                       INT64_EQ/DOUBLE_EQ 支持（混合时编译器路由到 DOUBLE 统一比较） */
                    if(left_is_num && right_is_num) { ok = 1; }
                    if((tl == VAL_STRING && tr != VAL_STRING) || (tl != VAL_STRING && tr == VAL_STRING)) {
                        /* 字符串与非字符串比较交给运行时动态判定 */
                        ok = 1;
                    }
                    if(!ok) {
                        LOG_ERROR("语义错误：==/!= 两侧类型不一致 %s vs %s\n", valtype_to_cstr(tl), valtype_to_cstr(tr));
                        err = 1;
                    }
                }
                node->val_type = VAL_BOOL;
            } else if(op == OP_LOGIC_AND || op == OP_LOGIC_OR) {
                // 逻辑运算：任意类型按 truthy 判定，结果 bool
                node->val_type = VAL_BOOL;
            } else if(op == OP_IMPLEMENTS) {
                // implements 操作符：检查对象是否实现接口，结果 bool
                node->val_type = VAL_BOOL;
            } else if(op == OP_MOD) {
                if(!(left_unknown || right_unknown)) {
                    if(!(left_is_num && right_is_num)) {
                        LOG_ERROR("语义错误：%% 只支持数值类型\n");
                        err = 1;
                    }
                }
                if(tl == VAL_DOUBLE || tr == VAL_DOUBLE)
                    node->val_type = VAL_DOUBLE;
                else
                    node->val_type = VAL_INT;
            } else {
                /* - * /：既支持数值运算，也支持字符串运算
                   （"ab"*3 重复、"abcabc"/3 前缀截取、"abcabc"-3 尾部截取，
                    由运行时 PTR_MUL/DIV/SUB 实现）。 */
                if(left_unknown || right_unknown) {
                    /* 含未解析类型（如 bigint/decimal 标注变量），无法判定 */
                    node->val_type = VAL_NONE;
                } else if(left_is_str || right_is_str) {
                    node->val_type = VAL_STRING;
                } else if(left_is_num && right_is_num) {
                    if(tl == VAL_DOUBLE || tr == VAL_DOUBLE)
                        node->val_type = VAL_DOUBLE;
                    else
                        node->val_type = VAL_INT;
                } else {
                    LOG_ERROR("语义错误：运算符只支持数值或字符串类型\n");
                    err = 1;
                }
            }
            break;
        }
        case AST_ASSIGN: {
            const char* assign_vn = node->u.assign.varname;
            int is_const_decl = node->u.assign.is_const;
            /* const 重复赋值拦截（put 之前查）：
             * 顶层无遮蔽直接拦截；函数/lambda 内仅当本层已登记过该名字才算重复
             * （本层首次出现是对外部常量的合法遮蔽，put 会保存并清除外层标记）。 */
            if(!is_const_decl && static_sym_is_const(assign_vn) &&
               (!static_sym_scope_active() || static_sym_level_knows(assign_vn))) {
                LOG_ERROR("语义错误(第%d行)：不能给常量 '%s' 重新赋值\n", node->line, assign_vn);
                err = 1;
            }
            /* const 声明的编译期折叠：初始化表达式若可静态求值（含 const func
             * 调用），就地改写为结果字面量，并记录供后续 const 引用；
             * 不能静态求值（含运行时调用）则保持原样，仍受不可重新赋值约束。 */
            if(is_const_decl) {
                CEVal cev;
                if(ce_eval(node->u.assign.expr, &cev)) {
                    AstNode* lit = ce_make_literal(cev);
                    if(lit) {
                        lit->line = node->line;
                        node->u.assign.expr = lit;
                        cv_put(assign_vn, cev);
                    }
                }
            }
            err |= typecheck_expr(node->u.assign.expr);
            node->val_type = node->u.assign.expr->val_type;
            int assign_capture = 0;
            if(in_lambda) {
                const char* vn = node->u.assign.varname;
                ValueType st;
                /* 注意：必须在 static_sym_put 之前判断 in_static，否则本赋值刚 put 的名字
                 * 会被误判为外层变量。 */
                int in_static = static_sym_get(vn, &st);
                /* 判定：参数 / 已登记本层局部 / 全局 → 本 lambda 局部；
                 * 否则若名字已存在于外层作用域（外层函数局部）→ 捕获（引用语义）；
                 * 完全未出现 → 本 lambda 新局部。 */
                assign_capture = !is_lambda_param(vn) && !is_lambda_local(vn) &&
                                 !is_global_var(vn) && in_static;
            }
            /* 变量持有函数值时存 VAL_NONE（动态），避免后续引用被误判为函数名引用（AST_FUNCREF） */
            static_sym_put(node->u.assign.varname,
                           node->val_type == VAL_FUNC ? VAL_NONE : node->val_type);
            /* const 声明：put 登记后标记，使同作用域后续赋值被拦截 */
            if(is_const_decl) static_sym_set_const(assign_vn);
            /* 记录变量持有的自定义类型（供后续字段访问的访问控制推断） */
            const char* rown = tc_owner(node->u.assign.expr);
            if(rown) vo_set(assign_vn, rown);
            if(in_lambda) {
                const char* vn = node->u.assign.varname;
                if(!assign_capture) {
                    if(lambda_locals_cnt >= lambda_locals_cap) {
                        int nc = lambda_locals_cap > 0 ? lambda_locals_cap * 2 : 64;
                        char** nt = (char**)realloc(lambda_locals, (size_t)nc * sizeof(char*));
                        if(!nt) { LOG_ERROR("lambda 局部表扩容内存不足\n"); exit(EXIT_FAILURE); }
                        lambda_locals = nt; lambda_locals_cap = nc;
                    }
                    lambda_locals[lambda_locals_cnt++] = strdup(vn);
                } else {
                    cap_add(vn);
                }
            }
            break;
        }
        case AST_INDEX: {
            err |= typecheck_expr(node->u.index.arr);
            err |= typecheck_expr(node->u.index.idx);
            if(node->u.index.arr->val_type != VAL_ARRAY &&
               node->u.index.arr->val_type != VAL_STRING &&
               node->u.index.arr->val_type != VAL_MAP &&
               node->u.index.arr->val_type != VAL_FORMDATA &&
               node->u.index.arr->val_type != VAL_NONE) {
                LOG_ERROR("语义错误(第%d行)：下标访问的对象不是数组、字符串或字典\n", node->line);
                err = 1;
            }
            /* class 字段读的访问控制：idx 为字符串字面量且能推断 arr 所属类。
             * map/数组下标（idx 非字符串或 arr 无 owner）自然跳过。 */
            if(node->u.index.idx->type == AST_STRING) {
                const char* fow = tc_owner(node->u.index.arr);
                if(fow) {
                    FieldInfo* ffi = tc_find_field(fow, node->u.index.idx->u.sval);
                    if(ffi && !tc_access_ok(fow, ffi->access)) {
                        LOG_ERROR("语义错误(第%d行)：字段 '%s' 为 %s，当前上下文不可访问\n",
                                  node->line, node->u.index.idx->u.sval,
                                  ffi->access == ACCESS_PRIVATE ? "private" : "protected");
                        err = 1;
                    }
                }
            }
            node->val_type = VAL_NONE;   // 元素类型不可静态追踪
            break;
        }
        case AST_INDEX_ASSIGN: {
            err |= typecheck_expr(node->u.index_assign.arr);
            err |= typecheck_expr(node->u.index_assign.idx);
            err |= typecheck_expr(node->u.index_assign.value);
            /* 只读属性：__mapname__ / __structname__ 编译期拦截（DOT 属性赋值与 "[" 下标赋值） */
            if(node->u.index_assign.idx->type == AST_STRING &&
               (strcmp(node->u.index_assign.idx->u.sval, "__mapname__") == 0 ||
                strcmp(node->u.index_assign.idx->u.sval, "__structname__") == 0)) {
                LOG_ERROR("语义错误(第%d行)：只读属性 %s 不能赋值\n", node->line, node->u.index_assign.idx->u.sval);
                err = 1;
            }
            /* class 字段写：const 字段拦截 + private/protected 访问控制。
             * map/数组赋值（arr 无 owner）自然跳过。 */
            if(node->u.index_assign.idx->type == AST_STRING) {
                const char* mow = tc_owner(node->u.index_assign.arr);
                if(mow) {
                    FieldInfo* mfi = tc_find_field(mow, node->u.index_assign.idx->u.sval);
                    if(mfi) {
                        if(mfi->is_const && !g_in_ctor) {
                            LOG_ERROR("语义错误(第%d行)：不能给 const 字段 '%s' 赋值\n",
                                      node->line, node->u.index_assign.idx->u.sval);
                            err = 1;
                        }
                        if(!tc_access_ok(mow, mfi->access)) {
                            LOG_ERROR("语义错误(第%d行)：字段 '%s' 为 %s，当前上下文不可访问\n",
                                      node->line, node->u.index_assign.idx->u.sval,
                                      mfi->access == ACCESS_PRIVATE ? "private" : "protected");
                            err = 1;
                        }
                    }
                }
            }
            if(node->u.index_assign.arr->val_type != VAL_ARRAY &&
               node->u.index_assign.arr->val_type != VAL_MAP &&
               node->u.index_assign.arr->val_type != VAL_NONE) {
                LOG_ERROR("语义错误(第%d行)：下标访问的对象不是数组或字典\n", node->line);
                err = 1;
            }
            node->val_type = node->u.index_assign.value->val_type;
            break;
        }
        case AST_ARRAY_LIT:
            err |= typecheck_expr(node->u.array_lit.elems);
            node->val_type = VAL_ARRAY;
            break;
        case AST_MAP_LIT:
            err |= typecheck_expr(node->u.map_lit.entries);
            node->val_type = VAL_MAP;
            break;
        case AST_COMP_LIST:
            err |= typecheck_expr(node->u.comp.iter);
            if(node->u.comp.cond) err |= typecheck_expr(node->u.comp.cond);
            /* 注册循环变量后检查表达式 */
            if(node->u.comp.var && node->u.comp.var->type == AST_VAR)
                static_sym_put(node->u.comp.var->u.varname, VAL_NONE);
            err |= typecheck_expr(node->u.comp.expr);
            node->val_type = VAL_ARRAY;
            break;
        case AST_COMP_MAP:
            err |= typecheck_expr(node->u.comp.iter);
            if(node->u.comp.cond) err |= typecheck_expr(node->u.comp.cond);
            if(node->u.comp.var && node->u.comp.var->type == AST_VAR)
                static_sym_put(node->u.comp.var->u.varname, VAL_NONE);
            err |= typecheck_expr(node->u.comp.expr);
            err |= typecheck_expr(node->u.comp.value);
            node->val_type = VAL_MAP;
            break;
        case AST_MAP_ENTRY:
            err |= typecheck_expr(node->u.map_entry.key);
            err |= typecheck_expr(node->u.map_entry.value);
            node->val_type = VAL_MAP;
            break;
        case AST_TRY:
            err |= typecheck_expr(node->u.trynode.body);
            /* catch 变量先注册（作用域：catch 块内）再检查 catch 块 */
            if(node->u.trynode.catch_var) {
                static_sym_put(node->u.trynode.catch_var, VAL_NONE);
                err |= typecheck_expr(node->u.trynode.catch_body);
            }
            err |= typecheck_expr(node->u.trynode.finally_body);
            node->val_type = VAL_NONE;
            break;
        case AST_THROW:
            err |= typecheck_expr(node->u.thrownode.expr);
            node->val_type = VAL_NONE;
            break;
        case AST_DESTRUCT:
            err |= typecheck_expr(node->u.destruct.rhs);
            for(int i = 0; i < node->u.destruct.count; i++)
                static_sym_put(node->u.destruct.names[i], VAL_NONE);
            node->val_type = VAL_NONE;
            break;
        case AST_SPREAD:
            err |= typecheck_expr(node->u.spread.expr);
            node->val_type = VAL_NONE;
            break;
        case AST_PRINT:
            err |= typecheck_expr(node->u.print.args);
            node->val_type = VAL_NONE;
            break;
        case AST_SEQ: {
            /* 迭代遍历，避免长链表导致栈溢出 */
            /* AST_SEQ 可能是左偏树或右偏树，用栈模拟递归。
               栈动态扩容：固定容量会在长程序中静默丢弃 SEQ 节点，
               导致超出深度的语句被跳过语义检查。 */
            int stack_cap = 256;
            AstNode** stack = (AstNode**)malloc(sizeof(AstNode*) * stack_cap);
            if(!stack) { perror("typecheck AST_SEQ"); exit(EXIT_FAILURE); }
            int sp = 0;
            AstNode* cur = node;
            ValueType last_type = VAL_NONE;

            while(cur || sp > 0) {
                /* 向左遍历到底，把路径上的节点压栈 */
                while(cur && cur->type == AST_SEQ) {
                    if(sp >= stack_cap) {
                        stack_cap *= 2;
                        stack = (AstNode**)realloc(stack, sizeof(AstNode*) * stack_cap);
                        if(!stack) { perror("typecheck AST_SEQ realloc"); exit(EXIT_FAILURE); }
                    }
                    stack[sp++] = cur;
                    cur = cur->u.seq.first;
                }
                /* 处理叶子节点 */
                if(cur) {
                    err |= typecheck_expr(cur);
                    last_type = cur->val_type;
                }
                /* 弹出栈顶，处理 second */
                if(sp > 0) {
                    AstNode* n = stack[--sp];
                    cur = n->u.seq.second;
                } else {
                    cur = NULL;
                }
            }
            free(stack);
            node->val_type = last_type;
            break;
        }
        case AST_IF:{
            err |= typecheck_expr(node->u.ifnode.cond);
            err |= typecheck_expr(node->u.ifnode.then_stmt);
            if(node->u.ifnode.elif_chain) {
                err |= typecheck_expr(node->u.ifnode.elif_chain);
            }
            if(node->u.ifnode.else_stmt) {
                err |= typecheck_expr(node->u.ifnode.else_stmt);
            }
            node->val_type = VAL_DOUBLE;
            break;
        }
        case AST_BLOCK:
            if(node->u.block.stmts) err |= typecheck_expr(node->u.block.stmts);
            node->val_type = VAL_DOUBLE;
            break;
        case AST_DEFER:
            /* defer { body }：检查 body（与 AST_BLOCK 一致） */
            if(node->u.defer.body) err |= typecheck_expr(node->u.defer.body);
            node->val_type = VAL_NONE;
            break;
        case AST_IF_CHAIN:
            err |= typecheck_expr(node->u.if_chain.cond);
            err |= typecheck_expr(node->u.if_chain.if_body);
            {
                AstNode* p = node->u.if_chain.elif_list;
                while(p) {
                    err |= typecheck_expr(p->u.elif.cond);
                    err |= typecheck_expr(p->u.elif.body);
                    p = p->u.elif.next;
                }
            }
            if(node->u.if_chain.else_body) err |= typecheck_expr(node->u.if_chain.else_body);
            node->val_type = VAL_DOUBLE;
            break;
        case AST_ELIF:
            err |= typecheck_expr(node->u.elif.cond);
            err |= typecheck_expr(node->u.elif.body);
            node->val_type = VAL_DOUBLE;
            break;
        case AST_WHILE:
            err |= typecheck_expr(node->u.while_node.cond);
            err |= typecheck_expr(node->u.while_node.body);
            node->val_type = VAL_DOUBLE;
            break;
        case AST_DO_WHILE:
            err |= typecheck_expr(node->u.while_node.body);
            err |= typecheck_expr(node->u.while_node.cond);
            node->val_type = VAL_DOUBLE;
            break;
        case AST_FOR:
            if(node->u.for_node.init) err |= typecheck_expr(node->u.for_node.init);
            if(node->u.for_node.cond) err |= typecheck_expr(node->u.for_node.cond);
            if(node->u.for_node.update) err |= typecheck_expr(node->u.for_node.update);
            err |= typecheck_expr(node->u.for_node.body);
            node->val_type = VAL_DOUBLE;
            break;
        case AST_CAST: {
            err |= typecheck_expr(node->u.cast.child);
            /* 容器泛型：<T>[..] / (T)[..] / <string,V>{..} → 逐元素/逐值强转，仍是容器 */
            if(node->u.cast.child->val_type == VAL_ARRAY || node->u.cast.child->val_type == VAL_MAP) {
                node->val_type = node->u.cast.child->val_type;
                break;
            }
            switch(node->u.cast.cast_type) {
                case CAST_INT:      node->val_type = VAL_INT; break;
                case CAST_DOUBLE:   node->val_type = VAL_DOUBLE; break;
                case CAST_CHAR:     node->val_type = VAL_CHAR; break;
                case CAST_BOOL:     node->val_type = VAL_BOOL; break;
                case CAST_STRING:   node->val_type = VAL_STRING; break;
                case CAST_ASCII:    node->val_type = VAL_INT; break;
                case CAST_BYTE:     node->val_type = VAL_BYTE; break;
                default:            node->val_type = VAL_INT;
            }
            break;
        }
        case AST_TYPE_ANNOTATION: {
            /* 类型标注 <type>expr：必须检查子表达式，否则未定义变量等错误逃过语义检查，
               编译出跨栈不平衡字节码（VALUE 压栈 / INT64 弹出 → 栈下溢） */
            err |= typecheck_expr(node->u.type_annotation.expr);
            /* formdata 字面量：<formdata>{...} / <formdata>[[k,v],...] */
            if(node->u.type_annotation.cast_type == CAST_FORMDATA) {
                node->val_type = VAL_FORMDATA;
                break;
            }
            /* 容器泛型：<T>[..] 逐元素转型，仍是容器 */
            if(node->u.type_annotation.expr->val_type == VAL_ARRAY ||
               node->u.type_annotation.expr->val_type == VAL_MAP) {
                node->val_type = node->u.type_annotation.expr->val_type;
                break;
            }
            /* <T>bytes(...)：类型化 bytes，结果仍是 bytes（动态，可下标 / .len），
             * 不能按 CAST_DOUBLE/INT 等定型，否则 d[0] 等下标会被拦截 */
            if(node->u.type_annotation.expr->type == AST_CALL &&
               node->u.type_annotation.expr->u.call.name &&
               strcmp(node->u.type_annotation.expr->u.call.name, "bytes") == 0) {
                node->val_type = VAL_NONE;
                break;
            }
            switch(node->u.type_annotation.cast_type) {
                case CAST_INT: case CAST_INT_INFER: node->val_type = VAL_INT; break;
                case CAST_DOUBLE:   node->val_type = VAL_DOUBLE; break;
                case CAST_CHAR:     node->val_type = VAL_CHAR; break;
                case CAST_BOOL:     node->val_type = VAL_BOOL; break;
                case CAST_STRING:   node->val_type = VAL_STRING; break;
                case CAST_ASCII:    node->val_type = VAL_INT; break;
                case CAST_BYTE:     node->val_type = VAL_BYTE; break;
                /* 自定义类型（struct/class/type/ptr/bigint/decimal/bitdecimal 等）
                 * 均为动态类型（VALUE 栈），标注不应改变 val_type 为 INT，
                 * 否则字段访问（a.x）会被 AST_INDEX 拦截 */
                default:            node->val_type = VAL_NONE;
            }
            break;
        }
        case AST_TERNARY: {
            err |= typecheck_expr(node->u.ternary.cond);
            err |= typecheck_expr(node->u.ternary.true_expr);
            err |= typecheck_expr(node->u.ternary.false_expr);
            ValueType t1 = node->u.ternary.true_expr->val_type;
            ValueType t2 = node->u.ternary.false_expr->val_type;
            if(t1 == VAL_STRING || t2 == VAL_STRING) {
                node->val_type = VAL_STRING;
            } else {
                node->val_type = t1;
            }
            break;
        }
        case AST_CALL: {
            err |= typecheck_call(node);
            break;
        }
        case AST_METHOD_CALL: {
            /* recv.method(args)：检查接收者与实参，并做方法 private/protected 访问控制 */
            err |= typecheck_expr(node->u.method_call.recv);
            err |= typecheck_call_args(node->u.method_call.args);
            const char* cow = tc_owner(node->u.method_call.recv);
            if(cow) {
                /* 访问控制以方法的"定义类"为准：接收者可能是子类实例，
                 * 但 private 成员的可见性仍限定在声明它的类内部 */
                const char* def_owner = NULL;
                AstNode* mdef = class_find_method_owner(cow, node->u.method_call.method, &def_owner);
                if(mdef && mdef->type == AST_FUNC_DEF) {
                    int mam = mdef->u.func_def.access_modifier;
                    if(!tc_access_ok(def_owner ? def_owner : cow, mam)) {
                        LOG_ERROR("语义错误(第%d行)：方法 '%s' 为 %s，当前上下文不可调用\n",
                                  node->line, node->u.method_call.method,
                                  mam == ACCESS_PRIVATE ? "private" : "protected");
                        err = 1;
                    }
                }
            }
            node->val_type = VAL_NONE;
            break;
        }
        case AST_DYN_CALL: {
            // 调用链 f(1)(2)：callee 是任意表达式（函数值），动态语言不做静态函数性校验
            err |= typecheck_expr(node->u.dyn_call.callee);
            err |= typecheck_call_args(node->u.dyn_call.args);
            node->val_type = VAL_NONE;
            break;
        }
        case AST_RETURN: {
            if(node->u.ret.ret_val) {
                err |= typecheck_expr(node->u.ret.ret_val);
                node->val_type = node->u.ret.ret_val->val_type;
            } else {
                node->val_type = VAL_NONE;
            }
            break;
        }
        case AST_YIELD: {
            /* yield 值必须递归检查：否则 gen arrow 中引用的外层变量
             * 不会被记录为捕获，导致 lambda 捕获数为 0、误发 GETFUNC */
            if(node->u.yieldnode.value) {
                err |= typecheck_expr(node->u.yieldnode.value);
                node->val_type = node->u.yieldnode.value->val_type;
            } else {
                node->val_type = VAL_NONE;
            }
            break;
        }
        case AST_SWITCH: {
            err |= typecheck_expr(node->u.sw.cond);
            AstNode* cp = node->u.sw.cases;
            while(cp) {
                if(cp->u.cs.const_val) err |= typecheck_expr(cp->u.cs.const_val);
                if(cp->u.cs.bind_var) {
                    static_sym_put(cp->u.cs.bind_var, VAL_NONE);
                }
                if(cp->u.cs.guard) err |= typecheck_expr(cp->u.cs.guard);
                err |= typecheck_expr(cp->u.cs.body);
                cp = cp->u.cs.next;
            }
            node->val_type = VAL_NONE;
            break;
        }
        case AST_CASE: {
            if(node->u.cs.const_val) err |= typecheck_expr(node->u.cs.const_val);
            err |= typecheck_expr(node->u.cs.body);
            node->val_type = VAL_NONE;
            break;
        }
        case AST_BREAK:
        case AST_CONTINUE:
            node->val_type = VAL_NONE;
            break;
        case AST_FUNC_DEF: {
            // ...rest 位置检查
            AstNode* p = node->u.func_def.params;
            while(p) {
                if(p->u.param.is_ellipsis && p->u.param.next != NULL) {
                    LOG_ERROR("语义错误：可变参数...必须放在参数列表最后\n");
                    err = 1;
                }
                p = p->u.param.next;
            }
            int is_lambda = (strncmp(node->u.func_def.name, "_lambda_", 8) == 0 ||
                             strncmp(node->u.func_def.name, "_arrow_", 7) == 0);
            if(func_depth > 0 && !is_lambda) {
                // 嵌套具名函数：codegen 不支持，语义检查同样跳过（保持一致）
                node->val_type = VAL_FUNC;
                break;
            }
            func_depth++;
            AstNode* save_cur = g_cur_func_def;
            g_cur_func_def = node;
            sym_save();
            int vo_base = g_vo_cnt;
            int save_in_lambda = in_lambda;
            AstNode* save_params = g_lambda_params;
            int save_lc = lambda_locals_cnt;
            if(is_lambda) { in_lambda = 1; g_lambda_params = node->u.func_def.params; lambda_locals_cnt = 0; cap_push(); }
            // 登记参数（覆盖同名全局实现遮蔽；类型动态 → VAL_NONE 占位）
            for(p = node->u.func_def.params; p; p = p->u.param.next) {
                static_sym_put(p->u.param.name, VAL_NONE);
            }
            /* 是否构造函数：处于方法上下文且名字以 ___init__ 结尾 */
            int this_is_ctor = 0;
            if(g_method_owner) {
                const char* fnm = node->u.func_def.name;
                size_t fnl = strlen(fnm);
                this_is_ctor = (fnl >= 9 && strcmp(fnm + fnl - 9, "___init__") == 0);
            }
            int save_in_ctor = g_in_ctor;
            g_in_ctor = this_is_ctor;
            err |= typecheck_expr(node->u.func_def.body);
            g_in_ctor = save_in_ctor;
            /* const func：纯函数约束校验（body 只允许 const 声明/return、
             * 表达式只由参数/常量/纯运算/其他 const func 调用组成） */
            if(node->u.func_def.is_const && !ce_validate_func(node)) {
                LOG_ERROR("语义错误(第%d行)：const 函数 '%s' 不是纯函数：体内只允许常量声明与 return\n",
                          node->line, node->u.func_def.name);
                err = 1;
            }
            if(is_lambda) {
                in_lambda = save_in_lambda; g_lambda_params = save_params; lambda_locals_cnt = save_lc;
                int ncapt = cap_pop_record(node->u.func_def.name);
                // 该 lambda 捕获了外层局部变量：
                // - lambda 自身需重编译以把捕获变量注册为槽位
                // - 其所在外层函数体需重编译以发射 OPC_MKCLOSURE
                if(ncapt > 0) {
                    /* 加入重编译列表的辅助 */
                    #define RECOMPILE_ADD(n) do { \
                        int _dup = 0; \
                        for(int _i = 0; _i < g_recompile_cnt; _i++) \
                            if(g_recompile[_i] == (n)) { _dup = 1; break; } \
                        if(!_dup) { \
                            if(g_recompile_cnt >= g_recompile_cap) { \
                                int _nc = g_recompile_cap > 0 ? g_recompile_cap * 2 : 16; \
                                AstNode** _nt = (AstNode**)realloc(g_recompile, (size_t)_nc * sizeof(AstNode*)); \
                                if(!_nt) { LOG_ERROR("重编译表扩容内存不足\n"); exit(EXIT_FAILURE); } \
                                g_recompile = _nt; g_recompile_cap = _nc; \
                            } \
                            g_recompile[g_recompile_cnt++] = (n); \
                        } \
                    } while(0)
                    RECOMPILE_ADD(node);
                    /* 外层作用域：若是 struct/class 方法（非 lambda 名）进方法重编译表
                     * （需携带属主）；若是嵌套 lambda 或普通函数，按普通函数处理。
                     * 注意不能仅凭 g_method_owner 判断——方法内多层嵌套时，外层仍是 lambda */
                    if(save_cur) {
                        const char* sn = save_cur->u.func_def.name;
                        _Bool outer_is_lambda =
                            (strncmp(sn, "_arrow_", 7) == 0 ||
                             strncmp(sn, "_lambda_", 8) == 0);
                        if(g_method_owner && !outer_is_lambda)
                            method_recomp_add(g_method_owner, save_cur);
                        else
                            RECOMPILE_ADD(save_cur);
                    }
                    #undef RECOMPILE_ADD
                }
            }
            vo_rollback(vo_base);
            sym_restore();
            g_cur_func_def = save_cur;
            func_depth--;
            node->val_type = VAL_FUNC;
            break;
        }

        default: break;
    }
    return err;
}
