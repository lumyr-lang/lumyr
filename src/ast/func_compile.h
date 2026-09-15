#ifndef FUNC_COMPILE_H
#define FUNC_COMPILE_H
#include "lumyr_value_type.h"
#include "ast_node_type.h"
#include "ir/bytecode.h"

// 解释器 payload：挂在 RuntimeFunc.captures（capture_count == -1 标记），不修改 RuntimeFunc 结构体
typedef struct InterpFuncPayload {
    AstNode* body;            // 函数体 AST（保留引用，VM 模式不再执行）
    BytecodeFunc* bytecode;   // 函数体字节码 IR（VM 执行）
    char** param_names;       // 参数名拷贝（普通参数在前，可变参数最后）
    int param_cnt;
    int has_variadic;
    AstNode** default_vals;   // 默认值表达式数组（普通参数，NULL=无默认值），长度 param_cnt
    int* has_default;          // 是否有默认值（1/0），长度 param_cnt
    int* param_is_ref;         // 参数是否是引用传递（1=ref，0=值传递），长度 param_cnt
    /* 闭包捕获：仅闭包实例（capture_count==-1 且 captured_cell_count>0）使用。
     * captured_names[i] 为捕获变量名，captured_cells[i] 为对应的堆 Value* 单元指针。
     * 共享模板 payload（parse 期创建）此两字段为 NULL/0。 */
    char** captured_names;
    Value** captured_cells;
    int captured_cell_count;
    int is_generator;  // 生成器函数标记（gen func）
} InterpFuncPayload;

// 将AST_FUNC_DEF节点编译生成RuntimeFunc（不持有AstNode，内部提取信息生成IR/解释器句柄）
RuntimeFunc* compile_func_from_ast(AstNode* func_def_ast);
RuntimeFunc* compile_func_from_ast_with_class(AstNode* func_def_ast, const char* class_name);

// ---- 全局函数 AST 表（用于 const fn 编译期求值查找）----
// 注册函数 AST 节点（compile_func_from_ast 内部自动调用）
void func_ast_register(const char* name, AstNode* func_ast);
// 通过函数名查找 AST 节点，未找到返回 NULL
AstNode* func_ast_lookup(const char* name);

// typecheck 转换函数名引用（AST_VAR→AST_FUNCREF）后重编译该函数字节码并替换
void func_compile_recompile(AstNode* def);

// 销毁RuntimeFunc（不碰AST）
void runtime_func_destroy(RuntimeFunc* f);

// ---- 解释器 payload 访问（AST_CALL 参数绑定用）----
// capture_count == -1 标记解释器 payload 模式
_Bool interp_func_is_payload(const RuntimeFunc* rf);
int interp_func_param_cnt(const RuntimeFunc* rf);
_Bool interp_func_has_variadic(const RuntimeFunc* rf);
// 参数名（普通参数在前，可变参数最后），索引越界返回 NULL
const char* interp_func_param_name(const RuntimeFunc* rf, int idx);
// 参数是否有默认值（普通参数索引），越界返回 0
int interp_func_param_has_default(const RuntimeFunc* rf, int idx);
int interp_func_param_is_ref(const RuntimeFunc* rf, int idx);  // 参数是否是 ref 引用传递
// 参数默认值表达式 AST（普通参数索引），越界或无默认值返回 NULL
AstNode* interp_func_param_default(const RuntimeFunc* rf, int idx);

// ---- 解释器 entry 需要知道自己对应的 RuntimeFunc（payload 在 captures 里）----
// 函数调用前设置当前被调函数，entry 入口读取；返回旧值用于调用后恢复
RuntimeFunc* interp_set_current_rf(RuntimeFunc* rf);
RuntimeFunc* interp_current_rf(void);

// ---- 闭包捕获（语义分析 → IR → VM）----
// typecheck 阶段登记某 lambda 引用的外层局部变量名列表（拷贝）。
void func_compile_set_lambda_captures(const char* lambda_name, const char* const* names, int count);
// 查询 lambda 的捕获变量个数（IR 据此决定发射 OPC_GETFUNC 还是 OPC_MKCLOSURE）
int lambda_capture_count(const char* lambda_name);
// 查询 lambda 第 i 个捕获变量名（0 <= i < count）
const char* lambda_capture_name(const char* lambda_name, int i);

// VM 在 OPC_MKCLOSURE 时调用：沿当前帧链装箱 free 变量并生成新的闭包 RuntimeFunc。
// template 为该 lambda 的全局共享 RuntimeFunc（capture_count==-1）。
// 返回一个 VAL_FUNC（持有新堆分配 RuntimeFunc）。
Value closure_make_instance(RuntimeFunc* template_rf, StackFrame* cur_frame);

// 调用闭包时：把 payload 中捕获的 cell 注入新帧的 cell 表（供函数体 OPC_LOAD/STORE_VAR 路由）。
void closure_bind_cells(const RuntimeFunc* rf, StackFrame* callee);

// GC 回调：扫描闭包 payload 中捕获 cell 内的 Value（gc_runtime.c 在 VAL_FUNC 分支调用）。
void lumyr_interp_scan_captures(const RuntimeFunc* rf, void (*mark)(Value));

#endif
