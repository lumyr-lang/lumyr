// lm_formdata.h —— formdata 对象（VAL_FORMDATA）
// 多部分表单数据：有序字段（允许同名），值可为文本/bytes/file，用于 multipart/form-data 请求
#ifndef LM_FORMDATA_H
#define LM_FORMDATA_H

#include "lm_value.h"

// 构造空 formdata（cap 为初始容量，<=0 时取默认值）
Value lumyr_formdata_make(int cap);

// 追加一个字段（nameVal 运行时转为字符串；同名追加即多文件/多值语义）
// 返回 1=成功，0=参数错误/内存不足
int lumyr_formdata_add(Value fd, Value nameVal, Value val);

// 设置字段：删除全部同名字段后追加（JS FormData.set 语义；数组值发送时展开）
// 返回 1=成功，0=参数错误/内存不足
int lumyr_formdata_set(Value fd, Value nameVal, Value val);

// 按名取第一个字段值（无匹配返回 none）
Value lumyr_formdata_get_by_name(Value fd, Value nameVal);

// 字段数（同名各计一次）
int lumyr_formdata_len(Value v);

// 取第 i 个字段名（越界返回 NULL）
const char* lumyr_formdata_name(Value v, int i);

// 取第 i 个字段值（越界返回 none）
Value lumyr_formdata_get(Value v, int i);

// 键名数组（含重复名，保持插入顺序）
Value lumyr_formdata_keys(Value v);

// 字段值数组（与 keys 一一对应，保持插入顺序）
Value lumyr_formdata_values(Value v);

// 按名取全部同名值 → 数组（无匹配返回空数组）
Value lumyr_formdata_get_all(Value v, Value nameVal);

// 是否存在同名字段（1=有，0=无/参数错误）
int lumyr_formdata_has(Value v, Value nameVal);

// 删除全部同名字段（原地压缩，保持其余字段相对顺序）
// 返回 1=有删除，0=无匹配/参数错误
int lumyr_formdata_delete(Value v, Value nameVal);

// 字段访问（len）
Value lumyr_formdata_field(Value v, const char* name);

// 字符串化（malloc，调用方 free）
char* lumyr_formdata_to_str(Value v);

#endif // LM_FORMDATA_H
