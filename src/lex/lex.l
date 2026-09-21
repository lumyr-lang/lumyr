/* 编译期模块：prev_token/in_case_expr 为单线程词法状态，未来并发编译需实例化。 */
%{
#include "yacc.tab.h"
#include "lumyr_value_type.h"
#include "ast/ast_types.h"  /* struct_lookup / class_lookup / type_lookup */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <limits.h>

// 前一个 token：用于区分 `[` 是下标（表达式后）还是数组字面量（表达式开始）
static int prev_token = 0;
// 前前一个 token：用于判断 `{` 前的 ID 是否处于声明上下文（extends/implements/struct/class/type）
static int prev_prev_token = 0;

// 上一个 ID 文本：用于判断 `{` 前的 ID 是否为已注册类型名（ClassName{field: value} 命名构造）
static char g_last_id_text[256] = {0};

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
        /* 内置类型关键字：出现在冒号返回类型后，其后的 { 一定是块而非 map */
        case TOK_INT: case TOK_DOUBLE: case TOK_CHAR: case TOK_STRING:
        case TOK_BOOL: case TOK_ASCII: case TOK_BYTE:
        case TOK_INT8: case TOK_INT16: case TOK_INT32: case TOK_INT64:
        case TOK_UINT8: case TOK_UINT16: case TOK_UINT32: case TOK_UINT64:
        case TOK_UINT: case TOK_LONGLONG: case TOK_LONG: case TOK_FLOAT:
        case TOK_ULONG: case TOK_UCHAR: case TOK_SHORT: case TOK_USHORT:
        case TOK_SIZE_T: case TOK_SSIZE_T: case TOK_VOID:
        case TOK_LONG_DOUBLE: case TOK_PTR:
            return 1;
        default:
            return 0;
    }
}

#define LEX_RET(t) do { prev_prev_token = prev_token; prev_token = (t); return (t); } while(0)
%}

%option yylineno

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

f\"([^"\\]|\\.)*\"   {
    yylval.s = strdup(yytext+2);   /* 跳过 f 和开头引号 */
    yylval.s[strlen(yylval.s)-1] = '\0';
    LEX_RET(FSTRING_LIT);
}

'[^'\''\n]'      {
    yylval.ch = yytext[1];
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
"<"                { LEX_RET(LT); }
"&&"        { LEX_RET(AND); }
"||"        { LEX_RET(OR); }
"!"         { LEX_RET(NOT); }
"%"         { LEX_RET(MOD); }

"+"             { LEX_RET(PLUS); }
"-"             { LEX_RET(MINUS); }
"*"             { LEX_RET(MUL); }
"/"             { LEX_RET(DIV); }

"+="        { LEX_RET(PLUSEQ); }
"-="        { LEX_RET(MINUSEQ); }
"*="        { LEX_RET(MULEQ); }
"/="        { LEX_RET(DIVEQ); }

"?"         { LEX_RET(QMARK); }
":"         { if(in_case_expr) { in_case_expr = 0; LEX_RET(CASE_COLON); }
             LEX_RET(COLON); }

"="             { LEX_RET(ASSIGN); }
";"             { LEX_RET(SEMI); }

"("             { LEX_RET(LPAREN); }
")"             { LEX_RET(RPAREN); }
"{"             {
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
                         (struct_lookup(g_last_id_text) || class_lookup(g_last_id_text) || type_lookup(g_last_id_text))) {
                          prev_token = MAP_OPEN; return MAP_OPEN;
                      }
                      LEX_RET(LBRACE);
                  }
                  else { prev_token = MAP_OPEN; return MAP_OPEN; } }
"}"             { LEX_RET(RBRACE); }
"["             {
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
