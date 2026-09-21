// lumyr-lang 字节码栈深度分析实现
// 4 核心栈设计：STACK_VALUE / INT64 / DOUBLE / PTR
#include "bc_stack.h"
#include "ir_types.h"
#include <stdio.h>
#include <stdlib.h>

/* ============================================================
 * op_stack_delta：计算一条指令执行后各栈的深度变化
 * 4 核心栈设计，所有细分类型合并到对应宽类型栈
 * ============================================================ */
StackDelta op_stack_delta(BytecodeFunc* fn, Instruction in)
{
    StackDelta d;
    d.value = 0;
    d.int64 = 0;
    d.double_stk = 0;
    d.ptr = 0;

    switch(in.op) {
        /* ===== Value 栈压入指令 ===== */
        case OPC_LOAD_CONST:
        case OPC_LOAD_VAR:
        case OPC_LOAD_VAR_REF:
        case OPC_GETFUNC:
        case OPC_MKCLOSURE:
        case OPC_PRE_INC: case OPC_POST_INC:
        case OPC_PRE_DEC: case OPC_POST_DEC:
        case OPC_DUP:
        case OPC_NEG: case OPC_POS: case OPC_LOGIC_NOT:
        case OPC_TO_BOOL:
        case OPC_INDEX_GET:
        case OPC_LOAD_STRUCT_PTR:
        case OPC_BUILTIN:
        case OPC_CALL_BUILTIN_METHOD: /* 同 OPC_BUILTIN：弹实参+receiver 后压 1 个返回值，保守 +1 */
        case OPC_CALLV:
        case OPC_RETURN:
        case OPC_RETURN_NIL:
        case OPC_GET_ERR:
        case OPC_ARRAY_LIT:
        case OPC_MAP_LIT:
            d.value = +1;
            break;

        /* LOAD_FIELD：弹 1 个 PTR，压 1 个 typed（按 a=stackcls 路由） */
        case OPC_LOAD_FIELD:
            d.ptr = -1;
            if(in.a == 1)        d.int64 = +1;        /* INT64 栈 */
            else if(in.a == 2)   d.double_stk = +1;   /* DOUBLE 栈 */
            /* cls==3 时 ptr -1+1=0，无需额外设置 */
            break;

        /* CLASS_NEW：压 1 个 PTR（实例指针） */
        case OPC_CLASS_NEW:
            d.ptr = +1;
            break;

        /* CALL_METHOD：实参（含 receiver）已分散压入 4 栈，保守不扣减（同 OPC_CALL，
         * 计入最大栈深避免误报）；keep_result 时按 callsite.ret_stack 压返回值到对应栈 */
        case OPC_CALL_METHOD:
            if(in.a >= 0 && in.a < fn->callsite_cnt && fn->callsites[in.a].keep_result) {
                switch((ExprType)fn->callsites[in.a].ret_stack) {
                case EXPR_TYPE_INT:         d.int64 = +1; break;
                case EXPR_TYPE_DOUBLE:      d.double_stk = +1; break;
                case EXPR_TYPE_PTR:         d.ptr = +1; break;
                default:                    d.value = +1; break;
                }
            }
            break;

        /* ===== 弹栈指令：a 选择栈（0 VALUE/1 INT64/2 DOUBLE/3 PTR） ===== */
        case OPC_POP:
            if(in.a == 1) d.int64 = -1;
            else if(in.a == 2) d.double_stk = -1;
            else if(in.a == 3) d.ptr = -1;
            else d.value = -1;
            break;
        case OPC_STORE_VAR:
            d.value = -1;
            break;
        case OPC_INDEX_SET:
            d.value = -3; /* idx + val + receiver */
            break;
        /* STORE_FIELD：弹 typed(值) + 弹 ptr(obj) + 压回 typed(值) = ptr -1 */
        case OPC_STORE_FIELD:
            d.ptr = -1;
            break;
        case OPC_STORE_NESTED_FIELD:
            d.value = -2; /* obj + val */
            break;
        case OPC_ADD: case OPC_SUB: case OPC_MUL:
        case OPC_DIV: case OPC_MOD:
        case OPC_VADD: case OPC_VSUB: case OPC_VMUL:
        case OPC_VDIV: case OPC_VMOD:
            d.value = -1; /* 2 个弹出，1 个压入 */
            break;
        case OPC_VNEG:
            d.value = 0;  /* 1 弹 1 压 */
            break;
        case OPC_GT: case OPC_LT: case OPC_GE:
        case OPC_LE: case OPC_EQ: case OPC_NE:
        case OPC_VGT: case OPC_VLT: case OPC_VGE:
        case OPC_VLE: case OPC_VEQ: case OPC_VNE:
        case OPC_IMPLEMENTS:
            d.value = -1; /* 2 个弹出，1 个压入 bool */
            break;
        case OPC_PRINT:
            d.value = -1;
            break;
        case OPC_THROW:
            d.value = -1;
            break;
        case OPC_JMP:
            break;
        case OPC_JMP_IF_FALSE:
        case OPC_JMP_IF_TRUE:
        case OPC_JMP_IF_NULL:
        case OPC_JMP_IF_FALSE_V:
        case OPC_JMP_IF_TRUE_V:
            d.value = -1;
            break;
        case OPC_TRY:
            break;
        case OPC_ENDTRY:
            break;
        case OPC_FIN_PUSH:
            break;
        case OPC_FINISH:
            break;
        case OPC_PEND_RETURN:
            break;
        case OPC_CATCH_MATCH:
            break;
        case OPC_YIELD:
            d.value = 0;
            break;
        case OPC_HALT:
            break;
        case OPC_NOP:
            break;

        /* PUSH_CONST_IDX：按常量池实际类型压入对应栈（与 vm_exec_load_const_idx 一致） */
        case OPC_PUSH_CONST_IDX:
            if(in.a >= 0 && in.a < fn->const_cnt) {
                switch(fn->const_pool[in.a].type) {
                    case CONST_INT64:
                    case CONST_UINT64: d.int64 = +1; break;
                    case CONST_DOUBLE: d.double_stk = +1; break;
                    case CONST_STRING: d.ptr = +1; break;
                    default: break;
                }
            }
            break;

        /* PUSH_INT_VAL / PUSH_CONST_VAL：直接压 VALUE 栈 */
        case OPC_PUSH_INT_VAL:
        case OPC_PUSH_CONST_VAL:
            d.value = +1;
            break;

        /* CALL：压返回值与否取决于 callsite.keep_result。
         * argc 个实参分散在各栈，此处不精确扣减（保守计入最大栈深，避免误报）。 */
        case OPC_CALL:
            if(in.a >= 0 && in.a < fn->callsite_cnt && fn->callsites[in.a].keep_result)
                d.value = +1;
            break;

        /* ===== INT64 栈压入指令 ===== */
        case OPC_PUSH_INT64_CONST:
        case OPC_LOAD_INT64_VAR:
            d.int64 = +1;
            break;

        /* ===== INT64 栈弹出指令 ===== */
        case OPC_STORE_INT64_VAR:
            d.int64 = -1;
            break;
        case OPC_INT64_ADD: case OPC_INT64_SUB:
        case OPC_INT64_MUL: case OPC_INT64_DIV:
        case OPC_INT64_MOD:
            d.int64 = -1; /* 2 个弹出，1 个压入 */
            break;
        case OPC_INT64_GT: case OPC_INT64_LT:
        case OPC_INT64_GE: case OPC_INT64_LE:
        case OPC_INT64_EQ: case OPC_INT64_NE:
            d.value = +1; /* 比较结果压入 Value 栈（bool） */
            d.int64 = -2;
            break;
        case OPC_INT64_TO_VALUE:
            d.int64 = -1;
            d.value = +1;
            break;
        case OPC_INT64_TO_DOUBLE:
            d.int64 = -1;
            d.double_stk = +1;
            break;
        case OPC_INT64_TO_PTR:
            d.int64 = -1;
            d.ptr = +1;
            break;
        case OPC_BOX_INT64:
            d.int64 = -1;
            d.value = +1;
            break;
        case OPC_BOX_DOUBLE:
            d.double_stk = -1;
            d.value = +1;
            break;
        case OPC_BOX_PTR:
            d.ptr = -1;
            d.value = +1;
            break;
        case OPC_UNBOX_INT64:
            d.value = -1;
            d.int64 = +1;
            break;
        case OPC_UNBOX_DOUBLE:
            d.value = -1;
            d.double_stk = +1;
            break;
        case OPC_UNBOX_PTR:
            d.value = -1;
            d.ptr = +1;
            break;
        case OPC_INT64_INDEX_SET:
            d.int64 = -3; /* idx + val + receiver */
            break;
        case OPC_INT64_ARRAY_LIT:
            d.int64 = -in.a; /* 弹出 n 个 */
            d.value = +1; /* 压入数组引用 */
            break;
        case OPC_PRINT_INT64:
            d.int64 = -1;
            break;

        /* ===== DOUBLE 栈压入指令 ===== */
        case OPC_PUSH_DOUBLE_CONST:
        case OPC_LOAD_DOUBLE_VAR:
            d.double_stk = +1;
            break;

        /* ===== DOUBLE 栈弹出指令 ===== */
        case OPC_STORE_DOUBLE_VAR:
            d.double_stk = -1;
            break;
        case OPC_DOUBLE_ADD: case OPC_DOUBLE_SUB:
        case OPC_DOUBLE_MUL: case OPC_DOUBLE_DIV:
            d.double_stk = -1; /* 2 个弹出，1 个压入 */
            break;
        case OPC_DOUBLE_GT: case OPC_DOUBLE_LT:
        case OPC_DOUBLE_GE: case OPC_DOUBLE_LE:
        case OPC_DOUBLE_EQ: case OPC_DOUBLE_NE:
            d.value = +1; /* 比较结果压入 Value 栈（bool） */
            d.double_stk = -2;
            break;
        case OPC_DOUBLE_TO_VALUE:
            d.double_stk = -1;
            d.value = +1;
            break;
        case OPC_DOUBLE_TO_INT64:
            d.double_stk = -1;
            d.int64 = +1;
            break;
        case OPC_DOUBLE_INDEX_SET:
            d.double_stk = -3; /* idx + val + receiver */
            break;
        case OPC_DOUBLE_ARRAY_LIT:
            d.double_stk = -in.a; /* 弹出 n 个 */
            d.value = +1; /* 压入数组引用 */
            break;
        case OPC_PRINT_DOUBLE:
            d.double_stk = -1;
            break;

        /* ===== PTR 栈指令 ===== */
        case OPC_PUSH_PTR_CONST:
        case OPC_LOAD_PTR_VAR:
            d.ptr = +1;
            break;
        case OPC_STORE_PTR_VAR:
            d.ptr = -1;
            break;
        case OPC_PTR_TO_INT64:
            d.ptr = -1;
            d.int64 = +1;
            break;
        case OPC_PTR_ARRAY_LIT:
            d.ptr = -in.a; /* 弹出 n 个 */
            d.value = +1; /* 压入数组引用 */
            break;
        case OPC_PRINT_PTR:
            d.ptr = -1;
            break;

        /* ===== CAST 指令（栈间转换） ===== */
        case OPC_CAST_INT:
        case OPC_CAST_INT8: case OPC_CAST_INT16:
        case OPC_CAST_INT32: case OPC_CAST_INT64:
        case OPC_CAST_UINT8: case OPC_CAST_UINT16:
        case OPC_CAST_UINT32: case OPC_CAST_UINT64:
        case OPC_CAST_BOOL: case OPC_CAST_CHAR:
        case OPC_CAST_BYTE: case OPC_CAST_LONG:
        case OPC_CAST_LONGLONG:
            /* Value → int64 */
            d.value = -1;
            d.int64 = +1;
            break;
        case OPC_CAST_DOUBLE: case OPC_CAST_FLOAT:
            /* Value → double */
            d.value = -1;
            d.double_stk = +1;
            break;
        case OPC_CAST_STRING: case OPC_CAST_ASCII:
            /* Value → ptr (string) */
            d.value = -1;
            d.ptr = +1;
            break;

        default:
            break;
    }

    return d;
}

