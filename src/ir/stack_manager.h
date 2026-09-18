/**
 * stack_manager.h - 统一栈管理模块
 *
 * 统一管理所有类型的专用栈（Value栈、int栈、double栈等），
 * 避免代码中到处都是自己计算栈大小和操作栈的问题。
 *
 * 设计原则：
 * 1. 所有类型的专用栈大小一致（都用同一个最大深度计算），避免栈溢出
 * 2. 统一的栈声明生成（CC模式）
 * 3. 统一的栈初始化和销毁（VM模式）
 * 4. 统一的栈压入/弹出操作接口
 * 5. 类型安全，不允许把value压入int栈（或反过来）
 */

#ifndef STACK_MANAGER_H
#define STACK_MANAGER_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 栈类型枚举
 * 合并设计：所有整数类型共享 INT64 栈（统一 int64_t 存储），
 * 所有浮点类型共享 DOUBLE 栈（统一 double 存储），
 * 指针类型共享 PTR 栈。保留旧枚举名作为别名，避免大规模改动。 */
typedef enum {
    STACK_VALUE = 0,      /* Value栈（通用栈） */
    STACK_INT = 1,        /* int栈 → 映射到 INT64 栈 */
    STACK_INT8 = 1,       /* int8栈 → 映射到 INT64 栈 */
    STACK_INT16 = 1,      /* int16栈 → 映射到 INT64 栈 */
    STACK_SHORT = 1,      /* short栈 → 映射到 INT64 栈 */
    STACK_INT32 = 1,      /* int32栈 → 映射到 INT64 栈 */
    STACK_INT64 = 1,      /* int64栈（核心整数栈） */
    STACK_UINT = 1,       /* uint栈 → 映射到 INT64 栈 */
    STACK_UINT8 = 1,      /* uint8栈 → 映射到 INT64 栈 */
    STACK_UINT16 = 1,     /* uint16栈 → 映射到 INT64 栈 */
    STACK_UINT32 = 1,     /* uint32栈 → 映射到 INT64 栈 */
    STACK_UINT64 = 1,     /* uint64栈 → 映射到 INT64 栈 */
    STACK_LONG = 1,       /* long栈 → 映射到 INT64 栈 */
    STACK_ULONG = 1,      /* ulong栈 → 映射到 INT64 栈 */
    STACK_SIZE_T = 1,     /* size_t栈 → 映射到 INT64 栈 */
    STACK_SSIZE_T = 1,    /* ssize_t栈 → 映射到 INT64 栈 */
    STACK_BOOL = 1,       /* bool栈 → 映射到 INT64 栈 */
    STACK_CHAR = 1,       /* char栈 → 映射到 INT64 栈 */
    STACK_BYTE = 1,       /* byte栈 → 映射到 INT64 栈 */
    STACK_LONG_LONG = 1,  /* long long栈 → 映射到 INT64 栈 */
    STACK_DOUBLE = 2,     /* double栈（核心浮点栈） */
    STACK_FLOAT = 2,      /* float栈 → 映射到 DOUBLE 栈 */
    STACK_LONG_DOUBLE = 2,/* long double栈 → 映射到 DOUBLE 栈 */
    STACK_PTR = 3,        /* 指针/字符串栈（核心指针栈） */
    STACK_TYPE_COUNT = 4  /* 实际核心栈数量 */
} StackType;

/* 栈信息结构体 */
typedef struct {
    StackType type;         /* 栈类型 */
    const char* name;       /* 栈名称（用于CC模式生成变量名） */
    const char* c_type;     /* C类型（用于CC模式生成声明） */
    size_t elem_size;       /* 元素大小（用于VM模式分配内存） */
    int has_dedicated_stack; /* 是否有专用栈（1=有，0=没有，用Value栈） */
} StackInfo;

/* 获取栈信息 */
const StackInfo* stack_get_info(StackType type);

/* 获取栈名称 */
const char* stack_get_name(StackType type);

/* 获取C类型 */
const char* stack_get_c_type(StackType type);

/* 获取元素大小 */
size_t stack_get_elem_size(StackType type);

/* 是否有专用栈 */
int stack_has_dedicated(StackType type);

