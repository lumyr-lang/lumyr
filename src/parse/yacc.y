/* 编译期模块：g_lambda_seq 为单线程语法状态，未来并发编译需实例化。 */
%code requires {
    typedef struct AstNode AstNode;
}
%{
#define YYERROR_VERBOSE
#include "ast/ast.h"
#include "ast/func_compile.h"
#include "ast/ast_types.h"
#include "parse/macro.h"
#include "annotation/lm_annotation.h"
#include <stdio.h>
#include <string.h>

/* yyerror 定义在 %% 之后，helper 中提前使用需前置声明 */
void yyerror(const char* s);

/* 最近一次 type_name 归约中的自定义类型名（ID 分支；builtin 分支为 NULL）
 * 归约后立即由上层字段声明动作消费（所有权转移），仅作单次传递，不持久保存 */
static char* g_last_custom_type_name = NULL;

/* type 声明属性收集器（yacc 动作顺序填充，声明语句动作消费后清空） */
static char** g_prop_names = NULL;
static ValueType* g_prop_types = NULL;
static int* g_prop_access_modifiers = NULL;
static int* g_prop_const_flags = NULL;   /* const 字段标记（1=构造后不可修改） */
/* 自定义类型名（字段为 struct/class 类型时记录，如 inner: Inner → "Inner"；其余为 NULL） */
static char** g_prop_struct_names = NULL;
/* 字段级访问器策略：-1=无字段注解（默认，随类级 @Data 决策）；0=@Xxx(false)；1=@Xxx(true)/无参 */
static int* g_prop_getter_policy = NULL;
static int* g_prop_setter_policy = NULL;
static int g_prop_n = 0, g_prop_cap = 0;
static void type_prop_push(char* name, ValueType vt, int access_modifier, int is_const, char* struct_name)
{
    if(g_prop_n >= g_prop_cap) {
        int nc = g_prop_cap > 0 ? g_prop_cap * 2 : 8;
        g_prop_names = (char**)realloc(g_prop_names, (size_t)nc * sizeof(char*));
        g_prop_types = (ValueType*)realloc(g_prop_types, (size_t)nc * sizeof(ValueType));
        g_prop_access_modifiers = (int*)realloc(g_prop_access_modifiers, (size_t)nc * sizeof(int));
        g_prop_const_flags = (int*)realloc(g_prop_const_flags, (size_t)nc * sizeof(int));
        g_prop_struct_names = (char**)realloc(g_prop_struct_names, (size_t)nc * sizeof(char*));
        g_prop_getter_policy = (int*)realloc(g_prop_getter_policy, (size_t)nc * sizeof(int));
        g_prop_setter_policy = (int*)realloc(g_prop_setter_policy, (size_t)nc * sizeof(int));
        g_prop_cap = nc;
    }
    g_prop_names[g_prop_n] = name;
    g_prop_types[g_prop_n] = vt;
    g_prop_access_modifiers[g_prop_n] = access_modifier;
    g_prop_const_flags[g_prop_n] = is_const;
    g_prop_struct_names[g_prop_n] = struct_name;  /* 不复制：归约动作的 $3 所有权转移 */
    g_prop_getter_policy[g_prop_n] = -1;
    g_prop_setter_policy[g_prop_n] = -1;
    g_prop_n++;
}
static void type_prop_clear(void)
{
    for(int i = 0; i < g_prop_n; i++) {
        free(g_prop_names[i]);
        free(g_prop_struct_names[i]);
    }
    free(g_prop_names); free(g_prop_types); free(g_prop_access_modifiers);
    free(g_prop_const_flags); free(g_prop_struct_names);
    free(g_prop_getter_policy); free(g_prop_setter_policy);
    g_prop_names = NULL; g_prop_types = NULL; g_prop_access_modifiers = NULL;
    g_prop_const_flags = NULL; g_prop_struct_names = NULL;
    g_prop_getter_policy = NULL; g_prop_setter_policy = NULL;
    g_prop_n = 0; g_prop_cap = 0;
}

/* struct 声明属性收集器（用精确 CastKind 类型） */
static char** g_struct_prop_names = NULL;
static CastKind* g_struct_cast_kinds = NULL;
static char** g_struct_prop_struct_names = NULL;
static int g_struct_prop_n = 0, g_struct_prop_cap = 0;
static char* g_current_struct_name = NULL; /* 当前正在解析的 struct 名，用于方法注册 */

/* class 声明属性收集器 */
static char** g_class_prop_names = NULL;
static ValueType* g_class_prop_types = NULL;
static int g_class_prop_n = 0, g_class_prop_cap = 0;
static char* g_current_class_name = NULL; /* 当前正在解析的 class 名，用于方法注册 */
static char* g_current_class_parent = NULL; /* 当前 class 的父类名 */
static int g_current_class_is_abstract = 0; /* 当前 class 是否是抽象类 */
static AstNode** g_class_methods = NULL; /* 当前 class 的方法定义临时列表 */
static AstNode* g_class_constructor = NULL; /* 当前 class 的构造函数（__init__ 方法） */
static char** g_class_interfaces = NULL; /* 当前 class 实现的接口名列表 */
static int g_class_ninterfaces = 0; /* 当前 class 实现的接口数量 */

/* 辅助：如果在 class/struct 内部，不注册到全局符号表（方法注册到类型方法表，
 * 必须通过接收者类型分派）。此前只检查 class，struct 方法会以裸名 sym_set
 * 一个 func_obj=NULL 的空壳，覆盖同名顶层函数。 */
static void try_register_global_func(const char* name, Value func_val) {
    if(g_current_class_name) return;
    if(g_current_struct_name) return;
    sym_set(name, func_val);
}
static int g_class_method_n = 0, g_class_method_cap = 0;

static void g_class_method_push(AstNode* m) {
    if(g_class_method_n >= g_class_method_cap) {
        int nc = g_class_method_cap > 0 ? g_class_method_cap * 2 : 8;
        g_class_methods = (AstNode**)realloc(g_class_methods, (size_t)nc * sizeof(AstNode*));
        g_class_method_cap = nc;
    }
    g_class_methods[g_class_method_n++] = m;
}

static void g_class_method_clear(void) {
    free(g_class_methods);
    g_class_methods = NULL;
    g_class_method_n = 0;
    g_class_method_cap = 0;
    g_class_constructor = NULL;
}

/* 构造 class 静态属性 assign 节点、注册静态成员访问表并压入方法列表 */
static AstNode* make_static_prop(char* prop_name, AstNode* expr, int access) {
    char* svn = (char*)malloc(strlen(g_current_class_name) + strlen(prop_name) + 2);
    sprintf(svn, "%s_%s", g_current_class_name, prop_name);
    class_static_member_register(svn, g_current_class_name, access);
    AstNode* assign = ast_assign(svn, expr);
    free(svn);
    g_class_method_push(assign);
    return assign;
}

static void class_prop_push(char* name, ValueType vt) {
    if(g_class_prop_n >= g_class_prop_cap) {
        int nc = g_class_prop_cap > 0 ? g_class_prop_cap * 2 : 8;
        g_class_prop_names = (char**)realloc(g_class_prop_names, (size_t)nc * sizeof(char*));
        g_class_prop_types = (ValueType*)realloc(g_class_prop_types, (size_t)nc * sizeof(ValueType));
        g_class_prop_cap = nc;
    }
    g_class_prop_names[g_class_prop_n] = name;
    g_class_prop_types[g_class_prop_n] = vt;
    g_class_prop_n++;
}

static void class_prop_clear(void) {
    for(int i = 0; i < g_class_prop_n; i++) free(g_class_prop_names[i]);
    free(g_class_prop_names); g_class_prop_names = NULL;
    free(g_class_prop_types); g_class_prop_types = NULL;
    g_class_prop_n = 0; g_class_prop_cap = 0;
}
/* 辅助：如果在 struct 定义内部，给 self 参数加上 struct 类型标注 */
static void annotate_self_if_in_struct(AstNode* func_def) {
    if(!func_def || func_def->type != AST_FUNC_DEF) return;
    /* struct 和 class 方法都需要 self 作为首参 */
    if(!g_current_struct_name && !g_current_class_name) return;
    /* 选择 self 的类型约束：struct 用 struct 名，class 由 class_add_method 后续覆写为 class:<名> */
    const char* self_type = g_current_struct_name ? g_current_struct_name : g_current_class_name;
    /* 检查是否已有 self 参数；若否则自动添加为首参 */
    AstNode* p = func_def->u.func_def.params;
    int has_self = 0;
    while(p) {
        if(p->u.param.name && strcmp(p->u.param.name, "self") == 0) {
            if(!p->u.param.constraint) {
                p->u.param.constraint = strdup(self_type);
            }
            has_self = 1;
            break;
        }
        p = p->u.param.next;
    }
    if(!has_self) {
        /* 自动添加 self 为首参（约束为当前类型名，IR 编译期作为 PTR 参数）
         * class 方法的 constraint 会被 class_add_method 覆写为 "class:<名>" */
        AstNode* self_param = ast_param(strdup("self"), 0, NULL);
        self_param->u.param.constraint = strdup(self_type);
        self_param->u.param.next = func_def->u.func_def.params;
        func_def->u.func_def.params = self_param;
    }
}
/* 静态方法无实例语义：移除解析期 annotate_self_if_in_struct 自动注入的 self 首参，
 * 否则顶层以 Class_method(args) 调用时实参会错位绑定到 self 槽 */
static void strip_static_self(AstNode* fn) {
    if(!fn || fn->type != AST_FUNC_DEF) return;
    AstNode* sp = fn->u.func_def.params;
    if(sp && sp->u.param.name && strcmp(sp->u.param.name, "self") == 0) {
        fn->u.func_def.params = sp->u.param.next;
        free(sp->u.param.name);
        if(sp->u.param.constraint) free(sp->u.param.constraint);
        free(sp);
    }
}

/* ===== @Data 注解与字段初始化器 ===== */

/* 当前 class 的字段初始化器语句链（self.field = expr，声明序），
 * class 结束规则统一注入构造函数 */
static AstNode* g_class_init_stmts = NULL;

/* 记录字段初始化器：field 带 "= expr" 声明时追加 self.field = expr */
static void record_field_init(const char* field, AstNode* expr) {
    AstNode* stmt = ast_index_assign(ast_var("self"), ast_string(field), expr);
    if(g_class_init_stmts)
        g_class_init_stmts = ast_seq_append(g_class_init_stmts, stmt);
    else
        g_class_init_stmts = ast_seq(stmt, NULL);
}

/* annotation_list（AST_SEQ 链）中是否存在指定注解。
 * 注意：单注解是裸 AST_ANNOTATION 节点（非 AST_SEQ 包装） */
static int annotation_list_has(AstNode* list, const char* name) {
    AstNode* a = list;
    while(a) {
        if(a->type == AST_ANNOTATION) {
            /* 裸单节点：命中判定后即终点 */
            return a->u.annotation.name && strcmp(a->u.annotation.name, name) == 0;
        }
        if(a->type != AST_SEQ) return 0;
        AstNode* f = a->u.seq.first;
        if(f && f->type == AST_ANNOTATION && f->u.annotation.name &&
           strcmp(f->u.annotation.name, name) == 0) return 1;
        a = a->u.seq.second;
    }
    return 0;
}

/* 把类级注解登记到注解注册表（此前类注解只解析不登记，无法查询）。
 * 单注解为裸 AST_ANNOTATION；多个为 AST_SEQ 链（first=注解，second=后续） */
static void register_class_annotations(AstNode* list) {
    AstNode* a = list;
    while(a) {
        if(a->type == AST_ANNOTATION) {
            if(a->u.annotation.name) {
                int cat = annotation_is_system(a->u.annotation.name)
                              ? ANNOTATION_CATEGORY_SYSTEM : ANNOTATION_CATEGORY_USER;
                annotation_register(a->u.annotation.name, ANNOTATION_TYPE_CLASS, cat,
                                    a->u.annotation.args, g_current_class_name, NULL, NULL);
            }
            return;
        }
        if(a->type != AST_SEQ) return;
        AstNode* f = a->u.seq.first;
        if(f && f->type == AST_ANNOTATION && f->u.annotation.name) {
            int cat = annotation_is_system(f->u.annotation.name)
                          ? ANNOTATION_CATEGORY_SYSTEM : ANNOTATION_CATEGORY_USER;
            annotation_register(f->u.annotation.name, ANNOTATION_TYPE_CLASS, cat,
                                f->u.annotation.args, g_current_class_name, NULL, NULL);
        }
        a = a->u.seq.second;
    }
}

/* 当前 class 临时方法列表中是否已存在同名方法（@Data 不覆盖手写实现） */
static int class_method_exists(const char* mname) {
    for(int i = 0; i < g_class_method_n; i++) {
        AstNode* m = g_class_methods[i];
        if(m && m->type == AST_FUNC_DEF && m->u.func_def.name &&
           strcmp(m->u.func_def.name, mname) == 0) return 1;
    }
    return 0;
}

/* 拼接 Lombok 风格访问器名：prefix + 首字母大写的字段名
 * 如 ("get","name") -> getName；("set","age") -> setAge；
 * 字段首字母已大写则保持（"URL" -> getURL，幂等） */
static char* make_accessor_name(const char* prefix, const char* fld) {
    size_t nlen = strlen(prefix) + strlen(fld) + 1;
    char* name = (char*)malloc(nlen);
    strcpy(name, prefix);
    size_t off = strlen(prefix);
    name[off] = (fld[0] >= 'a' && fld[0] <= 'z') ? (char)(fld[0] - 'a' + 'A') : fld[0];
    strcpy(name + off + 1, fld + 1);
    return name;
}

/* 解析字段级 @Getter / @Setter 注解，写入该字段的访问器策略。
 *  - 无参数：策略为 1（生成）
 *  - @Xxx(true)/@Xxx(false)：取 bool 字面量
 *  - 实参不是 bool：语法错误
 * 同一字段重复标注时后者覆盖（链遍历顺序即书写顺序） */
static int apply_field_prop_annotations(int idx, AstNode* list) {
    if(!list) return 0;
    AstNode* a = list;
    while(a) {
        AstNode* cur;
        if(a->type == AST_ANNOTATION) cur = a;
        else if(a->type == AST_SEQ) cur = a->u.seq.first;
        else return 0;
        if(cur && cur->type == AST_ANNOTATION && cur->u.annotation.name) {
            int is_getter = -1;
            if(strcmp(cur->u.annotation.name, "Getter") == 0) is_getter = 1;
            else if(strcmp(cur->u.annotation.name, "Setter") == 0) is_getter = 0;
            if(is_getter >= 0) {
                int policy = 1;   /* 无参默认生成 */
                AstNode* args = cur->u.annotation.args;
                if(args) {
                    if(args->type != AST_BOOL) {
                        yyerror("@Getter/@Setter 参数须为 bool 字面量 true/false");
                        return 1;
                    }
                    else
                        policy = args->u.bval ? 1 : 0;
                }
                if(is_getter) g_prop_getter_policy[idx] = policy;
                else g_prop_setter_policy[idx] = policy;
            }
        }
        if(a->type == AST_ANNOTATION) return 0;   /* 裸单节点：遍历结束 */
        a = a->u.seq.second;
    }
    return 0;
}

/* 生成访问器：字段级 @Getter/@Setter 策略优先于类级 @Data；
 * 未标注字段随 @Data（getter/setter 均生成）。
 * const 字段强制不生成 setter（硬语义优先于 @Setter(true)）。
 * bool 字段 getter 命名为 isXxx（Lombok 惯例）；手写同名方法仍跳过。
 * 生成节点压入 g_class_methods，复用 class 结束规则的统一方法编译循环 */
static void apply_accessors(int has_data) {
    for(int i = 0; i < g_prop_n; i++) {
        const char* fld = g_prop_names[i];
        ValueType vt = g_prop_types[i];

        int want_getter = g_prop_getter_policy[i] >= 0 ? g_prop_getter_policy[i] : has_data;
        int want_setter = g_prop_setter_policy[i] >= 0 ? g_prop_setter_policy[i]
                                                       : (has_data && !g_prop_const_flags[i]);
        /* const 硬语义：构造后不可写，任何情况都不生成 setter */
        if(g_prop_const_flags[i]) want_setter = 0;

        /* getter: func getXxx(self): T { return self.f; } */
        if(want_getter) {
        char* gname = make_accessor_name(vt == VAL_BOOL ? "is" : "get", fld);
        if(!class_method_exists(gname)) {
            AstNode* selfp = ast_param("self", 0, NULL);
            AstNode* acc = ast_index(ast_var("self"), ast_string(fld));
            AstNode* blk = ast_block(ast_seq(ast_return(acc), NULL));
            AstNode* fn = ast_func_def(gname, selfp, blk);
            char* tn = valtype_to_name(vt);
            /* 具体标量类型保留返回标注；void/any（含容器/自定义类型）置空走动态 */
            if(tn && strcmp(tn, "void") != 0 && strcmp(tn, "any") != 0)
                fn->u.func_def.ret_type_name = tn;
            else
                free(tn);
            fn->u.func_def.is_class_method = 1;
            fn->u.func_def.access_modifier = 0;
            fn->u.func_def.is_getter = 1;
            g_class_method_push(fn);
        }
        free(gname);
        }

        /* setter: func setXxx(self, value) { self.f = value; } */
        if(want_setter) {
        char* sname = make_accessor_name("set", fld);
        if(!class_method_exists(sname)) {
            AstNode* valp = ast_param("value", 0, NULL);
            /* 标量字段：形参加类型约束，调用处自动类型转换；
             * 容器/自定义类型字段保持动态（any） */
            if(vt == VAL_INT || vt == VAL_DOUBLE || vt == VAL_STRING ||
               vt == VAL_BOOL || vt == VAL_CHAR) {
                valp->u.param.constraint = valtype_to_name(vt);
            }
            AstNode* selfp = ast_param("self", 0, NULL);
            selfp->u.param.next = valp;
            AstNode* st = ast_index_assign(ast_var("self"), ast_string(fld),
                                           ast_var("value"));
            AstNode* blk = ast_block(ast_seq(st, NULL));
            AstNode* fn = ast_func_def(sname, selfp, blk);
            fn->u.func_def.is_class_method = 1;
            fn->u.func_def.access_modifier = 0;
            fn->u.func_def.is_setter = 1;
            g_class_method_push(fn);
        }
        free(sname);
        }
    }
}

/* 字段初始化器注入构造函数：
 *  - 显式 ctor：初始化语句前插到函数体首部（先于用户逻辑执行）
 *  - 无 ctor：合成 Class___init__；若父类有无参 ctor，先合成 super() 调用，
 *    保留"子类无 ctor 时父类构造自动执行"的既有行为（ir_compile ctor 链）
 * 返回 1 = 致命错误（调用点 YYABORT 中止编译），0 = 正常 */
static int apply_field_initializers(void) {
    if(!g_class_init_stmts) return 0;
    AstNode* stmts = g_class_init_stmts;
    g_class_init_stmts = NULL;

    if(g_class_constructor) {
        AstNode* body = g_class_constructor->u.func_def.body;
        if(body && body->type == AST_BLOCK) {
            AstNode* old = body->u.block.stmts;
            body->u.block.stmts = ast_seq(stmts, old);
        }
        return 0;
    }

    AstNode* body_stmts = NULL;
    if(g_current_class_parent) {
        /* 沿继承链找第一个有 constructor 的祖先（与 ir_compile 同规则） */
        TypeDef* td2 = class_lookup(g_current_class_parent);
        TypeDef* pctor_td = NULL;
        while(td2) {
            if(td2->constructor) { pctor_td = td2; break; }
            if(!td2->parent) break;
            td2 = class_lookup(td2->parent);
        }
        if(pctor_td) {
            /* 统计父类 ctor 用户参数数（除 self 首参） */
            AstNode* pp = pctor_td->constructor->u.func_def.params;
            if(pp && pp->u.param.name && strcmp(pp->u.param.name, "self") == 0)
                pp = pp->u.param.next;
            int pn = 0;
            for(; pp; pp = pp->u.param.next) pn++;
            if(pn > 0) {
                yyerror("存在字段初始化器且类未定义构造函数，需要自动调用父类无参构造函数，"
                        "但父类构造函数含必填参数；请显式定义构造函数并调用 super(...)");
                return 1;
            }
            size_t l = strlen(pctor_td->name) + 10;
            char* nm = (char*)malloc(l);
            snprintf(nm, l, "%s___init__", pctor_td->name);
            /* super() 等价于直接调用父类 ctor：Parent___init__(self) */
            AstNode* calln = ast_call(nm, ast_var("self"));
            free(nm);
            body_stmts = ast_seq(calln, NULL);
        }
    }

    if(body_stmts)
        body_stmts = ast_seq_append(body_stmts, stmts);
    else
        body_stmts = stmts;

    AstNode* blk = ast_block(body_stmts);
    AstNode* selfp = ast_param("self", 0, NULL);
    /* self 约束须为类名：compile 据此设置 method_self_struct，字段赋值才能走
     * STORE_FIELD 快路径（与 annotate_self_if_in_struct 对显式 ctor 的处理一致） */
    selfp->u.param.constraint = strdup(g_current_class_name);
    size_t l = strlen(g_current_class_name) + 10;
    char* nm = (char*)malloc(l);
    snprintf(nm, l, "%s___init__", g_current_class_name);
    AstNode* ctor = ast_func_def(nm, selfp, blk);
    free(nm);
    ctor->u.func_def.is_class_method = 1;
    g_class_constructor = ctor;
    return 0;
}

static AstNode** g_struct_methods = NULL; /* 当前 struct 的方法定义临时列表 */
static int g_struct_method_n = 0, g_struct_method_cap = 0;
static void g_struct_method_push(AstNode* m) {
    if(g_struct_method_n >= g_struct_method_cap) {
        int nc = g_struct_method_cap > 0 ? g_struct_method_cap * 2 : 8;
        g_struct_methods = (AstNode**)realloc(g_struct_methods, (size_t)nc * sizeof(AstNode*));
        g_struct_method_cap = nc;
    }
    g_struct_methods[g_struct_method_n++] = m;
}
static void g_struct_method_clear(void) {
    free(g_struct_methods);
    g_struct_methods = NULL;
    g_struct_method_n = 0;
    g_struct_method_cap = 0;
}
static void struct_prop_push(char* name, CastKind ck, char* struct_name)
{
    if(g_struct_prop_n >= g_struct_prop_cap) {
        int nc = g_struct_prop_cap > 0 ? g_struct_prop_cap * 2 : 8;
        g_struct_prop_names = (char**)realloc(g_struct_prop_names, (size_t)nc * sizeof(char*));
        g_struct_cast_kinds = (CastKind*)realloc(g_struct_cast_kinds, (size_t)nc * sizeof(CastKind));
        g_struct_prop_struct_names = (char**)realloc(g_struct_prop_struct_names, (size_t)nc * sizeof(char*));
        g_struct_prop_cap = nc;
    }
    g_struct_prop_names[g_struct_prop_n] = name;
    g_struct_cast_kinds[g_struct_prop_n] = ck;
    g_struct_prop_struct_names[g_struct_prop_n] = struct_name;
    g_struct_prop_n++;
}
static void struct_prop_clear(void)
{
    for(int i = 0; i < g_struct_prop_n; i++) {
        free(g_struct_prop_names[i]);
        if(g_struct_prop_struct_names[i]) free(g_struct_prop_struct_names[i]);
    }
    free(g_struct_prop_names); free(g_struct_cast_kinds); free(g_struct_prop_struct_names);
    g_struct_prop_names = NULL; g_struct_cast_kinds = NULL; g_struct_prop_struct_names = NULL;
    g_struct_prop_n = 0; g_struct_prop_cap = 0;
}

/* 泛型 <Person>[e1,e2] → 每个元素包 Person(e) 构造调用（遍历 ast_seq 链） */
static AstNode* wrap_type_list(const char* tname, AstNode* chain)
{
    if(!chain) return NULL;
    if(chain->type == AST_SEQ) {
        chain->u.seq.first = wrap_type_list(tname, chain->u.seq.first);
        chain->u.seq.second = wrap_type_list(tname, chain->u.seq.second);
        return chain;
    }
    /* 与语法动作一致：struct/class 名走 ast_class_new，否则普通调用。
       此前一律 ast_call，"Pt(1)" 被当普通函数，落回动态路径变成 int64 */
    if(struct_lookup(tname) || class_lookup(tname)) {
        return ast_class_new(strdup(tname), 1, chain);
    }
    return ast_call(strdup(tname), chain);
}
/* 泛型 map 字面量 <K,V>{k1:v1,...}：沿 SEQ 链给每个 entry 的键包 K cast、值包 V cast，
   再生成普通 map_lit。编译器按 entry 目标 VALUE 编译，CAST 负责把键/值转到 K/V，
   存储仍为 ValueMap（运行时键支持任意类型）。 */
static AstNode* wrap_map_kv(AstNode* items, CastKind kck, CastKind vck)
{
    if(!items) return NULL;
    if(items->type == AST_SEQ) {
        items->u.seq.first  = wrap_map_kv(items->u.seq.first, kck, vck);
        items->u.seq.second = wrap_map_kv(items->u.seq.second, kck, vck);
        return items;
    }
    if(items->type == AST_MAP_ENTRY) {
        items->u.map_entry.key   = new_cast_node(kck, items->u.map_entry.key);
        items->u.map_entry.value = new_cast_node(vck, items->u.map_entry.value);
    }
    return items;
}
/* catch 子句辅助：创建单个 catch 子句节点（用 AST_SEQ 包装，first=type_string, second=var_body_seq） */
static AstNode* make_catch_clause(char* type_name, char* var_name, AstNode* body)
{
    /* 用一个特殊节点存储 catch 子句：AST_SEQ(first=type_string, second=AST_SEQ(first=var_name_string, second=body))
       无类型时用空字符串占位，避免 first=NULL 导致其他遍历逻辑崩溃 */
    AstNode* type_node = ast_string(type_name ? type_name : "");
    AstNode* var_node = ast_string(var_name);
    AstNode* var_body = ast_seq(var_node, body);
    return ast_seq(type_node, var_body);
}

/* 递归展平左结合的 AST_SEQ 链，收集所有叶子节点（catch_clause） */
static void flatten_catch_seq(AstNode* node, AstNode*** arr, int* count, int* cap)
{
    if(!node) return;
    /* catch_clause 本身也是 AST_SEQ，但它的 first 是 AST_STRING（type 或 var），
       而 catch_clause_list 的 AST_SEQ 的 first 不是 AST_STRING，以此区分 */
    if(node->type == AST_SEQ && node->u.seq.first && node->u.seq.first->type != AST_STRING) {
        flatten_catch_seq(node->u.seq.first, arr, count, cap);
        flatten_catch_seq(node->u.seq.second, arr, count, cap);
    } else {
        if(*count >= *cap) {
            *cap = *cap > 0 ? *cap * 2 : 8;
            *arr = (AstNode**)realloc(*arr, sizeof(AstNode*) * (*cap));
        }
        (*arr)[(*count)++] = node;
    }
}

/* 判断是否是单个无类型 catch（用于回退到旧的 ast_try） */
static int is_single_untagged_catch(AstNode* catch_chain)
{
    if(!catch_chain || catch_chain->type != AST_SEQ) return 0;
    /* catch_clause 结构：AST_SEQ(first=type_string, second=AST_SEQ(first=var_string, second=body))
       如果 first 是 AST_STRING 且为空字符串，说明是无类型 catch */
    if(catch_chain->u.seq.first && catch_chain->u.seq.first->type == AST_STRING) {
        const char* s = catch_chain->u.seq.first->u.sval;
        if(s && s[0] == '\0') return 1;
    }
    return 0;
}

/* 从单个无类型 catch_clause 中提取 var_name 和 body */
static void extract_single_catch(AstNode* clause, char** var_name, AstNode** body)
{
    if(clause && clause->type == AST_SEQ) {
        AstNode* var_body = clause->u.seq.second;
        if(var_body && var_body->type == AST_SEQ) {
            AstNode* var_node = var_body->u.seq.first;
            *var_name = var_node ? strdup(var_node->u.sval) : NULL;
            *body = var_body->u.seq.second;
        }
    }
}

/* 从 AST_SEQ 链中收集 catch 子句，构建 ast_try_multi */
static AstNode* build_try_multi(AstNode* body, AstNode* catch_chain, AstNode* finally_body)
{
    AstNode** clauses = NULL;
    int count = 0, cap = 0;
    flatten_catch_seq(catch_chain, &clauses, &count, &cap);

    CatchClause* catches = (CatchClause*)calloc((size_t)count, sizeof(CatchClause));
    for(int i = 0; i < count; i++) {
        AstNode* clause = clauses[i];
        /* clause = AST_SEQ(first=type_string, second=AST_SEQ(first=var_string, second=body)) */
        if(clause && clause->type == AST_SEQ) {
            AstNode* type_node = clause->u.seq.first;
            AstNode* var_body = clause->u.seq.second;
            /* 空字符串表示无类型（捕获所有异常），NULL 表示有类型 */
            if(type_node && type_node->u.sval && type_node->u.sval[0] != '\0') {
                catches[i].type = strdup(type_node->u.sval);
            } else {
                catches[i].type = NULL;
            }
            if(var_body && var_body->type == AST_SEQ) {
                AstNode* var_node = var_body->u.seq.first;
                catches[i].var = var_node ? strdup(var_node->u.sval) : NULL;
                catches[i].body = var_body->u.seq.second;
            }
        }
    }

    free(clauses);
    return ast_try_multi(body, catches, count, finally_body);
}

extern int yylineno;
AstNode* new_cast_node(CastKind cast_type, AstNode* child);
AstNode* maybe_template(const char* s);      // 字符串模板拆解（parse/tmpl.c）
void yyerror(const char* s);
/* 泛型命名字段构造 <CustomType>{ name: v, ... }：把具名 init 展开成位置构造
   ast_class_new（与 CustomType(...) 同路），字段按声明序排列，缺省用 ast_none()。 */
static void collect_map_entries(AstNode* node, AstNode*** arr, int* n, int* cap) {
    if(!node) return;
    if(node->type == AST_SEQ) {
        collect_map_entries(node->u.seq.first, arr, n, cap);
        collect_map_entries(node->u.seq.second, arr, n, cap);
        return;
    }
    if(*n >= *cap) { *cap = *cap ? *cap*2 : 8; *arr = realloc(*arr, (size_t)*cap * sizeof(AstNode*)); }
    (*arr)[(*n)++] = node;
}
static AstNode* wrap_struct_named(const char* tname, AstNode* items)
{
    TypeDef* td = type_lookup(tname);
    if(!td) {
        char buf[256];
        snprintf(buf, sizeof buf, "未定义类型 '%s'", tname);
        yyerror(buf);
        return NULL;
    }
    AstNode** entries = NULL; int en = 0, ecap = 0;
    collect_map_entries(items, &entries, &en, &ecap);
    /* 建立 字段名 -> 值 映射 */
    AstNode** values = (AstNode**)calloc((size_t)(td->nprops > 0 ? td->nprops : 1), sizeof(AstNode*));
    for(int i = 0; i < en; i++) {
        AstNode* e = entries[i];
        if(!e || e->type != AST_MAP_ENTRY) continue;
        const char* fname = e->u.map_entry.key->u.sval;
        int idx = -1;
        for(int j = 0; j < td->nprops; j++) {
            if(td->props[j] && strcmp(td->props[j], fname) == 0) { idx = j; break; }
        }
        if(idx < 0) {
            char buf[256];
            snprintf(buf, sizeof buf, "类型 '%s' 无字段 '%s'", tname, fname);
            yyerror(buf);
            free(values); free(entries);
            return NULL;
        }
        values[idx] = e->u.map_entry.value;
    }
    free(entries);
    if(td->is_struct || td->is_class) {
        /* struct/class：按声明序组装位置实参，走 ast_class_new 构造 */
        AstNode* args = NULL;
        for(int i = 0; i < td->nprops; i++)
            args = ast_arg_append(args, values[i] ? values[i] : ast_none());
        free(values);
        return ast_class_new(strdup(tname), td->nprops, args);
    }
    /* type 形状：构造带类型 cast 的 map literal，每个值 cast 到字段声明类型 */
    AstNode* out = NULL;
    for(int i = 0; i < td->nprops; i++) {
        if(!values[i]) continue;
        CastKind ck = td->ptypes ? valuetype_to_castkind((int)td->ptypes[i]) : CAST_NONE;
        AstNode* v = (ck != CAST_NONE) ? new_cast_node(ck, values[i]) : values[i];
        AstNode* entry = ast_map_entry(ast_string(strdup(td->props[i])), v);
        out = out ? ast_seq(out, entry) : entry;
    }
    free(values);
    return ast_map_lit(out);
}
/* type 形状的位置构造 T(v0, v1, ...)：按字段声明序把位置实参组装成
 * 带字段类型 cast 的 map literal，与命名构造 T{field: v, ...} 的结果一致；
 * 尾部缺省字段跳过（同名构造行为），实参多于字段数则报错。 */
static AstNode* wrap_type_positional(const char* tname, AstNode* args)
{
    TypeDef* td = type_lookup(tname);
    if(!td) {
        char buf[256];
        snprintf(buf, sizeof buf, "未定义类型 '%s'", tname);
        yyerror(buf);
        return NULL;
    }
    AstNode** pos = NULL; int pn = 0, pcap = 0;
    collect_map_entries(args, &pos, &pn, &pcap);   /* left-leaning 链 → 位置序数组 */
    if(pn > td->nprops) {
        char buf[256];
        snprintf(buf, sizeof buf, "类型 '%s' 构造参数过多：需要至多 %d 个，给定 %d 个", tname, td->nprops, pn);
        yyerror(buf);
        free(pos);
        return NULL;
    }
    AstNode* out = NULL;
    for(int i = 0; i < pn; i++) {
        CastKind ck = td->ptypes ? valuetype_to_castkind((int)td->ptypes[i]) : CAST_NONE;
        AstNode* v = (ck != CAST_NONE) ? new_cast_node(ck, pos[i]) : pos[i];
        AstNode* entry = ast_map_entry(ast_string(strdup(td->props[i])), v);
        out = out ? ast_seq(out, entry) : entry;
    }
    free(pos);
    return ast_map_lit(out);
}
static int g_lambda_seq = 0;                 // 匿名函数内部名 _lambda_N
static int g_unpack_tmp_counter = 0;              // unpack 临时变量计数器
// 把一个语句列表（AST_SEQ 链）展开，追加到另一个语句列表末尾
// 注意：必须使用 ast_seq(list, item) 保持左嵌套结构，与 yacc 原生 stmt_list 一致
// 使用 ast_seq_append 会创建右嵌套结构，导致运行时崩溃
static AstNode* stmt_list_append_list(AstNode* list, AstNode* items) {
    if (!items) return list;
    if (items->type == AST_SEQ) {
        list = stmt_list_append_list(list, items->u.seq.first);
        list = stmt_list_append_list(list, items->u.seq.second);
        return list;
    }
    return ast_seq(list, items);
}
static char* make_unpack_tmp_name(void) {
    char* name = (char*)malloc(32);
    snprintf(name, 32, "unpacktmp%d", g_unpack_tmp_counter++);
    return name;
}
/* 链式比较：若 left 已是比较/相等 binop，则 a<b<c → (a<b) && (b<c)；
 * 若 left 已是 (a<b)&&(b<c) 形式（链化结果），则取链尾的右操作数作中项续链：
 *   ((a<b)&&(b<c))<d → ((a<b)&&(b<c)) && (c<d)
 * 中项克隆以避免节点共享；非比较/非链化 binop 退回普通 binop */
static AstNode* chain_cmp(AstNode* left, int op, AstNode* right) {
    if(left && left->type == AST_BINOP) {
        int lop = left->u.bin.op;
        if(lop == OP_LT || lop == OP_GT || lop == OP_GE || lop == OP_LE ||
           lop == OP_EQ || lop == OP_NE) {
            AstNode* mid = ast_clone_node(left->u.bin.right);
            return ast_binop(OP_LOGIC_AND, left, ast_binop(op, mid, right));
        }
        if(lop == OP_LOGIC_AND && left->u.bin.right &&
           left->u.bin.right->type == AST_BINOP) {
            int rop = left->u.bin.right->u.bin.op;
            if(rop == OP_LT || rop == OP_GT || rop == OP_GE || rop == OP_LE ||
               rop == OP_EQ || rop == OP_NE) {
                AstNode* mid = ast_clone_node(left->u.bin.right->u.bin.right);
                return ast_binop(OP_LOGIC_AND, left, ast_binop(op, mid, right));
            }
        }
    }
    return ast_binop(op, left, right);
}
// 构建对象解构赋值序列：unpack {a,b} = obj -> _tmp=obj; a=_tmp.a; b=_tmp.b;
static AstNode* build_object_destruct(AstNode* list, AstNode* names, AstNode* rhs) {
    char* tmp = make_unpack_tmp_name();
    // 直接把语句追加到 list 后面，使用 ast_seq 保持左嵌套结构
    list = ast_seq(list, ast_assign(tmp, rhs));
    AstNode* cur = names;
    while (cur && cur->type == AST_SEQ) {
        AstNode* var_node = cur->u.seq.first;
        if (var_node && var_node->type == AST_VAR) {
            const char* name = var_node->u.varname;
            AstNode* idx = ast_index(ast_var(tmp), ast_string(name));
            list = ast_seq(list, ast_assign(name, idx));
        }
        cur = cur->u.seq.second;
    }
    return list;
}
// 构建数组解构赋值序列：unpack [x,y] = arr -> _tmp=arr; x=_tmp[0]; y=_tmp[1];
static AstNode* build_array_destruct(AstNode* list, AstNode* names, AstNode* rhs) {
    char* tmp = make_unpack_tmp_name();
    // 直接把语句追加到 list 后面
    list = ast_seq(list, ast_assign(tmp, rhs));
    AstNode* cur = names;
    int index = 0;
    while (cur && cur->type == AST_SEQ) {
        AstNode* var_node = cur->u.seq.first;
        if (var_node && var_node->type == AST_VAR) {
            const char* name = var_node->u.varname;
            AstNode* idx = ast_index(ast_var(tmp), ast_int(index));
            list = ast_seq(list, ast_assign(name, idx));
        }
        cur = cur->u.seq.second;
        index++;
    }
    return list;
}
int yylex(void);
AstNode* root;
/* 模块系统（第一阶段）：判断一个标识符是否为 import 别名命名空间 */
int lm_is_module_alias(const char* name);
// AST 构造辅助：报错定位用（节点行号 = 当前 lookahead 行）
static inline AstNode* l_set_line(AstNode* __n) { if(__n) __n->line = yylineno; return __n; }
#define L(n) l_set_line(n)

/* enum 自动递增值：未显式赋值的成员取此值并自增；显式赋值会重置此值 */
static long long g_enum_next_val = 0;

/* enum 字面量表：编译期存储 enum 名 → map_lit AST，用于在 case Enum.MEMBER
 * 上下文中编译期求值为对应字面量值（如 case Op.ADD → case 1），避免函数内
 * 无法访问全局 enum 变量的作用域限制。 */
typedef struct EnumTableEntry {
    char* name;
    AstNode* map_lit;
    struct EnumTableEntry* next;
} EnumTableEntry;
static EnumTableEntry* g_enum_table = NULL;

static void enum_table_register(const char* name, AstNode* map_lit) {
    /* 重复注册则更新 map_lit */
    EnumTableEntry* e;
    for(e = g_enum_table; e; e = e->next) {
        if(strcmp(e->name, name) == 0) { e->map_lit = map_lit; return; }
    }
    e = (EnumTableEntry*)malloc(sizeof(EnumTableEntry));
    e->name = strdup(name);
    e->map_lit = map_lit;
    e->next = g_enum_table;
    g_enum_table = e;
}

/* 在 enum_name 的 map 字面量中查找 member_name 对应的 value，返回其克隆节点；
 * map 字面量结构：entries 是 AST_SEQ 链表，每项 first 是 AST_MAP_ENTRY，key/value 分别在 u.map_entry.key/value */
static AstNode* enum_table_lookup_member(const char* enum_name, const char* member_name) {
    EnumTableEntry* e;
    for(e = g_enum_table; e; e = e->next) {
        if(strcmp(e->name, enum_name) == 0 && e->map_lit) {
            /* enum_members 规则: enum_members COMMA enum_member → ast_seq($1, $3)
             * 即 AST_SEQ{first=之前列表, second=当前 entry}，链表是 left-leaning
             * 遍历方向：取 second 作当前 entry，first 作 next 指针 */
            AstNode* p = e->map_lit->u.map_lit.entries;
            while(p) {
                AstNode* entry = NULL;
                if(p->type == AST_SEQ) {
                    entry = p->u.seq.second;
                    p = p->u.seq.first;
                } else if(p->type == AST_MAP_ENTRY) {
                    entry = p;
                    p = NULL;
                } else {
                    break;
                }
                if(entry && entry->type == AST_MAP_ENTRY) {
                    AstNode* key = entry->u.map_entry.key;
                    if(key && key->type == AST_STRING && strcmp(key->u.sval, member_name) == 0) {
                        return ast_clone_node(entry->u.map_entry.value);
                    }
                }
            }
            return NULL;
        }
    }
    return NULL;
}
%}

