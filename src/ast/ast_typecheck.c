/* 编译期模块：func_depth/in_lambda/lambda_locals_cnt/g_global_vars_cnt 为单线程编译状态，
 * 未来并发编译需实例化。 */
#include "ast_typecheck.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ast_symtab.h"
#include "ast_runtime_sym.h"
#include "func_compile.h"
#include "ast_types.h"
#include "ast_node.h"


// ---------------- 作用域快照 ----------------
// static_sym 是全局单表；检查函数体前保存、检查后恢复，
// 使函数参数/局部变量不泄漏到顶层，且参数能遮蔽同名全局。
static SymStaticEntry* saved_table = NULL;
static int saved_count = 0;
static int saved_cap = 0;

/* 作用域快照：保存当前表（仅前 count 项），检查函数体后恢复。
 * 嵌套深度无硬上限：每次进入分配 saved_count 项副本。 */
static void sym_save(void) {
    saved_count = static_sym_count;
    if(saved_cap < saved_count) {
        SymStaticEntry* nt = (SymStaticEntry*)realloc(saved_table, (size_t)saved_count * sizeof(SymStaticEntry));
        if(!nt) { fprintf(stderr, "作用域快照内存不足\n"); exit(EXIT_FAILURE); }
        saved_table = nt;
        saved_cap = saved_count;
    }
    memcpy(saved_table, static_sym_table, (size_t)saved_count * sizeof(SymStaticEntry));
}

static void sym_restore(void) {
    for (int i = saved_count; i < static_sym_count; ++i) {
        free(static_sym_table[i].name);
    }
    static_sym_count = saved_count;
}

// 函数体递归深度：>0 表示正在检查某函数体，其内的嵌套 func 定义跳过
/* typecheck 把函数名引用（AST_VAR→AST_FUNCREF）就地转换后，记录受影响的函数定义，
 * 结束阶段重编译其字节码（parse 期生成的字节码里函数名引用还是 LOAD_VAR）。 */
static AstNode* g_cur_func_def = NULL;
static AstNode** g_recompile = NULL;
static int g_recompile_cnt = 0;
static int g_recompile_cap = 0;

static int func_depth = 0;
static int g_collect_err = 0;   // 顶层收集阶段错误（函数重复定义等）
/* 匿名函数捕获：lambda 只能访问 参数 + 全局变量 + 自身局部；
 * 引用到的外层函数局部变量记录为"捕获变量"，运行时装箱为闭包 cell。 */
static int in_lambda = 0;
static AstNode* g_lambda_params = NULL;
static char** lambda_locals = NULL;
static int lambda_locals_cnt = 0;
static int lambda_locals_cap = 0;
static char** g_global_vars = NULL;
static int g_global_vars_cnt = 0;
static int g_global_vars_cap = 0;

// 捕获列表栈：每层 lambda 一个，记录其引用的外层局部变量名（引用语义）
typedef struct { char** names; int cnt; int cap; } CaptList;
static CaptList* g_cap_stack = NULL;
static int g_cap_depth = 0;
static int g_cap_cap = 0;

static void cap_push(void) {
    if(g_cap_depth >= g_cap_cap) {
        int nc = g_cap_cap > 0 ? g_cap_cap * 2 : 8;
        CaptList* nt = (CaptList*)realloc(g_cap_stack, (size_t)nc * sizeof(CaptList));
        if(!nt) { fprintf(stderr, "捕获栈扩容内存不足\n"); exit(EXIT_FAILURE); }
        g_cap_stack = nt; g_cap_cap = nc;
    }
    g_cap_stack[g_cap_depth].names = NULL;
    g_cap_stack[g_cap_depth].cnt = 0;
    g_cap_stack[g_cap_depth].cap = 0;
    g_cap_depth++;
}

static void cap_add(const char* name) {
    CaptList* L = &g_cap_stack[g_cap_depth - 1];
    for(int i = 0; i < L->cnt; i++) if(strcmp(L->names[i], name) == 0) return;
    if(L->cnt >= L->cap) {
        int nc = L->cap > 0 ? L->cap * 2 : 8;
        char** nn = (char**)realloc(L->names, (size_t)nc * sizeof(char*));
        if(!nn) { fprintf(stderr, "捕获名表扩容内存不足\n"); exit(EXIT_FAILURE); }
        L->names = nn; L->cap = nc;
    }
    L->names[L->cnt++] = strdup(name);
}

// 弹出当前 lambda 的捕获列表并登记到侧表，返回捕获个数
static int cap_pop_record(const char* lambda_name) {
    g_cap_depth--;
    CaptList* L = &g_cap_stack[g_cap_depth];
    func_compile_set_lambda_captures(lambda_name, (const char* const*)L->names, L->cnt);
    int n = L->cnt;
    for(int i = 0; i < L->cnt; i++) free(L->names[i]);
    free(L->names);
    L->names = NULL; L->cnt = 0; L->cap = 0;
    return n;
}

static int is_global_var(const char* n) {
    if(strcmp(n, "log") == 0) return 1;  // 预定义全局对象：log.debug/info/warn/error/fatal
    for(int i = 0; i < g_global_vars_cnt; i++)
        if(strcmp(g_global_vars[i], n) == 0) return 1;
    return 0;
}
static int is_lambda_local(const char* n) {
    for(int i = 0; i < lambda_locals_cnt; i++)
        if(strcmp(lambda_locals[i], n) == 0) return 1;
    return 0;
}
static int is_lambda_param(const char* n) {
    for(AstNode* p = g_lambda_params; p; p = p->u.param.next)
        if(strcmp(p->u.param.name, n) == 0) return 1;
    return 0;
}

