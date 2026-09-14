#ifndef LM_ANNOTATION_H
#define LM_ANNOTATION_H

#include "ast/ast_node_type.h"
#include "ir/rbtree.h"

/* 注解类型标记（可以多个标记，用位运算） */
#define ANNOTATION_TYPE_CLASS    0x01  /* 类注解 */
#define ANNOTATION_TYPE_FUNC     0x02  /* 函数注解 */
#define ANNOTATION_TYPE_FIELD    0x04  /* 字段注解 */
#define ANNOTATION_TYPE_PARAM    0x08  /* 参数注解 */
#define ANNOTATION_TYPE_GLOBAL   0x10  /* 全局注解 */

/* 注解类别标记（系统内置/用户自定义） */
#define ANNOTATION_CATEGORY_SYSTEM    0x01  /* 系统内置注解（作为关键字处理） */
#define ANNOTATION_CATEGORY_USER      0x02  /* 用户自定义注解 */

/* 系统内置注解名称 */
#define ANNOTATION_ABSTRACT    "abstract"
#define ANNOTATION_OVERRIDE    "override"
#define ANNOTATION_DEPRECATED  "deprecated"
#define ANNOTATION_INLINE      "inline"
#define ANNOTATION_NOINLINE    "noinline"
#define ANNOTATION_PACKED      "packed"
#define ANNOTATION_ALIGNED     "aligned"
#define ANNOTATION_SECTION     "section"
#define ANNOTATION_UNUSED      "unused"
#define ANNOTATION_USED        "used"
#define ANNOTATION_WEAK        "weak"
#define ANNOTATION_ALIAS       "alias"
#define ANNOTATION_VISIBILITY  "visibility"
#define ANNOTATION_CONSTRUCTOR "constructor"
#define ANNOTATION_DESTRUCTOR  "destructor"

/* 注解信息结构体 */
typedef struct {
    char* name;              /* 注解名称 */
    int type_marks;          /* 注解类型标记（位运算，可以多个标记） */
    int category;            /* 注解类别标记（系统内置/用户自定义） */
    AstNode* args;           /* 注解参数（AST_SEQ 链表） */
    char* target_class;      /* 目标类名（NULL=非类注解） */
    char* target_func;       /* 目标函数名（NULL=非函数注解） */
    char* target_field;      /* 目标字段名（NULL=非字段注解） */
} AnnotationInfo;

/* 初始化注解注册表 */
void annotation_init(void);

/* 清理注解注册表 */
void annotation_cleanup(void);

/* 注册系统内置注解（在初始化时调用） */
void annotation_register_system(void);

/* 注册注解（返回 1=成功，0=失败） */
int annotation_register(const char* name, int type_marks, int category, AstNode* args,
                        const char* target_class, const char* target_func, const char* target_field);

/* 检查是否是系统内置注解（返回 1=是，0=不是） */
int annotation_is_system(const char* name);

/* 查找某个函数的指定注解（返回 AnnotationInfo* 或 NULL） */
AnnotationInfo* annotation_lookup_func(const char* class_name, const char* func_name, const char* annotation_name);

/* 查找某个类的指定注解（返回 AnnotationInfo* 或 NULL） */
AnnotationInfo* annotation_lookup_class(const char* class_name, const char* annotation_name);

/* 检查某个函数是否有指定注解（返回 1=有，0=没有） */
int annotation_has_func_annotation(const char* class_name, const char* func_name, const char* annotation_name);

/* 检查某个类是否有指定注解（返回 1=有，0=没有） */
int annotation_has_class_annotation(const char* class_name, const char* annotation_name);

/* 遍历所有注解 */
void annotation_foreach(void (*callback)(AnnotationInfo* info, void* user_data), void* user_data);

#endif /* LM_ANNOTATION_H */
