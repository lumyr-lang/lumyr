#include "ast_node.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// lexer 维护的当前行号（flex %option yylineno 生成）；解析动作执行时近似指向节点结束行
extern int yylineno;



AstNode* ast_int(long long v)
{
    AstNode* p = ast_new(AST_INT);
    p->val_type = VAL_INT;
    p->u.inum = v;
    return p;
}

AstNode* ast_num(double v)
{
    AstNode* p = malloc(sizeof(AstNode));
    p->type = AST_NUM;
    p->val_type = VAL_DOUBLE;
    p->u.num = v;
    return p;
}

AstNode* ast_bool(_Bool v)
{
    AstNode* p = malloc(sizeof(AstNode));
    p->type = AST_BOOL;
    p->val_type = VAL_BOOL;
    p->line = yylineno;
    p->u.bval = v;
    return p;
}

AstNode* ast_none(void)
{
    AstNode* p = malloc(sizeof(AstNode));
    p->type = AST_NONE;
    p->val_type = VAL_NONE;
    p->line = yylineno;
    return p;
}

AstNode* ast_funcref(const char* name)
{
    AstNode* p = malloc(sizeof(AstNode));
    p->type = AST_FUNCREF;
    p->val_type = VAL_FUNC;
    p->line = yylineno;
    p->u.varname = strdup(name);
    return p;
}

AstNode* ast_new_char(char c)
{
    AstNode* n = ast_new(AST_CHAR);
    n->u.ch = c;
    n->val_type = VAL_CHAR;
    return n;
}

AstNode* ast_string(const char* s)
{
    AstNode* p = malloc(sizeof(AstNode));
    p->type = AST_STRING;
    p->val_type = VAL_STRING;
    p->u.sval = strdup(s);
    return p;
}

AstNode* ast_var(char* name)
{
    AstNode* p = malloc(sizeof(AstNode));
    p->type = AST_VAR;
    p->val_type = VAL_NONE;
    p->line = yylineno;
    p->u.varname = strdup(name);
    return p;
}

AstNode* ast_binop(BinOp op, AstNode* l, AstNode* r)
{
    AstNode* p = malloc(sizeof(AstNode));
    p->type = AST_BINOP;
    p->val_type = VAL_NONE;
    p->u.bin.op = op;
    p->u.bin.left = l;
    p->u.bin.right = r;
    return p;
}

AstNode* ast_assign(char* name, AstNode* e)
{
    AstNode* p = malloc(sizeof(AstNode));
    p->type = AST_ASSIGN;
    p->val_type = VAL_NONE;
    p->line = yylineno;
    p->u.assign.varname = strdup(name);
    p->u.assign.expr = e;
    return p;
}

AstNode* ast_print(AstNode* args)
{
    AstNode* p = malloc(sizeof(AstNode));
    p->type = AST_PRINT;
    p->val_type = VAL_NONE;
    p->u.print.args = args;
    return p;
}

AstNode* ast_seq(AstNode* a, AstNode* b)
{
    AstNode* p = malloc(sizeof(AstNode));
    p->type = AST_SEQ;
    p->val_type = VAL_NONE;
    p->u.seq.first = a;
    p->u.seq.second = b;
    return p;
}
// 把 item 追加到 list 链的末尾（保持扁平：末尾 AST_SEQ.second=NULL）
AstNode* ast_seq_append(AstNode* list, AstNode* item) {
    if(!list) return ast_seq(item, NULL);
    AstNode* p = list;
    while(p->type == AST_SEQ && p->u.seq.second && p->u.seq.second->type == AST_SEQ)
        p = p->u.seq.second;
    if(p->type == AST_SEQ) {
        AstNode* last = p->u.seq.second;
        if(last && last->type != AST_SEQ)
            p->u.seq.second = ast_seq(last, ast_seq(item, NULL));
        else if(!last)
            p->u.seq.second = ast_seq(item, NULL);
        else
            p->u.seq.second = ast_seq_append(last, item);
    }
    return list;
}
// 递归收集 AST_SEQ 树中所有叶子节点的 varname（左到右顺序）
static void collect_vn(AstNode* n, char*** arr, int* cnt, int* cap) {
    if(!n) return;
    if(n->type == AST_SEQ) {
        collect_vn(n->u.seq.first, arr, cnt, cap);
        collect_vn(n->u.seq.second, arr, cnt, cap);
    } else {
        if(*cnt >= *cap) {
            *cap = *cap > 0 ? *cap * 2 : 8;
            *arr = (char**)realloc(*arr, (size_t)(*cap) * sizeof(char*));
        }
        (*arr)[(*cnt)++] = n->u.varname;
    }
}
void ast_collect_varnames(AstNode* seq, char*** out_names, int* out_count) {
    int cap = 0;
    *out_names = NULL;
    *out_count = 0;
    collect_vn(seq, out_names, out_count, &cap);
}

