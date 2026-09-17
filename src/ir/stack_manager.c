/**
 * stack_manager.c - 统一栈管理模块实现
 *
 * 统一管理所有类型的专用栈（Value栈、int栈、double栈等），
 * 避免代码中到处都是自己计算栈大小和操作栈的问题。
 */

#include "stack_manager.h"
#include "lumyr_value_type.h"  /* Value类型定义 */

/* 栈信息表 */
static const StackInfo stack_info_table[STACK_TYPE_COUNT] = {
    [STACK_VALUE] = {
        .type = STACK_VALUE,
        .name = "__stk",
        .c_type = "Value",
        .elem_size = sizeof(Value),
        .has_dedicated_stack = 1,
    },
    [STACK_INT] = {
        .type = STACK_INT,
        .name = "__int_stack",
        .c_type = "int",
        .elem_size = sizeof(int),
        .has_dedicated_stack = 1,
    },
    [STACK_DOUBLE] = {
        .type = STACK_DOUBLE,
        .name = "__double_stack",
        .c_type = "double",
        .elem_size = sizeof(double),
        .has_dedicated_stack = 1,
    },
    [STACK_FLOAT] = {
        .type = STACK_FLOAT,
        .name = "__float_stack",
        .c_type = "float",
        .elem_size = sizeof(float),
        .has_dedicated_stack = 1,
    },
    [STACK_UINT] = {
        .type = STACK_UINT,
        .name = "__uint_stack",
        .c_type = "unsigned int",
        .elem_size = sizeof(unsigned int),
        .has_dedicated_stack = 1,
    },
    [STACK_BOOL] = {
        .type = STACK_BOOL,
        .name = "__bool_stack",
        .c_type = "_Bool",
        .elem_size = sizeof(_Bool),
        .has_dedicated_stack = 1,
    },
    [STACK_CHAR] = {
        .type = STACK_CHAR,
        .name = "__char_stack",
        .c_type = "char",
        .elem_size = sizeof(char),
        .has_dedicated_stack = 1,
    },
    [STACK_BYTE] = {
        .type = STACK_BYTE,
        .name = "__byte_stack",
        .c_type = "unsigned char",
        .elem_size = sizeof(unsigned char),
        .has_dedicated_stack = 1,
    },
    [STACK_INT8] = {
        .type = STACK_INT8,
        .name = "__int8_stack",
        .c_type = "int8_t",
        .elem_size = sizeof(int8_t),
        .has_dedicated_stack = 1,
    },
    [STACK_INT16] = {
        .type = STACK_INT16,
        .name = "__int16_stack",
        .c_type = "int16_t",
        .elem_size = sizeof(int16_t),
        .has_dedicated_stack = 1,
    },
    [STACK_SHORT] = {
        .type = STACK_SHORT,
        .name = "__short_stack",
        .c_type = "short",
        .elem_size = sizeof(short),
        .has_dedicated_stack = 1,
    },
    [STACK_INT32] = {
        .type = STACK_INT32,
        .name = "__int32_stack",
        .c_type = "int32_t",
        .elem_size = sizeof(int32_t),
        .has_dedicated_stack = 1,
    },
    [STACK_INT64] = {
        .type = STACK_INT64,
        .name = "__int64_stack",
        .c_type = "int64_t",
        .elem_size = sizeof(int64_t),
        .has_dedicated_stack = 1,
    },
    [STACK_UINT8] = {
        .type = STACK_UINT8,
        .name = "__uint8_stack",
        .c_type = "uint8_t",
        .elem_size = sizeof(uint8_t),
        .has_dedicated_stack = 1,
    },
    [STACK_UINT16] = {
        .type = STACK_UINT16,
        .name = "__uint16_stack",
        .c_type = "uint16_t",
        .elem_size = sizeof(uint16_t),
        .has_dedicated_stack = 1,
    },
    [STACK_UINT32] = {
        .type = STACK_UINT32,
        .name = "__uint32_stack",
        .c_type = "uint32_t",
        .elem_size = sizeof(uint32_t),
        .has_dedicated_stack = 1,
    },
    [STACK_UINT64] = {
        .type = STACK_UINT64,
        .name = "__uint64_stack",
        .c_type = "uint64_t",
        .elem_size = sizeof(uint64_t),
        .has_dedicated_stack = 1,
    },
    [STACK_LONG] = {
        .type = STACK_LONG,
        .name = "__long_stack",
        .c_type = "long",
        .elem_size = sizeof(long),
        .has_dedicated_stack = 1,
    },
    [STACK_ULONG] = {
        .type = STACK_ULONG,
        .name = "__ulong_stack",
        .c_type = "unsigned long",
        .elem_size = sizeof(unsigned long),
        .has_dedicated_stack = 1,
    },
    [STACK_SIZE_T] = {
        .type = STACK_SIZE_T,
        .name = "__size_t_stack",
        .c_type = "size_t",
        .elem_size = sizeof(size_t),
        .has_dedicated_stack = 1,
    },
    [STACK_SSIZE_T] = {
        .type = STACK_SSIZE_T,
        .name = "__ssize_t_stack",
        .c_type = "ssize_t",
        .elem_size = sizeof(ssize_t),
        .has_dedicated_stack = 1,
    },
    [STACK_LONG_DOUBLE] = {
        .type = STACK_LONG_DOUBLE,
        .name = "__long_double_stack",
        .c_type = "long double",
        .elem_size = sizeof(long double),
        .has_dedicated_stack = 1,
    },
    [STACK_LONG_LONG] = {
        .type = STACK_LONG_LONG,
        .name = "__long_long_stack",
        .c_type = "long long",
        .elem_size = sizeof(long long),
        .has_dedicated_stack = 1,
    },
};

