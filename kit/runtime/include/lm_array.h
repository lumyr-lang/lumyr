// lm_array.h —— 数组操作内置函数（原地修改语义）
#ifndef LM_ARRAY_H
#define LM_ARRAY_H

#include "lm_value.h"

Value lumyr_range_n(Value* args, int n);  // range(n) / range(a,b) / range(a,b,step)
Value lumyr_del(Value* arr, Value idx);    // del(arr, idx)：原地删除，返回自身
Value lumyr_insert(Value* arr, Value idx, Value val);  // insert(arr, idx, val)：原地插入
Value lumyr_sum(Value arr);               // sum(arr)：数字数组求和
Value lumyr_avg(Value arr);               // avg(arr)：平均值
Value lumyr_sort(Value arr);              // sort(arr)：升序，返回新数组
Value lumyr_reverse(Value arr);           // reverse(arr)：反转，返回新数组

#endif //LM_ARRAY_H

Value lumyr_array_add(Value* arr, Value val);  // 原地追加（2x扩容），返回自身

// addBytes(arr, seq, start, count)：C 层批量追加，消除大报文逐字节 .add 的解释器开销
//   seq 为 bytes：追加 data[start..start+count) 各字节（以 int 元素）
//   seq 为 int 数组：追加 seq[start..] 元素（语义等价逐元素 add，用于数组间整段拷贝）
//   count<0 表示到 seq 末尾；一次扩容，原地修改返回自身 / bulk append, in-place
Value lumyr_array_add_bytes(Value* arr, Value seq, int start, int count);

Value lumyr_index_of(Value arr, Value x);         // 首个相等下标，-1 未找到
Value lumyr_array_get_safe(Value arr, Value idx); // 安全取，越界 → null
Value lumyr_array_set_method(Value arr, Value idx, Value val); // 原地改，返回数组
Value lumyr_array_first(Value arr);               // 首元素，空 → null
Value lumyr_array_last(Value arr);                // 尾元素，空 → null

Value lumyr_map_add(Value m, Value k, Value v);  // 字典设键值，返回 m（m.add(k,v)）
Value lumyr_array_clear(Value* v);                // 原地清空（保留capacity），返回自身

// 数组/字典扁平化：展开嵌套数组（深度 depth，默认 1；负数 → 无限展开）
Value lumyr_array_flat(Value v, int depth);

// addAll(a, b)：数组原地追加 / 字典原地合并，返回 a
Value lumyr_array_addall(Value* a, Value b);

// 注意：ValueArray 只用于存储 Value 类型数组；类型化数组（IntArray、DoubleArray 等）请使用 lumyr_typed_arrays.h 中的专门结构体