AstNode* ast_if(AstNode* cond, AstNode* then_stmt, AstNode* elif_chain, AstNode* else_stmt)
{
    AstNode* p = malloc(sizeof(AstNode));
    p->type = AST_IF;
    p->val_type = VAL_NONE;
    p->u.ifnode.cond = cond;
    p->u.ifnode.then_stmt = then_stmt;
    p->u.ifnode.elif_chain = elif_chain;
    p->u.ifnode.else_stmt = else_stmt;
    return p;
}

AstNode* ast_block(AstNode* stmts)
{
    AstNode* n = ast_new(AST_BLOCK);
    n->u.block.stmts = stmts;
    return n;
}

AstNode* ast_elif(AstNode* cond, AstNode* body)
{
    AstNode* n = ast_new(AST_ELIF);
    n->u.elif.cond = cond;
    n->u.elif.body = body;
    n->u.elif.next = NULL;
    return n;
}

AstNode* ast_elif_append(AstNode* list, AstNode* elif)
{
    if (!list) return elif;
    AstNode* p = list;
    while (p->u.elif.next != NULL) {
        p = p->u.elif.next;
    }
    p->u.elif.next = elif;
    return list;
}

AstNode* ast_if_chain(AstNode* cond, AstNode* if_body, AstNode* elif_list, AstNode* else_body)
{
    AstNode* n = ast_new(AST_IF_CHAIN);
    n->u.if_chain.cond = cond;
    n->u.if_chain.if_body = if_body;
    n->u.if_chain.elif_list = elif_list;
    n->u.if_chain.else_body = else_body;
    return n;
}

AstNode* ast_new(AstType type)
{
    AstNode* n = calloc(1, sizeof(AstNode));
    if (!n) abort();
    n->type = type;
    n->val_type = VAL_NONE;
    n->line = yylineno;
    return n;
}

AstNode* ast_while(AstNode* cond, AstNode* body)
{
    AstNode* n = ast_new(AST_WHILE);
    n->u.while_node.cond = cond;
    n->u.while_node.body = body;
    return n;
}

AstNode* ast_do_while(AstNode* cond, AstNode* body)
{
    AstNode* n = ast_new(AST_DO_WHILE);
    n->u.while_node.cond = cond;
    n->u.while_node.body = body;
    return n;
}

AstNode* ast_annotation(char* name, AstNode* args)
{
    AstNode* n = ast_new(AST_ANNOTATION);
    n->u.annotation.name = strdup(name);
    n->u.annotation.args = args;
    return n;
}

AstNode* ast_safe_call(AstNode* obj, char* method, AstNode* args)
{
    AstNode* n = ast_new(AST_SAFE_CALL);
    n->u.safe_call.obj = obj;
    n->u.safe_call.method = strdup(method);
    n->u.safe_call.args = args;
    return n;
}

AstNode* ast_null_coalesce(AstNode* left, AstNode* right)
{
    AstNode* n = ast_new(AST_NULL_COALESCE);
    n->u.null_coalesce.left = left;
    n->u.null_coalesce.right = right;
    return n;
}

AstNode* ast_macro_def(char* name, AstNode* params, AstNode* body)
{
    AstNode* n = ast_new(AST_MACRO_DEF);
    n->u.macro_def.name = strdup(name);
    n->u.macro_def.params = params;
    n->u.macro_def.body = body;
    return n;
}

