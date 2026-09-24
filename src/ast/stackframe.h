#ifndef STACKFRAME_H
#define STACKFRAME_H

#include "lumyr_value_type.h"

// 新建栈帧：parent 为调用者栈帧，可为 NULL（顶层帧）
// 新建的帧默认非共享（线程私有）；全局共享帧需调用 stackframe_set_shared 标记
StackFrame* stackframe_new(StackFrame* parent);

// 标记为全局共享帧（main 顶层帧）：多线程沿 parent 链访问，get/set/bind 内部加 rwlock
void stackframe_set_shared(StackFrame* f);

// 销毁栈帧：释放帧内持有的字符串/数组等资源
void stackframe_destroy(StackFrame* f);

// 查找变量：从当前帧向上沿 parent 链查找；找到返回值的拷贝并置 *found=1，
// 找不到 *found=0 并返回零值
Value stackframe_get(StackFrame* f, const char* name, _Bool* found);

// 赋值语义：沿链查找，找到则原地更新；找不到则在当前帧新建
void stackframe_set(StackFrame* f, const char* name, Value v);

// 设置变量的类型标记（CastKind 枚举，-1 表示无精确类型）
void stackframe_set_type_tag(StackFrame* f, const char* name, int type_tag);

// 获取变量的类型标记（-1 表示无精确类型）
int stackframe_get_type_tag(StackFrame* f, const char* name);

// 绑定语义（参数绑定用）：只在当前帧查找/创建，不向上查找
void stackframe_bind(StackFrame* f, const char* name, Value v);

// 获取 int64 类型变量的原始 int64_t 值（宽槽，所有整数类型统一存储）
int64_t stackframe_get_int64(StackFrame* f, const char* name, _Bool* found);

// 绑定 int64 变量（宽槽，所有整数类型统一存储）
void stackframe_bind_int64(StackFrame* f, const char* name, int64_t v);

// 获取 double 类型变量的原始 double 值（宽槽，所有浮点类型统一存储）
double stackframe_get_double(StackFrame* f, const char* name, _Bool* found);

// 绑定 double 变量（宽槽，所有浮点类型统一存储）
void stackframe_bind_double(StackFrame* f, const char* name, double v);

// 获取指针类型变量的原始 void* 值（宽槽，所有指针/字符串类型统一存储）
void* stackframe_get_ptr(StackFrame* f, const char* name, _Bool* found);

// 绑定指针变量（宽槽，所有指针/字符串类型统一存储）
void stackframe_bind_ptr(StackFrame* f, const char* name, void* v);

// ref 引用绑定：使槽 name 别名调用方 caller 帧 caller_slot 槽的存储（自动 box/unbox）
void stackframe_bind_ref(StackFrame* f, const char* name, StackFrame* caller, int caller_slot);

// 确保帧内变量槽（含 vals/typed 槽/refs）至少分配到 need 个（按索引访问前调用）
void stackframe_ensure_slots(StackFrame* f, int need);

// ---- 闭包单元（cell）支持 ----
void stackframe_add_cell(StackFrame* f, const char* name, Value* cell_ptr);
Value* stackframe_ensure_cell(StackFrame* f, const char* name);
Value** stackframe_find_cell(StackFrame* f, const char* name);

#endif //STACKFRAME_H
