#include "ast_interp.h"
#include "lumyr_ffi.h"
#include "ast_runtime_sym.h"
#include "func_compile.h"
#include "stackframe.h"
#include "lumyr_types.h"
#include "ast_node_type.h"
#include "ast_types.h"
#include "lm_map.h"
#include "lm_class.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

// ===================== 运行时辅助函数 =====================
Value make_int(long long i)      { Value v; v.type = VAL_INT; v.v.i = i; return v; }
Value make_double(double d)      { Value v; v.type = VAL_DOUBLE; v.v.d = d; return v; }
Value make_bool(_Bool b)         { Value v; v.type = VAL_BOOL; v.v.b = b; return v; }
Value make_string(const char* s) { Value v; v.type = VAL_STRING; v.str_inline = 0; if(s == NULL) { v.v.s = NULL; return v; } size_t len = strlen(s); if(len <= LUMYR_SSO_MAX) { v.str_inline = 1; v.v.sso.len = (uint8_t)len; memcpy(v.v.sso.data, s, len); v.v.sso.data[len] = '\0'; } else { v.v.s = strdup(s); } return v; }
Value make_char(char ch)         { Value v; v.type = VAL_CHAR; v.v.c = ch; return v; }
Value make_nil(void)             { Value v; v.type = VAL_NONE; return v; }



// 布尔条件判断小helper，统一提取
_Bool value_to_bool(Value v)
{
    if(v.type == VAL_INT) return v.v.i != 0;
    if(v.type == VAL_DOUBLE) return v.v.d != 0.0;
    if(v.type == VAL_BOOL) return v.v.b;
    if(v.type == VAL_CHAR) return (unsigned char)v.v.c != 0;
    return 0;
}

// 未定义变量/函数：统一报错退出
static void runtime_undefined(const char* what, const char* name)
{
    fprintf(stderr, "Runtime Error: 未定义%s: %s\n", what, name);
    exit(EXIT_FAILURE);
}

// ===================== 实参链表工具 =====================
// ast_arg_append 用 ast_seq 左嵌套串联实参：seq(seq(a,b),c)
// 递归收集，保证 a,b,c 从左到右求值顺序
static void arg_list_count(AstNode* args, int* n)
{
    if(!args) return;
    if(args->type != AST_SEQ) { (*n)++; return; }
    arg_list_count(args->u.seq.first, n);
    arg_list_count(args->u.seq.second, n);
}

static void arg_list_collect(AstNode* args, Value* out, int* idx, EvalCtx* ctx, StackFrame* frame)
{
    if(!args) return;
    if(args->type != AST_SEQ) { out[(*idx)++] = ast_eval_ctx(args, ctx, frame); return; }
    arg_list_collect(args->u.seq.first, out, idx, ctx, frame);
    arg_list_collect(args->u.seq.second, out, idx, ctx, frame);
}

/* 顶层入口：栈上构造EvalCtx + 顶层栈帧，执行后销毁 */
Value ast_eval(AstNode* node)
{
    EvalCtx local_ctx = {0};
    StackFrame* top = stackframe_new(NULL);
    stackframe_set_shared(top);
    Value ret = ast_eval_ctx(node, &local_ctx, top);
    stackframe_destroy(top);
    return ret;
}

// ===================== 核心解释器：全部递归走 ast_eval_ctx，透传ctx与frame =====================

/*
 * 求值：二元运算
 */