// ---------------- 阶段1：顶层收集 ----------------
// 遍历顶层（不深入函数体）：登记全部函数名 + 顶层赋值变量，
// 使函数前向引用、函数体读取全局变量在阶段2都能通过。

static void collect_top_level(AstNode* node) {
    if (!node) return;
    switch (node->type) {
        case AST_ASSIGN:
            static_sym_put(node->u.assign.varname, VAL_NONE);
            collect_top_level(node->u.assign.expr);
            if(g_global_vars_cnt >= g_global_vars_cap) {
                int nc = g_global_vars_cap > 0 ? g_global_vars_cap * 2 : 64;
                char** nt = (char**)realloc(g_global_vars, (size_t)nc * sizeof(char*));
                if(!nt) { fprintf(stderr, "全局变量表扩容内存不足\n"); exit(EXIT_FAILURE); }
                g_global_vars = nt; g_global_vars_cap = nc;
            }
            g_global_vars[g_global_vars_cnt++] = strdup(node->u.assign.varname);
            break;
        case AST_FUNC_DEF: {
            // 函数重复定义：与 C 语义一致，编译期报错（双通道一致；
            // 避免 VM 静默覆盖与 C 生成端重复 static 定义导致 gcc 失败的分歧）
            // class 方法跳过重复定义检查（方法注册到 class 方法表，不注册到全局符号表）
            if(strncmp(node->u.func_def.name, "_lambda_", 8) != 0 && !node->u.func_def.is_class_method) {
                ValueType ty;
                if(static_sym_get(node->u.func_def.name, &ty) && ty == VAL_FUNC) {
                    fprintf(stderr, "语义错误(第%d行)：函数 \"%s\" 重复定义\n",
                            node->line, node->u.func_def.name);
                    g_collect_err = 1;
                }
            }
            // 登记函数名（支持前向引用）；不深入函数体；class 方法也登记（用于方法调用查找）
            if(!node->u.func_def.is_class_method) {
                static_sym_put(node->u.func_def.name, VAL_FUNC);
            }
            break;
        }
        case AST_EXTERN_FUNC: {
            // FFI 外部函数声明：登记函数名（支持前向引用）
            static_sym_put(node->u.extern_func.name, VAL_FUNC);
            break;
        }
        case AST_BINOP:
            collect_top_level(node->u.bin.left);
            collect_top_level(node->u.bin.right);
            break;
        case AST_UNARY:
            collect_top_level(node->u.uny.child);
            break;
        case AST_CAST:
            collect_top_level(node->u.cast.child);
            break;
        case AST_INDEX:
            collect_top_level(node->u.index.arr);
            collect_top_level(node->u.index.idx);
            break;
        case AST_INDEX_ASSIGN:
            collect_top_level(node->u.index_assign.arr);
            collect_top_level(node->u.index_assign.idx);
            collect_top_level(node->u.index_assign.value);
            break;
        case AST_ARRAY_LIT:
            collect_top_level(node->u.array_lit.elems);
            break;
        case AST_MAP_LIT:
            collect_top_level(node->u.map_lit.entries);
            break;
        case AST_TRY:
            collect_top_level(node->u.trynode.body);
            collect_top_level(node->u.trynode.catch_body);
            collect_top_level(node->u.trynode.finally_body);
            break;
        case AST_THROW:
            collect_top_level(node->u.thrownode.expr);
            break;
        case AST_DESTRUCT:
            for(int i = 0; i < node->u.destruct.count; i++) {
                static_sym_put(node->u.destruct.names[i], VAL_NONE);
                if(g_global_vars_cnt >= g_global_vars_cap) {
                    int nc = g_global_vars_cap > 0 ? g_global_vars_cap * 2 : 64;
                    char** nt = (char**)realloc(g_global_vars, (size_t)nc * sizeof(char*));
                    if(!nt) { fprintf(stderr, "全局变量表扩容内存不足\n"); exit(EXIT_FAILURE); }
                    g_global_vars = nt; g_global_vars_cap = nc;
                }
                g_global_vars[g_global_vars_cnt++] = strdup(node->u.destruct.names[i]);
            }
            collect_top_level(node->u.destruct.rhs);
            break;
        case AST_SPREAD:
            collect_top_level(node->u.spread.expr);
            break;
        case AST_MAP_ENTRY:
            collect_top_level(node->u.map_entry.key);
            collect_top_level(node->u.map_entry.value);
            break;
        case AST_TERNARY:
            collect_top_level(node->u.ternary.cond);
            collect_top_level(node->u.ternary.true_expr);
            collect_top_level(node->u.ternary.false_expr);
            break;
        case AST_PRINT:
            collect_top_level(node->u.print.args);
            break;
        case AST_SEQ:
            collect_top_level(node->u.seq.first);
            collect_top_level(node->u.seq.second);
            break;
        case AST_BLOCK:
            collect_top_level(node->u.block.stmts);
            break;
        case AST_IF:
            collect_top_level(node->u.ifnode.cond);
            collect_top_level(node->u.ifnode.then_stmt);
            collect_top_level(node->u.ifnode.elif_chain);
            collect_top_level(node->u.ifnode.else_stmt);
            break;
        case AST_IF_CHAIN:
            collect_top_level(node->u.if_chain.cond);
            collect_top_level(node->u.if_chain.if_body);
            {
                AstNode* p = node->u.if_chain.elif_list;
                while (p) {
                    collect_top_level(p->u.elif.cond);
                    collect_top_level(p->u.elif.body);
                    p = p->u.elif.next;
                }
            }
            collect_top_level(node->u.if_chain.else_body);
            break;
        case AST_ELIF:
            collect_top_level(node->u.elif.cond);
            collect_top_level(node->u.elif.body);
            break;
        case AST_FOR:
            collect_top_level(node->u.for_node.init);
            collect_top_level(node->u.for_node.cond);
            collect_top_level(node->u.for_node.update);
            collect_top_level(node->u.for_node.body);
            break;
        case AST_WHILE:
            collect_top_level(node->u.while_node.cond);
            collect_top_level(node->u.while_node.body);
            break;
        case AST_DO_WHILE:
            collect_top_level(node->u.while_node.body);
            collect_top_level(node->u.while_node.cond);
            break;
        case AST_SWITCH:
            collect_top_level(node->u.sw.cond);
            {
                AstNode* cp = node->u.sw.cases;
                while (cp) {
                    if (cp->u.cs.const_val) collect_top_level(cp->u.cs.const_val);
                    collect_top_level(cp->u.cs.body);
                    cp = cp->u.cs.next;
                }
            }
            break;
        case AST_CASE:
            if (node->u.cs.const_val) collect_top_level(node->u.cs.const_val);
            collect_top_level(node->u.cs.body);
            break;
        case AST_RETURN:
            collect_top_level(node->u.ret.ret_val);
            break;
        case AST_CALL: {
            AstNode* a = node->u.call.args;
            if (a) {
                if (a->type != AST_SEQ) collect_top_level(a);
                else {
                    collect_top_level(a->u.seq.first);
                    collect_top_level(a->u.seq.second);
                }
            }
            break;
        }
        default:
            break;
    }
}

