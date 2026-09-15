#ifndef STACKFRAME_H
#define STACKFRAME_H

#include "lumyr_value_type.h"

// 新建栈帧：parent 为调用者栈帧，可为 NULL（顶层帧）
// 新建的帧默认非共享（线程私有）；全局共享帧需调用 stackframe_set_shared 标记
StackFrame* stackframe_new(StackFrame* parent);

// 标记为全局共享帧（main 顶层帧）：多线程沿 parent 链访问，get/set/bind 内部加 rwlock
void stackframe_set_shared(StackFrame* f);

// 销毁栈帧：释放帧内持有的字符串/数组等资源
// 注意：VAL_FUNC 为引用语义（函数对象生命周期由注册方管理），帧内不销毁
void stackframe_destroy(StackFrame* f);

// 查找变量：从当前帧向上沿 parent 链查找；找到返回值的拷贝并置 *found=1，
// 找不到 *found=0 并返回零值。共享帧在锁内完成遍历与拷贝，返回后不再依赖帧内存。
Value stackframe_get(StackFrame* f, const char* name, _Bool* found);

// 赋值语义：沿链查找，找到则原地更新；找不到则在当前帧新建（绑定当前帧，词法遮蔽）
void stackframe_set(StackFrame* f, const char* name, Value v);

// 设置变量的类型标记（CastKind 枚举，-1 表示无精确类型）
void stackframe_set_type_tag(StackFrame* f, const char* name, int type_tag);

// 获取变量的类型标记（-1 表示无精确类型）
int stackframe_get_type_tag(StackFrame* f, const char* name);

// 获取 int 类型变量的原始 int 值，零提取、零类型检查
// 直接从 int_vals 数组读取，用于 OPC_LOAD_INT_VAR 指令
int stackframe_get_int(StackFrame* f, const char* name, _Bool* found);

// 绑定语义（参数绑定用）：只在当前帧查找/创建，不向上查找，遮蔽父帧同名变量
void stackframe_bind(StackFrame* f, const char* name, Value v);

// 绑定 int 变量：同时更新 vals（包装成 Value）和 int_vals（原始 int 值），零重复提取
void stackframe_bind_int(StackFrame* f, const char* name, int iv);

// ---- 闭包单元（cell）支持 ----
// 把 name→cell_ptr 注册到当前帧 cell 表（lambda 调用时注入捕获变量用）。
void stackframe_add_cell(StackFrame* f, const char* name, Value* cell_ptr);

// 沿 parent 链查找变量所属帧并返回其 cell 指针；若该变量还是普通槽位，
// 则在它所属帧内"装箱"：分配堆 Value 并把当前槽值拷入，此后该帧内同名访问走 cell。
// 找不到该变量返回 NULL（调用方报错）。
Value* stackframe_ensure_cell(StackFrame* f, const char* name);

// 沿 parent 链查找已存在的 cell 指针（不创建），找不到返回 NULL。
Value** stackframe_find_cell(StackFrame* f, const char* name);

#endif //STACKFRAME_H