/* 获取栈信息 */
const StackInfo* stack_get_info(StackType type) {
    if (type < 0 || type >= STACK_TYPE_COUNT) {
        return NULL;
    }
    return &stack_info_table[type];
}

/* 获取栈名称 */
const char* stack_get_name(StackType type) {
    const StackInfo* info = stack_get_info(type);
    return info ? info->name : NULL;
}

/* 获取C类型 */
const char* stack_get_c_type(StackType type) {
    const StackInfo* info = stack_get_info(type);
    return info ? info->c_type : NULL;
}

/* 获取元素大小 */
size_t stack_get_elem_size(StackType type) {
    const StackInfo* info = stack_get_info(type);
    return info ? info->elem_size : 0;
}

/* 是否有专用栈 */
int stack_has_dedicated(StackType type) {
    const StackInfo* info = stack_get_info(type);
    return info ? info->has_dedicated_stack : 0;
}

/*
 * CC模式：生成所有栈的声明
 * 所有类型的专用栈大小一致（都用max_depth + STACK_SIZE_MARGIN），
 * 避免栈溢出。
 */
void stack_emit_declarations(FILE* out, int max_depth) {
    int stack_size = max_depth + STACK_SIZE_MARGIN;

    fprintf(out, "    /* 统一栈管理：所有类型的专用栈大小一致，避免栈溢出 */\n");

    for (int i = 0; i < STACK_TYPE_COUNT; i++) {
        const StackInfo* info = &stack_info_table[i];
        if (info->has_dedicated_stack) {
            fprintf(out, "    %s %s[%d];\n", info->c_type, info->name, stack_size);
            fprintf(out, "    int %s_sp = 0;\n", info->name);
        }
    }
}

/*
 * CC模式：生成单个栈的压入操作
 */
void stack_emit_push(FILE* out, StackType type, const char* value_expr) {
    const StackInfo* info = stack_get_info(type);
    if (!info || !info->has_dedicated_stack) {
        return;
    }
    fprintf(out, "    %s[%s_sp++] = %s;\n", info->name, info->name, value_expr);
}

/*
 * CC模式：生成单个栈的弹出操作
 */
void stack_emit_pop(FILE* out, StackType type, const char* dest_var) {
    const StackInfo* info = stack_get_info(type);
    if (!info || !info->has_dedicated_stack) {
        return;
    }
    fprintf(out, "    { %s __v = %s[--%s_sp]; %s = __v; }\n",
            info->c_type, info->name, info->name, dest_var);
}