%union {
    double d;
    long long ll;
    char ch;
    char* s;
    AstNode* node;
}

%token PRINT ID NUMBER INTEGER BIG_INTEGER PLUS MINUS MUL DIV ASSIGN SEMI LPAREN RPAREN
%token<s> BIG_INTEGER
%token<s> BIG_DECIMAL
%token TRUE FALSE NULL_LIT STRING_LIT FSTRING_LIT MAP_OPEN
%token IF ELSEIF ELSE
%token GE LE EQ NE GT LT
%token LBRACE RBRACE
%token WHILE FOR TOK_DO TOK_IN TOK_ITER
%token TOK_CHAR_LIT
%token TOK_INT TOK_DOUBLE TOK_CHAR TOK_STRING TOK_BOOL TOK_ASCII TOK_BYTE
%token TOK_INT8 TOK_INT16 TOK_INT32 TOK_INT64 TOK_UINT8 TOK_UINT16 TOK_UINT32 TOK_UINT64 TOK_UINT TOK_LONG TOK_LONGLONG TOK_FLOAT TOK_ULONG TOK_UCHAR TOK_SHORT TOK_USHORT TOK_SIZE_T TOK_SSIZE_T TOK_VOID TOK_LONG_DOUBLE TOK_PTR
%token<ll> TOK_TYPE_ANNOT   /* 类型标注 <type>：词法层面整体匹配，值为 CastKind 枚举 */
%token TOK_TYPE TOK_STRUCT TOK_ENUM TOK_INTERFACE TOK_IMPLEMENTS TOK_EXTENDS TOK_EXTEND TOK_UNPACK TOK_CLASS TOK_SUPER TOK_STATIC TOK_ABSTRACT TOK_PUBLIC TOK_PRIVATE TOK_PROTECTED
%token PLUSPLUS MINUSMINUS
%token QMARK COLON CASE_COLON
%token ARROW  /* => 箭头函数 */
%token SWITCH CASE DEFAULT BREAK RETURN TRY CATCH THROW FINALLY
%token CONTINUE
%token FUNC ELLIPSIS TOK_AT SAFE_CALL NULL_COALESCE CONST MACRO TOK_GEN TOK_YIELD TOK_EXTERN TOK_REF
%token TOK_ASSERT TOK_DEFER
%token READ WRITE
%token COMMA
%token AND OR NOT MOD
%token BAND BOR BXOR BNOT SHL SHR   /* 位运算：& | ^ ~ << >> */
%token POW                          /* 中缀幂 ** */
%token PLUSEQ MINUSEQ MULEQ DIVEQ MODEQ
%token BANDEQ BOREQ BXOREQ SHLEQ SHREQ  /* 位复合赋值：&= |= ^= <<= >>= */
%token LBRACKET RBRACKET
%token ARRAY_OPEN
%token DOT
%token ERROR

%right PLUSPLUS MINUSMINUS   /*后置自增，最高优先级*/
%left GT LT GE LE EQ NE       /*比较运算符，优先级低于加法（与C语言一致）*/
%left PLUS MINUS              /*加法*/
%left MUL DIV MOD             /*乘法*/
%left AND
%left OR
%left NULL_COALESCE
%right QMARK COLON   /*三元 ?: 右结合，低于比较*/
%right ASSIGN        /*赋值最低*/
%precedence ELSE

%type<node> program stmt_list closed_stmt open_stmt block_stmt try_stmt
%type<node> elif_clause_list elif_clause else_part
%type<node> expr ternary_expr logic_or_expr logic_and_expr assignment_expr unary_expr power_expr postfix_expr multiplicative_expr additive_expr comparison_expr shift_expr equality_expr bit_and_expr bit_xor_expr bit_or_expr expr_opt for_init for_incr primary map_items map_item
%type<node> switch_stmt case_list case_item break_stmt continue_stmt assert_stmt defer_stmt const_expr return_stmt yield_stmt expr_list
%type<node> catch_clause_list catch_clause
%type<s> opt_catch_type
%type<node> func_def func_def_list param_list param arg_list arg destruct_lhs type_prop_list type_prop struct_prop_list struct_prop class_prop_list class_prop class_start class_header class_header_inherit class_header_implements class_header_inherit_implements abstract_class_header enum_members enum_member annotation annotation_list macro_def generic_param_list generic_param_items opt_generic_param_list interface_methods interface_method interface_list unpack_obj_pattern unpack_arr_pattern unpack_name_list struct_header annotated_decl
%type<ll> type_name builtin_type_name type_keyword access_modifier map_generic_type
%type<s> type_name_str
%type <ch> char_lit
%type<ll> INTEGER
%type<d> NUMBER
%type<s> ID STRING_LIT FSTRING_LIT operator

%%

program
    : stmt_list { $$ = $1; root = $$; }
    ;

