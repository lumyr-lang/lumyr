// tmpl.c —— 字符串模板（"a={x}" 内插）拆解 + 内插表达式小解析器
//
// 设计：模板字符串在 parser 的 primary 动作里交给 maybe_template()。
// 拆段规则：
//   {expr}  → 占位 "{}"，expr 用内插表达式解析器解析为 AST（复用 ast_* 构造）
//   {{      → 字面 '{'（文本保留 "{{"，format 转义一致）
//   }       → 文本保留（format 报"未配对的 '}'"）
// 最后组 format(fmt, expr...) 调用 —— format 已双通道，模板天然一致。
//
// 内插表达式解析器：手写词法 + 递归下降（复用 yacc 的运算符优先级），
// 避免 bison 不可重入状态下的子解析。支持：数字/ID/字符串/char/bool/null/
// 数组字面量/一元/二元/比较/逻辑/三元/调用/下标/括号/强转。

#include "ast/ast.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

extern int yylineno;

// ---------------- 内插表达式词法 ----------------

typedef enum {
    TT_EOF, TT_ID, TT_INT, TT_FNUM, TT_STR, TT_CHAR,
    TT_TRUE, TT_FALSE, TT_NULL,
    TT_LPAREN, TT_RPAREN, TT_LBRACKET, TT_RBRACKET, TT_COMMA, TT_DOT,
    TT_QMARK, TT_COLON,
    TT_PLUS, TT_MINUS, TT_STAR, TT_SLASH, TT_PERCENT,
    TT_EQ, TT_NE, TT_LT, TT_GT, TT_LE, TT_GE,
    TT_AND, TT_OR, TT_NOT,
    TT_CAST,            // (int) (double) 等强转关键字
} TpTokType;

typedef struct {
    TpTokType type;
    char* text;          // ID / 字符串内容 / 强转类型名（动态，无长度上限）
    int text_cap;        // text 容量
    long long ival;
    double fval;
    char ch;
} TpTok;

// token 文本按需扩容
static void tok_ensure(TpTok* t, int need)
{
    if(need <= t->text_cap) return;
    int nc = t->text_cap > 0 ? t->text_cap * 2 : 64;
    while(nc < need) nc *= 2;
    char* nw = (char*)realloc(t->text, (size_t)nc);
    if(!nw) { fprintf(stderr, "模板解析：token 文本扩容内存不足\n"); exit(EXIT_FAILURE); }
    t->text = nw;
    t->text_cap = nc;
}

typedef struct {
    const char* p;
    const char* end;
    TpTok look;
    int have_look;
} TpParser;

static void tp_err(const char* msg)
{
    fprintf(stderr, "语义错误(第%d行)：模板字符串内插表达式错误：%s\n", yylineno, msg);
    exit(EXIT_FAILURE);
}

// 跳过字符串/字符字面量（含 \. 转义）；用于模板拆段的 } 配对扫描
static void skip_quoted(const char** pp, const char* end)
{
    const char* p = *pp;
    p++;  // 开引号
    while(p < end && *p != '\0') {
        if(*p == '\\' && p + 1 < end) { p += 2; continue; }
        if(*p == '"' || *p == '\'') { p++; break; }
        p++;
    }
    *pp = p;
}