/* ============================================================
 * op_stack_push：指令执行瞬间的额外栈高
 * ============================================================ */
int op_stack_push(OpCode op)
{
    switch(op) {
        /* 压入 1 个 */
        case OPC_LOAD_CONST:
        case OPC_LOAD_VAR:
        case OPC_LOAD_VAR_REF:
        case OPC_GETFUNC:
        case OPC_MKCLOSURE:
        case OPC_PUSH_INT64_CONST:
        case OPC_PUSH_DOUBLE_CONST:
        case OPC_PUSH_PTR_CONST:
        case OPC_LOAD_INT64_VAR:
        case OPC_LOAD_DOUBLE_VAR:
        case OPC_LOAD_PTR_VAR:
        case OPC_DUP:
        case OPC_INDEX_GET:
        case OPC_LOAD_FIELD:
        case OPC_LOAD_STRUCT_PTR:
        case OPC_BUILTIN:
        case OPC_CALL_BUILTIN_METHOD:
        case OPC_CALL:
        case OPC_CALLV:
        case OPC_CALL_METHOD:
        case OPC_RETURN:
        case OPC_RETURN_NIL:
        case OPC_GET_ERR:
        case OPC_CLASS_NEW:
        case OPC_NEG: case OPC_POS: case OPC_LOGIC_NOT:
        case OPC_TO_BOOL:
            return 1;

        /* 压入 2 个（算术运算：弹出 2 个，压入 1 个，峰值 +1） */
        case OPC_ADD: case OPC_SUB: case OPC_MUL:
        case OPC_DIV: case OPC_MOD:
        case OPC_INT64_ADD: case OPC_INT64_SUB:
        case OPC_INT64_MUL: case OPC_INT64_DIV:
        case OPC_INT64_MOD:
        case OPC_DOUBLE_ADD: case OPC_DOUBLE_SUB:
        case OPC_DOUBLE_MUL: case OPC_DOUBLE_DIV:
            return 1;

        /* 比较运算：弹出 2 个，压入 1 个 bool */
        case OPC_GT: case OPC_LT: case OPC_GE:
        case OPC_LE: case OPC_EQ: case OPC_NE:
        case OPC_IMPLEMENTS:
        case OPC_INT64_GT: case OPC_INT64_LT:
        case OPC_INT64_GE: case OPC_INT64_LE:
        case OPC_INT64_EQ: case OPC_INT64_NE:
        case OPC_DOUBLE_GT: case OPC_DOUBLE_LT:
        case OPC_DOUBLE_GE: case OPC_DOUBLE_LE:
        case OPC_DOUBLE_EQ: case OPC_DOUBLE_NE:
            return 1;

        /* 栈间转换 */
        case OPC_INT64_TO_VALUE: case OPC_DOUBLE_TO_VALUE:
        case OPC_INT64_TO_DOUBLE: case OPC_DOUBLE_TO_INT64:
        case OPC_INT64_TO_PTR: case OPC_PTR_TO_INT64:
        case OPC_CAST_INT: case OPC_CAST_DOUBLE:
        case OPC_CAST_STRING: case OPC_CAST_BOOL:
        case OPC_UNBOX_INT64: case OPC_UNBOX_DOUBLE: case OPC_UNBOX_PTR:
        case OPC_STR_TO_INT64: case OPC_STR_TO_DOUBLE:
            return 1;

        /* 不压栈 */
        case OPC_NOP:
        case OPC_POP:
        case OPC_STORE_VAR:
        case OPC_STORE_INT64_VAR:
        case OPC_STORE_DOUBLE_VAR:
        case OPC_STORE_PTR_VAR:
        case OPC_PRINT:
        case OPC_PRINT_INT64:
        case OPC_PRINT_DOUBLE:
        case OPC_PRINT_PTR:
        case OPC_JMP:
        case OPC_JMP_IF_FALSE:
        case OPC_JMP_IF_TRUE:
        case OPC_JMP_IF_NULL:
        case OPC_TRY:
        case OPC_ENDTRY:
        case OPC_FIN_PUSH:
        case OPC_FINISH:
        case OPC_PEND_RETURN:
        case OPC_CATCH_MATCH:
        case OPC_THROW:
        case OPC_YIELD:
        case OPC_HALT:
        case OPC_PRE_INC: case OPC_POST_INC:
        case OPC_PRE_DEC: case OPC_POST_DEC:
            return 0;

        /* 数组操作（需要额外栈空间） */
        case OPC_INDEX_SET:
        case OPC_TYPED_INDEX_SET:
        case OPC_INT64_INDEX_SET:
        case OPC_DOUBLE_INDEX_SET:
        case OPC_STORE_FIELD:
        case OPC_STORE_NESTED_FIELD:
            return 1;

        case OPC_ARRAY_LIT:
        case OPC_INT64_ARRAY_LIT:
        case OPC_DOUBLE_ARRAY_LIT:
        case OPC_PTR_ARRAY_LIT:
        case OPC_MAP_LIT:
            return 1;

        default:
            return 0;
    }
}
