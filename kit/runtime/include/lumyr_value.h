#ifndef LUMYR_VALUE_H
#define LUMYR_VALUE_H

#include <stdio.h>
#include <stdlib.h>
#include "lumyr_value_type.h"

// runtime错误抛出
void runtime_error(const char* msg);

// try/catch 全局错误状态：当前错误跳转点（NULL=无 try，直接退出）与错误消息
// 错误机制全部动态化（g_err_msg/g_err_type/g_trace/__g_* 按需扩容，无硬上限）
#include <setjmp.h>
extern _Thread_local jmp_buf* g_err_jmp;
extern _Thread_local char* g_err_msg;
extern _Thread_local char* g_err_type;
extern _Thread_local const char** g_trace;
extern _Thread_local int g_trace_n;
extern _Thread_local jmp_buf* __g_jbs;
extern _Thread_local jmp_buf** __g_prev;
extern _Thread_local int __g_depth;
extern _Thread_local int* __g_sp0;
extern _Thread_local int* __g_tgt;
extern _Thread_local int* __g_tn;
extern _Thread_local int* __g_fn;
extern _Thread_local int* __g_fin_act;
extern _Thread_local int* __g_fin_dep;
extern _Thread_local int* __g_fin_tgt;
extern _Thread_local int __g_fin_n;
extern _Thread_local Value __g_pend_val;

void __g_ensure(int need);
void g_trace_push(const char* nm);
void g_err_msg_set(const char* s);
void g_err_type_set(const char* s);

// ---------------- 值构造API ----------------
Value val_none(void);
Value val_int(long long v);
Value val_double(double v);
Value val_bool(_Bool v);
Value val_char(char v);
Value val_string(const char* s);
// ❗ 删除这一行：Value val_func(AstNode* func_ast);
Value val_array(int len);
// int 泛型数组：创建 IntArray，包装为 VAL_STRUCT_PTR 类型的 Value
Value val_int_array(int len);
Value val_double_array(int len);
Value val_float_array(int len);
Value val_uint_array(int len);
Value val_bool_array(int len);  // 创建 bool 泛型数组（elem_type=VAL_BOOL，彻底隔离）
Value val_char_array(int len);  // 创建 char 泛型数组（elem_type=VAL_CHAR，彻底隔离）
Value val_byte_array(int len);  // 创建 byte 泛型数组（elem_type=VAL_BYTE，彻底隔离）
Value val_int8_array(int len);
Value val_int16_array(int len);
Value val_int32_array(int len);
Value val_int64_array(int len);
Value val_uint8_array(int len);
Value val_uint16_array(int len);  // 创建 int8 泛型数组（elem_type=VAL_INT8，彻底隔离）
// 初始化调用方提供的栈上 ValueArray（items 仍走 gc_alloc），设置 stack_alloc=1，返回 Value
// 编译通道专用：VM 通道始终用 val_array（堆分配）
Value val_array_from_stack(ValueArray* va, int len);
// 初始化调用方提供的栈上 ValueArray + 栈上 items 缓冲区（完全免堆），
// 设置 stack_alloc=1 + items_stack_alloc=1，返回 Value。
// 调用方需保证 items 缓冲区至少 len 个 Value 且已初始化为 val_none()。
// 编译通道专用：VM 通道不使用。
Value val_array_from_stack_items(ValueArray* va, Value* items, int len);
Value val_map(void);
// 编译通道栈分配 map：初始化调用方提供的栈上 ValueMap（buckets/entries 仍堆分配），
// 设置 stack_alloc=1，返回 Value。GC 标记时跳过 ValueMap 自身（无 GCObject 头），
// 但仍标记 buckets/tree 及递归键值。VM 路径不使用此函数。
Value val_map_from_stack(ValueMap* vm);

// ---------------- 内存管理 ----------------
void val_destroy(Value* v);
Value val_clone(const Value* src);

// ---------------- debug工具 ----------------
const char* val_typename(ValueType t);
void val_print(const Value* v);

// ---------------- 自增自减运算 ----------------
Value lumyr_post_inc(Value* v);
Value lumyr_pre_inc(Value* v);
Value lumyr_post_dec(Value* v);
Value lumyr_pre_dec(Value* v);

#endif
