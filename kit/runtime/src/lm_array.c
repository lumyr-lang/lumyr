// lm_array.c —— 数组操作内置函数
#include "lm_array.h"
#include "gc_runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <limits.h>

/* 辅助函数：根据value的类型提取整数值（各数据类型专用，避免混用long long） */
static long long array_extract_ll(Value v) {
    switch(v.type) {
        case VAL_INT:     return v.v.i;
        case VAL_INT8:    return (long long)v.v.i8;
        case VAL_INT16:   return (long long)v.v.i16;
        case VAL_INT32:   return (long long)v.v.i32;
        case VAL_INT64:   return v.v.i;
        case VAL_BYTE:    return (long long)v.v.u8;
        case VAL_UINT8:   return (long long)v.v.u8;
        case VAL_UINT16:  return (long long)v.v.u16;
        case VAL_UINT32:  return (long long)v.v.u32;
        case VAL_UINT64:  return (long long)v.v.u64;
        case VAL_LONG:    return (long long)v.v.l;
        case VAL_ULONG:   return (long long)v.v.ul;
        case VAL_SIZE_T:  return (long long)v.v.st;
        case VAL_SSIZE_T: return (long long)v.v.sst;
        case VAL_BOOL:    return v.v.b ? 1 : 0;
        case VAL_CHAR:    return (long long)(unsigned char)v.v.c;
        case VAL_DOUBLE:  return (long long)v.v.d;
        default:          return 0;
    }
}

// sort_cmp 排序方向（lumyr_sort 设置后调用 qsort）
static _Thread_local int g_sort_numeric = 0;   /* TLS：多线程 sort 互不干扰 */



Value lumyr_range_n(Value* args, int n) {
    if(n < 1 || n > 3) runtime_error("range() 需要 1 到 3 个参数");
    long long a = 0, b, step = 1;
    if(n == 1) { b = range_to_ll(args[0]); if(b < 0) runtime_error("range() 上界不能为负数"); }
    else if(n == 2) { a = range_to_ll(args[0]); b = range_to_ll(args[1]); }
    else { a = range_to_ll(args[0]); b = range_to_ll(args[1]); step = range_to_ll(args[2]); if(step == 0) runtime_error("range() 步长不能为 0"); }
    // 元素数：正步长 (b-a) 向上；负步长 (a-b) 向上；方向不对 → 空
    long double span = step > 0 ? ((long double)b - a) : ((long double)a - b);
    long long len = 0;
    if(span > 0) len = (long long)((span + (step > 0 ? step : -step) - 1) / (step > 0 ? step : -step));
    if(len < 0) len = 0;
    /* 无人为上限：与 C 一致，数组长度受 int 类型与内存共同约束
       （INT_MAX 仅为类型上限，防止 (int) 截断；实际先受 malloc 失败约束） */
    if(len > INT_MAX) runtime_error("range() 元素数超出数组长度上限（INT_MAX）");
    Value arr = val_array((int)len);
    for(long long i = 0; i < len; i++) {
        arr.v.array->items[i] = lumyr_make_int(a + i * step);
    }
    return arr;
}

Value lumyr_del(Value* arr, Value idx)
{
    if(arr->type == VAL_MAP) {
        lumyr_check_mapname_ro(*arr, idx, "删除");
        lumyr_map_del(arr, idx);
        return *arr;
    }
    if(arr->type != VAL_ARRAY) runtime_error("del() 第一个参数必须是数组或字典");
    if(idx.type != VAL_INT && idx.type != VAL_INT8 && idx.type != VAL_INT16 && idx.type != VAL_INT32 && idx.type != VAL_INT64 && idx.type != VAL_BYTE && idx.type != VAL_UINT8 && idx.type != VAL_UINT16 && idx.type != VAL_UINT32 && idx.type != VAL_UINT64 && idx.type != VAL_LONG && idx.type != VAL_ULONG && idx.type != VAL_SIZE_T && idx.type != VAL_SSIZE_T && idx.type != VAL_BOOL && idx.type != VAL_CHAR && idx.type != VAL_DOUBLE) runtime_error("del() 下标必须是整数");
    long long i = array_extract_ll(idx);
    int n = arr->v.array->len;
    if(i < 0 || i >= n) { char b[96]; snprintf(b, sizeof b, "del() 下标 %lld 越界（长度 %d）", i, n); runtime_error(b); }
    for(int k = (int)i; k < n - 1; k++) arr->v.array->items[k] = arr->v.array->items[k + 1];
    arr->v.array->len--;
    return *arr;
}

