#ifndef LUMYR_TYPED_ARRAYS_H
#define LUMYR_TYPED_ARRAYS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <sys/types.h>   /* ssize_t（POSIX，macOS/Linux 需要） */

/* ==================== 类型化数组结构体 ==================== */
/* 每个类型化数组都有独立的结构体，避免大 union 的内存浪费和类型转换开销 */

/* 整数类型数组（有符号） */
typedef struct {
    int* items;
    int len;
    int cap;
} IntArray;

typedef struct {
    int8_t* items;
    int len;
    int cap;
} Int8Array;

typedef struct {
    int16_t* items;
    int len;
    int cap;
} Int16Array;

typedef struct {
    int32_t* items;
    int len;
    int cap;
} Int32Array;

typedef struct {
    int64_t* items;
    int len;
    int cap;
} Int64Array;

typedef struct {
    long* items;
    int len;
    int cap;
} LongArray;

typedef struct {
    char* items;
    int len;
    int cap;
} CharArray;

/* 整数类型数组（无符号） */
typedef struct {
    uint8_t* items;
    int len;
    int cap;
} UInt8Array;

typedef struct {
    uint16_t* items;
    int len;
    int cap;
} UInt16Array;

typedef struct {
    uint32_t* items;
    int len;
    int cap;
} UInt32Array;

typedef struct {
    uint64_t* items;
    int len;
    int cap;
} UInt64Array;

typedef struct {
    unsigned long* items;
    int len;
    int cap;
} ULongArray;

typedef struct {
    unsigned char* items;
    int len;
    int cap;
} UCharArray;

/* 平台相关类型数组 */
typedef struct {
    size_t* items;
    int len;
    int cap;
} SizeTArray;

typedef struct {
    ssize_t* items;
    int len;
    int cap;
} SSizeTArray;

/* 布尔类型数组 */
typedef struct {
    _Bool* items;
    int len;
    int cap;
} BoolArray;

/* 浮点类型数组 */
typedef struct {
    float* items;
    int len;
    int cap;
} FloatArray;

typedef struct {
    double* items;
    int len;
    int cap;
} DoubleArray;

/* 指针/字符串类型数组 */
typedef struct {
    char** items;
    int len;
    int cap;
} StringArray;

typedef struct {
    void** items;
    int len;
    int cap;
} PtrArray;

/* 回调函数数组 */
typedef struct {
    void (**items)();
    int len;
    int cap;
} CallbackArray;

/* ==================== 函数式操作函数指针类型 ==================== */

/* 映射函数：接受一个元素，返回一个元素（同类型） */
typedef int (*IntMapFunc)(int);
typedef int8_t (*Int8MapFunc)(int8_t);
typedef int16_t (*Int16MapFunc)(int16_t);
typedef int32_t (*Int32MapFunc)(int32_t);
typedef int64_t (*Int64MapFunc)(int64_t);
typedef long (*LongMapFunc)(long);
typedef char (*CharMapFunc)(char);
typedef uint8_t (*UInt8MapFunc)(uint8_t);
typedef uint16_t (*UInt16MapFunc)(uint16_t);
typedef uint32_t (*UInt32MapFunc)(uint32_t);
typedef uint64_t (*UInt64MapFunc)(uint64_t);
typedef unsigned long (*ULongMapFunc)(unsigned long);
typedef unsigned char (*UCharMapFunc)(unsigned char);
typedef size_t (*SizeTMapFunc)(size_t);
typedef ssize_t (*SSizeTMapFunc)(ssize_t);
typedef _Bool (*BoolMapFunc)(_Bool);
typedef float (*FloatMapFunc)(float);
typedef double (*DoubleMapFunc)(double);
typedef char* (*StringMapFunc)(char*);
typedef void* (*PtrMapFunc)(void*);

