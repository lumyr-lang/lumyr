#include "lumyr_typed_arrays.h"
#include <stdlib.h>
#include <string.h>

/* ==================== 通用辅助宏 ==================== */
#define TYPED_ARRAY_NEW(type, cap) \
    type* arr = (type*)malloc(sizeof(type)); \
    if(!arr) return NULL; \
    arr->len = 0; \
    arr->cap = (cap > 0) ? cap : 8; \
    arr->items = (typeof(arr->items))malloc((size_t)arr->cap * sizeof(*arr->items)); \
    if(!arr->items) { free(arr); return NULL; } \
    return arr;

#define TYPED_ARRAY_FREE(arr) \
    if(arr) { if(arr->items) free(arr->items); free(arr); }

#define TYPED_ARRAY_ADD(arr, val) \
    if(!arr) return; \
    if(arr->len >= arr->cap) { \
        arr->cap = (arr->cap > 0) ? arr->cap * 2 : 8; \
        arr->items = (typeof(arr->items))realloc(arr->items, (size_t)arr->cap * sizeof(*arr->items)); \
    } \
    arr->items[arr->len++] = val;

#define TYPED_ARRAY_GET(arr, idx) \
    if(!arr || idx < 0 || idx >= arr->len) return 0; \
    return arr->items[idx];

#define TYPED_ARRAY_SET(arr, idx, val) \
    if(!arr || idx < 0 || idx >= arr->len) return; \
    arr->items[idx] = val;

/* 在指定位置插入元素 */
#define TYPED_ARRAY_INSERT(arr, idx, val) \
    if(!arr || idx < 0 || idx > arr->len) return; \
    if(arr->len >= arr->cap) { \
        arr->cap = (arr->cap > 0) ? arr->cap * 2 : 8; \
        arr->items = (typeof(arr->items))realloc(arr->items, (size_t)arr->cap * sizeof(*arr->items)); \
    } \
    for(int i = arr->len; i > idx; i--) { \
        arr->items[i] = arr->items[i - 1]; \
    } \
    arr->items[idx] = val; \
    arr->len++;

/* 删除指定位置的元素 */
#define TYPED_ARRAY_REMOVE(arr, idx) \
    if(!arr || idx < 0 || idx >= arr->len) return; \
    for(int i = idx; i < arr->len - 1; i++) { \
        arr->items[i] = arr->items[i + 1]; \
    } \
    arr->len--;

/* 清空数组 */
#define TYPED_ARRAY_CLEAR(arr) \
    if(!arr) return; \
    arr->len = 0;

/* 查找元素下标 */
#define TYPED_ARRAY_INDEX_OF(arr, val) \
    if(!arr) return -1; \
    for(int i = 0; i < arr->len; i++) { \
        if(arr->items[i] == val) return i; \
    } \
    return -1;

/* 判断是否包含元素 */
#define TYPED_ARRAY_CONTAINS(arr, val) \
    if(!arr) return 0; \
    for(int i = 0; i < arr->len; i++) { \
        if(arr->items[i] == val) return 1; \
    } \
    return 0;

/* 获取首元素 */
#define TYPED_ARRAY_FIRST(arr) \
    if(!arr || arr->len == 0) return 0; \
    return arr->items[0];

/* 获取尾元素 */
#define TYPED_ARRAY_LAST(arr) \
    if(!arr || arr->len == 0) return 0; \
    return arr->items[arr->len - 1];

/* 获取长度 */
#define TYPED_ARRAY_LEN(arr) \
    if(!arr) return 0; \
    return arr->len;

/* 获取容量 */
#define TYPED_ARRAY_CAP(arr) \
    if(!arr) return 0; \
    return arr->cap;

/* 反转数组 */
#define TYPED_ARRAY_REVERSE(arr) \
    if(!arr || arr->len < 2) return; \
    for(int i = 0; i < arr->len / 2; i++) { \
        typeof(arr->items[0]) tmp = arr->items[i]; \
        arr->items[i] = arr->items[arr->len - 1 - i]; \
        arr->items[arr->len - 1 - i] = tmp; \
    }

/* ==================== 整数类型数组实现 ==================== */

/* IntArray */
IntArray* int_array_new(int cap) { TYPED_ARRAY_NEW(IntArray, cap) }
void int_array_free(IntArray* arr) { TYPED_ARRAY_FREE(arr) }
void int_array_add(IntArray* arr, int val) { TYPED_ARRAY_ADD(arr, val) }
int int_array_get(IntArray* arr, int idx) { TYPED_ARRAY_GET(arr, idx) }
void int_array_set(IntArray* arr, int idx, int val) { TYPED_ARRAY_SET(arr, idx, val) }

/* Int8Array */
Int8Array* int8_array_new(int cap) { TYPED_ARRAY_NEW(Int8Array, cap) }
void int8_array_free(Int8Array* arr) { TYPED_ARRAY_FREE(arr) }
void int8_array_add(Int8Array* arr, int8_t val) { TYPED_ARRAY_ADD(arr, val) }
int8_t int8_array_get(Int8Array* arr, int idx) { TYPED_ARRAY_GET(arr, idx) }
void int8_array_set(Int8Array* arr, int idx, int8_t val) { TYPED_ARRAY_SET(arr, idx, val) }

/* Int16Array */
Int16Array* int16_array_new(int cap) { TYPED_ARRAY_NEW(Int16Array, cap) }
void int16_array_free(Int16Array* arr) { TYPED_ARRAY_FREE(arr) }
void int16_array_add(Int16Array* arr, int16_t val) { TYPED_ARRAY_ADD(arr, val) }
int16_t int16_array_get(Int16Array* arr, int idx) { TYPED_ARRAY_GET(arr, idx) }
void int16_array_set(Int16Array* arr, int idx, int16_t val) { TYPED_ARRAY_SET(arr, idx, val) }

/* Int32Array */
Int32Array* int32_array_new(int cap) { TYPED_ARRAY_NEW(Int32Array, cap) }
void int32_array_free(Int32Array* arr) { TYPED_ARRAY_FREE(arr) }
void int32_array_add(Int32Array* arr, int32_t val) { TYPED_ARRAY_ADD(arr, val) }
int32_t int32_array_get(Int32Array* arr, int idx) { TYPED_ARRAY_GET(arr, idx) }
void int32_array_set(Int32Array* arr, int idx, int32_t val) { TYPED_ARRAY_SET(arr, idx, val) }

/* Int64Array */
Int64Array* int64_array_new(int cap) { TYPED_ARRAY_NEW(Int64Array, cap) }
void int64_array_free(Int64Array* arr) { TYPED_ARRAY_FREE(arr) }
void int64_array_add(Int64Array* arr, int64_t val) { TYPED_ARRAY_ADD(arr, val) }
int64_t int64_array_get(Int64Array* arr, int idx) { TYPED_ARRAY_GET(arr, idx) }
void int64_array_set(Int64Array* arr, int idx, int64_t val) { TYPED_ARRAY_SET(arr, idx, val) }

/* LongArray */
LongArray* long_array_new(int cap) { TYPED_ARRAY_NEW(LongArray, cap) }
void long_array_free(LongArray* arr) { TYPED_ARRAY_FREE(arr) }
void long_array_add(LongArray* arr, long val) { TYPED_ARRAY_ADD(arr, val) }
long long_array_get(LongArray* arr, int idx) { TYPED_ARRAY_GET(arr, idx) }
void long_array_set(LongArray* arr, int idx, long val) { TYPED_ARRAY_SET(arr, idx, val) }

/* CharArray */
CharArray* char_array_new(int cap) { TYPED_ARRAY_NEW(CharArray, cap) }
void char_array_free(CharArray* arr) { TYPED_ARRAY_FREE(arr) }
void char_array_add(CharArray* arr, char val) { TYPED_ARRAY_ADD(arr, val) }
char char_array_get(CharArray* arr, int idx) { TYPED_ARRAY_GET(arr, idx) }
void char_array_set(CharArray* arr, int idx, char val) { TYPED_ARRAY_SET(arr, idx, val) }