AstNode* ast_for(AstNode* init, AstNode* cond, AstNode* update, AstNode* body)
{
    AstNode* n = ast_new(AST_FOR);
    n->u.for_node.init = init;
    n->u.for_node.cond = cond;
    n->u.for_node.update = update;
    n->u.for_node.body = body;
    return n;
}

AstNode* ast_unary(BinOp op, AstNode* child)
{
    AstNode* n = ast_new(AST_UNARY);
    n->u.uny.op = op;
    n->u.uny.child = child;
    return n;
}

AstNode* new_cast_node(int cast_type, AstNode* child)
{
    AstNode* n = ast_new(AST_CAST);
    n->u.cast.cast_type = cast_type;
    n->u.cast.child = child;
    return n;
}

AstNode* ast_ternary(AstNode* cond, AstNode* t, AstNode* f)
{
    AstNode* n = ast_new(AST_TERNARY);
    n->u.ternary.cond = cond;
    n->u.ternary.true_expr = t;
    n->u.ternary.false_expr = f;
    return n;
}

// 深拷贝 AST 节点（复合赋值下标展开时避免同一节点被引用两次导致双重释放）
AstNode* ast_clone_node(const AstNode* src)
{
    if(!src) return NULL;
    switch(src->type) {
        case AST_INT:    return ast_int(src->u.inum);
        case AST_NUM:    return ast_num(src->u.num);
        case AST_BOOL:   return ast_bool(src->u.bval ? 1 : 0);
        case AST_NONE:   return ast_none();
        case AST_CHAR:   return ast_new_char(src->u.ch);
        case AST_STRING: return ast_string(src->u.sval);
        case AST_VAR:    return ast_var(strdup(src->u.varname));
        case AST_BINOP:  return ast_binop(src->u.bin.op,
                                           ast_clone_node(src->u.bin.left),
                                           ast_clone_node(src->u.bin.right));
        case AST_UNARY:  return ast_unary(src->u.uny.op, ast_clone_node(src->u.uny.child));
        case AST_CAST: {
            AstNode* n = ast_new(AST_CAST);
            n->u.cast.cast_type = src->u.cast.cast_type;
            n->u.cast.child = ast_clone_node(src->u.cast.child);
            return n;
        }
        case AST_INDEX:  return ast_index(ast_clone_node(src->u.index.arr),
                                          ast_clone_node(src->u.index.idx));
        case AST_BLOCK: {
            AstNode* n = ast_new(AST_BLOCK);
            n->u.block.stmts = ast_clone_node(src->u.block.stmts);
            return n;
        }
        case AST_CALL: {
            AstNode* n = ast_new(AST_CALL);
            n->u.call.name = strdup(src->u.call.name);
            n->u.call.args = ast_clone_node(src->u.call.args);
            return n;
        }
        case AST_SEQ: {
            AstNode* n = ast_new(AST_SEQ);
            n->u.seq.first = ast_clone_node(src->u.seq.first);
            n->u.seq.second = ast_clone_node(src->u.seq.second);
            return n;
        }
        case AST_PRINT: {
            AstNode* n = ast_new(AST_PRINT);
            n->u.print.args = ast_clone_node(src->u.print.args);
            return n;
        }
        case AST_ASSIGN: {
            AstNode* n = ast_new(AST_ASSIGN);
            n->u.assign.varname = strdup(src->u.assign.varname);
            n->u.assign.expr = ast_clone_node(src->u.assign.expr);
            return n;
        }
        case AST_IF: {
            AstNode* n = ast_new(AST_IF);
            n->u.ifnode.cond = ast_clone_node(src->u.ifnode.cond);
            n->u.ifnode.then_stmt = ast_clone_node(src->u.ifnode.then_stmt);
            n->u.ifnode.elif_chain = ast_clone_node(src->u.ifnode.elif_chain);
            n->u.ifnode.else_stmt = ast_clone_node(src->u.ifnode.else_stmt);
            return n;
        }
        case AST_IF_CHAIN: {
            AstNode* n = ast_new(AST_IF_CHAIN);
            n->u.if_chain.cond = ast_clone_node(src->u.if_chain.cond);
            n->u.if_chain.if_body = ast_clone_node(src->u.if_chain.if_body);
            n->u.if_chain.elif_list = ast_clone_node(src->u.if_chain.elif_list);
            n->u.if_chain.else_body = ast_clone_node(src->u.if_chain.else_body);
            return n;
        }
        case AST_ELIF: {
            AstNode* n = ast_new(AST_ELIF);
            n->u.elif.cond = ast_clone_node(src->u.elif.cond);
            n->u.elif.body = ast_clone_node(src->u.elif.body);
            n->u.elif.next = ast_clone_node(src->u.elif.next);
            return n;
        }
        case AST_WHILE: {
            AstNode* n = ast_new(AST_WHILE);
            n->u.while_node.cond = ast_clone_node(src->u.while_node.cond);
            n->u.while_node.body = ast_clone_node(src->u.while_node.body);
            return n;
        }
        case AST_DO_WHILE: {
            AstNode* n = ast_new(AST_DO_WHILE);
            n->u.while_node.cond = ast_clone_node(src->u.while_node.cond);
            n->u.while_node.body = ast_clone_node(src->u.while_node.body);
            return n;
        }
        case AST_FOR: {
            AstNode* n = ast_new(AST_FOR);
            n->u.for_node.init = ast_clone_node(src->u.for_node.init);
            n->u.for_node.cond = ast_clone_node(src->u.for_node.cond);
            n->u.for_node.update = ast_clone_node(src->u.for_node.update);
            n->u.for_node.body = ast_clone_node(src->u.for_node.body);
            return n;
        }
        case AST_RETURN: {
            AstNode* n = ast_new(AST_RETURN);
            n->u.ret.ret_val = ast_clone_node(src->u.ret.ret_val);
            return n;
        }
        default:
            // 复合赋值下标展开只会克隆表达式节点；其余类型直接复制（保守）
            {
                AstNode* n = ast_new(src->type);
                *n = *src;   // 浅拷贝整个结构（含 union）
                return n;
            }
    }
}