/*
 * VM模式：初始化所有栈
 * 返回0表示成功，-1表示失败
 */
int stack_vm_init(VMStackManager* mgr, int max_depth) {
    if (!mgr) {
        return -1;
    }

    mgr->max_depth = max_depth + STACK_SIZE_MARGIN;

    for (int i = 0; i < STACK_TYPE_COUNT; i++) {
        const StackInfo* info = &stack_info_table[i];
        if (info->has_dedicated_stack) {
            mgr->stacks[i] = malloc(info->elem_size * mgr->max_depth);
            if (!mgr->stacks[i]) {
                /* 分配失败，清理已分配的栈 */
                for (int j = 0; j < i; j++) {
                    if (mgr->stacks[j]) {
                        free(mgr->stacks[j]);
                        mgr->stacks[j] = NULL;
                    }
                }
                return -1;
            }
            mgr->sp[i] = 0;
        } else {
            mgr->stacks[i] = NULL;
            mgr->sp[i] = 0;
        }
    }

    return 0;
}

/*
 * VM模式：销毁所有栈
 */
void stack_vm_destroy(VMStackManager* mgr) {
    if (!mgr) {
        return;
    }

    for (int i = 0; i < STACK_TYPE_COUNT; i++) {
        if (mgr->stacks[i]) {
            free(mgr->stacks[i]);
            mgr->stacks[i] = NULL;
        }
        mgr->sp[i] = 0;
    }
}

/*
 * VM模式：压入值到指定栈
 * 返回0表示成功，-1表示栈溢出
 */
int stack_vm_push(VMStackManager* mgr, StackType type, const void* value) {
    if (!mgr || !value) {
        return -1;
    }

    const StackInfo* info = stack_get_info(type);
    if (!info || !info->has_dedicated_stack) {
        return -1;
    }

    if (mgr->sp[type] >= mgr->max_depth) {
        return -1;  /* 栈溢出 */
    }

    memcpy((char*)mgr->stacks[type] + (mgr->sp[type] * info->elem_size),
           value, info->elem_size);
    mgr->sp[type]++;

    return 0;
}

/*
 * VM模式：从指定栈弹出值
 * 返回0表示成功，-1表示栈下溢
 */
int stack_vm_pop(VMStackManager* mgr, StackType type, void* dest) {
    if (!mgr || !dest) {
        return -1;
    }

    const StackInfo* info = stack_get_info(type);
    if (!info || !info->has_dedicated_stack) {
        return -1;
    }

    if (mgr->sp[type] <= 0) {
        return -1;  /* 栈下溢 */
    }

    mgr->sp[type]--;
    memcpy(dest,
           (char*)mgr->stacks[type] + (mgr->sp[type] * info->elem_size),
           info->elem_size);

    return 0;
}

/*
 * VM模式：获取栈顶元素（不弹出）
 * 返回0表示成功，-1表示栈为空
 */
int stack_vm_peek(VMStackManager* mgr, StackType type, void* dest) {
    if (!mgr || !dest) {
        return -1;
    }

    const StackInfo* info = stack_get_info(type);
    if (!info || !info->has_dedicated_stack) {
        return -1;
    }

    if (mgr->sp[type] <= 0) {
        return -1;  /* 栈为空 */
    }

    memcpy(dest,
           (char*)mgr->stacks[type] + ((mgr->sp[type] - 1) * info->elem_size),
           info->elem_size);

    return 0;
}

/*
 * 获取栈当前深度
 */
int stack_vm_get_depth(VMStackManager* mgr, StackType type) {
    if (!mgr) {
        return -1;
    }
    return mgr->sp[type];
}

/*
 * 检查栈是否为空
 */
int stack_vm_is_empty(VMStackManager* mgr, StackType type) {
    if (!mgr) {
        return 1;
    }
    return mgr->sp[type] <= 0;
}

/*
 * 检查栈是否已满
 */
int stack_vm_is_full(VMStackManager* mgr, StackType type) {
    if (!mgr) {
        return 1;
    }
    return mgr->sp[type] >= mgr->max_depth;
}

/* ========== Thread-Local 全局栈管理（用于VM模式） ========== */