/* ==================== 无符号整数类型数组实现 ==================== */

/* UInt8Array */
UInt8Array* uint8_array_new(int cap) { TYPED_ARRAY_NEW(UInt8Array, cap) }
void uint8_array_free(UInt8Array* arr) { TYPED_ARRAY_FREE(arr) }
void uint8_array_add(UInt8Array* arr, uint8_t val) { TYPED_ARRAY_ADD(arr, val) }
uint8_t uint8_array_get(UInt8Array* arr, int idx) { TYPED_ARRAY_GET(arr, idx) }
void uint8_array_set(UInt8Array* arr, int idx, uint8_t val) { TYPED_ARRAY_SET(arr, idx, val) }

/* UInt16Array */
UInt16Array* uint16_array_new(int cap) { TYPED_ARRAY_NEW(UInt16Array, cap) }
void uint16_array_free(UInt16Array* arr) { TYPED_ARRAY_FREE(arr) }
void uint16_array_add(UInt16Array* arr, uint16_t val) { TYPED_ARRAY_ADD(arr, val) }
uint16_t uint16_array_get(UInt16Array* arr, int idx) { TYPED_ARRAY_GET(arr, idx) }
void uint16_array_set(UInt16Array* arr, int idx, uint16_t val) { TYPED_ARRAY_SET(arr, idx, val) }

/* UInt32Array */
UInt32Array* uint32_array_new(int cap) { TYPED_ARRAY_NEW(UInt32Array, cap) }
void uint32_array_free(UInt32Array* arr) { TYPED_ARRAY_FREE(arr) }
void uint32_array_add(UInt32Array* arr, uint32_t val) { TYPED_ARRAY_ADD(arr, val) }
uint32_t uint32_array_get(UInt32Array* arr, int idx) { TYPED_ARRAY_GET(arr, idx) }
void uint32_array_set(UInt32Array* arr, int idx, uint32_t val) { TYPED_ARRAY_SET(arr, idx, val) }

/* UInt64Array */
UInt64Array* uint64_array_new(int cap) { TYPED_ARRAY_NEW(UInt64Array, cap) }
void uint64_array_free(UInt64Array* arr) { TYPED_ARRAY_FREE(arr) }
void uint64_array_add(UInt64Array* arr, uint64_t val) { TYPED_ARRAY_ADD(arr, val) }
uint64_t uint64_array_get(UInt64Array* arr, int idx) { TYPED_ARRAY_GET(arr, idx) }
void uint64_array_set(UInt64Array* arr, int idx, uint64_t val) { TYPED_ARRAY_SET(arr, idx, val) }

/* ULongArray */
ULongArray* ulong_array_new(int cap) { TYPED_ARRAY_NEW(ULongArray, cap) }
void ulong_array_free(ULongArray* arr) { TYPED_ARRAY_FREE(arr) }
void ulong_array_add(ULongArray* arr, unsigned long val) { TYPED_ARRAY_ADD(arr, val) }
unsigned long ulong_array_get(ULongArray* arr, int idx) { TYPED_ARRAY_GET(arr, idx) }
void ulong_array_set(ULongArray* arr, int idx, unsigned long val) { TYPED_ARRAY_SET(arr, idx, val) }

/* UCharArray */
UCharArray* uchar_array_new(int cap) { TYPED_ARRAY_NEW(UCharArray, cap) }
void uchar_array_free(UCharArray* arr) { TYPED_ARRAY_FREE(arr) }
void uchar_array_add(UCharArray* arr, unsigned char val) { TYPED_ARRAY_ADD(arr, val) }
unsigned char uchar_array_get(UCharArray* arr, int idx) { TYPED_ARRAY_GET(arr, idx) }
void uchar_array_set(UCharArray* arr, int idx, unsigned char val) { TYPED_ARRAY_SET(arr, idx, val) }

/* ==================== 平台相关类型数组实现 ==================== */

/* SizeTArray */
SizeTArray* size_t_array_new(int cap) { TYPED_ARRAY_NEW(SizeTArray, cap) }
void size_t_array_free(SizeTArray* arr) { TYPED_ARRAY_FREE(arr) }
void size_t_array_add(SizeTArray* arr, size_t val) { TYPED_ARRAY_ADD(arr, val) }
size_t size_t_array_get(SizeTArray* arr, int idx) { TYPED_ARRAY_GET(arr, idx) }
void size_t_array_set(SizeTArray* arr, int idx, size_t val) { TYPED_ARRAY_SET(arr, idx, val) }

/* SSizeTArray */
SSizeTArray* ssize_t_array_new(int cap) { TYPED_ARRAY_NEW(SSizeTArray, cap) }
void ssize_t_array_free(SSizeTArray* arr) { TYPED_ARRAY_FREE(arr) }
void ssize_t_array_add(SSizeTArray* arr, ssize_t val) { TYPED_ARRAY_ADD(arr, val) }
ssize_t ssize_t_array_get(SSizeTArray* arr, int idx) { TYPED_ARRAY_GET(arr, idx) }
void ssize_t_array_set(SSizeTArray* arr, int idx, ssize_t val) { TYPED_ARRAY_SET(arr, idx, val) }

/* ==================== 布尔类型数组实现 ==================== */

/* BoolArray */
BoolArray* bool_array_new(int cap) { TYPED_ARRAY_NEW(BoolArray, cap) }
void bool_array_free(BoolArray* arr) { TYPED_ARRAY_FREE(arr) }
void bool_array_add(BoolArray* arr, _Bool val) { TYPED_ARRAY_ADD(arr, val) }
_Bool bool_array_get(BoolArray* arr, int idx) { TYPED_ARRAY_GET(arr, idx) }
void bool_array_set(BoolArray* arr, int idx, _Bool val) { TYPED_ARRAY_SET(arr, idx, val) }

/* ==================== 浮点类型数组实现 ==================== */

/* FloatArray */
FloatArray* float_array_new(int cap) { TYPED_ARRAY_NEW(FloatArray, cap) }
void float_array_free(FloatArray* arr) { TYPED_ARRAY_FREE(arr) }
void float_array_add(FloatArray* arr, float val) { TYPED_ARRAY_ADD(arr, val) }
float float_array_get(FloatArray* arr, int idx) { TYPED_ARRAY_GET(arr, idx) }
void float_array_set(FloatArray* arr, int idx, float val) { TYPED_ARRAY_SET(arr, idx, val) }

/* DoubleArray */
DoubleArray* double_array_new(int cap) { TYPED_ARRAY_NEW(DoubleArray, cap) }
void double_array_free(DoubleArray* arr) { TYPED_ARRAY_FREE(arr) }
void double_array_add(DoubleArray* arr, double val) { TYPED_ARRAY_ADD(arr, val) }
double double_array_get(DoubleArray* arr, int idx) { TYPED_ARRAY_GET(arr, idx) }
void double_array_set(DoubleArray* arr, int idx, double val) { TYPED_ARRAY_SET(arr, idx, val) }

/* ==================== 指针/字符串类型数组实现 ==================== */

/* StringArray */
StringArray* string_array_new(int cap) { TYPED_ARRAY_NEW(StringArray, cap) }
void string_array_free(StringArray* arr) { TYPED_ARRAY_FREE(arr) }
void string_array_add(StringArray* arr, char* val) { TYPED_ARRAY_ADD(arr, val) }
char* string_array_get(StringArray* arr, int idx) { TYPED_ARRAY_GET(arr, idx) }
void string_array_set(StringArray* arr, int idx, char* val) { TYPED_ARRAY_SET(arr, idx, val) }

/* PtrArray */
PtrArray* ptr_array_new(int cap) { TYPED_ARRAY_NEW(PtrArray, cap) }
void ptr_array_free(PtrArray* arr) { TYPED_ARRAY_FREE(arr) }
void ptr_array_add(PtrArray* arr, void* val) { TYPED_ARRAY_ADD(arr, val) }
void* ptr_array_get(PtrArray* arr, int idx) { TYPED_ARRAY_GET(arr, idx) }
void ptr_array_set(PtrArray* arr, int idx, void* val) { TYPED_ARRAY_SET(arr, idx, val) }