AstNode* ast_switch(AstNode* expr, AstNode* cases)
{
    AstNode* n = ast_new(AST_SWITCH);
    n->u.sw.cond = expr;
    n->u.sw.cases = cases;
    return n;
}

AstNode* ast_case(AstNode* const_expr, AstNode* body, int is_default)
{
    AstNode* n = ast_new(AST_CASE);
    n->u.cs.const_val = const_expr;
    n->u.cs.body = body;
    n->u.cs.is_default = is_default;
    n->u.cs.next = NULL;
    n->u.cs.is_type_match = 0;
    n->u.cs.match_type = 0;
    return n;
}

AstNode* ast_case_type(int match_type, AstNode* body)
{
    AstNode* n = ast_new(AST_CASE);
    n->u.cs.const_val = NULL;
    n->u.cs.body = body;
    n->u.cs.is_default = 0;
    n->u.cs.next = NULL;
    n->u.cs.is_type_match = 1;
    n->u.cs.match_type = match_type;
    n->u.cs.bind_var = NULL;
    n->u.cs.guard = NULL;
    return n;
}

AstNode* ast_case_guard(const char* bind_var, AstNode* guard, AstNode* body)
{
    AstNode* n = ast_new(AST_CASE);
    n->u.cs.const_val = NULL;
    n->u.cs.body = body;
    n->u.cs.is_default = 0;
    n->u.cs.next = NULL;
    n->u.cs.is_type_match = 0;
    n->u.cs.match_type = 0;
    n->u.cs.bind_var = bind_var ? strdup(bind_var) : NULL;
    n->u.cs.guard = guard;
    return n;
}

AstNode* ast_break(void)
{
    AstNode* n = ast_new(AST_BREAK);
    return n;
}

AstNode* ast_continue(void)
{
    AstNode* n = ast_new(AST_CONTINUE);
    return n;
}

AstNode* ast_return(AstNode* expr)
{
    AstNode* n = ast_new(AST_RETURN);
    n->u.ret.ret_val = expr;
    return n;
}

AstNode* ast_case_append(AstNode* case_list, AstNode* one_case)
{
    if (!one_case) return case_list;
    if (!case_list) return one_case;
    AstNode* p = case_list;
    while (p->u.cs.next != NULL) {
        p = p->u.cs.next;
    }
    p->u.cs.next = one_case;
    return case_list;
}


