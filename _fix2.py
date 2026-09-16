import os
with open('src/ir/vm.c', 'r', encoding='utf-8') as f:
    content = f.read()
old = '''            case OPC_FLOAT_TO_DOUBLE: {
                /* 从 float 栈弹出一个 float，转换为 double，压入 double 栈 */
                float fv = FLOAT_POP();
                DOUBLE_PUSH((double)fv);
                break;
            }
            case OPC_BOOL_ARRAY_LIT: {'''
new = '''            case OPC_FLOAT_TO_DOUBLE: {
                /* 从 float 栈弹出一个 float，转换为 double，压入 double 栈 */
                float fv = FLOAT_POP();
                DOUBLE_PUSH((double)fv);
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
            case OPC_BOOL_ARRAY_LIT: {'''
if old in content:
    content = content.replace(old, new)
    print("vm handler ok")
else:
    print("vm handler not found")
with open('src/ir/vm.c', 'w', encoding='utf-8') as f:
    f.write(content)
print("vm.c done")