// 实参链表是左嵌套 AST_SEQ 链，递归展开逐个检查
static int typecheck_arg_count(AstNode* chain)
{
    if(!chain) return 0;
    if(chain->type == AST_SEQ) return typecheck_arg_count(chain->u.seq.first) + typecheck_arg_count(chain->u.seq.second);
    return 1;
}
static int typecheck_call_args(AstNode* args) {
    if (!args) return 0;
    if (args->type != AST_SEQ) {
        return typecheck_expr(args);
    }
    return typecheck_call_args(args->u.seq.first) | typecheck_call_args(args->u.seq.second);
}

int ast_typecheck(AstNode* node)
{
    static_sym_reset();
    func_depth = 0;
    g_collect_err = 0;
    if(!node) return 0;
    // 阶段1：顶层收集（函数名 + 全局变量），支持前向引用/函数体读全局
    collect_top_level(node);
    // 阶段2：全面检查（含函数体递归）
    int err = g_collect_err | typecheck_expr(node);
    // 阶段3：函数体内函数名引用被转成 AST_FUNCREF 的函数，重编译字节码
    if(!err) {
        for(int i = 0; i < g_recompile_cnt; i++)
            func_compile_recompile(g_recompile[i]);
    }
    return err;
}

// 实参个数：AST_SEQ 二叉链递归计数
static int count_args(AstNode* args)
{
    if(!args) return 0;
    if(args->type != AST_SEQ) return 1;
    return count_args(args->u.seq.first) + count_args(args->u.seq.second);
}


/*
 * 类型检查：函数调用
 * 返回错误标志（0=无错误，1=有错误）
 */