stmt_list
    : %empty                   { $$ = NULL; }
    | stmt_list closed_stmt    { $$ = ast_seq($1, $2); }
    /* unpack 解构赋值：展开为多个独立赋值语句，直接加入 stmt_list */
    | stmt_list TOK_UNPACK unpack_obj_pattern ASSIGN expr SEMI {
        $$ = build_object_destruct($1, $3, $5);
    }
    | stmt_list TOK_UNPACK unpack_arr_pattern ASSIGN expr SEMI {
        $$ = build_array_destruct($1, $3, $5);
    }
    ;

closed_stmt
    : expr SEMI                      { $$ = $1; }
    /* 变量声明带类型标注：a: Type = expr 等价于 a = <Type>expr。
     * 必须在语句层而非 assignment_expr（见 assignment_expr 注释）：语句开头
     * ID 后 COLON 在此 shift 进声明；三元 ? ID : 中 ID 后 COLON 则归约为表达式，
     * COLON 作三元分隔符，两者不再共享冲突状态。 */
    | ID COLON map_generic_type ASSIGN expr SEMI {
          /* 非空 T：写入前做非空断言（T? 规则不做） */
          $$ = ast_assign($1, ast_type_annotation((CastKind)$3,
                    ast_unary(OP_NONNULL_ASSERT, $5)));
      }
    /* 可空局部声明 a: T? = expr：变量可持 null 或 T，须落 VALUE(动态)槽，
     * 不做静态类型标注（否则 null 被 UNBOX 成 0）；语义等同无标注赋值。 */
    | ID COLON map_generic_type QMARK ASSIGN expr SEMI {
          $$ = ast_assign($1, $6);
      }
    /* const 常量声明：const x = expr / const x: Type = expr，初始化后不可重新赋值 */
    | CONST ID ASSIGN expr SEMI {
          $$ = ast_assign_const($2, $4);
      }
    | CONST ID COLON map_generic_type ASSIGN expr SEMI {
          $$ = ast_assign_const($2, ast_type_annotation((CastKind)$4,
                    ast_unary(OP_NONNULL_ASSERT, $6)));
      }
    | CONST ID COLON map_generic_type QMARK ASSIGN expr SEMI {
          $$ = ast_assign_const($2, $7);
      }
    | destruct_lhs ASSIGN expr SEMI {
        char** names = NULL; int cnt = 0;
        ast_collect_varnames($1, &names, &cnt);
        $$ = ast_destruct(names, cnt, $3);
    }
    | PRINT LPAREN arg_list RPAREN SEMI  { $$ = ast_print($3); }
    | block_stmt                     { $$ = $1; }
    /* 宏调用作为语句：如果是宏，则展开为语句列表；否则作为表达式语句 */
    | ID LPAREN arg_list RPAREN SEMI {
          if(macro_is_defined($1)) {
              AstNode* mdef = macro_lookup($1);
              $$ = macro_expand(mdef, $3);
          } else {
              $$ = L(ast_call($1, $3));
          }
      }
    | open_stmt                      { $$ = $1; }
    | WHILE LPAREN expr RPAREN closed_stmt     { $$ = ast_while($3, $5, NULL); }
    /* 标签循环 label: while(...) {...} —— 标签传给 ast_while，break/continue label 可跳转 */
    | ID COLON WHILE LPAREN expr RPAREN closed_stmt { $$ = ast_while($5, $7, $1); }
    | FOR LPAREN for_init SEMI expr_opt SEMI for_incr RPAREN closed_stmt { $$ = ast_for($3, $5, $7, $9, NULL); }
    | ID COLON FOR LPAREN for_init SEMI expr_opt SEMI for_incr RPAREN closed_stmt { $$ = ast_for($5, $7, $9, $11, $1); }
    /* for-each 循环：for x in obj，同时支持数组和生成器（运行时判断类型） */
    | FOR ID TOK_IN expr closed_stmt {
        static int fe_counter = 0;
        int n = fe_counter++;
        char oname[64], isarr_name[64], idx_name[64], len_name[64];
        snprintf(oname, sizeof(oname), "__fe_obj_%d", n);
        snprintf(isarr_name, sizeof(isarr_name), "__fe_isarr_%d", n);
        snprintf(idx_name, sizeof(idx_name), "__fe_idx_%d", n);
        snprintf(len_name, sizeof(len_name), "__fe_len_%d", n);
        const char* lbl = NULL;   /* for-in 不支持标签（其内部 desugar 出 while 无外部标签） */

        /* __fe_obj = obj; */
        AstNode* obj_assign = ast_assign(strdup(oname), ast_clone_node($4));
        /* __fe_isarr = (type(__fe_obj) == "array"); */
        AstNode* type_call = ast_call(strdup("type"), ast_seq(ast_var(strdup(oname)), NULL));
        AstNode* isarr_cond = ast_binop(OP_EQ, type_call, ast_string(strdup("array")));
        AstNode* isarr_assign = ast_assign(strdup(isarr_name), isarr_cond);
        /* __fe_idx = 0; __fe_len = 0; */
        AstNode* idx_init = ast_assign(strdup(idx_name), ast_int(0));
        AstNode* len_init = ast_assign(strdup(len_name), ast_int(0));
        /* if (__fe_isarr) { __fe_len = len(__fe_obj); } */
        AstNode* len_call = ast_call(strdup("len"), ast_seq(ast_var(strdup(oname)), NULL));
        AstNode* len_assign = ast_assign(strdup(len_name), len_call);
        AstNode* if_len = ast_if(ast_var(strdup(isarr_name)), ast_block(len_assign), NULL, NULL);

        /* while (1) { ... } */
        /* 数组分支：if (__fe_idx >= __fe_len) break; x = __fe_obj[__fe_idx]; __fe_idx++; */
        AstNode* arr_break_cond = ast_binop(OP_GE, ast_var(strdup(idx_name)), ast_var(strdup(len_name)));
        AstNode* arr_break = ast_if(arr_break_cond, ast_break(NULL), NULL, NULL);
        AstNode* arr_x_assign = ast_assign(strdup($2), ast_index(ast_var(strdup(oname)), ast_var(strdup(idx_name))));
        AstNode* arr_idx_inc = ast_unary(OP_POST_INC, ast_var(strdup(idx_name)));
        AstNode* arr_branch = ast_block(ast_seq(arr_break, ast_seq(arr_x_assign, arr_idx_inc)));

        /* 生成器分支：x = next(__fe_obj); if (x == null) break; */
        AstNode* gen_next_call = ast_call(strdup("next"), ast_seq(ast_var(strdup(oname)), NULL));
        AstNode* gen_x_assign = ast_assign(strdup($2), gen_next_call);
        AstNode* gen_break_cond = ast_binop(OP_EQ, ast_var(strdup($2)), ast_none());
        AstNode* gen_break = ast_if(gen_break_cond, ast_break(NULL), NULL, NULL);
        AstNode* gen_branch = ast_block(ast_seq(gen_x_assign, gen_break));

        /* if (__fe_isarr) { arr_branch } else { gen_branch } */
        AstNode* if_type = ast_if(ast_var(strdup(isarr_name)), arr_branch, gen_branch, NULL);

        /* while 循环体：if_type + 原始循环体 */
        AstNode* while_body = ast_block(ast_seq(if_type, $5));

        /* while (1) { while_body } */
        AstNode* while_loop = ast_while(ast_int(1), while_body, NULL);

        /* 整体：obj_assign; isarr_assign; idx_init; len_init; if_len; while_loop */
        AstNode* stmts = ast_seq(obj_assign, ast_seq(isarr_assign, ast_seq(idx_init, ast_seq(len_init, ast_seq(if_len, while_loop)))));
        $$ = ast_block(stmts);
    }
    | FOR ID COMMA ID TOK_IN expr closed_stmt {
        static int fe_counter2 = 0;
        char kname[64], iname[64];
        snprintf(kname, sizeof(kname), "__fe_keys_%d", fe_counter2);
        snprintf(iname, sizeof(iname), "__fe_i_%d", fe_counter2++);
        AstNode* iter_copy = ast_clone_node($6);
        AstNode* keys_init = ast_assign(strdup(kname),
            ast_call(strdup("keys"), ast_seq(ast_clone_node($6), NULL)));
        AstNode* init = ast_assign(strdup(iname), ast_int(0));
        AstNode* cond = ast_binop(OP_LT, ast_var(strdup(iname)),
            ast_call(strdup("len"), ast_seq(ast_var(strdup(kname)), NULL)));
        AstNode* update = ast_unary(OP_POST_INC, ast_var(strdup(iname)));
        AstNode* k_assign = ast_assign(strdup($2), ast_index(ast_var(strdup(kname)), ast_var(strdup(iname))));
        AstNode* v_assign = ast_assign(strdup($4), ast_index(iter_copy, ast_var(strdup($2))));
        AstNode* body = ast_block(ast_seq(k_assign, ast_seq(v_assign, $7)));
        AstNode* for_stmt = ast_for(init, cond, update, body, NULL);
        $$ = ast_block(ast_seq(keys_init, for_stmt));
    }
    /* 迭代器协议：for x iter obj，调用 obj.next()，返回 null 结束 */
    | FOR ID TOK_ITER expr closed_stmt {
        static int iter_counter = 0;
        char oname[64]; snprintf(oname, sizeof(oname), "__iter_obj_%d", iter_counter++);
        /* __iter_obj_N = obj; */
        AstNode* obj_assign = ast_assign(strdup(oname), ast_clone_node($4));
        /* next(__iter_obj_N) */
        AstNode* next_call = ast_call(strdup("next"), ast_seq(ast_var(strdup(oname)), NULL));
        /* x = next(__iter_obj_N) */
        AstNode* x_assign = ast_assign(strdup($2), next_call);
        /* if (x == null) break; */
        AstNode* null_cond = ast_binop(OP_EQ, ast_var(strdup($2)), ast_none());
        AstNode* break_stmt = ast_break(NULL);
        AstNode* if_break = ast_if(null_cond, break_stmt, NULL, NULL);
        /* body: x = next(...); if (x == null) break; <original body> */
        AstNode* loop_body = ast_block(ast_seq(x_assign, ast_seq(if_break, $5)));
        /* while (1) { body } */
        AstNode* while_stmt = ast_while(ast_int(1), loop_body, NULL);
        /* __iter_obj_N = obj; while (1) { ... } */
        $$ = ast_block(ast_seq(obj_assign, while_stmt));
    }

    | TOK_DO closed_stmt WHILE LPAREN expr RPAREN SEMI { $$ = ast_do_while($5, $2, NULL); }
    | ID COLON TOK_DO closed_stmt WHILE LPAREN expr RPAREN SEMI { $$ = ast_do_while($7, $4, $1); }
    | switch_stmt                    { $$ = $1; }
    | break_stmt                     { $$ = $1; }
    | continue_stmt                  { $$ = $1; }
    | assert_stmt                    { $$ = $1; }
    | defer_stmt                     { $$ = $1; }
    | return_stmt                    { $$ = $1; }
    | yield_stmt                     { $$ = $1; }
    | func_def                       { $$ = $1; }          /* 新增函数定义语句 */
    | macro_def                      { $$ = $1; }          /* 宏定义语句 */
    | WRITE STRING_LIT expr SEMI {
          /* write "path" value → write_file(path, value)；普通路径不内插 */
          AstNode* p = ast_string($2);
          free($2);
          $$ = L(ast_call(strdup("write_file"), ast_seq(p, $3)));
      }
    | WRITE FSTRING_LIT expr SEMI {
          /* write f"path" value → write_file(path, value)；f 前缀路径支持模板内插 */
          AstNode* p = L(maybe_template($2));
          free($2);
          $$ = L(ast_call(strdup("write_file"), ast_seq(p, $3)));
      }
    | try_stmt { $$ = $1; }
    | THROW expr SEMI {
          /* throw expr：显式抛错；值在运行时包装成错误对象 */
          $$ = L(ast_throw($2));
      }
    | TOK_TYPE opt_generic_param_list ID LBRACE type_prop_list RBRACE {
          /* type Person { ... }：编译期注册形状（无接口实现） */
          char** gnames = NULL;
          int gcnt = 0;
          AstNode* gp = $2;
          while(gp) { gcnt++; gp = gp->u.param.next; }
          if(gcnt > 0) {
              gnames = (char**)malloc((size_t)gcnt * sizeof(char*));
              int gi = 0;
              gp = $2;
              while(gp) { gnames[gi++] = strdup(gp->u.param.name); gp = gp->u.param.next; }
          }
          type_register($3, g_prop_names, g_prop_types, g_prop_n, gnames, gcnt, NULL, 0);
          if(gnames) { for(int gi = 0; gi < gcnt; gi++) free(gnames[gi]); free(gnames); }
          type_prop_clear();
          free($3);
          $$ = L(ast_none());
      }
    | TOK_TYPE opt_generic_param_list ID TOK_IMPLEMENTS interface_list LBRACE type_prop_list RBRACE {
          /* type Dog implements Printable { ... }：编译期注册形状（带接口实现） */
          char** gnames = NULL;
          int gcnt = 0;
          AstNode* gp = $2;
          while(gp) { gcnt++; gp = gp->u.param.next; }
          if(gcnt > 0) {
              gnames = (char**)malloc((size_t)gcnt * sizeof(char*));
              int gi = 0;
              gp = $2;
              while(gp) { gnames[gi++] = strdup(gp->u.param.name); gp = gp->u.param.next; }
          }
          /* 解析接口列表 */
          char** ifaces = NULL;
          int nifaces = 0;
          AstNode* ip = $5;
          while(ip) { nifaces++; ip = ip->u.param.next; }
          if(nifaces > 0) {
              ifaces = (char**)malloc((size_t)nifaces * sizeof(char*));
              int ii = 0;
              ip = $5;
              while(ip) { ifaces[ii++] = strdup(ip->u.param.name); ip = ip->u.param.next; }
          }
          type_register($3, g_prop_names, g_prop_types, g_prop_n, gnames, gcnt, ifaces, nifaces);
          if(gnames) { for(int gi = 0; gi < gcnt; gi++) free(gnames[gi]); free(gnames); }
          if(ifaces) { for(int ii = 0; ii < nifaces; ii++) free(ifaces[ii]); free(ifaces); }
          type_prop_clear();
          free($3);
          $$ = L(ast_none());
      }
    | struct_header struct_prop_list RBRACE {
          /* struct Point { x: int, y: int, func dist(): int {...} }：编译期注册 struct 类型 */
          /* g_current_struct_name 已在 struct_header 中设置 */
          struct_register(g_current_struct_name, g_struct_prop_names, g_struct_cast_kinds, g_struct_prop_struct_names, g_struct_prop_n);
          /* struct_register 已完成，struct_lookup 现在可用；
           * struct_add_method 内部统一完成：self 约束、字节码编译（唯一内部名）、
           * TypeDef + RuntimeTypeInfo 方法表登记 */
          for(int mi = 0; mi < g_struct_method_n; mi++) {
              AstNode* mnode = g_struct_methods[mi];
              if(mnode && mnode->type == AST_FUNC_DEF) {
                  struct_add_method(g_current_struct_name, mnode->u.func_def.name, mnode);
              }
          }
          g_struct_method_clear();
          struct_prop_clear();
          g_current_struct_name = NULL;
          $$ = L(ast_none());
      }
    | annotation_list class_header class_prop_list RBRACE {
          /* @annotation class Point { ... }：带注解的 class 定义（无继承） */
          /* 注解暂时保存，后续可扩展语义处理 */
          char* saved_class_name = g_current_class_name;
          class_register(g_current_class_name, g_prop_names, g_prop_types, g_prop_access_modifiers, g_prop_const_flags, g_prop_struct_names, g_prop_n, NULL, NULL);
          if(g_current_class_is_abstract) {
              TypeDef* td = type_lookup(g_current_class_name);
              if(td) td->is_abstract = 1;
          }
          /* 类级注解登记 + @Data 访问器生成；字段初始化器注入 ctor */
          register_class_annotations($1);
          apply_accessors(annotation_list_has($1, "Data"));
          if(apply_field_initializers()) YYABORT;
          for(int mi = 0; mi < g_class_method_n; mi++) {
              AstNode* mnode = g_class_methods[mi];
              if(mnode && mnode->type == AST_FUNC_DEF && !mnode->u.func_def.is_static_method) {
                  class_add_method(g_current_class_name, mnode->u.func_def.name, mnode);
              }
          }
          if(g_class_constructor && g_class_constructor->type == AST_FUNC_DEF) {
              RuntimeFunc* ctor_rf = compile_func_from_ast(g_class_constructor);
              class_set_constructor(g_current_class_name, g_class_constructor, ctor_rf);
          }
          AstNode* method_list = NULL;
          for(int mi = 0; mi < g_class_method_n; mi++) {
              AstNode* mnode = g_class_methods[mi];
              if(mnode && mnode->type == AST_ASSIGN) {
                  /* 静态属性：作为顶层节点执行初始化（全局名 Class_prop） */
                  method_list = method_list ? ast_seq(method_list, mnode) : mnode;
              } else if(mnode && mnode->type == AST_FUNC_DEF && mnode->u.func_def.is_static_method) {
                  g_current_class_name = NULL;
                  RuntimeFunc* rf = compile_func_from_ast(mnode);
                  if(rf) {
                      Value fv;
                      fv.type = VAL_FUNC;
                      fv.v.func.ffi_func = NULL;
                      fv.v.func.is_ffi = 0;
                      fv.v.func.func_obj = (void*)rf;
                      sym_set(mnode->u.func_def.name, fv);
                  }
                  g_current_class_name = saved_class_name;
              } else {
                  /* 非静态方法已通过 class_add_method 编译，不加入 method_list 避免重复编译 */
              }
          }
          g_class_method_clear();
          type_prop_clear();
          g_current_class_name = NULL;
          $$ = method_list ? L(method_list) : L(ast_none());
      }
    | class_header class_prop_list RBRACE {
          /* class Point { x: int, y: int, func dist(): int {...} }：编译期注册 class 类型（无继承） */
          char* saved_class_name = g_current_class_name;
          class_register(g_current_class_name, g_prop_names, g_prop_types, g_prop_access_modifiers, g_prop_const_flags, g_prop_struct_names, g_prop_n, NULL, NULL);
          apply_accessors(0);   /* 无类级 @Data：字段级 @Getter/@Setter 仍独立生效 */
          /* 标记是否是抽象类 */
          if(g_current_class_is_abstract) {
              TypeDef* td = type_lookup(g_current_class_name);
              if(td) td->is_abstract = 1;
          }
          /* 字段初始化器注入 ctor */
          if(apply_field_initializers()) YYABORT;
          /* 添加方法到 class 方法表（静态方法不加入） */
          for(int mi = 0; mi < g_class_method_n; mi++) {
              AstNode* mnode = g_class_methods[mi];
              if(mnode && mnode->type == AST_FUNC_DEF && !mnode->u.func_def.is_static_method) {
                  class_add_method(g_current_class_name, mnode->u.func_def.name, mnode);
              }
          }
          /* 保存构造函数（__init__ 方法）到 TypeDef */
          if(g_class_constructor && g_class_constructor->type == AST_FUNC_DEF) {
              RuntimeFunc* ctor_rf = compile_func_from_ast(g_class_constructor);
              class_set_constructor(g_current_class_name, g_class_constructor, ctor_rf);
          }
          /* 把静态属性与静态方法的 AST 节点保存到临时顶层列表 */
          AstNode* method_list = NULL;
          for(int mi = 0; mi < g_class_method_n; mi++) {
              AstNode* mnode = g_class_methods[mi];
              if(mnode && mnode->type == AST_ASSIGN) {
                  /* 静态属性：作为顶层节点执行初始化（全局名 Class_prop） */
                  method_list = method_list ? ast_seq(method_list, mnode) : mnode;
              } else if(mnode && mnode->type == AST_FUNC_DEF && mnode->u.func_def.is_static_method) {
                  /* 静态方法：直接编译并注册到全局符号表 */
                  g_current_class_name = NULL;  // 临时设置为 NULL，以便注册到全局符号表
                  RuntimeFunc* rf = compile_func_from_ast(mnode);
                  if(rf) {
                      Value fv;
                      fv.type = VAL_FUNC;
                      fv.v.func.ffi_func = NULL;
                      fv.v.func.is_ffi = 0;
                      fv.v.func.func_obj = (void*)rf;
                      sym_set(mnode->u.func_def.name, fv);
                  }
                  g_current_class_name = saved_class_name;  // 恢复
              } else {
                  /* 非静态方法已经通过 class_add_method 编译了，不加入 method_list 避免重复编译 */
              }
          }
          g_class_method_clear();
          type_prop_clear();
          g_current_class_name = NULL;
          $$ = method_list ? L(method_list) : L(ast_none());
      }
    | annotation_list class_header_inherit class_prop_list RBRACE {
          /* @annotation class Point extends Shape { ... }：带注解的 class 定义（带继承） */
          char* saved_class_name = g_current_class_name;
          class_register(g_current_class_name, g_prop_names, g_prop_types, g_prop_access_modifiers, g_prop_const_flags, g_prop_struct_names, g_prop_n, g_current_class_parent, NULL);
          /* 类级注解登记 + @Data 访问器生成；字段初始化器注入 ctor */
          register_class_annotations($1);
          apply_accessors(annotation_list_has($1, "Data"));
          if(apply_field_initializers()) YYABORT;
          for(int mi = 0; mi < g_class_method_n; mi++) {
              AstNode* mnode = g_class_methods[mi];
              if(mnode && mnode->type == AST_FUNC_DEF && !mnode->u.func_def.is_static_method) {
                  class_add_method(g_current_class_name, mnode->u.func_def.name, mnode);
              }
          }
          if(g_class_constructor && g_class_constructor->type == AST_FUNC_DEF) {
              RuntimeFunc* ctor_rf = compile_func_from_ast(g_class_constructor);
              class_set_constructor(g_current_class_name, g_class_constructor, ctor_rf);
          }
          AstNode* method_list = NULL;
          for(int mi = 0; mi < g_class_method_n; mi++) {
              AstNode* mnode = g_class_methods[mi];
              if(mnode && mnode->type == AST_ASSIGN) {
                  /* 静态属性：作为顶层节点执行初始化（全局名 Class_prop） */
                  method_list = method_list ? ast_seq(method_list, mnode) : mnode;
              } else if(mnode && mnode->type == AST_FUNC_DEF && mnode->u.func_def.is_static_method) {
                  /* 静态方法：编译并注册到全局符号表 */
                  g_current_class_name = NULL;
                  RuntimeFunc* rf = compile_func_from_ast(mnode);
                  if(rf) {
                      Value fv;
                      fv.type = VAL_FUNC;
                      fv.v.func.ffi_func = NULL;
                      fv.v.func.is_ffi = 0;
                      fv.v.func.func_obj = (void*)rf;
                      sym_set(mnode->u.func_def.name, fv);
                  }
                  g_current_class_name = saved_class_name;
              }
          }
          g_class_method_clear();
          type_prop_clear();
          g_current_class_name = NULL;
          $$ = method_list ? L(method_list) : L(ast_none());
      }
    | class_header_inherit class_prop_list RBRACE {
          /* class Point extends Shape { ... }：编译期注册 class 类型（带继承） */
          char* saved_class_name = g_current_class_name;
          class_register(g_current_class_name, g_prop_names, g_prop_types, g_prop_access_modifiers, g_prop_const_flags, g_prop_struct_names, g_prop_n, g_current_class_parent, NULL);
          apply_accessors(0);   /* 无类级 @Data：字段级 @Getter/@Setter 仍独立生效 */
          /* 字段初始化器注入 ctor */
          if(apply_field_initializers()) YYABORT;
          /* 添加方法到 class 方法表（静态方法不加入） */
          for(int mi = 0; mi < g_class_method_n; mi++) {
              AstNode* mnode = g_class_methods[mi];
              if(mnode && mnode->type == AST_FUNC_DEF && !mnode->u.func_def.is_static_method) {
                  class_add_method(g_current_class_name, mnode->u.func_def.name, mnode);
              }
          }
          /* 保存构造函数（__init__ 方法）到 TypeDef */
          if(g_class_constructor && g_class_constructor->type == AST_FUNC_DEF) {
              RuntimeFunc* ctor_rf = compile_func_from_ast(g_class_constructor);
              class_set_constructor(g_current_class_name, g_class_constructor, ctor_rf);
          }
          /* 收集需在顶层执行的初始化节点：静态属性 AST_ASSIGN；静态方法在此编译 */
          AstNode* method_list = NULL;
          for(int mi = 0; mi < g_class_method_n; mi++) {
              AstNode* mnode = g_class_methods[mi];
              if(mnode && mnode->type == AST_ASSIGN) {
                  /* 静态属性：作为顶层节点执行初始化（全局名 Class_prop） */
                  method_list = method_list ? ast_seq(method_list, mnode) : mnode;
              } else if(mnode && mnode->type == AST_FUNC_DEF && mnode->u.func_def.is_static_method) {
                  /* 静态方法：编译并注册到全局符号表 */
                  g_current_class_name = NULL;
                  RuntimeFunc* rf = compile_func_from_ast(mnode);
                  if(rf) {
                      Value fv;
                      fv.type = VAL_FUNC;
                      fv.v.func.ffi_func = NULL;
                      fv.v.func.is_ffi = 0;
                      fv.v.func.func_obj = (void*)rf;
                      sym_set(mnode->u.func_def.name, fv);
                  }
                  g_current_class_name = saved_class_name;
              } else {
                  /* 非静态方法已通过 class_add_method 编译，不加入 method_list 避免重复编译 */
              }
          }
          g_class_method_clear();
          type_prop_clear();
          g_current_class_name = NULL;
          g_current_class_parent = NULL;
          $$ = method_list ? L(method_list) : L(ast_none());
      }
    | annotation_list class_header_implements class_prop_list RBRACE {
          /* @annotation class Point implements Printable { ... }：带注解的 class 定义（带接口实现） */
          char* saved_class_name = g_current_class_name;
          class_register(g_current_class_name, g_prop_names, g_prop_types, g_prop_access_modifiers, g_prop_const_flags, g_prop_struct_names, g_prop_n, NULL, g_class_interfaces);
          /* 类级注解登记 + @Data 访问器生成；字段初始化器注入 ctor */
          register_class_annotations($1);
          apply_accessors(annotation_list_has($1, "Data"));
          if(apply_field_initializers()) YYABORT;
          for(int mi = 0; mi < g_class_method_n; mi++) {
              AstNode* mnode = g_class_methods[mi];
              if(mnode && mnode->type == AST_FUNC_DEF && !mnode->u.func_def.is_static_method) {
                  class_add_method(g_current_class_name, mnode->u.func_def.name, mnode);
              }
          }
          if(g_class_constructor && g_class_constructor->type == AST_FUNC_DEF) {
              RuntimeFunc* ctor_rf = compile_func_from_ast(g_class_constructor);
              class_set_constructor(g_current_class_name, g_class_constructor, ctor_rf);
          }
          AstNode* method_list2 = NULL;
          for(int mi = 0; mi < g_class_method_n; mi++) {
              AstNode* mnode = g_class_methods[mi];
              if(mnode && mnode->type == AST_ASSIGN) {
                  /* 静态属性：作为顶层节点执行初始化（全局名 Class_prop） */
                  method_list2 = method_list2 ? ast_seq(method_list2, mnode) : mnode;
              } else if(mnode && mnode->type == AST_FUNC_DEF && mnode->u.func_def.is_static_method) {
                  /* 静态方法：编译并注册到全局符号表 */
                  g_current_class_name = NULL;
                  RuntimeFunc* rf = compile_func_from_ast(mnode);
                  if(rf) {
                      Value fv;
                      fv.type = VAL_FUNC;
                      fv.v.func.ffi_func = NULL;
                      fv.v.func.is_ffi = 0;
                      fv.v.func.func_obj = (void*)rf;
                      sym_set(mnode->u.func_def.name, fv);
                  }
                  g_current_class_name = saved_class_name;
              }
          }
          for(int ii = 0; ii < g_class_ninterfaces; ii++) {
              class_check_interface_implementation(g_current_class_name, g_class_interfaces[ii]);
          }
          g_class_method_clear();
          type_prop_clear();
          g_current_class_name = NULL;
          g_current_class_parent = NULL;
          for(int ii = 0; ii < g_class_ninterfaces; ii++) free(g_class_interfaces[ii]);
          free(g_class_interfaces);
          g_class_interfaces = NULL;
          g_class_ninterfaces = 0;
          $$ = method_list2 ? L(method_list2) : L(ast_none());
      }
    | class_header_implements class_prop_list RBRACE {
          /* class Point implements Printable { ... }：编译期注册 class 类型（带接口实现） */
          char* saved_class_name = g_current_class_name;
          class_register(g_current_class_name, g_prop_names, g_prop_types, g_prop_access_modifiers, g_prop_const_flags, g_prop_struct_names, g_prop_n, NULL, g_class_interfaces);
          apply_accessors(0);   /* 无类级 @Data：字段级 @Getter/@Setter 仍独立生效 */
          /* 字段初始化器注入 ctor */
          if(apply_field_initializers()) YYABORT;
          /* 添加方法到 class 方法表（静态方法不加入） */
          for(int mi = 0; mi < g_class_method_n; mi++) {
              AstNode* mnode = g_class_methods[mi];
              if(mnode && mnode->type == AST_FUNC_DEF && !mnode->u.func_def.is_static_method) {
                  class_add_method(g_current_class_name, mnode->u.func_def.name, mnode);
              }
          }
          /* 保存构造函数（__init__ 方法）到 TypeDef */
          if(g_class_constructor && g_class_constructor->type == AST_FUNC_DEF) {
              RuntimeFunc* ctor_rf = compile_func_from_ast(g_class_constructor);
              class_set_constructor(g_current_class_name, g_class_constructor, ctor_rf);
          }
          /* 把方法定义的 AST 节点保存到临时列表（包括构造函数） */
          AstNode* method_list2 = NULL;
          for(int mi = 0; mi < g_class_method_n; mi++) {
              AstNode* mnode = g_class_methods[mi];
              if(mnode && mnode->type == AST_ASSIGN) {
                  /* 静态属性：作为顶层节点执行初始化（全局名 Class_prop） */
                  method_list2 = method_list2 ? ast_seq(method_list2, mnode) : mnode;
              } else if(mnode && mnode->type == AST_FUNC_DEF && mnode->u.func_def.is_static_method) {
                  /* 静态方法：编译并注册到全局符号表 */
                  g_current_class_name = NULL;
                  RuntimeFunc* rf = compile_func_from_ast(mnode);
                  if(rf) {
                      Value fv;
                      fv.type = VAL_FUNC;
                      fv.v.func.ffi_func = NULL;
                      fv.v.func.is_ffi = 0;
                      fv.v.func.func_obj = (void*)rf;
                      sym_set(mnode->u.func_def.name, fv);
                  }
                  g_current_class_name = saved_class_name;
              }
          }
          /* 接口方法检查：检查 class 是否实现了接口中定义的所有方法 */
          for(int ii = 0; ii < g_class_ninterfaces; ii++) {
              class_check_interface_implementation(g_current_class_name, g_class_interfaces[ii]);
          }
          g_class_method_clear();
          type_prop_clear();
          g_current_class_name = NULL;
          g_current_class_parent = NULL;
          /* 释放接口名列表 */
          for(int ii = 0; ii < g_class_ninterfaces; ii++) free(g_class_interfaces[ii]);
          free(g_class_interfaces);
          g_class_interfaces = NULL;
          g_class_ninterfaces = 0;
          $$ = method_list2 ? L(method_list2) : L(ast_none());
      }
    | abstract_class_header class_prop_list RBRACE {
          /* abstract class Shape { ... }：抽象类定义（无继承） */
          char* saved_class_name = g_current_class_name;
          class_register(g_current_class_name, g_prop_names, g_prop_types, g_prop_access_modifiers, g_prop_const_flags, g_prop_struct_names, g_prop_n, NULL, NULL);
          apply_accessors(0);   /* 无类级 @Data：字段级 @Getter/@Setter 仍独立生效 */
          /* 标记为抽象类 */
          TypeDef* td = type_lookup(g_current_class_name);
          if(td) td->is_abstract = 1;
          /* 字段初始化器注入 ctor */
          if(apply_field_initializers()) YYABORT;
          /* 添加方法到 class 方法表（静态方法不加入） */
          for(int mi = 0; mi < g_class_method_n; mi++) {
              AstNode* mnode = g_class_methods[mi];
              if(mnode && mnode->type == AST_FUNC_DEF && !mnode->u.func_def.is_static_method) {
                  class_add_method(g_current_class_name, mnode->u.func_def.name, mnode);
              }
          }
          /* 保存构造函数（__init__ 方法）到 TypeDef */
          if(g_class_constructor && g_class_constructor->type == AST_FUNC_DEF) {
              RuntimeFunc* ctor_rf = compile_func_from_ast(g_class_constructor);
              class_set_constructor(g_current_class_name, g_class_constructor, ctor_rf);
          }
          /* 把方法定义的 AST 节点保存到临时列表（包括构造函数） */
          AstNode* abs_method_list = NULL;
          for(int mi = 0; mi < g_class_method_n; mi++) {
              AstNode* mnode = g_class_methods[mi];
              if(mnode && mnode->type == AST_ASSIGN) {
                  /* 静态属性：作为顶层节点执行初始化（全局名 Class_prop） */
                  abs_method_list = abs_method_list ? ast_seq(abs_method_list, mnode) : mnode;
              } else if(mnode && mnode->type == AST_FUNC_DEF && mnode->u.func_def.is_static_method) {
                  /* 静态方法：编译并注册到全局符号表 */
                  g_current_class_name = NULL;
                  RuntimeFunc* rf = compile_func_from_ast(mnode);
                  if(rf) {
                      Value fv;
                      fv.type = VAL_FUNC;
                      fv.v.func.ffi_func = NULL;
                      fv.v.func.is_ffi = 0;
                      fv.v.func.func_obj = (void*)rf;
                      sym_set(mnode->u.func_def.name, fv);
                  }
                  g_current_class_name = saved_class_name;
              }
          }
          g_class_method_clear();
          type_prop_clear();
          g_current_class_name = NULL;
          g_current_class_is_abstract = 0;
          $$ = abs_method_list ? L(abs_method_list) : L(ast_none());
      }
    | annotation_list class_header_inherit_implements class_prop_list RBRACE {
          /* @annotation class Point extends Shape implements Printable { ... }：带注解的 class 定义（带继承和接口实现） */
          char* saved_class_name = g_current_class_name;
          class_register(g_current_class_name, g_prop_names, g_prop_types, g_prop_access_modifiers, g_prop_const_flags, g_prop_struct_names, g_prop_n, g_current_class_parent, g_class_interfaces);
          /* 类级注解登记 + @Data 访问器生成；字段初始化器注入 ctor */
          register_class_annotations($1);
          apply_accessors(annotation_list_has($1, "Data"));
          if(apply_field_initializers()) YYABORT;
          for(int mi = 0; mi < g_class_method_n; mi++) {
              AstNode* mnode = g_class_methods[mi];
              if(mnode && mnode->type == AST_FUNC_DEF && !mnode->u.func_def.is_static_method) {
                  class_add_method(g_current_class_name, mnode->u.func_def.name, mnode);
              }
          }
          if(g_class_constructor && g_class_constructor->type == AST_FUNC_DEF) {
              RuntimeFunc* ctor_rf = compile_func_from_ast(g_class_constructor);
              class_set_constructor(g_current_class_name, g_class_constructor, ctor_rf);
          }
          AstNode* method_list3 = NULL;
          for(int mi = 0; mi < g_class_method_n; mi++) {
              AstNode* mnode = g_class_methods[mi];
              if(mnode && mnode->type == AST_ASSIGN) {
                  /* 静态属性：作为顶层节点执行初始化（全局名 Class_prop） */
                  method_list3 = method_list3 ? ast_seq(method_list3, mnode) : mnode;
              } else if(mnode && mnode->type == AST_FUNC_DEF && mnode->u.func_def.is_static_method) {
                  /* 静态方法：编译并注册到全局符号表 */
                  g_current_class_name = NULL;
                  RuntimeFunc* rf = compile_func_from_ast(mnode);
                  if(rf) {
                      Value fv;
                      fv.type = VAL_FUNC;
                      fv.v.func.ffi_func = NULL;
                      fv.v.func.is_ffi = 0;
                      fv.v.func.func_obj = (void*)rf;
                      sym_set(mnode->u.func_def.name, fv);
                  }
                  g_current_class_name = saved_class_name;
              }
          }
          for(int ii = 0; ii < g_class_ninterfaces; ii++) {
              class_check_interface_implementation(g_current_class_name, g_class_interfaces[ii]);
          }
          g_class_method_clear();
          type_prop_clear();
          g_current_class_name = NULL;
          g_current_class_parent = NULL;
          for(int ii = 0; ii < g_class_ninterfaces; ii++) free(g_class_interfaces[ii]);
          free(g_class_interfaces);
          g_class_interfaces = NULL;
          g_class_ninterfaces = 0;
          $$ = method_list3 ? L(method_list3) : L(ast_none());
      }
    | class_header_inherit_implements class_prop_list RBRACE {
          /* class Point extends Shape implements Printable { ... }：编译期注册 class 类型（带继承和接口实现） */
          char* saved_class_name = g_current_class_name;
          class_register(g_current_class_name, g_prop_names, g_prop_types, g_prop_access_modifiers, g_prop_const_flags, g_prop_struct_names, g_prop_n, g_current_class_parent, g_class_interfaces);
          apply_accessors(0);   /* 无类级 @Data：字段级 @Getter/@Setter 仍独立生效 */
          /* 字段初始化器注入 ctor */
          if(apply_field_initializers()) YYABORT;
          /* 添加方法到 class 方法表（静态方法不加入） */
          for(int mi = 0; mi < g_class_method_n; mi++) {
              AstNode* mnode = g_class_methods[mi];
              if(mnode && mnode->type == AST_FUNC_DEF && !mnode->u.func_def.is_static_method) {
                  class_add_method(g_current_class_name, mnode->u.func_def.name, mnode);
              }
          }
          /* 保存构造函数（__init__ 方法）到 TypeDef */
          if(g_class_constructor && g_class_constructor->type == AST_FUNC_DEF) {
              RuntimeFunc* ctor_rf = compile_func_from_ast(g_class_constructor);
              class_set_constructor(g_current_class_name, g_class_constructor, ctor_rf);
          }
          /* 把方法定义的 AST 节点保存到临时列表（包括构造函数） */
          AstNode* method_list3 = NULL;
          for(int mi = 0; mi < g_class_method_n; mi++) {
              AstNode* mnode = g_class_methods[mi];
              if(mnode && mnode->type == AST_ASSIGN) {
                  /* 静态属性：作为顶层节点执行初始化（全局名 Class_prop） */
                  method_list3 = method_list3 ? ast_seq(method_list3, mnode) : mnode;
              } else if(mnode && mnode->type == AST_FUNC_DEF && mnode->u.func_def.is_static_method) {
                  /* 静态方法：编译并注册到全局符号表 */
                  g_current_class_name = NULL;
                  RuntimeFunc* rf = compile_func_from_ast(mnode);
                  if(rf) {
                      Value fv;
                      fv.type = VAL_FUNC;
                      fv.v.func.ffi_func = NULL;
                      fv.v.func.is_ffi = 0;
                      fv.v.func.func_obj = (void*)rf;
                      sym_set(mnode->u.func_def.name, fv);
                  }
                  g_current_class_name = saved_class_name;
              }
          }
          /* 接口方法检查：检查 class 是否实现了接口中定义的所有方法 */
          for(int ii = 0; ii < g_class_ninterfaces; ii++) {
              class_check_interface_implementation(g_current_class_name, g_class_interfaces[ii]);
          }
          g_class_method_clear();
          type_prop_clear();
          g_current_class_name = NULL;
          g_current_class_parent = NULL;
          /* 释放接口名列表 */
          for(int ii = 0; ii < g_class_ninterfaces; ii++) free(g_class_interfaces[ii]);
          free(g_class_interfaces);
          g_class_interfaces = NULL;
          g_class_ninterfaces = 0;
          $$ = method_list3 ? L(method_list3) : L(ast_none());
      }
    | TOK_ENUM ID LBRACE {
          /* 进入 enum 体前重置自动递增计数器 */
          g_enum_next_val = 0;
      } enum_members RBRACE {
          /* enum Color { RED, GREEN } → Color = {"RED":0,"GREEN":1}
           * enum Op { ADD = 1, SUB, MUL, DIV } → Op = {"ADD":1, "SUB":2, "MUL":3, "DIV":4} */
          AstNode* ml = ast_map_lit($5);
          enum_table_register($2, ml);
          $$ = L(ast_assign($2, ml));
      }
    | TOK_INTERFACE ID LBRACE interface_methods RBRACE {
          /* interface Printable { func to_string(): string }：注册接口到符号表 */
          interface_register($2, $4, NULL);
          free($2);
          $$ = L(ast_none());
      }
    | TOK_INTERFACE ID TOK_EXTENDS ID LBRACE interface_methods RBRACE {
          /* interface Colored extends Printable { ... }：注册接口到符号表（带父接口） */
          interface_register($2, $6, $4);
          free($2);
          free($4);
          $$ = L(ast_none());
      }
    | TOK_EXTEND ID LBRACE func_def_list RBRACE {
          /* extend String { func a() { ... } func b() { ... } }：扩展方法 */
          /* 返回函数定义列表，让语义检查阶段能收集这些函数 */
          $$ = $4;
      }
    | TOK_EXTEND ID LBRACE RBRACE {
          /* extend String { }：扩展方法语法占位 */
          $$ = L(ast_none());
      }
    ;