static void tp_next_tok(TpParser* tp, TpTok* t)
{
    const char* p = tp->p;
    while(p < tp->end && isspace((unsigned char)*p)) p++;
    if(p >= tp->end || *p == '\0') { t->type = TT_EOF; return; }
    char c = *p;
    // 转义引号：\" 或 \' 表示字符串/字符边界（模板源码中的转义保留原样）
    if(c == '\\' && p + 1 < tp->end && (p[1] == '"' || p[1] == '\'')) {
        tp->p = p + 1;
        tp_next_tok(tp, t);
        return;
    }
    // 数字
    if(isdigit((unsigned char)c) || (c == '.' && p + 1 < tp->end && isdigit((unsigned char)p[1]))) {
        char* endp;
        long long iv = strtoll(p, &endp, 10);
        if(*endp == '.' || *endp == 'e' || *endp == 'E') {
            t->fval = strtod(p, &endp);
            t->type = TT_FNUM;
        } else {
            t->ival = iv;
            t->type = TT_INT;
        }
        tp->p = endp;
        return;
    }
    // ID / 关键字
    if(isalpha((unsigned char)c) || c == '_') {
        int n = 0;
        while(p < tp->end && (isalnum((unsigned char)*p) || *p == '_')) {
            tok_ensure(t, n + 2);
            t->text[n++] = *p++;
        }
        tok_ensure(t, n + 1);
        t->text[n] = '\0';
        tp->p = p;
        if(strcmp(t->text, "true") == 0) t->type = TT_TRUE;
        else if(strcmp(t->text, "false") == 0) t->type = TT_FALSE;
        else if(strcmp(t->text, "null") == 0) t->type = TT_NULL;
        else if(strcmp(t->text, "int") == 0 || strcmp(t->text, "double") == 0 ||
                strcmp(t->text, "string") == 0 || strcmp(t->text, "bool") == 0 ||
                strcmp(t->text, "char") == 0 || strcmp(t->text, "ASCII") == 0)
            t->type = TT_CAST;
        else t->type = TT_ID;
        return;
    }
    // 字符串
    if(c == '"') {
        const char* q = p + 1;
        int n = 0;
        while(q < tp->end && *q != '\0' && *q != '"') {
            if(*q == '\\' && q + 1 < tp->end) {
                if(q[1] == '"' || q[1] == '\'') { q++; continue; }   // 转义引号：跳过 \，引号留作边界
                q += 2; continue;                                      // 其他转义：原样跳过
            }
            tok_ensure(t, n + 2);
            t->text[n++] = *q;
            q++;
        }
        tok_ensure(t, n + 1);
        t->text[n] = '\0';
        if(q >= tp->end || *q != '"') tp_err("字符串未闭合");
        tp->p = q + 1;
        t->type = TT_STR;
        return;
    }
    // char
    if(c == '\'') {
        const char* q = p + 1;
        if(q < tp->end && *q != '\'' && *q != '\0') {
            t->ch = *q;
            tp->p = q + 2;
            t->type = TT_CHAR;
            return;
        }
        tp_err("字符字面量格式错误");
    }
    // 运算符
    p++;
    tp->p = p;
    switch(c) {
        case '(': t->type = TT_LPAREN; return;
        case ')': t->type = TT_RPAREN; return;
        case '.': t->type = TT_DOT; return;
        case '[': t->type = TT_LBRACKET; return;
        case ']': t->type = TT_RBRACKET; return;
        case ',': t->type = TT_COMMA; return;
        case '?': t->type = TT_QMARK; return;
        case ':': t->type = TT_COLON; return;
        case '+': t->type = TT_PLUS; return;
        case '-': t->type = TT_MINUS; return;
        case '*': t->type = TT_STAR; return;
        case '/': t->type = TT_SLASH; return;
        case '%': t->type = TT_PERCENT; return;
        case '!':
            if(tp->p < tp->end && *tp->p == '=') { tp->p++; t->type = TT_NE; }
            else t->type = TT_NOT;
            return;
        case '=':
            if(tp->p < tp->end && *tp->p == '=') { tp->p++; t->type = TT_EQ; }
            else tp_err("不支持赋值运算符");
            return;
        case '<':
            if(tp->p < tp->end && *tp->p == '=') { tp->p++; t->type = TT_LE; }
            else t->type = TT_LT;
            return;
        case '>':
            if(tp->p < tp->end && *tp->p == '=') { tp->p++; t->type = TT_GE; }
            else t->type = TT_GT;
            return;
        case '&':
            if(tp->p < tp->end && *tp->p == '&') { tp->p++; t->type = TT_AND; }
            else tp_err("不支持单 & 运算符");
            return;
        case '|':
            if(tp->p < tp->end && *tp->p == '|') { tp->p++; t->type = TT_OR; }
            else tp_err("不支持单 | 运算符");
            return;
        case '{':
            tp_err("内插表达式暂不支持匿名函数字面量（可先赋值给变量再内插引用）");
        case '}':
            tp_err("内插表达式内出现未配对的 '}'");
        default:
            tp_err("无法识别的字符");
    }
}

