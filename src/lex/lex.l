/* 编译期模块：prev_token/in_case_expr 为单线程词法状态，未来并发编译需实例化。 */
%{
#include "yacc.tab.h"
#include "lumyr_value_type.h"
#include "ast/ast_types.h"  /* struct_lookup / class_lookup / type_lookup */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <limits.h>

// 前一个 token：用于区分 `[` 是下标（表达式后）还是数组字面量（表达式开始）
static int prev_token = 0;
// 前前一个 token：用于判断 `{` 前的 ID 是否处于声明上下文（extends/implements/struct/class/type）
static int prev_prev_token = 0;
// 前第三个 token：泛型头 `class Box<T> {` 中 { 前是 TOK_GENERIC，
// 需再向前看一个 token 区分声明头（其前为 class/struct/interface/extends）与
// 表达式中的命名构造/泛型 map 字面量（其前为 = / ( / , / : 等）
static int prev_prev_prev_token = 0;
// 前第四个 token：区分方法返回类型后的函数体 `): Pair<K,V> {`（prev4=RPAREN）
// 与 map 值位置的泛型命名构造 `: Box<int>{...}`（prev4 非 RPAREN）
static int prev4_token = 0;

// 上一个 ID 文本：用于判断 `{` 前的 ID 是否为已注册类型名（ClassName{field: value} 命名构造）
static char g_last_id_text[256] = {0};

/* 多行反引号字符串缓冲区 */
static char mstr_buf[65536];
static int mstr_len = 0;
/* 多行反引号字符串是否为 f-string（支持 {expr} 内插） */
static int mstr_is_fstring = 0;

// 表达式结束类 token（其后 `[` 是下标）
static int in_case_expr = 0;   /* case 值后的冒号 → CASE_COLON（与字典/三元冒号区分） */

static int is_expr_end(int t)
{
    switch(t) {
        case ID: case INTEGER: case NUMBER: case STRING_LIT: case TOK_CHAR_LIT:
        case TRUE: case FALSE: case RBRACKET: case RPAREN:
        case PLUSPLUS: case MINUSMINUS:
        case ELSE: case ELSEIF:
        case CASE_COLON:          /* case 10: { → 块 */
        case TRY:                 /* try { → 块 */
        case FINALLY:             /* finally { → 块 */
        case TOK_DO:              /* do { → 块 */
        case TOK_DEFER:            /* defer { → 块 */
        /* 内置类型关键字：出现在冒号返回类型后，其后的 { 一定是块而非 map */
        case TOK_INT: case TOK_DOUBLE: case TOK_CHAR: case TOK_STRING:
        case TOK_BOOL: case TOK_ASCII: case TOK_BYTE:
        case TOK_INT8: case TOK_INT16: case TOK_INT32: case TOK_INT64:
        case TOK_UINT8: case TOK_UINT16: case TOK_UINT32: case TOK_UINT64:
        case TOK_UINT: case TOK_LONGLONG: case TOK_LONG: case TOK_FLOAT:
        case TOK_ULONG: case TOK_UCHAR: case TOK_SHORT: case TOK_USHORT:
        case TOK_SIZE_T: case TOK_SSIZE_T: case TOK_VOID:
        case TOK_LONG_DOUBLE: case TOK_PTR:
        case TOK_DATE: case TOK_DATETIME: case TOK_TIME_KW: case TOK_TIMEDELTA:
        case TOK_TUPLE: case TOK_BYTES: case TOK_COMPLEX:
        case TOK_FILE: case TOK_FOLDER:
        case TOK_TYPE_ANNOT:      /* <int> 后的 { 可能是类体（extends C<int> {）或命名构造（C<int>{...}） */
        case ARROW:                 /* => { → 函数体块 */
            return 1;
        default:
            return 0;
    }
}

#define LEX_RET(t) do { prev4_token = prev_prev_prev_token; prev_prev_prev_token = prev_prev_token; prev_prev_token = prev_token; prev_token = (t); return (t); } while(0)

/* ===== 字面量后缀类型推断（数字字面量规则共用，yacc 动作读取） =====
 * 无后缀 = CAST_NONE：编译期按值推断（int32 范围内=int，否则=int64/double）
 * 有后缀 = 精确类型：
 *   整数：5L→long  5LL→long long  5u→uint  5ul→ulong  5ull→uint64
 *         5i8/5i16/5i32/5i64  5u8/5u16/5u32/5u64
 *   浮点：5f→float  5d→double  5ld→long double（f/F/d/D/ld/LD/lD/Ld）
 *   进制不影响类型：0x1F/0b1010/0o17 按值推断（同十进制）
 */
CastKind g_lit_cast = CAST_NONE;

/* 后缀类型传递队列：YYSTYPE 是 union（所有字段偏移0），无法同时携带值与 CastKind；
 * 而 bison LALR(1) 会因 lookahead 提前 lex 下一个 token，单个全局变量会被覆盖。
 * lex 数字 token 时入队，对应归约动作出队（FIFO 深度≤2）。 */
#define CAST_Q_CAP 16
static int castQ[CAST_Q_CAP];
static int castQHead = 0, castQTail = 0;
static void castQ_push(int ck) {
    castQ[castQTail] = ck;
    castQTail = (castQTail + 1) & (CAST_Q_CAP - 1);
}
int g_cast_take(void) {
    int x = castQ[castQHead];
    castQHead = (castQHead + 1) & (CAST_Q_CAP - 1);
    return x;
}

/* 从串尾扫描整数后缀（宽度家族 i8..u64，含数字，需先于 [uUlL] 家族判定） */
static CastKind lit_int_suffix(const char* s, int len)
{
    if(len >= 3) {
        const char* r = s + len - 3;
        if((r[0]=='i'||r[0]=='u') && r[1]=='1' && r[2]=='6') return r[0]=='i'?CAST_INT16:CAST_UINT16;
        if((r[0]=='i'||r[0]=='u') && r[1]=='3' && r[2]=='2') return r[0]=='i'?CAST_INT32:CAST_UINT32;
        if((r[0]=='i'||r[0]=='u') && r[1]=='6' && r[2]=='4') return r[0]=='i'?CAST_INT64:CAST_UINT64;
    }
    if(len >= 2) {
        const char* q = s + len - 2;
        if(q[0]=='i' && q[1]=='8') return CAST_INT8;
        if(q[0]=='u' && q[1]=='8') return CAST_UINT8;
    }
    /* [uUlL]{0,3} 家族（十六进制数字不含 u/U/l/L，从尾回扫安全） */
    int u = 0, l = 0;
    const char* p = s + len;
    while(p > s) {
        char ch = p[-1];
        if(ch=='u'||ch=='U') { u = 1; p--; }
        else if(ch=='l'||ch=='L') { l++; p--; }
        else break;
    }
    if(!u && !l) return CAST_NONE;
    if(u) return (l>=2)?CAST_UINT64 : (l==1)?CAST_ULONG : CAST_UINT;
    return (l>=2)?CAST_LONGLONG : CAST_LONG;
}

/* 数字字面量统一入口：识别进制前缀/小数/指数/后缀，
 * 填充 yylval 与 g_lit_cast，返回 token（INTEGER/NUMBER/BIG_INTEGER/BIG_DECIMAL） */
static int lex_number(const char* s)
{
    int len = (int)strlen(s);
    g_lit_cast = CAST_NONE;

    /* ===== 进制前缀：十六进制/二进制/八进制整数（后缀仅 [uUlL] 家族） ===== */
    if(s[0]=='0' && len>=2 && (s[1]=='x'||s[1]=='X'||s[1]=='b'||s[1]=='B'||s[1]=='o'||s[1]=='O')) {
        int base = (s[1]=='x'||s[1]=='X') ? 16 : (s[1]=='b'||s[1]=='B') ? 2 : 8;
        errno = 0;
        /* 跳过 0x/0b/0o 前缀：strtoull 仅十六进制自带 0x 前缀识别 */
        unsigned long long v = strtoull(s + 2, NULL, base);
        if(errno == ERANGE) {
            /* 超过 uint64 最大值，存成字符串（用于 <bigint> 类型） */
            yylval.s = strdup(s);
            return BIG_INTEGER;
        }
        g_lit_cast = lit_int_suffix(s, len);
        castQ_push((int)g_lit_cast);
        yylval.ll = (long long)v;
        return INTEGER;
    }

    /* ===== 十进制：小数点或指数 → 浮点字面量 ===== */
    int has_dot = 0, has_exp = 0;
    for(int i = 0; i < len; i++) {
        if(s[i]=='.') has_dot = 1;
        else if(s[i]=='e'||s[i]=='E') has_exp = 1;
    }
    if(has_dot || has_exp) {
        /* 浮点后缀：ld/LD/lD/Ld → long double，f/F → float，d/D → double */
        char c1 = s[len-1];
        if((c1=='d'||c1=='D') && len>=2 && (s[len-2]=='l'||s[len-2]=='L')) g_lit_cast = CAST_LONG_DOUBLE;
        else if(c1=='f'||c1=='F') g_lit_cast = CAST_FLOAT;
        else if(c1=='d'||c1=='D') g_lit_cast = CAST_DOUBLE;
        if(g_lit_cast == CAST_NONE) {
            /* 无后缀：小数部分超过 15 位（double 精度）→ 存成字符串（用于 <decimal>） */
            const char* dot = strchr(s, '.');
            int frac_len = dot ? (int)strlen(dot + 1) : 0;
            if(frac_len > 15) {
                yylval.s = strdup(s);
                return BIG_DECIMAL;
            }
        }
        castQ_push((int)g_lit_cast);
        yylval.d = atof(s);   /* atof 遇到后缀字符自动停止 */
        return NUMBER;
    }

    /* ===== 十进制整数 ===== */
    /* 浮点后缀作用于整数字面量：5f→float(5.0) 5d→double(5.0) 5ld→long double(5.0) */
    char c1 = s[len-1];
    if(c1=='f'||c1=='F') { g_lit_cast = CAST_FLOAT; castQ_push((int)CAST_FLOAT); yylval.d = atof(s); return NUMBER; }
    if((c1=='d'||c1=='D') && len>=2 && (s[len-2]=='l'||s[len-2]=='L')) { g_lit_cast = CAST_LONG_DOUBLE; castQ_push((int)CAST_LONG_DOUBLE); yylval.d = atof(s); return NUMBER; }
    if(c1=='d'||c1=='D') { g_lit_cast = CAST_DOUBLE; castQ_push((int)CAST_DOUBLE); yylval.d = atof(s); return NUMBER; }

    errno = 0;
    unsigned long long v = strtoull(s, NULL, 10);
    if(errno == ERANGE) {
        /* 超过 uint64 最大值，存成字符串（用于 <bigint> 类型） */
        yylval.s = strdup(s);
        return BIG_INTEGER;
    }
    g_lit_cast = lit_int_suffix(s, len);
    castQ_push((int)g_lit_cast);
    yylval.ll = (long long)v;
    return INTEGER;
}
%}

