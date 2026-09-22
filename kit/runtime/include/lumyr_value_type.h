#ifndef LUMYR_VALUE_TYPE_H
#define LUMYR_VALUE_TYPE_H

#include <string.h>
#include <pthread.h>
#include <stdint.h>
#include <sys/types.h>   /* ssize_t（POSIX，macOS/Linux 需要） */

typedef struct Value Value;
typedef struct EvalCtx EvalCtx;
typedef struct MapEntry MapEntry;
typedef struct ValueMap ValueMap;
// 新增前置声明：FuncEntry 参数需要 StackFrame*，此时还没完整定义 StackFrame
typedef struct StackFrame StackFrame;
typedef struct FFIFunc FFIFunc;

// 修改：函数入口回调类型，新增 StackFrame* frame 参数
// 函数原型类型，RuntimeFunc.entry 使用 FuncEntry*
typedef Value (FuncEntry)(int arg_cnt, const Value* args, EvalCtx* ctx, StackFrame* frame);

// 运行时函数对象：所有运行时需要的信息全部在这里
typedef struct RuntimeFunc {
    FuncEntry* entry;        // 执行入口：解释器就写解释器入口；编译器替换成机器码入口
    int param_count;         // 普通参数个数
    int has_variadic;        // 新增：是否有 ...rest 可变参数
    Value* captures;         // 闭包捕获值
    int capture_count;
    char* name;              // 字节码VM用：函数名（用于动态调用时查找 BytecodeFunc）
    void* bound_self;        // 绑定方法：非 NULL 时为实例指针，动态调用自动注入 self 槽
} RuntimeFunc;

