/*
 * 应用引导代码生成 —— 实现
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "boot_gen.h"
#include "di_analysis.h"
#include "ast/ast_node.h"
#include "ast/ast_node_type.h"
#include "ast/ast_types.h"
#include "ast/func_compile.h"
#include "../ir/ir_compile.h"
#include "lumyr_log.h"

/* ---------- Task 8：适配函数生成辅助 ---------- */

/* 服务临时变量名：__lm_boot_svc_<拓扑位置> */
static char* svc_var_name(int topoPos) {
    char buf[64];
    snprintf(buf, sizeof buf, "__lm_boot_svc_%d", topoPos);
    return strdup(buf);
}

/* 控制器临时变量名 __lm_boot_ctl_<i> */
static char* ctl_var_name(int idx) {
    char buf[128];
    snprintf(buf, sizeof buf, "__lm_boot_ctl_%d", idx);
    return strdup(buf);
}

/* 模型构造函数名 __lm_model_<Class> */
static char* model_builder_name(const char* cls) {
    size_t need = strlen("__lm_model_") + strlen(cls) + 1;
    char* s = malloc(need);
    snprintf(s, need, "__lm_model_%s", cls);
    return s;
}

/* 类专属注册函数名 __lm_reg_<ClassName>（自动注册与手动 app.controller 共用） */
static char* reg_func_name(const char* class_name) {
    size_t need = strlen("__lm_reg_") + strlen(class_name) + 1;
    char* s = malloc(need);
    snprintf(s, need, "__lm_reg_%s", class_name);
    return s;
}

/* 适配函数名 __lm_adapter_<ctl>_<route> */
static char* adapter_func_name(int ci, int ri) {
    char buf[128];
    snprintf(buf, sizeof buf, "__lm_adapter_%d_%d", ci, ri);
    return strdup(buf);
}

/* 生成 400 错误返回语句块：
 * __lm_r = ServerResponse(400); __lm_r.text(msg); return __lm_r; */
static AstNode* build_400_block(const char* msg) {
    AstNode* mk = ast_assign(strdup("__lm_r"),
                             ast_class_new(strdup("ServerResponse"), 1, ast_int(400), NULL));
    AstNode* tx = ast_method_call(ast_var(strdup("__lm_r")), strdup("text"),
                                  ast_string(strdup(msg)));
    AstNode* ret = ast_return(ast_var(strdup("__lm_r")));
    return ast_seq(mk, ast_seq(tx, ret));
}

/* 为模型类生成 __lm_model_<Class>(m) 反序列化函数：
 * inst = <Class>(); 公开字段逐个 if (m.contains(f)) inst[f] = m[f]; return inst;
 * 类未注册返回 NULL（调用方报编译错误） */
static AstNode* build_model_builder(const char* cls) {
    TypeDef* td = class_lookup(cls);
    if(!td) return NULL;
    AstNode* body = ast_assign(strdup("inst"),
                               ast_class_new(strdup(cls), 0, NULL, NULL));
    for(int i = 0; i < td->nprops; i++) {
        if(!td->props || !td->props[i]) continue;
        /* 仅映射公开字段（0=public；访问修饰表缺失视为全公开） */
        if(td->prop_access_modifiers && td->prop_access_modifiers[i] != 0) continue;
        AstNode* cond = ast_method_call(ast_var(strdup("m")), strdup("contains"),
                                        ast_string(strdup(td->props[i])));
        AstNode* setField = ast_index_assign(ast_var(strdup("inst")),
                                             ast_string(strdup(td->props[i])),
                                             ast_index(ast_var(strdup("m")),
                                                       ast_string(strdup(td->props[i]))));
        body = ast_seq(body, ast_if(cond, setField, NULL, NULL));
    }
    body = ast_seq(body, ast_return(ast_var(strdup("inst"))));
    AstNode* params = ast_param(strdup("m"), 0, NULL);
    return ast_func_def(model_builder_name(cls), params, ast_block(body));
}

