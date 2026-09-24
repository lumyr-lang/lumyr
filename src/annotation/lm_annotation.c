#include "lm_annotation.h"
#include <stdlib.h>
#include <string.h>

/* 注解注册表（使用红黑树存储，key 为 (class_name, func_name)） */
static RBTree* g_annotation_tree = NULL;

/* 系统内置注解名称列表（用于快速判断是否是系统内置注解） */
static const char* g_system_annotations[] = {
    ANNOTATION_ABSTRACT,
    ANNOTATION_OVERRIDE,
    ANNOTATION_DEPRECATED,
    ANNOTATION_INLINE,
    ANNOTATION_NOINLINE,
    ANNOTATION_PACKED,
    ANNOTATION_ALIGNED,
    ANNOTATION_SECTION,
    ANNOTATION_UNUSED,
    ANNOTATION_USED,
    ANNOTATION_WEAK,
    ANNOTATION_ALIAS,
    ANNOTATION_VISIBILITY,
    ANNOTATION_CONSTRUCTOR,
    ANNOTATION_DESTRUCTOR,
    ANNOTATION_FUNCALIAS,
    NULL
};

/* 遍历回调函数的用户数据 */
typedef struct {
    void (*callback)(AnnotationInfo* info, void* user_data);
    void* user_data;
} AnnotationForeachData;

/* 红黑树遍历回调函数 */
static void annotation_rbtree_callback(const char* class_name, const char* method_name, void* data, void* user_data) {
    AnnotationForeachData* foreach_data = (AnnotationForeachData*)user_data;
    if (data && foreach_data && foreach_data->callback) {
        foreach_data->callback((AnnotationInfo*)data, foreach_data->user_data);
    }
}

/* 初始化注解注册表 */
void annotation_init(void) {
    if (g_annotation_tree) {
        rbtree_destroy(g_annotation_tree);
    }
    g_annotation_tree = rbtree_create();
}

/* 清理注解注册表 */
void annotation_cleanup(void) {
    if (g_annotation_tree) {
        rbtree_destroy(g_annotation_tree);
        g_annotation_tree = NULL;
    }
}

/* 注册系统内置注解（在初始化时调用） */
void annotation_register_system(void) {
    /* 系统内置注解不需要注册到红黑树中，因为它们是关键字，在词法分析阶段就被识别了 */
    /* 这个函数预留用于后续扩展，比如注册系统内置注解的元数据 */
}

/* 检查是否是系统内置注解（返回 1=是，0=不是） */
int annotation_is_system(const char* name) {
    if (!name) return 0;
    for (int i = 0; g_system_annotations[i] != NULL; i++) {
        if (strcmp(name, g_system_annotations[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

/* 注册注解（返回 1=成功，0=失败） */
int annotation_register(const char* name, int type_marks, int category, AstNode* args,
                        const char* target_class, const char* target_func, const char* target_field) {
    if (!name) return 0;
    /* 惰性初始化注解注册表（与 ast_types/func_table 等模块一致的懒初始化策略） */
    if (!g_annotation_tree) g_annotation_tree = rbtree_create();
    if (!g_annotation_tree) return 0;

    /* 创建注解信息 */
    AnnotationInfo* info = (AnnotationInfo*)malloc(sizeof(AnnotationInfo));
    if (!info) return 0;
    memset(info, 0, sizeof(AnnotationInfo));

    info->name = strdup(name);
    info->type_marks = type_marks;
    info->category = category;
    info->args = args;
    info->target_class = target_class ? strdup(target_class) : NULL;
    info->target_func = target_func ? strdup(target_func) : NULL;
    info->target_field = target_field ? strdup(target_field) : NULL;

    /* 插入到红黑树中，key 为 (target_class, target_func) */
    /* 对于类注解（没有 target_func），用 "__class__" 作为 method_name */
    const char* method_name = target_func ? target_func : "__class__";
    rbtree_insert(g_annotation_tree, NS_PROPERTY, target_class, method_name, info);

    return 1;
}

/* 查找某个函数的指定注解（返回 AnnotationInfo* 或 NULL） */
AnnotationInfo* annotation_lookup_func(const char* class_name, const char* func_name, const char* annotation_name) {
    if (!func_name || !annotation_name || !g_annotation_tree) return NULL;

    /* 从红黑树中查找 */
    AnnotationInfo* info = (AnnotationInfo*)rbtree_find(g_annotation_tree, NS_PROPERTY, class_name, func_name);
    if (info && info->name && strcmp(info->name, annotation_name) == 0) {
        return info;
    }

    return NULL;
}

/* 查找某个类的指定注解（返回 AnnotationInfo* 或 NULL） */
AnnotationInfo* annotation_lookup_class(const char* class_name, const char* annotation_name) {
    if (!class_name || !annotation_name || !g_annotation_tree) return NULL;

    /* 从红黑树中查找，类注解的 method_name 为 "__class__" */
    AnnotationInfo* info = (AnnotationInfo*)rbtree_find(g_annotation_tree, NS_PROPERTY, class_name, "__class__");
    if (info && info->name && strcmp(info->name, annotation_name) == 0) {
        return info;
    }

    return NULL;
}

/* 检查某个函数是否有指定注解（返回 1=有，0=没有） */
int annotation_has_func_annotation(const char* class_name, const char* func_name, const char* annotation_name) {
    return annotation_lookup_func(class_name, func_name, annotation_name) != NULL;
}

/* 检查某个类是否有指定注解（返回 1=有，0=没有） */
int annotation_has_class_annotation(const char* class_name, const char* annotation_name) {
    return annotation_lookup_class(class_name, annotation_name) != NULL;
}

/* 遍历所有注解 */
void annotation_foreach(void (*callback)(AnnotationInfo* info, void* user_data), void* user_data) {
    if (!callback || !g_annotation_tree) return;

    AnnotationForeachData foreach_data;
    foreach_data.callback = callback;
    foreach_data.user_data = user_data;

    rbtree_foreach(g_annotation_tree, annotation_rbtree_callback, &foreach_data);
}
