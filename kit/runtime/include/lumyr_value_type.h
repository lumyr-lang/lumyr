#ifndef LUMYR_VALUE_TYPE_H
#define LUMYR_VALUE_TYPE_H

#include <pthread.h>
#include <stdint.h>

typedef struct Value Value;
typedef struct EvalCtx EvalCtx;
typedef struct MapEntry MapEntry;
typedef struct ValueMap ValueMap;
// ✅ 新增前置声明：FuncEntry 参数需要 StackFrame*，此时还没完整定义 StackFrame
typedef struct StackFrame StackFrame;
typedef struct FFIFunc FFIFunc;

// ✅ 修改：函数入口回调类型，新增 StackFrame* frame 参数
// 函数原型类型，RuntimeFunc.entry 使用 FuncEntry*
typedef Value (FuncEntry)(int arg_cnt, const Value* args, EvalCtx* ctx, StackFrame* frame);

// 运行时函数对象：所有运行时需要的信息全部在这里
typedef struct RuntimeFunc {
    FuncEntry* entry;        // 执行入口：解释器就写解释器入口；编译器替换成机器码入口
    int param_count;         // 普通参数个数
    int has_variadic;        // 新增：是否有 ...rest 可变参数
    Value* captures;         // 闭包捕获值
    int capture_count;
} RuntimeFunc;

// 强制转换类型，给 new_cast_node 使用
typedef enum {
    CAST_INT,
    CAST_DOUBLE,
    CAST_STRING,
    CAST_BOOL,
    CAST_ASCII,
    CAST_CHAR,
    CAST_BYTE,     // (byte)x 强转：C 风格截断为 8 位无符号整数
    // 固定宽度整数（运行时统一 long long 存储，强转时 C 风格截断）
    CAST_INT8,
    CAST_INT16,
    CAST_INT32,
    CAST_INT64,
    CAST_UINT8,
    CAST_UINT16,
    CAST_UINT32,
    CAST_UINT64,
    CAST_LONG,       // long：平台相关，lm 统一 64 位
    CAST_LONGLONG,   // long long：64 位（= int 默认）
    CAST_FLOAT,      // float：32 位单精度，运行时 double 存储，强转时截断精度
    // 补充 C 标准类型（与 FFI 类型对齐，运行时统一 long long/double 存储）
    CAST_ULONG,      // unsigned long
    CAST_UCHAR,      // unsigned char（= byte，但语义明确）
    CAST_SHORT,      // short（16 位有符号）
    CAST_USHORT,     // unsigned short（16 位无符号）
    CAST_SIZE_T,     // size_t（无符号整数，平台相关）
    CAST_SSIZE_T,    // ssize_t（有符号整数，平台相关）
    CAST_VOID,       // void（无类型/无返回值）
    CAST_LONG_DOUBLE,// long double（扩展精度浮点）
    CAST_PTR,        // 指针/句柄（用 int 存储指针值）
} CastKind;

// 值类型：语言支持的数据类型（包含原 FFI 的所有 C 类型，从 100 开始编号）
typedef enum {
    // 运行时值类型（0-99）
    VAL_NONE = 0,
    VAL_INT,
    VAL_DOUBLE,
    VAL_BOOL,
    VAL_CHAR,
    VAL_STRING,
    VAL_FUNC,
    VAL_ARRAY,
    VAL_MAP,
    VAL_ERROR,     // 错误对象：type/message/stack（throw 与运行时错误统一）
    VAL_BYTE,      // 8 位无符号整数（0-255，C 风格截断；算术/比较按数值类型处理）
    VAL_GENERATOR,  // 生成器对象：保存冻结的执行状态，next() 恢复执行
    VAL_STRUCT_PTR,  // C结构体指针：零拷贝传递，直接存储void*，配合__structname__标识类型
    VAL_CLASS_PTR,    // class实例指针：零拷贝传递，直接存储void*，配合vtable标识类型，与struct区分
    VAL_TYPED_ARRAY,  // 类型化数组：统一处理 IntArray、DoubleArray、StringArray 等，通过 elem_type 区分元素类型

    // C 类型（原 FFI 类型，从 100 开始编号，用于类型化数组和 FFI）
    VAL_VOID = 100,
    VAL_INT8,         // int8_t / signed char
    VAL_INT16,        // int16_t / short
    VAL_INT32,        // int32_t / int
    VAL_INT64,        // int64_t / long long
    VAL_LONG_LONG,    // long long：64位有符号整数（与C语言long long对齐）
    VAL_LONG,         // long
    VAL_UINT8,        // uint8_t / unsigned char / byte
    VAL_UINT16,       // uint16_t / unsigned short
    VAL_UINT32,       // uint32_t / unsigned int / uint
    VAL_UINT64,       // uint64_t / unsigned long long
    VAL_ULONG,        // unsigned long
    VAL_UCHAR,        // unsigned char
    VAL_SHORT,        // short（16位有符号，与 int16 隔离）
    VAL_USHORT,       // unsigned short（16位无符号，与 uint16 隔离）
    VAL_SIZE_T,       // size_t
    VAL_SSIZE_T,      // ssize_t / ptrdiff_t
    VAL_FLOAT,        // float（单精度）
    VAL_LONG_DOUBLE,  // long double（扩展精度）
    VAL_PTR,          // void* / 任意指针 / 句柄
    VAL_CALLBACK      // 回调函数（函数指针）
} ValueType;