static TpTok tp_peek(TpParser* tp)
{
    if(!tp->have_look) {
        free(tp->look.text);        // 释放上一 token 文本（take 后仍归 look 所有）
        tp->look.text = NULL;
        tp->look.text_cap = 0;
        tp_next_tok(tp, &tp->look);
        tp->have_look = 1;
    }
    return tp->look;
}

static TpTok tp_take(TpParser* tp)
{
    TpTok t = tp_peek(tp);
    tp->have_look = 0;
    return t;
}

static void tp_expect(TpParser* tp, TpTokType ty, const char* what)
{
    TpTok t = tp_take(tp);
    if(t.type != ty) tp_err(what);
}

// ---------------- 递归下降（优先级从低到高） ----------------

static AstNode* tp_expr(TpParser* tp);
static AstNode* tp_ternary(TpParser* tp);
static AstNode* tp_or(TpParser* tp);
static AstNode* tp_and(TpParser* tp);
static AstNode* tp_eq(TpParser* tp);
static AstNode* tp_rel(TpParser* tp);
static AstNode* tp_add(TpParser* tp);
static AstNode* tp_mul(TpParser* tp);
static AstNode* tp_unary(TpParser* tp);
static AstNode* tp_postfix(TpParser* tp);
static AstNode* tp_primary(TpParser* tp);

static AstNode* tp_ternary(TpParser* tp)
{
    AstNode* c = tp_or(tp);
    if(tp_peek(tp).type == TT_QMARK) {
        tp_take(tp);
        AstNode* t = tp_expr(tp);
        tp_expect(tp, TT_COLON, "缺少 ':'");
        AstNode* f = tp_ternary(tp);
        return ast_ternary(c, t, f);
    }
    return c;
}

static AstNode* tp_or(TpParser* tp)
{
    AstNode* l = tp_and(tp);
    while(tp_peek(tp).type == TT_OR) {
        tp_take(tp);
        l = ast_binop(OP_LOGIC_OR, l, tp_and(tp));
    }
    return l;
}

static AstNode* tp_and(TpParser* tp)
{
    AstNode* l = tp_eq(tp);
    while(tp_peek(tp).type == TT_AND) {
        tp_take(tp);
        l = ast_binop(OP_LOGIC_AND, l, tp_eq(tp));
    }
    return l;
}

static AstNode* tp_eq(TpParser* tp)
{
    AstNode* l = tp_rel(tp);
    for(;;) {
        TpTok t = tp_peek(tp);
        if(t.type == TT_EQ) { tp_take(tp); l = ast_binop(OP_EQ, l, tp_rel(tp)); }
        else if(t.type == TT_NE) { tp_take(tp); l = ast_binop(OP_NE, l, tp_rel(tp)); }
        else return l;
    }
}

static AstNode* tp_rel(TpParser* tp)
{
    AstNode* l = tp_add(tp);
    for(;;) {
        TpTok t = tp_peek(tp);
        if(t.type == TT_LT) { tp_take(tp); l = ast_binop(OP_LT, l, tp_add(tp)); }
        else if(t.type == TT_GT) { tp_take(tp); l = ast_binop(OP_GT, l, tp_add(tp)); }
        else if(t.type == TT_LE) { tp_take(tp); l = ast_binop(OP_LE, l, tp_add(tp)); }
        else if(t.type == TT_GE) { tp_take(tp); l = ast_binop(OP_GE, l, tp_add(tp)); }
        else return l;
    }
}

static AstNode* tp_add(TpParser* tp)
{
    AstNode* l = tp_mul(tp);
    for(;;) {
        TpTok t = tp_peek(tp);
        if(t.type == TT_PLUS) { tp_take(tp); l = ast_binop(OP_ADD, l, tp_mul(tp)); }
        else if(t.type == TT_MINUS) { tp_take(tp); l = ast_binop(OP_SUB, l, tp_mul(tp)); }
        else return l;
    }
}