%option yylineno
%x MSTR

%%
"type"          { LEX_RET(TOK_TYPE); }
"struct"        { LEX_RET(TOK_STRUCT); }
"class"         { LEX_RET(TOK_CLASS); }
"super"         { LEX_RET(TOK_SUPER); }
"enum"          { LEX_RET(TOK_ENUM); }
"interface"     { LEX_RET(TOK_INTERFACE); }
"implements"    { LEX_RET(TOK_IMPLEMENTS); }
"extends"       { LEX_RET(TOK_EXTENDS); }
"extend"        { LEX_RET(TOK_EXTEND); }
"unpack"        { LEX_RET(TOK_UNPACK); }
"func"          { LEX_RET(FUNC); }
"gen"           { LEX_RET(TOK_GEN); }
"ref"           { LEX_RET(TOK_REF); }
"yield"         { LEX_RET(TOK_YIELD); }
"const"         { LEX_RET(CONST); }
"macro"         { LEX_RET(MACRO); }
"extern"        { LEX_RET(TOK_EXTERN); }

"print"         { LEX_RET(PRINT); }
"true"          { LEX_RET(TRUE); }
"false"         { LEX_RET(FALSE); }
"null"          { LEX_RET(NULL_LIT); }
"if"            { LEX_RET(IF); }
"elseif"        { LEX_RET(ELSEIF); }
"else"          { LEX_RET(ELSE); }
"while"         { LEX_RET(WHILE); }
"do"            { LEX_RET(TOK_DO); }
"for"           { LEX_RET(FOR); }
"@"             { LEX_RET(TOK_AT); }
"in"            { LEX_RET(TOK_IN); }
"iter"          { LEX_RET(TOK_ITER); }