static int typecheck_call(AstNode* node)
{
    int err = 0;
            // 默认参数填充：如果实参不足，用函数定义中的默认值表达式填充
            {
                if(sym_has(node->u.call.name)) {
                Value fv = sym_get(node->u.call.name);
                if(fv.type == VAL_FUNC) {
                    RuntimeFunc* rf = (RuntimeFunc*)fv.v.func.func_obj;
                    if(interp_func_is_payload(rf)) {
                        int nargs = typecheck_arg_count(node->u.call.args);
                        int pcount = interp_func_param_cnt(rf);
                        // 从缺失的第一个参数开始，逐个填充默认值
                        for(int pi = nargs; pi < pcount; pi++) {
                            if(interp_func_param_has_default(rf, pi)) {
                                AstNode* dv = interp_func_param_default(rf, pi);
                                if(dv) {
                                    // 深拷贝默认值表达式，避免与函数定义共享节点导致重复释放
                                    AstNode* dv_copy = ast_clone_node(dv);
                                    node->u.call.args = ast_arg_append(node->u.call.args, dv_copy);
                                }
                            }
                        }
                    }
                }
                }
            }
            // 实参逐个检查（含嵌套调用）
            err |= typecheck_call_args(node->u.call.args);
            // 函数名：已定义函数 或 赋过函数值的变量 均可（与解释器一致）；
            // 内置函数白名单：len/type/input/range/substr（用户函数同名时用户优先）
            ValueType t;
            if(type_lookup(node->u.call.name) != NULL) {
                /* type 构造调用：参数个数 == 属性数（或单 map 原样）。
                   函数体先于 typecheck 被 ir 编译（yacc 动作 compile_func_from_ast），
                   构造展开会摘空 args——args 为空时跳过（已展开，运行时宽松处理） */
                int nargs = typecheck_arg_count(node->u.call.args);
                if(nargs > 0) {
                    int nprops = type_lookup(node->u.call.name)->nprops;
                    if(!(nargs == nprops || nargs == 1)) {
                        fprintf(stderr, "语义错误(第%d行)：类型构造参数个数错误：需要 %d 个（或单个 map），实际 %d 个\n",
                                node->line, nprops, nargs);
                    }
                }
            } else if(!static_sym_get(node->u.call.name, &t)) {
                static const struct { const char* name; int min; int max; } builtins[] = {
                    {"len", 1, 1}, {"type", 1, 1}, {"input", 0, 0}, {"range", 1, 3}, {"substr", 3, 3},
                    {"toupper", 1, 1}, {"tolower", 1, 1}, {"split", 2, 2}, {"del", 2, 2}, {"insert", 3, 3},
                    {"floor", 1, 1}, {"ceil", 1, 1}, {"abs", 1, 1}, {"sqrt", 1, 1},
                    {"max", 1, -1}, {"min", 1, -1}, {"join", 2, 2}, {"contains", 2, 2},
                    {"repeat", 2, 2}, {"replace", 3, 3}, {"sum", 1, 1}, {"avg", 1, 1},
                    {"format", 1, -1}, {"sort", 1, 1}, {"reverse", 1, 1},
                    {"map", 2, 2}, {"filter", 2, 2}, {"reduce", 3, 3},
                    {"strip", 1, 1}, {"startswith", 2, 2}, {"endswith", 2, 2},
                    {"read_file", 1, 1}, {"write_file", 2, 2}, {"file_exists", 1, 1},
                    {"keys", 1, 1}, {"values", 1, 1},
                    {"thread", 1, -1}, {"thread_join", 1, 1},
                    {"mutex", 0, 0}, {"rmutex", 0, 0}, {"rwlock", 0, 0}, {"spinlock", 0, 0},
                    {"lock", 1, 1}, {"unlock", 1, 1}, {"trylock", 1, 1},
                    {"rdlock", 1, 1}, {"wrlock", 1, 1},
                    {"tryrdlock", 1, 1}, {"trywrlock", 1, 1},
                    {"condvar", 0, 0}, {"cond_wait", 2, 2}, {"cond_wait_timeout", 3, 3}, {"cond_signal", 1, 1}, {"cond_broadcast", 1, 1},
                    {"threadlocal_get", 1, 1}, {"threadlocal_set", 2, 2},
                    {"get", 1, 3}, {"post", 1, 3}, {"put", 1, 3}, {"delete", 1, 3}, {"head", 1, 3}, {"patch", 1, 3},

                    {"add", 2, 3}, {"remove", 2, 2}, {"clear", 1, 1},
                    {"arr_get", 2, 2}, {"indexOf", 2, 2}, {"set", 3, 3}, {"first", 1, 1}, {"last", 1, 1}, {"has", 2, 2},
                    {"flat", 1, 2}, {"qs", 1, 2}, {"addAll", 2, 2}, {"bytes", 1, 2}, {"str", 1, 2},
                    {"json", 1, 2}, {"stringify", 1, 2},
                    {"encode", 1, 2}, {"decode", 1, 2},
                    {"encodeURL", 1, 1}, {"decodeURL", 1, 1},
                    {"md5", 1, 1}, {"encodeBase64", 1, 1}, {"decodeBase64", 1, 1},
                    {"regex_match", 2, 2}, {"regex_search", 2, 2}, {"regex_replace", 3, 3},
                    {"now", 0, 0}, {"timestamp", 0, 0}, {"timestamp_ms", 0, 0},
                    {"sleep", 1, 1}, {"date", 0, 0}, {"time", 0, 0}, {"datetime", 0, 0},
                    {"format_time", 1, 2},
                    {"debug", 1, 2}, {"info", 1, 2}, {"warn", 1, 2}, {"error", 1, 2}, {"fatal", 1, 2},
                    {"gc_count", 0, 0}, {"gc_bytes", 0, 0}, {"gc_collect", 0, 0}, {"gc_stw_ns", 0, 0}, {"next", 1, 1}, {"send", 2, 2}, {"receive", 0, 0}, {"close", 1, 1}, {"GenThrow", 2, 2}, {"chain", 2, 2}, {"zip", 2, 2}, {"skip", 2, 2}, {"take", 2, 2}, {"enumerate", 1, 1}, {"next", 1, 1},
                };
                int found = 0;
                int nbuiltins = (int)(sizeof(builtins) / sizeof(builtins[0]));
                for(int k = 0; k < nbuiltins; k++) {
                    if(strcmp(node->u.call.name, builtins[k].name) == 0) {
                        found = 1;
                        int nargs = count_args(node->u.call.args);
                        int bad = (nargs < builtins[k].min) ||
                                  (builtins[k].max >= 0 && nargs > builtins[k].max);
                        if(bad) {
                            if(builtins[k].max >= 0 && builtins[k].min == builtins[k].max)
                                fprintf(stderr,"语义错误(第%d行)：%s() 需要 %d 个实参（给了 %d 个）\n", node->line,
                                        builtins[k].name, builtins[k].min, nargs);
                            else if(builtins[k].max < 0)
                                fprintf(stderr,"语义错误(第%d行)：%s() 需要至少 %d 个实参（给了 %d 个）\n", node->line,
                                        builtins[k].name, builtins[k].min, nargs);
                            else
                                fprintf(stderr,"语义错误(第%d行)：%s() 需要 %d 到 %d 个实参（给了 %d 个）\n", node->line,
                                        builtins[k].name, builtins[k].min, builtins[k].max, nargs);
                            err = 1;
                        }
                        break;
                    }
                }
                if(!found) {
                    /* 方法调用（如 a.speak() 转换为 speak(a)）可能是 class 方法，
                       不在全局符号表中，运行时根据对象类型动态查找，所以编译期不报错 */
                    int nargs = typecheck_arg_count(node->u.call.args);
                    if(nargs == 0) {
                        /* 无参数的函数调用，不可能是方法调用，报错 */
                        fprintf(stderr,"语义错误(第%d行)：调用未定义函数 %s\n", node->line, node->u.call.name);
                        err = 1;
                    }
                    /* 有参数的函数调用，可能是方法调用，不报错，运行时查找 */
                }
            } else if(t != VAL_NONE && t != VAL_FUNC) {
                fprintf(stderr,"语义错误(第%d行)：%s 不是函数\n", node->line, node->u.call.name);
                err = 1;
            }
            node->val_type = VAL_NONE;
    return err;
}

