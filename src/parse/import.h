/*
 * lumyr 模块系统（第二阶段）——文本预处理 + Name Mangling
 *
 * 设计背景：lumyr 的 bison/flex parser 是全局不可重入状态，无法在语法动作中
 * 递归解析被导入文件。因此采用「文本预处理」方案：在 yyparse() 之前，把
 * 主文件中的 `import "path" as name;` 语句展开为被导入模块的全部源码内联，
 * 并在模块末尾生成一个导出符号 map（利用 lumyr 已有的 `a.b` map 点访问语法，
 * `math.add(1,2)` 天然等价于 `math["add"](1,2)`，无需新增成员访问语法）。
 *
 * 第二阶段在第一阶段基础上引入 Name Mangling 实现真正的符号隔离：
 *   - 每个被导入模块首次处理时分配递增 module_id；
 *   - 模块内全部顶层符号（func/type/enum/顶层赋值/解构赋值）重写为
 *     `__lm_mod_<id>_<name>`，私有符号外部不可见；
 *   - 全局已处理表（key=realpath）去重：同一模块只内联一次；
 *   - active 栈循环导入检测；
 *   - 导出 map 的值使用 mangled 名：{"add": __lm_mod_0_add}。
 * 主文件不做 mangle，其符号为程序全局符号。
 *
 * 该方案对 VM 通道与编译通道完全一致：两通道消费的是同一份合并后的源码。
 *
 * 已知限制（Plan A 固有，文档化）：
 *   - 函数内局部变量若与模块级符号同名，会被一并 mangle（语义改变）。
 *     建议模块内避免局部变量遮蔽模块级符号名。
 *   - 非真正的符号表隔离，调试时符号名被改写。
 *   - 后续可升级到独立 AST 合并 / 可重入 parser。
 */
#ifndef LM_IMPORT_H
#define LM_IMPORT_H

/*
 * 预处理主文件。
 *   src_path：主源文件路径。
 *   had_mod_out：输出标志：
 *       1  -> 确实处理了 import/export，返回值为合并后文本（malloc'd，调用方 free）。
 *       0  -> 源文件不含任何 import/export 语法，返回 NULL（调用方可走原始 fopen 路径）。
 *      -1  -> 出错（错误信息已打印到 stderr），返回 NULL。
 */
char* lm_preprocess_main(const char* src_path, int* had_mod_out);

/*
 * 模块命名空间别名注册表。
 * 预处理阶段把 `import ".." as alias;` 的 alias 及其导出成员名表登记进来；
 * yacc 在解析 `a.b(x)` 方法链时，若接收者是模块别名，则按 map 动态调用处理
 * （a["b"](x)），而不是把接收者当前参（b(a,x)）。
 * 成员校验无兜底：a.b / a.b(x) 的成员不在导出表中时编译期直接报错。
 */
void lm_register_alias(const char* name, char* const* exports, int nexports);
int  lm_is_module_alias(const char* name);
/* 返回 1=是别名且有此导出成员；0=是别名但无此成员；-1=不是别名 */
int  lm_alias_export_lookup(const char* alias, const char* member);

#endif