// add(arr, val)：原地追加（2x 扩容），返回数组本身支持链式
Value lumyr_array_add(Value* arr, Value val)
{
    if(arr->type != VAL_ARRAY) runtime_error("add() 第一个参数必须是数组");
    int n = arr->v.array->len;
    if(n >= arr->v.array->cap) {
        int newcap = arr->v.array->cap == 0 ? 4 : arr->v.array->cap * 2;
        arr->v.array->items = (Value*)gc_realloc(arr->v.array->items, sizeof(Value) * newcap);
        arr->v.array->cap = newcap;
    }
    gc_write_barrier(val);  /* 增量标记写屏障 */
    gc_remembered_set_check(*arr, val);  /* 老年代容器引用新生代时加入 remembered set */
    arr->v.array->items[n] = val;
    arr->v.array->len++;
    return *arr;
}

// insert(arr, idx, val)：原地插入（idx 允许 0..n），返回数组本身
Value lumyr_insert(Value* arr, Value idx, Value val)
{
    if(arr->type != VAL_ARRAY) runtime_error("insert() 第一个参数必须是数组");
    if(idx.type != VAL_INT && idx.type != VAL_INT8 && idx.type != VAL_INT16 && idx.type != VAL_INT32 && idx.type != VAL_INT64 && idx.type != VAL_BYTE && idx.type != VAL_UINT8 && idx.type != VAL_UINT16 && idx.type != VAL_UINT32 && idx.type != VAL_UINT64 && idx.type != VAL_LONG && idx.type != VAL_ULONG && idx.type != VAL_SIZE_T && idx.type != VAL_SSIZE_T && idx.type != VAL_BOOL && idx.type != VAL_CHAR && idx.type != VAL_DOUBLE) runtime_error("insert() 下标必须是整数");
    long long i = array_extract_ll(idx);
    int n = arr->v.array->len;
    if(i < 0 || i > n) { char b[96]; snprintf(b, sizeof b, "insert() 下标 %lld 越界（允许 0..%d）", i, n); runtime_error(b); }
    // 先追加一个元素（扩容）
    if(n >= arr->v.array->cap) {
        int newcap = arr->v.array->cap == 0 ? 4 : arr->v.array->cap * 2;
        arr->v.array->items = (Value*)gc_realloc(arr->v.array->items, sizeof(Value) * newcap);
        arr->v.array->cap = newcap;
    }
    arr->v.array->len++;
    // 后移
    for(int k = n; k > (int)i; k--) arr->v.array->items[k] = arr->v.array->items[k - 1];
    gc_write_barrier(val);  /* 增量标记写屏障 */
    gc_remembered_set_check(*arr, val);  /* 老年代容器引用新生代时加入 remembered set */
    arr->v.array->items[(int)i] = val;
    return *arr;
}

// indexOf(arr, x)：首个相等元素下标，-1 未找到（== 类型敏感语义）
Value lumyr_index_of(Value arr, Value x)
{
    if(arr.type != VAL_ARRAY) runtime_error("indexOf() 第一个参数必须是数组");
    for(int i = 0; i < arr.v.array->len; i++) {
        Value eq = lumyr_eq(arr.v.array->items[i], x);
        if(lumyr_to_bool(eq)) return lumyr_make_int(i);
    }
    return lumyr_make_int(-1);
}

// arr_get(arr, i)：安全取（越界/非数组 → null，不抛错）
Value lumyr_array_get_safe(Value arr, Value idx)
{
    if(arr.type == VAL_MAP) {
        return lumyr_map_get(arr, idx);
    }
    if(arr.type != VAL_ARRAY) return val_none();
    if(idx.type != VAL_INT && idx.type != VAL_INT8 && idx.type != VAL_INT16 && idx.type != VAL_INT32 && idx.type != VAL_INT64 && idx.type != VAL_BYTE && idx.type != VAL_UINT8 && idx.type != VAL_UINT16 && idx.type != VAL_UINT32 && idx.type != VAL_UINT64 && idx.type != VAL_LONG && idx.type != VAL_ULONG && idx.type != VAL_SIZE_T && idx.type != VAL_SSIZE_T && idx.type != VAL_BOOL && idx.type != VAL_CHAR && idx.type != VAL_DOUBLE) return val_none();
    long long i = array_extract_ll(idx);
    if(i < 0 || i >= arr.v.array->len) return val_none();
    return arr.v.array->items[i];
}