int typecheck_expr(AstNode* node)
{
    if(!node) return 0;
    int err = 0;
    switch(node->type) {
        case AST_INT:
            node->val_type = VAL_INT;
            break;
        case AST_NUM:
            node->val_type = VAL_DOUBLE;
            break;
        case AST_BOOL:
            node->val_type = VAL_BOOL;
            break;
        case AST_NONE:
            node->val_type = VAL_NONE;
            break;
        case AST_STRING:
            node->val_type = VAL_STRING;
            break;
        case AST_CHAR:
            node->val_type = VAL_CHAR;
            break;
        case AST_VAR:{
            if(in_lambda && !is_lambda_param(node->u.varname) &&
               !is_global_var(node->u.varname) && !is_lambda_local(node->u.varname)) {
                // 引用外层函数局部变量 → 记录为当前 lambda 的捕获变量（运行时装箱）
                cap_add(node->u.varname);
            }
            ValueType t;
            if(static_sym_get(node->u.varname, &t)) {
                if(t == VAL_FUNC) {
                    // 函数名引用：就地转 AST_FUNCREF（函数作为值）
                    node->type = AST_FUNCREF;
                    node->val_type = VAL_FUNC;
                    // 若发生在函数体内，该函数的 parse 期字节码需重编译
                    if(g_cur_func_def) {
                        int dup = 0;
                        for(int i = 0; i < g_recompile_cnt; i++)
                            if(g_recompile[i] == g_cur_func_def) { dup = 1; break; }
                        if(!dup) {
                            if(g_recompile_cnt >= g_recompile_cap) {
                                int nc = g_recompile_cap > 0 ? g_recompile_cap * 2 : 16;
                                AstNode** nt = (AstNode**)realloc(g_recompile, (size_t)nc * sizeof(AstNode*));
                                if(!nt) { fprintf(stderr, "重编译表扩容内存不足\n"); exit(EXIT_FAILURE); }
                                g_recompile = nt; g_recompile_cap = nc;
                            }
                            g_recompile[g_recompile_cnt++] = g_cur_func_def;
                        }
                    }
                } else {
                    node->val_type = t;
                }
            } else {
                fprintf(stderr,"语义错误(第%d行)：使用未定义变量 %s\n", node->line, node->u.varname);
                node->val_type = VAL_NONE;
                err = 1;
            }
            break;
        }
        case AST_FUNCREF:
            node->val_type = VAL_FUNC;
            break;
        case AST_UNARY: {
            AstNode* kid = node->u.uny.child;
            BinOp op = node->u.uny.op;
            err |= typecheck_expr(kid);
            if(op == OP_PRE_INC || op == OP_POST_INC || op == OP_PRE_DEC || op == OP_POST_DEC) {
                if(kid->type != AST_VAR) {
                    fprintf(stderr,"语义错误：++/-- 的操作数必须是变量\n");
                    return -1;
                }
            }
            node->val_type = kid->val_type;
            break;
        }
        case AST_BINOP:{
            err |= typecheck_expr(node->u.bin.left);
            err |= typecheck_expr(node->u.bin.right);
            ValueType tl = node->u.bin.left->val_type;
            ValueType tr = node->u.bin.right->val_type;
            int left_unknown = (tl == VAL_NONE);
            int right_unknown = (tr == VAL_NONE);
            int left_is_str = (tl == VAL_STRING);
            int right_is_str = (tr == VAL_STRING);
            int left_is_num = (tl == VAL_INT || tl == VAL_DOUBLE || tl == VAL_CHAR || tl == VAL_BYTE);
            int right_is_num = (tr == VAL_INT || tr == VAL_DOUBLE || tr == VAL_CHAR || tr == VAL_BYTE);
            int left_is_bool = (tl == VAL_BOOL);
            int right_is_bool= (tr == VAL_BOOL);
            BinOp op = node->u.bin.op;
            if(op == OP_ADD) {
                if(left_unknown || right_unknown) {
                    node->val_type = VAL_NONE;
                } else if(left_is_str || right_is_str || left_is_bool || right_is_bool) {
                    node->val_type = VAL_STRING;
                } else if(left_is_num && right_is_num) {
                    if(tl == VAL_DOUBLE || tr == VAL_DOUBLE)
                        node->val_type = VAL_DOUBLE;
                    else
                        node->val_type = VAL_INT;
                } else {
                    fprintf(stderr,"语义错误：不支持 %s + %s\n", valtype_to_cstr(tl), valtype_to_cstr(tr));
                    err = 1;
                }
            } else if(op == OP_EQ || op == OP_NE) {
                if(left_unknown || right_unknown) {
                    node->val_type = VAL_BOOL;
                } else {
                    int ok = 0;
                    if(tl == tr) { ok = 1; }
                    if( (tl == VAL_CHAR && tr == VAL_INT) || (tl == VAL_INT && tr == VAL_CHAR) ||
                        (tl == VAL_BYTE && tr == VAL_INT) || (tl == VAL_INT && tr == VAL_BYTE) ||
                        (tl == VAL_CHAR && tr == VAL_BYTE) || (tl == VAL_BYTE && tr == VAL_CHAR) ) { ok = 1; }
                    if(!ok) {
                        fprintf(stderr,"语义错误：==/!= 两侧类型不一致 %s vs %s\n", valtype_to_cstr(tl), valtype_to_cstr(tr));
                        err = 1;
                    }
                }
                node->val_type = VAL_BOOL;
            } else if(op == OP_LOGIC_AND || op == OP_LOGIC_OR) {
                // 逻辑运算：任意类型按 truthy 判定，结果 bool
                node->val_type = VAL_BOOL;
            } else if(op == OP_IMPLEMENTS) {
                // implements 操作符：检查对象是否实现接口，结果 bool
                node->val_type = VAL_BOOL;
            } else if(op == OP_MOD) {
                if(!(left_unknown || right_unknown)) {
                    if(!(left_is_num && right_is_num)) {
                        fprintf(stderr,"语义错误：%% 只支持数值类型\n");
                        err = 1;
                    }
                }
                if(tl == VAL_DOUBLE || tr == VAL_DOUBLE)
                    node->val_type = VAL_DOUBLE;
                else
                    node->val_type = VAL_INT;
            } else {
                if(!(left_unknown || right_unknown)) {
                    if(!(left_is_num && right_is_num)) {
                        fprintf(stderr,"语义错误：运算符只支持数值类型\n");
                        err = 1;
                    }
                }
                if(tl == VAL_DOUBLE || tr == VAL_DOUBLE)
                    node->val_type = VAL_DOUBLE;
                else
                    node->val_type = VAL_INT;
            }
            break;
        }
        case AST_ASSIGN:
            err |= typecheck_expr(node->u.assign.expr);
            node->val_type = node->u.assign.expr->val_type;
            int assign_capture = 0;
            if(in_lambda) {
                const char* vn = node->u.assign.varname;
                ValueType st;
                /* 注意：必须在 static_sym_put 之前判断 in_static，否则本赋值刚 put 的名字
                 * 会被误判为外层变量。 */
                int in_static = static_sym_get(vn, &st);
                /* 判定：参数 / 已登记本层局部 / 全局 → 本 lambda 局部；
                 * 否则若名字已存在于外层作用域（外层函数局部）→ 捕获（引用语义）；
                 * 完全未出现 → 本 lambda 新局部。 */
                assign_capture = !is_lambda_param(vn) && !is_lambda_local(vn) &&
                                 !is_global_var(vn) && in_static;
            }
            static_sym_put(node->u.assign.varname, node->val_type);
            if(in_lambda) {
                const char* vn = node->u.assign.varname;
                if(!assign_capture) {
                    if(lambda_locals_cnt >= lambda_locals_cap) {
                        int nc = lambda_locals_cap > 0 ? lambda_locals_cap * 2 : 64;
                        char** nt = (char**)realloc(lambda_locals, (size_t)nc * sizeof(char*));
                        if(!nt) { fprintf(stderr, "lambda 局部表扩容内存不足\n"); exit(EXIT_FAILURE); }
                        lambda_locals = nt; lambda_locals_cap = nc;
                    }
                    lambda_locals[lambda_locals_cnt++] = strdup(vn);
                } else {
                    cap_add(vn);
                }
            }
            break;
        case AST_INDEX: {
            err |= typecheck_expr(node->u.index.arr);
            err |= typecheck_expr(node->u.index.idx);
            if(node->u.index.arr->val_type != VAL_ARRAY &&
               node->u.index.arr->val_type != VAL_STRING &&
               node->u.index.arr->val_type != VAL_MAP &&
               node->u.index.arr->val_type != VAL_NONE) {
                fprintf(stderr,"语义错误(第%d行)：下标访问的对象不是数组、字符串或字典\n", node->line);
                err = 1;
            }
            node->val_type = VAL_NONE;   // 元素类型不可静态追踪
            break;
        }
        case AST_INDEX_ASSIGN: {
            err |= typecheck_expr(node->u.index_assign.arr);
            err |= typecheck_expr(node->u.index_assign.idx);
            err |= typecheck_expr(node->u.index_assign.value);
            /* 只读属性：__mapname__ / __structname__ 编译期拦截（DOT 属性赋值与 "[" 下标赋值） */
            if(node->u.index_assign.idx->type == AST_STRING &&
               (strcmp(node->u.index_assign.idx->u.sval, "__mapname__") == 0 ||
                strcmp(node->u.index_assign.idx->u.sval, "__structname__") == 0)) {
                fprintf(stderr, "语义错误(第%d行)：只读属性 %s 不能赋值\n", node->line, node->u.index_assign.idx->u.sval);
                err = 1;
            }
            if(node->u.index_assign.arr->val_type != VAL_ARRAY &&
               node->u.index_assign.arr->val_type != VAL_MAP &&
               node->u.index_assign.arr->val_type != VAL_NONE) {
                fprintf(stderr,"语义错误(第%d行)：下标访问的对象不是数组或字典\n", node->line);
                err = 1;
            }
            node->val_type = node->u.index_assign.value->val_type;
            break;
        }
        case AST_ARRAY_LIT:
            err |= typecheck_expr(node->u.array_lit.elems);
            node->val_type = VAL_ARRAY;
            break;
        case AST_MAP_LIT:
            err |= typecheck_expr(node->u.map_lit.entries);
            node->val_type = VAL_MAP;
            break;
        case AST_MAP_ENTRY:
            err |= typecheck_expr(node->u.map_entry.key);
            err |= typecheck_expr(node->u.map_entry.value);
            node->val_type = VAL_MAP;
            break;
        case AST_TRY:
            err |= typecheck_expr(node->u.trynode.body);
            /* catch 变量先注册（作用域：catch 块内）再检查 catch 块 */
            if(node->u.trynode.catch_var) {
                static_sym_put(node->u.trynode.catch_var, VAL_NONE);
                err |= typecheck_expr(node->u.trynode.catch_body);
            }
            err |= typecheck_expr(node->u.trynode.finally_body);
            node->val_type = VAL_NONE;
            break;
        case AST_THROW:
            err |= typecheck_expr(node->u.thrownode.expr);
            node->val_type = VAL_NONE;
            break;
        case AST_DESTRUCT:
            err |= typecheck_expr(node->u.destruct.rhs);
            for(int i = 0; i < node->u.destruct.count; i++)
                static_sym_put(node->u.destruct.names[i], VAL_NONE);
            node->val_type = VAL_NONE;
            break;
        case AST_SPREAD:
            err |= typecheck_expr(node->u.spread.expr);
            node->val_type = VAL_NONE;
            break;
        case AST_PRINT:
            err |= typecheck_expr(node->u.print.args);
            node->val_type = VAL_NONE;
            break;
        case AST_SEQ:
            err |= typecheck_expr(node->u.seq.first);
            err |= typecheck_expr(node->u.seq.second);
            node->val_type = node->u.seq.second->val_type;
            break;
        case AST_IF:{
            err |= typecheck_expr(node->u.ifnode.cond);
            err |= typecheck_expr(node->u.ifnode.then_stmt);
            if(node->u.ifnode.elif_chain) {
                err |= typecheck_expr(node->u.ifnode.elif_chain);
            }
            if(node->u.ifnode.else_stmt) {
                err |= typecheck_expr(node->u.ifnode.else_stmt);
            }
            node->val_type = VAL_DOUBLE;
            break;
        }
        case AST_BLOCK:
            if(node->u.block.stmts) err |= typecheck_expr(node->u.block.stmts);
            node->val_type = VAL_DOUBLE;
            break;
        case AST_IF_CHAIN:
            err |= typecheck_expr(node->u.if_chain.cond);
            err |= typecheck_expr(node->u.if_chain.if_body);
            {
                AstNode* p = node->u.if_chain.elif_list;
                while(p) {
                    err |= typecheck_expr(p->u.elif.cond);
                    err |= typecheck_expr(p->u.elif.body);
                    p = p->u.elif.next;
                }
            }
            if(node->u.if_chain.else_body) err |= typecheck_expr(node->u.if_chain.else_body);
            node->val_type = VAL_DOUBLE;
            break;
        case AST_ELIF:
            err |= typecheck_expr(node->u.elif.cond);
            err |= typecheck_expr(node->u.elif.body);
            node->val_type = VAL_DOUBLE;
            break;
        case AST_WHILE:
            err |= typecheck_expr(node->u.while_node.cond);
            err |= typecheck_expr(node->u.while_node.body);
            node->val_type = VAL_DOUBLE;
            break;
        case AST_DO_WHILE:
            err |= typecheck_expr(node->u.while_node.body);
            err |= typecheck_expr(node->u.while_node.cond);
            node->val_type = VAL_DOUBLE;
            break;
        case AST_FOR:
            if(node->u.for_node.init) err |= typecheck_expr(node->u.for_node.init);
            if(node->u.for_node.cond) err |= typecheck_expr(node->u.for_node.cond);
            if(node->u.for_node.update) err |= typecheck_expr(node->u.for_node.update);
            err |= typecheck_expr(node->u.for_node.body);
            node->val_type = VAL_DOUBLE;
            break;
        case AST_CAST: {
            err |= typecheck_expr(node->u.cast.child);
            /* 容器泛型：<T>[..] / (T)[..] / <string,V>{..} → 逐元素/逐值强转，仍是容器 */
            if(node->u.cast.child->val_type == VAL_ARRAY || node->u.cast.child->val_type == VAL_MAP) {
                node->val_type = node->u.cast.child->val_type;
                break;
            }
            switch(node->u.cast.cast_type) {
                case CAST_INT:      node->val_type = VAL_INT; break;
                case CAST_DOUBLE:   node->val_type = VAL_DOUBLE; break;
                case CAST_CHAR:     node->val_type = VAL_CHAR; break;
                case CAST_BOOL:     node->val_type = VAL_BOOL; break;
                case CAST_STRING:   node->val_type = VAL_STRING; break;
                case CAST_ASCII:    node->val_type = VAL_INT; break;
                case CAST_BYTE:     node->val_type = VAL_BYTE; break;
                default:            node->val_type = VAL_INT;
            }
            break;
        }
        case AST_TERNARY: {
            err |= typecheck_expr(node->u.ternary.cond);
            err |= typecheck_expr(node->u.ternary.true_expr);
            err |= typecheck_expr(node->u.ternary.false_expr);
            ValueType t1 = node->u.ternary.true_expr->val_type;
            ValueType t2 = node->u.ternary.false_expr->val_type;
            if(t1 == VAL_STRING || t2 == VAL_STRING) {
                node->val_type = VAL_STRING;
            } else {
                node->val_type = t1;
            }
            break;
        }
        case AST_CALL: {
            err |= typecheck_call(node);
            break;
        }
        case AST_DYN_CALL: {
            // 调用链 f(1)(2)：callee 是任意表达式（函数值），动态语言不做静态函数性校验
            err |= typecheck_expr(node->u.dyn_call.callee);
            err |= typecheck_call_args(node->u.dyn_call.args);
            node->val_type = VAL_NONE;
            break;
        }
        case AST_RETURN: {
            if(node->u.ret.ret_val) {
                err |= typecheck_expr(node->u.ret.ret_val);
                node->val_type = node->u.ret.ret_val->val_type;
            } else {
                node->val_type = VAL_NONE;
            }
            break;
        }
        case AST_SWITCH: {
            err |= typecheck_expr(node->u.sw.cond);
            AstNode* cp = node->u.sw.cases;
            while(cp) {
                if(cp->u.cs.const_val) err |= typecheck_expr(cp->u.cs.const_val);
                if(cp->u.cs.bind_var) {
                    static_sym_put(cp->u.cs.bind_var, VAL_NONE);
                }
                if(cp->u.cs.guard) err |= typecheck_expr(cp->u.cs.guard);
                err |= typecheck_expr(cp->u.cs.body);
                cp = cp->u.cs.next;
            }
            node->val_type = VAL_NONE;
            break;
        }
        case AST_CASE: {
            if(node->u.cs.const_val) err |= typecheck_expr(node->u.cs.const_val);
            err |= typecheck_expr(node->u.cs.body);
            node->val_type = VAL_NONE;
            break;
        }
        case AST_BREAK:
        case AST_CONTINUE:
            node->val_type = VAL_NONE;
            break;
        case AST_FUNC_DEF: {
            // ...rest 位置检查
            AstNode* p = node->u.func_def.params;
            while(p) {
                if(p->u.param.is_ellipsis && p->u.param.next != NULL) {
                    fprintf(stderr,"语义错误：可变参数...必须放在参数列表最后\n");
                    err = 1;
                }
                p = p->u.param.next;
            }
            int is_lambda = strncmp(node->u.func_def.name, "_lambda_", 8) == 0;
            if(func_depth > 0 && !is_lambda) {
                // 嵌套具名函数：codegen 不支持，语义检查同样跳过（保持一致）
                node->val_type = VAL_FUNC;
                break;
            }
            func_depth++;
            AstNode* save_cur = g_cur_func_def;
            g_cur_func_def = node;
            sym_save();
            int save_in_lambda = in_lambda;
            AstNode* save_params = g_lambda_params;
            int save_lc = lambda_locals_cnt;
            if(is_lambda) { in_lambda = 1; g_lambda_params = node->u.func_def.params; lambda_locals_cnt = 0; cap_push(); }
            // 登记参数（覆盖同名全局实现遮蔽；类型动态 → VAL_NONE 占位）
            for(p = node->u.func_def.params; p; p = p->u.param.next) {
                static_sym_put(p->u.param.name, VAL_NONE);
            }
            err |= typecheck_expr(node->u.func_def.body);
            if(is_lambda) {
                in_lambda = save_in_lambda; g_lambda_params = save_params; lambda_locals_cnt = save_lc;
                int ncapt = cap_pop_record(node->u.func_def.name);
                // 该 lambda 捕获了外层局部变量：其所在外层函数体需重编译以发射 OPC_MKCLOSURE
                if(ncapt > 0 && save_cur) {
                    int dup = 0;
                    for(int i = 0; i < g_recompile_cnt; i++)
                        if(g_recompile[i] == save_cur) { dup = 1; break; }
                    if(!dup) {
                        if(g_recompile_cnt >= g_recompile_cap) {
                            int ncap2 = g_recompile_cap > 0 ? g_recompile_cap * 2 : 16;
                            AstNode** nt = (AstNode**)realloc(g_recompile, (size_t)ncap2 * sizeof(AstNode*));
                            if(!nt) { fprintf(stderr, "重编译表扩容内存不足\n"); exit(EXIT_FAILURE); }
                            g_recompile = nt; g_recompile_cap = ncap2;
                        }
                        g_recompile[g_recompile_cnt++] = save_cur;
                    }
                }
            }
            sym_restore();
            g_cur_func_def = save_cur;
            func_depth--;
            node->val_type = VAL_FUNC;
            break;
        }

        default: break;
    }
    return err;
}
