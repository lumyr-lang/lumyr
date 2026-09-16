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

/* 栈类型枚举 */
typedef enum {
    STACK_VALUE = 0,      /* Value栈（通用栈） */
    STACK_INT,             /* int栈 */
    STACK_DOUBLE,          /* double栈 */
    STACK_FLOAT,           /* float栈 */
    STACK_UINT,            /* uint栈 */
    STACK_BOOL,            /* bool栈 */
    STACK_CHAR,            /* char栈 */
    STACK_BYTE,            /* byte栈 */
    STACK_INT8,            /* int8栈 */
    STACK_INT16,           /* int16栈 */
    STACK_INT32,           /* int32栈 */
    STACK_INT64,           /* int64栈 */
    STACK_UINT8,           /* uint8栈 */
    STACK_UINT16,          /* uint16栈 */
    STACK_UINT32,          /* uint32栈 */
    STACK_UINT64,          /* uint64栈 */
    STACK_LONG,            /* long栈 */
    STACK_ULONG,           /* ulong栈 */
    STACK_SIZE_T,          /* size_t栈 */
    STACK_SSIZE_T,         /* ssize_t栈 */
    STACK_LONG_DOUBLE,     /* long double栈 */
    STACK_TYPE_COUNT       /* 栈类型数量 */
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

#endif /* STACK_MANAGER_H */