/* 字符串是否在列表中（模型构造函数去重用） */
static int str_in_list(char** list, int n, const char* s) {
    for(int i = 0; i < n; i++)
        if(strcmp(list[i], s) == 0) return 1;
    return 0;
}

/* 标量/容器参数 → RequestBinder 严格绑定函数（平名）。
 * 按位宽分派：窄类型先范围核对再收窄，杜绝静默截断；
 * map/array 走 JSON 串解析；未列出的类型返回 0（类名走模型绑定，
 * 其余在生成期报错）。 */
static int scalar_binder_kind(const char* type, const char** flatName) {
    /* map/array：name_to_castkind（ast_types.c）不含这两个名字，显式特判 */
    if(strcmp(type, "map") == 0)   { *flatName = "RequestBinder_asMap"; return 1; }
    if(strcmp(type, "array") == 0) { *flatName = "RequestBinder_asArray"; return 1; }
    int ck = name_to_castkind(type);
    switch(ck) {
    case CAST_INT: case CAST_INT32:
        *flatName = "RequestBinder_asInt"; return 1;
    case CAST_INT8:
        *flatName = "RequestBinder_asInt8"; return 1;
    case CAST_SHORT: case CAST_INT16:
        *flatName = "RequestBinder_asShort"; return 1;
    case CAST_USHORT: case CAST_UINT16:
        *flatName = "RequestBinder_asUshort"; return 1;
    case CAST_BYTE: case CAST_UINT8:
        *flatName = "RequestBinder_asByte"; return 1;
    case CAST_LONG: case CAST_LONGLONG: case CAST_INT64: case CAST_SSIZE_T:
        *flatName = "RequestBinder_asLong"; return 1;
    case CAST_UINT: case CAST_UINT32: case CAST_UINT64: case CAST_ULONG: case CAST_SIZE_T:
        *flatName = "RequestBinder_asUlong"; return 1;
    case CAST_DOUBLE:
        *flatName = "RequestBinder_asDouble"; return 1;
    case CAST_FLOAT:
        *flatName = "RequestBinder_asFloat"; return 1;
    case CAST_BOOL:
        *flatName = "RequestBinder_asBool"; return 1;
    case CAST_STRING:
        *flatName = "RequestBinder_asString"; return 1;
    case CAST_MAP:
        *flatName = "RequestBinder_asMap"; return 1;
    case CAST_ARRAY:
        *flatName = "RequestBinder_asArray"; return 1;
    default:
        return 0;
    }
}

/* 按服务类名找它的拓扑位置（变量名后缀）；无则 -1 */
static int svc_topo_pos(DiRegistry* r, const char* class_name) {
    for(int i = 0; i < r->nservices; i++)
        if(strcmp(r->services[r->topo[i]].class_name, class_name) == 0) return i;
    return -1;
}

/* 构造实参链：按 deps 依次引用对应服务变量 */
static AstNode* build_dep_args(DiRegistry* r, DiParam* deps, int ndeps) {
    AstNode* args = NULL;
    for(int i = 0; i < ndeps; i++) {
        int pos = svc_topo_pos(r, deps[i].type);
        AstNode* ref = ast_var(svc_var_name(pos));
        args = args ? ast_arg_append(args, ref) : ref;
    }
    return args;
}

/* 打印引导文本（--print-boot），仅呈现等价计划 */
static void print_boot_text(DiRegistry* r) {
    printf("func __lm_app_boot__(app, container) {\n");
    for(int i = 0; i < r->nservices; i++) {
        DiService* s = &r->services[r->topo[i]];
        printf("    __lm_boot_svc_%d = new %s(", i, s->class_name);
        for(int d = 0; d < s->ndeps; d++) {
            if(d) printf(", ");
            int pos = svc_topo_pos(r, s->deps[d].type);
            printf("__lm_boot_svc_%d", pos);
        }
        printf(");\n");
        printf("    container.register(\"%s\", __lm_boot_svc_%d);\n",
               s->service_name, i);
    }
    for(int i = 0; i < r->ncontrollers; i++) {
        DiController* c = &r->controllers[i];
        printf("    __lm_boot_ctl_%d = new %s(", i, c->class_name);
        for(int d = 0; d < c->ndeps; d++) {
            if(d) printf(", ");
            int pos = svc_topo_pos(r, c->deps[d].type);
            printf("__lm_boot_svc_%d", pos);
        }
        printf(");\n");
        printf("    app.controller(__lm_boot_ctl_%d);\n", i);
    }
    printf("}\n__lm_register_boot(__lm_app_boot__);\n");
}

