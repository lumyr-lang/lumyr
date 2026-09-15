"""
修改 vm.c，添加 lumyr_typed_arrays.h 头文件包含
"""
import os

with open('src/ir/vm.c', 'r', encoding='utf-8') as f:
    content = f.read()

# 添加 lumyr_typed_arrays.h 头文件包含
old_include = '#include "lumyr_ffi.h"\n#include "ast/stackframe.h"'
new_include = '#include "lumyr_ffi.h"\n#include "lumyr_typed_arrays.h"\n#include "ast/stackframe.h"'

if old_include in content:
    content = content.replace(old_include, new_include)
    print("添加 lumyr_typed_arrays.h 头文件包含成功")
else:
    print("未找到目标内容，需要手动检查")

with open('src/ir/vm.c', 'w', encoding='utf-8') as f:
    f.write(content)

print("完成")