static Value ast_eval_binop(AstNode* node, EvalCtx* ctx, StackFrame* frame)
{
            Value lv = ast_eval_ctx(node->u.bin.left, ctx, frame);
            Value rv = ast_eval_ctx(node->u.bin.right, ctx, frame);
            /* OP_IMPLEMENTS: obj implements Interface — 鸭子类型检查 */
            if(node->u.bin.op == OP_IMPLEMENTS) {
                if(rv.type != VAL_STRING) return make_bool(0);
                const char* iface_name = lumyr_str_cstr(&rv);
                InterfaceDef* idef = interface_lookup(iface_name);
                if(!idef) return make_bool(0);
                /* 检查对象是否有接口要求的所有方法（属性） */
                if(lv.type != VAL_MAP) return make_bool(0);
                for(int i = 0; i < idef->nmethods; i++) {
                    Value key = make_string(idef->methods[i].name);
                    if(!lumyr_map_has(lv, key)) return make_bool(0);
                }
                return make_bool(1);
            }
            double l = val_to_num(lv);
            double r = val_to_num(rv);

            switch(node->u.bin.op){
                case OP_ADD:{
                    int need_concat = (node->val_type == VAL_STRING);
                    if(node->val_type == VAL_NONE)
                    {
                        if(lv.type == VAL_STRING || rv.type == VAL_STRING || lv.type == VAL_BOOL || rv.type == VAL_BOOL)
                        {
                            need_concat = 1;
                        }
                    }
                    if(need_concat)
                    {
                        Value *llv = &lv;
                        Value *rrv = &rv;
                        char buf_l[64], buf_r[64];
                        const char *sl, *sr;

                        if(llv->type == VAL_INT)      { snprintf(buf_l,sizeof(buf_l),"%lld", llv->v.i); sl = buf_l; }
                        else if(llv->type == VAL_DOUBLE){ snprintf(buf_l,sizeof(buf_l),"%g", llv->v.d); sl = buf_l; }
                        else if(llv->type == VAL_BOOL) { sl = llv->v.b ? "true":"false"; }
                        else if(llv->type == VAL_STRING){ sl = lumyr_str_cstr(llv); }
                        else if(llv->type == VAL_CHAR) { snprintf(buf_l,sizeof(buf_l),"%c", llv->v.c); sl = buf_l; }
                        else sl = "";

                        if(rrv->type == VAL_INT)      { snprintf(buf_r,sizeof(buf_r),"%lld", rrv->v.i); sr = buf_r; }
                        else if(rrv->type == VAL_DOUBLE){ snprintf(buf_r,sizeof(buf_r),"%g", rrv->v.d); sr = buf_r; }
                        else if(rrv->type == VAL_BOOL) { sr = rrv->v.b ? "true":"false"; }
                        else if(rrv->type == VAL_STRING){ sr = lumyr_str_cstr(rrv); }
                        else if(rrv->type == VAL_CHAR) { snprintf(buf_r,sizeof(buf_r),"%c", rrv->v.c); sr = buf_r; }
                        else sr = "";

                        char* res = lumyr_concat(sl, sr);
                        return make_string(res);
                    }
                    else
                    {
                        double res = l + r;
                        return (lv.type == VAL_INT && rv.type == VAL_INT)
                            ? make_int((long long)res)
                            : make_double(res);
                    }
                }
                case OP_SUB:{
                    double res = l - r;
                    return (lv.type == VAL_INT && rv.type == VAL_INT)
                        ? make_int((long long)res)
                        : make_double(res);
                }
                case OP_MUL:{
                    double res = l * r;
                    return (lv.type == VAL_INT && rv.type == VAL_INT)
                        ? make_int((long long)res)
                        : make_double(res);
                }
                case OP_DIV:{
                    double res = l / r;
                    return make_double(res);
                }
                case OP_GT:  return make_bool(l > r);
                case OP_LT:  return make_bool(l < r);
                case OP_GE:  return make_bool(l >= r);
                case OP_LE:  return make_bool(l <= r);
                case OP_EQ:  return make_bool(value_equal(lv, rv));
                case OP_NE:  return make_bool(!value_equal(lv, rv));
                default:     return make_int(0);
            }
}