"switch"        {  LEX_RET(SWITCH); }
"match"         {  LEX_RET(SWITCH); }  /* match as alias for pattern matching */
"case"          { in_case_expr = 1;    LEX_RET(CASE); }
"default"       { in_case_expr = 1; LEX_RET(DEFAULT); }
"try"           {    LEX_RET(TRY); }
"throw"         {  LEX_RET(THROW); }
"catch"         {  LEX_RET(CATCH); }
"finally"       { LEX_RET(FINALLY); }
"break"         {   LEX_RET(BREAK); }
"continue"      { LEX_RET(CONTINUE); }
"return"        {  LEX_RET(RETURN); }
"assert"        { LEX_RET(TOK_ASSERT); }
"defer"         { LEX_RET(TOK_DEFER); }

"int"           { LEX_RET(TOK_INT); }
"double"        { LEX_RET(TOK_DOUBLE); }
"char"          { LEX_RET(TOK_CHAR); }
"string"        { LEX_RET(TOK_STRING); }
"bool"          { LEX_RET(TOK_BOOL); }
"ascii"         { LEX_RET(TOK_ASCII); }
"byte"          { LEX_RET(TOK_BYTE); }
"int8"          { LEX_RET(TOK_INT8); }
"int16"         { LEX_RET(TOK_INT16); }
"int32"         { LEX_RET(TOK_INT32); }
"int64"         { LEX_RET(TOK_INT64); }
"uint8"         { LEX_RET(TOK_UINT8); }
"uint16"        { LEX_RET(TOK_UINT16); }
"uint32"        { LEX_RET(TOK_UINT32); }
"uint64"        { LEX_RET(TOK_UINT64); }
"uint"          { LEX_RET(TOK_UINT); }
"long long"     { LEX_RET(TOK_LONGLONG); }
"longlong"      { LEX_RET(TOK_LONGLONG); }
"long"          { LEX_RET(TOK_LONG); }
"float"         { LEX_RET(TOK_FLOAT); }
"ulong"         { LEX_RET(TOK_ULONG); }
"unsigned long" { LEX_RET(TOK_ULONG); }
"uchar"         { LEX_RET(TOK_UCHAR); }
"unsigned char" { LEX_RET(TOK_UCHAR); }
"short"         { LEX_RET(TOK_SHORT); }
"ushort"        { LEX_RET(TOK_USHORT); }
"unsigned short" { LEX_RET(TOK_USHORT); }
"size_t"        { LEX_RET(TOK_SIZE_T); }
"ssize_t"       { LEX_RET(TOK_SSIZE_T); }
"void"          { LEX_RET(TOK_VOID); }
"long double"   { LEX_RET(TOK_LONG_DOUBLE); }
"longdouble"    { LEX_RET(TOK_LONG_DOUBLE); }
"ptr"           { LEX_RET(TOK_PTR); }
"pointer"       { LEX_RET(TOK_PTR); }
"handle"        { LEX_RET(TOK_PTR); }
"date"          { LEX_RET(TOK_DATE); }
"datetime"      { LEX_RET(TOK_DATETIME); }
"time"          { LEX_RET(TOK_TIME_KW); }
"timedelta"     { LEX_RET(TOK_TIMEDELTA); }
"tuple"         { LEX_RET(TOK_TUPLE); }
"bytes"         { LEX_RET(TOK_BYTES); }
"complex"       { LEX_RET(TOK_COMPLEX); }
"calendar"      { LEX_RET(TOK_CALENDAR); }
"file"          { LEX_RET(TOK_FILE); }
"folder"        { LEX_RET(TOK_FOLDER); }
"static"        { LEX_RET(TOK_STATIC); }
"abstract"      { LEX_RET(TOK_ABSTRACT); }
"public"        { LEX_RET(TOK_PUBLIC); }
"private"       { LEX_RET(TOK_PRIVATE); }
"protected"     { LEX_RET(TOK_PROTECTED); }

