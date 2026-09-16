"""
修复ir_compile.c中的int算术运算指令生成逻辑：
在生成int算术运算指令之后，把结果从int专用栈弹出，包装成Value，压入Value栈，
以兼容后续的赋值逻辑（赋值给普通变量时需要从Value栈弹出值）。
"""
import os

with open('src/ir/ir_compile.c', 'r', encoding='utf-8') as f:
    content = f.read()

# 找到int算术运算指令生成的代码
old_code = '''                /* 生成 int 专用算术运算指令 */
                static const OpCode int_map[] = {
                    [OP_ADD] = OPC_INT_ADD, [OP_SUB] = OPC_INT_SUB, [OP_MUL] = OPC_INT_MUL,
                    [OP_DIV] = OPC_INT_DIV, [OP_MOD] = OPC_INT_MOD,
                };
                emit(c, int_map[bop], 0, 0);
            } else {'''

new_code = '''                /* 生成 int 专用算术运算指令 */
                static const OpCode int_map[] = {
                    [OP_ADD] = OPC_INT_ADD, [OP_SUB] = OPC_INT_SUB, [OP_MUL] = OPC_INT_MUL,
                    [OP_DIV] = OPC_INT_DIV, [OP_MOD] = OPC_INT_MOD,
                };
                emit(c, int_map[bop], 0, 0);
                /* 把结果从 int 专用栈弹出，包装成 Value，压入 Value 栈
                   以兼容后续的赋值逻辑（赋值给普通变量时需要从 Value 栈弹出值）
                   注意：如果赋值给声明为 int 的变量，会使用 OPC_STORE_INT_VAR，
                   但当前的赋值逻辑还没有针对这种情况优化，后续可以优化 */
                emit(c, OPC_INT_TO_VALUE, 0, 0);
            } else {'''

if old_code in content:
    content = content.replace(old_code, new_code)
    print("修复int算术运算指令生成逻辑成功")
else:
    print("未找到目标内容")

with open('src/ir/ir_compile.c', 'w', encoding='utf-8') as f:
    f.write(content)

print("完成：修复ir_compile.c中的int算术运算指令生成逻辑")
