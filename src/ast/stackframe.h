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

// 获取 double 类型变量的原始 double 值，零提取、零类型检查
// 直接从 double_vals 数组读取，用于 OPC_LOAD_DOUBLE_VAR 指令
double stackframe_get_double(StackFrame* f, const char* name, _Bool* found);

// 绑定 double 变量：同时更新 vals（包装成 Value）和 double_vals（原始 double 值），零重复提取
void stackframe_bind_double(StackFrame* f, const char* name, double dv);

// 获取 float 类型变量的原始 float 值，零提取、零类型检查
float stackframe_get_float(StackFrame* f, const char* name, _Bool* found);

// 绑定 float 变量：同时更新 vals（包装成 Value）和 float_vals（原始 float 值），零重复提取
void stackframe_bind_float(StackFrame* f, const char* name, float fv);

// 获取 uint 类型变量的原始 uint 值，零提取、零类型检查
unsigned int stackframe_get_uint(StackFrame* f, const char* name, _Bool* found);

// 绑定 uint 变量：同时更新 vals（包装成 Value）和 uint_vals（原始 uint 值），零重复提取
void stackframe_bind_uint(StackFrame* f, const char* name, unsigned int uv);

// 获取 bool 类型变量的原始 bool 值，零提取、零类型检查
_Bool stackframe_get_bool(StackFrame* f, const char* name, _Bool* found);

// 绑定 bool 变量：同时更新 vals 和 bool_vals，零重复提取
void stackframe_bind_bool(StackFrame* f, const char* name, _Bool bv);

// 获取 char 类型变量的原始 char 值，零提取、零类型检查
char stackframe_get_char(StackFrame* f, const char* name, _Bool* found);

// 绑定 char 变量：同时更新 vals 和 char_vals，零重复提取
void stackframe_bind_char(StackFrame* f, const char* name, char cv);

// 获取 byte 类型变量的原始 byte 值，零提取、零类型检查
unsigned char stackframe_get_byte(StackFrame* f, const char* name, _Bool* found);

// 绑定 byte 变量：同时更新 vals 和 byte_vals，零重复提取
void stackframe_bind_byte(StackFrame* f, const char* name, unsigned char bv);

// 获取 int8 类型变量的原始 int8 值，零提取、零类型检查
int8_t stackframe_get_int8(StackFrame* f, const char* name, _Bool* found);

// 绑定 int8 变量：同时更新 vals 和 int8_vals，零重复提取
void stackframe_bind_int8(StackFrame* f, const char* name, int8_t i8v);

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