/* ==================== 回调函数类型数组实现 ==================== */

/* CallbackArray */
CallbackArray* callback_array_new(int cap) { TYPED_ARRAY_NEW(CallbackArray, cap) }
void callback_array_free(CallbackArray* arr) { TYPED_ARRAY_FREE(arr) }
void callback_array_add(CallbackArray* arr, void (*val)()) { TYPED_ARRAY_ADD(arr, val) }
void (*callback_array_get(CallbackArray* arr, int idx))() { TYPED_ARRAY_GET(arr, idx) }
void callback_array_set(CallbackArray* arr, int idx, void (*val)()) { TYPED_ARRAY_SET(arr, idx, val) }

/* ==================== 结构体数组和Class数组实现 ==================== */

/* StructArray */
StructArray* struct_array_new(int cap) { TYPED_ARRAY_NEW(StructArray, cap) }
void struct_array_free(StructArray* arr) { TYPED_ARRAY_FREE(arr) }
void struct_array_add(StructArray* arr, void* val) { TYPED_ARRAY_ADD(arr, val) }
void* struct_array_get(StructArray* arr, int idx) { TYPED_ARRAY_GET(arr, idx) }
void struct_array_set(StructArray* arr, int idx, void* val) { TYPED_ARRAY_SET(arr, idx, val) }

/* ClassArray */
ClassArray* class_array_new(int cap) { TYPED_ARRAY_NEW(ClassArray, cap) }
void class_array_free(ClassArray* arr) { TYPED_ARRAY_FREE(arr) }
void class_array_add(ClassArray* arr, void* val) { TYPED_ARRAY_ADD(arr, val) }
void* class_array_get(ClassArray* arr, int idx) { TYPED_ARRAY_GET(arr, idx) }
void class_array_set(ClassArray* arr, int idx, void* val) { TYPED_ARRAY_SET(arr, idx, val) }


/* ==================== 完整操作函数实现（增删改查等） ==================== */

/* IntArray 完整操作实现 */
void int_array_insert(IntArray* arr, int idx, int val) { TYPED_ARRAY_INSERT(arr, idx, val) }
void int_array_remove(IntArray* arr, int idx) { TYPED_ARRAY_REMOVE(arr, idx) }
void int_array_clear(IntArray* arr) { TYPED_ARRAY_CLEAR(arr) }
int int_array_index_of(IntArray* arr, int val) { TYPED_ARRAY_INDEX_OF(arr, val) }
int int_array_contains(IntArray* arr, int val) { TYPED_ARRAY_CONTAINS(arr, val) }
int int_array_first(IntArray* arr) { TYPED_ARRAY_FIRST(arr) }
int int_array_last(IntArray* arr) { TYPED_ARRAY_LAST(arr) }
int int_array_len(IntArray* arr) { TYPED_ARRAY_LEN(arr) }
int int_array_cap(IntArray* arr) { TYPED_ARRAY_CAP(arr) }
void int_array_reverse(IntArray* arr) { TYPED_ARRAY_REVERSE(arr) }

/* Int8Array 完整操作实现 */
void int8_array_insert(Int8Array* arr, int idx, int8_t val) { TYPED_ARRAY_INSERT(arr, idx, val) }
void int8_array_remove(Int8Array* arr, int idx) { TYPED_ARRAY_REMOVE(arr, idx) }
void int8_array_clear(Int8Array* arr) { TYPED_ARRAY_CLEAR(arr) }
int int8_array_index_of(Int8Array* arr, int8_t val) { TYPED_ARRAY_INDEX_OF(arr, val) }
int int8_array_contains(Int8Array* arr, int8_t val) { TYPED_ARRAY_CONTAINS(arr, val) }
int8_t int8_array_first(Int8Array* arr) { TYPED_ARRAY_FIRST(arr) }
int8_t int8_array_last(Int8Array* arr) { TYPED_ARRAY_LAST(arr) }
int int8_array_len(Int8Array* arr) { TYPED_ARRAY_LEN(arr) }
int int8_array_cap(Int8Array* arr) { TYPED_ARRAY_CAP(arr) }
void int8_array_reverse(Int8Array* arr) { TYPED_ARRAY_REVERSE(arr) }

/* Int16Array 完整操作实现 */
void int16_array_insert(Int16Array* arr, int idx, int16_t val) { TYPED_ARRAY_INSERT(arr, idx, val) }
void int16_array_remove(Int16Array* arr, int idx) { TYPED_ARRAY_REMOVE(arr, idx) }
void int16_array_clear(Int16Array* arr) { TYPED_ARRAY_CLEAR(arr) }
int int16_array_index_of(Int16Array* arr, int16_t val) { TYPED_ARRAY_INDEX_OF(arr, val) }
int int16_array_contains(Int16Array* arr, int16_t val) { TYPED_ARRAY_CONTAINS(arr, val) }
int16_t int16_array_first(Int16Array* arr) { TYPED_ARRAY_FIRST(arr) }
int16_t int16_array_last(Int16Array* arr) { TYPED_ARRAY_LAST(arr) }
int int16_array_len(Int16Array* arr) { TYPED_ARRAY_LEN(arr) }
int int16_array_cap(Int16Array* arr) { TYPED_ARRAY_CAP(arr) }
void int16_array_reverse(Int16Array* arr) { TYPED_ARRAY_REVERSE(arr) }

/* Int32Array 完整操作实现 */
void int32_array_insert(Int32Array* arr, int idx, int32_t val) { TYPED_ARRAY_INSERT(arr, idx, val) }
void int32_array_remove(Int32Array* arr, int idx) { TYPED_ARRAY_REMOVE(arr, idx) }
void int32_array_clear(Int32Array* arr) { TYPED_ARRAY_CLEAR(arr) }
int int32_array_index_of(Int32Array* arr, int32_t val) { TYPED_ARRAY_INDEX_OF(arr, val) }
int int32_array_contains(Int32Array* arr, int32_t val) { TYPED_ARRAY_CONTAINS(arr, val) }
int32_t int32_array_first(Int32Array* arr) { TYPED_ARRAY_FIRST(arr) }
int32_t int32_array_last(Int32Array* arr) { TYPED_ARRAY_LAST(arr) }
int int32_array_len(Int32Array* arr) { TYPED_ARRAY_LEN(arr) }
int int32_array_cap(Int32Array* arr) { TYPED_ARRAY_CAP(arr) }
void int32_array_reverse(Int32Array* arr) { TYPED_ARRAY_REVERSE(arr) }

/* Int64Array 完整操作实现 */
void int64_array_insert(Int64Array* arr, int idx, int64_t val) { TYPED_ARRAY_INSERT(arr, idx, val) }
void int64_array_remove(Int64Array* arr, int idx) { TYPED_ARRAY_REMOVE(arr, idx) }
void int64_array_clear(Int64Array* arr) { TYPED_ARRAY_CLEAR(arr) }
int int64_array_index_of(Int64Array* arr, int64_t val) { TYPED_ARRAY_INDEX_OF(arr, val) }
int int64_array_contains(Int64Array* arr, int64_t val) { TYPED_ARRAY_CONTAINS(arr, val) }
int64_t int64_array_first(Int64Array* arr) { TYPED_ARRAY_FIRST(arr) }
int64_t int64_array_last(Int64Array* arr) { TYPED_ARRAY_LAST(arr) }
int int64_array_len(Int64Array* arr) { TYPED_ARRAY_LEN(arr) }
int int64_array_cap(Int64Array* arr) { TYPED_ARRAY_CAP(arr) }
void int64_array_reverse(Int64Array* arr) { TYPED_ARRAY_REVERSE(arr) }