/* 接口实现列表：Printable, Comparable */
interface_list : ID { $$ = ast_param($1, 0, NULL); }
               | interface_list COMMA ID { $$ = ast_param_append($1, ast_param($3, 0, NULL)); }
               ;

/* 接口方法签名列表 */
interface_methods : interface_method { $$ = $1; }
                  | interface_methods interface_method { $$ = ast_param_append($1, $2); }
                  ;

/* 接口方法签名：func name(params): return_type */
interface_method : FUNC ID LPAREN param_list RPAREN COLON type_name SEMI {
                      $$ = ast_param($2, 0, NULL);
                      /* 用 constraint 字段存储返回类型，简化实现 */
                      $$->u.param.constraint = valtype_to_name($7);
                  }
                  | FUNC ID LPAREN param_list RPAREN SEMI {
                      $$ = ast_param($2, 0, NULL);
                  }
                  ;

/* 运算符重载支持的运算符 */
operator : PLUS  { $$ = strdup("+"); }
         | MINUS { $$ = strdup("-"); }
         | MUL   { $$ = strdup("*"); }
         | DIV   { $$ = strdup("/"); }
         | MOD   { $$ = strdup("%%"); }
         | EQ    { $$ = strdup("=="); }
         | NE    { $$ = strdup("!="); }
         | LT    { $$ = strdup("<"); }
         | GT    { $$ = strdup(">"); }
         | LE    { $$ = strdup("<="); }
         | GE    { $$ = strdup(">="); }
         ;

/* 带注解的声明：统一处理所有声明类型的注解 */
annotated_decl:
    annotation_list func_def  {
        /* 带注解的函数定义 */
        $$ = $2;
        if($$ && $$->type == AST_FUNC_DEF) {
            $$->u.func_def.annotations = $1;
            /* 处理注解：注册到注解注册表，识别系统内置注解 */
            AstNode* ann = $1;
            while(ann) {
                if(ann->type == AST_ANNOTATION && ann->u.annotation.name) {
                    int type_marks = ANNOTATION_TYPE_FUNC;
                    if(g_current_class_name) {
                        type_marks |= ANNOTATION_TYPE_CLASS;
                    }
                    int category = annotation_is_system(ann->u.annotation.name) ? ANNOTATION_CATEGORY_SYSTEM : ANNOTATION_CATEGORY_USER;
                    annotation_register(ann->u.annotation.name, type_marks, category,
                                        ann->u.annotation.args,
                                        g_current_class_name, $$->u.func_def.name, NULL);
                    /* 识别系统内置注解 */
                    if(strcmp(ann->u.annotation.name, "abstract") == 0) {
                        $$->u.func_def.is_abstract_method = 1;
                    }
                    if(strcmp(ann->u.annotation.name, "override") == 0) {
                        $$->u.func_def.is_override_method = 1;
                    }
                }
                ann = ann->u.seq.second;
            }
        }
      }
    | func_def  {
        /* 不带注解的函数定义 */
        $$ = $1;
      }
    ;

