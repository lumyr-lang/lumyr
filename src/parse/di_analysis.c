/*
 * DI 元信息收集与依赖图分析（编译期）—— 实现
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "di_analysis.h"
#include "annotation/lm_annotation.h"
#include "lumyr_log.h"
#include "ast/ast_types.h"
#include "ast/ast_node_type.h"

/* 支持的 HTTP 映射注解名（动词） */
static const char* kMapAnn[] = { "Get", "Post", "Put", "Delete", "Patch", "Head" };
#define N_MAP_ANN ((int)(sizeof(kMapAnn) / sizeof(kMapAnn[0])))

static DiRegistry* g_last = NULL;
/* 收集期是否发生过错误（collect_class 置 1） */
static int g_collect_error = 0;

/* ---------- 基础工具 ---------- */

/* 左倾 AST_SEQ 树按源码顺序线性化到数组（first 递归后 second） */
static void seq_flatten(AstNode* n, AstNode*** arr, int* cnt, int* cap) {
    if(!n) return;
    if(n->type == AST_SEQ) {
        seq_flatten(n->u.seq.first, arr, cnt, cap);
        seq_flatten(n->u.seq.second, arr, cnt, cap);
        return;
    }
    if(*cnt >= *cap) {
        *cap = *cap ? *cap * 2 : 8;
        *arr = (AstNode**)realloc(*arr, (size_t)*cap * sizeof(AstNode*));
    }
    (*arr)[(*cnt)++] = n;
}

/* 取注解实参（顺序数组）；返回长度 */
static int ann_args(const AnnotationInfo* ann, AstNode*** out) {
    int cnt = 0, cap = 0;
    *out = NULL;
    if(ann && ann->args) seq_flatten(ann->args, out, &cnt, &cap);
    return cnt;
}

/* 从动词实参节点取 HTTP 动词字符串（返回 strdup）：
 * 支持字符串字面量 "GET" 与 HttpMethod.GET 常量写法 */
static char* extract_verb(AstNode* node) {
    if(node->type == AST_STRING)
        return strdup(node->u.sval ? node->u.sval : "");
    if(node->type == AST_VAR && node->u.varname) {
        const char* p = strrchr(node->u.varname, '.');
        return strdup(p ? p + 1 : node->u.varname);
    }
    return NULL;
}

/* 收集形参链表，skip_self=1 时跳过第一个（self） */
static void collect_params(AstNode* list, int skip_self, DiParam** out, int* nout) {
    *out = NULL;
    *nout = 0;
    AstNode* p = list;
    if(skip_self && p) p = p->u.param.next;
    int cap = 0;
    for(; p; p = p->u.param.next) {
        if(*nout >= cap) {
            cap = cap ? cap * 2 : 4;
            *out = (DiParam*)realloc(*out, (size_t)cap * sizeof(DiParam));
        }
        DiParam* dp = &(*out)[(*nout)++];
        dp->name = strdup(p->u.param.name ? p->u.param.name : "");
        dp->type = p->u.param.constraint ? strdup(p->u.param.constraint) : NULL;
    }
}

/* 在类方法中选定 DI 构造器：取形参最多的构造器重载；
 * 同形参数的两个重载并列 → 返回 -2（歧义）；无构造器 → *node=NULL。
 * 构造器 AST 来自 TypeDef.ctor_overload_nodes（含主构造）。 */
static int pick_ctor(TypeDef* td, AstNode** node, int* argc) {
    *node = NULL;
    *argc = -1;
    AstNode* tie = NULL;
    for(int i = 0; i < td->nctor_overload_nodes; i++) {
        AstNode* m = td->ctor_overload_nodes[i];
        if(!m || m->type != AST_FUNC_DEF) continue;
        int n = 0;
        for(AstNode* q = m->u.func_def.params; q; q = q->u.param.next) n++;
        if(n > 0) n--;  /* 去 self */
        if(n > *argc) { *argc = n; *node = m; tie = NULL; }
        else if(n == *argc) { tie = m; }
    }
    if(tie) return -2;
    return 0;
}

/* ---------- 释放 ---------- */