/* LongArray 完整操作实现 */
void long_array_insert(LongArray* arr, int idx, long val) { TYPED_ARRAY_INSERT(arr, idx, val) }
void long_array_remove(LongArray* arr, int idx) { TYPED_ARRAY_REMOVE(arr, idx) }
void long_array_clear(LongArray* arr) { TYPED_ARRAY_CLEAR(arr) }
int long_array_index_of(LongArray* arr, long val) { TYPED_ARRAY_INDEX_OF(arr, val) }
int long_array_contains(LongArray* arr, long val) { TYPED_ARRAY_CONTAINS(arr, val) }
long long_array_first(LongArray* arr) { TYPED_ARRAY_FIRST(arr) }
long long_array_last(LongArray* arr) { TYPED_ARRAY_LAST(arr) }
int long_array_len(LongArray* arr) { TYPED_ARRAY_LEN(arr) }
int long_array_cap(LongArray* arr) { TYPED_ARRAY_CAP(arr) }
void long_array_reverse(LongArray* arr) { TYPED_ARRAY_REVERSE(arr) }

/* CharArray 完整操作实现 */
void char_array_insert(CharArray* arr, int idx, char val) { TYPED_ARRAY_INSERT(arr, idx, val) }
void char_array_remove(CharArray* arr, int idx) { TYPED_ARRAY_REMOVE(arr, idx) }
void char_array_clear(CharArray* arr) { TYPED_ARRAY_CLEAR(arr) }
int char_array_index_of(CharArray* arr, char val) { TYPED_ARRAY_INDEX_OF(arr, val) }
int char_array_contains(CharArray* arr, char val) { TYPED_ARRAY_CONTAINS(arr, val) }
char char_array_first(CharArray* arr) { TYPED_ARRAY_FIRST(arr) }
char char_array_last(CharArray* arr) { TYPED_ARRAY_LAST(arr) }
int char_array_len(CharArray* arr) { TYPED_ARRAY_LEN(arr) }
int char_array_cap(CharArray* arr) { TYPED_ARRAY_CAP(arr) }
void char_array_reverse(CharArray* arr) { TYPED_ARRAY_REVERSE(arr) }

/* UInt8Array 完整操作实现 */
void uint8_array_insert(UInt8Array* arr, int idx, uint8_t val) { TYPED_ARRAY_INSERT(arr, idx, val) }
void uint8_array_remove(UInt8Array* arr, int idx) { TYPED_ARRAY_REMOVE(arr, idx) }
void uint8_array_clear(UInt8Array* arr) { TYPED_ARRAY_CLEAR(arr) }
int uint8_array_index_of(UInt8Array* arr, uint8_t val) { TYPED_ARRAY_INDEX_OF(arr, val) }
int uint8_array_contains(UInt8Array* arr, uint8_t val) { TYPED_ARRAY_CONTAINS(arr, val) }
uint8_t uint8_array_first(UInt8Array* arr) { TYPED_ARRAY_FIRST(arr) }
uint8_t uint8_array_last(UInt8Array* arr) { TYPED_ARRAY_LAST(arr) }
int uint8_array_len(UInt8Array* arr) { TYPED_ARRAY_LEN(arr) }
int uint8_array_cap(UInt8Array* arr) { TYPED_ARRAY_CAP(arr) }
void uint8_array_reverse(UInt8Array* arr) { TYPED_ARRAY_REVERSE(arr) }

/* UInt16Array 完整操作实现 */
void uint16_array_insert(UInt16Array* arr, int idx, uint16_t val) { TYPED_ARRAY_INSERT(arr, idx, val) }
void uint16_array_remove(UInt16Array* arr, int idx) { TYPED_ARRAY_REMOVE(arr, idx) }
void uint16_array_clear(UInt16Array* arr) { TYPED_ARRAY_CLEAR(arr) }
int uint16_array_index_of(UInt16Array* arr, uint16_t val) { TYPED_ARRAY_INDEX_OF(arr, val) }
int uint16_array_contains(UInt16Array* arr, uint16_t val) { TYPED_ARRAY_CONTAINS(arr, val) }
uint16_t uint16_array_first(UInt16Array* arr) { TYPED_ARRAY_FIRST(arr) }
uint16_t uint16_array_last(UInt16Array* arr) { TYPED_ARRAY_LAST(arr) }
int uint16_array_len(UInt16Array* arr) { TYPED_ARRAY_LEN(arr) }
int uint16_array_cap(UInt16Array* arr) { TYPED_ARRAY_CAP(arr) }
void uint16_array_reverse(UInt16Array* arr) { TYPED_ARRAY_REVERSE(arr) }

/* UInt32Array 完整操作实现 */
void uint32_array_insert(UInt32Array* arr, int idx, uint32_t val) { TYPED_ARRAY_INSERT(arr, idx, val) }
void uint32_array_remove(UInt32Array* arr, int idx) { TYPED_ARRAY_REMOVE(arr, idx) }
void uint32_array_clear(UInt32Array* arr) { TYPED_ARRAY_CLEAR(arr) }
int uint32_array_index_of(UInt32Array* arr, uint32_t val) { TYPED_ARRAY_INDEX_OF(arr, val) }
int uint32_array_contains(UInt32Array* arr, uint32_t val) { TYPED_ARRAY_CONTAINS(arr, val) }
uint32_t uint32_array_first(UInt32Array* arr) { TYPED_ARRAY_FIRST(arr) }
uint32_t uint32_array_last(UInt32Array* arr) { TYPED_ARRAY_LAST(arr) }
int uint32_array_len(UInt32Array* arr) { TYPED_ARRAY_LEN(arr) }
int uint32_array_cap(UInt32Array* arr) { TYPED_ARRAY_CAP(arr) }
void uint32_array_reverse(UInt32Array* arr) { TYPED_ARRAY_REVERSE(arr) }

/* UInt64Array 完整操作实现 */
void uint64_array_insert(UInt64Array* arr, int idx, uint64_t val) { TYPED_ARRAY_INSERT(arr, idx, val) }
void uint64_array_remove(UInt64Array* arr, int idx) { TYPED_ARRAY_REMOVE(arr, idx) }
void uint64_array_clear(UInt64Array* arr) { TYPED_ARRAY_CLEAR(arr) }
int uint64_array_index_of(UInt64Array* arr, uint64_t val) { TYPED_ARRAY_INDEX_OF(arr, val) }
int uint64_array_contains(UInt64Array* arr, uint64_t val) { TYPED_ARRAY_CONTAINS(arr, val) }
uint64_t uint64_array_first(UInt64Array* arr) { TYPED_ARRAY_FIRST(arr) }
uint64_t uint64_array_last(UInt64Array* arr) { TYPED_ARRAY_LAST(arr) }
int uint64_array_len(UInt64Array* arr) { TYPED_ARRAY_LEN(arr) }
int uint64_array_cap(UInt64Array* arr) { TYPED_ARRAY_CAP(arr) }
void uint64_array_reverse(UInt64Array* arr) { TYPED_ARRAY_REVERSE(arr) }

/* ULongArray 完整操作实现 */
void ulong_array_insert(ULongArray* arr, int idx, unsigned long val) { TYPED_ARRAY_INSERT(arr, idx, val) }
void ulong_array_remove(ULongArray* arr, int idx) { TYPED_ARRAY_REMOVE(arr, idx) }
void ulong_array_clear(ULongArray* arr) { TYPED_ARRAY_CLEAR(arr) }
int ulong_array_index_of(ULongArray* arr, unsigned long val) { TYPED_ARRAY_INDEX_OF(arr, val) }
int ulong_array_contains(ULongArray* arr, unsigned long val) { TYPED_ARRAY_CONTAINS(arr, val) }
unsigned long ulong_array_first(ULongArray* arr) { TYPED_ARRAY_FIRST(arr) }
unsigned long ulong_array_last(ULongArray* arr) { TYPED_ARRAY_LAST(arr) }
int ulong_array_len(ULongArray* arr) { TYPED_ARRAY_LEN(arr) }
int ulong_array_cap(ULongArray* arr) { TYPED_ARRAY_CAP(arr) }
void ulong_array_reverse(ULongArray* arr) { TYPED_ARRAY_REVERSE(arr) }

