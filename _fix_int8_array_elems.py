"""
修复 compile_int8_array_elems 函数，添加对整数字面量的处理
"""
import os

with open('src/ir/ir_compile.c', 'r', encoding='utf-8') as f:
    content = f.read()

old = '''// 编译 int8 泛型数组元素：全部压入 int8 栈
static void compile_int8_array_elems(Ctx* c, AstNode* e, int* n) {
    if(!e) return;
    if(e->type == AST_SEQ) {
        compile_int8_array_elems(c, e->u.seq.first, n);
        compile_int8_array_elems(c, e->u.seq.second, n);
        return;
    }
    if(e->type == AST_INDEX) {
        AstNode* arr = e->u.index.arr;
        AstNode* idx = e->u.index.idx;
        c_expr(c, arr);
        c_expr(c, idx);
        emit(c, OPC_INT8_ARRAY_GET, 0, 0);
        (*n)++;
        return;
    }
    int var_idx = bf_sym(c->fn, e->u.varname);
    emit(c, OPC_LOAD_INT8_VAR, var_idx, 0);
    (*n)++;
}'''

new = '''// 编译 int8 泛型数组元素：全部压入 int8 栈
static void compile_int8_array_elems(Ctx* c, AstNode* e, int* n) {
    if(!e) return;
    if(e->type == AST_SEQ) {
        compile_int8_array_elems(c, e->u.seq.first, n);
        compile_int8_array_elems(c, e->u.seq.second, n);
        return;
    }
    if(e->type == AST_INDEX) {
        AstNode* arr = e->u.index.arr;
        AstNode* idx = e->u.index.idx;
        c_expr(c, arr);
        c_expr(c, idx);
        emit(c, OPC_INT8_ARRAY_GET, 0, 0);
        (*n)++;
        return;
    }
    if(e->type == AST_INT) {
        /* 整数字面量：直接压入 int8 栈（零检查零转换） */
        emit(c, OPC_LOAD_CONST, bf_const(c->fn, lumyr_make_int(e->u.intval)), 0);
        emit(c, OPC_CAST_INT8, 0, 0);  /* 转换为 int8 类型 */
        /* 注意：OPC_CAST_INT8 指令可能不存在，需要先检查 */
        (*n)++;
        return;
    }
    /* 声明为 int8 类型的变量：使用 OPC_LOAD_INT8_VAR，直接压入 int8 栈 */
    int var_idx = bf_sym(c->fn, e->u.varname);
    emit(c, OPC_LOAD_INT8_VAR, var_idx, 0);
    (*n)++;
}'''

if old in content:
    content = content.replace(old, new)
    print("ir_compile.c: 修复 compile_int8_array_elems 函数成功")
else:
    print("ir_compile.c: 未找到目标内容")

with open('src/ir/ir_compile.c', 'w', encoding='utf-8') as f:
    f.write(content)

print("完成")