static void free_registry(DiRegistry* r) {
    if(!r) return;
    for(int i = 0; i < r->ncontrollers; i++) {
        DiController* c = &r->controllers[i];
        free(c->class_name);
        free(c->base_path);
        for(int k = 0; k < c->nroutes; k++) {
            DiRoute* rt = &c->routes[k];
            free(rt->verb);
            free(rt->path);
            free(rt->method_name);
            free(rt->return_type);
            for(int p = 0; p < rt->nparams; p++) {
                free(rt->params[p].name);
                free(rt->params[p].type);
            }
            free(rt->params);
        }
        free(c->routes);
        for(int p = 0; p < c->ndeps; p++) {
            free(c->deps[p].name);
            free(c->deps[p].type);
        }
        free(c->deps);
    }
    free(r->controllers);
    for(int i = 0; i < r->nservices; i++) {
        DiService* s = &r->services[i];
        free(s->class_name);
        free(s->service_name);
        for(int p = 0; p < s->ndeps; p++) {
            free(s->deps[p].name);
            free(s->deps[p].type);
        }
        free(s->deps);
    }
    free(r->services);
    free(r->topo);
    free(r);
}

/* ---------- 控制器/服务收集 ---------- */

/* 校验映射注解参数并产出一条路由；返回 0 成功，1 用户错误 */
static int build_route(const char* class_name, AstNode* method_node,
                       const char* ann_name, const AnnotationInfo* ann,
                       DiRoute* rt) {
    AstNode** args = NULL;
    int nargs = ann_args(ann, &args);
    memset(rt, 0, sizeof(*rt));

    if(strcmp(ann_name, "Route") == 0) {
        if(nargs < 2) {
            LOG_ERROR("[DI] 类 %s 的方法 %s：@Route 需要 (动词, 路径) 两个参数 / "
                      "class %s method %s: @Route requires (verb, path) arguments\n",
                      class_name, method_node->u.func_def.name,
                      class_name, method_node->u.func_def.name);
            free(args);
            return 1;
        }
        rt->verb = extract_verb(args[0]);
        if(!rt->verb) {
            LOG_ERROR("[DI] 类 %s 的方法 %s：@Route 首参必须是 HTTP 方法字符串或 HttpMethod 常量 / "
                      "class %s method %s: first @Route arg must be an HTTP method string or HttpMethod constant\n",
                      class_name, method_node->u.func_def.name,
                      class_name, method_node->u.func_def.name);
            free(args);
            return 1;
        }
        if(args[1]->type != AST_STRING) {
            LOG_ERROR("[DI] 类 %s 的方法 %s：@Route 路径必须是字符串字面量 / "
                      "class %s method %s: @Route path must be a string literal\n",
                      class_name, method_node->u.func_def.name,
                      class_name, method_node->u.func_def.name);
            free(rt->verb);
            free(args);
            return 1;
        }
        rt->path = strdup(args[1]->u.sval ? args[1]->u.sval : "");
    } else {
        /* @Get/@Post...：至多一个路径参数 */
        if(nargs >= 1) {
            if(args[0]->type != AST_STRING) {
                LOG_ERROR("[DI] 类 %s 的方法 %s：@%s 路径必须是字符串字面量 / "
                          "class %s method %s: @%s path must be a string literal\n",
                          class_name, method_node->u.func_def.name, ann_name,
                          class_name, method_node->u.func_def.name, ann_name);
                free(args);
                return 1;
            }
            rt->path = strdup(args[0]->u.sval ? args[0]->u.sval : "");
        } else {
            rt->path = strdup("");
        }
        rt->verb = strdup(ann_name);
    }
    free(args);

    /* 动词统一大写 */
    for(char* q = rt->verb; *q; q++) *q = (char)toupper((unsigned char)*q);

    rt->method_name = strdup(method_node->u.func_def.name
                             ? method_node->u.func_def.name : "");
    rt->return_type = method_node->u.func_def.ret_type_name
                      ? strdup(method_node->u.func_def.ret_type_name) : NULL;
    collect_params(method_node->u.func_def.params, 1, &rt->params, &rt->nparams);

    if(method_node->u.func_def.is_abstract_method) {
        LOG_ERROR("[DI] 类 %s 的映射方法 %s 不能是抽象方法 / "
                  "class %s: mapped method %s must not be abstract\n",
                  class_name, rt->method_name, class_name, rt->method_name);
        return 1;
    }
    return 0;
}

