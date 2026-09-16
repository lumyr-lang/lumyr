"""
添加int/uint/float/double到long long的类型转换指令的栈深度计算、指令名称和VM处理
"""
import os

# ========== 1. bytecode.c: 添加栈深度计算和指令名称 ==========
with open('src/ir/bytecode.c', 'r', encoding='utf-8') as f:
    content = f.read()

# 添加栈深度计算 - 在OPC_FLOAT_TO_DOUBLE之后添加
old_stack_delta = '''        case OPC_FLOAT_TO_DOUBLE:
            return 0;                        /* 专用栈之间转换，不改变 Value 栈深度 */
        /* long long 专用指令 */'''

new_stack_delta = '''        case OPC_FLOAT_TO_DOUBLE:
        case OPC_INT_TO_LONG_LONG:
        case OPC_UINT_TO_LONG_LONG:
        case OPC_FLOAT_TO_LONG_LONG:
        case OPC_DOUBLE_TO_LONG_LONG:
            return 0;                        /* 专用栈之间转换，不改变 Value 栈深度 */
        /* long long 专用指令 */'''

if old_stack_delta in content:
    content = content.replace(old_stack_delta, new_stack_delta)
    print("bytecode.c: 添加类型转换指令栈深度计算成功")
else:
    print("bytecode.c: 未找到栈深度计算目标内容")

# 添加指令名称 - 在OPC_FLOAT_TO_DOUBLE之后添加
old_opc_name = '''        case OPC_FLOAT_TO_DOUBLE: return "FLOAT_TO_DOUBLE";
        /* long long 专用指令 */'''

new_opc_name = '''        case OPC_FLOAT_TO_DOUBLE: return "FLOAT_TO_DOUBLE";
        case OPC_INT_TO_LONG_LONG: return "INT_TO_LONG_LONG";
        case OPC_UINT_TO_LONG_LONG: return "UINT_TO_LONG_LONG";
        case OPC_FLOAT_TO_LONG_LONG: return "FLOAT_TO_LONG_LONG";
        case OPC_DOUBLE_TO_LONG_LONG: return "DOUBLE_TO_LONG_LONG";
        /* long long 专用指令 */'''

if old_opc_name in content:
    content = content.replace(old_opc_name, new_opc_name)
    print("bytecode.c: 添加类型转换指令名称成功")
else:
    print("bytecode.c: 未找到指令名称目标内容")

with open('src/ir/bytecode.c', 'w', encoding='utf-8') as f:
    f.write(content)

# ========== 2. vm.c: 添加VM处理 ==========
with open('src/ir/vm.c', 'r', encoding='utf-8') as f:
    content = f.read()

old_vm_handler = '''            case OPC_FLOAT_TO_DOUBLE: {
                double dv = (double)FLOAT_POP();
                DOUBLE_PUSH(dv);
                break;
            }
            case OPC_PUSH_LONG_LONG_CONST: {'''

new_vm_handler = '''            case OPC_FLOAT_TO_DOUBLE: {
                double dv = (double)FLOAT_POP();
                DOUBLE_PUSH(dv);
                break;
            }
            case OPC_INT_TO_LONG_LONG: {
                long long llv = (long long)INT_POP();
                LONG_LONG_PUSH(llv);
                break;
            }
            case OPC_UINT_TO_LONG_LONG: {
                long long llv = (long long)UINT_POP();
                LONG_LONG_PUSH(llv);
                break;
            }
            case OPC_FLOAT_TO_LONG_LONG: {
                long long llv = (long long)FLOAT_POP();
                LONG_LONG_PUSH(llv);
                break;
            }
            case OPC_DOUBLE_TO_LONG_LONG: {
                long long llv = (long long)DOUBLE_POP();
                LONG_LONG_PUSH(llv);
                break;
            }
            case OPC_PUSH_LONG_LONG_CONST: {'''

if old_vm_handler in content:
    content = content.replace(old_vm_handler, new_vm_handler)
    print("vm.c: 添加类型转换指令VM处理成功")
else:
    print("vm.c: 未找到VM处理目标内容")

with open('src/ir/vm.c', 'w', encoding='utf-8') as f:
    f.write(content)

print("完成！")