"..."           { LEX_RET(ELLIPSIS); }

\"([^"\\]|\\.)*\"   {
    yylval.s = strdup(yytext+1);
    yylval.s[strlen(yylval.s)-1] = '\0';
    LEX_RET(STRING_LIT);
}

"`"  { BEGIN(MSTR); mstr_buf[0]='\0'; mstr_len=0; mstr_is_fstring=0; }
"f`" { BEGIN(MSTR); mstr_buf[0]='\0'; mstr_len=0; mstr_is_fstring=1; }
<MSTR>"`"  {
    BEGIN(INITIAL);
    mstr_buf[mstr_len] = '\0';
    yylval.s = strdup(mstr_buf);
    LEX_RET(mstr_is_fstring ? FSTRING_LIT : STRING_LIT);
}
<MSTR>\n  { mstr_buf[mstr_len++]='\n'; }
<MSTR>\\n  { mstr_buf[mstr_len++]='\n'; }
<MSTR>\\t  { mstr_buf[mstr_len++]='\t'; }
<MSTR>\\r  { mstr_buf[mstr_len++]='\r'; }
<MSTR>\\\\ { mstr_buf[mstr_len++]='\\'; }
<MSTR>.  { mstr_buf[mstr_len++]=yytext[0]; }
<MSTR><<EOF>> { BEGIN(INITIAL); mstr_buf[mstr_len]='\0'; yylval.s=strdup(mstr_buf); LEX_RET(mstr_is_fstring ? FSTRING_LIT : STRING_LIT); }