func_def : FUNC TOK_TYPE_ANNOT ID LPAREN param_list RPAREN block_stmt {
          $$ = ast_func_def($3, $5, $7);
          $$->u.func_def.annotations = NULL;
          $$->u.func_def.ret_type_name = strdup(castkind_to_name($2));
          annotate_self_if_in_struct($$);
          /* 语义分析阶段：编译这个函数定义，生成RuntimeFunc，注册到全局符号 */
          RuntimeFunc* rf = NULL;
          if(!g_current_class_name && !g_current_struct_name) {
              /* 只有全局函数才在这里编译，class 方法在 class_add_method 中编译 */
              rf = compile_func_from_ast($$);
          }
          Value func_val = {0};
          func_val.type = VAL_FUNC;
          func_val.v.func.func_obj = rf;
          func_val.v.func.ffi_func = NULL;
          func_val.v.func.is_ffi = 0;
          try_register_global_func($3, func_val); /* class内部不注册全局符号表 */
        }
        | FUNC ID LPAREN param_list RPAREN block_stmt {
          $$ = ast_func_def($2, $4, $6);
          $$->u.func_def.annotations = NULL;
          $$->u.func_def.ret_type_name = NULL;
          annotate_self_if_in_struct($$);
          /* 语义分析阶段：编译这个函数定义，生成RuntimeFunc，注册到全局符号 */
          RuntimeFunc* rf = NULL;
          if(!g_current_class_name && !g_current_struct_name) {
              /* 只有全局函数才在这里编译；class 方法在 class_add_method 中编译；
               * struct 方法在 struct RBRACE 规则中（struct_register 后）统一编译，
               * 否则 struct_lookup 在编译时返回 NULL，无法走 typed 栈 fast path */
              rf = compile_func_from_ast($$);
          }
          Value func_val = {0};
          func_val.type = VAL_FUNC;
          func_val.v.func.func_obj = rf;
          func_val.v.func.ffi_func = NULL;
          func_val.v.func.is_ffi = 0;
          try_register_global_func($2, func_val); /* class内部不注册全局符号表 */
        }
        | FUNC ID LPAREN param_list RPAREN COLON type_name_str block_stmt {
          /* 冒号后缀返回类型：func name(params) : type { ... } */
          $$ = ast_func_def($2, $4, $8);
          $$->u.func_def.annotations = NULL;
          $$->u.func_def.ret_type_name = $7;
          annotate_self_if_in_struct($$);
          RuntimeFunc* rf = NULL;
          if(!g_current_class_name && !g_current_struct_name) {
              rf = compile_func_from_ast($$);
          }
          Value func_val = {0};
          func_val.type = VAL_FUNC;
          func_val.v.func.func_obj = rf;
          func_val.v.func.ffi_func = NULL;
          func_val.v.func.is_ffi = 0;
          try_register_global_func($2, func_val);
        }
        | FUNC operator LPAREN param_list RPAREN block_stmt {
          /* 运算符重载：func +(other) { ... } */
          $$ = ast_func_def($2, $4, $6);
          $$->u.func_def.annotations = NULL;
          annotate_self_if_in_struct($$);
          RuntimeFunc* rf = NULL;
          if(!g_current_class_name && !g_current_struct_name) {
              /* 只有全局函数才在这里编译，class 方法在 class_add_method 中编译 */
              rf = compile_func_from_ast($$);
          }
          Value func_val = {0};
          func_val.type = VAL_FUNC;
          func_val.v.func.func_obj = rf;
          func_val.v.func.ffi_func = NULL;
          func_val.v.func.is_ffi = 0;
          try_register_global_func($2, func_val); /* class内部不注册全局符号表 */
        }
        | CONST FUNC ID LPAREN param_list RPAREN block_stmt {
          $$ = ast_func_def($3, $5, $7);
          $$->u.func_def.annotations = NULL;
          $$->u.func_def.is_const = 1;
          annotate_self_if_in_struct($$);
          /* 语义分析阶段：编译这个函数定义，生成RuntimeFunc，注册到全局符号 */
          RuntimeFunc* rf = NULL;
          if(!g_current_class_name && !g_current_struct_name) {
              /* 只有全局函数才在这里编译，class 方法在 class_add_method 中编译 */
              rf = compile_func_from_ast($$);
          }
          Value func_val = {0};
          func_val.type = VAL_FUNC;
          func_val.v.func.func_obj = rf;
          func_val.v.func.ffi_func = NULL;
          func_val.v.func.is_ffi = 0;
          try_register_global_func($3, func_val); /* class内部不注册全局符号表 */
        }
        | CONST FUNC ID LPAREN param_list RPAREN COLON type_name_str block_stmt {
          /* const func 返回类型后缀：const func name(params) : type { ... } */
          $$ = ast_func_def($3, $5, $9);
          $$->u.func_def.annotations = NULL;
          $$->u.func_def.is_const = 1;
          $$->u.func_def.ret_type_name = $8;
          annotate_self_if_in_struct($$);
          RuntimeFunc* rf = NULL;
          if(!g_current_class_name && !g_current_struct_name) {
              rf = compile_func_from_ast($$);
          }
          Value func_val = {0};
          func_val.type = VAL_FUNC;
          func_val.v.func.func_obj = rf;
          func_val.v.func.ffi_func = NULL;
          func_val.v.func.is_ffi = 0;
          try_register_global_func($3, func_val);
        }
        /* 生成器函数：gen func name(params) { body } */
        | TOK_GEN FUNC ID LPAREN param_list RPAREN block_stmt {
          $$ = ast_func_def($3, $5, $7);
          $$->u.func_def.annotations = NULL;
          $$->u.func_def.is_generator = 1;
          annotate_self_if_in_struct($$);
          RuntimeFunc* rf = NULL;
          if(!g_current_class_name && !g_current_struct_name) {
              /* 只有全局函数才在这里编译，class 方法在 class_add_method 中编译 */
              rf = compile_func_from_ast($$);
          }
          Value func_val = {0};
          func_val.type = VAL_FUNC;
          func_val.v.func.func_obj = rf;
          func_val.v.func.ffi_func = NULL;
          func_val.v.func.is_ffi = 0;
          try_register_global_func($3, func_val); /* class内部不注册全局符号表 */
        }
        /* 生成器函数 + 前置返回类型：gen func <int> name(params) { body } */
        | TOK_GEN FUNC TOK_TYPE_ANNOT ID LPAREN param_list RPAREN block_stmt {
          $$ = ast_func_def($4, $6, $8);
          $$->u.func_def.annotations = NULL;
          $$->u.func_def.is_generator = 1;
          $$->u.func_def.ret_type_name = strdup(castkind_to_name($3));
          annotate_self_if_in_struct($$);
          RuntimeFunc* rf = NULL;
          if(!g_current_class_name && !g_current_struct_name) {
              rf = compile_func_from_ast($$);
          }
          Value func_val = {0};
          func_val.type = VAL_FUNC;
          func_val.v.func.func_obj = rf;
          func_val.v.func.ffi_func = NULL;
          func_val.v.func.is_ffi = 0;
          try_register_global_func($4, func_val);
        }
        /* 生成器函数 + 冒号后缀返回类型：gen func name(params) : type { body } */
        | TOK_GEN FUNC ID LPAREN param_list RPAREN COLON type_name_str block_stmt {
          $$ = ast_func_def($3, $5, $9);
          $$->u.func_def.annotations = NULL;
          $$->u.func_def.is_generator = 1;
          $$->u.func_def.ret_type_name = $8;
          annotate_self_if_in_struct($$);
          RuntimeFunc* rf = NULL;
          if(!g_current_class_name && !g_current_struct_name) {
              rf = compile_func_from_ast($$);
          }
          Value func_val = {0};
          func_val.type = VAL_FUNC;
          func_val.v.func.func_obj = rf;
          func_val.v.func.ffi_func = NULL;
          func_val.v.func.is_ffi = 0;
          try_register_global_func($3, func_val);
        }
        /* 泛型函数：func<T> name(params) { body } */
        | FUNC generic_param_list ID LPAREN param_list RPAREN block_stmt {
          $$ = ast_func_def($3, $5, $7);
          $$->u.func_def.annotations = NULL;
          annotate_self_if_in_struct($$);
          $$->u.func_def.generic_params = $2;
          RuntimeFunc* rf = NULL;
          if(!g_current_class_name && !g_current_struct_name) {
              /* 只有全局函数才在这里编译，class 方法在 class_add_method 中编译 */
              rf = compile_func_from_ast($$);
          }
          Value func_val = {0};
          func_val.type = VAL_FUNC;
          func_val.v.func.func_obj = rf;
          func_val.v.func.ffi_func = NULL;
          func_val.v.func.is_ffi = 0;
          try_register_global_func($3, func_val); /* class内部不注册全局符号表 */
        }
        /* FFI 外部函数声明：extern <ret_type> func name(params) */
        | TOK_EXTERN TOK_TYPE_ANNOT FUNC ID LPAREN param_list RPAREN SEMI {
          $$ = ast_extern_func($4, $6, castkind_to_name($2), NULL);
        }
        /* FFI 外部函数声明（指定库）：extern "libname" <ret_type> func name(params) */
        | TOK_EXTERN STRING_LIT TOK_TYPE_ANNOT FUNC ID LPAREN param_list RPAREN SEMI {
          $$ = ast_extern_func($5, $7, castkind_to_name($3), $2);
        } ;

/* 函数定义列表：用于扩展方法块 */
func_def_list
    : func_def                     { $$ = $1; }
    | func_def_list func_def      { $$ = ast_seq($1, $2); }
    ;

/* 泛型参数列表：<T> / <T, U>（用 param.next 链接，与函数参数一致） */
generic_param_list : LT generic_param_items GT { $$ = $2; }
                   ;

opt_generic_param_list : %empty { $$ = NULL; }
                       | generic_param_list { $$ = $1; }
                       ;

generic_param_items : ID { $$ = ast_param($1, 0, NULL); }
                    | ID COLON ID { $$ = ast_param_constraint($1, $3); }
                    | generic_param_items COMMA ID { $$ = ast_param_append($1, ast_param($3, 0, NULL)); }
                    | generic_param_items COMMA ID COLON ID { $$ = ast_param_append($1, ast_param_constraint($3, $5)); }
                    ;

/* 宏定义：macro name(params) { body } */
macro_def : MACRO ID LPAREN param_list RPAREN block_stmt {
          $$ = ast_macro_def($2, $4, $6);
          /* 注册到宏表，供后续宏展开使用 */
          macro_register($2, $4, $6);
        } ;

/* 参数列表：支持 a,b,...rest；可变参数只能放在最后一个 */
param_list
    : %empty                 { $$ = NULL; }
    | param                  { $$ = $1; }
    | param_list COMMA param { $$ = ast_param_append($1, $3); }
;

param
    : ID                     { $$ = ast_param($1, 0, NULL); } /*普通参数 is_ellipsis=0 */
    | ID ASSIGN expr         { $$ = ast_param($1, 0, $3); } /*带默认值的参数 */
    | ELLIPSIS ID            { $$ = ast_param($2, 1, NULL); } /* ...args 可变参数 is_ellipsis=1 */
    | TOK_TYPE_ANNOT ID      { $$ = ast_param($2, 0, NULL); $$->u.param.constraint = strdup(castkind_to_name($1)); } /*带类型标注的参数 <int>a */
    | LT ID GT ID            { $$ = ast_param($4, 0, NULL); $$->u.param.constraint = strdup($2); } /*自定义类型标注的参数 <Point>a */
    | TOK_REF ID             { $$ = ast_param($2, 0, NULL); $$->u.param.is_ref = 1; } /*引用传递参数 ref p */
    | TOK_REF TOK_TYPE_ANNOT ID { $$ = ast_param($3, 0, NULL); $$->u.param.is_ref = 1; $$->u.param.constraint = strdup(castkind_to_name($2)); } /*带基本类型标注的ref参数 ref <int> p */
    | TOK_REF LT ID GT ID    { $$ = ast_param($5, 0, NULL); $$->u.param.is_ref = 1; $$->u.param.constraint = strdup($3); } /*带自定义类型标注的ref参数 ref <Point> p */
    /* 冒号后缀类型标注：n: string, m: map, p: Point（与 <type> name 等价，更友好）
     * 直接用 builtin_type_name / ID 而非 type_name_str，避免与返回类型/三元/map 上下文冲突 */
    | ID COLON builtin_type_name { $$ = ast_param($1, 0, NULL); $$->u.param.constraint = strdup(castkind_to_name($3)); } /*基本类型 n: int */
    | ID COLON ID               { $$ = ast_param($1, 0, NULL); $$->u.param.constraint = strdup($3); } /*自定义类型 n: Point */
    | ID COLON builtin_type_name ASSIGN expr { $$ = ast_param($1, 0, $5); $$->u.param.constraint = strdup(castkind_to_name($3)); } /*基本类型+默认值 n: int = 5 */
    | ID COLON ID ASSIGN expr   { $$ = ast_param($1, 0, $5); $$->u.param.constraint = strdup($3); } /*自定义类型+默认值 n: Point = ... */
    | TOK_REF ID COLON builtin_type_name { $$ = ast_param($2, 0, NULL); $$->u.param.is_ref = 1; $$->u.param.constraint = strdup(castkind_to_name($4)); } /*ref + 基本类型 ref p: int */
    | TOK_REF ID COLON ID       { $$ = ast_param($2, 0, NULL); $$->u.param.is_ref = 1; $$->u.param.constraint = strdup($4); } /*ref + 自定义类型 ref p: Point */
    | TOK_REF ID COLON builtin_type_name ASSIGN expr { $$ = ast_param($2, 0, $6); $$->u.param.is_ref = 1; $$->u.param.constraint = strdup(castkind_to_name($4)); } /*ref + 基本类型+默认值 */
    | TOK_REF ID COLON ID ASSIGN expr { $$ = ast_param($2, 0, $6); $$->u.param.is_ref = 1; $$->u.param.constraint = strdup($4); } /*ref + 自定义类型+默认值 */
    /* ===== 可空形参 T?：constraint 记基础类型，is_nullable=1（绑定时跳过非空校验） ===== */
    | TOK_TYPE_ANNOT ID QMARK { $$ = ast_param($2, 0, NULL); $$->u.param.constraint = strdup(castkind_to_name($1)); $$->u.param.is_nullable = 1; } /* <int>a? */
    | LT ID GT ID QMARK      { $$ = ast_param($4, 0, NULL); $$->u.param.constraint = strdup($2); $$->u.param.is_nullable = 1; } /* <Point>a? */
    | ID COLON builtin_type_name QMARK { $$ = ast_param($1, 0, NULL); $$->u.param.constraint = strdup(castkind_to_name($3)); $$->u.param.is_nullable = 1; } /* n: int? */
    | ID COLON ID QMARK               { $$ = ast_param($1, 0, NULL); $$->u.param.constraint = strdup($3); $$->u.param.is_nullable = 1; } /* n: Point? */
    | ID COLON builtin_type_name QMARK ASSIGN expr { $$ = ast_param($1, 0, $6); $$->u.param.constraint = strdup(castkind_to_name($3)); $$->u.param.is_nullable = 1; } /* n: int? = 5 */
    | ID COLON ID QMARK ASSIGN expr   { $$ = ast_param($1, 0, $6); $$->u.param.constraint = strdup($3); $$->u.param.is_nullable = 1; } /* n: Point? = ... */
;

/* 注解：@name 或 @name(args) */
annotation
    : TOK_AT ID                        { $$ = ast_annotation($2, NULL); }
    | TOK_AT ID LPAREN arg_list RPAREN { $$ = ast_annotation($2, $4); }
;

/* 注解列表：一个或多个注解 */
annotation_list
    : annotation                 { $$ = $1; }
    | annotation_list annotation { $$ = ast_seq_append($1, $2); }
;

/* 调用实参列表 */
arg_list
    : %empty               { $$ = NULL; }
    | arg                  { $$ = $1; }
    | arg_list COMMA arg   { $$ = ast_arg_append($1, $3); }
;

arg
    : expr                 { $$ = $1; }
    | ELLIPSIS unary_expr  { $$ = ast_spread($2); }
;

/* 解构赋值左边：a,b,c 标识符列表（AST_SEQ 链的 AST_VAR） */
destruct_lhs
    : ID COMMA ID                  { $$ = ast_seq(ast_var($1), ast_seq(ast_var($3), NULL)); }
    | destruct_lhs COMMA ID        { $$ = ast_seq_append($1, ast_var($3)); }
;

/* unpack 对象解构模式：{a, b, c}（unpack 后 { 被 lexer 识别为 MAP_OPEN） */
unpack_obj_pattern
    : MAP_OPEN unpack_name_list RBRACE   { $$ = $2; }
    ;

/* unpack 数组解构模式：[x, y, z]（unpack 后 [ 被 lexer 识别为 ARRAY_OPEN） */
unpack_arr_pattern
    : ARRAY_OPEN unpack_name_list RBRACKET   { $$ = $2; }
    ;

/* unpack 变量名列表：a, b, c */
unpack_name_list
    : ID                              { $$ = ast_seq(ast_var($1), NULL); }
    | unpack_name_list COMMA ID      { $$ = ast_seq_append($1, ast_var($3)); }
    ;


open_stmt
    : IF LPAREN expr RPAREN closed_stmt elif_clause_list else_part         { $$ = ast_if_chain($3, $5, $6, $7); }
    ;

/* try { body } catch (e) { handler } [finally { body }]；catch/finally 至少其一 */
try_stmt
    : TRY block_stmt catch_clause_list {
          if(is_single_untagged_catch($3)) {
              char* var_name = NULL;
              AstNode* catch_body = NULL;
              extract_single_catch($3, &var_name, &catch_body);
              $$ = ast_try($2, var_name, catch_body, NULL);
          } else {
              $$ = build_try_multi($2, $3, NULL);
          }
      }
    | TRY block_stmt catch_clause_list FINALLY block_stmt {
          if(is_single_untagged_catch($3)) {
              char* var_name = NULL;
              AstNode* catch_body = NULL;
              extract_single_catch($3, &var_name, &catch_body);
              $$ = ast_try($2, var_name, catch_body, $5);
          } else {
              $$ = build_try_multi($2, $3, $5);
          }
      }
    | TRY block_stmt FINALLY block_stmt {
          $$ = ast_try($2, NULL, NULL, $4);
      }
    ;

/* 多个 catch 子句列表（用 AST_SEQ 链接） */
catch_clause_list
    : catch_clause {
          $$ = $1;
      }
    | catch_clause_list catch_clause {
          $$ = ast_seq($1, $2);
      }
    ;

/* 单个 catch 子句：catch (e) / catch (Type e) / catch (e: Type)
   使用 opt_catch_type 避免移进/归约冲突；
   第三个分支支持冒号后缀类型语法（用户更易书写） */
catch_clause
    : CATCH LPAREN ID opt_catch_type RPAREN block_stmt {
          if($4) {
              $$ = make_catch_clause(strdup($3), strdup($4), $6);
          } else {
              $$ = make_catch_clause(NULL, strdup($3), $6);
          }
      }
    | CATCH LPAREN ID COLON ID RPAREN block_stmt {
          $$ = make_catch_clause(strdup($5), strdup($3), $7);
      }
    ;

/* 可选的 catch 类型名：有类型或空 */
opt_catch_type
    : ID { $$ = $1; }
    | %empty { $$ = NULL; }
    ;


block_stmt
    : LBRACE stmt_list RBRACE      { $$ = ast_block($2); }
    | LBRACE RBRACE                { $$ = ast_block(NULL); }
    ;

break_stmt
    : BREAK SEMI { $$ = ast_break(NULL); }
    | BREAK ID SEMI { $$ = ast_break($2); }
    ;

continue_stmt
    : CONTINUE SEMI { $$ = ast_continue(NULL); }
    | CONTINUE ID SEMI { $$ = ast_continue($2); }
    ;

/* assert cond; / assert cond, "msg"; —— 语法糖：调用 builtin __assert(cond[, msg]) */
assert_stmt
    : TOK_ASSERT expr SEMI { $$ = L(ast_call(strdup("__assert"), $2)); }
    | TOK_ASSERT expr COMMA expr SEMI { $$ = L(ast_call(strdup("__assert"), ast_seq($2, $4))); }
    ;

/* defer { body } —— 延迟到函数退出（return/throw/fallthrough）时按 LIFO 执行 */
defer_stmt
    : TOK_DEFER block_stmt { $$ = L(ast_defer($2)); }
    ;

/* 多返回值表达式列表：用左嵌套 seq 收集，最终在 return_stmt 折成数组字面量
 * 仅用于 RETURN 后的逗号列表语境，与函数调用参数 COMMA 不冲突（lookahead 区分） */
expr_list
    : expr COMMA expr        { $$ = ast_seq($1, $3); }
    | expr_list COMMA expr   { $$ = ast_seq($1, $3); }
    ;

return_stmt    : RETURN SEMI              { $$ = ast_return(NULL); }
               | RETURN expr SEMI         { $$ = ast_return($2); }
               | RETURN expr_list SEMI    { $$ = ast_return(L(ast_array_lit($2, -1))); }
;

/* yield 语句：生成器函数中产生一个值并暂停 */
yield_stmt     : TOK_YIELD SEMI           { $$ = ast_yield(NULL); }
               | TOK_YIELD expr SEMI      { $$ = ast_yield($2); }
;

switch_stmt
    : SWITCH LPAREN expr RPAREN LBRACE case_list RBRACE {
        $$ = ast_switch($3, $6);
    }
    ;

case_list
    : %empty                 { $$ = NULL; }
    | case_list case_item    { $$ = ast_case_append($1, $2); }
    ;

case_item
    : CASE const_expr CASE_COLON stmt_list {
        $$ = ast_case($2, $4, 0);
    }
    | CASE type_keyword CASE_COLON stmt_list {
        $$ = ast_case_type($2, $4);
    }
    | CASE ID CASE_COLON stmt_list {
        $$ = ast_case_guard($2, NULL, $4);
    }
    | CASE ID IF expr CASE_COLON stmt_list {
        $$ = ast_case_guard($2, $4, $6);
    }
    | DEFAULT CASE_COLON stmt_list {
        $$ = ast_case(NULL, $3, 1);
    }
    ;

/* type keyword for pattern matching: case int: / case string: etc */
type_keyword
    : TOK_INT      { $$ = VAL_INT; }
    | TOK_DOUBLE   { $$ = VAL_DOUBLE; }
    | TOK_STRING   { $$ = VAL_STRING; }
    | TOK_BOOL     { $$ = VAL_BOOL; }
    | TOK_CHAR     { $$ = VAL_CHAR; }
    ;

/* case后面只能是编译期常量：数字、整数、char字面量、字符串字面量、enum 成员（ID.ID） */
const_expr
    : NUMBER                  { $$ = ast_num($1); }
    | INTEGER                 { $$ = ast_int($1); }
    | BIG_INTEGER             { $$ = ast_string($1); free($1); }  /* 超大整数存成字符串，用于 <bigint> */
    | BIG_DECIMAL             { $$ = ast_string($1); free($1); }  /* 高精度浮点存成字符串，用于 <decimal> */
    | char_lit                { $$ = ast_new_char($1); }
    | STRING_LIT              { $$ = ast_string($1); free($1); }
    | TRUE                    { $$ = ast_bool(1); }
    | FALSE                   { $$ = ast_bool(0); }
    | ID DOT ID               {
          /* enum 成员访问：编译期在 enum 字面量表中查 member_name 对应的值，
           * 找到则替换为字面量值（如 case Op.ADD → case 1），避免函数内无法访问
           * 全局 enum 变量的作用域限制；未找到则回退为运行时 ast_index 表达式 */
          AstNode* v = enum_table_lookup_member($1, $3);
          if(v) {
              $$ = v;
          } else {
              $$ = ast_index(ast_var($1), ast_string($3));
          }
          free($1); free($3);
      }
    ;

elif_clause_list
    : %empty                       { $$ = NULL; }
    | elif_clause_list elif_clause { $$ = ast_elif_append($1, $2); }
    ;

elif_clause
    : ELSEIF LPAREN expr RPAREN closed_stmt { $$ = ast_elif($3, $5); }
    ;

else_part
    : %empty           { $$ = NULL; }
    | ELSE closed_stmt { $$ = $2; }
    ;

expr_opt
    : %empty { $$ = NULL; }
    | expr   { $$ = $1; }
    ;

for_init
    : %empty                { $$ = NULL; }
    | expr                  { $$ = $1; }
    ;

for_incr
    : %empty                { $$ = NULL; }
    | expr                  { $$ = $1; }
    ;

char_lit
    : TOK_CHAR_LIT { $$ = $<ch>1; }
    ;