AstNode* ast_func_def(char* name, AstNode* params, AstNode* body) {
    AstNode* n = ast_new(AST_FUNC_DEF);
    n->u.func_def.name = strdup(name);
    n->u.func_def.params = params;
    n->u.func_def.body = body;
    n->u.func_def.annotations = NULL;
    n->u.func_def.is_const = 0;
    n->u.func_def.generic_params = NULL;
    n->u.func_def.is_generator = 0;
    n->u.func_def.ret_type_name = NULL;
    return n;
}

AstNode* ast_extern_func(char* name, AstNode* params, char* ret_type_name, char* libname) {
    AstNode* n = ast_new(AST_EXTERN_FUNC);
    n->u.extern_func.name = strdup(name);
    n->u.extern_func.params = params;
    n->u.extern_func.ret_type_name = ret_type_name ? strdup(ret_type_name) : NULL;
    n->u.extern_func.libname = libname ? strdup(libname) : NULL;
    return n;
}

// 类型标注 <type>expr：给变量打类型标记（等价 C 的类型声明）
// 数组/map 字面量：当前直接返回原字面量，元素级类型转换后续完善
AstNode* ast_type_annotation(int cast_type, AstNode* expr) {
    AstNode* n = ast_new(AST_TYPE_ANNOTATION);
    n->u.type_annotation.cast_type = cast_type;
    n->u.type_annotation.expr = expr;
    return n;
}

AstNode* ast_interface_annotation(char* interface_name, AstNode* expr) {
    AstNode* n = ast_new(AST_INTERFACE_ANNOTATION);
    n->u.interface_annotation.interface_name = interface_name;
    n->u.interface_annotation.expr = expr;
    return n;
}

AstNode* ast_yield(AstNode* value) {
    AstNode* n = ast_new(AST_YIELD);
    n->u.yieldnode.value = value;
    return n;
}

AstNode* ast_param(char* name, int is_ellipsis, AstNode* default_val) {
    AstNode* n = ast_new(AST_PARAM);
    n->u.param.name = strdup(name);
    n->u.param.is_ellipsis = is_ellipsis;
    n->u.param.default_val = default_val;
    n->u.param.next = NULL;
    n->u.param.constraint = NULL;
    n->u.param.is_ref = 0;
    return n;
}

AstNode* ast_param_constraint(char* name, char* constraint) {
    AstNode* n = ast_new(AST_PARAM);
    n->u.param.name = strdup(name);
    n->u.param.is_ellipsis = 0;
    n->u.param.default_val = NULL;
    n->u.param.next = NULL;
    n->u.param.constraint = constraint ? strdup(constraint) : NULL;
    n->u.param.is_ref = 0;
    return n;
}

AstNode* ast_param_append(AstNode* list, AstNode* p) {
    if(!list) return p;
    AstNode* cur = list;
    while(cur->u.param.next) cur = cur->u.param.next;
    cur->u.param.next = p;
    return list;
}

AstNode* ast_call(char* func_name, AstNode* args) {
    AstNode* n = ast_new(AST_CALL);
    n->u.call.name = strdup(func_name);
    n->u.call.args = args;
    return n;
}

AstNode* ast_dyn_call(AstNode* callee, AstNode* args) {
    AstNode* n = ast_new(AST_DYN_CALL);
    n->u.dyn_call.callee = callee;
    n->u.dyn_call.args = args;
    return n;
}

/* 实参链（左嵌套 AST_SEQ）头部插入 recv：方法链 a.b(x,y) → b(a,x,y) */
AstNode* ast_seq_front(AstNode* chain, AstNode* recv) {
    if(!chain) return recv;
    if(chain->type == AST_SEQ) {
        chain->u.seq.first = ast_seq_front(chain->u.seq.first, recv);
        return chain;
    }
    return ast_seq(recv, chain);
}

AstNode* ast_index(AstNode* arr, AstNode* idx) {
    AstNode* n = ast_new(AST_INDEX);
    n->u.index.arr = arr;
    n->u.index.idx = idx;
    return n;
}