f\"([^"\\]|\\.)*\"   {
    yylval.s = strdup(yytext+2);   /* 跳过 f 和开头引号 */
    yylval.s[strlen(yylval.s)-1] = '\0';
    LEX_RET(FSTRING_LIT);
}

'(\\.|[^'\''\\\n])'      {   /* 字符字面量：'A' 或转义 \n \t \r \0 \\ \' \" */
    if(yytext[1] == '\\') {
        char c = yytext[2];
        switch(c) {
            case 'n':  yylval.ch = '\n'; break;
            case 't':  yylval.ch = '\t'; break;
            case 'r':  yylval.ch = '\r'; break;
            case '0':  yylval.ch = '\0'; break;
            case '\\': yylval.ch = '\\'; break;
            case '\'': yylval.ch = '\''; break;
            case '"':  yylval.ch = '"';  break;
            default:   yylval.ch = c;    break;
        }
    } else {
        yylval.ch = yytext[1];
    }
    LEX_RET(TOK_CHAR_LIT);
}

"."                     {
    LEX_RET(DOT);
}

[a-zA-Z_][a-zA-Z0-9_]*  {
    if(strcmp(yytext, "read") == 0) { LEX_RET(READ); }
    if(strcmp(yytext, "write") == 0) { LEX_RET(WRITE); }
    snprintf(g_last_id_text, sizeof(g_last_id_text), "%s", yytext);
    yylval.s = strdup(yytext);
    LEX_RET(ID);
}

0[xX][0-9a-fA-F]+(i8|i16|i32|i64|u8|u16|u32|u64|[uUlL]{0,3})  |
0[bB][01]+(i8|i16|i32|i64|u8|u16|u32|u64|[uUlL]{0,3})         |
0[oO][0-7]+(i8|i16|i32|i64|u8|u16|u32|u64|[uUlL]{0,3})        |
[0-9]+\.[0-9]+([eE][+-]?[0-9]+)?(ld|LD|lD|Ld|[fFdD])?  |
[0-9]+[eE][+-]?[0-9]+(ld|LD|lD|Ld|[fFdD])?             |
[0-9]+(i8|i16|i32|i64|u8|u16|u32|u64|[uU][lL]{0,2}|[lL][lL]?[uU]?|[fFdD]|[lL][dD])  {
    /* 数字字面量统一入口：进制前缀 0x/0b/0o、小数/指数、类型后缀
     * 5L→long 5LL→long long 5u/5ul/5ull→uint 族 5i8..5u64→对应宽度
     * 5.5f→float 5.5d→double 5.5ld→long double；进制不影响类型 */
    /* 注意：不可用 LEX_RET(lex_number(yytext))——宏会把实参求值两次，
     * 导致后缀队列双份入队 */
    {
        int tkNum = lex_number(yytext);
        prev_prev_token = prev_token;
        prev_token = tkNum;
        return tkNum;
    }
}

[0-9]+\.[0-9]+          {
    /* 浮点数字面量：
     * 如果小数部分超过 15 位有效数字（double 的精度），存成字符串（用于 <decimal> 类型）
     * 否则，正常解析成 double */
    const char* dot = strchr(yytext, '.');
    int frac_len = dot ? strlen(dot + 1) : 0;
    if(frac_len > 15) {
        /* 高精度浮点，存成字符串（用于 <decimal> 类型） */
        yylval.s = strdup(yytext);
        LEX_RET(BIG_DECIMAL);
    } else {
        castQ_push((int)CAST_NONE);
        yylval.d = atof(yytext);
        LEX_RET(NUMBER);
    }
}

[0-9]+                  {
    /* 使用 strtoull 解析整数字面量，支持 uint64 最大值（18446744073709551615）
       优先级：先尝试用无符号解析，后续根据上下文（类型注解）决定如何解释
       如果超过 uint64 最大值，存成字符串（用于 <bigint> 类型） */
    errno = 0;
    unsigned long long ullval = strtoull(yytext, NULL, 10);
    if(errno == ERANGE) {
        /* 超过 uint64 最大值，存成字符串（用于 <bigint> 类型） */
        yylval.s = strdup(yytext);
        LEX_RET(BIG_INTEGER);
    } else {
        castQ_push((int)CAST_NONE);
        yylval.ll = (long long)ullval;
        LEX_RET(INTEGER);
    }
}