// set(arr, i, v)：原地改（与 a[i]=v 一致），返回数组本身支持链式
Value lumyr_array_set_method(Value arr, Value idx, Value val)
{
    if(arr.type == VAL_MAP) {
        lumyr_check_mapname_ro(arr, idx, "赋值");
        lumyr_map_set(&arr, idx, val);
        return arr;
    }
    if(arr.type != VAL_ARRAY) runtime_error("set() 第一个参数必须是数组或字典");
    if(idx.type != VAL_INT && idx.type != VAL_INT8 && idx.type != VAL_INT16 && idx.type != VAL_INT32 && idx.type != VAL_INT64 && idx.type != VAL_BYTE && idx.type != VAL_UINT8 && idx.type != VAL_UINT16 && idx.type != VAL_UINT32 && idx.type != VAL_UINT64 && idx.type != VAL_LONG && idx.type != VAL_ULONG && idx.type != VAL_SIZE_T && idx.type != VAL_SSIZE_T && idx.type != VAL_BOOL && idx.type != VAL_CHAR && idx.type != VAL_DOUBLE) runtime_error("set() 下标必须是整数");
    long long i = array_extract_ll(idx);
    if(i < 0 || i >= arr.v.array->len) {
        char b[96]; snprintf(b, sizeof b, "set() 下标 %lld 越界（长度 %d）", i, arr.v.array->len);
        runtime_error(b);
    }
    Value* slot = &arr.v.array->items[i];
    gc_write_barrier(val);  /* 增量标记写屏障 */
    gc_remembered_set_check(arr, val);  /* 老年代容器引用新生代时加入 remembered set */
    *slot = val;
    return arr;
}

// first(arr) / last(arr)：首/尾元素（空数组 → null）
Value lumyr_array_first(Value arr)
{
    if(arr.type == VAL_MAP) {
        if(arr.v.map->len == 0) return val_none();
        MapIter it; map_iter_init(&it, arr.v.map);
        Value k, vv; map_iter_next(&it, &k, &vv);
        return vv;
    }
    if(arr.type != VAL_ARRAY || arr.v.array->len == 0) return val_none();
    return arr.v.array->items[0];
}
Value lumyr_array_last(Value arr)
{
    if(arr.type == VAL_MAP) {
        if(arr.v.map->len == 0) return val_none();
        MapIter it; map_iter_init(&it, arr.v.map);
        Value k, vv, lastv;
        while(map_iter_next(&it, &k, &vv)) lastv = vv;
        return lastv;
    }
    if(arr.type != VAL_ARRAY || arr.v.array->len == 0) return val_none();
    return arr.v.array->items[arr.v.array->len - 1];
}

// add 的 map 路径：m.add(k, v) 设键值，返回 m（链式）
Value lumyr_map_add(Value m, Value k, Value v)
{
    if(m.type != VAL_MAP) runtime_error("add() 第一个参数必须是数组或字典");
    lumyr_check_mapname_ro(m, k, "赋值");
    lumyr_map_set(&m, k, v);
    return m;
}

// clear 容器：原地清空（保留 capacity），返回自身支持链式
// 只读 __mapname__ 不被清理：type 构造对象 clear 后类名属性保留
Value lumyr_array_clear(Value* v)
{
    if(v->type == VAL_MAP) {
        ValueMap* m = v->v.map;
        Value cnv = val_none();
        if(lumyr_map_has(*v, lumyr_make_string("__mapname__")))
            cnv = lumyr_map_get(*v, lumyr_make_string("__mapname__"));
        // 清空所有桶（entry 由 GC 回收，此处仅从数据结构摘除）
        for(int bi = 0; bi < m->cap; bi++) {
            m->buckets[bi] = NULL;
            m->tree[bi] = 0;
        }
        m->len = 0;
        if(cnv.type != VAL_NONE) { lumyr_map_set(v, lumyr_make_string("__mapname__"), cnv); }
        return *v;
    }
    if(v->type == VAL_ARRAY) {
        v->v.array->len = 0;
        return *v;
    }
    runtime_error("clear() 参数必须是数组或字典");
    return *v;
}

