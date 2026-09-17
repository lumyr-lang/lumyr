#ifndef LUMYR_TYPES_H
#define LUMYR_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include "lumyr_value.h"

// AST节点类型
typedef enum {
    AST_INT,
    AST_NUM,
    AST_BOOL,
    AST_CHAR,
    AST_STRING,
    AST_NONE,   // null 字面量
    AST_FUNCREF,  // 函数名引用（函数作为值）
    AST_VAR,
    AST_BINOP,
    AST_UNARY,
    AST_ASSIGN,
    AST_PRINT,
    AST_SEQ,
    AST_IF,
    AST_BLOCK,
    AST_IF_CHAIN,
    AST_ELIF,
    AST_WHILE,
    AST_DO_WHILE,
    AST_ANNOTATION,
    AST_SAFE_CALL,
    AST_NULL_COALESCE,
    AST_MACRO_DEF,
    AST_FOR,
    AST_CAST,
    AST_TERNARY,
    AST_SWITCH,
    AST_CASE,
    AST_BREAK,
    AST_CONTINUE,
    AST_RETURN,
    AST_FUNC_DEF,      // 函数定义 func f(a,...args){}
    AST_PARAM,         // 形参节点（普通 / ...可变）
    AST_CALL,          // 函数调用 f(1,2,3)
    AST_DYN_CALL,      // 动态调用链 f(1)(2)：callee 是表达式（函数值）
    AST_INDEX,         // 数组下标读 a[i]
    AST_INDEX_ASSIGN,  // 数组下标写 a[i] = v
    AST_ARRAY_LIT,     // 数组字面量 [1,2,3]
    AST_MAP_LIT,       // 字典字面量 {"k": v, ...}
    AST_MAP_ENTRY,     // 字典字面量的一项（键表达式 + 值表达式）
    AST_TRY,           // try { body } catch (e) { handler } finally { }（catch/finally 可省略其一）
    AST_THROW,         // throw expr：显式抛错
    AST_DESTRUCT,      // 解构赋值 a,b = [1,2]
    AST_SPREAD,        // 展开运算符 ...expr（数组/map 字面量内）
    AST_YIELD,         // yield 表达式（生成器函数中）
    AST_EXTERN_FUNC,   // FFI 外部函数声明 extern func f(a): ret
    AST_TYPE_ANNOTATION, // 类型标注 <type>expr：给变量打类型标记（等价 C 的类型声明）
    AST_INTERFACE_ANNOTATION, // 接口类型标注 <Interface>expr：给变量打接口引用类型标记
    AST_CLASS_NEW,       // 创建 class 实例（C 结构体）：ClassNew("Animal")
} AstType;

// 二元运算符
typedef enum {
    OP_ADD,
    OP_SUB,
    OP_MUL,
    OP_DIV,
    OP_MOD,
    OP_GT,
    OP_LT,
    OP_GE,
    OP_LE,
    OP_EQ,
    OP_NE,
    OP_LOGIC_AND,
    OP_LOGIC_OR,
    OP_LOGIC_NOT,
    OP_PRE_INC,
    OP_POST_INC,
    OP_PRE_DEC,
    OP_POST_DEC,
    OP_UNARY_PLUS,
    OP_UNARY_MINUS,
    OP_IMPLEMENTS,  // obj implements Interface
} BinOp;

// 辅助：ValueType → C源码字符串（用于语义检查错误消息，需覆盖所有类型）
static inline const char* valtype_to_cstr(ValueType t)
{
    switch(t){
        /* 基础类型 */
        case VAL_NONE:       return "null";
        case VAL_INT:        return "long long";
        case VAL_DOUBLE:     return "double";
        case VAL_BOOL:       return "_Bool";
        case VAL_CHAR:       return "char";
        case VAL_STRING:     return "char*";
        case VAL_FUNC:       return "func";
        case VAL_ARRAY:      return "void*";
        case VAL_MAP:        return "map";
        case VAL_ERROR:      return "error";
        case VAL_BYTE:       return "uint8_t";
        case VAL_GENERATOR:  return "generator";
        case VAL_STRUCT_PTR: return "struct*";
        case VAL_CLASS_PTR:  return "class*";
        case VAL_TYPED_ARRAY: return "typed_array";
        /* 扩展固定宽度类型 */
        case VAL_VOID:       return "void";
        case VAL_INT8:       return "int8_t";
        case VAL_INT16:      return "int16_t";
        case VAL_INT32:      return "int32_t";
        case VAL_INT64:      return "int64_t";
        case VAL_LONG_LONG:  return "long long";
        case VAL_LONG:       return "long";
        case VAL_UINT8:      return "uint8_t";
        case VAL_UINT16:     return "uint16_t";
        case VAL_UINT32:     return "uint32_t";
        case VAL_UINT:       return "unsigned int";
        case VAL_UINT64:     return "uint64_t";
        case VAL_ULONG:      return "unsigned long";
        case VAL_UCHAR:      return "unsigned char";
        case VAL_SHORT:      return "short";
        case VAL_USHORT:     return "unsigned short";
        case VAL_SIZE_T:     return "size_t";
        case VAL_SSIZE_T:    return "ssize_t";
        case VAL_FLOAT:      return "float";
        case VAL_LONG_DOUBLE: return "long double";
        case VAL_PTR:        return "void*";
        case VAL_CALLBACK:   return "callback";
        default:             return "unknown";
    }
}

#endif