// 数组运行时对象，VAL_ARRAY 使用（原地修改语义，cap 预分配容量）
// GC 管理：ValueArray* 本身由 gc_alloc(vtype=VAL_ARRAY) 分配（堆指针，引用语义）；
//          items 缓冲区也由 gc_alloc(vtype=VAL_ARRAY) 管理，扩容用 gc_realloc。
// stack_alloc：0=堆分配（默认，有 GCObject 头），1=编译通道栈分配（无 GCObject 头，GC 标记时跳过自身）
// items_stack_alloc：0=items 堆分配（默认），1=编译通道栈分配（无 GCObject 头，GC 标记时跳过 items 自身但仍递归标记 items[i]）
// 注意：ValueArray 只用于存储 Value 类型数组；类型化数组（IntArray、DoubleArray 等）请使用 lumyr_typed_arrays.h 中的专门结构体
typedef struct {
    Value* items;              // Value 类型数组
    int len;
    int cap;  // 预分配容量（>= len），add 时按需 2x 扩容
    uint8_t stack_alloc;        // 0=堆分配，1=编译通道栈分配
    uint8_t items_stack_alloc;  // 0=items堆分配，1=items栈分配（仅 stack_alloc=1 时有效）
} ValueArray;

// 类型化数组运行时对象，VAL_TYPED_ARRAY 使用（统一处理所有类型化数组，通过 elem_type 区分元素类型）
// 与 ValueArray 的区别：ValueArray 存储 Value 类型元素，TypedArray 存储精确类型元素（int、double 等）
// GC 管理：TypedArray* 本身由 gc_alloc(vtype=VAL_TYPED_ARRAY) 分配；items 缓冲区也由 gc_alloc 管理
typedef struct {
    void* items;           // 指向具体类型的数组（int*、double*、char** 等）
    int len;
    int cap;               // 预分配容量（>= len），add 时按需 2x 扩容
    ValueType elem_type;   // 元素类型（VAL_INT、VAL_DOUBLE、VAL_STRING 等）
    uint8_t stack_alloc;   // 0=堆分配，1=编译通道栈分配
} TypedArray;

// 错误对象，VAL_ERROR 使用（type 为错误类别，message 为消息，stack 为调用栈回溯）
// GC 管理：ValueError 内联在 Value 里；type/message/stack 字符串由 gc_alloc(vtype=VAL_STRING) 管理。
typedef struct {
    char* type;      // 错误类型名（"RuntimeError" / throw 自定义）
    char* message;   // 错误消息
    char* stack;     // 调用栈回溯文本（可空）
} ValueError;

// VAL_FUNC：直接持有独立运行时函数堆对象；FFI 外部函数用 ffi_func 字段
typedef struct {
    RuntimeFunc* func_obj;
    FFIFunc* ffi_func;    /* FFI 外部函数对象（is_ffi=1 时有效） */
    int is_ffi;            /* 1=FFI 外部函数，0=普通函数 */
} ValueFunc;

// SSO 最大内联字符数（按字节算，不含 \0）
#define LUMYR_SSO_MAX 22