primary
    : NUMBER                  { $$ = ast_num($1); }
    | INTEGER                 { $$ = ast_int($1); }
    | BIG_INTEGER             { $$ = ast_string($1); free($1); }  /* 超大整数存成字符串，用于 <bigint> */
    | BIG_DECIMAL             { $$ = ast_string($1); free($1); }  /* 高精度浮点存成字符串，用于 <decimal> */
    | TRUE                    { $$ = ast_bool(1); }
    | FALSE                   { $$ = ast_bool(0); }
    | NULL_LIT                { $$ = ast_none(); }
    | STRING_LIT              { $$ = ast_string($1); free($1); }
    | FSTRING_LIT             { $$ = L(maybe_template($1)); free($1); }
    | char_lit                { $$ = ast_new_char($1); }
    | ID                      { $$ = L(ast_var($1)); }
    | TOK_SUPER               { $$ = L(ast_var(strdup("super"))); }  /* super 关键字：父类引用 */
    | ID LPAREN arg_list RPAREN {
          /* 宏调用：如果是已注册的宏，则展开；否则作为普通函数调用 */
          if(macro_is_defined($1)) {
              AstNode* mdef = macro_lookup($1);
              $$ = L(macro_expand(mdef, $3));
          } else if(struct_lookup($1) || class_lookup($1)) {
              /* 已注册的 struct/class 名：构造调用 → ast_class_new */
              int argc = 0;
              for(AstNode* p = $3; p; p = (p->type == AST_SEQ) ? p->u.seq.second : NULL) argc++;
              $$ = L(ast_class_new($1, argc, $3));
          } else if(type_lookup($1)) {
              /* type 形状位置构造 T(v0, v1)：组装成带字段 cast 的 map literal（与 T{...} 同路），
               * 否则退化为普通调用 T 不是函数，误发 INT64_INDEX_SET(142) */
              AstNode* made = wrap_type_positional($1, $3);
              if(!made) YYERROR;   /* 构造校验失败：让 yyparse 返回 1，避免 NULL 进 AST 后段错误 */
              $$ = L(made);
          } else {
              $$ = L(ast_call($1, $3));
          }
      }  /* 函数调用 foo(a,b,c) 或宏调用 或构造调用 */
    | ARRAY_OPEN arg_list RBRACKET { $$ = ast_array_lit($2, -1); }  /* 数组字面量 [1,2,3] / []（lexer 按上下文消歧） */
    /* 列表推导式：[expr for x in iter] / [expr for x in iter if cond] */
    | ARRAY_OPEN expr FOR ID TOK_IN expr RBRACKET {
          $$ = L(ast_comp_list($2, ast_var(strdup($4)), $6, NULL));
          free($4);
      }
    | ARRAY_OPEN expr FOR ID TOK_IN expr IF expr RBRACKET {
          $$ = L(ast_comp_list($2, ast_var(strdup($4)), $6, $8));
          free($4);
      }
    | MAP_OPEN map_items RBRACE   { $$ = ast_map_lit($2); }    /* 字典字面量 {"k": v, name: 1} / {}（lexer 上下文消歧：表达式位置） */
    /* 字典推导式：{k:v for x in iter} / {k:v for x in iter if cond}
     * 使用 ID COLON expr 避免与 map_items 的 shift/reduce 冲突；
     * key 取 ID 的变量值（非 string 转换） */
    | MAP_OPEN ID COLON expr FOR ID TOK_IN expr RBRACE {
          $$ = L(ast_comp_map(ast_var(strdup($2)), $4, ast_var(strdup($6)), $8, NULL));
          free($2); free($6);
      }
    | MAP_OPEN ID COLON expr FOR ID TOK_IN expr IF expr RBRACE {
          $$ = L(ast_comp_map(ast_var(strdup($2)), $4, ast_var(strdup($6)), $8, $10));
          free($2); free($6);
      }
    /* 命名构造简写 ClassName{field: value, ...}：lexer 在类型名后的 { 识别为 MAP_OPEN
     * 等价于 <ClassName>{field: value}，复用 wrap_struct_named 按字段名映射位置参数 */
    | ID MAP_OPEN map_items RBRACE {
          if(struct_lookup($1) || class_lookup($1) || type_lookup($1)) {
              AstNode* made = wrap_struct_named($1, $3);
              if(!made) YYERROR;   /* 字段校验失败：终止解析（yyparse 返回 1），勿把 NULL 塞进 AST */
              $$ = L(made);
          } else {
              /* 非类型名：回退为普通 map 字面量（ID 作 map 前缀不合法） */
              char buf[256];
              snprintf(buf, sizeof buf, "'%s' 不是已注册的类型，无法使用命名构造 %s{...}", $1, $1);
              yyerror(buf);
              $$ = L(ast_map_lit($3));
          }
      }
    | LPAREN expr RPAREN      { $$ = $2; }
    /* 强转 (int)x 接 postfix_expr：C 语义，(int)a[0] = (int)(a[0])（cast 作用于整个后缀表达式） */
    | LPAREN TOK_INT RPAREN postfix_expr   { $$ = new_cast_node(CAST_INT, $4); }
    | LPAREN TOK_DOUBLE RPAREN postfix_expr { $$ = new_cast_node(CAST_DOUBLE, $4); }
    | LPAREN TOK_STRING RPAREN postfix_expr { $$ = new_cast_node(CAST_STRING, $4); }
    | LPAREN TOK_BOOL RPAREN postfix_expr   { $$ = new_cast_node(CAST_BOOL, $4); }
    | LPAREN TOK_ASCII RPAREN postfix_expr  { $$ = new_cast_node(CAST_ASCII, $4); }
    | LPAREN TOK_CHAR RPAREN postfix_expr   { $$ = new_cast_node(CAST_CHAR, $4); }
    | LPAREN TOK_BYTE RPAREN postfix_expr   { $$ = new_cast_node(CAST_BYTE, $4); }
    | LPAREN TOK_INT8 RPAREN postfix_expr   { $$ = new_cast_node(CAST_INT8, $4); }
    | LPAREN TOK_INT16 RPAREN postfix_expr  { $$ = new_cast_node(CAST_INT16, $4); }
    | LPAREN TOK_INT32 RPAREN postfix_expr  { $$ = new_cast_node(CAST_INT32, $4); }
    | LPAREN TOK_INT64 RPAREN postfix_expr  { $$ = new_cast_node(CAST_INT64, $4); }
    | LPAREN TOK_UINT8 RPAREN postfix_expr  { $$ = new_cast_node(CAST_UINT8, $4); }
    | LPAREN TOK_UINT16 RPAREN postfix_expr { $$ = new_cast_node(CAST_UINT16, $4); }
    | LPAREN TOK_UINT32 RPAREN postfix_expr { $$ = new_cast_node(CAST_UINT32, $4); }
    | LPAREN TOK_UINT64 RPAREN postfix_expr { $$ = new_cast_node(CAST_UINT64, $4); }
    | LPAREN TOK_UINT RPAREN postfix_expr   { $$ = new_cast_node(CAST_UINT, $4); }
    | LPAREN TOK_LONG RPAREN postfix_expr   { $$ = new_cast_node(CAST_LONG, $4); }
    | LPAREN TOK_LONGLONG RPAREN postfix_expr { $$ = new_cast_node(CAST_LONGLONG, $4); }
    | LPAREN TOK_FLOAT RPAREN postfix_expr    { $$ = new_cast_node(CAST_FLOAT, $4); }
    | LPAREN TOK_ULONG RPAREN postfix_expr   { $$ = new_cast_node(CAST_ULONG, $4); }
    | LPAREN TOK_UCHAR RPAREN postfix_expr   { $$ = new_cast_node(CAST_UCHAR, $4); }
    | LPAREN TOK_SHORT RPAREN postfix_expr   { $$ = new_cast_node(CAST_SHORT, $4); }
    | LPAREN TOK_USHORT RPAREN postfix_expr  { $$ = new_cast_node(CAST_USHORT, $4); }
    | LPAREN TOK_SIZE_T RPAREN postfix_expr  { $$ = new_cast_node(CAST_SIZE_T, $4); }
    | LPAREN TOK_SSIZE_T RPAREN postfix_expr { $$ = new_cast_node(CAST_SSIZE_T, $4); }
    | LPAREN TOK_VOID RPAREN postfix_expr    { $$ = new_cast_node(CAST_VOID, $4); }
    | LPAREN TOK_LONG_DOUBLE RPAREN postfix_expr { $$ = new_cast_node(CAST_LONG_DOUBLE, $4); }
    | LPAREN TOK_PTR RPAREN postfix_expr     { $$ = new_cast_node(CAST_PTR, $4); }
    /* 泛型容器字面量：(byte)[1,2,3] 逐元素强转 / (byte){"a":1} 逐值强转
       （lexer 上下文消歧后 cast 后接 LBRACKET/LBRACE，按容器字面量解释） */
    | LPAREN TOK_INT RPAREN LBRACKET arg_list RBRACKET
        { $$ = new_cast_node(CAST_INT, ast_array_lit($5, -1)); }
    | LPAREN TOK_INT RPAREN LBRACE map_items RBRACE
        { $$ = new_cast_node(CAST_INT, ast_map_lit($5)); }
    | LPAREN TOK_DOUBLE RPAREN LBRACKET arg_list RBRACKET
        { $$ = new_cast_node(CAST_DOUBLE, ast_array_lit($5, -1)); }
    | LPAREN TOK_DOUBLE RPAREN LBRACE map_items RBRACE
        { $$ = new_cast_node(CAST_DOUBLE, ast_map_lit($5)); }
    | LPAREN TOK_STRING RPAREN LBRACKET arg_list RBRACKET
        { $$ = new_cast_node(CAST_STRING, ast_array_lit($5, -1)); }
    | LPAREN TOK_STRING RPAREN LBRACE map_items RBRACE
        { $$ = new_cast_node(CAST_STRING, ast_map_lit($5)); }
    | LPAREN TOK_BOOL RPAREN LBRACKET arg_list RBRACKET
        { $$ = new_cast_node(CAST_BOOL, ast_array_lit($5, -1)); }
    | LPAREN TOK_BOOL RPAREN LBRACE map_items RBRACE
        { $$ = new_cast_node(CAST_BOOL, ast_map_lit($5)); }
    | LPAREN TOK_ASCII RPAREN LBRACKET arg_list RBRACKET
        { $$ = new_cast_node(CAST_ASCII, ast_array_lit($5, -1)); }
    | LPAREN TOK_ASCII RPAREN LBRACE map_items RBRACE
        { $$ = new_cast_node(CAST_ASCII, ast_map_lit($5)); }
    | LPAREN TOK_CHAR RPAREN LBRACKET arg_list RBRACKET
        { $$ = new_cast_node(CAST_CHAR, ast_array_lit($5, -1)); }
    | LPAREN TOK_CHAR RPAREN LBRACE map_items RBRACE
        { $$ = new_cast_node(CAST_CHAR, ast_map_lit($5)); }
    | LPAREN TOK_BYTE RPAREN LBRACKET arg_list RBRACKET
        { $$ = new_cast_node(CAST_BYTE, ast_array_lit($5, -1)); }
    | LPAREN TOK_BYTE RPAREN LBRACE map_items RBRACE
        { $$ = new_cast_node(CAST_BYTE, ast_map_lit($5)); }
    /* 类型标注：<T>value 给变量打类型标记（等价 C 的类型声明 int a = 8）
       <string,V>{k:v} map 值强转（键固定 string） */
    | TOK_TYPE_ANNOT unary_expr
        {
            $$ = ast_type_annotation($1, $2);
        }
    | LT ID GT unary_expr {
          /* 接口类型标注：<Printable>expr → 接口引用类型 */
          if(interface_lookup($2) != NULL) {
              $$ = ast_interface_annotation($2, $4);
          } else if(type_lookup($2) != NULL && $4 && $4->type == AST_MAP_LIT) {
              /* 命名字段构造：<CustomType>{ name: v, ... } 展开为位置构造 */
              AstNode* made = wrap_struct_named($2, $4->u.map_lit.entries);
              free($2);
              if(!made) YYERROR;   /* 字段校验失败：终止解析，避免 NULL 节点导致后续段错误 */
              $$ = L(made);
          } else if(type_lookup($2) != NULL && $4 && $4->type == AST_ARRAY_LIT) {
              /* 泛型形状数组：<Person>[e1, e2] → [Person(e1), Person(e2)]
                 （本规则是实际生效路径：[..] 先被归约为数组字面量；
                  下方 LT ID GT ARRAY_OPEN 规则因移进冲突不可达） */
              $$ = L(ast_array_lit(wrap_type_list($2, $4->u.array_lit.elems), -1));
              free($2);
          } else {
              /* 非接口类型：暂时当作普通表达式处理（后续可扩展自定义类型标注） */
              $$ = $4;
              free($2);
          }
      }
    | LT map_generic_type COMMA map_generic_type GT MAP_OPEN map_items RBRACE
        { $$ = ast_map_lit(wrap_map_kv($7, (CastKind)$2, (CastKind)$4)); }
    | LT ID GT ARRAY_OPEN arg_list RBRACKET {
          /* 泛型自定义类型：<Person>[e1,e2] → [Person(e1), Person(e2)]（形状构造） */
          if(type_lookup($2) != NULL) {
              $$ = L(ast_array_lit(wrap_type_list($2, $5), -1));
              free($2);
          } else {
              yyerror("未定义类型");
          }
      }
    | TOK_TYPE LPAREN expr RPAREN {
          /* type 关键字兼作内置函数：type(x) → 类型名字符串 */
          $$ = L(ast_call(strdup("type"), $3));
      }
    | FUNC LPAREN param_list RPAREN block_stmt {
          /* 匿名函数表达式：生成内部名 _lambda_N，与具名同路注册（VM sym + IR 函数表） */
          char nm[64];
          snprintf(nm, sizeof nm, "_lambda_%d", g_lambda_seq++);
          $$ = L(ast_func_def(nm, $3, $5));
          RuntimeFunc* rf = NULL;
          if(!g_current_class_name && !g_current_struct_name) {
              /* 只有全局函数才在这里编译，class 方法在 class_add_method 中编译 */
              rf = compile_func_from_ast($$);
          }
          Value func_val = {0};
          func_val.type = VAL_FUNC;
          func_val.v.func.func_obj = rf;
          func_val.v.func.ffi_func = NULL;
          func_val.v.func.is_ffi = 0;
          sym_set(nm, func_val);
      }
    /* 箭头函数：(params) => { body } 或 (params): RetType => { body }
     * params 支持冒号后缀类型标注（name: string 等）；
     * 与匿名 func 表达式同路：生成内部名 _arrow_N，走 ast_func_def + compile_func_from_ast */
    | LPAREN param_list RPAREN ARROW block_stmt {
          char nm[64];
          snprintf(nm, sizeof nm, "_arrow_%d", g_lambda_seq++);
          $$ = L(ast_func_def(nm, $2, $5));
          RuntimeFunc* rf = NULL;
          if(!g_current_class_name && !g_current_struct_name) {
              rf = compile_func_from_ast($$);
          }
          Value func_val = {0};
          func_val.type = VAL_FUNC;
          func_val.v.func.func_obj = rf;
          func_val.v.func.ffi_func = NULL;
          func_val.v.func.is_ffi = 0;
          sym_set(nm, func_val);
      }
    | LPAREN param_list RPAREN COLON type_name_str ARROW block_stmt {
          char nm[64];
          snprintf(nm, sizeof nm, "_arrow_%d", g_lambda_seq++);
          $$ = L(ast_func_def(nm, $2, $7));
          $$->u.func_def.ret_type_name = $5;
          RuntimeFunc* rf = NULL;
          if(!g_current_class_name && !g_current_struct_name) {
              rf = compile_func_from_ast($$);
          }
          Value func_val = {0};
          func_val.type = VAL_FUNC;
          func_val.v.func.func_obj = rf;
          func_val.v.func.ffi_func = NULL;
          func_val.v.func.is_ffi = 0;
          sym_set(nm, func_val);
      }
    /* 箭头生成器函数：gen (params) => { yield ... } / gen (params): RetType => { yield ... }
     * 与 gen func name() 同路：is_generator=1，调用时返回 GeneratorObject 而非直接执行 */
    | TOK_GEN LPAREN param_list RPAREN ARROW block_stmt {
          char nm[64];
          snprintf(nm, sizeof nm, "_arrow_%d", g_lambda_seq++);
          $$ = L(ast_func_def(nm, $3, $6));
          $$->u.func_def.is_generator = 1;
          RuntimeFunc* rf = NULL;
          if(!g_current_class_name && !g_current_struct_name) {
              rf = compile_func_from_ast($$);
          }
          Value func_val = {0};
          func_val.type = VAL_FUNC;
          func_val.v.func.func_obj = rf;
          func_val.v.func.ffi_func = NULL;
          func_val.v.func.is_ffi = 0;
          sym_set(nm, func_val);
      }
    | TOK_GEN LPAREN param_list RPAREN COLON type_name_str ARROW block_stmt {
          char nm[64];
          snprintf(nm, sizeof nm, "_arrow_%d", g_lambda_seq++);
          $$ = L(ast_func_def(nm, $3, $8));
          $$->u.func_def.is_generator = 1;
          $$->u.func_def.ret_type_name = $6;
          RuntimeFunc* rf = NULL;
          if(!g_current_class_name && !g_current_struct_name) {
              rf = compile_func_from_ast($$);
          }
          Value func_val = {0};
          func_val.type = VAL_FUNC;
          func_val.v.func.func_obj = rf;
          func_val.v.func.ffi_func = NULL;
          func_val.v.func.is_ffi = 0;
          sym_set(nm, func_val);
      }
    | READ STRING_LIT {
          /* read "path" → 文件内容；普通路径不内插；结果可后续缀（.len() 等） */
          AstNode* p = ast_string($2);
          free($2);
          $$ = L(ast_call(strdup("read_file"), p));
      }
    | READ FSTRING_LIT {
          /* read f"path" → 文件内容；f 前缀路径支持模板内插；结果可后续缀 */
          AstNode* p = L(maybe_template($2));
          free($2);
          $$ = L(ast_call(strdup("read_file"), p));
      }
    ;

postfix_expr
    : primary
    | postfix_expr LBRACKET expr RBRACKET  { $$ = L(ast_index($1, $3)); }  /* 数组下标 a[i] */
    /* 切片 a[start:end] / a[start:] / a[:end] / a[:] —— 语法糖：调用 builtin slice(arr, start, end) */
    | postfix_expr LBRACKET expr COLON expr RBRACKET
        { $$ = L(ast_call(strdup("slice"), ast_seq($1, ast_seq($3, $5)))); }
    | postfix_expr LBRACKET expr COLON RBRACKET
        { $$ = L(ast_call(strdup("slice"), ast_seq($1, ast_seq($3, ast_none())))); }
    | postfix_expr LBRACKET COLON expr RBRACKET
        { $$ = L(ast_call(strdup("slice"), ast_seq($1, ast_seq(ast_none(), $4)))); }
    | postfix_expr LBRACKET COLON RBRACKET
        { $$ = L(ast_call(strdup("slice"), ast_seq($1, ast_seq(ast_none(), ast_none())))); }
    | postfix_expr PLUSPLUS   { $$ = ast_unary(OP_POST_INC, $1); }
    | postfix_expr MINUSMINUS { $$ = ast_unary(OP_POST_DEC, $1); }
    /* 调用链 f(1)(2)：callee 为表达式（函数值），动态调用 */
    | postfix_expr LPAREN arg_list RPAREN {
          /* super(args)：调用父类构造函数 <parent>___init__(self, args) */
          if($1->type == AST_VAR && strcmp($1->u.varname, "super") == 0) {
              AstNode* self_arg = L(ast_var(strdup("self")));
              AstNode* all_args = $3 ? ast_seq_front($3, self_arg) : self_arg;
              /* 父类构造函数名：<parent>___init__ */
              char ctor_name[256];
              snprintf(ctor_name, sizeof(ctor_name), "%s___init__", g_current_class_parent ? g_current_class_parent : "unknown");
              $$ = L(ast_call(strdup(ctor_name), all_args));
          } else {
              $$ = L(ast_dyn_call($1, $3));
          }
      }
    /* 方法链 a.b(x,y) → b(a,x,y)（语法糖，接收者作为首参） */
    | postfix_expr DOT ID LPAREN arg_list RPAREN {
          AstNode* recv = $1;
          AstNode* margs = $5;
          /* requests.get/post/put/delete/head/patch：内置 HTTP 命名空间，
             接收者 requests 不进参数（url 是第一个实参） */
          if(recv->type == AST_VAR && strcmp(recv->u.varname, "requests") == 0 &&
             (strcmp($3, "get") == 0 || strcmp($3, "post") == 0 || strcmp($3, "put") == 0 ||
              strcmp($3, "delete") == 0 || strcmp($3, "head") == 0 || strcmp($3, "patch") == 0)) {
              $$ = L(ast_call($3, margs));
          } else if(strcmp($3, "get") == 0) {
              /* x.get(k)：统一走方法分派（内置 BUILTIN_GET：数组/字典安全取，越界/缺键 → null） */
              $$ = L(ast_method_call(recv, $3, margs));
          } else if(recv->type == AST_VAR && lm_is_module_alias(recv->u.varname)) {
              /* 模块命名空间 m.add(1,2) -> m["add"](1,2)：map 取值后动态调用，不把接收者当前参 */
              AstNode* fn = L(ast_index(recv, ast_string(strdup($3))));
              free($3);
              $$ = L(ast_dyn_call(fn, margs));
          } else if(recv->type == AST_VAR && strcmp(recv->u.varname, "super") == 0) {
              /* super.method(args)：保留 method_call 节点，编译器用父类方法表分派 */
              $$ = L(ast_method_call(recv, $3, margs));
          } else {
              /* 接收者绑定的方法调用 recv.method(args)：运行时按 recv 实际类型分派
               * （方法表含继承槽位，重写覆盖在原位置 → 多态），不再拍平为全局函数名调用 */
              $$ = L(ast_method_call(recv, $3, margs));
          }
      }
    /* 属性访问 a.b → a["b"]（map 点属性；无参方法链语法不再保留） */
    | postfix_expr DOT ID {
          /* 枚举成员访问：recv 是简单 ID 且 ID 是已注册 enum 名 → 编译期求值为字面量值
           * 避免函数内访问全局 enum 变量的作用域限制（case Op.ADD 和 op==Op.ADD 都受益） */
          if($1->type == AST_VAR) {
              AstNode* v = enum_table_lookup_member($1->u.varname, $3);
              if(v) { $$ = L(v); free($3); }
              else  $$ = L(ast_index($1, ast_string($3)));
          } else {
              $$ = L(ast_index($1, ast_string($3)));
          }
      }
    /* 生成器 .throw(err)：throw 是关键字，特殊处理，转换成 GenThrow(recv, err) */
    | postfix_expr DOT THROW LPAREN arg_list RPAREN {
          AstNode* recv = $1;
          AstNode* margs = $5;
          $$ = L(ast_call("GenThrow", margs ? ast_seq_front(margs, recv) : recv));
      }
    /* 安全方法调用 a?.b(x,y)：a 为 null 时返回 null */
    | postfix_expr SAFE_CALL ID LPAREN arg_list RPAREN {
          $$ = L(ast_safe_call($1, $3, $5));
      }
    /* 安全属性访问 a?.b → a 为 null 时返回 null */
    | postfix_expr SAFE_CALL ID { $$ = L(ast_safe_call($1, $3, NULL)); }
    ;