// floor/ceil：向下/向上取整，返回 int

static double array_sum_d(Value arr, long long* isum, int* all_int)
{
    double dsum = 0;
    *isum = 0; *all_int = 1;
    for(int i = 0; i < arr.v.array->len; i++) {
        Value v = arr.v.array->items[i];
        if(v.type != VAL_INT && v.type != VAL_DOUBLE)
            runtime_error("sum()/avg() 数组元素必须是数字");
        if(v.type == VAL_DOUBLE) *all_int = 0;
        dsum += value_as_number(v);
        if(v.type == VAL_INT) *isum += v.v.i;
        else if(v.type == VAL_INT8 || v.type == VAL_INT16 || v.type == VAL_INT32 || v.type == VAL_INT64 || v.type == VAL_BYTE || v.type == VAL_UINT8 || v.type == VAL_UINT16 || v.type == VAL_UINT32 || v.type == VAL_UINT64 || v.type == VAL_LONG || v.type == VAL_ULONG || v.type == VAL_SIZE_T || v.type == VAL_SSIZE_T || v.type == VAL_BOOL || v.type == VAL_CHAR) *isum += array_extract_ll(v);
    }
    return dsum;
}

Value lumyr_sum(Value arr)
{
    if(arr.type != VAL_ARRAY) runtime_error("sum() 参数必须是数组");
    long long isum; int all_int;
    double dsum = array_sum_d(arr, &isum, &all_int);
    if(all_int) return lumyr_make_int(isum);
    return lumyr_make_double(dsum);
}

Value lumyr_avg(Value arr)
{
    if(arr.type != VAL_ARRAY) runtime_error("avg() 参数必须是数组");
    if(arr.v.array->len == 0) runtime_error("avg() 不能对空数组求平均");
    long long isum; int all_int;
    double dsum = array_sum_d(arr, &isum, &all_int);
    return lumyr_make_double(dsum / arr.v.array->len);
}

// format(fmt, args...)：{} 占位依次替换（{{ 和 }} 转义字面花括号）

static int sort_cmp(const void* pa, const void* pb)
{
    const Value* a = (const Value*)pa;
    const Value* b = (const Value*)pb;
    if(g_sort_numeric) {
        double x = value_as_number(*a), y = value_as_number(*b);
        return (x > y) - (x < y);
    }
    return strcmp(lumyr_str_cstr(a), lumyr_str_cstr(b));
}

Value lumyr_sort(Value arr) {
    if(arr.type != VAL_ARRAY) runtime_error("sort() 参数必须是数组");
    int n = arr.v.array->len;
    int all_num = 1, all_str = 1;
    for(int i = 0; i < n; i++) {
        Value v = arr.v.array->items[i];
        if(v.type != VAL_INT && v.type != VAL_DOUBLE) all_num = 0;
        if(v.type != VAL_STRING) all_str = 0;
    }
    if(!all_num && !all_str) runtime_error("sort() 数组元素须全为数字或全为字符串");
    int numeric = all_num;
    Value r = val_array(n);
    for(int i = 0; i < n; i++) {
        gc_write_barrier(arr.v.array->items[i]);  /* 增量标记写屏障 */
        r.v.array->items[i] = arr.v.array->items[i];
    }
    if(n > 1) {
        g_sort_numeric = numeric;
        qsort(r.v.array->items, (size_t)n, sizeof(Value), sort_cmp);
    }
    return r;
}

// reverse：反转（任意类型）

Value lumyr_reverse(Value arr) {
    if(arr.type != VAL_ARRAY) runtime_error("reverse() 参数必须是数组");
    int n = arr.v.array->len;
    Value r = val_array(n);
    for(int i = 0; i < n; i++) {
        gc_write_barrier(arr.v.array->items[n - 1 - i]);  /* 增量标记写屏障 */
        r.v.array->items[i] = arr.v.array->items[n - 1 - i];
    }
    return r;
}