/* 释放半构建完成的控制器（错误路径用） */
static void free_controller_partial(DiController* c) {
    free(c->class_name);
    free(c->base_path);
    for(int z = 0; z < c->nroutes; z++) {
        DiRoute* x = &c->routes[z];
        free(x->verb); free(x->path); free(x->method_name); free(x->return_type);
        for(int p = 0; p < x->nparams; p++) { free(x->params[p].name); free(x->params[p].type); }
        free(x->params);
    }
    free(c->routes);
    for(int p = 0; p < c->ndeps; p++) { free(c->deps[p].name); free(c->deps[p].type); }
    free(c->deps);
}

/* 释放半构建完成的服务（错误路径用） */
static void free_service_partial(DiService* s) {
    free(s->class_name);
    free(s->service_name);
    for(int p = 0; p < s->ndeps; p++) { free(s->deps[p].name); free(s->deps[p].type); }
    free(s->deps);
}

/* 遍历每个已注册类型，收集控制器与服务 */
static void collect_class(const char* name, TypeDef* td, void* ud) {
    DiRegistry* r = (DiRegistry*)ud;
    const AnnotationInfo* annC = annotation_lookup_class(name, "Controller");
    const AnnotationInfo* annS = annotation_lookup_class(name, "Service");
    if(annC && annS) {
        LOG_ERROR("[DI] 类 %s 不能同时标注 @Controller 和 @Service / "
                  "class %s cannot be annotated with both @Controller and @Service\n",
                  name, name);
        g_collect_error = 1;
        return;
    }
    if(!annC && !annS) return;

    if(annC) {
        DiController c;
        memset(&c, 0, sizeof(c));
        c.class_name = strdup(name);

        AstNode** cargs = NULL;
        int nc = ann_args(annC, &cargs);
        if(nc >= 1 && cargs[0]->type != AST_STRING) {
            LOG_ERROR("[DI] 类 %s：@Controller basePath 必须是字符串字面量 / "
                      "class %s: @Controller basePath must be a string literal\n",
                      name, name);
            free(cargs);
            free_controller_partial(&c);
            g_collect_error = 1;
            return;
        }
        c.base_path = strdup(nc >= 1 && cargs[0]->u.sval ? cargs[0]->u.sval : "");
        free(cargs);

        /* 逐个方法查映射注解；无映射注解的方法不入表 */
        int rcap = 0;
        for(int i = 0; i < td->nmethods; i++) {
            AstNode* m = td->method_nodes[i];
            if(!m) continue;
            const char* hit_ann = NULL;
            const AnnotationInfo* hit_info = NULL;
            for(int k = 0; k < N_MAP_ANN; k++) {
                const AnnotationInfo* a = annotation_lookup_func(name, m->u.func_def.name, kMapAnn[k]);
                if(a) {
                    if(hit_info) {
                        LOG_ERROR("[DI] 类 %s 的方法 %s 声明了多个映射注解，一个方法只能映射一个路由 / "
                                  "class %s method %s has multiple mapping annotations; only one allowed\n",
                                  name, m->u.func_def.name, name, m->u.func_def.name);
                        free_controller_partial(&c);
                        g_collect_error = 1;
                        return;
                    }
                    hit_ann = kMapAnn[k];
                    hit_info = a;
                }
            }
            /* @Route 参与同样的"只能一个"校验 */
            const AnnotationInfo* aRoute = annotation_lookup_func(name, m->u.func_def.name, "Route");
            if(aRoute) {
                if(hit_info) {
                    LOG_ERROR("[DI] 类 %s 的方法 %s 声明了多个映射注解，一个方法只能映射一个路由 / "
                              "class %s method %s has multiple mapping annotations; only one allowed\n",
                              name, m->u.func_def.name, name, m->u.func_def.name);
                    free_controller_partial(&c);
                    g_collect_error = 1;
                    return;
                }
                hit_ann = "Route";
                hit_info = aRoute;
            }
            if(!hit_info) continue;

            if(c.nroutes >= rcap) {
                rcap = rcap ? rcap * 2 : 4;
                c.routes = (DiRoute*)realloc(c.routes, (size_t)rcap * sizeof(DiRoute));
            }
            if(build_route(name, m, hit_ann, hit_info, &c.routes[c.nroutes]) != 0) {
                free_controller_partial(&c);
                g_collect_error = 1;
                return;
            }
            c.nroutes++;
        }

        /* 选定构造器（控制器依赖必须全部可解析为服务） */
        AstNode* ctor = NULL;
        int cargc = 0;
        if(pick_ctor(td, &ctor, &cargc) == -2) {
            LOG_ERROR("[DI] 控制器 %s 存在形参数相同的多个构造器，DI 无法选择，请合并 / "
                      "controller %s has multiple constructors with equal arity; DI cannot choose\n",
                      name, name);
            free_controller_partial(&c);
            g_collect_error = 1;
            return;
        }
        if(ctor) collect_params(ctor->u.func_def.params, 1, &c.deps, &c.ndeps);

        r->controllers = (DiController*)realloc(r->controllers,
                            (size_t)(r->ncontrollers + 1) * sizeof(DiController));
        r->controllers[r->ncontrollers++] = c;
        return;
    }

    /* ---- @Service ---- */
    DiService s;
    memset(&s, 0, sizeof(s));
    s.class_name = strdup(name);

    AstNode** sargs = NULL;
    int ns = ann_args(annS, &sargs);
    if(ns >= 1 && sargs[0]->type != AST_STRING) {
        LOG_ERROR("[DI] 类 %s：@Service 服务名必须是字符串字面量 / "
                  "class %s: @Service name must be a string literal\n",
                  name, name);
        free(sargs);
        free_service_partial(&s);
        g_collect_error = 1;
        return;
    }
    s.service_name = strdup(ns >= 1 && sargs[0]->u.sval ? sargs[0]->u.sval : name);
    free(sargs);

    AstNode* ctor = NULL;
    int cargc = 0;
    if(pick_ctor(td, &ctor, &cargc) == -2) {
        LOG_ERROR("[DI] 服务 %s 存在形参数相同的多个构造器，DI 无法选择，请合并 / "
                  "service %s has multiple constructors with equal arity; DI cannot choose\n",
                  name, name);
        free_service_partial(&s);
        g_collect_error = 1;
        return;
    }
    if(ctor) collect_params(ctor->u.func_def.params, 1, &s.deps, &s.ndeps);

    r->services = (DiService*)realloc(r->services,
                    (size_t)(r->nservices + 1) * sizeof(DiService));
    r->services[r->nservices++] = s;
}

