/*
 * 统一符号表
 * 把 class、结构体、普通函数、变量都用一个红黑树统一管理
 * 键为 (file_name, scope, name)，用于生成代码的时候跨文件相同函数变量不重复
 */
#ifndef SYMBOL_TABLE_H
#define SYMBOL_TABLE_H

#include <stddef.h>

/* 符号类型 */
typedef enum {
    SYMBOL_VAR,        /* 变量 */
    SYMBOL_FUNC,       /* 普通函数 */
    SYMBOL_CLASS,      /* class 定义 */
    SYMBOL_STRUCT,     /* struct 定义 */
    SYMBOL_TYPE,       /* type 别名 */
    SYMBOL_INTERFACE,  /* interface 定义 */
    SYMBOL_METHOD,     /* class/struct 方法 */
    SYMBOL_FIELD,      /* class/struct 字段 */
    SYMBOL_UNKNOWN     /* 未知类型 */
} SymbolType;

/* 符号条目 */
typedef struct SymbolEntry {
    SymbolType type;           /* 符号类型 */
    char* file_name;           /* 所在文件名 */
    char* scope;               /* 作用域（class名/struct名/NULL表示全局） */
    char* name;                /* 符号名 */
    void* data;                /* 具体数据（BytecodeFunc指针、TypeDef指针、Value等） */
    char* c_name;              /* 生成的 C 名称（带文件名前缀，避免冲突） */
    struct SymbolEntry* next;  /* 用于哈希冲突链表（预留） */
} SymbolEntry;

/* 符号表（红黑树实现） */
typedef struct SymbolTable SymbolTable;

/* 创建符号表 */
SymbolTable* symbol_table_create(void);

/* 销毁符号表 */
void symbol_table_destroy(SymbolTable* table);

/* 添加符号 */
/* file_name: 文件名，scope: 作用域（NULL表示全局），name: 符号名 */
/* type: 符号类型，data: 具体数据 */
/* 返回 0 表示成功，-1 表示失败（已存在） */
int symbol_table_add(SymbolTable* table, const char* file_name, const char* scope,
                     const char* name, SymbolType type, void* data);

/* 查找符号 */
/* file_name: 文件名，scope: 作用域（NULL表示全局），name: 符号名 */
/* 返回符号条目，NULL表示未找到 */
SymbolEntry* symbol_table_find(SymbolTable* table, const char* file_name,
                                const char* scope, const char* name);

/* 按名字查找（不管 scope 和 file_name，用于兼容旧代码） */
/* 返回第一个匹配名字的符号条目，NULL表示未找到 */
SymbolEntry* symbol_table_find_by_name(SymbolTable* table, const char* name);

/* 按 scope 和 name 查找（不管 file_name） */
SymbolEntry* symbol_table_find_by_scope_name(SymbolTable* table, const char* scope,
                                               const char* name);

/* 删除符号 */
int symbol_table_remove(SymbolTable* table, const char* file_name, const char* scope,
                        const char* name);

/* 遍历所有符号 */
typedef void (*SymbolTableCallback)(const char* file_name, const char* scope,
                                     const char* name, SymbolType type, void* data,
                                     void* user_data);
void symbol_table_foreach(SymbolTable* table, SymbolTableCallback callback, void* user_data);

/* 获取符号表大小 */
size_t symbol_table_size(SymbolTable* table);

/* 生成唯一的 C 名称（带文件名前缀，避免跨文件冲突） */
/* file_name: 文件名，scope: 作用域（NULL表示全局），name: 符号名 */
/* 返回生成的 C 名称，需要调用者释放 */
char* symbol_table_gen_c_name(const char* file_name, const char* scope, const char* name);

/* 全局符号表实例 */
extern SymbolTable* g_symbol_table;

/* 初始化全局符号表 */
void symbol_table_global_init(void);

/* 销毁全局符号表 */
void symbol_table_global_destroy(void);

#endif /* SYMBOL_TABLE_H */
