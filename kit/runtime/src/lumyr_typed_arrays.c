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