Value ast_eval_ctx(AstNode* node, EvalCtx* ctx, StackFrame* frame)
{
    if(!node) return make_nil();
    switch(node->type) {
        case AST_INT:
            return make_int(node->u.inum);
        case AST_NUM:
            return make_double(node->u.num);
        case AST_BOOL:
            return make_bool(node->u.bval);
        case AST_STRING:
            return make_string(node->u.sval);
        case AST_CHAR:
            return make_char(node->u.ch);

        // ---- 变量读：栈帧链查找 ----
        case AST_VAR: {
            _Bool fnd = 0;
            Value vv = stackframe_get(frame, node->u.varname, &fnd);
            if(!fnd) runtime_undefined("变量", node->u.varname);
            return vv;
        }

        case AST_BREAK:
            ctx->hit_break = 1;
            return make_nil();

        case AST_CONTINUE:
            ctx->hit_continue = 1;
            return make_nil();

        // ---- return：先求值再置标志；ret_val 克隆一份脱离栈帧生命周期 ----
        case AST_RETURN: {
            Value rv = node->u.ret.ret_val
                ? ast_eval_ctx(node->u.ret.ret_val, ctx, frame)
                : make_nil();
            ctx->hit_return = 1;
            ctx->ret_val = val_clone(&rv);   // 字符串/数组深拷贝；函数引用浅拷贝
            return rv;                       // 语句上下文，返回值不参与资源管理
        }

        case AST_UNARY:
        {
            AstNode* kid = node->u.uny.child;
            BinOp op = node->u.uny.op;
            switch(op)
            {
                case OP_POST_INC:
                case OP_PRE_INC:
                case OP_POST_DEC:
                case OP_PRE_DEC:
                {
                    const char* vname = kid->u.varname;
                    _Bool fnd = 0;
                    Value __old = stackframe_get(frame, vname, &fnd);
                    if(!fnd) runtime_undefined("变量", vname);
                    switch(op)
                    {
                        case OP_POST_INC: { Value __v = lumyr_post_inc(&__old); stackframe_bind(frame, vname, __old); return __v; }
                        case OP_PRE_INC:  { Value __v = lumyr_pre_inc(&__old);  stackframe_bind(frame, vname, __old); return __v; }
                        case OP_POST_DEC: { Value __v = lumyr_post_dec(&__old); stackframe_bind(frame, vname, __old); return __v; }
                        case OP_PRE_DEC:  { Value __v = lumyr_pre_dec(&__old);  stackframe_bind(frame, vname, __old); return __v; }
                        default: return make_int(0);
                    }
                }
                case OP_UNARY_PLUS:
                {
                    Value sub = ast_eval_ctx(kid, ctx, frame);
                    return sub;
                }
                case OP_UNARY_MINUS:
                {
                    Value sub = ast_eval_ctx(kid, ctx, frame);
                    double num = val_to_num(sub);
                    return make_double(-num);
                }
                default:
                    fprintf(stderr,"ast_eval_ctx:未知一元运算符\n");
                    return make_int(0);
            }
        }

        case AST_BINOP:
            return ast_eval_binop(node, ctx, frame);

        // ---- 变量写：赋值语义（链上找到更新，找不到当前帧新建）----
        case AST_ASSIGN:{
            Value v = ast_eval_ctx(node->u.assign.expr, ctx, frame);
            stackframe_set(frame, node->u.assign.varname, v);
            return v;
        }

        case AST_PRINT:{
            /* 多参数打印：递归收集 args 列表（左嵌套 AST_SEQ: seq(seq(a,b),c)），依次打印 */
            int cnt = 0;
            arg_list_count(node->u.print.args, &cnt);
            if(cnt > 0) {
                Value* vals = malloc(sizeof(Value) * (size_t)cnt);
                int idx = 0;
                arg_list_collect(node->u.print.args, vals, &idx, ctx, frame);
                for(int i = 0; i < cnt; i++) {
                    if(i > 0) printf(" ");  /* 参数之间用空格分隔 */
                    Value v = vals[i];
                    if(v.type == VAL_INT)
                        printf("%lld", v.v.i);
                    else if(v.type == VAL_CHAR)
                        printf("%c", v.v.c);
                    else if(v.type == VAL_BOOL)
                        printf("%s", v.v.b ? "true":"false");
                    else if(v.type == VAL_STRING)
                        printf("%s", lumyr_str_cstr(&v));
                    else if(v.type == VAL_DOUBLE)
                        printf("%g", v.v.d);
                    else if(v.type == VAL_NONE)
                        printf("null");
                    else
                        printf("%g", v.v.d);
                }
                free(vals);
            }
            printf("\n");
            return make_nil();
        }

        case AST_SEQ:{
            ast_eval_ctx(node->u.seq.first, ctx, frame);
            /* 遇到break/continue/return直接停止执行后续seq */
            if(ctx->hit_break || ctx->hit_continue || ctx->hit_return) {
                return make_nil();
            }
            return ast_eval_ctx(node->u.seq.second, ctx, frame);
        }

        case AST_IF:{
            Value cond_val = ast_eval_ctx(node->u.ifnode.cond, ctx, frame);
            _Bool cond = value_to_bool(cond_val);
            if(cond) {
                return ast_eval_ctx(node->u.ifnode.then_stmt, ctx, frame);
            }
            if(node->u.ifnode.elif_chain != NULL) {
                return ast_eval_ctx(node->u.ifnode.elif_chain, ctx, frame);
            }
            if(node->u.ifnode.else_stmt != NULL) {
                return ast_eval_ctx(node->u.ifnode.else_stmt, ctx, frame);
            }
            return make_double(0);
        }

        case AST_BLOCK:
        {
            Value ret = make_int(0);
            if (node->u.block.stmts != NULL) {
                ret = ast_eval_ctx(node->u.block.stmts, ctx, frame);
            }
            return ret;
        }

        case AST_IF_CHAIN:
        {
            Value ret = make_int(0);
            Value cond_val = ast_eval_ctx(node->u.if_chain.cond, ctx, frame);
            _Bool cond = value_to_bool(cond_val);

            if (cond) {
                ret = ast_eval_ctx(node->u.if_chain.if_body, ctx, frame);
            } else {
                AstNode* p = node->u.if_chain.elif_list;
                int matched = 0;
                while(p != NULL) {
                    Value c = ast_eval_ctx(p->u.elif.cond, ctx, frame);
                    _Bool bc = value_to_bool(c);
                    if(bc) {
                        ret = ast_eval_ctx(p->u.elif.body, ctx, frame);
                        matched = 1;
                        break;
                    }
                    p = p->u.elif.next;
                }
                if(!matched && node->u.if_chain.else_body != NULL) {
                    ret = ast_eval_ctx(node->u.if_chain.else_body, ctx, frame);
                }
            }
            return ret;
        }

        case AST_WHILE: {
            ctx->hit_break = 0;
            ctx->hit_continue = 0;
            for(;;) {
                Value cv = ast_eval_ctx(node->u.while_node.cond, ctx, frame);
                _Bool cond = value_to_bool(cv);
                if(!cond) break;

                ast_eval_ctx(node->u.while_node.body, ctx, frame);

                if(ctx->hit_return) break;      // 函数return：退出循环，flag交给函数入口消费
                if(ctx->hit_break) {
                    ctx->hit_break = 0;
                    break;
                }
                if(ctx->hit_continue) {
                    ctx->hit_continue = 0;
                    continue;
                }
            }
            return make_int(0);
        }
        case AST_DO_WHILE: {
            ctx->hit_break = 0;
            ctx->hit_continue = 0;
            for(;;) {
                ast_eval_ctx(node->u.while_node.body, ctx, frame);

                if(ctx->hit_return) break;
                if(ctx->hit_break) {
                    ctx->hit_break = 0;
                    break;
                }
                if(ctx->hit_continue) {
                    ctx->hit_continue = 0;
                }

                Value cv = ast_eval_ctx(node->u.while_node.cond, ctx, frame);
                _Bool cond = value_to_bool(cv);
                if(!cond) break;
            }
            return make_int(0);
        }

        case AST_FOR: {
            ctx->hit_break = 0;
            ctx->hit_continue = 0;
            if(node->u.for_node.init) {
                ast_eval_ctx(node->u.for_node.init, ctx, frame);
            }
            for(;;) {
                int ok = 1;
                if(node->u.for_node.cond) {
                    Value cv = ast_eval_ctx(node->u.for_node.cond, ctx, frame);
                    _Bool cond = value_to_bool(cv);
                    if(!cond) ok = 0;
                }
                if(!ok) break;

                ast_eval_ctx(node->u.for_node.body, ctx, frame);

                if(ctx->hit_return) break;
                if(ctx->hit_break) {
                    ctx->hit_break = 0;
                    break;
                }
                if(ctx->hit_continue) {
                    ctx->hit_continue = 0;
                }

                if(node->u.for_node.update) {
                    ast_eval_ctx(node->u.for_node.update, ctx, frame);
                }
            }
            return make_int(0);
        }

        case AST_SWITCH: {
            // 简易switch实现，无fallthrough，遇到break/return退出
            ctx->hit_break = 0;
            Value sw_val = ast_eval_ctx(node->u.sw.cond, ctx, frame);
            AstNode* pcase = node->u.sw.cases;
            int matched = 0;
            while(pcase)
            {
                if(pcase->type != AST_CASE) break;
                int is_hit = 0;
                if(pcase->u.cs.is_default)
                {
                    is_hit = 1;
                }
                else
                {
                    Value cval = ast_eval_ctx(pcase->u.cs.const_val, ctx, frame);
                    if(value_equal(sw_val, cval)){
                        is_hit = 1;
                    }
                }
                if(is_hit)
                {
                    matched = 1;
                    ast_eval_ctx(pcase->u.cs.body, ctx, frame);
                    if(ctx->hit_return) break;      // return：退出switch，flag交给函数入口消费
                    if(ctx->hit_break)
                    {
                        ctx->hit_break = 0;
                        break;
                    }
                }
                pcase = pcase->u.cs.next;
            }
            (void)matched;
            return make_int(0);
        }

        case AST_CASE:
            // case节点只被switch遍历，不单独eval
            return make_nil();

        case AST_CAST: {
            Value subv = ast_eval_ctx(node->u.cast.child, ctx, frame);
            switch(node->u.cast.cast_type) {
                case CAST_INT:
                {
                    long long iv;
                    if(subv.type == VAL_INT) iv = subv.v.i;
                    else if(subv.type == VAL_DOUBLE) iv = (long long)subv.v.d;
                    else if(subv.type == VAL_CHAR) iv = (unsigned char)subv.v.c;
                    else if(subv.type == VAL_BOOL) iv = subv.v.b ? 1 : 0;
                    else if(subv.type == VAL_STRING)
                    {
                        char *endp;
                        iv = strtoll(lumyr_str_cstr(&subv), &endp, 10);
                    }
                    else iv = 0;
                    return make_int(iv);
                }
                case CAST_DOUBLE: {
                    double dv;
                    if(subv.type == VAL_INT) dv = (double)subv.v.i;
                    else if(subv.type == VAL_DOUBLE) dv = subv.v.d;
                    else if(subv.type == VAL_CHAR) dv = (unsigned char)subv.v.c;
                    else if(subv.type == VAL_BOOL) dv = subv.v.b ? 1.0 : 0.0;
                    else dv = 0.0;
                    return make_double(dv);
                }
                case CAST_CHAR: {
                    char cv;
                    if(subv.type == VAL_INT) cv = (char)(subv.v.i & 0xFF);
                    else if(subv.type == VAL_DOUBLE) cv = (char)((long long)subv.v.d &0xFF);
                    else if(subv.type == VAL_CHAR) cv = subv.v.c;
                    else if(subv.type == VAL_BOOL) cv = subv.v.b ? 1 : 0;
                    else cv = 0;
                    return make_char(cv);
                }
                case CAST_BOOL: {
                    int b = 0;
                    if(subv.type == VAL_INT) b = (subv.v.i !=0);
                    else if(subv.type == VAL_DOUBLE) b = (subv.v.d != 0.0);
                    else if(subv.type == VAL_CHAR) b = ((unsigned char)subv.v.c !=0);
                    else if(subv.type == VAL_BOOL) b = subv.v.b;
                    return make_bool(b);
                }
                case CAST_STRING: {
                    char buf[256];
                    if(subv.type == VAL_INT) snprintf(buf, sizeof(buf), "%lld", subv.v.i);
                    else if(subv.type == VAL_DOUBLE) snprintf(buf, sizeof(buf), "%g", subv.v.d);
                    else if(subv.type == VAL_CHAR) snprintf(buf, sizeof(buf), "%c", subv.v.c);
                    else if(subv.type == VAL_BOOL) strcpy(buf, subv.v.b ? "true":"false");
                    else if(subv.type == VAL_STRING) return make_string(lumyr_str_cstr(&subv));
                    else strcpy(buf,"");
                    return make_string(buf);
                }
                case CAST_ASCII: {
                    char cv;
                    if(subv.type == VAL_INT)        cv = (char)(subv.v.i & 0xFF);
                    else if(subv.type == VAL_DOUBLE) cv = (char)((long long)subv.v.d &0xFF);
                    else if(subv.type == VAL_CHAR)  cv = subv.v.c;
                    else if(subv.type == VAL_BOOL)  cv = subv.v.b ? 1 : 0;
                    else cv = 0;
                    long long av = (unsigned char)cv;
                    return make_int(av);
                }
                case CAST_INT8:   return make_int((long long)(int8_t)subv.v.i);
                case CAST_INT16:  return make_int((long long)(int16_t)subv.v.i);
                case CAST_INT32:  return make_int((long long)(int32_t)subv.v.i);
                case CAST_INT64:  return make_int((long long)(int64_t)subv.v.i);
                case CAST_UINT8:  return make_int((long long)(uint8_t)(unsigned long long)subv.v.i);
                case CAST_UINT16: return make_int((long long)(uint16_t)(unsigned long long)subv.v.i);
                case CAST_UINT32: return make_int((long long)(uint32_t)(unsigned long long)subv.v.i);
                case CAST_UINT64: return make_int((long long)(uint64_t)(unsigned long long)subv.v.i);
                case CAST_LONG: case CAST_LONGLONG: return make_int(subv.v.i);
                case CAST_FLOAT: return make_double((double)(float)(subv.type == VAL_DOUBLE ? subv.v.d : (double)subv.v.i));
                case CAST_ULONG: return make_int((long long)(unsigned long)(unsigned long long)subv.v.i);
                case CAST_UCHAR: return make_int((long long)(unsigned char)(unsigned long long)subv.v.i);
                case CAST_SHORT: return make_int((long long)(short)subv.v.i);
                case CAST_USHORT: return make_int((long long)(unsigned short)(unsigned long long)subv.v.i);
                case CAST_SIZE_T: return make_int((long long)(size_t)(unsigned long long)subv.v.i);
                case CAST_SSIZE_T: return make_int((long long)(ssize_t)subv.v.i);
                case CAST_LONG_DOUBLE: return make_double((double)(long double)(subv.type == VAL_DOUBLE ? subv.v.d : (double)subv.v.i));
                case CAST_PTR: return make_int(subv.v.i);
                case CAST_VOID: return make_nil();
                default: {
                    return make_int(0);
                }
            }
        }

        case AST_TYPE_ANNOTATION: {
            /* 类型标注 <type>expr：给值打类型标记（等价 C 的类型声明）
               基础版本：按标注类型转换值，整数统一 VAL_INT，浮点统一 VAL_DOUBLE
               后续扩展：变量级类型标记传播，编译期零开销优化 */
            Value subv = ast_eval_ctx(node->u.type_annotation.expr, ctx, frame);
            int ct = node->u.type_annotation.cast_type;
            /* 浮点类型标注 */
            if(ct == CAST_DOUBLE || ct == CAST_FLOAT || ct == CAST_LONG_DOUBLE) {
                if(subv.type == VAL_DOUBLE) return subv;
                if(subv.type == VAL_INT) return make_double((double)subv.v.i);
                return make_double(0);
            }
            /* void 标注 */
            if(ct == CAST_VOID) return make_nil();
            /* 整数类型标注（统一 VAL_INT，值做 C 风格截断/扩展） */
            {
                long long iv;
                if(subv.type == VAL_INT) iv = subv.v.i;
                else if(subv.type == VAL_DOUBLE) iv = (long long)subv.v.d;
                else if(subv.type == VAL_CHAR) iv = (unsigned char)subv.v.c;
                else if(subv.type == VAL_BOOL) iv = subv.v.b ? 1 : 0;
                else iv = 0;
                /* 按标注类型做 C 风格截断/扩展 */
                switch(ct) {
                    case CAST_INT8: iv = (long long)(int8_t)iv; break;
                    case CAST_INT16: case CAST_SHORT: iv = (long long)(int16_t)iv; break;
                    case CAST_INT32: case CAST_INT: iv = (long long)(int32_t)iv; break;
                    case CAST_UINT8: case CAST_UCHAR: case CAST_BYTE: iv = (long long)(uint8_t)(unsigned long long)iv; break;
                    case CAST_UINT16: case CAST_USHORT: iv = (long long)(uint16_t)(unsigned long long)iv; break;
                    case CAST_UINT32: iv = (long long)(uint32_t)(unsigned long long)iv; break;
                    case CAST_CHAR: iv = (long long)(unsigned char)iv; break;
                    case CAST_BOOL: iv = iv ? 1 : 0; break;
                    /* int64/long/long long/uint64/ulong/size_t/ssize_t/ptr：不截断 */
                    default: break;
                }
                return make_int(iv);
            }
        }

        case AST_TERNARY:
        {
            Value cv = ast_eval_ctx(node->u.ternary.cond, ctx, frame);
            _Bool cond = value_to_bool(cv);
            if(cond) {
                return ast_eval_ctx(node->u.ternary.true_expr, ctx, frame);
            } else {
                return ast_eval_ctx(node->u.ternary.false_expr, ctx, frame);
            }
        }

        // ---- 函数定义：注册进当前栈帧（parse期yacc已注册全局符号表，双保险） ----
        case AST_FUNC_DEF:
        {
            RuntimeFunc* rf = compile_func_from_ast(node);
            Value fv;
            fv.type = VAL_FUNC;
            fv.v.func.func_obj = rf;
            fv.v.func.ffi_func = NULL;
            fv.v.func.is_ffi = 0;
            stackframe_set(frame, node->u.func_def.name, fv);
            return fv;
        }
        case AST_EXTERN_FUNC:
        {
            /* FFI 外部函数声明：创建 FFIFunc 对象，包装成 Value 注册到符号表 */
            FFIType ret_type = lumyr_ffi_type_from_name(node->u.extern_func.ret_type_name);
            int param_count = 0;
            AstNode* p = node->u.extern_func.params;
            while(p) { param_count++; p = p->u.param.next; }
            FFIType* param_types = NULL;
            if(param_count > 0) {
                param_types = (FFIType*)malloc((size_t)param_count * sizeof(FFIType));
                p = node->u.extern_func.params;
                for(int i = 0; i < param_count && p; i++) {
                    param_types[i] = lumyr_ffi_type_from_name(p->u.param.constraint);
                    p = p->u.param.next;
                }
            }
            FFIFunc* ffi = lumyr_ffi_func_create(node->u.extern_func.name,
                                                    node->u.extern_func.libname,
                                                    ret_type, param_types, param_count);
            free(param_types);
            Value fv;
            fv.type = VAL_FUNC;
            fv.v.func.func_obj = NULL;
            fv.v.func.ffi_func = ffi;
            fv.v.func.is_ffi = 1;
            sym_set(node->u.extern_func.name, fv);
            return make_nil();
        }

        // ---- 函数调用：新建栈帧 → 参数绑定 → 调用entry → 销毁栈帧 ----
        case AST_CALL: {
            char* fname = node->u.call.name;

            // 特殊处理：lumyr_interface_cast 内置函数（接口类型转换）
            if(strcmp(fname, "lumyr_interface_cast") == 0) {
                int argc = 0;
                arg_list_count(node->u.call.args, &argc);
                Value* args = NULL;
                if(argc > 0) {
                    args = (Value*)malloc(sizeof(Value) * (size_t)argc);
                    int idx = 0;
                    arg_list_collect(node->u.call.args, args, &idx, ctx, frame);
                }
                const char* iface_name = (argc >= 2 && args[1].type == VAL_STRING) ? lumyr_str_cstr(&args[1]) : "";
                Value ret = lumyr_interface_cast(args[0], iface_name);
                free(args);
                return ret;
            }

            // 1. 查函数：先查栈帧链（求值时注册的），再查全局符号表（parse期yacc注册的）
            Value func_val;
            _Bool fnd = 0;
            Value gv = stackframe_get(frame, fname, &fnd);
            if(fnd && gv.type == VAL_FUNC) {
                func_val = gv;
            } else if(sym_has(fname)) {
                func_val = sym_get(fname);
            } else {
                runtime_undefined("函数", fname);
            }
            if(func_val.type != VAL_FUNC) {
                fprintf(stderr, "Runtime Error: 尝试调用非函数: %s\n", fname);
                exit(EXIT_FAILURE);
            }
            /* FFI 外部函数调用 */
            if(func_val.v.func.is_ffi && func_val.v.func.ffi_func) {
                int argc = 0;
                arg_list_count(node->u.call.args, &argc);
                Value* args = NULL;
                if(argc > 0) {
                    args = (Value*)malloc(sizeof(Value) * (size_t)argc);
                    int idx = 0;
                    arg_list_collect(node->u.call.args, args, &idx, ctx, frame);
                }
                Value ret = lumyr_ffi_call(func_val.v.func.ffi_func, args, argc);
                free(args);
                return ret;
            }
            RuntimeFunc* rf = func_val.v.func.func_obj;

            // 2. 求值实参（左嵌套AST_SEQ链，递归收集保持从左到右顺序）
            int arg_count = 0;
            arg_list_count(node->u.call.args, &arg_count);
            Value* eval_args = NULL;
            if(arg_count > 0) {
                eval_args = (Value*)malloc(sizeof(Value) * arg_count);
                int ai = 0;
                arg_list_collect(node->u.call.args, eval_args, &ai, ctx, frame);
            }

            // 3. 新建栈帧（调用者帧为parent）
            StackFrame* callee = stackframe_new(frame);

            // 4. 参数绑定（解释器payload：普通参数按位绑定，缺省补nil；可变参数打包成数组）
            if(interp_func_is_payload(rf)) {
                int pcnt = interp_func_param_cnt(rf);
                int i = 0;
                for(; i < pcnt; i++) {
                    const char* pname = interp_func_param_name(rf, i);
                    Value bound = (i < arg_count) ? val_clone(&eval_args[i]) : val_none();
                    stackframe_bind(callee, pname, bound);
                }
                if(interp_func_has_variadic(rf)) {
                    const char* vname = interp_func_param_name(rf, pcnt);
                    int rest = arg_count - i;
                    if(rest < 0) rest = 0;
                    Value arr = val_array(rest);
                    for(int k = 0; k < rest; k++) {
                        arr.v.array->items[k] = val_clone(&eval_args[i + k]);
                    }
                    stackframe_bind(callee, vname, arr);
                }
            }

            // 5. 调用 entry：
            //    - 设置当前被调函数（解释器entry据此查payload）
            //    - 保存/复位break/continue（函数内部控制流不外泄）
            //    - return由entry消费：hit_return清0、ret_val归还
            RuntimeFunc* prev_rf = interp_set_current_rf(rf);
            int saved_break = ctx->hit_break;
            int saved_cont = ctx->hit_continue;
            ctx->hit_break = 0;
            ctx->hit_continue = 0;
            Value ret = rf->entry(arg_count, eval_args, ctx, callee);
            ctx->hit_break = saved_break;
            ctx->hit_continue = saved_cont;
            interp_set_current_rf(prev_rf);

            // 6. 销毁栈帧（返回值已在entry侧脱离帧生命周期）
            stackframe_destroy(callee);

            // 7. 释放实参缓冲区。
            // 注意：不val_destroy各Value——实参可能是帧内字符串/数组的浅拷贝，
            // 销毁会误伤所属栈帧导致双释放；新鲜字符串实参的少量泄漏可接受。
            free(eval_args);

            return ret;
        }

    default:
        return make_double(0);
    }
}