/* UCharArray 完整操作实现 */
void uchar_array_insert(UCharArray* arr, int idx, unsigned char val) { TYPED_ARRAY_INSERT(arr, idx, val) }
void uchar_array_remove(UCharArray* arr, int idx) { TYPED_ARRAY_REMOVE(arr, idx) }
void uchar_array_clear(UCharArray* arr) { TYPED_ARRAY_CLEAR(arr) }
int uchar_array_index_of(UCharArray* arr, unsigned char val) { TYPED_ARRAY_INDEX_OF(arr, val) }
int uchar_array_contains(UCharArray* arr, unsigned char val) { TYPED_ARRAY_CONTAINS(arr, val) }
unsigned char uchar_array_first(UCharArray* arr) { TYPED_ARRAY_FIRST(arr) }
unsigned char uchar_array_last(UCharArray* arr) { TYPED_ARRAY_LAST(arr) }
int uchar_array_len(UCharArray* arr) { TYPED_ARRAY_LEN(arr) }
int uchar_array_cap(UCharArray* arr) { TYPED_ARRAY_CAP(arr) }
void uchar_array_reverse(UCharArray* arr) { TYPED_ARRAY_REVERSE(arr) }

/* SizeTArray 完整操作实现 */
void size_t_array_insert(SizeTArray* arr, int idx, size_t val) { TYPED_ARRAY_INSERT(arr, idx, val) }
void size_t_array_remove(SizeTArray* arr, int idx) { TYPED_ARRAY_REMOVE(arr, idx) }
void size_t_array_clear(SizeTArray* arr) { TYPED_ARRAY_CLEAR(arr) }
int size_t_array_index_of(SizeTArray* arr, size_t val) { TYPED_ARRAY_INDEX_OF(arr, val) }
int size_t_array_contains(SizeTArray* arr, size_t val) { TYPED_ARRAY_CONTAINS(arr, val) }
size_t size_t_array_first(SizeTArray* arr) { TYPED_ARRAY_FIRST(arr) }
size_t size_t_array_last(SizeTArray* arr) { TYPED_ARRAY_LAST(arr) }
int size_t_array_len(SizeTArray* arr) { TYPED_ARRAY_LEN(arr) }
int size_t_array_cap(SizeTArray* arr) { TYPED_ARRAY_CAP(arr) }
void size_t_array_reverse(SizeTArray* arr) { TYPED_ARRAY_REVERSE(arr) }

/* SSizeTArray 完整操作实现 */
void ssize_t_array_insert(SSizeTArray* arr, int idx, ssize_t val) { TYPED_ARRAY_INSERT(arr, idx, val) }
void ssize_t_array_remove(SSizeTArray* arr, int idx) { TYPED_ARRAY_REMOVE(arr, idx) }
void ssize_t_array_clear(SSizeTArray* arr) { TYPED_ARRAY_CLEAR(arr) }
int ssize_t_array_index_of(SSizeTArray* arr, ssize_t val) { TYPED_ARRAY_INDEX_OF(arr, val) }
int ssize_t_array_contains(SSizeTArray* arr, ssize_t val) { TYPED_ARRAY_CONTAINS(arr, val) }
ssize_t ssize_t_array_first(SSizeTArray* arr) { TYPED_ARRAY_FIRST(arr) }
ssize_t ssize_t_array_last(SSizeTArray* arr) { TYPED_ARRAY_LAST(arr) }
int ssize_t_array_len(SSizeTArray* arr) { TYPED_ARRAY_LEN(arr) }
int ssize_t_array_cap(SSizeTArray* arr) { TYPED_ARRAY_CAP(arr) }
void ssize_t_array_reverse(SSizeTArray* arr) { TYPED_ARRAY_REVERSE(arr) }

/* BoolArray 完整操作实现 */
void bool_array_insert(BoolArray* arr, int idx, _Bool val) { TYPED_ARRAY_INSERT(arr, idx, val) }
void bool_array_remove(BoolArray* arr, int idx) { TYPED_ARRAY_REMOVE(arr, idx) }
void bool_array_clear(BoolArray* arr) { TYPED_ARRAY_CLEAR(arr) }
int bool_array_index_of(BoolArray* arr, _Bool val) { TYPED_ARRAY_INDEX_OF(arr, val) }
int bool_array_contains(BoolArray* arr, _Bool val) { TYPED_ARRAY_CONTAINS(arr, val) }
_Bool bool_array_first(BoolArray* arr) { TYPED_ARRAY_FIRST(arr) }
_Bool bool_array_last(BoolArray* arr) { TYPED_ARRAY_LAST(arr) }
int bool_array_len(BoolArray* arr) { TYPED_ARRAY_LEN(arr) }
int bool_array_cap(BoolArray* arr) { TYPED_ARRAY_CAP(arr) }
void bool_array_reverse(BoolArray* arr) { TYPED_ARRAY_REVERSE(arr) }

/* FloatArray 完整操作实现 */
void float_array_insert(FloatArray* arr, int idx, float val) { TYPED_ARRAY_INSERT(arr, idx, val) }
void float_array_remove(FloatArray* arr, int idx) { TYPED_ARRAY_REMOVE(arr, idx) }
void float_array_clear(FloatArray* arr) { TYPED_ARRAY_CLEAR(arr) }
int float_array_index_of(FloatArray* arr, float val) { TYPED_ARRAY_INDEX_OF(arr, val) }
int float_array_contains(FloatArray* arr, float val) { TYPED_ARRAY_CONTAINS(arr, val) }
float float_array_first(FloatArray* arr) { TYPED_ARRAY_FIRST(arr) }
float float_array_last(FloatArray* arr) { TYPED_ARRAY_LAST(arr) }
int float_array_len(FloatArray* arr) { TYPED_ARRAY_LEN(arr) }
int float_array_cap(FloatArray* arr) { TYPED_ARRAY_CAP(arr) }
void float_array_reverse(FloatArray* arr) { TYPED_ARRAY_REVERSE(arr) }

/* DoubleArray 完整操作实现 */
void double_array_insert(DoubleArray* arr, int idx, double val) { TYPED_ARRAY_INSERT(arr, idx, val) }
void double_array_remove(DoubleArray* arr, int idx) { TYPED_ARRAY_REMOVE(arr, idx) }
void double_array_clear(DoubleArray* arr) { TYPED_ARRAY_CLEAR(arr) }
int double_array_index_of(DoubleArray* arr, double val) { TYPED_ARRAY_INDEX_OF(arr, val) }
int double_array_contains(DoubleArray* arr, double val) { TYPED_ARRAY_CONTAINS(arr, val) }
double double_array_first(DoubleArray* arr) { TYPED_ARRAY_FIRST(arr) }
double double_array_last(DoubleArray* arr) { TYPED_ARRAY_LAST(arr) }
int double_array_len(DoubleArray* arr) { TYPED_ARRAY_LEN(arr) }
int double_array_cap(DoubleArray* arr) { TYPED_ARRAY_CAP(arr) }
void double_array_reverse(DoubleArray* arr) { TYPED_ARRAY_REVERSE(arr) }

/* StringArray 完整操作实现 */
void string_array_insert(StringArray* arr, int idx, char* val) { TYPED_ARRAY_INSERT(arr, idx, val) }
void string_array_remove(StringArray* arr, int idx) { TYPED_ARRAY_REMOVE(arr, idx) }
void string_array_clear(StringArray* arr) { TYPED_ARRAY_CLEAR(arr) }
int string_array_index_of(StringArray* arr, char* val) { TYPED_ARRAY_INDEX_OF(arr, val) }
int string_array_contains(StringArray* arr, char* val) { TYPED_ARRAY_CONTAINS(arr, val) }
char* string_array_first(StringArray* arr) { TYPED_ARRAY_FIRST(arr) }
char* string_array_last(StringArray* arr) { TYPED_ARRAY_LAST(arr) }
int string_array_len(StringArray* arr) { TYPED_ARRAY_LEN(arr) }
int string_array_cap(StringArray* arr) { TYPED_ARRAY_CAP(arr) }
void string_array_reverse(StringArray* arr) { TYPED_ARRAY_REVERSE(arr) }