/* 过滤函数：接受一个元素，返回布尔值（1=保留，0=过滤） */
typedef int (*IntFilterFunc)(int);
typedef int (*Int8FilterFunc)(int8_t);
typedef int (*Int16FilterFunc)(int16_t);
typedef int (*Int32FilterFunc)(int32_t);
typedef int (*Int64FilterFunc)(int64_t);
typedef int (*LongFilterFunc)(long);
typedef int (*CharFilterFunc)(char);
typedef int (*UInt8FilterFunc)(uint8_t);
typedef int (*UInt16FilterFunc)(uint16_t);
typedef int (*UInt32FilterFunc)(uint32_t);
typedef int (*UInt64FilterFunc)(uint64_t);
typedef int (*ULongFilterFunc)(unsigned long);
typedef int (*UCharFilterFunc)(unsigned char);
typedef int (*SizeTFilterFunc)(size_t);
typedef int (*SSizeTFilterFunc)(ssize_t);
typedef int (*BoolFilterFunc)(_Bool);
typedef int (*FloatFilterFunc)(float);
typedef int (*DoubleFilterFunc)(double);
typedef int (*StringFilterFunc)(char*);
typedef int (*PtrFilterFunc)(void*);

/* 归约函数：接受累加器和当前元素，返回新的累加器 */
typedef int (*IntReduceFunc)(int, int);
typedef double (*DoubleReduceFunc)(double, double);
typedef void* (*PtrReduceFunc)(void*, void*);

/* 比较函数：接受两个元素，返回负数（a<b）、零（a==b）、正数（a>b） */
typedef int (*IntCompareFunc)(int, int);
typedef int (*Int8CompareFunc)(int8_t, int8_t);
typedef int (*Int16CompareFunc)(int16_t, int16_t);
typedef int (*Int32CompareFunc)(int32_t, int32_t);
typedef int (*Int64CompareFunc)(int64_t, int64_t);
typedef int (*LongCompareFunc)(long, long);
typedef int (*CharCompareFunc)(char, char);
typedef int (*UInt8CompareFunc)(uint8_t, uint8_t);
typedef int (*UInt16CompareFunc)(uint16_t, uint16_t);
typedef int (*UInt32CompareFunc)(uint32_t, uint32_t);
typedef int (*UInt64CompareFunc)(uint64_t, uint64_t);
typedef int (*ULongCompareFunc)(unsigned long, unsigned long);
typedef int (*UCharCompareFunc)(unsigned char, unsigned char);
typedef int (*SizeTCompareFunc)(size_t, size_t);
typedef int (*SSizeTCompareFunc)(ssize_t, ssize_t);
typedef int (*BoolCompareFunc)(_Bool, _Bool);
typedef int (*FloatCompareFunc)(float, float);
typedef int (*DoubleCompareFunc)(double, double);
typedef int (*StringCompareFunc)(char*, char*);
typedef int (*PtrCompareFunc)(void*, void*);

/* 默认比较函数 */
int int_default_compare(int a, int b);
int int8_default_compare(int8_t a, int8_t b);
int int16_default_compare(int16_t a, int16_t b);
int int32_default_compare(int32_t a, int32_t b);
int int64_default_compare(int64_t a, int64_t b);
int long_default_compare(long a, long b);
int char_default_compare(char a, char b);
int uint8_default_compare(uint8_t a, uint8_t b);
int uint16_default_compare(uint16_t a, uint16_t b);
int uint32_default_compare(uint32_t a, uint32_t b);
int uint64_default_compare(uint64_t a, uint64_t b);
int ulong_default_compare(unsigned long a, unsigned long b);
int uchar_default_compare(unsigned char a, unsigned char b);
int size_t_default_compare(size_t a, size_t b);
int ssize_t_default_compare(ssize_t a, ssize_t b);
int bool_default_compare(_Bool a, _Bool b);
int float_default_compare(float a, float b);
int double_default_compare(double a, double b);
int string_default_compare(char* a, char* b);
int ptr_default_compare(void* a, void* b);

/* 结构体数组（类型安全，存储 VAL_STRUCT_PTR 类型的指针） */
typedef struct {
    void** items;      /* 结构体指针数组，每个元素都是 VAL_STRUCT_PTR 类型 */
    int len;
    int cap;
} StructArray;

/* Class数组（类型安全，存储 VAL_CLASS_PTR 类型的指针） */
typedef struct {
    void** items;      /* class实例指针数组，每个元素都是 VAL_CLASS_PTR 类型 */
    int len;
    int cap;
} ClassArray;

/* ==================== 类型化数组统一访问宏 ==================== */
/* 通过这些宏可以统一访问不同类型的数组，避免重复代码 */