"++"        { LEX_RET(PLUSPLUS); }
"--"        { LEX_RET(MINUSMINUS); }
"?."        { LEX_RET(SAFE_CALL); }
"??"        { LEX_RET(NULL_COALESCE); }
">="        { LEX_RET(GE); }
"<="        { LEX_RET(LE); }
"=="        { LEX_RET(EQ); }
"!="        { LEX_RET(NE); }
">"         { LEX_RET(GT); }
"<string>"         { yylval.ll = CAST_STRING; LEX_RET(TOK_TYPE_ANNOT); }
"<ascii>"          { yylval.ll = CAST_ASCII; LEX_RET(TOK_TYPE_ANNOT); }
"<int>"            { yylval.ll = CAST_INT; LEX_RET(TOK_TYPE_ANNOT); }
"<double>"         { yylval.ll = CAST_DOUBLE; LEX_RET(TOK_TYPE_ANNOT); }
"<bool>"           { yylval.ll = CAST_BOOL; LEX_RET(TOK_TYPE_ANNOT); }
"<char>"           { yylval.ll = CAST_CHAR; LEX_RET(TOK_TYPE_ANNOT); }
"<byte>"           { yylval.ll = CAST_BYTE; LEX_RET(TOK_TYPE_ANNOT); }
"<int8>"           { yylval.ll = CAST_INT8; LEX_RET(TOK_TYPE_ANNOT); }
"<int16>"          { yylval.ll = CAST_INT16; LEX_RET(TOK_TYPE_ANNOT); }
"<int32>"          { yylval.ll = CAST_INT32; LEX_RET(TOK_TYPE_ANNOT); }
"<int64>"          { yylval.ll = CAST_INT64; LEX_RET(TOK_TYPE_ANNOT); }
"<uint8>"          { yylval.ll = CAST_UINT8; LEX_RET(TOK_TYPE_ANNOT); }
"<uint16>"         { yylval.ll = CAST_UINT16; LEX_RET(TOK_TYPE_ANNOT); }
"<uint32>"         { yylval.ll = CAST_UINT32; LEX_RET(TOK_TYPE_ANNOT); }
"<uint>"           { yylval.ll = CAST_UINT; LEX_RET(TOK_TYPE_ANNOT); }
"<uint64>"         { yylval.ll = CAST_UINT64; LEX_RET(TOK_TYPE_ANNOT); }
"<long>"           { yylval.ll = CAST_LONG; LEX_RET(TOK_TYPE_ANNOT); }
"<long long>"      { yylval.ll = CAST_LONGLONG; LEX_RET(TOK_TYPE_ANNOT); }
"<float>"          { yylval.ll = CAST_FLOAT; LEX_RET(TOK_TYPE_ANNOT); }
"<ulong>"          { yylval.ll = CAST_ULONG; LEX_RET(TOK_TYPE_ANNOT); }
"<uchar>"          { yylval.ll = CAST_UCHAR; LEX_RET(TOK_TYPE_ANNOT); }
"<short>"          { yylval.ll = CAST_SHORT; LEX_RET(TOK_TYPE_ANNOT); }
"<ushort>"         { yylval.ll = CAST_USHORT; LEX_RET(TOK_TYPE_ANNOT); }
"<unsigned short>" { yylval.ll = CAST_USHORT; LEX_RET(TOK_TYPE_ANNOT); }
"<unsigned long>"  { yylval.ll = CAST_ULONG; LEX_RET(TOK_TYPE_ANNOT); }
"<unsigned char>"  { yylval.ll = CAST_UCHAR; LEX_RET(TOK_TYPE_ANNOT); }
"<size_t>"         { yylval.ll = CAST_SIZE_T; LEX_RET(TOK_TYPE_ANNOT); }
"<ssize_t>"        { yylval.ll = CAST_SSIZE_T; LEX_RET(TOK_TYPE_ANNOT); }
"<void>"           { yylval.ll = CAST_VOID; LEX_RET(TOK_TYPE_ANNOT); }
"<long double>"    { yylval.ll = CAST_LONG_DOUBLE; LEX_RET(TOK_TYPE_ANNOT); }
"<bigint>"         { yylval.ll = CAST_BIGINT; LEX_RET(TOK_TYPE_ANNOT); }
"<decimal>"        { yylval.ll = CAST_DECIMAL; LEX_RET(TOK_TYPE_ANNOT); }
"<bitdecimal>"     { yylval.ll = CAST_BITDECIMAL; LEX_RET(TOK_TYPE_ANNOT); }
"<ptr>"            { yylval.ll = CAST_PTR; LEX_RET(TOK_TYPE_ANNOT); }
"<pointer>"        { yylval.ll = CAST_PTR; LEX_RET(TOK_TYPE_ANNOT); }
"<handle>"         { yylval.ll = CAST_PTR; LEX_RET(TOK_TYPE_ANNOT); }
"<date>"           { LEX_RET(TOK_DATE); }
"<datetime>"       { LEX_RET(TOK_DATETIME); }
"<time>"           { LEX_RET(TOK_TIME_KW); }
"<timedelta>"      { LEX_RET(TOK_TIMEDELTA); }
"<calendar>"       { LEX_RET(TOK_CALENDAR); }
"<file>"           { LEX_RET(TOK_FILE); }
"<folder>"         { LEX_RET(TOK_FOLDER); }
"<formdata>"       { yylval.ll = CAST_FORMDATA; LEX_RET(TOK_TYPE_ANNOT); }
    /* ===== 泛型标记（Java 风格泛型形参/实参）=====
     * 值为尖括号内原文，语法层解析；整体匹配以避免 < 与比较运算符歧义。
     * 约定：泛型尖括号内侧不留空格（a <b,c> 才是泛型；a < b, c > 是比较） */
    /* 多项列表：<K,V> / <string,int> / <K extends Number,V?>（首字符必须非空格） */