/*
 * CC模式：生成所有栈的声明
 * 所有类型的专用栈大小一致（都用max_depth + STACK_SIZE_MARGIN），
 * 避免栈溢出。
 */
#define STACK_SIZE_MARGIN 16  /* 栈大小余量 */

void stack_emit_declarations(FILE* out, int max_depth);

/*
 * CC模式：生成单个栈的压入操作
 */
void stack_emit_push(FILE* out, StackType type, const char* value_expr);

/*
 * CC模式：生成单个栈的弹出操作
 */
void stack_emit_pop(FILE* out, StackType type, const char* dest_var);

/*
 * VM模式：初始化所有栈
 * 返回0表示成功，-1表示失败
 */
typedef struct {
    void* stacks[STACK_TYPE_COUNT];  /* 各类型栈的指针 */
    int sp[STACK_TYPE_COUNT];         /* 各类型栈的栈指针 */
    int max_depth;                     /* 栈最大深度 */
} VMStackManager;

int stack_vm_init(VMStackManager* mgr, int max_depth);

/*
 * VM模式：销毁所有栈
 */
void stack_vm_destroy(VMStackManager* mgr);

/*
 * VM模式：压入值到指定栈
 * 返回0表示成功，-1表示栈溢出
 */
int stack_vm_push(VMStackManager* mgr, StackType type, const void* value);

/*
 * VM模式：从指定栈弹出值
 * 返回0表示成功，-1表示栈下溢
 */
int stack_vm_pop(VMStackManager* mgr, StackType type, void* dest);

/*
 * VM模式：获取栈顶元素（不弹出）
 * 返回0表示成功，-1表示栈为空
 */
int stack_vm_peek(VMStackManager* mgr, StackType type, void* dest);

/*
 * 获取栈当前深度
 */
int stack_vm_get_depth(VMStackManager* mgr, StackType type);

/*
 * 检查栈是否为空
 */
int stack_vm_is_empty(VMStackManager* mgr, StackType type);

/*
 * 检查栈是否已满
 */
int stack_vm_is_full(VMStackManager* mgr, StackType type);

/* ========== Thread-Local 全局栈管理（用于VM模式） ========== */

/*
 * 全局Thread-Local栈管理器
 * 每个线程都有自己的栈管理器，避免多线程竞争
 */
extern _Thread_local VMStackManager* g_stack_mgr;

/*
 * 初始化全局Thread-Local栈管理器
 * 返回0表示成功，-1表示失败
 */
int stack_global_init(int max_depth);

/*
 * 销毁全局Thread-Local栈管理器
 */
void stack_global_destroy(void);

/*
 * 获取指定类型栈的指针（内联函数，减少函数调用开销）
 */
static inline void* stack_global_get_stack(StackType type) {
    if (!g_stack_mgr || type < 0 || type >= STACK_TYPE_COUNT) {
        return NULL;
    }
    return g_stack_mgr->stacks[type];
}

/*
 * 获取指定类型栈的栈指针（内联函数，减少函数调用开销）
 */
static inline int* stack_global_get_sp(StackType type) {
    if (!g_stack_mgr || type < 0 || type >= STACK_TYPE_COUNT) {
        return NULL;
    }
    return &g_stack_mgr->sp[type];
}

/*
 * 获取指定类型栈的容量（用于原来的宏定义）
 */
int stack_global_get_cap(StackType type);

/*
 * 设置指定类型栈的容量（用于扩容）
 */
void stack_global_set_cap(StackType type, int cap);

/*
 * 扩容指定类型栈
 * 返回0表示成功，-1表示失败
 */
int stack_global_ensure(StackType type, int need);

/* ========== 栈缓存优化（用于高频操作，避免重复访问全局变量） ========== */

/*
 * 栈缓存结构体：缓存指定类型栈的指针和栈空间
 * 用于函数内部高频操作，避免重复访问 g_stack_mgr 全局变量
 */
typedef struct {
    void* stack;    /* 栈空间指针 */
    int* sp;         /* 栈指针 */
    StackType type;  /* 栈类型 */
} StackCache;

/*
 * 初始化栈缓存：在函数开始时调用，一次性获取栈指针和栈空间
 * 注意：如果在函数执行过程中调用了 stack_global_ensure 导致栈扩容，
 *       需要重新初始化缓存（因为栈空间指针可能变了）
 */