// 运行时带标签的值（支持多类型，字符串支持 SSO 内联优化）
struct Value {
    ValueType type;          // 4字节，偏移0
    uint8_t str_inline;      // 1字节，偏移4：仅VAL_STRING时有效，1=内联，0=堆
    // 偏移5-7：3字节填充（编译器自动对齐）
    union {                  // 24字节，偏移8
        // ===== 有符号整数类型 =====
        int i;                // VAL_INT：32位有符号整数（与C语言int对齐）
        long long ll;         // VAL_LONG_LONG / VAL_INT64：64位有符号整数（与C语言long long对齐）
        int8_t i8;            // VAL_INT8：8位有符号整数
        int16_t i16;          // VAL_INT16 / VAL_SHORT：16位有符号整数
        int32_t i32;          // VAL_INT32：32位有符号整数
        long l;               // VAL_LONG：long类型
        // ===== 无符号整数类型 =====
        uint8_t u8;           // VAL_UINT8 / VAL_BYTE / VAL_UCHAR：8位无符号整数
        uint16_t u16;         // VAL_UINT16 / VAL_USHORT：16位无符号整数
        uint32_t u32;         // VAL_UINT32：32位无符号整数
        uint64_t u64;         // VAL_UINT64：64位无符号整数
        unsigned long ul;     // VAL_ULONG：unsigned long类型
        // ===== 浮点类型 =====
        double d;             // VAL_DOUBLE：双精度浮点数
        float f;              // VAL_FLOAT：单精度浮点数
        long double ld;       // VAL_LONG_DOUBLE：扩展精度浮点数
        // ===== 其他基础类型 =====
        _Bool b;              // VAL_BOOL：布尔类型
        char c;               // VAL_CHAR：字符类型
        size_t st;            // VAL_SIZE_T：size_t类型
        ssize_t sst;          // VAL_SSIZE_T：ssize_t类型
        char* s;             // VAL_STRING：堆上字符串（str_inline=0时有效）
        struct {             // SSO内联字符串（str_inline=1时有效）
            uint8_t len;     // 字符串长度（不含\0），最大22
            char data[23];   // 内联数据，含\0，最多存22字符
        } sso;
        ValueFunc func;        // VAL_FUNC
        ValueArray* array;     // VAL_ARRAY（堆指针，引用语义，与 ValueMap* 一致）
        ValueMap* map;         // VAL_MAP：堆上共享对象（原地改语义与数组 items 一致）
        ValueError err;        // VAL_ERROR：错误对象（type/message/stack，堆上字符串）
        void* generator;       // VAL_GENERATOR：GeneratorObject* 指针（vm.c 中定义）
        void* struct_ptr;      // VAL_STRUCT_PTR：C结构体指针（零拷贝传递，类型由外部标识）
        TypedArray* typed_array;  // VAL_TYPED_ARRAY：类型化数组（统一处理，通过 elem_type 区分元素类型）
    } v;
};

// 哈希表条目（同时作为链表节点和红黑树节点）
struct MapEntry {
    Value key;
    Value value;
    uint32_t hash;
    struct MapEntry* next;    // 链表指针
    struct MapEntry* left;    // 红黑树左子
    struct MapEntry* right;   // 红黑树右子
    struct MapEntry* parent;  // 红黑树父
    int color;                // 红黑树颜色：0=红, 1=黑
};

// 字典运行时对象，VAL_MAP 使用（哈希表 + 红黑树自适应，Java HashMap 策略）
// GC 管理：ValueMap* 本身由 gc_alloc(vtype=VAL_MAP) 管理；
//          buckets/tree 数组及 MapEntry 节点也由 gc_alloc(vtype=VAL_MAP) 管理（独立 GC 对象，各自 sweep）。
// stack_alloc：0=堆分配（默认，有 GCObject 头），1=编译通道栈分配（无 GCObject 头，GC 标记时跳过自身但仍标记 buckets/entries）
struct ValueMap {
    MapEntry** buckets;   // 桶数组（每桶是链表或红黑树根）
    unsigned char* tree;  // 桶类型标记：0=链表, 1=红黑树
    int len;              // 元素数
    int cap;              // 桶数（2的幂）
    uint8_t stack_alloc;  // 0=堆分配，1=编译通道栈分配
};

// 解释器执行上下文：只负责控制流 break/continue/return，不存局部变量
struct EvalCtx {
    int hit_break;
    int hit_continue;
    int hit_return;
    Value ret_val;
};