/* ---------- 依赖图 ---------- */

/* 按类名（类型）找服务下标，无则 -1 */
static int find_service_by_class(DiRegistry* r, const char* type) {
    for(int i = 0; i < r->nservices; i++)
        if(strcmp(r->services[i].class_name, type) == 0) return i;
    return -1;
}

/* 校验一个构造器依赖列表：每个形参必须有类型标注且命中服务表。
 * kind: "服务"/"控制器"（报错措辞）。返回 0 成功 */
static int check_deps(DiRegistry* r, const char* owner, const char* kind,
                      DiParam* deps, int ndeps) {
    for(int i = 0; i < ndeps; i++) {
        if(!deps[i].type) {
            LOG_ERROR("[DI] %s %s 的构造器形参 %s 缺少类型标注，DI 无法解析 / "
                      "%s %s: constructor parameter %s lacks a type annotation; DI cannot resolve it\n",
                      kind, owner, deps[i].name, kind, owner, deps[i].name);
            return 1;
        }
        if(find_service_by_class(r, deps[i].type) < 0) {
            LOG_ERROR("[DI] %s %s 的构造器形参 %s 的类型 %s 不是已注册服务（请标注 @Service） / "
                      "%s %s: type %s of constructor parameter %s is not a registered service (annotate it with @Service)\n",
                      kind, owner, deps[i].name, deps[i].type,
                      kind, owner, deps[i].type, deps[i].name);
            return 1;
        }
    }
    return 0;
}