/* PtrArray 完整操作实现 */
void ptr_array_insert(PtrArray* arr, int idx, void* val) { TYPED_ARRAY_INSERT(arr, idx, val) }
void ptr_array_remove(PtrArray* arr, int idx) { TYPED_ARRAY_REMOVE(arr, idx) }
void ptr_array_clear(PtrArray* arr) { TYPED_ARRAY_CLEAR(arr) }
int ptr_array_index_of(PtrArray* arr, void* val) { TYPED_ARRAY_INDEX_OF(arr, val) }
int ptr_array_contains(PtrArray* arr, void* val) { TYPED_ARRAY_CONTAINS(arr, val) }
void* ptr_array_first(PtrArray* arr) { TYPED_ARRAY_FIRST(arr) }
void* ptr_array_last(PtrArray* arr) { TYPED_ARRAY_LAST(arr) }
int ptr_array_len(PtrArray* arr) { TYPED_ARRAY_LEN(arr) }
int ptr_array_cap(PtrArray* arr) { TYPED_ARRAY_CAP(arr) }
void ptr_array_reverse(PtrArray* arr) { TYPED_ARRAY_REVERSE(arr) }

/* StructArray 完整操作实现 */
void struct_array_insert(StructArray* arr, int idx, void* val) { TYPED_ARRAY_INSERT(arr, idx, val) }
void struct_array_remove(StructArray* arr, int idx) { TYPED_ARRAY_REMOVE(arr, idx) }
void struct_array_clear(StructArray* arr) { TYPED_ARRAY_CLEAR(arr) }
int struct_array_index_of(StructArray* arr, void* val) { TYPED_ARRAY_INDEX_OF(arr, val) }
int struct_array_contains(StructArray* arr, void* val) { TYPED_ARRAY_CONTAINS(arr, val) }
void* struct_array_first(StructArray* arr) { TYPED_ARRAY_FIRST(arr) }
void* struct_array_last(StructArray* arr) { TYPED_ARRAY_LAST(arr) }
int struct_array_len(StructArray* arr) { TYPED_ARRAY_LEN(arr) }
int struct_array_cap(StructArray* arr) { TYPED_ARRAY_CAP(arr) }
void struct_array_reverse(StructArray* arr) { TYPED_ARRAY_REVERSE(arr) }

/* ClassArray 完整操作实现 */
void class_array_insert(ClassArray* arr, int idx, void* val) { TYPED_ARRAY_INSERT(arr, idx, val) }
void class_array_remove(ClassArray* arr, int idx) { TYPED_ARRAY_REMOVE(arr, idx) }
void class_array_clear(ClassArray* arr) { TYPED_ARRAY_CLEAR(arr) }
int class_array_index_of(ClassArray* arr, void* val) { TYPED_ARRAY_INDEX_OF(arr, val) }
int class_array_contains(ClassArray* arr, void* val) { TYPED_ARRAY_CONTAINS(arr, val) }
void* class_array_first(ClassArray* arr) { TYPED_ARRAY_FIRST(arr) }
void* class_array_last(ClassArray* arr) { TYPED_ARRAY_LAST(arr) }
int class_array_len(ClassArray* arr) { TYPED_ARRAY_LEN(arr) }
int class_array_cap(ClassArray* arr) { TYPED_ARRAY_CAP(arr) }
void class_array_reverse(ClassArray* arr) { TYPED_ARRAY_REVERSE(arr) }

/* ==================== 函数式操作实现（map、filter、reduce、flat） ==================== */

/* IntArray 函数式操作 */
IntArray* int_array_map(IntArray* arr, IntMapFunc func) {
    if(!arr || !func) return NULL;
    IntArray* result = int_array_new(arr->len);
    if(!result) return NULL;
    for(int i = 0; i < arr->len; i++) {
        result->items[i] = func(arr->items[i]);
    }
    result->len = arr->len;
    return result;
}

IntArray* int_array_filter(IntArray* arr, IntFilterFunc func) {
    if(!arr || !func) return NULL;
    IntArray* result = int_array_new(arr->len);
    if(!result) return NULL;
    for(int i = 0; i < arr->len; i++) {
        if(func(arr->items[i])) {
            result->items[result->len++] = arr->items[i];
        }
    }
    return result;
}

int int_array_reduce(IntArray* arr, IntReduceFunc func, int initial) {
    if(!arr || !func) return initial;
    int acc = initial;
    for(int i = 0; i < arr->len; i++) {
        acc = func(acc, arr->items[i]);
    }
    return acc;
}

/* DoubleArray 函数式操作 */
DoubleArray* double_array_map(DoubleArray* arr, DoubleMapFunc func) {
    if(!arr || !func) return NULL;
    DoubleArray* result = double_array_new(arr->len);
    if(!result) return NULL;
    for(int i = 0; i < arr->len; i++) {
        result->items[i] = func(arr->items[i]);
    }
    result->len = arr->len;
    return result;
}

DoubleArray* double_array_filter(DoubleArray* arr, DoubleFilterFunc func) {
    if(!arr || !func) return NULL;
    DoubleArray* result = double_array_new(arr->len);
    if(!result) return NULL;
    for(int i = 0; i < arr->len; i++) {
        if(func(arr->items[i])) {
            result->items[result->len++] = arr->items[i];
        }
    }
    return result;
}

double double_array_reduce(DoubleArray* arr, DoubleReduceFunc func, double initial) {
    if(!arr || !func) return initial;
    double acc = initial;
    for(int i = 0; i < arr->len; i++) {
        acc = func(acc, arr->items[i]);
    }
    return acc;
}

/* StringArray 函数式操作 */
StringArray* string_array_map(StringArray* arr, StringMapFunc func) {
    if(!arr || !func) return NULL;
    StringArray* result = string_array_new(arr->len);
    if(!result) return NULL;
    for(int i = 0; i < arr->len; i++) {
        result->items[i] = func(arr->items[i]);
    }
    result->len = arr->len;
    return result;
}

StringArray* string_array_filter(StringArray* arr, StringFilterFunc func) {
    if(!arr || !func) return NULL;
    StringArray* result = string_array_new(arr->len);
    if(!result) return NULL;
    for(int i = 0; i < arr->len; i++) {
        if(func(arr->items[i])) {
            result->items[result->len++] = arr->items[i];
        }
    }
    return result;
}

StringArray* string_array_flat(StringArray** arrays, int count) {
    if(!arrays || count <= 0) return NULL;
    int total_len = 0;
    for(int i = 0; i < count; i++) {
        if(arrays[i]) total_len += arrays[i]->len;
    }
    StringArray* result = string_array_new(total_len);
    if(!result) return NULL;
    for(int i = 0; i < count; i++) {
        if(arrays[i]) {
            for(int j = 0; j < arrays[i]->len; j++) {
                result->items[result->len++] = arrays[i]->items[j];
            }
        }
    }
    return result;
}

/* PtrArray 函数式操作 */
PtrArray* ptr_array_map(PtrArray* arr, PtrMapFunc func) {
    if(!arr || !func) return NULL;
    PtrArray* result = ptr_array_new(arr->len);
    if(!result) return NULL;
    for(int i = 0; i < arr->len; i++) {
        result->items[i] = func(arr->items[i]);
    }
    result->len = arr->len;
    return result;
}

PtrArray* ptr_array_filter(PtrArray* arr, PtrFilterFunc func) {
    if(!arr || !func) return NULL;
    PtrArray* result = ptr_array_new(arr->len);
    if(!result) return NULL;
    for(int i = 0; i < arr->len; i++) {
        if(func(arr->items[i])) {
            result->items[result->len++] = arr->items[i];
        }
    }
    return result;
}