/* 字典字面量 {"k": v, name: 1, ...}；键为字符串字面量（支持模板）或标识符 */
map_items
    : %empty          { $$ = NULL; }
    | map_item        { $$ = $1; }
    | map_items COMMA map_item { $$ = ast_seq($1, $3); }
    ;

map_item
    : STRING_LIT COLON expr {
          AstNode* k = ast_string($1);
          free($1);
          $$ = ast_map_entry(k, $3);
      }
    | FSTRING_LIT COLON expr {
          AstNode* k = L(maybe_template($1));
          free($1);
          $$ = ast_map_entry(k, $3);
      }
    | ID COLON expr {
          $$ = ast_map_entry(ast_string(strdup($1)), $3);
      }
    | const_expr COLON expr {
          $$ = ast_map_entry($1, $3);
      }
    | ARRAY_OPEN expr RBRACKET COLON expr {
          $$ = ast_map_entry($2, $5);
      }
    | ELLIPSIS unary_expr {
          $$ = ast_spread($2);
      }
    ;

/* type 声明属性清单 */
type_prop_list
    : %empty                     { $$ = NULL; }
    | type_prop                  { $$ = $1; }
    | type_prop_list COMMA type_prop { $$ = ast_seq($1, $3); }
    ;
type_prop
    : ID COLON type_name         {
          char* sn = g_last_custom_type_name; g_last_custom_type_name = NULL;
          ValueType vt = $3;
          /* 命名字段类型（type_name_to_valtype 对自定义名返回 VAL_NONE）：
             struct/class → 对应引用类型；type 形状运行时即 map → VAL_MAP。
             此前统一落 VAL_NONE 被 cast 成 long long，嵌套值被销毁 */
          if(vt == VAL_NONE && sn) {
              if(struct_lookup(sn)) vt = VAL_STRUCT_PTR;
              else if(class_lookup(sn)) vt = VAL_CLASS_PTR;
              else if(type_lookup(sn)) vt = VAL_MAP;
          }
          type_prop_push($1, vt, 0, 0, sn);
          $$ = ast_none();
      }
    /* 泛型数组字段：<int> 或 <int>array（TOK_TYPE_ANNOT） */
    | ID COLON TOK_TYPE_ANNOT {
        type_prop_push($1, VAL_TYPED_ARRAY, 0, 0, NULL);
        $$ = ast_none();
    }
    | ID COLON TOK_TYPE_ANNOT ID {
        if(strcmp($4, "array") != 0) {
            yyerror("泛型数组字段后缀须为 'array'");
        }
        type_prop_push($1, VAL_TYPED_ARRAY, 0, 0, NULL);
        free($4);
        $$ = ast_none();
    }
    /* 泛型 map 字段：<K,V> 或 <K,V>map */
    | ID COLON LT map_generic_type COMMA map_generic_type GT {
        type_prop_push($1, VAL_MAP, 0, 0, NULL);
        $$ = ast_none();
    }
    | ID COLON LT map_generic_type COMMA map_generic_type GT ID {
        if(strcmp($8, "map") != 0) {
            yyerror("泛型 map 字段后缀须为 'map'");
        }
        type_prop_push($1, VAL_MAP, 0, 0, NULL);
        free($8);
        $$ = ast_none();
    }
    ;
struct_prop_list
    : %empty                     { $$ = NULL; }
    | struct_prop                { $$ = $1; }
    | struct_prop_list struct_prop { $$ = ast_seq($1, $2); }
    | struct_prop_list func_def  {
        /* struct 方法定义：保存到临时列表，struct 注册后再统一处理 */
        if($2 && $2->type == AST_FUNC_DEF) {
            /* 统一处理 self：无显式 self 时注入首参（约束=当前 struct 名），
             * 已有 self 则补 constraint。与 class 方法规则保持一致，
             * 否则无 self 方法 param_cnt=0、method_self_struct 缺失，方法体 self 未定义 */
            annotate_self_if_in_struct($2);
            g_struct_method_push($2);
        }
        $$ = ast_seq($1, $2);
      }
    ;
struct_prop
    : ID COLON builtin_type_name SEMI { struct_prop_push($1, $3, NULL); $$ = ast_none(); }
    | ID COLON ID SEMI {
        /* 容器字段：map/array 引用语义（走 PTR 栈），此前被当未知 struct 引用致值丢失 */
        if(strcmp($3, "map") == 0) {
            struct_prop_push($1, CAST_MAP, NULL);
            $$ = ast_none();
        } else if(strcmp($3, "array") == 0) {
            struct_prop_push($1, CAST_ARRAY, NULL);
            $$ = ast_none();
        }
        /* 自定义类型字段（引用语义，走 PTR 栈）：struct → STRUCT_PTR；class → CLASS_PTR。
         * 必须记录正确 CastKind，否则 receiver 被当 INT64，方法分派取错类型信息而崩溃 */
        else if(struct_lookup($3)) {
            struct_prop_push($1, CAST_STRUCT_PTR, $3);
            $$ = ast_none();
        } else if(class_lookup($3)) {
            struct_prop_push($1, CAST_CLASS_PTR, $3);
            $$ = ast_none();
        } else {
            /* 前向引用/未知：仍记录类型名按 STRUCT_PTR，方法分派靠名字；给出提示 */
            fprintf(stderr, "parse: 字段 \"%s\" 的类型 \"%s\" 尚未注册，按 struct 引用处理\n", $1, $3);
            struct_prop_push($1, CAST_STRUCT_PTR, $3);
            $$ = ast_none();
        }
      }
    /* 泛型数组字段：<int> 或 <int>array（词法器把 <int> 整体识别为 TOK_TYPE_ANNOT） */
    | ID COLON TOK_TYPE_ANNOT SEMI {
        struct_prop_push($1, CAST_TYPED_ARRAY, NULL);
        $$ = ast_none();
    }
    | ID COLON TOK_TYPE_ANNOT ID SEMI {
        if(strcmp($4, "array") != 0) {
            yyerror("泛型数组字段后缀须为 'array'");
        }
        struct_prop_push($1, CAST_TYPED_ARRAY, NULL);
        free($4);
        $$ = ast_none();
    }
    /* 泛型 map 字段：<K,V> 或 <K,V>map */
    | ID COLON LT map_generic_type COMMA map_generic_type GT SEMI {
        struct_prop_push($1, CAST_MAP, NULL);
        $$ = ast_none();
    }
    | ID COLON LT map_generic_type COMMA map_generic_type GT ID SEMI {
        if(strcmp($8, "map") != 0) {
            yyerror("泛型 map 字段后缀须为 'map'");
        }
        struct_prop_push($1, CAST_MAP, NULL);
        free($8);
        $$ = ast_none();
    }
    ;
/* class 声明属性清单（支持属性和方法定义） */
class_prop_list
    : %empty                     { $$ = NULL; }
    | class_prop                 { $$ = $1; }
    | class_prop_list class_prop { $$ = ast_seq($1, $2); }
    /* —— 以下 4 条为"首个成员即以 public/private/protected 开头"的无前缀版本。
          空 class_prop_list 起点（class_header • class_prop_list）若直接 shift
          access_modifier，bison 内核只保留 class_prop 字段规则，会丢失接
          static / func 的 class_prop_list 递归规则，导致 private static、
          private func 作为首成员时报语法错误。此处显式补齐。 —— */
    | access_modifier TOK_STATIC ID ASSIGN expr SEMI    {
        /* private/public/protected static count = 0（首成员） */
        make_static_prop($3, $5, $1);
        $$ = NULL;
      }
    | access_modifier TOK_STATIC ID COLON type_name ASSIGN expr SEMI    {
        /* private/public/protected static count: int = 0（首成员，带类型标注） */
        make_static_prop($3, $7, $1);
        $$ = NULL;
      }
    | access_modifier func_def    {
        /* private/public/protected func ...（首成员，实例方法） */
        if($2 && $2->type == AST_FUNC_DEF) {
            $2->u.func_def.is_class_method = 1;
            $2->u.func_def.access_modifier = $1;
            if(g_current_class_name && strcmp($2->u.func_def.name, g_current_class_name) == 0) {
                char* ctor_name = (char*)malloc(strlen(g_current_class_name) + 10);
                sprintf(ctor_name, "%s___init__", g_current_class_name);
                free($2->u.func_def.name);
                $2->u.func_def.name = ctor_name;
                g_class_constructor = $2;
            } else {
                g_class_method_push($2);
            }
        }
        $$ = $2;
      }
    | access_modifier TOK_STATIC func_def    {
        /* private/public/protected static func ...（首成员，静态方法） */
        if($3 && $3->type == AST_FUNC_DEF) {
            if(!type_lookup(g_current_class_name)) {
                char* saved_class_name = g_current_class_name;
          class_register(g_current_class_name, g_prop_names, g_prop_types, g_prop_access_modifiers, g_prop_const_flags, g_prop_struct_names, g_prop_n, g_current_class_parent, g_class_interfaces);
            }
            char* static_name = (char*)malloc(strlen(g_current_class_name) + strlen($3->u.func_def.name) + 2);
            sprintf(static_name, "%s_%s", g_current_class_name, $3->u.func_def.name);
            free($3->u.func_def.name);
            $3->u.func_def.name = static_name;
            $3->u.func_def.is_class_method = 0;
            $3->u.func_def.is_static_method = 1;
            $3->u.func_def.access_modifier = $1;
            strip_static_self($3);
            class_static_member_register(static_name, g_current_class_name, $1);
            RuntimeFunc* rf = compile_func_from_ast($3);
            if(rf) {
                Value fv;
                fv.type = VAL_FUNC;
                fv.v.func.ffi_func = NULL;
                fv.v.func.is_ffi = 0;
                fv.v.func.func_obj = (void*)rf;
                sym_set($3->u.func_def.name, fv);
            }
        }
        $$ = $3;
      }
    | class_prop_list annotation_list class_prop  {
        /* class 属性定义（字段级 @Getter/@Setter）：class_prop 已完成字段注册，
           把注解策略写入最后一个字段（idx = g_prop_n-1） */
        if(g_prop_n > 0 && apply_field_prop_annotations(g_prop_n - 1, $2)) YYABORT;
        $$ = ast_seq($1, $3);
      }
    | class_prop_list annotated_decl  {
        /* class 方法定义（支持注解）：保存到临时列表，class 注册后再统一处理 */
        if($2 && $2->type == AST_FUNC_DEF) {
            /* 标记为 class 方法，跳过顶层重复定义检查 */
            $2->u.func_def.is_class_method = 1;
            /* 构造函数：方法名与类名相同，不加入方法表，单独保存 */
            if(g_current_class_name && strcmp($2->u.func_def.name, g_current_class_name) == 0) {
                /* 给构造函数一个唯一的名字 <类名>___init__，避免 CC 模式下多个类的构造函数冲突 */
                char* ctor_name = (char*)malloc(strlen(g_current_class_name) + 10);
                sprintf(ctor_name, "%s___init__", g_current_class_name);
                free($2->u.func_def.name);
                $2->u.func_def.name = ctor_name;
                g_class_constructor = $2;
            } else {
                g_class_method_push($2);
            }
        }
        $$ = ast_seq($1, $2);
      }
    | class_prop_list annotation_list TOK_STATIC func_def  {
        /* @annotation static func ...：带注解的 class 静态方法定义 */
        if($4 && $4->type == AST_FUNC_DEF) {
            /* 提前注册 class 类型定义，以便静态方法中可以调用构造函数 */
            if(!type_lookup(g_current_class_name)) {
                char* saved_class_name = g_current_class_name;
          class_register(g_current_class_name, g_prop_names, g_prop_types, g_prop_access_modifiers, g_prop_const_flags, g_prop_struct_names, g_prop_n, g_current_class_parent, g_class_interfaces);
            }
            $4->u.func_def.annotations = $2;
            char* static_name = (char*)malloc(strlen(g_current_class_name) + strlen($4->u.func_def.name) + 2);
            sprintf(static_name, "%s_%s", g_current_class_name, $4->u.func_def.name);
            free($4->u.func_def.name);
            $4->u.func_def.name = static_name;
            $4->u.func_def.is_class_method = 0;
            $4->u.func_def.is_static_method = 1;
            /* 注册静态成员访问表（注解 static 方法默认 public） */
            class_static_member_register(static_name, g_current_class_name, 0);
            RuntimeFunc* rf = compile_func_from_ast($4);
            if(rf) {
                Value fv;
                fv.type = VAL_FUNC;
                fv.v.func.ffi_func = NULL;
                fv.v.func.is_ffi = 0;
                fv.v.func.func_obj = (void*)rf;
                sym_set($4->u.func_def.name, fv);
            }
        }
        $$ = ast_seq($1, $4);
      }
    | class_prop_list TOK_STATIC ID ASSIGN expr SEMI    {
        /* class 静态属性：static count = 0，全局名加类名前缀 */
        make_static_prop($3, $5, 0);
        $$ = $1;
      }
    | class_prop_list TOK_STATIC ID COLON type_name ASSIGN expr SEMI    {
        /* class 静态属性（带类型标注）：static count: int = 0 */
        AstNode* assign = make_static_prop($3, $7, 0);
        (void)assign; /* 类型校验在 AST_ASSIGN 统一处理 */
        $$ = $1;
      }
    | class_prop_list access_modifier TOK_STATIC ID ASSIGN expr SEMI    {
        /* private/public/protected static count = 0 */
        make_static_prop($4, $6, $2);
        $$ = $1;
      }
    | class_prop_list TOK_STATIC access_modifier ID ASSIGN expr SEMI    {
        /* static private/public/protected count = 0（修饰符顺序等价） */
        make_static_prop($4, $6, $3);
        $$ = $1;
      }
    | class_prop_list access_modifier TOK_STATIC ID COLON type_name ASSIGN expr SEMI    {
        /* private/public/protected static count: int = 0 */
        make_static_prop($4, $8, $2);
        $$ = $1;
      }
    | class_prop_list TOK_STATIC access_modifier ID COLON type_name ASSIGN expr SEMI    {
        /* static private/public/protected count: int = 0（修饰符顺序等价） */
        make_static_prop($4, $8, $3);
        $$ = $1;
      }
    | class_prop_list TOK_STATIC func_def  {
        /* class 静态方法定义：生成一个全局函数，函数名加上 class 名前缀 */
        if($3 && $3->type == AST_FUNC_DEF) {
            /* 提前注册 class 类型定义，以便静态方法中可以调用构造函数 */
            if(!type_lookup(g_current_class_name)) {
                char* saved_class_name = g_current_class_name;
          class_register(g_current_class_name, g_prop_names, g_prop_types, g_prop_access_modifiers, g_prop_const_flags, g_prop_struct_names, g_prop_n, g_current_class_parent, g_class_interfaces);
            }
            /* 给静态方法一个唯一的名字 <类名>_<方法名>，避免全局命名冲突 */
            char* static_name = (char*)malloc(strlen(g_current_class_name) + strlen($3->u.func_def.name) + 2);
            sprintf(static_name, "%s_%s", g_current_class_name, $3->u.func_def.name);
            free($3->u.func_def.name);
            $3->u.func_def.name = static_name;
            /* 标记为静态方法 */
            $3->u.func_def.is_class_method = 0;  // 不标记为 class 方法，作为普通全局函数处理
            $3->u.func_def.is_static_method = 1;
            strip_static_self($3);
            /* 注册静态成员访问表（默认 public） */
            class_static_member_register(static_name, g_current_class_name, 0);
            /* 立即注册到符号表，以便语义检查阶段能找到 */
            RuntimeFunc* rf = compile_func_from_ast($3);
            if(rf) {
                Value fv;
                fv.type = VAL_FUNC;
                fv.v.func.ffi_func = NULL;
                fv.v.func.is_ffi = 0;
                fv.v.func.func_obj = (void*)rf;
                sym_set($3->u.func_def.name, fv);
            }
        }
        $$ = ast_seq($1, $3);
      }
    | class_prop_list access_modifier func_def  {
        /* public/private/protected func ...：带访问修饰符的 class 方法定义 */
        if($3 && $3->type == AST_FUNC_DEF) {
            $3->u.func_def.is_class_method = 1;
            $3->u.func_def.access_modifier = $2;
            if(g_current_class_name && strcmp($3->u.func_def.name, g_current_class_name) == 0) {
                char* ctor_name = (char*)malloc(strlen(g_current_class_name) + 10);
                sprintf(ctor_name, "%s___init__", g_current_class_name);
                free($3->u.func_def.name);
                $3->u.func_def.name = ctor_name;
                g_class_constructor = $3;
            } else {
                g_class_method_push($3);
            }
        }
        $$ = ast_seq($1, $3);
      }
    | class_prop_list access_modifier TOK_STATIC func_def  {
        /* public/private/protected static func ...：带访问修饰符的 class 静态方法定义 */
        if($4 && $4->type == AST_FUNC_DEF) {
            /* 提前注册 class 类型定义，以便静态方法中可以调用构造函数 */
            if(!type_lookup(g_current_class_name)) {
                char* saved_class_name = g_current_class_name;
          class_register(g_current_class_name, g_prop_names, g_prop_types, g_prop_access_modifiers, g_prop_const_flags, g_prop_struct_names, g_prop_n, g_current_class_parent, g_class_interfaces);
            }
            char* static_name = (char*)malloc(strlen(g_current_class_name) + strlen($4->u.func_def.name) + 2);
            sprintf(static_name, "%s_%s", g_current_class_name, $4->u.func_def.name);
            free($4->u.func_def.name);
            $4->u.func_def.name = static_name;
            $4->u.func_def.is_class_method = 0;
            $4->u.func_def.is_static_method = 1;
            $4->u.func_def.access_modifier = $2;
            strip_static_self($4);
            class_static_member_register(static_name, g_current_class_name, $2);
            RuntimeFunc* rf = compile_func_from_ast($4);
            if(rf) {
                Value fv;
                fv.type = VAL_FUNC;
                fv.v.func.ffi_func = NULL;
                fv.v.func.is_ffi = 0;
                fv.v.func.func_obj = (void*)rf;
                sym_set($4->u.func_def.name, fv);
            }
        }
        $$ = ast_seq($1, $4);
      }
    | class_prop_list TOK_STATIC access_modifier func_def  {
        /* static public/private/protected func ...：修饰符顺序与上一条等价 */
        if($4 && $4->type == AST_FUNC_DEF) {
            if(!type_lookup(g_current_class_name)) {
                char* saved_class_name = g_current_class_name;
          class_register(g_current_class_name, g_prop_names, g_prop_types, g_prop_access_modifiers, g_prop_const_flags, g_prop_struct_names, g_prop_n, g_current_class_parent, g_class_interfaces);
            }
            char* static_name = (char*)malloc(strlen(g_current_class_name) + strlen($4->u.func_def.name) + 2);
            sprintf(static_name, "%s_%s", g_current_class_name, $4->u.func_def.name);
            free($4->u.func_def.name);
            $4->u.func_def.name = static_name;
            $4->u.func_def.is_class_method = 0;
            $4->u.func_def.is_static_method = 1;
            $4->u.func_def.access_modifier = $3;
            strip_static_self($4);
            class_static_member_register(static_name, g_current_class_name, $3);
            RuntimeFunc* rf = compile_func_from_ast($4);
            if(rf) {
                Value fv;
                fv.type = VAL_FUNC;
                fv.v.func.ffi_func = NULL;
                fv.v.func.is_ffi = 0;
                fv.v.func.func_obj = (void*)rf;
                sym_set($4->u.func_def.name, fv);
            }
        }
        $$ = ast_seq($1, $4);
      }
    ;
access_modifier
    : TOK_PUBLIC                   { $$ = 0LL; }  /* 0 = public */
    | TOK_PRIVATE                  { $$ = 1LL; }  /* 1 = private */
    | TOK_PROTECTED                { $$ = 2LL; }  /* 2 = protected */
    ;

