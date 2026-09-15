"""
修改 yacc.y 中的泛型数组处理逻辑
当泛型是基本类型时，创建类型化数组
"""
import os

with open('src/parse/yacc.y', 'r', encoding='utf-8') as f:
    content = f.read()

# 修改泛型数组处理逻辑
old_code = '''    | LT ID GT ARRAY_OPEN arg_list RBRACKET {
          /* 泛型自定义类型：<Person>[e1,e2] → [Person(e1), Person(e2)]（形状构造） */
          if(type_lookup($2) != NULL) {
              $$ = L(ast_array_lit(wrap_type_list($2, $5), -1));
              free($2);
          } else {
              yyerror("未定义类型");
          }
      }'''

new_code = '''    | LT ID GT ARRAY_OPEN arg_list RBRACKET {
          /* 泛型数组：<int>[e1,e2] → 类型化数组；<Person>[e1,e2] → [Person(e1), Person(e2)]（形状构造） */
          int ffi_type = lumyr_ffi_type_from_name($2);
          if(ffi_type != FFI_INT || strcmp($2, "int") == 0) {
              /* 基本类型：创建类型化数组 */
              $$ = L(ast_array_lit($5, ffi_type));
              free($2);
          } else if(type_lookup($2) != NULL) {
              /* 自定义类型：将每个元素包装成构造函数调用 */
              $$ = L(ast_array_lit(wrap_type_list($2, $5), -1));
              free($2);
          } else {
              yyerror("未定义类型");
          }
      }'''

if old_code in content:
    content = content.replace(old_code, new_code)
    print("修改泛型数组处理逻辑成功")
else:
    print("未找到目标代码，需要手动检查")

with open('src/parse/yacc.y', 'w', encoding='utf-8') as f:
    f.write(content)

print("完成")