// 强制转换类型，给 new_cast_node 使用
// 与 ValueType 枚举一一对应，覆盖所有数据类型
typedef enum {
    /* 基础类型 */
    CAST_NONE,       // null
    CAST_INT,
    CAST_DOUBLE,
    CAST_STRING,
    CAST_BOOL,
    CAST_ASCII,
    CAST_CHAR,
    CAST_BYTE,       // (byte)x 强转：C 风格截断为 8 位无符号整数
    CAST_FUNC,       // 函数引用
    CAST_ARRAY,      // 数组
    CAST_MAP,        // 字典/哈希表
    CAST_ERROR,      // 错误对象
    CAST_GENERATOR,  // 生成器
    CAST_STRUCT_PTR, // C 结构体指针
    CAST_CLASS_PTR,  // class 实例指针
    CAST_TYPED_ARRAY,// 类型化数组
    /* 固定宽度整数（运行时统一 long long 存储，强转时 C 风格截断） */
    CAST_INT8,
    CAST_INT16,
    CAST_INT32,
    CAST_INT64,
    CAST_UINT8,
    CAST_UINT16,
    CAST_UINT32,
    CAST_UINT,       // unsigned int（平台相关，通常 32 位）
    CAST_UINT64,
    CAST_LONG,       // long：平台相关，lm 统一 64 位
    CAST_LONGLONG,   // long long：64 位（= int 默认）
    CAST_FLOAT,      // float：32 位单精度，运行时 double 存储，强转时截断精度
    /* 补充 C 标准类型（与 FFI 类型对齐，运行时统一 long long/double 存储） */
    CAST_ULONG,      // unsigned long
    CAST_UCHAR,      // unsigned char（= byte，但语义明确）
    CAST_SHORT,      // short（16 位有符号）
    CAST_USHORT,     // unsigned short（16 位无符号）
    CAST_SIZE_T,     // size_t（无符号整数，平台相关）
    CAST_SSIZE_T,    // ssize_t（有符号整数，平台相关）
    CAST_VOID,       // void（无类型/无返回值）
    CAST_LONG_DOUBLE,// long double（扩展精度浮点）
    CAST_PTR,        // 指针/句柄（用 int 存储指针值）
    CAST_CALLBACK,   // 回调函数（函数指针）
    /* 高精度类型（任意精度/十进制浮点） */
    CAST_BIGINT,     // bigint：任意精度整数（堆分配，PTR 栈存储）
    CAST_DECIMAL,    // decimal：高精度十进制浮点（堆分配，PTR 栈存储）
    CAST_BITDECIMAL, // bitdecimal：基于 GMP mpf_t 的高精度十进制浮点（堆分配，PTR 栈存储）
    /* 日期时间族（堆分配，PTR 栈存储） */
    CAST_DATE,       // date：日期对象（仅日期）
    CAST_DATETIME,   // datetime：日期+时间对象
    CAST_TIME,       // time：当天时间对象
    CAST_TIMEDELTA,  // timedelta：时间间隔对象
    /* 容器与数值扩展类型（堆分配，PTR 栈存储） */
    CAST_TUPLE,      // tuple：不可变固定长度异构序列
    CAST_SET,        // set：无序唯一元素集合
    CAST_BYTES,      // bytes：不可变字节串
    CAST_COMPLEX,    // complex：复数（real+imag double）
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
    VAL_DATE,         // date：日期对象（仅日期，不含时间，epoch 为当天 00:00 UTC 秒）
    VAL_DATETIME,     // datetime：日期+时间对象（epoch + nsec）
    VAL_TIME,         // time：当天时间对象（epoch 为当天秒数 [0,86400) + nsec）
    VAL_TIMEDELTA,    // timedelta：时间间隔对象（可负，epoch + nsec，规整同号）
    VAL_TUPLE,        // tuple：不可变固定长度异构序列（堆分配对象）
    VAL_SET,         // set：无序唯一元素集合（堆分配对象，基于哈希）
    VAL_BYTES,       // bytes：不可变字节串（堆分配对象）
    VAL_COMPLEX,     // complex：复数（堆分配对象，real+imag double）

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
    VAL_UINT32,       // uint32_t
    VAL_UINT,         // unsigned int / uint（与 uint32 同宽，type 隔离）
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
    VAL_CALLBACK,     // 回调函数
    VAL_BIGINT,       // bigint：任意精度整数（堆分配对象，PTR 栈存储指针）
    VAL_DECIMAL,      // decimal：高精度十进制浮点（堆分配对象，PTR 栈存储指针）
    VAL_BITDECIMAL    // bitdecimal：基于 GMP mpf_t 的高精度十进制浮点（堆分配对象，PTR 栈存储指针）
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
        long long ll;         // VAL_LONG_LONG：64位有符号整数（与C语言long long对齐）
        int64_t i64;          // VAL_INT64：64位有符号整数
        int8_t i8;            // VAL_INT8：8位有符号整数
        int16_t i16;          // VAL_INT16：16位有符号整数
        short sh;             // VAL_SHORT：16位有符号整数
        int32_t i32;          // VAL_INT32：32位有符号整数
        long l;               // VAL_LONG：long类型
        // ===== 无符号整数类型 =====
        uint8_t u8;           // VAL_UINT8：8位无符号整数
        unsigned char uc;     // VAL_UCHAR：8位无符号整数
        uint8_t by;           // VAL_BYTE：8位无符号整数
        uint16_t u16;         // VAL_UINT16：16位无符号整数
        unsigned short us;    // VAL_USHORT：16位无符号整数
        uint32_t u32;         // VAL_UINT32：32位无符号整数
        unsigned int ui;      // VAL_UINT：32位无符号整数
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
        void* bigint;           // VAL_BIGINT：BigInt* 指针（堆分配对象）
        void* decimal;          // VAL_DECIMAL：Decimal* 指针（堆分配对象）
        void* bitdecimal;       // VAL_BITDECIMAL：BitDecimal* 指针（堆分配对象，基于 GMP mpf_t）
        void* date_obj;         // VAL_DATE/VAL_DATETIME/VAL_TIME/VAL_TIMEDELTA：DateObj* 指针（堆分配对象）
        void* tuple_obj;        // VAL_TUPLE：TupleObj* 指针（堆分配对象）
        void* set_obj;          // VAL_SET：SetObj* 指针（堆分配对象）
        void* bytes_obj;        // VAL_BYTES：BytesObj* 指针（堆分配对象）
        void* complex_obj;      // VAL_COMPLEX：ComplexObj* 指针（堆分配对象）
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
    /* 宽槽变量存储（工业级合并风格）
       不再为每个 C 类型开独立数组，而是合并为 3 个统一槽：
       - int_slots：所有整数类型（bool/char/byte/int8~int64/uint8~uint64/
         long/ulong/size_t/ssize_t/short）统一存为 int64_t，读时按 type_tags 截断
       - flt_slots：所有浮点类型（float/double/long double）统一存为 double
       - ptr_slots：所有指针类型（char star / void star）统一存为 void*
       栈帧创建/扩容从 20+ 次 malloc 降为 3 次，高并发下内存与分配开销大幅降低 */
    int64_t* int_slots;    /* 整数槽（所有整数/布尔/字符类型） */
    double*  flt_slots;    /* 浮点槽（float/double/long double） */
    void**   ptr_slots;    /* 指针槽（char star / void star / 句柄） */
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
    uint8_t* type_tags;   /* 变量类型标记（CastKind 枚举，0=CAST_NONE 表示无精确类型），与 names/vals 平行数组 */
    /* ref 引用传递：refs[i] 非 NULL 表示槽 i 是调用方某存储的别名，
     * LOAD/STORE 时通过 refs[i]->ptr 转发并按 refs[i]->type 自动 box/unbox。 */
    struct RefDesc** refs;
} StackFrame;