#define TYPED_ARRAY_LEN(arr) ((arr)->len)
#define TYPED_ARRAY_CAP(arr) ((arr)->cap)
#define TYPED_ARRAY_ITEMS(arr, type) ((type*)((arr)->items))

/* ==================== 类型化数组创建/销毁函数声明 ==================== */

/* 整数类型 */
IntArray* int_array_new(int cap);
void int_array_free(IntArray* arr);
void int_array_add(IntArray* arr, int val);
int int_array_get(IntArray* arr, int idx);
void int_array_set(IntArray* arr, int idx, int val);

Int8Array* int8_array_new(int cap);
void int8_array_free(Int8Array* arr);
void int8_array_add(Int8Array* arr, int8_t val);
int8_t int8_array_get(Int8Array* arr, int idx);
void int8_array_set(Int8Array* arr, int idx, int8_t val);

Int16Array* int16_array_new(int cap);
void int16_array_free(Int16Array* arr);
void int16_array_add(Int16Array* arr, int16_t val);
int16_t int16_array_get(Int16Array* arr, int idx);
void int16_array_set(Int16Array* arr, int idx, int16_t val);

Int32Array* int32_array_new(int cap);
void int32_array_free(Int32Array* arr);
void int32_array_add(Int32Array* arr, int32_t val);
int32_t int32_array_get(Int32Array* arr, int idx);
void int32_array_set(Int32Array* arr, int idx, int32_t val);

Int64Array* int64_array_new(int cap);
void int64_array_free(Int64Array* arr);
void int64_array_add(Int64Array* arr, int64_t val);
int64_t int64_array_get(Int64Array* arr, int idx);
void int64_array_set(Int64Array* arr, int idx, int64_t val);

LongArray* long_array_new(int cap);
void long_array_free(LongArray* arr);
void long_array_add(LongArray* arr, long val);
long long_array_get(LongArray* arr, int idx);
void long_array_set(LongArray* arr, int idx, long val);

CharArray* char_array_new(int cap);
void char_array_free(CharArray* arr);
void char_array_add(CharArray* arr, char val);
char char_array_get(CharArray* arr, int idx);
void char_array_set(CharArray* arr, int idx, char val);

/* 无符号整数类型 */
UInt8Array* uint8_array_new(int cap);
void uint8_array_free(UInt8Array* arr);
void uint8_array_add(UInt8Array* arr, uint8_t val);
uint8_t uint8_array_get(UInt8Array* arr, int idx);
void uint8_array_set(UInt8Array* arr, int idx, uint8_t val);

UInt16Array* uint16_array_new(int cap);
void uint16_array_free(UInt16Array* arr);
void uint16_array_add(UInt16Array* arr, uint16_t val);
uint16_t uint16_array_get(UInt16Array* arr, int idx);
void uint16_array_set(UInt16Array* arr, int idx, uint16_t val);

UInt32Array* uint32_array_new(int cap);
void uint32_array_free(UInt32Array* arr);
void uint32_array_add(UInt32Array* arr, uint32_t val);
uint32_t uint32_array_get(UInt32Array* arr, int idx);
void uint32_array_set(UInt32Array* arr, int idx, uint32_t val);

UInt64Array* uint64_array_new(int cap);
void uint64_array_free(UInt64Array* arr);
void uint64_array_add(UInt64Array* arr, uint64_t val);
uint64_t uint64_array_get(UInt64Array* arr, int idx);
void uint64_array_set(UInt64Array* arr, int idx, uint64_t val);

ULongArray* ulong_array_new(int cap);
void ulong_array_free(ULongArray* arr);
void ulong_array_add(ULongArray* arr, unsigned long val);
unsigned long ulong_array_get(ULongArray* arr, int idx);
void ulong_array_set(ULongArray* arr, int idx, unsigned long val);

UCharArray* uchar_array_new(int cap);
void uchar_array_free(UCharArray* arr);
void uchar_array_add(UCharArray* arr, unsigned char val);
unsigned char uchar_array_get(UCharArray* arr, int idx);
void uchar_array_set(UCharArray* arr, int idx, unsigned char val);

/* 平台相关类型 */
SizeTArray* size_t_array_new(int cap);
void size_t_array_free(SizeTArray* arr);
void size_t_array_add(SizeTArray* arr, size_t val);
size_t size_t_array_get(SizeTArray* arr, int idx);
void size_t_array_set(SizeTArray* arr, int idx, size_t val);