class_prop
    : ID COLON type_name SEMI    {
          char* sn = g_last_custom_type_name; g_last_custom_type_name = NULL;
          ValueType vt = $3;
          /* 自定义类型字段：type_name 对自定义名返回 VAL_NONE，须按注册表修正
             （与 type_prop 规则一致），否则 cls 走错栈、指针被当整数 */
          if(vt == VAL_NONE && sn) {
              if(struct_lookup(sn)) vt = VAL_STRUCT_PTR;
              else if(class_lookup(sn)) vt = VAL_CLASS_PTR;
              else if(type_lookup(sn)) vt = VAL_MAP;
          }
          type_prop_push($1, vt, 0, 0, sn);
          $$ = ast_none();
      }
    | access_modifier ID COLON type_name SEMI    {
          char* sn = g_last_custom_type_name; g_last_custom_type_name = NULL;
          ValueType vt = $4;
          if(vt == VAL_NONE && sn) {
              if(struct_lookup(sn)) vt = VAL_STRUCT_PTR;
              else if(class_lookup(sn)) vt = VAL_CLASS_PTR;
              else if(type_lookup(sn)) vt = VAL_MAP;
          }
          type_prop_push($2, vt, $1, 0, sn);
          $$ = ast_none();
      }
    /* 字段初始化器：name: type = expr（构造期注入 ctor 执行） */
    | ID COLON type_name ASSIGN expr SEMI    {
          char* sn = g_last_custom_type_name; g_last_custom_type_name = NULL;
          ValueType vt = $3;
          if(vt == VAL_NONE && sn) {
              if(struct_lookup(sn)) vt = VAL_STRUCT_PTR;
              else if(class_lookup(sn)) vt = VAL_CLASS_PTR;
              else if(type_lookup(sn)) vt = VAL_MAP;
          }
          type_prop_push($1, vt, 0, 0, sn);
          record_field_init($1, ast_unary(OP_NONNULL_ASSERT, $5));
          $$ = ast_none();
      }
    | access_modifier ID COLON type_name ASSIGN expr SEMI    {
          char* sn = g_last_custom_type_name; g_last_custom_type_name = NULL;
          ValueType vt = $4;
          if(vt == VAL_NONE && sn) {
              if(struct_lookup(sn)) vt = VAL_STRUCT_PTR;
              else if(class_lookup(sn)) vt = VAL_CLASS_PTR;
              else if(type_lookup(sn)) vt = VAL_MAP;
          }
          type_prop_push($2, vt, $1, 0, sn);
          record_field_init($2, ast_unary(OP_NONNULL_ASSERT, $6));
          $$ = ast_none();
      }
    /* 可空字段 T?：字段槽仍为具体类型（对象是 C 布局，lumyr_field_get 依赖具体 valtype）。
     * 引用族（class/struct/map/string/array/bigint）以 NULL 指针表示 null，完全支持；
     * primitive(int/double) 槽无法容纳 null（运行时对象模型限制，见测试尾注）。
     * 初始器不做非空断言。 */
    | ID COLON type_name QMARK SEMI    {
          char* sn = g_last_custom_type_name; g_last_custom_type_name = NULL;
          ValueType vt = $3;
          if(vt == VAL_NONE && sn) {
              if(struct_lookup(sn)) vt = VAL_STRUCT_PTR;
              else if(class_lookup(sn)) vt = VAL_CLASS_PTR;
              else if(type_lookup(sn)) vt = VAL_MAP;
          }
          type_prop_push($1, vt, 0, 0, sn);
          $$ = ast_none();
      }
    | ID COLON type_name QMARK ASSIGN expr SEMI    {
          char* sn = g_last_custom_type_name; g_last_custom_type_name = NULL;
          ValueType vt = $3;
          if(vt == VAL_NONE && sn) {
              if(struct_lookup(sn)) vt = VAL_STRUCT_PTR;
              else if(class_lookup(sn)) vt = VAL_CLASS_PTR;
              else if(type_lookup(sn)) vt = VAL_MAP;
          }
          type_prop_push($1, vt, 0, 0, sn);
          record_field_init($1, $6);   /* 可空：不做断言；引用 null 落 NULL */
          $$ = ast_none();
      }
    /* const 字段：const id: Type（构造后不可修改），可带访问修饰 public const id: Type */
    | CONST ID COLON type_name SEMI    {
          char* sn = g_last_custom_type_name; g_last_custom_type_name = NULL;
          ValueType vt = $4;
          if(vt == VAL_NONE && sn) {
              if(struct_lookup(sn)) vt = VAL_STRUCT_PTR;
              else if(class_lookup(sn)) vt = VAL_CLASS_PTR;
              else if(type_lookup(sn)) vt = VAL_MAP;
          }
          type_prop_push($2, vt, 0, 1, sn);
          $$ = ast_none();
      }
    | access_modifier CONST ID COLON type_name SEMI    {
          char* sn = g_last_custom_type_name; g_last_custom_type_name = NULL;
          ValueType vt = $5;
          if(vt == VAL_NONE && sn) {
              if(struct_lookup(sn)) vt = VAL_STRUCT_PTR;
              else if(class_lookup(sn)) vt = VAL_CLASS_PTR;
              else if(type_lookup(sn)) vt = VAL_MAP;
          }
          type_prop_push($3, vt, $1, 1, sn);
          $$ = ast_none();
      }
    /* 泛型数组字段：<int> 或 <int>array（TOK_TYPE_ANNOT） */
    | ID COLON TOK_TYPE_ANNOT SEMI {
        type_prop_push($1, VAL_TYPED_ARRAY, 0, 0, NULL);
        $$ = ast_none();
    }
    | ID COLON TOK_TYPE_ANNOT ID SEMI {
        if(strcmp($4, "array") != 0) {
            yyerror("泛型数组字段后缀须为 'array'");
        }
        type_prop_push($1, VAL_TYPED_ARRAY, 0, 0, NULL);
        free($4);
        $$ = ast_none();
    }
    /* 泛型 map 字段：<K,V> 或 <K,V>map */
    | ID COLON LT map_generic_type COMMA map_generic_type GT SEMI {
        type_prop_push($1, VAL_MAP, 0, 0, NULL);
        $$ = ast_none();
    }
    | ID COLON LT map_generic_type COMMA map_generic_type GT ID SEMI {
        if(strcmp($8, "map") != 0) {
            yyerror("泛型 map 字段后缀须为 'map'");
        }
        type_prop_push($1, VAL_MAP, 0, 0, NULL);
        free($8);
        $$ = ast_none();
    }
    /* 带访问修饰符的泛型数组字段：private items: <int> */
    | access_modifier ID COLON TOK_TYPE_ANNOT SEMI {
        type_prop_push($2, VAL_TYPED_ARRAY, $1, 0, NULL);
        $$ = ast_none();
    }
    | access_modifier ID COLON TOK_TYPE_ANNOT ID SEMI {
        if(strcmp($5, "array") != 0) {
            yyerror("泛型数组字段后缀须为 'array'");
        }
        type_prop_push($2, VAL_TYPED_ARRAY, $1, 0, NULL);
        free($5);
        $$ = ast_none();
    }
    /* 带访问修饰符的泛型 map 字段：private m: <string,int> */
    | access_modifier ID COLON LT map_generic_type COMMA map_generic_type GT SEMI {
        type_prop_push($2, VAL_MAP, $1, 0, NULL);
        $$ = ast_none();
    }
    | access_modifier ID COLON LT map_generic_type COMMA map_generic_type GT ID SEMI {
        if(strcmp($9, "map") != 0) {
            yyerror("泛型 map 字段后缀须为 'map'");
        }
        type_prop_push($2, VAL_MAP, $1, 0, NULL);
        free($9);
        $$ = ast_none();
    }
    ;
builtin_type_name
    : TOK_STRING                 { $$ = CAST_STRING; }
    | TOK_INT                    { $$ = CAST_INT; }
    | TOK_DOUBLE                 { $$ = CAST_DOUBLE; }
    | TOK_BOOL                   { $$ = CAST_BOOL; }
    | TOK_CHAR                   { $$ = CAST_CHAR; }
    | TOK_ASCII                  { $$ = CAST_ASCII; }
    | TOK_BYTE                   { $$ = CAST_BYTE; }
    | TOK_INT8                   { $$ = CAST_INT8; }
    | TOK_INT16                  { $$ = CAST_INT16; }
    | TOK_INT32                  { $$ = CAST_INT32; }
    | TOK_INT64                  { $$ = CAST_INT64; }
    | TOK_UINT8                  { $$ = CAST_UINT8; }
    | TOK_UINT16                 { $$ = CAST_UINT16; }
    | TOK_UINT32                 { $$ = CAST_UINT32; }
    | TOK_UINT64                 { $$ = CAST_UINT64; }
    | TOK_UINT                   { $$ = CAST_UINT64; }
    | TOK_LONG                   { $$ = CAST_LONG; }
    | TOK_LONGLONG               { $$ = CAST_LONGLONG; }
    | TOK_FLOAT                  { $$ = CAST_FLOAT; }
    | TOK_ULONG                  { $$ = CAST_ULONG; }
    | TOK_UCHAR                  { $$ = CAST_UCHAR; }
    | TOK_SHORT                  { $$ = CAST_SHORT; }
    | TOK_USHORT                 { $$ = CAST_USHORT; }
    | TOK_SIZE_T                 { $$ = CAST_SIZE_T; }
    | TOK_SSIZE_T                { $$ = CAST_SSIZE_T; }
    | TOK_VOID                   { $$ = CAST_VOID; }
    | TOK_LONG_DOUBLE            { $$ = CAST_LONG_DOUBLE; }
    | TOK_PTR                    { $$ = CAST_PTR; }
    ;
type_name
    : builtin_type_name          { g_last_custom_type_name = NULL; $$ = castkind_to_valtype($1); }
    | ID                         { g_last_custom_type_name = $1; $$ = type_name_to_valtype($1); }
    ;

/* 类型名字符串（用于 FFI 参数类型标注，直接返回原始字符串，避免 ValueType 枚举冲突） */
type_name_str
    : ID                         { $$ = $1; }
    | builtin_type_name          { $$ = castkind_to_name($1); }
    ;

/* 双泛型 map <K,V> 的类型实参：内建类型直接取 CastKind；自定义类型查注册表
   （struct→STRUCT_PTR、class→CLASS_PTR、type 形状→MAP）。
   此前裸走 type_name：自定义名返回 VAL_NONE，被默认映射成 CAST_LONGLONG，
   class/struct 实例在 entry cast 中被字符串转整数而销毁 */
map_generic_type
    : builtin_type_name          { $$ = $1; }
    | ID                         {
          if(strcmp($1, "map") == 0) {
              $$ = CAST_MAP;
              free($1);
          } else if(strcmp($1, "array") == 0) {
              $$ = CAST_ARRAY;
              free($1);
          } else if(strcmp($1, "bigint") == 0) {
              $$ = CAST_BIGINT;
              free($1);
          } else if(strcmp($1, "decimal") == 0) {
              $$ = CAST_DECIMAL;
              free($1);
          } else if(strcmp($1, "bitdecimal") == 0) {
              $$ = CAST_BITDECIMAL;
              free($1);
          } else if(struct_lookup($1)) {
              $$ = CAST_STRUCT_PTR;
              free($1);
          } else if(class_lookup($1)) {
              $$ = CAST_CLASS_PTR;
              free($1);
          } else if(type_lookup($1)) {
              $$ = CAST_MAP;
              free($1);
          } else {
              yyerror("未定义类型");
              $$ = CAST_NONE;
          }
      }
    ;

/* 枚举成员（C 风格自动递增）：
 *   ID            → 值 = g_enum_next_val，随后自增
 *   ID = INTEGER  → 值 = INTEGER，并将 g_enum_next_val 重置为 INTEGER+1
 * 例：enum Color { RED, GREEN }   → {RED:0, GREEN:1}
 *     enum Op { ADD=1, SUB, MUL } → {ADD:1, SUB:2, MUL:3} */
enum_members
    : %empty                     { $$ = NULL; }
    | enum_member                { $$ = $1; }
    | enum_members COMMA enum_member { $$ = ast_seq($1, $3); }
    ;
enum_member
    : ID                         { $$ = ast_map_entry(ast_string(strdup($1)), ast_int(g_enum_next_val)); free($1); g_enum_next_val++; }
    | ID ASSIGN INTEGER          { $$ = ast_map_entry(ast_string(strdup($1)), ast_int($3)); free($1); g_enum_next_val = $3 + 1; }
    | ID ASSIGN MINUS INTEGER    { $$ = ast_map_entry(ast_string(strdup($1)), ast_int(-$4)); free($1); g_enum_next_val = -$4 + 1; }
    ;

unary_expr
    : power_expr
    | PLUSPLUS unary_expr     { $$ = ast_unary(OP_PRE_INC, $2); }
    | MINUSMINUS unary_expr   { $$ = ast_unary(OP_PRE_DEC, $2); }
    | PLUS unary_expr         { $$ = ast_unary(OP_UNARY_PLUS, $2); }
    | MINUS unary_expr        { $$ = ast_unary(OP_UNARY_MINUS, $2); }
    | NOT unary_expr          { $$ = ast_unary(OP_LOGIC_NOT, $2); }
    | BNOT unary_expr         { $$ = ast_unary(OP_BIT_NOT, $2); }
    ;

/* 幂层：右结合，位于一元运算符之下（-2**2 = -(2**2)），指数为 unary（支持负指数） */
power_expr
    : postfix_expr
    | postfix_expr POW unary_expr   { $$ = ast_binop(OP_POW, $1, $3); }
    ;

multiplicative_expr
    : unary_expr
    | multiplicative_expr MUL unary_expr  { $$ = ast_binop(OP_MUL, $1, $3); }
    | multiplicative_expr DIV unary_expr  { $$ = ast_binop(OP_DIV, $1, $3); }
    | multiplicative_expr MOD unary_expr  { $$ = ast_binop(OP_MOD, $1, $3); }
    ;

additive_expr
    : multiplicative_expr
    | additive_expr PLUS multiplicative_expr  { $$ = ast_binop(OP_ADD, $1, $3); }
    | additive_expr MINUS multiplicative_expr { $$ = ast_binop(OP_SUB, $1, $3); }
    ;

/* 移位层：<< >>（优先级低于加减） */
shift_expr
    : additive_expr
    | shift_expr SHL additive_expr    { $$ = ast_binop(OP_SHL, $1, $3); }
    | shift_expr SHR additive_expr    { $$ = ast_binop(OP_SHR, $1, $3); }
    ;

/* 关系层：> < >= <=（支持链式 a<b<c → (a<b)&&(b<c)） */
comparison_expr
    : shift_expr
    | comparison_expr GT shift_expr    { $$ = chain_cmp($1, OP_GT, $3); }
    | comparison_expr LT shift_expr    { $$ = chain_cmp($1, OP_LT, $3); }
    | comparison_expr GE shift_expr    { $$ = chain_cmp($1, OP_GE, $3); }
    | comparison_expr LE shift_expr    { $$ = chain_cmp($1, OP_LE, $3); }
    ;

/* 相等层：== !=（支持链式 a==b==c → (a==b)&&(b==c)） */
equality_expr
    : comparison_expr
    | equality_expr EQ comparison_expr    { $$ = chain_cmp($1, OP_EQ, $3); }
    | equality_expr NE comparison_expr    { $$ = chain_cmp($1, OP_NE, $3); }
    | equality_expr TOK_IMPLEMENTS ID     { $$ = ast_binop(OP_IMPLEMENTS, $1, ast_string($3)); }
    ;

/* 位与：& */
bit_and_expr
    : equality_expr
    | bit_and_expr BAND equality_expr  { $$ = ast_binop(OP_BIT_AND, $1, $3); }
    ;

/* 位异或：^ */
bit_xor_expr
    : bit_and_expr
    | bit_xor_expr BXOR bit_and_expr   { $$ = ast_binop(OP_BIT_XOR, $1, $3); }
    ;

/* 位或：| */
bit_or_expr
    : bit_xor_expr
    | bit_or_expr BOR bit_xor_expr     { $$ = ast_binop(OP_BIT_OR, $1, $3); }
    ;

logic_and_expr
    : bit_or_expr
    | logic_and_expr AND bit_or_expr  { $$ = ast_binop(OP_LOGIC_AND, $1, $3); }
    ;

logic_or_expr
    : logic_and_expr
    | logic_or_expr OR logic_and_expr     { $$ = ast_binop(OP_LOGIC_OR, $1, $3); }
    | logic_or_expr NULL_COALESCE logic_and_expr { $$ = ast_null_coalesce($1, $3); }
    ;

ternary_expr
    : logic_or_expr
    | logic_or_expr QMARK expr COLON ternary_expr  { $$ = ast_ternary($1, $3, $5); }
    ;

assignment_expr
    : ternary_expr
    | ID ASSIGN assignment_expr  { $$ = ast_assign($1, $3); }
    /* 变量声明带类型标注（a: Type = expr）已移到 closed_stmt 语句层：若作为
     * assignment_expr 产生式，其 ID COLON 开头会与三元 cond ? a : b 的 COLON
     * 在 LALR 状态冲突——ID 后的 COLON 被默认 shift 进声明，三元分支被误当
     * 类型名，报"未定义类型"。 */
    /* destruct_lhs 移到 closed_stmt 层面，避免与函数参数列表的 COMMA 冲突 */
    | ID PLUSEQ assignment_expr  { $$ = ast_assign($1, ast_binop(OP_ADD, ast_var($1), $3)); }
    | ID MINUSEQ assignment_expr { $$ = ast_assign($1, ast_binop(OP_SUB, ast_var($1), $3)); }
    | ID MULEQ assignment_expr   { $$ = ast_assign($1, ast_binop(OP_MUL, ast_var($1), $3)); }
    | ID DIVEQ assignment_expr   { $$ = ast_assign($1, ast_binop(OP_DIV, ast_var($1), $3)); }
    | ID MODEQ assignment_expr   { $$ = ast_assign($1, ast_binop(OP_MOD, ast_var($1), $3)); }
    /* 位复合赋值：x &= y ⇔ x = x & y（其余同理） */
    | ID BANDEQ assignment_expr  { $$ = ast_assign($1, ast_binop(OP_BIT_AND, ast_var($1), $3)); }
    | ID BOREQ assignment_expr   { $$ = ast_assign($1, ast_binop(OP_BIT_OR,  ast_var($1), $3)); }
    | ID BXOREQ assignment_expr  { $$ = ast_assign($1, ast_binop(OP_BIT_XOR, ast_var($1), $3)); }
    | ID SHLEQ assignment_expr   { $$ = ast_assign($1, ast_binop(OP_SHL,     ast_var($1), $3)); }
    | ID SHREQ assignment_expr   { $$ = ast_assign($1, ast_binop(OP_SHR,     ast_var($1), $3)); }
    | postfix_expr LBRACKET expr RBRACKET ASSIGN assignment_expr
        { $$ = ast_index_assign($1, $3, $6); }
    | postfix_expr LBRACKET expr RBRACKET PLUSEQ assignment_expr
        { $$ = ast_index_assign($1, $3, ast_binop(OP_ADD, ast_index(ast_clone_node($1), ast_clone_node($3)), $6)); }
    | postfix_expr LBRACKET expr RBRACKET MINUSEQ assignment_expr
        { $$ = ast_index_assign($1, $3, ast_binop(OP_SUB, ast_index(ast_clone_node($1), ast_clone_node($3)), $6)); }
    | postfix_expr LBRACKET expr RBRACKET MULEQ assignment_expr
        { $$ = ast_index_assign($1, $3, ast_binop(OP_MUL, ast_index(ast_clone_node($1), ast_clone_node($3)), $6)); }
    | postfix_expr LBRACKET expr RBRACKET DIVEQ assignment_expr
        { $$ = ast_index_assign($1, $3, ast_binop(OP_DIV, ast_index(ast_clone_node($1), ast_clone_node($3)), $6)); }
    | postfix_expr LBRACKET expr RBRACKET MODEQ assignment_expr
        { $$ = ast_index_assign($1, $3, ast_binop(OP_MOD, ast_index(ast_clone_node($1), ast_clone_node($3)), $6)); }
    /* 数组元素位复合赋值：a[i] &= v ⇔ a[i] = a[i] & v（其余同理） */
    | postfix_expr LBRACKET expr RBRACKET BANDEQ assignment_expr
        { $$ = ast_index_assign($1, $3, ast_binop(OP_BIT_AND, ast_index(ast_clone_node($1), ast_clone_node($3)), $6)); }
    | postfix_expr LBRACKET expr RBRACKET BOREQ assignment_expr
        { $$ = ast_index_assign($1, $3, ast_binop(OP_BIT_OR,  ast_index(ast_clone_node($1), ast_clone_node($3)), $6)); }
    | postfix_expr LBRACKET expr RBRACKET BXOREQ assignment_expr
        { $$ = ast_index_assign($1, $3, ast_binop(OP_BIT_XOR, ast_index(ast_clone_node($1), ast_clone_node($3)), $6)); }
    | postfix_expr LBRACKET expr RBRACKET SHLEQ assignment_expr
        { $$ = ast_index_assign($1, $3, ast_binop(OP_SHL,     ast_index(ast_clone_node($1), ast_clone_node($3)), $6)); }
    | postfix_expr LBRACKET expr RBRACKET SHREQ assignment_expr
        { $$ = ast_index_assign($1, $3, ast_binop(OP_SHR,     ast_index(ast_clone_node($1), ast_clone_node($3)), $6)); }
    /* 属性赋值 m.key = v → m["key"] = v */
    | postfix_expr DOT ID ASSIGN assignment_expr
        { $$ = ast_index_assign($1, ast_string($3), $5); }
    ;

expr
    : assignment_expr
    ;

struct_header: TOK_STRUCT ID LBRACE {
          /* 在 LBRACE 时就设置 g_current_struct_name，这样方法定义时就能获取到 */
          g_current_struct_name = $2;
          $$ = NULL;
      }
    ;
/* 可选 public 前缀：public class X（类默认全局可见，public 仅为显式标注） */
class_start
    : TOK_CLASS                 { $$ = NULL; }
    | TOK_PUBLIC TOK_CLASS      { $$ = NULL; }
    ;
class_header: class_start ID LBRACE {
          /* 在 LBRACE 时就设置 g_current_class_name，这样方法定义时就能获取到 */
          g_current_class_name = $2;
          g_current_class_is_abstract = 0;
          /* 无继承：必须显式清空，防止上一个 extends 类的父类名残留
           * （残留会让 apply_field_initializers 误判本类有带参父类 ctor） */
          g_current_class_parent = NULL;
          $$ = NULL;
      }
    ;
abstract_class_header: TOK_ABSTRACT class_start ID LBRACE {
          /* 抽象类定义：标记为抽象类，不能被实例化（abstract public class 顺序） */
          g_current_class_name = $3;
          g_current_class_is_abstract = 1;
          g_current_class_parent = NULL;
          $$ = NULL;
      }
    | class_start TOK_ABSTRACT TOK_CLASS ID LBRACE {
          /* public abstract class 顺序（等价写法） */
          g_current_class_name = $4;
          g_current_class_is_abstract = 1;
          g_current_class_parent = NULL;
          $$ = NULL;
      }
    ;
class_header_inherit: class_start ID TOK_EXTENDS ID LBRACE {
          /* 在 LBRACE 时就设置 g_current_class_name 和 g_current_class_parent */
          g_current_class_name = $2;
          g_current_class_parent = $4;
          $$ = NULL;
      }
    ;
class_header_implements: class_start ID TOK_IMPLEMENTS interface_list LBRACE {
          /* class 实现接口：设置 g_current_class_name 和接口列表 */
          g_current_class_name = $2;
          g_current_class_parent = NULL;
          /* 从 interface_list 中提取接口名 */
          g_class_interfaces = NULL;
          g_class_ninterfaces = 0;
          AstNode* _iface = $4;
          while(_iface) {
              if(_iface->u.param.name) {
                  g_class_interfaces = (char**)realloc(g_class_interfaces, (size_t)(g_class_ninterfaces + 1) * sizeof(char*));
                  g_class_interfaces[g_class_ninterfaces++] = strdup(_iface->u.param.name);
              }
              _iface = _iface->u.param.next;
          }
          /* 添加 NULL 终止符，因为 class_register 函数通过 while(interfaces[nifaces]) nifaces++ 来计算接口数量 */
          if(g_class_interfaces) {
              g_class_interfaces = (char**)realloc(g_class_interfaces, (size_t)(g_class_ninterfaces + 1) * sizeof(char*));
              g_class_interfaces[g_class_ninterfaces] = NULL;
          }
          $$ = NULL;
      }
    ;
class_header_inherit_implements: class_start ID TOK_EXTENDS ID TOK_IMPLEMENTS interface_list LBRACE {
          /* class 继承并实现接口：设置 g_current_class_name、g_current_class_parent 和接口列表 */
          g_current_class_name = $2;
          g_current_class_parent = $4;
          /* 从 interface_list 中提取接口名 */
          g_class_interfaces = NULL;
          g_class_ninterfaces = 0;
          AstNode* _iface = $6;
          while(_iface) {
              if(_iface->u.param.name) {
                  g_class_interfaces = (char**)realloc(g_class_interfaces, (size_t)(g_class_ninterfaces + 1) * sizeof(char*));
                  g_class_interfaces[g_class_ninterfaces++] = strdup(_iface->u.param.name);
              }
              _iface = _iface->u.param.next;
          }
          /* 添加 NULL 终止符，因为 class_register 函数通过 while(interfaces[nifaces]) nifaces++ 来计算接口数量 */
          if(g_class_interfaces) {
              g_class_interfaces = (char**)realloc(g_class_interfaces, (size_t)(g_class_ninterfaces + 1) * sizeof(char*));
              g_class_interfaces[g_class_ninterfaces] = NULL;
          }
          $$ = NULL;
      }
    ;

%%

void yyerror(const char* s){
    fprintf(stderr,"语法错误(第%d行): %s\n", yylineno, s);
}
