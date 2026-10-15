// lumyr_typed_maps.h —— 类型化字典（扁平化 8 块连续数组 + 索引版红黑树）
// 与 TypedArray 对齐：运行时按 key_type/value_type 分发，GC 仅标记 PTR 族元素
// 红黑树阈值对齐 VAL_MAP：桶链表 >8 且总容量 >=64 树化；红黑树 <6 退化为链表
#ifndef LUMYR_TYPED_MAPS_H
#define LUMYR_TYPED_MAPS_H

#include "lumyr_value_type.h"
#include <stdint.h>

#define TMAP_INIT_CAP 16
#define TMAP_LOAD_FACTOR 0.75
#define TMAP_TREEIFY_THRESHOLD 8
#define TMAP_UNTREEIFY_THRESHOLD 6
#define TMAP_MIN_TREEIFY_CAP 64
#define TMAP_RED   0
#define TMAP_BLACK 1

/* ==================== 3.1 C 层 16 个类型安全 struct（宏生成，参照 typed array） ==================== */
/* 给 C API/FFI 类型安全调用，VM/GC 路径不直接用它们（VM/GC 用 3.2 节的统一 TypedMap） */
#define TYPED_MAP_STRUCT(KPREFIX, KTYPE, VPREFIX, VTYPE) \
typedef struct { \
    KTYPE*     keys; \
    VTYPE*     values; \
    uint32_t*  hashes; \
    int32_t*   next; \
    int32_t*   left; \
    int32_t*   right; \
    int32_t*   parent; \
    int8_t*    rb_color; \
    int32_t*   buckets; \
    unsigned char* tree; \
    int        len; \
    int        cap; \
    uint8_t    stack_alloc; \
    volatile int write_flag; \
} KPREFIX##_##VPREFIX##Map;

TYPED_MAP_STRUCT(Int, int64_t, Int, int64_t)          /* IntIntMap */
TYPED_MAP_STRUCT(Int, int64_t, Double, double)        /* IntDoubleMap */
TYPED_MAP_STRUCT(Int, int64_t, String, char*)         /* IntStringMap */
TYPED_MAP_STRUCT(Int, int64_t, Ptr, void*)            /* IntPtrMap */
TYPED_MAP_STRUCT(Double, double, Int, int64_t)         /* DoubleIntMap */
TYPED_MAP_STRUCT(Double, double, Double, double)      /* DoubleDoubleMap */
TYPED_MAP_STRUCT(Double, double, String, char*)       /* DoubleStringMap */
TYPED_MAP_STRUCT(Double, double, Ptr, void*)          /* DoublePtrMap */
TYPED_MAP_STRUCT(String, char*, Int, int64_t)          /* StringIntMap */
TYPED_MAP_STRUCT(String, char*, Double, double)       /* StringDoubleMap */
TYPED_MAP_STRUCT(String, char*, String, char*)        /* StringStringMap */
TYPED_MAP_STRUCT(String, char*, Ptr, void*)           /* StringPtrMap */
TYPED_MAP_STRUCT(Ptr, void*, Int, int64_t)            /* PtrIntMap */
TYPED_MAP_STRUCT(Ptr, void*, Double, double)          /* PtrDoubleMap */
TYPED_MAP_STRUCT(Ptr, void*, String, char*)           /* PtrStringMap */
TYPED_MAP_STRUCT(Ptr, void*, Ptr, void*)              /* PtrPtrMap */

/* ==================== 3.2 运行时统一 TypedMap 容器（VM/GC 只认这个） ==================== */
/* 参照 TypedArray 运行时结构：void* keys/values + key_type/value_type 运行时分发
 * 扁平化 8 块连续数组 + buckets + tree + 索引版红黑树
 * free_head/free_count: 空闲链表（next 串联，避免频繁 realloc） */
typedef struct TypedMap {
    void* keys;              /* 按 key_type 紧凑存储的 key 数组 (int64_t / double / char / void 指针) */
    void* values;            /* 按 value_type 紧凑存储的 value 数组 */
    uint32_t* hashes;        /* 预算好的 key 哈希数组（避免重复哈希） */
    int32_t* next;           /* 链表 next 索引（拉链；-1=链尾） */
    int32_t* left;           /* 红黑树左子索引（-1=无；桶树化后生效） */
    int32_t* right;          /* 红黑树右子索引 */
    int32_t* parent;         /* 红黑树父索引 */
    int8_t* rb_color;        /* 红黑树节点颜色：0=红, 1=黑（与 MapEntry.color 同语义） */
    int32_t* buckets;        /* 桶头索引数组，-1=空桶（cap 个槽，2 的幂） */
    unsigned char* tree;     /* 桶类型标记：0=链表, 1=红黑树 */
    int len;
    int cap;                 /* 桶数 = keys/values 容量（同扩同缩） */
    ValueType key_type;      /* 键类型（VAL_INT/VAL_STRING/...） */
    ValueType value_type;    /* 值类型 */
    uint8_t stack_alloc;     /* 0=堆分配，1=编译通道栈分配 */
    volatile int write_flag; /* 并发结构修改检测，与 ValueMap/TypedArray 同语义 */
    int32_t free_head;       /* 空闲槽位链头（-1=无空闲），删除时挂链，新增时优先复用 */
    int free_count;          /* 空闲槽位数（free_count > 60% x cap 时触发 compact） */
} TypedMap;

/* ==================== API ==================== */

/* 创建空 TypedMap（cap=0 用默认 TMAP_INIT_CAP） */
TypedMap* lumyr_tmap_new(ValueType key_type, ValueType value_type, int cap);

/* 从动态 ValueMap 转换为 TypedMap（按键/值类型提取并存储） */
TypedMap* lumyr_tmap_from_value_map(struct ValueMap* m, ValueType key_type, ValueType value_type);

/* 插入/更新键值对（从 Value 提取并写入类型化槽位） */
void lumyr_tmap_set(TypedMap* m, Value key, Value val);

/* 查找键 -> 值（缺键返回 val_none()） */
Value lumyr_tmap_get(TypedMap* m, Value key);

/* 键是否存在 */
int lumyr_tmap_has(TypedMap* m, Value key);

/* 删除键 */
void lumyr_tmap_del(TypedMap* m, Value key);

/* 所有键 -> ValueArray */
Value lumyr_tmap_keys(TypedMap* m);

/* 所有值 -> ValueArray */
Value lumyr_tmap_values(TypedMap* m);

/* 长度 */
int lumyr_tmap_len(TypedMap* m);

/* TypedMap → 动态 ValueMap（键值装箱为 Value，供 toMap/json/copy 复用） */
Value lumyr_tmap_to_value_map(TypedMap* m);

/* 释放（仅栈分配实例用；堆分配由 GC sweep 回收） */
void lumyr_tmap_free(TypedMap* m);

#endif /* LUMYR_TYPED_MAPS_H */