// strip：去首尾空白（空格/tab/换行/回车/垂直制表/换页）

// ===== 数组/字典扁平化 =====
// depth：展开层数（1=展开一层，2=两层，负数=无限）；字典 → 值数组再展开
typedef struct { Value* items; int len; int cap; } FlatBuf;
static void flat_push(FlatBuf* b, Value v) {
    if(b->len >= b->cap) { b->cap = b->cap ? b->cap * 2 : 16; b->items = (Value*)gc_realloc(b->items, b->cap * sizeof(Value)); }
    gc_write_barrier(v);  /* 增量标记写屏障 */
    b->items[b->len++] = v;
}
static void flat_rec(Value v, int depth, FlatBuf* b) {
    if(v.type == VAL_ARRAY) {
        if(depth > 0) {
            for(int i = 0; i < v.v.array->len; i++) flat_rec(v.v.array->items[i], depth - 1, b);
        } else {
            /* depth 耗尽：数组元素逐个 push（元素若是数组保持原样） */
            for(int i = 0; i < v.v.array->len; i++) flat_push(b, v.v.array->items[i]);
        }
    } else if(v.type == VAL_MAP && depth > 0) {
        /* 字典：按值展开（flat 的字典语义 = 值数组的扁平化） */
        Value vals = lumyr_map_values(v);
        for(int i = 0; i < vals.v.array->len; i++) flat_rec(vals.v.array->items[i], depth - 1, b);
    } else {
        flat_push(b, v);
    }
}
Value lumyr_array_flat(Value v, int depth) {
    if(depth < 0) depth = 2147483647;  /* 负数 → 无限展开 */
    FlatBuf b = {0};
    if(v.type == VAL_ARRAY && depth == 0) {
        /* 深度 0：浅拷贝（顶层元素逐个复制，不递归） */
        for(int i = 0; i < v.v.array->len; i++) flat_push(&b, v.v.array->items[i]);
    } else if(v.type == VAL_MAP && depth == 0) {
        /* 深度 0：字典 → 值数组（不递归） */
        Value vals = lumyr_map_values(v);
        for(int i = 0; i < vals.v.array->len; i++) flat_push(&b, vals.v.array->items[i]);
    } else {
        flat_rec(v, depth, &b);
    }
    Value r = val_array(b.len);
    for(int i = 0; i < b.len; i++) {
        gc_write_barrier(b.items[i]);  /* 增量标记写屏障 */
        r.v.array->items[i] = b.items[i];
    }
    return r;
}

// ===== addAll：数组原地追加 / 字典原地合并 =====
Value lumyr_array_addall(Value* a, Value b) {
    if(a->type == VAL_ARRAY && b.type == VAL_ARRAY) {
        int na = a->v.array->len, nb = b.v.array->len;
        // 确保容量
        while(a->v.array->cap < na + nb) {
            int newcap = a->v.array->cap == 0 ? 4 : a->v.array->cap * 2;
            a->v.array->items = (Value*)gc_realloc(a->v.array->items, sizeof(Value) * newcap);
            a->v.array->cap = newcap;
        }
        for(int i = 0; i < nb; i++) {
            gc_write_barrier(b.v.array->items[i]);  /* 增量标记写屏障 */
            gc_remembered_set_check(*a, b.v.array->items[i]);  /* 老年代容器引用新生代时加入 rs */
            a->v.array->items[na + i] = b.v.array->items[i];
        }
        a->v.array->len = na + nb;
        return *a;
    }
    if(a->type == VAL_MAP && b.type == VAL_MAP) {
        MapIter it; map_iter_init(&it, b.v.map);
        Value k, vv;
        while(map_iter_next(&it, &k, &vv))
            lumyr_map_set(a, k, vv);
        return *a;
    }
    runtime_error("addAll() 参数类型不匹配：数组+数组 或 字典+字典");
    return *a;
}


// ==================== 类型化数组实现 ====================
// 类型化数组：数组元素用对应的 C 类型存储，避免 Value 结构体转换开销
// elem_type 使用 ValueType 枚举，与 FFI 类型系统对齐

#include "lumyr_ffi.h"

// 获取类型化数组元素的大小
