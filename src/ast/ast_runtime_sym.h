#ifndef AST_RUNTIME_SYM_H
#define AST_RUNTIME_SYM_H

#include "lumyr_value.h"

#define SYM_INITIAL_CAP 64

/* 运行时符号表（全局变量/函数）：红黑树存储，支持 class_name 扩展 */

/* 初始化符号表（在 main 函数开始时调用） */
void sym_init(void);

/* 清空符号表（在 main 函数结束时调用） */
void sym_clear(void);

/* 设置/获取当前文件名（用于跨文件命名冲突处理） */
void sym_set_current_file(const char* file_name);
const char* sym_get_current_file(void);

/* 设置符号（class_name 为 NULL 表示普通符号） */
void sym_set(const char* n, Value v);
void sym_set_class(const char* class_name, const char* n, Value v);

/* 获取符号（class_name 为 NULL 表示普通符号） */
Value sym_get(const char* n);
Value sym_get_class(const char* class_name, const char* n);

/* 获取符号指针（用于修改） */
Value* sym_get_ptr(const char* n);

/* 检查符号是否存在（class_name 为 NULL 表示普通符号） */
_Bool sym_has(const char* n);
_Bool sym_has_class(const char* class_name, const char* n);

/* 删除符号 */
void sym_del(const char* n);

/* 工具函数 */
double val_to_num(Value v);
int value_equal(Value a, Value b);
char* lumyr_concat(const char* s1, const char* s2);

#endif