static AstNode* tp_mul(TpParser* tp)
{
    AstNode* l = tp_unary(tp);
    for(;;) {
        TpTok t = tp_peek(tp);
        if(t.type == TT_STAR) { tp_take(tp); l = ast_binop(OP_MUL, l, tp_unary(tp)); }
        else if(t.type == TT_SLASH) { tp_take(tp); l = ast_binop(OP_DIV, l, tp_unary(tp)); }
        else if(t.type == TT_PERCENT) { tp_take(tp); l = ast_binop(OP_MOD, l, tp_unary(tp)); }
        else return l;
    }
}

static AstNode* tp_unary(TpParser* tp)
{
    TpTok t = tp_peek(tp);
    if(t.type == TT_MINUS) { tp_take(tp); return ast_unary(OP_UNARY_MINUS, tp_unary(tp)); }
    if(t.type == TT_PLUS)  { tp_take(tp); return ast_unary(OP_UNARY_PLUS, tp_unary(tp)); }
    if(t.type == TT_NOT)   { tp_take(tp); return ast_unary(OP_LOGIC_NOT, tp_unary(tp)); }
    return tp_postfix(tp);
}

static AstNode* tp_primary(TpParser* tp);

static AstNode* tp_postfix(TpParser* tp)
{
    AstNode* e = tp_primary(tp);
    for(;;) {
        TpTok t = tp_peek(tp);
        if(t.type == TT_LBRACKET) {
            tp_take(tp);
            AstNode* idx = tp_expr(tp);
            tp_expect(tp, TT_RBRACKET, "缺少 ']'");
            e = ast_index(e, idx);
        } else if(t.type == TT_LPAREN) {
            // 调用：仅允许 ID 表达式被调用
            if(e->type != AST_VAR) tp_err("只有具名函数可以调用");
            char* nm = strdup(e->u.varname);
            tp_take(tp);
            AstNode* args = NULL;
            if(tp_peek(tp).type != TT_RPAREN) {
                args = tp_expr(tp);
                while(tp_peek(tp).type == TT_COMMA) {
                    tp_take(tp);
                    args = ast_seq(args, tp_expr(tp));
                }
            }
            tp_expect(tp, TT_RPAREN, "缺少 ')'");
            e = ast_call(nm, args);
        } else if(t.type == TT_DOT) {
            // 方法链 a.b(x) → b(a, x)（与主语法一致）
            tp_take(tp);
            TpTok m = tp_take(tp);
            if(m.type != TT_ID) tp_err("方法名必须是标识符");
            char* nm = strdup(m.text);
            AstNode* margs = NULL;
            if(tp_peek(tp).type == TT_LPAREN) {
                tp_take(tp);
                if(tp_peek(tp).type != TT_RPAREN) {
                    margs = tp_expr(tp);
                    while(tp_peek(tp).type == TT_COMMA) {
                        tp_take(tp);
                        margs = ast_seq(margs, tp_expr(tp));
                    }
                }
                tp_expect(tp, TT_RPAREN, "缺少 ')'");
            }
            e = ast_call(nm, margs ? ast_seq_front(margs, e) : e);
        } else break;
    }
    return e;
}