SSizeTArray* ssize_t_array_new(int cap);
void ssize_t_array_free(SSizeTArray* arr);
void ssize_t_array_add(SSizeTArray* arr, ssize_t val);
ssize_t ssize_t_array_get(SSizeTArray* arr, int idx);
void ssize_t_array_set(SSizeTArray* arr, int idx, ssize_t val);

/* 布尔类型 */
BoolArray* bool_array_new(int cap);
void bool_array_free(BoolArray* arr);
void bool_array_add(BoolArray* arr, _Bool val);
_Bool bool_array_get(BoolArray* arr, int idx);
void bool_array_set(BoolArray* arr, int idx, _Bool val);

/* 浮点类型 */
FloatArray* float_array_new(int cap);
void float_array_free(FloatArray* arr);
void float_array_add(FloatArray* arr, float val);
float float_array_get(FloatArray* arr, int idx);
void float_array_set(FloatArray* arr, int idx, float val);

DoubleArray* double_array_new(int cap);
void double_array_free(DoubleArray* arr);
void double_array_add(DoubleArray* arr, double val);
double double_array_get(DoubleArray* arr, int idx);
void double_array_set(DoubleArray* arr, int idx, double val);

/* 指针/字符串类型 */
StringArray* string_array_new(int cap);
void string_array_free(StringArray* arr);
void string_array_add(StringArray* arr, char* val);
char* string_array_get(StringArray* arr, int idx);
void string_array_set(StringArray* arr, int idx, char* val);

PtrArray* ptr_array_new(int cap);
void ptr_array_free(PtrArray* arr);
void ptr_array_add(PtrArray* arr, void* val);
void* ptr_array_get(PtrArray* arr, int idx);
void ptr_array_set(PtrArray* arr, int idx, void* val);

/* 回调函数类型 */
CallbackArray* callback_array_new(int cap);
void callback_array_free(CallbackArray* arr);
void callback_array_add(CallbackArray* arr, void (*val)());
void (*callback_array_get(CallbackArray* arr, int idx))();
void callback_array_set(CallbackArray* arr, int idx, void (*val)());

/* ==================== 结构体数组和Class数组函数声明 ==================== */

/* StructArray */
StructArray* struct_array_new(int cap);
void struct_array_free(StructArray* arr);
void struct_array_add(StructArray* arr, void* val);
void* struct_array_get(StructArray* arr, int idx);
void struct_array_set(StructArray* arr, int idx, void* val);

/* ClassArray */
ClassArray* class_array_new(int cap);
void class_array_free(ClassArray* arr);
void class_array_add(ClassArray* arr, void* val);
void* class_array_get(ClassArray* arr, int idx);
void class_array_set(ClassArray* arr, int idx, void* val);
void class_array_insert(ClassArray* arr, int idx, void* val);
void class_array_remove(ClassArray* arr, int idx);
void class_array_clear(ClassArray* arr);
int class_array_index_of(ClassArray* arr, void* val);
int class_array_contains(ClassArray* arr, void* val);
void* class_array_first(ClassArray* arr);
void* class_array_last(ClassArray* arr);
int class_array_len(ClassArray* arr);
int class_array_cap(ClassArray* arr);
void class_array_reverse(ClassArray* arr);

/* ==================== 函数式操作函数声明（map、filter、reduce、flat） ==================== */

/* IntArray */
IntArray* int_array_map(IntArray* arr, IntMapFunc func);
IntArray* int_array_filter(IntArray* arr, IntFilterFunc func);
int int_array_reduce(IntArray* arr, IntReduceFunc func, int initial);

/* DoubleArray */
DoubleArray* double_array_map(DoubleArray* arr, DoubleMapFunc func);
DoubleArray* double_array_filter(DoubleArray* arr, DoubleFilterFunc func);
double double_array_reduce(DoubleArray* arr, DoubleReduceFunc func, double initial);

/* StringArray */
StringArray* string_array_map(StringArray* arr, StringMapFunc func);
StringArray* string_array_filter(StringArray* arr, StringFilterFunc func);
StringArray* string_array_flat(StringArray** arrays, int count);