/* 全局Thread-Local栈管理器 */
_Thread_local VMStackManager* g_stack_mgr = NULL;

/* 每个栈的容量（用于扩容，因为VMStackManager中没有cap字段） */
_Thread_local int g_stack_caps[STACK_TYPE_COUNT] = {0};

#define STACK_GLOBAL_INIT_CAP 64

/*
 * 初始化全局Thread-Local栈管理器
 * 返回0表示成功，-1表示失败
 */
int stack_global_init(int max_depth) {
    if (g_stack_mgr) {
        return 0;  /* 已经初始化 */
    }

    g_stack_mgr = (VMStackManager*)malloc(sizeof(VMStackManager));
    if (!g_stack_mgr) {
        return -1;
    }

    memset(g_stack_mgr, 0, sizeof(VMStackManager));
    g_stack_mgr->max_depth = max_depth > 0 ? max_depth : STACK_GLOBAL_INIT_CAP;

    /* 初始化所有栈 */
    for (int i = 0; i < STACK_TYPE_COUNT; i++) {
        const StackInfo* info = &stack_info_table[i];
        if (info->has_dedicated_stack) {
            g_stack_mgr->stacks[i] = malloc(info->elem_size * g_stack_mgr->max_depth);
            if (!g_stack_mgr->stacks[i]) {
                /* 分配失败，清理已分配的栈 */
                for (int j = 0; j < i; j++) {
                    if (g_stack_mgr->stacks[j]) {
                        free(g_stack_mgr->stacks[j]);
                        g_stack_mgr->stacks[j] = NULL;
                    }
                }
                free(g_stack_mgr);
                g_stack_mgr = NULL;
                return -1;
            }
            g_stack_mgr->sp[i] = 0;
            g_stack_caps[i] = g_stack_mgr->max_depth;
        } else {
            g_stack_mgr->stacks[i] = NULL;
            g_stack_mgr->sp[i] = 0;
            g_stack_caps[i] = 0;
        }
    }

    return 0;
}

/*
 * 销毁全局Thread-Local栈管理器
 */
void stack_global_destroy(void) {
    if (!g_stack_mgr) {
        return;
    }

    for (int i = 0; i < STACK_TYPE_COUNT; i++) {
        if (g_stack_mgr->stacks[i]) {
            free(g_stack_mgr->stacks[i]);
            g_stack_mgr->stacks[i] = NULL;
        }
        g_stack_mgr->sp[i] = 0;
        g_stack_caps[i] = 0;
    }

    free(g_stack_mgr);
    g_stack_mgr = NULL;
}

/*
 * 获取指定类型栈的容量（用于原来的宏定义）
 */
int stack_global_get_cap(StackType type) {
    if (type < 0 || type >= STACK_TYPE_COUNT) {
        return 0;
    }
    return g_stack_caps[type];
}

/*
 * 设置指定类型栈的容量（用于扩容）
 */
void stack_global_set_cap(StackType type, int cap) {
    if (type < 0 || type >= STACK_TYPE_COUNT) {
        return;
    }
    g_stack_caps[type] = cap;
}

/*
 * 扩容指定类型栈
 * 返回0表示成功，-1表示失败
 */
int stack_global_ensure(StackType type, int need) {
    if (!g_stack_mgr || type < 0 || type >= STACK_TYPE_COUNT) {
        return -1;
    }

    const StackInfo* info = &stack_info_table[type];
    if (!info->has_dedicated_stack) {
        return -1;
    }

    if (g_stack_mgr->sp[type] + need <= g_stack_caps[type]) {
        return 0;  /* 容量足够，不需要扩容 */
    }

    int nc = g_stack_caps[type] > 0 ? g_stack_caps[type] : STACK_GLOBAL_INIT_CAP;
    while (nc < g_stack_mgr->sp[type] + need) {
        nc *= 2;
    }

    void* ns = realloc(g_stack_mgr->stacks[type], (size_t)nc * info->elem_size);
    if (!ns) {
        return -1;  /* 扩容失败 */
    }

    g_stack_mgr->stacks[type] = ns;
    g_stack_caps[type] = nc;

    return 0;
}
