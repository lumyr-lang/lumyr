/*
 * 应用引导代码生成（编译期 AST 注入）
 *
 * 依据 di_analyze 的元信息与拓扑序，直接构造引导函数 AST 并注入 root：
 *
 *   func __lm_app_boot__(app, container) {
 *       // 拓扑序：被依赖服务先构造，单例注册进容器
 *       __lm_boot_svc_0 = new XxxService(__lm_boot_svc_dep...);
 *       container.register("serviceName", __lm_boot_svc_0);
 *       ...
 *       // 控制器最后构造
 *       __lm_boot_ctl_0 = new XxxController(__lm_boot_svc...);
 *       app.controller(__lm_boot_ctl_0);
 *       ...
 *   }
 *   __lm_register_boot(__lm_app_boot__);
 *
 * 采用 AST 直接生成（与 @Data 同一机制）：注解元信息在 parse 后才完整，
 * 无法再走源码预处理通道，运行时仍保持零反射。
 */
#ifndef LM_BOOT_GEN_H
#define LM_BOOT_GEN_H

struct AstNode;

/* 把引导节点注入 root；返回合并后的新 root。
 * 无控制器/服务时原样返回。--print-boot 时仅打印引导文本不改 root。 */
struct AstNode* boot_gen_inject(struct AstNode* root, int print_only);

#endif /* LM_BOOT_GEN_H */