void* ptr_array_reduce(PtrArray* arr, PtrReduceFunc func, void* initial) {
    if(!arr || !func) return initial;
    void* acc = initial;
    for(int i = 0; i < arr->len; i++) {
        acc = func(acc, arr->items[i]);
    }
    return acc;
}

PtrArray* ptr_array_flat(PtrArray** arrays, int count) {
    if(!arrays || count <= 0) return NULL;
    int total_len = 0;
    for(int i = 0; i < count; i++) {
        if(arrays[i]) total_len += arrays[i]->len;
    }
    PtrArray* result = ptr_array_new(total_len);
    if(!result) return NULL;
    for(int i = 0; i < count; i++) {
        if(arrays[i]) {
            for(int j = 0; j < arrays[i]->len; j++) {
                result->items[result->len++] = arrays[i]->items[j];
            }
        }
    }
    return result;
}

/* StructArray 函数式操作 */
StructArray* struct_array_filter(StructArray* arr, PtrFilterFunc func) {
    if(!arr || !func) return NULL;
    StructArray* result = struct_array_new(arr->len);
    if(!result) return NULL;
    for(int i = 0; i < arr->len; i++) {
        if(func(arr->items[i])) {
            result->items[result->len++] = arr->items[i];
        }
    }
    return result;
}

StructArray* struct_array_flat(StructArray** arrays, int count) {
    if(!arrays || count <= 0) return NULL;
    int total_len = 0;
    for(int i = 0; i < count; i++) {
        if(arrays[i]) total_len += arrays[i]->len;
    }
    StructArray* result = struct_array_new(total_len);
    if(!result) return NULL;
    for(int i = 0; i < count; i++) {
        if(arrays[i]) {
            for(int j = 0; j < arrays[i]->len; j++) {
                result->items[result->len++] = arrays[i]->items[j];
            }
        }
    }
    return result;
}

/* ClassArray 函数式操作 */
ClassArray* class_array_filter(ClassArray* arr, PtrFilterFunc func) {
    if(!arr || !func) return NULL;
    ClassArray* result = class_array_new(arr->len);
    if(!result) return NULL;
    for(int i = 0; i < arr->len; i++) {
        if(func(arr->items[i])) {
            result->items[result->len++] = arr->items[i];
        }
    }
    return result;
}

ClassArray* class_array_flat(ClassArray** arrays, int count) {
    if(!arrays || count <= 0) return NULL;
    int total_len = 0;
    for(int i = 0; i < count; i++) {
        if(arrays[i]) total_len += arrays[i]->len;
    }
    ClassArray* result = class_array_new(total_len);
    if(!result) return NULL;
    for(int i = 0; i < count; i++) {
        if(arrays[i]) {
            for(int j = 0; j < arrays[i]->len; j++) {
                result->items[result->len++] = arrays[i]->items[j];
            }
        }
    }
    return result;
}

/* ==================== 排序操作实现（快速排序） ==================== */

/* 默认比较函数实现 */
int int_default_compare(int a, int b) { return (a > b) - (a < b); }
int int8_default_compare(int8_t a, int8_t b) { return (a > b) - (a < b); }
int int16_default_compare(int16_t a, int16_t b) { return (a > b) - (a < b); }
int int32_default_compare(int32_t a, int32_t b) { return (a > b) - (a < b); }
int int64_default_compare(int64_t a, int64_t b) { return (a > b) - (a < b); }
int long_default_compare(long a, long b) { return (a > b) - (a < b); }
int char_default_compare(char a, char b) { return (a > b) - (a < b); }
int uint8_default_compare(uint8_t a, uint8_t b) { return (a > b) - (a < b); }
int uint16_default_compare(uint16_t a, uint16_t b) { return (a > b) - (a < b); }
int uint32_default_compare(uint32_t a, uint32_t b) { return (a > b) - (a < b); }
int uint64_default_compare(uint64_t a, uint64_t b) { return (a > b) - (a < b); }
int ulong_default_compare(unsigned long a, unsigned long b) { return (a > b) - (a < b); }
int uchar_default_compare(unsigned char a, unsigned char b) { return (a > b) - (a < b); }
int size_t_default_compare(size_t a, size_t b) { return (a > b) - (a < b); }
int ssize_t_default_compare(ssize_t a, ssize_t b) { return (a > b) - (a < b); }
int bool_default_compare(_Bool a, _Bool b) { return (a > b) - (a < b); }
int float_default_compare(float a, float b) { return (a > b) - (a < b); }
int double_default_compare(double a, double b) { return (a > b) - (a < b); }
int string_default_compare(char* a, char* b) {
    if(!a && !b) return 0;
    if(!a) return -1;
    if(!b) return 1;
    return strcmp(a, b);
}
int ptr_default_compare(void* a, void* b) { return (a > b) - (a < b); }

/* 快速排序宏（用于批量生成排序函数） */
#define TYPED_ARRAY_SORT_IMPL(prefix, type, struct_name, cmp_type) \
static void prefix##_quick_sort(type* items, int low, int high, cmp_type cmp) { \
    if(low < high) { \
        type pivot = items[high]; \
        int i = low - 1; \
        for(int j = low; j < high; j++) { \
            if(cmp(items[j], pivot) <= 0) { \
                i++; \
                type tmp = items[i]; items[i] = items[j]; items[j] = tmp; \
            } \
        } \
        type tmp = items[i + 1]; items[i + 1] = items[high]; items[high] = tmp; \
        int pi = i + 1; \
        prefix##_quick_sort(items, low, pi - 1, cmp); \
        prefix##_quick_sort(items, pi + 1, high, cmp); \
    } \
} \
void prefix##_array_sort_with(struct_name* arr, cmp_type cmp) { \
    if(!arr || !cmp || arr->len < 2) return; \
    prefix##_quick_sort(arr->items, 0, arr->len - 1, cmp); \
}

/* 为所有类型化数组生成排序函数 */
TYPED_ARRAY_SORT_IMPL(int, int, IntArray, IntCompareFunc)
TYPED_ARRAY_SORT_IMPL(int8, int8_t, Int8Array, Int8CompareFunc)
TYPED_ARRAY_SORT_IMPL(int16, int16_t, Int16Array, Int16CompareFunc)
TYPED_ARRAY_SORT_IMPL(int32, int32_t, Int32Array, Int32CompareFunc)
TYPED_ARRAY_SORT_IMPL(int64, int64_t, Int64Array, Int64CompareFunc)
TYPED_ARRAY_SORT_IMPL(long, long, LongArray, LongCompareFunc)
TYPED_ARRAY_SORT_IMPL(char, char, CharArray, CharCompareFunc)
TYPED_ARRAY_SORT_IMPL(uint8, uint8_t, UInt8Array, UInt8CompareFunc)
TYPED_ARRAY_SORT_IMPL(uint16, uint16_t, UInt16Array, UInt16CompareFunc)
TYPED_ARRAY_SORT_IMPL(uint32, uint32_t, UInt32Array, UInt32CompareFunc)
TYPED_ARRAY_SORT_IMPL(uint64, uint64_t, UInt64Array, UInt64CompareFunc)
TYPED_ARRAY_SORT_IMPL(ulong, unsigned long, ULongArray, ULongCompareFunc)
TYPED_ARRAY_SORT_IMPL(uchar, unsigned char, UCharArray, UCharCompareFunc)
TYPED_ARRAY_SORT_IMPL(size_t, size_t, SizeTArray, SizeTCompareFunc)
TYPED_ARRAY_SORT_IMPL(ssize_t, ssize_t, SSizeTArray, SSizeTCompareFunc)
TYPED_ARRAY_SORT_IMPL(bool, _Bool, BoolArray, BoolCompareFunc)
TYPED_ARRAY_SORT_IMPL(float, float, FloatArray, FloatCompareFunc)
TYPED_ARRAY_SORT_IMPL(double, double, DoubleArray, DoubleCompareFunc)
TYPED_ARRAY_SORT_IMPL(string, char*, StringArray, StringCompareFunc)
TYPED_ARRAY_SORT_IMPL(ptr, void*, PtrArray, PtrCompareFunc)
TYPED_ARRAY_SORT_IMPL(struct, void*, StructArray, PtrCompareFunc)
TYPED_ARRAY_SORT_IMPL(class, void*, ClassArray, PtrCompareFunc)