"<"[A-Za-z_?][A-Za-z0-9_? ,]*","[A-Za-z0-9_? ,]*">" {
                    yylval.s = strndup(yytext + 1, (size_t)(yyleng - 2)); LEX_RET(TOK_GENERIC); }
"<?>"               { yylval.s = strdup("?"); LEX_RET(TOK_GENERIC); } /* 通配符类型引用 */
"<>"                { yylval.s = strdup(""); LEX_RET(TOK_GENERIC); }  /* diamond：类型推断占位 */
    /* 单标识符泛型引用：<T>/<Printable>/<map>
     * （已知基本类型 <int>/<string> 等由上方 TOK_TYPE_ANNOT 规则等长优先匹配） */
"<"[A-Za-z_][A-Za-z0-9_]*">" {
                    yylval.s = strndup(yytext + 1, (size_t)(yyleng - 2)); LEX_RET(TOK_GENERIC); }
"<"                { LEX_RET(LT); }
"&&"        { LEX_RET(AND); }
"||"        { LEX_RET(OR); }
"!"         { LEX_RET(NOT); }
"%"         { LEX_RET(MOD); }

"<<"        { LEX_RET(SHL); }
">>"        { LEX_RET(SHR); }
"&"         { LEX_RET(BAND); }
"|"         { LEX_RET(BOR); }
"^"         { LEX_RET(BXOR); }
"~"         { LEX_RET(BNOT); }

"+"             { LEX_RET(PLUS); }
"-"             { LEX_RET(MINUS); }
"**"            { LEX_RET(POW); }
"*"             { LEX_RET(MUL); }
"/"             { LEX_RET(DIV); }

"+="        { LEX_RET(PLUSEQ); }
"-="        { LEX_RET(MINUSEQ); }
"*="        { LEX_RET(MULEQ); }
"/="        { LEX_RET(DIVEQ); }
"%="        { LEX_RET(MODEQ); }

"&="        { LEX_RET(BANDEQ); }
"|="        { LEX_RET(BOREQ); }
"^="        { LEX_RET(BXOREQ); }
"<<="       { LEX_RET(SHLEQ); }
">>="       { LEX_RET(SHREQ); }

"?"         { LEX_RET(QMARK); }
":"         { if(in_case_expr) { in_case_expr = 0; LEX_RET(CASE_COLON); }
             LEX_RET(COLON); }

"=>"             { LEX_RET(ARROW); }
"="             { LEX_RET(ASSIGN); }
";"             { LEX_RET(SEMI); }