static AstNode* tp_primary(TpParser* tp)
{
    TpTok t = tp_take(tp);
    switch(t.type) {
        case TT_INT:    return ast_int(t.ival);
        case TT_FNUM:   return ast_num(t.fval);
        case TT_TRUE:   return ast_bool(1);
        case TT_FALSE:  return ast_bool(0);
        case TT_NULL:   return ast_none();
        case TT_STR:    return ast_string(t.text);
        case TT_CHAR:   return ast_new_char(t.ch);
        case TT_CAST: {
            // ASCII(...) 是内置调用；强转只在 (type) 形式（TT_LPAREN 分支处理）
            if(strcmp(t.text, "ASCII") == 0 && tp_peek(tp).type == TT_LPAREN) {
                tp_take(tp);
                AstNode* args = NULL;
                if(tp_peek(tp).type != TT_RPAREN) {
                    args = tp_expr(tp);
                    while(tp_peek(tp).type == TT_COMMA) {
                        tp_take(tp);
                        args = ast_seq(args, tp_expr(tp));
                    }
                }
                tp_expect(tp, TT_RPAREN, "缺少 ')'");
                return ast_call(strdup("ASCII"), args);
            }
            tp_err("类型关键字不能在此使用");
        }
        case TT_ID: {
            // 数组字面量消歧：ID 后跟 '[' 是下标（在 postfix 处理），这里只需返回变量
            return ast_var(strdup(t.text));
        }
        case TT_LBRACKET: {
            AstNode* elems = NULL;
            if(tp_peek(tp).type != TT_RBRACKET) {
                elems = tp_expr(tp);
                while(tp_peek(tp).type == TT_COMMA) {
                    tp_take(tp);
                    elems = ast_seq(elems, tp_expr(tp));
                }
            }
            tp_expect(tp, TT_RBRACKET, "缺少 ']'");
            return ast_array_lit(elems, -1);
        }
        case TT_LPAREN: {
            // 强转：(int) primary
            TpTok nx = tp_peek(tp);
            if(nx.type == TT_CAST) {
                TpTok ct = tp_take(tp);
                tp_expect(tp, TT_RPAREN, "缺少 ')'");
                AstNode* kid = tp_primary(tp);
                int cast = -1;
                if(strcmp(ct.text, "int") == 0) cast = CAST_INT;
                else if(strcmp(ct.text, "double") == 0) cast = CAST_DOUBLE;
                else if(strcmp(ct.text, "string") == 0) cast = CAST_STRING;
                else if(strcmp(ct.text, "bool") == 0) cast = CAST_BOOL;
                else if(strcmp(ct.text, "char") == 0) cast = CAST_CHAR;
                else if(strcmp(ct.text, "ASCII") == 0) cast = CAST_ASCII;
                return new_cast_node(cast, kid);
            }
            AstNode* e = tp_expr(tp);
            tp_expect(tp, TT_RPAREN, "缺少 ')'");
            return e;
        }
        default:
            tp_err("无法解析的表达式");
    }
    return NULL;
}

static AstNode* tp_expr(TpParser* tp) { return tp_ternary(tp); }

// ---------------- 模板拆解 ----------------

static int tp_strlen(const char* s) { return (int)strlen(s); }

// 构造 format 调用：args 为左嵌套 AST_SEQ 链（与 yacc arg_list 一致）
static AstNode* make_format_call(char* fmt_text, AstNode* exprs)
{
    AstNode* args = ast_string(fmt_text);
    if(exprs) args = ast_seq(args, exprs);
    return ast_call(strdup("format"), args);
}