/* PtrArray */
PtrArray* ptr_array_map(PtrArray* arr, PtrMapFunc func);
PtrArray* ptr_array_filter(PtrArray* arr, PtrFilterFunc func);
void* ptr_array_reduce(PtrArray* arr, PtrReduceFunc func, void* initial);
PtrArray* ptr_array_flat(PtrArray** arrays, int count);

/* StructArray */
StructArray* struct_array_filter(StructArray* arr, PtrFilterFunc func);
StructArray* struct_array_flat(StructArray** arrays, int count);

/* ClassArray */
ClassArray* class_array_filter(ClassArray* arr, PtrFilterFunc func);
ClassArray* class_array_flat(ClassArray** arrays, int count);

/* ==================== 排序操作函数声明 ==================== */

/* 使用自定义比较函数排序 */
void int_array_sort_with(IntArray* arr, IntCompareFunc cmp);
void int8_array_sort_with(Int8Array* arr, Int8CompareFunc cmp);
void int16_array_sort_with(Int16Array* arr, Int16CompareFunc cmp);
void int32_array_sort_with(Int32Array* arr, Int32CompareFunc cmp);
void int64_array_sort_with(Int64Array* arr, Int64CompareFunc cmp);
void long_array_sort_with(LongArray* arr, LongCompareFunc cmp);
void char_array_sort_with(CharArray* arr, CharCompareFunc cmp);
void uint8_array_sort_with(UInt8Array* arr, UInt8CompareFunc cmp);
void uint16_array_sort_with(UInt16Array* arr, UInt16CompareFunc cmp);
void uint32_array_sort_with(UInt32Array* arr, UInt32CompareFunc cmp);
void uint64_array_sort_with(UInt64Array* arr, UInt64CompareFunc cmp);
void ulong_array_sort_with(ULongArray* arr, ULongCompareFunc cmp);
void uchar_array_sort_with(UCharArray* arr, UCharCompareFunc cmp);
void size_t_array_sort_with(SizeTArray* arr, SizeTCompareFunc cmp);
void ssize_t_array_sort_with(SSizeTArray* arr, SSizeTCompareFunc cmp);
void bool_array_sort_with(BoolArray* arr, BoolCompareFunc cmp);
void float_array_sort_with(FloatArray* arr, FloatCompareFunc cmp);
void double_array_sort_with(DoubleArray* arr, DoubleCompareFunc cmp);
void string_array_sort_with(StringArray* arr, StringCompareFunc cmp);
void ptr_array_sort_with(PtrArray* arr, PtrCompareFunc cmp);
void struct_array_sort_with(StructArray* arr, PtrCompareFunc cmp);
void class_array_sort_with(ClassArray* arr, PtrCompareFunc cmp);

/* 使用默认比较函数排序（升序） */
void int_array_sort(IntArray* arr);
void int8_array_sort(Int8Array* arr);
void int16_array_sort(Int16Array* arr);
void int32_array_sort(Int32Array* arr);
void int64_array_sort(Int64Array* arr);
void long_array_sort(LongArray* arr);
void char_array_sort(CharArray* arr);
void uint8_array_sort(UInt8Array* arr);
void uint16_array_sort(UInt16Array* arr);
void uint32_array_sort(UInt32Array* arr);
void uint64_array_sort(UInt64Array* arr);
void ulong_array_sort(ULongArray* arr);
void uchar_array_sort(UCharArray* arr);
void size_t_array_sort(SizeTArray* arr);
void ssize_t_array_sort(SSizeTArray* arr);
void bool_array_sort(BoolArray* arr);
void float_array_sort(FloatArray* arr);
void double_array_sort(DoubleArray* arr);
void string_array_sort(StringArray* arr);
void ptr_array_sort(PtrArray* arr);
void struct_array_sort(StructArray* arr);
void class_array_sort(ClassArray* arr);

/* ==================== 集合操作函数声明（addAll、removeAll、containsAll 等） ==================== */

/* IntArray */
void int_array_add_all(IntArray* arr, IntArray* other);
void int_array_remove_all(IntArray* arr, IntArray* other);
int int_array_contains_all(IntArray* arr, IntArray* other);
void int_array_retain_all(IntArray* arr, IntArray* other);
IntArray* int_array_copy(IntArray* arr);
IntArray* int_array_subarray(IntArray* arr, int start, int end);
void int_array_swap(IntArray* arr, int i, int j);
int int_array_min(IntArray* arr);
int int_array_max(IntArray* arr);
int int_array_sum(IntArray* arr);