AstNode* ast_index_assign(AstNode* arr, AstNode* idx, AstNode* value) {
    AstNode* n = ast_new(AST_INDEX_ASSIGN);
    n->u.index_assign.arr = arr;
    n->u.index_assign.idx = idx;
    n->u.index_assign.value = value;
    return n;
}

AstNode* ast_array_lit(AstNode* elems) {
    AstNode* n = ast_new(AST_ARRAY_LIT);
    n->u.array_lit.elems = elems;
    return n;
}

AstNode* ast_map_lit(AstNode* entries) {
    AstNode* n = ast_new(AST_MAP_LIT);
    n->u.map_lit.entries = entries;
    return n;
}

AstNode* ast_class_new(char* class_name, int argc, AstNode* args) {
    AstNode* n = ast_new(AST_CLASS_NEW);
    n->u.class_new.class_name = class_name;
    n->u.class_new.argc = argc;
    n->u.class_new.args = args;
    return n;
}

AstNode* ast_try(AstNode* body, char* catch_var, AstNode* catch_body, AstNode* finally_body) {
    AstNode* n = ast_new(AST_TRY);
    n->u.trynode.body = body;
    n->u.trynode.catch_var = catch_var;
    n->u.trynode.catch_body = catch_body;
    n->u.trynode.finally_body = finally_body;
    return n;
}

AstNode* ast_try_multi(AstNode* body, CatchClause* catches, int catch_count, AstNode* finally_body) {
    AstNode* n = ast_new(AST_TRY);
    n->u.trynode.body = body;
    n->u.trynode.catches = catches;
    n->u.trynode.catch_count = catch_count;
    n->u.trynode.finally_body = finally_body;
    /* 兼容旧字段：如果只有一个 catch 且无类型，填充旧字段 */
    if (catch_count == 1 && catches[0].type == NULL) {
        n->u.trynode.catch_var = catches[0].var;
        n->u.trynode.catch_body = catches[0].body;
    } else {
        n->u.trynode.catch_var = NULL;
        n->u.trynode.catch_body = NULL;
    }
    return n;
}

AstNode* ast_throw(AstNode* expr) {
    AstNode* n = ast_new(AST_THROW);
    n->u.thrownode.expr = expr;
    return n;
}
AstNode* ast_destruct(char** names, int count, AstNode* rhs) {
    AstNode* n = ast_new(AST_DESTRUCT);
    n->u.destruct.names = names;
    n->u.destruct.count = count;
    n->u.destruct.rhs = rhs;
    return n;
}
AstNode* ast_spread(AstNode* expr) {
    AstNode* n = ast_new(AST_SPREAD);
    n->u.spread.expr = expr;
    return n;
}

AstNode* ast_map_entry(AstNode* key, AstNode* value) {
    AstNode* n = ast_new(AST_MAP_ENTRY);
    n->u.map_entry.key = key;
    n->u.map_entry.value = value;
    return n;
}

AstNode* ast_arg_append(AstNode* list, AstNode* arg) {
    // 复用于AST_SEQ风格链表，简单复用AST_SEQ串联实参
    return ast_seq(list, arg);
}



