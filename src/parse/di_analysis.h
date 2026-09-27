/*
 * DI 元信息收集与依赖图分析（编译期）
 *
 * 在全量 parse + typecheck 完成后运行：
 *   1. 收集 @Controller（basePath、方法映射与形参签名）与 @Service（服务名、构造器依赖）；
 *   2. 校验注解参数合法性（路径字面量、映射方法名）；
 *   3. 校验依赖完整性（形参类型必须是已注册服务）与循环依赖；
 *   4. 生成服务拓扑序，供引导代码生成（boot_gen）使用。
 *
 * 所有类名/类型名一律使用「解析所见」名称：扫描模块中类名与参数类型
 * 均已被 mangle（__lm_mod_<id>_X），与注册名天然一致。
 */
#ifndef LM_DI_ANALYSIS_H
#define LM_DI_ANALYSIS_H

/* 方法/构造器形参（不含 self） */
typedef struct {
    char* name;   /* 形参名 */
    char* type;   /* 类型标注（NULL=无标注） */
} DiParam;

/* 控制器方法映射 */
typedef struct {
    char* verb;        /* HTTP 方法（大写 GET/POST...） */
    char* path;        /* 方法级路径（原样，可含 :param） */
    char* method_name; /* 类中方法名 */
    DiParam* params;   /* 除 self 外形参（用于参数绑定） */
    int nparams;
    char* return_type; /* 返回类型标注（NULL=无） */
} DiRoute;

/* 控制器 */
typedef struct {
    char* class_name;  /* 解析所见类名 */
    char* base_path;   /* @Controller basePath（""=无） */
    DiRoute* routes;
    int nroutes;
    DiParam* deps;     /* 选定构造器形参（依赖，必须全部为服务） */
    int ndeps;
} DiController;

/* 服务 */
typedef struct {
    char* class_name;   /* 解析所见类名 */
    char* service_name; /* @Service 名（缺省=类名） */
    DiParam* deps;      /* 选定构造器形参（依赖） */
    int ndeps;
} DiService;

/* 全部元信息 */
typedef struct {
    DiController* controllers;
    int ncontrollers;
    DiService* services;
    int nservices;
    int* topo;         /* 服务下标拓扑序（被依赖者在前） */
} DiRegistry;

/*
 * 执行分析。成功返回注册表（进程内保留，di_meta_last 取）；
 * 任一校验失败打印中英双语错误并返回 NULL。
 */
DiRegistry* di_analyze(void);

DiRegistry* di_meta_last(void);

/* 判断 class_name 是否为已注册 @Controller（供 app.controller 调用点校验） */
int di_meta_is_controller(const char* class_name);

#endif /* LM_DI_ANALYSIS_H */