/* ref 引用描述符：指向调用方栈帧的实际存储，附带类型以实现自动 box/unbox */
typedef struct RefDesc {
    void* ptr;    /* 调用方存储指针（Value或int64_t或double或void指针，由 type 决定） */
    int type;     /* 调用方变量的 CastKind（-1=CAST_NONE 表示 VALUE） */
} RefDesc;



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

// 日期时间对象，VAL_DATE/VAL_DATETIME/VAL_TIME/VAL_TIMEDELTA 使用（堆分配，GC 管理）
// 双模式存储：epoch 为主存储（用于 diff/add 等整数运算），字段为缓存（懒计算）
// 语义按 kind 解释：
//  - VAL_DATE: epoch = UTC 当天 00:00 的秒数（自 1970-01-01）；缓存 year/month/day/weekday/yearday
//  - VAL_DATETIME: epoch = 自 1970-01-01 00:00:00 UTC 的秒数；nsec 纳秒；缓存全部字段
//  - VAL_TIME: epoch = 当天 00:00 起的秒数 (0-86399)；nsec 纳秒；缓存 hour/min/sec
//  - VAL_TIMEDELTA: epoch = 总秒数（可负）；nsec 纳秒（与 epoch 同号规整到 |nsec|<1e9）
typedef struct DateObj {
    int64_t epoch;       // epoch 秒（按 kind 解释语义）
    int32_t nsec;        // 纳秒部分（0-999999999，timedelta 与 epoch 同号规整）
    int32_t year;        // 缓存：年（date/datetime）
    int32_t month;       // 缓存：月 1-12
    int32_t day;         // 缓存：日 1-31
    int32_t hour;        // 缓存：时 0-23（datetime/time）
    int32_t min;         // 缓存：分 0-59
    int32_t sec;         // 缓存：秒 0-59
    int32_t weekday;     // 缓存：周几 0=周日..6=周六（date/datetime）
    int32_t yearday;     // 缓存：年内序日 1-366
    int32_t tz_offset_min; // 时区偏移（分钟）：INT32_MIN=本地时区，0=UTC，480=UTC+8，-300=UTC-5
    uint8_t cached;      // 1=缓存字段已填充
    ValueType kind;      // VAL_DATE/VAL_DATETIME/VAL_TIME/VAL_TIMEDELTA
} DateObj;

// tuple 对象，VAL_TUPLE 使用（不可变固定长度异构序列，堆分配，GC 管理）
// GC 管理：TupleObj* 由 gc_alloc(vtype=VAL_TUPLE) 分配；
//          items 缓冲区也由 gc_alloc(vtype=VAL_TUPLE) 管理，gc_mark 递归标记 items[i]
// stack_alloc：0=堆分配（默认），1=编译通道栈分配（无 GCObject 头，GC 标记跳过自身但仍递归标记 items[i]）
typedef struct {
    Value* items;        // 元素数组（不可变，构造后只读）
    int len;             // 元素个数
    uint8_t stack_alloc; // 0=堆分配，1=编译通道栈分配
} TupleObj;

// set 对象，VAL_SET 使用（无序唯一元素集合，堆分配，GC 管理）
// 复用 ValueMap 作底层存储（key=元素，value=null 标记存在性），复用已测试的哈希/扩容机制
// GC 管理：SetObj* 由 gc_alloc(vtype=VAL_SET) 分配；内部 ValueMap 由 gc_alloc(VAL_MAP) 管理，
//          gc_mark 递归标记 map 指针
typedef struct {
    ValueMap* map;        // 底层 map（key=元素，value=null）
    uint8_t stack_alloc; // 0=堆分配，1=编译通道栈分配
} SetObj;

// bytes 对象，VAL_BYTES 使用（不可变字节串，堆分配，GC 管理）
// GC 管理：BytesObj* 由 gc_alloc(vtype=VAL_BYTES) 分配；
//          data 缓冲区也由 gc_alloc(vtype=VAL_BYTES) 管理（无内部 Value 引用，无需递归标记）
typedef struct {
    uint8_t* data;        // 字节数据（不可变，构造后只读）
    int len;              // 字节长度
    uint8_t stack_alloc;  // 0=堆分配，1=编译通道栈分配
} BytesObj;

// complex 对象，VAL_COMPLEX 使用（复数 real+imag double，堆分配，GC 管理）
// 无内部 Value 引用，gc_mark 只标记自身
typedef struct {
    double real;     // 实部
    double imag;     // 虚部
} ComplexObj;

#endif //LUMYR_VALUE_TYPE_H