#define STACK_CACHE_INIT(cache, stack_type) do { \
    (cache).stack = stack_global_get_stack(stack_type); \
    (cache).sp = stack_global_get_sp(stack_type); \
    (cache).type = stack_type; \
} while(0)

/*
 * 使用缓存进行压栈操作
 */
#define STACK_CACHE_PUSH(cache, val, c_type) do { \
    ((c_type*)(cache).stack)[(*(cache).sp)++] = (val); \
} while(0)

/*
 * 使用缓存进行弹栈操作
 */
#define STACK_CACHE_POP(cache, c_type) (((c_type*)(cache).stack)[--(*(cache).sp)])

/*
 * 使用缓存查看栈顶元素
 */
#define STACK_CACHE_PEEK(cache, c_type) (((c_type*)(cache).stack)[(*(cache).sp) - 1])

/*
 * 使用缓存查看栈顶第 idx 个元素
 */
#define STACK_CACHE_TOP(cache, idx, c_type) (((c_type*)(cache).stack)[(*(cache).sp) - 1 - (idx)])

/*
 * 确保栈有足够空间（如果扩容了，需要重新初始化缓存）
 * 返回1表示扩容了（需要重新初始化缓存），0表示没有扩容
 */
#define STACK_CACHE_ENSURE(cache, need) ( \
    (*(cache).sp) + (need) > stack_global_get_cap((cache).type) ? \
    (stack_global_ensure((cache).type, need), STACK_CACHE_INIT(cache, (cache).type), 1) : \
    0 \
)

/* ========== 统一栈操作宏定义（所有栈操作必须通过这些宏，禁止直接操作栈） ========== */

/*
 * 压栈操作宏定义
 * 自动处理自动扩容，禁止直接使用 g_stack_mgr->sp[type]++
 */

/* 压入 int64 到 INT64 栈 */
#define PUSH_INT64(val) stack_vm_push(g_stack_mgr, STACK_INT64, &(int64_t){(val)})

/* 压入 double 到 DOUBLE 栈 */
#define PUSH_DOUBLE(val) stack_vm_push(g_stack_mgr, STACK_DOUBLE, &(double){(val)})

/* 压入指针到 PTR 栈 */
#define PUSH_PTR(val) stack_vm_push(g_stack_mgr, STACK_PTR, &(void*){(val)})

/* 压入 Value 到 VALUE 栈 */
#define PUSH_VALUE(val) stack_vm_push(g_stack_mgr, STACK_VALUE, &(Value){(val)})

/*
 * 弹栈操作宏定义
 * 自动处理栈下溢检查
 */

/* 从 INT64 栈弹出 int64 */
#define POP_INT64() ({ int64_t _val; stack_vm_pop(g_stack_mgr, STACK_INT64, &_val); _val; })

/* 从 DOUBLE 栈弹出 double */
#define POP_DOUBLE() ({ double _val; stack_vm_pop(g_stack_mgr, STACK_DOUBLE, &_val); _val; })

/* 从 PTR 栈弹出指针 */
#define POP_PTR() ({ void* _val; stack_vm_pop(g_stack_mgr, STACK_PTR, &_val); _val; })

/* 从 VALUE 栈弹出 Value */
#define POP_VALUE() ({ Value _val; stack_vm_pop(g_stack_mgr, STACK_VALUE, &_val); _val; })

/*
 * 查看栈顶元素宏定义（不弹出）
 */

/* 查看 INT64 栈栈顶元素 */
#define PEEK_INT64() ({ int64_t _val; stack_vm_peek(g_stack_mgr, STACK_INT64, &_val); _val; })

/* 查看 DOUBLE 栈栈顶元素 */
#define PEEK_DOUBLE() ({ double _val; stack_vm_peek(g_stack_mgr, STACK_DOUBLE, &_val); _val; })

/* 查看 PTR 栈栈顶元素 */
#define PEEK_PTR() ({ void* _val; stack_vm_peek(g_stack_mgr, STACK_PTR, &_val); _val; })

/* 查看 VALUE 栈栈顶元素 */
#define PEEK_VALUE() ({ Value _val; stack_vm_peek(g_stack_mgr, STACK_VALUE, &_val); _val; })

#endif /* STACK_MANAGER_H */