/* 使用默认比较函数排序（升序） */
void int_array_sort(IntArray* arr) { int_array_sort_with(arr, int_default_compare); }
void int8_array_sort(Int8Array* arr) { int8_array_sort_with(arr, int8_default_compare); }
void int16_array_sort(Int16Array* arr) { int16_array_sort_with(arr, int16_default_compare); }
void int32_array_sort(Int32Array* arr) { int32_array_sort_with(arr, int32_default_compare); }
void int64_array_sort(Int64Array* arr) { int64_array_sort_with(arr, int64_default_compare); }
void long_array_sort(LongArray* arr) { long_array_sort_with(arr, long_default_compare); }
void char_array_sort(CharArray* arr) { char_array_sort_with(arr, char_default_compare); }
void uint8_array_sort(UInt8Array* arr) { uint8_array_sort_with(arr, uint8_default_compare); }
void uint16_array_sort(UInt16Array* arr) { uint16_array_sort_with(arr, uint16_default_compare); }
void uint32_array_sort(UInt32Array* arr) { uint32_array_sort_with(arr, uint32_default_compare); }
void uint64_array_sort(UInt64Array* arr) { uint64_array_sort_with(arr, uint64_default_compare); }
void ulong_array_sort(ULongArray* arr) { ulong_array_sort_with(arr, ulong_default_compare); }
void uchar_array_sort(UCharArray* arr) { uchar_array_sort_with(arr, uchar_default_compare); }
void size_t_array_sort(SizeTArray* arr) { size_t_array_sort_with(arr, size_t_default_compare); }
void ssize_t_array_sort(SSizeTArray* arr) { ssize_t_array_sort_with(arr, ssize_t_default_compare); }
void bool_array_sort(BoolArray* arr) { bool_array_sort_with(arr, bool_default_compare); }
void float_array_sort(FloatArray* arr) { float_array_sort_with(arr, float_default_compare); }
void double_array_sort(DoubleArray* arr) { double_array_sort_with(arr, double_default_compare); }
void string_array_sort(StringArray* arr) { string_array_sort_with(arr, string_default_compare); }
void ptr_array_sort(PtrArray* arr) { ptr_array_sort_with(arr, ptr_default_compare); }
void struct_array_sort(StructArray* arr) { struct_array_sort_with(arr, ptr_default_compare); }
void class_array_sort(ClassArray* arr) { class_array_sort_with(arr, ptr_default_compare); }

/* ==================== 集合操作实现（addAll、removeAll、containsAll 等） ==================== */

/* 通用集合操作宏（用于批量生成函数） */
#define TYPED_ARRAY_COLLECTION_OPS(prefix, type, struct_name) \
void prefix##_array_add_all(struct_name* arr, struct_name* other) { \
    if(!arr || !other) return; \
    for(int i = 0; i < other->len; i++) { \
        prefix##_array_add(arr, other->items[i]); \
    } \
} \
void prefix##_array_remove_all(struct_name* arr, struct_name* other) { \
    if(!arr || !other) return; \
    for(int i = arr->len - 1; i >= 0; i--) { \
        if(prefix##_array_contains(other, arr->items[i])) { \
            prefix##_array_remove(arr, i); \
        } \
    } \
} \
int prefix##_array_contains_all(struct_name* arr, struct_name* other) { \
    if(!arr || !other) return 0; \
    for(int i = 0; i < other->len; i++) { \
        if(!prefix##_array_contains(arr, other->items[i])) return 0; \
    } \
    return 1; \
} \
void prefix##_array_retain_all(struct_name* arr, struct_name* other) { \
    if(!arr || !other) return; \
    for(int i = arr->len - 1; i >= 0; i--) { \
        if(!prefix##_array_contains(other, arr->items[i])) { \
            prefix##_array_remove(arr, i); \
        } \
    } \
} \
struct_name* prefix##_array_copy(struct_name* arr) { \
    if(!arr) return NULL; \
    struct_name* result = prefix##_array_new(arr->len); \
    if(!result) return NULL; \
    for(int i = 0; i < arr->len; i++) { \
        result->items[i] = arr->items[i]; \
    } \
    result->len = arr->len; \
    return result; \
} \
struct_name* prefix##_array_subarray(struct_name* arr, int start, int end) { \
    if(!arr || start < 0 || end > arr->len || start >= end) return NULL; \
    int len = end - start; \
    struct_name* result = prefix##_array_new(len); \
    if(!result) return NULL; \
    for(int i = 0; i < len; i++) { \
        result->items[i] = arr->items[start + i]; \
    } \
    result->len = len; \
    return result; \
} \
void prefix##_array_swap(struct_name* arr, int i, int j) { \
    if(!arr || i < 0 || i >= arr->len || j < 0 || j >= arr->len) return; \
    type tmp = arr->items[i]; \
    arr->items[i] = arr->items[j]; \
    arr->items[j] = tmp; \
}

/* 为主要类型生成集合操作 */
TYPED_ARRAY_COLLECTION_OPS(int, int, IntArray)
TYPED_ARRAY_COLLECTION_OPS(double, double, DoubleArray)
TYPED_ARRAY_COLLECTION_OPS(string, char*, StringArray)
TYPED_ARRAY_COLLECTION_OPS(ptr, void*, PtrArray)
TYPED_ARRAY_COLLECTION_OPS(struct, void*, StructArray)
TYPED_ARRAY_COLLECTION_OPS(class, void*, ClassArray)

/* 数值类型的 min/max/sum 操作 */
int int_array_min(IntArray* arr) {
    if(!arr || arr->len == 0) return 0;
    int min_val = arr->items[0];
    for(int i = 1; i < arr->len; i++) {
        if(arr->items[i] < min_val) min_val = arr->items[i];
    }
    return min_val;
}

int int_array_max(IntArray* arr) {
    if(!arr || arr->len == 0) return 0;
    int max_val = arr->items[0];
    for(int i = 1; i < arr->len; i++) {
        if(arr->items[i] > max_val) max_val = arr->items[i];
    }
    return max_val;
}

int int_array_sum(IntArray* arr) {
    if(!arr) return 0;
    int sum = 0;
    for(int i = 0; i < arr->len; i++) {
        sum += arr->items[i];
    }
    return sum;
}

double double_array_min(DoubleArray* arr) {
    if(!arr || arr->len == 0) return 0.0;
    double min_val = arr->items[0];
    for(int i = 1; i < arr->len; i++) {
        if(arr->items[i] < min_val) min_val = arr->items[i];
    }
    return min_val;
}

double double_array_max(DoubleArray* arr) {
    if(!arr || arr->len == 0) return 0.0;
    double max_val = arr->items[0];
    for(int i = 1; i < arr->len; i++) {
        if(arr->items[i] > max_val) max_val = arr->items[i];
    }
    return max_val;
}

double double_array_sum(DoubleArray* arr) {
    if(!arr) return 0.0;
    double sum = 0.0;
    for(int i = 0; i < arr->len; i++) {
        sum += arr->items[i];
    }
    return sum;
}

/* 指针类型的 indexOfObject 操作 */
int ptr_array_index_of_object(PtrArray* arr, void* obj) {
    if(!arr) return -1;
    for(int i = 0; i < arr->len; i++) {
        if(arr->items[i] == obj) return i;
    }
    return -1;
}

int struct_array_index_of_object(StructArray* arr, void* obj) {
    if(!arr) return -1;
    for(int i = 0; i < arr->len; i++) {
        if(arr->items[i] == obj) return i;
    }
    return -1;
}

int class_array_index_of_object(ClassArray* arr, void* obj) {
    if(!arr) return -1;
    for(int i = 0; i < arr->len; i++) {
        if(arr->items[i] == obj) return i;
    }
    return -1;
}