/* DFS：颜色标记循环检测 + 后序追加即拓扑序（被依赖者先完成）。
 * color: 0 白 / 1 灰（递归栈中） / 2 黑 */
static int dfs_visit(DiRegistry* r, int u, int* color, int* stack, int depth, int* topo_n) {
    color[u] = 1;
    stack[depth] = u;
    DiService* s = &r->services[u];
    for(int i = 0; i < s->ndeps; i++) {
        int v = find_service_by_class(r, s->deps[i].type);
        if(v < 0) continue;  /* 缺失依赖前面已报错 */
        if(color[v] == 1) {
            /* 命中灰节点：打印从 v 到当前节点的环链路 */
            fprintf(stderr, "[DI] 检测到循环依赖 / circular dependency detected:\n  ");
            int start = 0;
            while(start < depth && stack[start] != v) start++;
            for(int k = start; k <= depth; k++)
                fprintf(stderr, "%s -> ", r->services[stack[k]].class_name);
            fprintf(stderr, "%s\n", r->services[v].class_name);
            return 1;
        }
        if(color[v] == 0) {
            if(dfs_visit(r, v, color, stack, depth + 1, topo_n)) return 1;
        }
    }
    color[u] = 2;
    r->topo[(*topo_n)++] = u;
    return 0;
}

/* ---------- 入口 ---------- */

DiRegistry* di_analyze(void) {
    g_collect_error = 0;
    DiRegistry* r = (DiRegistry*)calloc(1, sizeof(DiRegistry));
    type_foreach(collect_class, r);

    if(g_collect_error) {
        free_registry(r);
        return NULL;
    }

    /* 服务名查重 */
    for(int i = 0; i < r->nservices; i++) {
        for(int j = i + 1; j < r->nservices; j++) {
            if(strcmp(r->services[i].service_name, r->services[j].service_name) == 0) {
                LOG_ERROR("[DI] 服务名重复注册：%s（%s 与 %s） / "
                          "duplicate service name: %s (%s and %s)\n",
                          r->services[i].service_name,
                          r->services[i].class_name, r->services[j].class_name,
                          r->services[i].service_name,
                          r->services[i].class_name, r->services[j].class_name);
                free_registry(r);
                return NULL;
            }
        }
    }

    /* 依赖完整性 */
    for(int i = 0; i < r->nservices; i++)
        if(check_deps(r, r->services[i].class_name, "服务", r->services[i].deps, r->services[i].ndeps)) {
            free_registry(r);
            return NULL;
        }
    for(int i = 0; i < r->ncontrollers; i++)
        if(check_deps(r, r->controllers[i].class_name, "控制器", r->controllers[i].deps, r->controllers[i].ndeps)) {
            free_registry(r);
            return NULL;
        }

    /* 循环检测 + 拓扑序 */
    int* color = (int*)calloc((size_t)(r->nservices ? r->nservices : 1), sizeof(int));
    int* stack = (int*)malloc((size_t)(r->nservices + 1) * sizeof(int));
    r->topo = (int*)malloc((size_t)(r->nservices + 1) * sizeof(int));
    int topo_n = 0;
    int cycle = 0;
    for(int i = 0; i < r->nservices; i++) {
        if(color[i] == 0 && dfs_visit(r, i, color, stack, 0, &topo_n)) {
            cycle = 1;
            break;
        }
    }
    free(color);
    free(stack);
    if(cycle) {
        free_registry(r);
        return NULL;
    }

    g_last = r;
    return r;
}

DiRegistry* di_meta_last(void) {
    return g_last;
}

int di_meta_is_controller(const char* class_name) {
    if (!class_name || !g_last) return 0;
    for (int i = 0; i < g_last->ncontrollers; i++) {
        if (strcmp(g_last->controllers[i].class_name, class_name) == 0) return 1;
    }
    return 0;
}