AstNode* maybe_template(const char* s)
{
    const char* p = s;
    if(!strchr(s, '{')) return ast_string(s);

    // 第一遍：确认含内插（"{{" 不算）；否则当普通字符串
    {
        const char* q = s;
        int has_interp = 0;
        while(*q) {
            if(q[0] == '{' && q[1] == '{') { q += 2; continue; }
            if(q[0] == '{' && q[1] == '}') { q += 2; continue; }   // 字面 {}（format 占位）不算内插
            if(q[0] == '{') { has_interp = 1; break; }
            q++;
        }
        if(!has_interp) return ast_string(s);
        // 注：无内插的字符串原样返回——{{/}} 转义是模板特性（有内插的模板串才生效），
        // 且 format 的格式串实参不能被提前转义（否则 format("{{x}}") 双重转义报错）。
    }

    // 拆段
    size_t cap = strlen(s) + 64;
    char* fmt = (char*)malloc(cap + 1);
    size_t w = 0;
    AstNode* exprs = NULL;
    int n_expr = 0;

    while(*p) {
        if(p[0] == '{' && p[1] == '{') {
            fmt[w++] = '{'; fmt[w++] = '{';
            p += 2;
            continue;
        }
        if(p[0] == '}') {
            fmt[w++] = '}';       // 交给 format 报"未配对的 '}'"
            p++;
            continue;
        }
        if(p[0] == '{' && p[1] == '}') {
            fmt[w++] = '{'; fmt[w++] = '}';   // 字面 {}（format 占位）原样
            p += 2;
            continue;
        }
        if(p[0] == '{') {
            // 找匹配 '}'（跳过字符串/字符，跟踪 ( [ { 深度；内插结束 = 深度 0 的 '}'）
            // 同时找第一个深度 0 的 ':' 作为 spec 分隔符（{expr:spec}）
            // 三元运算符的 ':' 也会被当作分隔符，需用括号包住：{(a?b:c):spec}
            const char* q = p + 1;
            const char* expr_start = q;
            int depth = 0;
            const char* close = NULL;
            const char* colon = NULL;   // spec 分隔符位置（第一个顶层冒号）
            while(*q) {
                if(*q == '\\' && q[1]) { q += 2; continue; }   // 跳过转义序列（\" 等）
                if(*q == '"' || *q == '\'') { skip_quoted(&q, s + strlen(s)); continue; }
                if(*q == '(' || *q == '[' || *q == '{') depth++;
                else if(*q == ')') depth--;
                else if(*q == ']') depth--;
                else if(*q == '}') {
                    if(depth == 0) { close = q; break; }
                    depth--;
                }
                else if(*q == ':' && depth == 0 && !colon) {
                    colon = q;   // 记录 spec 分隔符
                }
                q++;
            }
            if(!close) {
                fprintf(stderr, "语义错误(第%d行)：模板字符串未闭合的 '{'\n", yylineno);
                exit(EXIT_FAILURE);
            }
            // 提取表达式文本（到 colon 或 close）
            const char* expr_end = colon ? colon : close;
            size_t elen = (size_t)(expr_end - expr_start);
            char* etext = (char*)malloc(elen + 1);
            memcpy(etext, expr_start, elen);
            etext[elen] = '\0';
            // 空 spec 分隔但空表达式：{:spec} 不允许（空内插）
            {
                const char* e = etext;
                while(*e && isspace((unsigned char)*e)) e++;
                if(*e == '\0') {
                    fprintf(stderr, "语义错误(第%d行)：模板字符串空的内插表达式（字面花括号请用 {{ 和 }}）\n", yylineno);
                    exit(EXIT_FAILURE);
                }
            }
            // 解析内插表达式
            TpParser tp;
            memset(&tp, 0, sizeof tp);
            tp.p = etext;
            tp.end = etext + elen;
            AstNode* e = tp_expr(&tp);
            TpTok tail = tp_peek(&tp);
            if(tail.type != TT_EOF) {
                fprintf(stderr, "语义错误(第%d行)：模板字符串内插表达式错误：多余内容\n", yylineno);
                exit(EXIT_FAILURE);
            }
            free(etext);
            // fmt 加占位：{spec}（spec 可空）；spec 原样写进 fmt，由 format 运行时解析
            // spec 内不含 '{' '}'（colon 到 close 之间，已确保无顶层 '}'）
            size_t slen = colon ? (size_t)(close - (colon + 1)) : 0;
            if(w + 2 + slen > cap) {
                while(w + 2 + slen > cap) cap *= 2;
                fmt = (char*)realloc(fmt, cap + 1);
            }
            fmt[w++] = '{';
            if(slen > 0) {
                memcpy(fmt + w, colon + 1, slen);
                w += slen;
            }
            fmt[w++] = '}';
            // expr 挂链（左嵌套，与 yacc arg_list 一致）
            exprs = exprs ? ast_seq(exprs, e) : e;
            n_expr++;
            p = close + 1;
            continue;
        }
        fmt[w++] = *p;
        p++;
    }
    fmt[w] = '\0';

    AstNode* node = make_format_call(fmt, exprs);
    free(fmt);
    return node;
}