// ===================== AST内存释放 =====================
void ast_free(AstNode* node) {
    if(!node) return;
    switch(node->type) {
        case AST_INT:
        case AST_NUM:
        case AST_BOOL:
        case AST_CHAR:
        case AST_NONE:
            break;
        case AST_STRING:
            free(node->u.sval);
            break;
        case AST_VAR:
        case AST_FUNCREF:
            free(node->u.varname);
            break;
        case AST_UNARY:
            ast_free(node->u.uny.child);
            break;
        case AST_BINOP:
            ast_free(node->u.bin.left);
            ast_free(node->u.bin.right);
            break;
        case AST_ASSIGN:
            free(node->u.assign.varname);
            ast_free(node->u.assign.expr);
            break;
        case AST_PRINT:
            ast_free(node->u.print.args);
            break;
        case AST_SEQ:
            ast_free(node->u.seq.first);
            ast_free(node->u.seq.second);
            break;
        case AST_IF:
            ast_free(node->u.ifnode.cond);
            ast_free(node->u.ifnode.then_stmt);
            ast_free(node->u.ifnode.elif_chain);
            ast_free(node->u.ifnode.else_stmt);
            break;
        case AST_BLOCK:
            ast_free(node->u.block.stmts);
            break;
        case AST_IF_CHAIN:{
            ast_free(node->u.if_chain.cond);
            ast_free(node->u.if_chain.if_body);
            AstNode* p = node->u.if_chain.elif_list;
            while(p){
                AstNode* nxt = p->u.elif.next;
                ast_free(p->u.elif.cond);
                ast_free(p->u.elif.body);
                free(p);
                p = nxt;
            }
            ast_free(node->u.if_chain.else_body);
            break;
        }
        case AST_ELIF:
            break;
        case AST_WHILE:
            ast_free(node->u.while_node.cond);
            ast_free(node->u.while_node.body);
            break;
        case AST_DO_WHILE:
            ast_free(node->u.while_node.cond);
            ast_free(node->u.while_node.body);
            break;
        case AST_ANNOTATION:
            free(node->u.annotation.name);
            ast_free(node->u.annotation.args);
            break;
        case AST_SAFE_CALL:
            ast_free(node->u.safe_call.obj);
            free(node->u.safe_call.method);
            ast_free(node->u.safe_call.args);
            break;
        case AST_NULL_COALESCE:
            ast_free(node->u.null_coalesce.left);
            ast_free(node->u.null_coalesce.right);
            break;
        case AST_MACRO_DEF:
            free(node->u.macro_def.name);
            ast_free(node->u.macro_def.params);
            ast_free(node->u.macro_def.body);
            break;
        case AST_FOR:
            ast_free(node->u.for_node.init);
            ast_free(node->u.for_node.cond);
            ast_free(node->u.for_node.update);
            ast_free(node->u.for_node.body);
            break;
        case AST_CAST:
            ast_free(node->u.cast.child);
            break;
        case AST_TERNARY:
            ast_free(node->u.ternary.cond);
            ast_free(node->u.ternary.true_expr);
            ast_free(node->u.ternary.false_expr);
            break;
        case AST_SWITCH:
            ast_free(node->u.sw.cond);
            {
                AstNode* p = node->u.sw.cases;
                while(p) {
                    AstNode* nx = p->u.cs.next;
                    ast_free(p->u.cs.const_val);
                    ast_free(p->u.cs.body);
                    free(p);
                    p = nx;
                }
            }
            break;
        case AST_CASE:
            break;
        case AST_BREAK:
        case AST_CONTINUE:
        case AST_RETURN:
            ast_free(node->u.ret.ret_val);
            break;

        case AST_YIELD:
            if (node->u.yieldnode.value) ast_free(node->u.yieldnode.value);
            break;

        // ==========新增函数相关节点释放==========
        case AST_FUNC_DEF:
        {
            free(node->u.func_def.name);
            // 释放形参链表 AST_PARAM
            AstNode* pp = node->u.func_def.params;
            while(pp)
            {
                AstNode* nx = pp->u.param.next;
                free(pp->u.param.name);
                free(pp);
                pp = nx;
            }
            ast_free(node->u.func_def.annotations);
            ast_free(node->u.func_def.body);
            break;
        }
        case AST_CALL:
        {
            free(node->u.call.name);
            // ast_arg_append 使用 AST_SEQ 链表，直接递归free
            ast_free(node->u.call.args);
            break;
        }
        case AST_DYN_CALL:
        {
            ast_free(node->u.dyn_call.callee);
            ast_free(node->u.dyn_call.args);
            break;
        }
        case AST_INDEX:
            ast_free(node->u.index.arr);
            ast_free(node->u.index.idx);
            break;
        case AST_INDEX_ASSIGN:
            ast_free(node->u.index_assign.arr);
            ast_free(node->u.index_assign.idx);
            ast_free(node->u.index_assign.value);
            break;
        case AST_ARRAY_LIT:
            ast_free(node->u.array_lit.elems);
            break;
        case AST_PARAM:
            // 只被AST_FUNC_DEF内部循环释放，外部不会单独走到这里
            break;

        default: break;
    }
    free(node);
}