/* DoubleArray */
void double_array_add_all(DoubleArray* arr, DoubleArray* other);
void double_array_remove_all(DoubleArray* arr, DoubleArray* other);
int double_array_contains_all(DoubleArray* arr, DoubleArray* other);
void double_array_retain_all(DoubleArray* arr, DoubleArray* other);
DoubleArray* double_array_copy(DoubleArray* arr);
DoubleArray* double_array_subarray(DoubleArray* arr, int start, int end);
void double_array_swap(DoubleArray* arr, int i, int j);
double double_array_min(DoubleArray* arr);
double double_array_max(DoubleArray* arr);
double double_array_sum(DoubleArray* arr);

/* StringArray */
void string_array_add_all(StringArray* arr, StringArray* other);
void string_array_remove_all(StringArray* arr, StringArray* other);
int string_array_contains_all(StringArray* arr, StringArray* other);
void string_array_retain_all(StringArray* arr, StringArray* other);
StringArray* string_array_copy(StringArray* arr);
StringArray* string_array_subarray(StringArray* arr, int start, int end);
void string_array_swap(StringArray* arr, int i, int j);

/* PtrArray */
void ptr_array_add_all(PtrArray* arr, PtrArray* other);
void ptr_array_remove_all(PtrArray* arr, PtrArray* other);
int ptr_array_contains_all(PtrArray* arr, PtrArray* other);
void ptr_array_retain_all(PtrArray* arr, PtrArray* other);
PtrArray* ptr_array_copy(PtrArray* arr);
PtrArray* ptr_array_subarray(PtrArray* arr, int start, int end);
void ptr_array_swap(PtrArray* arr, int i, int j);
int ptr_array_index_of_object(PtrArray* arr, void* obj);

/* StructArray */
void struct_array_add_all(StructArray* arr, StructArray* other);
void struct_array_remove_all(StructArray* arr, StructArray* other);
int struct_array_contains_all(StructArray* arr, StructArray* other);
void struct_array_retain_all(StructArray* arr, StructArray* other);
StructArray* struct_array_copy(StructArray* arr);
StructArray* struct_array_subarray(StructArray* arr, int start, int end);
void struct_array_swap(StructArray* arr, int i, int j);
int struct_array_index_of_object(StructArray* arr, void* obj);

/* ClassArray */
void class_array_add_all(ClassArray* arr, ClassArray* other);
void class_array_remove_all(ClassArray* arr, ClassArray* other);
int class_array_contains_all(ClassArray* arr, ClassArray* other);
void class_array_retain_all(ClassArray* arr, ClassArray* other);
ClassArray* class_array_copy(ClassArray* arr);
ClassArray* class_array_subarray(ClassArray* arr, int start, int end);
void class_array_swap(ClassArray* arr, int i, int j);
int class_array_index_of_object(ClassArray* arr, void* obj);

/* ==================== 通用数组操作宏（用于批量生成函数声明） ==================== */
/*
#define TYPED_ARRAY_DECLARE(prefix, type) \
prefix##Array* prefix##_array_new(int cap); \
void prefix##_array_free(prefix##Array* arr); \
void prefix##_array_add(prefix##Array* arr, type val); \
type prefix##_array_get(prefix##Array* arr, int idx); \
void prefix##_array_set(prefix##Array* arr, int idx, type val); \
void prefix##_array_insert(prefix##Array* arr, int idx, type val); \
void prefix##_array_remove(prefix##Array* arr, int idx); \
void prefix##_array_clear(prefix##Array* arr); \
int prefix##_array_index_of(prefix##Array* arr, type val); \
int prefix##_array_contains(prefix##Array* arr, type val); \
type prefix##_array_first(prefix##Array* arr); \
type prefix##_array_last(prefix##Array* arr); \
int prefix##_array_len(prefix##Array* arr); \
int prefix##_array_cap(prefix##Array* arr); \
void prefix##_array_reverse(prefix##Array* arr);
*/

#endif /* LUMYR_TYPED_ARRAYS_H */