/* 递归展平左倾 SEQ 树为线性节点数组 */
static void collectFlat(AstNode* node, AstNode*** out, int* n, int* cap) {
    if (!node) return;
    if (node->type == AST_SEQ) {
        collectFlat(node->u.seq.first, out, n, cap);
        collectFlat(node->u.seq.second, out, n, cap);
        return;
    }
    if (*n >= *cap) {
        *cap = *cap ? *cap * 2 : 16;
        *out = (AstNode**)realloc(*out, sizeof(AstNode*) * (size_t)*cap);
    }
    (*out)[(*n)++] = node;
}

AstNode* boot_gen_inject(AstNode* root, int print_only) {
    DiRegistry* r = di_meta_last();
    if(!r) return root;

    /* 先展平原顶层并定位哨兵。哨兵在任何情况下都不得进入运行时，
     * 故即使没有任何 DI 组件也要剔除（否则 c_stmt 遇到裸字符串语句报错）。 */
    AstNode** flat = NULL;
    int nflat = 0, fcap = 0;
    collectFlat(root, &flat, &nflat, &fcap);

    int lastMarker = -1;
    for (int i = 0; i < nflat; i++) {
        if (flat[i]->type == AST_STRING && flat[i]->u.sval &&
            strcmp(flat[i]->u.sval, "__LM_USER_BODY_START__") == 0) {
            lastMarker = i;
        }
    }

    /* 净化版顶层（仅剔除哨兵）：零 DI 组件时直接返回它 */
    AstNode* stripped = NULL;
    for (int i = 0; i < nflat; i++) {
        if (flat[i]->type == AST_STRING && flat[i]->u.sval &&
            strcmp(flat[i]->u.sval, "__LM_USER_BODY_START__") == 0) continue;
        stripped = stripped ? ast_seq(stripped, flat[i]) : flat[i];
    }

    if (r->nservices == 0 && r->ncontrollers == 0) {
        free(flat);
        return stripped;
    }

    if(print_only) {
        print_boot_text(r);
        free(flat);
        return root;
    }

    /* 无兜底：@Service/@Controller 的引导登记依赖框架模块 Container.lm
     * 导出的 __lm_register_boot。未导入框架时该名字无法解析，运行期会
     * 退化为对未声明标识符的垃圾读取（LOAD_VAR 0）——必须在编译期
     * 双语报错退出，而不是静默生成坏代码。 */
    if(!ir_func_table_lookup("__lm_register_boot")) {
        fprintf(stderr,
                "[boot] 检测到 @Service/@Controller 注解但未导入框架模块：请 import <App/Container> 或 <App/WebApplication> / "
                "@Service/@Controller annotations found but the framework module is not imported: add import <App/Container> or <App/WebApplication>\n");
        free(flat);
        return NULL;
    }

    AstNode* stmts = NULL;

    /* ---- 服务：拓扑序构造 + 注册容器 ---- */
    for(int i = 0; i < r->nservices; i++) {
        DiService* s = &r->services[r->topo[i]];
        char* varName = svc_var_name(i);

        /* varName = new Class(deps) */
        AstNode* depArgs = build_dep_args(r, s->deps, s->ndeps);
        AstNode* newExpr = ast_class_new(strdup(s->class_name), s->ndeps, depArgs, NULL);
        AstNode* assignNode = ast_assign(strdup(varName), newExpr);
        stmts = stmts ? ast_seq(stmts, assignNode) : assignNode;

        /* container.register("serviceName", varName) */
        AstNode* nameArg = ast_string(s->service_name);
        AstNode* varArg = ast_var(strdup(varName));
        AstNode* regArgs = ast_arg_append(nameArg, varArg);
        AstNode* regCall = ast_method_call(ast_var(strdup("container")),
                                           strdup("register"), regArgs);
        stmts = ast_seq(stmts, regCall);

        free(varName);
    }

    /* 类专属注册函数声明集合（与引导函数并列注入顶层） */
    AstNode* regFuncs = NULL;
    /* 适配函数声明集合（Task 8：每个映射方法一个） */
    AstNode* adapterFuncs = NULL;
    /* 模型构造函数声明集合（按类名去重） */
    AstNode* modelFuncs = NULL;
    char** modelDone = NULL;
    int nModelDone = 0, modelCap = 0;

    /* ---- 控制器：构造 + 调类专属注册函数 ---- */
    for(int i = 0; i < r->ncontrollers; i++) {
        DiController* c = &r->controllers[i];
        char* varName = ctl_var_name(i);

        AstNode* depArgs = build_dep_args(r, c->deps, c->ndeps);
        AstNode* newExpr = ast_class_new(strdup(c->class_name), c->ndeps, depArgs, NULL);
        AstNode* assignNode = ast_assign(strdup(varName), newExpr);
        stmts = stmts ? ast_seq(stmts, assignNode) : assignNode;

        /* __lm_reg_<Class>(app, ctlVar)：类专属注册函数，内含映射注册 */
        AstNode* callArgs = ast_arg_append(ast_var(strdup("app")),
                                           ast_var(strdup(varName)));
        AstNode* regCall = ast_call(reg_func_name(c->class_name), callArgs);
        stmts = ast_seq(stmts, regCall);

        /* -- 构建类专属注册函数体：仅注册带映射注解的方法（AC12）-- */
        AstNode* regBody = NULL;

        /* app.controllers.add(ctl)：保留已挂载控制器记账，
         * 与显式 controller() 通道的既有行为一致 */
        AstNode* controllersField = ast_index(ast_var(strdup("app")),
                                              ast_string(strdup("controllers")));
        AstNode* addMountCall = ast_method_call(controllersField,
                                                strdup("add"), ast_var(strdup("ctl")));
        regBody = addMountCall;

        for(int j = 0; j < c->nroutes; j++) {
            DiRoute* rt = &c->routes[j];
            char* adapterName = adapter_func_name(i, j);

            /* -- 适配函数体：参数按名绑定 + 类型校验/转换 + 返回包装 -- */
            AstNode* adapterBody = NULL;
            AstNode* callArgs = NULL;   /* instance.<method>(...) 实参链 */
            int hasArgs = 0;
            for(int k = 0; k < rt->nparams; k++) {
                DiParam* dp = &rt->params[k];
                char abuf[32];
                snprintf(abuf, sizeof abuf, "__lm_a%d", k);

                if(strcmp(dp->name, "req") == 0) {
                    /* req 形参：请求对象直接透传 */
                    callArgs = hasArgs ? ast_arg_append(callArgs, ast_var(strdup("req")))
                                       : ast_var(strdup("req"));
                    hasArgs = 1;
                    continue;
                }

                if(!dp->type || strlen(dp->type) == 0) {
                    /* 无类型标注：按名绑定原始值（路径→query→表单），不校验 */
                    AstNode* bind = ast_assign(strdup(abuf),
                        ast_method_call(ast_var(strdup("req")), strdup("param"),
                                        ast_string(strdup(dp->name))));
                    adapterBody = adapterBody ? ast_seq(adapterBody, bind) : bind;
                    callArgs = hasArgs ? ast_arg_append(callArgs, ast_var(strdup(abuf)))
                                       : ast_var(strdup(abuf));
                    hasArgs = 1;
                    continue;
                }

                const char* binderFlat = NULL;
                if(scalar_binder_kind(dp->type, &binderFlat)) {
                    /* 标量类型：严格校验转换，null（缺失或格式非法）→ 400 双语 */
                    AstNode* pcall = ast_method_call(ast_var(strdup("req")), strdup("param"),
                                                     ast_string(strdup(dp->name)));
                    AstNode* bind = ast_assign(strdup(abuf),
                                               ast_call(strdup(binderFlat), pcall));
                    adapterBody = adapterBody ? ast_seq(adapterBody, bind) : bind;
                    size_t need = strlen("缺少参数 '' 或类型不符，需要  / missing or invalid parameter '', expects ")
                                  + strlen(dp->name) * 2 + strlen(dp->type) * 2 + 1;
                    char* msg = malloc(need);
                    snprintf(msg, need,
                             "缺少参数 '%s' 或类型不符，需要 %s / missing or invalid parameter '%s', expects %s",
                             dp->name, dp->type, dp->name, dp->type);
                    AstNode* chk = ast_if(ast_binop(OP_EQ, ast_var(strdup(abuf)), ast_none()),
                                          build_400_block(msg), NULL, NULL);
                    adapterBody = ast_seq(adapterBody, chk);
                    free(msg);
                    callArgs = hasArgs ? ast_arg_append(callArgs, ast_var(strdup(abuf)))
                                       : ast_var(strdup(abuf));
                    hasArgs = 1;
                    continue;
                }

                /* 类模型：从 JSON body 反序列化（按公开字段映射，无参构造）。
                 * 模型构造函数按类名去重，只生成并编译一次 */
                if(!str_in_list(modelDone, nModelDone, dp->type)) {
                    AstNode* mb = build_model_builder(dp->type);
                    if(!mb) {
                        fprintf(stderr,
                                "BootGen: 方法 %s.%s 形参 %s 的类型 \"%s\" 不是已注册的类，无法从 JSON 绑定 / "
                                "cannot bind parameter %s of %s.%s: type \"%s\" is not a registered class (JSON binding unsupported)\n",
                                c->class_name, rt->method_name, dp->name, dp->type,
                                dp->name, c->class_name, rt->method_name, dp->type);
                        return NULL;
                    }
                    if(nModelDone == modelCap) {
                        modelCap = modelCap ? modelCap * 2 : 8;
                        modelDone = realloc(modelDone, (size_t)modelCap * sizeof(char*));
                    }
                    modelDone[nModelDone++] = strdup(dp->type);
                    modelFuncs = modelFuncs ? ast_seq(modelFuncs, mb) : mb;
                    compile_func_from_ast(mb);
                }
                AstNode* jcall = ast_method_call(ast_var(strdup("req")), strdup("json"), NULL);
                AstNode* bind = ast_assign(strdup(abuf),
                                           ast_call(model_builder_name(dp->type), jcall));
                adapterBody = adapterBody ? ast_seq(adapterBody, bind) : bind;
                callArgs = hasArgs ? ast_arg_append(callArgs, ast_var(strdup(abuf)))
                                   : ast_var(strdup(abuf));
                hasArgs = 1;
            }

            /* return BoundAdapter.wrapResult(instance.<method>(args...))
             * 点语法自然形态：instance 为动态接收者，走 VM 动态方法分派；
             * 动态分派返回值已按被调方 ret 标注装箱（vm_box_int64_as，
             * 与静态路径一致），不再需要绕过形状 */
            AstNode* mcall = ast_method_call(ast_var(strdup("instance")),
                                             strdup(rt->method_name), callArgs);
            AstNode* wrap = ast_call(strdup("BoundAdapter_wrapResult"), mcall);
            AstNode* retStmt = ast_return(wrap);
            adapterBody = adapterBody ? ast_seq(adapterBody, retStmt) : retStmt;

            /* func __lm_adapter_<i>_<j>(instance, req) { ... }
             * 先编译适配函数，注册函数体内 ast_var(adapterName) 才能解析为函数值 */
            AstNode* aparams = ast_param(strdup("instance"), 0, NULL);
            aparams = ast_param_append(aparams, ast_param(strdup("req"), 0, NULL));
            AstNode* adapterFn = ast_func_def(adapterName, aparams, ast_block(adapterBody));
            adapterFuncs = adapterFuncs ? ast_seq(adapterFuncs, adapterFn) : adapterFn;
            compile_func_from_ast(adapterFn);

            /* app.route(verb, basePath+path, BoundAdapter(ctl, adapterFn))
             * 登记适配器对象而非裸函数值：invokeHandler 走 h.serve(req)，
             * 由 BoundAdapter 以 (instance, req) 双参调适配函数 */
            char* fullPath = malloc(strlen(c->base_path) + strlen(rt->path) + 1);
            strcpy(fullPath, c->base_path);
            strcat(fullPath, rt->path);
            AstNode* adapterRef = ast_var(adapterName);
            AstNode* baArgs = ast_arg_append(ast_var(strdup("ctl")), adapterRef);
            AstNode* baNew = ast_class_new(strdup("BoundAdapter"), 2, baArgs, NULL);
            AstNode* routeArgs = ast_arg_append(ast_string(rt->verb), ast_string(fullPath));
            routeArgs = ast_arg_append(routeArgs, baNew);
            AstNode* routeCallNode = ast_method_call(ast_var(strdup("app")),
                                                     strdup("route"), routeArgs);
            regBody = regBody ? ast_seq(regBody, routeCallNode) : routeCallNode;
            free(adapterName);
        }

        /* func __lm_reg_<Class>(app, ctl) { regBody } */
        AstNode* regParams = ast_param(strdup("app"), 0, NULL);
        regParams = ast_param_append(regParams, ast_param(strdup("ctl"), 0, NULL));
        AstNode* regFn = ast_func_def(reg_func_name(c->class_name), regParams,
                                      ast_block(regBody));
        regFuncs = regFuncs ? ast_seq(regFuncs, regFn) : regFn;

        /* 注册函数不含注入式嵌套函数，直接编译进函数表，
         * 引导函数调用时查表即可 */
        compile_func_from_ast(regFn);

        free(varName);
    }

    /* func __lm_app_boot__(app, container) { stmts } */
    AstNode* block = ast_block(stmts);
    AstNode* params = ast_param(strdup("app"), 0, NULL);
    params = ast_param_append(params, ast_param(strdup("container"), 0, NULL));
    AstNode* bootFunc = ast_func_def(strdup("__lm_app_boot__"), params, block);

    /* __lm_register_boot(__lm_app_boot__) */
    AstNode* funcRef = ast_var(strdup("__lm_app_boot__"));
    AstNode* regBoot = ast_call(strdup("__lm_register_boot"), funcRef);

    /* 注入节点跳过了 parse 期函数编译：立即把引导函数编译进函数表，
     * 运行时登记与调用才取得到函数值。此刻 parse/typecheck 已完成，
     * 类构造器与方法均已就绪。 */
    compile_func_from_ast(bootFunc);

    /* 入口处已展平并定位最后一个哨兵。登记语句插在哨兵位置：
     * 框架已初始化、用户代码尚未执行；所有哨兵一并剔除；
     * 类注册函数与引导函数声明置于末尾。 */
    AstNode* merged = NULL;
    for (int i = 0; i < nflat; i++) {
        if (flat[i]->type == AST_STRING && flat[i]->u.sval &&
            strcmp(flat[i]->u.sval, "__LM_USER_BODY_START__") == 0) {
            /* 最后一个哨兵位置：插入登记语句（前面的哨兵只剔除） */
            if (i == lastMarker) merged = merged ? ast_seq(merged, regBoot) : regBoot;
            continue;
        }
        merged = merged ? ast_seq(merged, flat[i]) : flat[i];
    }
    if (lastMarker < 0) {
        /* 无哨兵（异常情况）：登记语句追加末尾，保证不静默漏注册 */
        merged = merged ? ast_seq(merged, regBoot) : regBoot;
    }
    if (modelFuncs) merged = ast_seq(merged, modelFuncs);
    if (adapterFuncs) merged = ast_seq(merged, adapterFuncs);
    if (regFuncs) merged = ast_seq(merged, regFuncs);
    merged = ast_seq(merged, bootFunc);

    free(flat);
    return merged;
}