// 新增，不修改原有EvalCtx，栈帧独立
// 多线程安全：main/全局帧 shared=1，get/set/bind 走 rwlock（主线程扩容 realloc 不移动共享帧内存
// 时，其他线程读取同帧会 use-after-free，故共享帧所有访问加锁）；函数帧为线程私有 shared=0 不加锁。
typedef struct StackFrame {
    char** names;    // 动态：按需扩容，无硬上限
    Value* vals;
    /* 类型化变量存储（与 names/vals 平行数组）
       声明为对应类型的变量同时存储在 vals（作为 Value）和类型化数组（作为原始值）
       OPC_LOAD_*_VAR 直接从类型化数组读取，实现零提取、零类型检查
       覆盖所有 C 标准类型，与 CastKind 枚举对齐 */
    /* 基础整数类型 */
    int* int_vals;                 /* int 类型变量 */
    long long* longlong_vals;      /* long long 类型变量（64位有符号） */
    long* long_vals;               /* long 类型变量（平台相关） */
    short* short_vals;             /* short 类型变量（16位有符号） */
    /* 固定宽度有符号整数 */
    int8_t* int8_vals;             /* int8_t / signed char */
    int16_t* int16_vals;           /* int16_t / short */
    int32_t* int32_vals;           /* int32_t / int */
    int64_t* int64_vals;           /* int64_t / long long */
    /* 固定宽度无符号整数 */
    uint8_t* uint8_vals;           /* uint8_t / unsigned char / byte */
    uint16_t* uint16_vals;         /* uint16_t / unsigned short */
    uint32_t* uint32_vals;         /* uint32_t / unsigned int */
    uint64_t* uint64_vals;         /* uint64_t / unsigned long long */
    /* 其他无符号整数 */
    unsigned char* uchar_vals;     /* unsigned char */
    unsigned short* ushort_vals;   /* unsigned short */
    unsigned long* ulong_vals;     /* unsigned long */
    size_t* size_t_vals;           /* size_t（无符号整数，平台相关） */
    ssize_t* ssize_t_vals;         /* ssize_t（有符号整数，平台相关） */
    /* 浮点类型 */
    float* float_vals;              /* float（32位单精度） */
    double* double_vals;            /* double（64位双精度） */
    long double* longdouble_vals;   /* long double（扩展精度） */
    /* 其他基础类型 */
    _Bool* bool_vals;               /* bool 类型变量 */
    char* char_vals;                /* char 类型变量 */
    unsigned char* byte_vals;       /* byte 类型变量（uint8_t，与 uint8_vals 同义） */
    char** string_vals;             /* string 类型变量（char* 指针，引用语义） */
    void** ptr_vals;                /* 指针/句柄类型变量（void* 指针） */
    int cnt;
    int cap;
    struct StackFrame* parent;
    pthread_rwlock_t rw;   // 共享帧（全局帧）读写锁；私有帧不使用
    _Bool shared;          // 1 = 全局共享帧（main 顶层帧），多线程可见
    /* 闭包单元（cell）表：被内层 lambda 捕获的局部变量从普通槽位"装箱"到堆上 Value*。
     * names[i] ↔ cells[i] 平行数组。访问变量时先查 cell 表（命中则解引用 *cells[i]），
     * 未命中再查普通槽位。cell 指针本身由闭包 RuntimeFunc 持有，帧销毁不释放 cell。 */
    char** cell_names;
    Value** cells;
    int cell_cnt;
    int cell_cap;
    int* type_tags;   /* 变量类型标记（CastKind 枚举，-1 表示无精确类型），与 names/vals 平行数组 */
} StackFrame;

#include <string.h>

// 获取字符串的C指针（内联返回sso.data，堆返回v.s），非字符串返回NULL
static inline const char* lumyr_str_cstr(const Value* v) {
    if (v->type != VAL_STRING) return NULL;
    return v->str_inline ? v->v.sso.data : v->v.s;
}

// 获取字符串长度（内联用sso.len，堆用strlen）
static inline int lumyr_str_len(const Value* v) {
    if (v->type != VAL_STRING) return 0;
    return v->str_inline ? (int)v->v.sso.len : (int)(v->v.s ? strlen(v->v.s) : 0);
}

#endif //LUMYR_VALUE_TYPE_H
