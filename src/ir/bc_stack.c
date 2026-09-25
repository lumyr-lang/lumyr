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
        case OPC_LOAD_GLOBAL:
        case OPC_GETFUNC:
        case OPC_MKCLOSURE:
        case OPC_PRE_INC: case OPC_POST_INC:
        case OPC_PRE_DEC: case OPC_POST_DEC:
        case OPC_DUP:
        case OPC_NEG: case OPC_POS: case OPC_LOGIC_NOT:
        case OPC_TO_BOOL:
        case OPC_LOAD_STRUCT_PTR:
        case OPC_GET_ERR:
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

        /* GENERIC_BIND：弹 1 PTR 实例，遍历字段转换后压回，净 0 */
        case OPC_GENERIC_BIND:
            d.ptr = 0;
            break;

        /* CALL_METHOD 与 CALL 共用下方精确扣减分支 */

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
            d.value = -2; /* 弹 val+idx+receiver（3），压回 val（1），净 -2 */
            break;
        /* INDEX_GET：弹 idx + receiver，压结果 = 净 -1。
         * 根因修复：此前误按 +1（当作纯压入），循环环内每轮虚增 2，
         * bc_analyze_stack 不动点迭代回边深度无限抬升，-S 挂死。 */
        case OPC_INDEX_GET:
            d.value = -1;
            break;
        /* CALLV：弹 argc 实参 + 1 函数值，压 1 返回值 = 净 -argc。
         * 根因修复：此前误按 +1（纯压入），环内每轮虚增 argc+1，分析挂死。
         * CALLV 只用于表达式语境（结果总保留）。 */
        case OPC_CALLV:
            d.value = -in.b;
            break;
        /* 字面量/内置：元素与实参先压栈，指令弹回后压 1 个结果，必须按计数扣减。
         * 根因修复：此前误按 +1（纯压入），循环环内每轮虚增，栈深分析挂死/误报。 */
        case OPC_ARRAY_LIT:            /* 弹 b 个元素压 1 数组 */
            d.value = 1 - in.b;
            break;
        case OPC_MAP_LIT:              /* 弹 2*b（key+val）压 1 map */
            d.value = 1 - 2 * in.b;
            break;
        case OPC_TYPED_BYTES:          /* 弹 1 源压 1 结果 */
            d.value = 0;
            break;
        case OPC_BUILTIN:              /* 弹 b 个实参压 1 返回值 */
            d.value = 1 - in.b;
            break;
        case OPC_CALL_BUILTIN_METHOD:  /* 弹 b 实参 + 1 receiver 压 1 返回值 = -b */
            d.value = -in.b;
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
        case OPC_VBAND: case OPC_VBOR: case OPC_VBXOR:
        case OPC_VSHL: case OPC_VSHR:
        case OPC_VPOW:
            d.value = -1; /* 2 弹 1 压 */
            break;
        case OPC_VBNOT:
            d.value = 0;  /* 1 弹 1 压 */
            break;
        case OPC_ASSERT_NONNULL:
            d.value = 0;  /* 1 弹 1 压（null 时抛出不压回） */
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

        /* CALL：VM 弹掉 argc 个实参（分散 4 栈），keep_result 时压 1 返回值。
         * 根因修复：实参栈型已在编译期记入 callsite.arg_stacks，必须精确扣减。
         * 此前不扣减（"保守计入"）——在循环环里实参深度每轮虚增，不动点迭代
         * 永不收敛（-S 挂死），且峰值深度被高估。arg_stacks 缺失时按全 VALUE 扣。 */
        case OPC_CALL:
        case OPC_CALL_METHOD: {
            CallSite* csx = (in.a >= 0 && in.a < fn->callsite_cnt)
                            ? &fn->callsites[in.a] : NULL;
            int argc = csx ? csx->argc : in.b;
            for(int _i = 0; _i < argc; _i++) {
                int stk = (csx && csx->arg_stacks) ? csx->arg_stacks[_i] : 0;
                if(stk == 1) d.int64--;
                else if(stk == 2) d.double_stk--;
                else if(stk == 3) d.ptr--;
                else d.value--;
            }
            if(csx && csx->keep_result) {
                switch((ExprType)csx->ret_stack) {
                case EXPR_TYPE_INT:         d.int64++; break;
                case EXPR_TYPE_DOUBLE:      d.double_stk++; break;
                case EXPR_TYPE_PTR:         d.ptr++; break;
                default:                    d.value++; break;
                }
            }
            break;
        }

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
        case OPC_INT64_BAND: case OPC_INT64_BOR: case OPC_INT64_BXOR:
        case OPC_INT64_SHL: case OPC_INT64_SHR:
        case OPC_INT64_POW:
            d.int64 = -1; /* 2 个弹出，1 个压入 */
            break;
        case OPC_INT64_BNOT:
            /* 弹1压1，净变化 0 */
            break;
        case OPC_INT64_TRUNC:
            /* 弹1压1（截断后压回），净变化 0 */
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
        case OPC_DOUBLE_POW:
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
        case OPC_LOAD_GLOBAL:
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
        case OPC_GENERIC_BIND:
        case OPC_NEG: case OPC_POS: case OPC_LOGIC_NOT:
        case OPC_TO_BOOL:
            return 1;

        /* 压入 2 个（算术运算：弹出 2 个，压入 1 个，峰值 +1） */
        case OPC_ADD: case OPC_SUB: case OPC_MUL:
        case OPC_DIV: case OPC_MOD:
        case OPC_INT64_ADD: case OPC_INT64_SUB:
        case OPC_INT64_MUL: case OPC_INT64_DIV:
        case OPC_INT64_MOD:
        case OPC_INT64_BAND: case OPC_INT64_BOR: case OPC_INT64_BXOR:
        case OPC_INT64_SHL: case OPC_INT64_SHR:
        case OPC_INT64_POW:
        case OPC_VBAND: case OPC_VBOR: case OPC_VBXOR:
        case OPC_VSHL: case OPC_VSHR:
        case OPC_VPOW:
        case OPC_DOUBLE_ADD: case OPC_DOUBLE_SUB:
        case OPC_DOUBLE_MUL: case OPC_DOUBLE_DIV:
        case OPC_DOUBLE_POW:
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
        case OPC_TYPED_BYTES:
            return 1;

        default:
            return 0;
    }
}