"("             { LEX_RET(LPAREN); }
")"             { LEX_RET(RPAREN); }
"{"             {
                  if(prev_token == TOK_GENERIC) {
                      /* 泛型标记后的 {：
                       * 声明头 class Box<T> { / class D extends P<K> { / implements I<K> { → LBRACE；
                       * 表达式中的泛型命名构造 Box<int>{...}、泛型 map 字面量 <K,V>{...} → MAP_OPEN。
                       * 声明头形态：紧邻为类型名 ID，再前一个是 class/struct/interface/
                       * extends/implements 关键字（implements 列表末项再前一个可能是逗号） */
                      if(prev_prev_token == ID &&
                         (prev_prev_prev_token == TOK_CLASS || prev_prev_prev_token == TOK_STRUCT ||
                          prev_prev_prev_token == TOK_INTERFACE || prev_prev_prev_token == TOK_EXTENDS ||
                          prev_prev_prev_token == TOK_IMPLEMENTS || prev_prev_prev_token == COMMA ||
                          /* 方法返回类型后的函数体：): Pair<K,V> { */
                          (prev_prev_prev_token == COLON && prev4_token == RPAREN))) {
                          LEX_RET(LBRACE);
                      }
                      prev_token = MAP_OPEN; return MAP_OPEN;
                  }
                  if(is_expr_end(prev_token)) {
                      /* 类型名后跟 { → 命名构造 ClassName{field: value}，按 MAP_OPEN 处理
                       * 排除声明上下文（extends/implements/struct/class/type 关键字后的类型名，
                       * 其 { 是类/结构体块而非命名构造）；
                       * 也排除函数返回类型标注上下文（func name(): Type {），
                       * 此时 prev_prev_token == COLON，{ 是函数体块 */
                      if(prev_token == ID && g_last_id_text[0] &&
                         prev_prev_token != TOK_EXTENDS && prev_prev_token != TOK_IMPLEMENTS &&
                         prev_prev_token != TOK_STRUCT && prev_prev_token != TOK_CLASS &&
                         prev_prev_token != TOK_TYPE && prev_prev_token != COLON &&
                         (struct_lookup(g_last_id_text) || class_lookup(g_last_id_text) ||
                          type_lookup(g_last_id_text) || is_socket_ctor_name(g_last_id_text))) {
                          prev_token = MAP_OPEN; return MAP_OPEN;
                      }
                      /* TOK_TYPE_ANNOT 后的 {：extends C<int> { → LBRACE；C<int>{...} → MAP_OPEN */
                      if(prev_token == TOK_TYPE_ANNOT &&
                         prev_prev_token == ID && g_last_id_text[0] &&
                         prev_prev_prev_token != TOK_EXTENDS && prev_prev_prev_token != TOK_IMPLEMENTS &&
                         prev_prev_prev_token != TOK_STRUCT && prev_prev_prev_token != TOK_CLASS &&
                         (struct_lookup(g_last_id_text) || class_lookup(g_last_id_text) ||
                          type_lookup(g_last_id_text) || is_socket_ctor_name(g_last_id_text))) {
                          prev_token = MAP_OPEN; return MAP_OPEN;
                      }
                      /* 裸类型标注 <formdata>{...} / <int>{...} → MAP_OPEN
                       * （prev_prev 非 ID，排除 C<int>{...} 命名构造与 extends C<int> { 声明头） */
                      if(prev_token == TOK_TYPE_ANNOT && prev_prev_token != ID) {
                          prev_token = MAP_OPEN; return MAP_OPEN;
                      }
                      LEX_RET(LBRACE);
                  }
                  else { prev_token = MAP_OPEN; return MAP_OPEN; } }
"}"             { LEX_RET(RBRACE); }
"["             {
                  /* TOK_TYPE_ANNOT 后的 [：类型化数组字面量 <int>[] / <int>[1,2,3] → ARRAY_OPEN
                   * （不能下标类型标注，必须按数组字面量处理） */
                  if(prev_token == TOK_TYPE_ANNOT) { prev_token = ARRAY_OPEN; return ARRAY_OPEN; }
                  if(is_expr_end(prev_token)) LEX_RET(LBRACKET);
                  else { prev_token = ARRAY_OPEN; return ARRAY_OPEN; } }
"]"             { LEX_RET(RBRACKET); }

","         { LEX_RET(COMMA); }

"//"[^\n]*           ;   /* 行注释 */
"/*"([^*]|\*+[^*/])*\*+"/" ;   /* 块注释 */



[ \t\r\n]       ;

.               {
    LEX_RET(ERROR);
}
%%

int yywrap(void) {
    return 1;
}
