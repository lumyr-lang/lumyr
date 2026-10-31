/*
 * ir_cgen.c - C 代码生成器（恢复中）
 *
 * 目标：将 BytecodeFunc 翻译为等价 C 代码，生成的二进制与 VM 输出一致。
 * 策略：IR 四栈作为来源，生成目标为 C 原生类型栈（局部变量）。
 */

#include "ir_cgen.h"
#include "bytecode_type.h"
#include "lumyr_value_type.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* FFI 声明列表（暂为空实现） */
void ffi_decl_add(const char* name, const char* libname, int ret_type, int* param_types, int param_count) {
    (void)name; (void)libname; (void)ret_type; (void)param_types; (void)param_count;
}
int ffi_decl_count(void) { return 0; }
FFIDecl* ffi_decl_get(int idx) { (void)idx; return NULL; }

/* 生成上下文 */
typedef struct {
    FILE* out;
    BytecodeFunc* fn;
    int label_counter;
    int indent;
} CGenCtx;

/* 跟踪 4 条栈上的临时变量名 */
typedef struct {
    char** val_names; int val_sp, val_cap;
    char** i64_names; int i64_sp, i64_cap;
    char** dbl_names; int dbl_sp, dbl_cap;
    char** ptr_names; int ptr_sp, ptr_cap;
    int has_returned; /* 已生成 return，避免 footer 再生成 */
} CGenStack;

static void cgen_indent(CGenCtx* ctx) {
    for(int i = 0; i < ctx->indent; i++) fprintf(ctx->out, "    ");
}

static void cgen_line(CGenCtx* ctx, const char* fmt, ...) {
    cgen_indent(ctx);
    va_list args;
    va_start(args, fmt);
    vfprintf(ctx->out, fmt, args);
    va_end(args);
    fprintf(ctx->out, "\n");
}

static void push_name(CGenCtx* ctx, char*** arr, int* sp, int* cap, const char* name) {
    (void)ctx;
    if(*sp >= *cap) {
        *cap = *cap ? *cap * 2 : 8;
        *arr = realloc(*arr, sizeof(char*) * (*cap));
    }
    (*arr)[(*sp)++] = strdup(name);
}

static char* pop_name(CGenCtx* ctx, char*** arr, int* sp) {
    (void)ctx;
    if(*sp <= 0) return strdup("__stack_underflow__");
    return (*arr)[--(*sp)];
}

static const char* var_name(CGenCtx* ctx, int idx) {
    static char buf[256];
    if(idx < 0 || idx >= ctx->fn->sym_cnt) {
        snprintf(buf, sizeof(buf), "__var_%d", idx);
    } else {
        const char* s = ctx->fn->syms[idx];
        if(!s || !*s) snprintf(buf, sizeof(buf), "__var_%d", idx);
        else snprintf(buf, sizeof(buf), "v_%s", s);
    }
    return buf;
}

static const char* c_type_for_cast_kind(CastKind ck) {
    switch(ck) {
        case CAST_INT: case CAST_INT8: case CAST_INT16: case CAST_INT32: case CAST_INT64:
        case CAST_SHORT: case CAST_LONG: case CAST_LONGLONG:
        case CAST_SSIZE_T:
            return "int64_t";
        case CAST_UINT8: case CAST_UINT16: case CAST_UINT32: case CAST_UINT:
        case CAST_UINT64: case CAST_ULONG: case CAST_USHORT: case CAST_UCHAR:
        case CAST_BYTE: case CAST_SIZE_T:
            return "uint64_t";
        case CAST_BOOL:
            return "int";
        case CAST_CHAR:
            return "char";
        case CAST_DOUBLE: case CAST_FLOAT: case CAST_LONG_DOUBLE:
            return "double";
        case CAST_STRING:
            return "const char*";
        default:
            return "Value";
    }
}

static const char* trunc_expr_for_cast_kind(const char* val, CastKind ck) {
    static char buf[256];
    switch(ck) {
        case CAST_INT: case CAST_INT32:
            snprintf(buf, sizeof(buf), "(int64_t)(int)(%s)", val); break;
        case CAST_INT8:
            snprintf(buf, sizeof(buf), "(int64_t)(int8_t)(%s)", val); break;
        case CAST_INT16:
            snprintf(buf, sizeof(buf), "(int64_t)(int16_t)(%s)", val); break;
        case CAST_SHORT:
            snprintf(buf, sizeof(buf), "(int64_t)(short)(%s)", val); break;
        case CAST_LONG:
            snprintf(buf, sizeof(buf), "(int64_t)(long)(%s)", val); break;
        case CAST_LONGLONG:
            snprintf(buf, sizeof(buf), "(int64_t)(long long)(%s)", val); break;
        case CAST_UINT8: case CAST_BYTE: case CAST_UCHAR:
            snprintf(buf, sizeof(buf), "(int64_t)(uint8_t)(%s)", val); break;
        case CAST_UINT16: case CAST_USHORT:
            snprintf(buf, sizeof(buf), "(int64_t)(uint16_t)(%s)", val); break;
        case CAST_UINT32: case CAST_UINT:
            snprintf(buf, sizeof(buf), "(int64_t)(uint32_t)(%s)", val); break;
        case CAST_UINT64: case CAST_ULONG: case CAST_SIZE_T:
            snprintf(buf, sizeof(buf), "(int64_t)(uint64_t)(%s)", val); break;
        case CAST_BOOL:
            snprintf(buf, sizeof(buf), "(int64_t)(!!(%s))", val); break;
        case CAST_CHAR:
            snprintf(buf, sizeof(buf), "(int64_t)(char)(%s)", val); break;
        case CAST_SSIZE_T:
            snprintf(buf, sizeof(buf), "(int64_t)(ssize_t)(%s)", val); break;
        default:
            snprintf(buf, sizeof(buf), "%s", val); break;
    }
    return buf;
}

static void emit_const_pool(CGenCtx* ctx) {
    cgen_line(ctx, "/* 常量池 */");
    for(int i = 0; i < ctx->fn->const_cnt; i++) {
        ConstEntry* e = &ctx->fn->const_pool[i];
        switch(e->type) {
            case CONST_STRING:
                cgen_line(ctx, "static const char* cstr_%d = \"%s\";", i, e->s ? e->s : "");
                break;
            case CONST_INT64:
                cgen_line(ctx, "static const int64_t ci64_%d = %lldLL;", i, (long long)e->i64);
                break;
            case CONST_UINT64:
                cgen_line(ctx, "static const uint64_t cu64_%d = %lluULL;", i, (unsigned long long)e->u64);
                break;
            case CONST_DOUBLE:
                cgen_line(ctx, "static const double cdbl_%d = %.17g;", i, e->d);
                break;
        }
    }
    if(ctx->fn->const_cnt == 0) cgen_line(ctx, "/* (empty) */");
}

static void emit_local_decls(CGenCtx* ctx) {
    cgen_line(ctx, "/* 局部变量 */");
    for(int i = 0; i < ctx->fn->sym_cnt; i++) {
        if(ctx->fn->var_is_global && ctx->fn->var_is_global[i]) continue;
        const char* name = var_name(ctx, i);
        CastKind ck = CAST_NONE;
        if(ctx->fn->var_type_tags) ck = ctx->fn->var_type_tags[i];
        if(ck == CAST_NONE) ck = CAST_INT; /* 无标记时默认 int */
        const char* ctype = c_type_for_cast_kind(ck);
        cgen_line(ctx, "%s %s = 0;", ctype, name);
    }
    if(ctx->fn->sym_cnt == 0) cgen_line(ctx, "/* (none) */");
}

static void emit_func_header(CGenCtx* ctx) {
    cgen_line(ctx, "/* 函数: %s */", ctx->fn->name ? ctx->fn->name : "main");
    cgen_line(ctx, "static int lumyr_main(void) {");
    ctx->indent++;
}

static void emit_func_footer(CGenCtx* ctx, CGenStack* stk) {
    if(!stk->has_returned) {
        cgen_line(ctx, "return 0;");
    }
    ctx->indent--;
    cgen_line(ctx, "}");
}

static const char* c_op_for_int64(OpCode op) {
    switch(op) {
        case OPC_INT64_ADD: return "+";
        case OPC_INT64_SUB: return "-";
        case OPC_INT64_MUL: return "*";
        case OPC_INT64_DIV: return "/";
        case OPC_INT64_MOD: return "%";
        case OPC_INT64_BAND: return "&";
        case OPC_INT64_BOR: return "|";
        case OPC_INT64_BXOR: return "^";
        case OPC_INT64_SHL: return "<<";
        case OPC_INT64_SHR: return ">>";
        default: return "?";
    }
}

static const char* c_op_for_double(OpCode op) {
    switch(op) {
        case OPC_DOUBLE_ADD: return "+";
        case OPC_DOUBLE_SUB: return "-";
        case OPC_DOUBLE_MUL: return "*";
        case OPC_DOUBLE_DIV: return "/";
        default: return "?";
    }
}

static void emit_instruction(CGenCtx* ctx, CGenStack* stk, int pc, Instruction* in) {
    (void)pc;
    char tmp[64];
    switch(in->op) {
        case OPC_PUSH_CONST_IDX: {
            ConstEntry* e = &ctx->fn->const_pool[in->a];
            if(e->type == CONST_STRING) {
                snprintf(tmp, sizeof(tmp), "p%d", pc);
                cgen_line(ctx, "const char* %s = cstr_%d;", tmp, in->a);
                push_name(ctx, &stk->ptr_names, &stk->ptr_sp, &stk->ptr_cap, tmp);
            } else if(e->type == CONST_INT64) {
                snprintf(tmp, sizeof(tmp), "i%d", pc);
                cgen_line(ctx, "int64_t %s = ci64_%d;", tmp, in->a);
                push_name(ctx, &stk->i64_names, &stk->i64_sp, &stk->i64_cap, tmp);
            } else if(e->type == CONST_UINT64) {
                snprintf(tmp, sizeof(tmp), "u%d", pc);
                cgen_line(ctx, "uint64_t %s = cu64_%d;", tmp, in->a);
                push_name(ctx, &stk->i64_names, &stk->i64_sp, &stk->i64_cap, tmp);
            } else if(e->type == CONST_DOUBLE) {
                snprintf(tmp, sizeof(tmp), "d%d", pc);
                cgen_line(ctx, "double %s = cdbl_%d;", tmp, in->a);
                push_name(ctx, &stk->dbl_names, &stk->dbl_sp, &stk->dbl_cap, tmp);
            }
            break;
        }
        case OPC_PUSH_INT64_CONST: {
            snprintf(tmp, sizeof(tmp), "i%d", pc);
            cgen_line(ctx, "int64_t %s = %dLL;", tmp, in->a);
            push_name(ctx, &stk->i64_names, &stk->i64_sp, &stk->i64_cap, tmp);
            break;
        }
        case OPC_INT64_TRUNC: {
            char* v = pop_name(ctx, &stk->i64_names, &stk->i64_sp);
            CastKind ck = (CastKind)in->a;
            snprintf(tmp, sizeof(tmp), "i%d", pc);
            cgen_line(ctx, "int64_t %s = %s;", tmp, trunc_expr_for_cast_kind(v, ck));
            push_name(ctx, &stk->i64_names, &stk->i64_sp, &stk->i64_cap, tmp);
            free(v);
            break;
        }
        case OPC_STORE_INT64_VAR: {
            char* v = pop_name(ctx, &stk->i64_names, &stk->i64_sp);
            cgen_line(ctx, "%s = %s;", var_name(ctx, in->a), v);
            free(v);
            break;
        }
        case OPC_LOAD_INT64_VAR: {
            snprintf(tmp, sizeof(tmp), "i%d", pc);
            cgen_line(ctx, "int64_t %s = %s;", tmp, var_name(ctx, in->a));
            push_name(ctx, &stk->i64_names, &stk->i64_sp, &stk->i64_cap, tmp);
            break;
        }
        case OPC_STORE_DOUBLE_VAR: {
            char* v = pop_name(ctx, &stk->dbl_names, &stk->dbl_sp);
            cgen_line(ctx, "%s = %s;", var_name(ctx, in->a), v);
            free(v);
            break;
        }
        case OPC_LOAD_DOUBLE_VAR: {
            snprintf(tmp, sizeof(tmp), "d%d", pc);
            cgen_line(ctx, "double %s = %s;", tmp, var_name(ctx, in->a));
            push_name(ctx, &stk->dbl_names, &stk->dbl_sp, &stk->dbl_cap, tmp);
            break;
        }
        case OPC_STORE_PTR_VAR: {
            char* v = pop_name(ctx, &stk->ptr_names, &stk->ptr_sp);
            cgen_line(ctx, "%s = %s;", var_name(ctx, in->a), v);
            free(v);
            break;
        }
        case OPC_LOAD_PTR_VAR: {
            snprintf(tmp, sizeof(tmp), "p%d", pc);
            cgen_line(ctx, "const char* %s = %s;", tmp, var_name(ctx, in->a));
            push_name(ctx, &stk->ptr_names, &stk->ptr_sp, &stk->ptr_cap, tmp);
            break;
        }
        case OPC_INT64_ADD: case OPC_INT64_SUB: case OPC_INT64_MUL: case OPC_INT64_DIV: case OPC_INT64_MOD:
        case OPC_INT64_BAND: case OPC_INT64_BOR: case OPC_INT64_BXOR: case OPC_INT64_SHL: case OPC_INT64_SHR: {
            char* rhs = pop_name(ctx, &stk->i64_names, &stk->i64_sp);
            char* lhs = pop_name(ctx, &stk->i64_names, &stk->i64_sp);
            snprintf(tmp, sizeof(tmp), "i%d", pc);
            if(in->op == OPC_INT64_DIV || in->op == OPC_INT64_MOD) {
                cgen_line(ctx, "int64_t %s = (%s != 0) ? (%s %s %s) : 0;", tmp, rhs, lhs, c_op_for_int64(in->op), rhs);
            } else {
                cgen_line(ctx, "int64_t %s = %s %s %s;", tmp, lhs, c_op_for_int64(in->op), rhs);
            }
            push_name(ctx, &stk->i64_names, &stk->i64_sp, &stk->i64_cap, tmp);
            free(lhs); free(rhs);
            break;
        }
        case OPC_DOUBLE_ADD: case OPC_DOUBLE_SUB: case OPC_DOUBLE_MUL: case OPC_DOUBLE_DIV: {
            char* rhs = pop_name(ctx, &stk->dbl_names, &stk->dbl_sp);
            char* lhs = pop_name(ctx, &stk->dbl_names, &stk->dbl_sp);
            snprintf(tmp, sizeof(tmp), "d%d", pc);
            if(in->op == OPC_DOUBLE_DIV) {
                cgen_line(ctx, "double %s = (%s != 0.0) ? (%s %s %s) : 0.0;", tmp, rhs, lhs, c_op_for_double(in->op), rhs);
            } else {
                cgen_line(ctx, "double %s = %s %s %s;", tmp, lhs, c_op_for_double(in->op), rhs);
            }
            push_name(ctx, &stk->dbl_names, &stk->dbl_sp, &stk->dbl_cap, tmp);
            free(lhs); free(rhs);
            break;
        }
        case OPC_PRINT_PTR: {
            char* v = pop_name(ctx, &stk->ptr_names, &stk->ptr_sp);
            cgen_line(ctx, "if(%s) printf(\"%%s\\n\", %s); else printf(\"(null)\\n\");", v, v);
            free(v);
            break;
        }
        case OPC_PRINT_INT64: {
            char* v = pop_name(ctx, &stk->i64_names, &stk->i64_sp);
            CastKind ck = (CastKind)in->a;
            switch(ck) {
                case CAST_UINT8: case CAST_UINT16: case CAST_UINT32: case CAST_UINT:
                case CAST_UINT64: case CAST_ULONG: case CAST_USHORT: case CAST_UCHAR:
                case CAST_BYTE: case CAST_SIZE_T:
                    cgen_line(ctx, "printf(\"%%llu\\n\", (unsigned long long)%s);", v);
                    break;
                case CAST_CHAR:
                    cgen_line(ctx, "printf(\"%%c\\n\", (char)%s);", v);
                    break;
                case CAST_BOOL:
                    cgen_line(ctx, "printf(\"%%s\\n\", %s ? \"true\" : \"false\");", v);
                    break;
                default:
                    cgen_line(ctx, "printf(\"%%lld\\n\", (long long)%s);", v);
                    break;
            }
            free(v);
            break;
        }
        case OPC_PRINT_DOUBLE: {
            char* v = pop_name(ctx, &stk->dbl_names, &stk->dbl_sp);
            cgen_line(ctx, "printf(\"%%g\\n\", %s);", v);
            free(v);
            break;
        }
        case OPC_INT64_TO_DOUBLE: {
            char* v = pop_name(ctx, &stk->i64_names, &stk->i64_sp);
            snprintf(tmp, sizeof(tmp), "d%d", pc);
            cgen_line(ctx, "double %s = (double)%s;", tmp, v);
            push_name(ctx, &stk->dbl_names, &stk->dbl_sp, &stk->dbl_cap, tmp);
            free(v);
            break;
        }
        case OPC_DOUBLE_TO_INT64: {
            char* v = pop_name(ctx, &stk->dbl_names, &stk->dbl_sp);
            snprintf(tmp, sizeof(tmp), "i%d", pc);
            cgen_line(ctx, "int64_t %s = (int64_t)%s;", tmp, v);
            push_name(ctx, &stk->i64_names, &stk->i64_sp, &stk->i64_cap, tmp);
            free(v);
            break;
        }
        case OPC_INT64_TO_STRING: {
            char* v = pop_name(ctx, &stk->i64_names, &stk->i64_sp);
            snprintf(tmp, sizeof(tmp), "p%d", pc);
            cgen_line(ctx, "char* %s = NULL; { char __buf[32]; snprintf(__buf, sizeof(__buf), \"%%lld\", (long long)%s); %s = strdup(__buf); }", tmp, v, tmp);
            push_name(ctx, &stk->ptr_names, &stk->ptr_sp, &stk->ptr_cap, tmp);
            free(v);
            break;
        }
        case OPC_DOUBLE_TO_STRING: {
            char* v = pop_name(ctx, &stk->dbl_names, &stk->dbl_sp);
            snprintf(tmp, sizeof(tmp), "p%d", pc);
            cgen_line(ctx, "char* %s = NULL; { char __buf[64]; snprintf(__buf, sizeof(__buf), \"%%g\", %s); %s = strdup(__buf); }", tmp, v, tmp);
            push_name(ctx, &stk->ptr_names, &stk->ptr_sp, &stk->ptr_cap, tmp);
            free(v);
            break;
        }
        case OPC_RETURN: {
            stk->has_returned = 1;
            cgen_line(ctx, "return 0;");
            break;
        }
        default:
            cgen_line(ctx, "/* TODO: op=%s a=%d b=%d */", opc_name(in->op), in->a, in->b);
            break;
    }
}

void ir_cgen_file(const char* out_c_path, BytecodeFunc* main_fn) {
    FILE* out = fopen(out_c_path, "w");
    if(!out) return;

    CGenCtx ctx = { out, main_fn, 0, 0 };
    CGenStack stk = {0};

    fprintf(out, "/* Generated by lumyr C code generator */\n");
    fprintf(out, "#include <stdint.h>\n");
    fprintf(out, "#include <stdio.h>\n");
    fprintf(out, "\n");

    emit_const_pool(&ctx);
    fprintf(out, "\n");
    emit_func_header(&ctx);
    emit_local_decls(&ctx);
    fprintf(out, "\n");

    for(int pc = 0; pc < main_fn->code_len; pc++) {
        emit_instruction(&ctx, &stk, pc, &main_fn->code[pc]);
    }

    emit_func_footer(&ctx, &stk);
    fprintf(out, "\nint main(void) {\n    return lumyr_main();\n}\n");

    fclose(out);

    /* 清理 */
    for(int i = 0; i < stk.val_sp; i++) free(stk.val_names[i]);
    for(int i = 0; i < stk.i64_sp; i++) free(stk.i64_names[i]);
    for(int i = 0; i < stk.dbl_sp; i++) free(stk.dbl_names[i]);
    for(int i = 0; i < stk.ptr_sp; i++) free(stk.ptr_names[i]);
    free(stk.val_names); free(stk.i64_names); free(stk.dbl_names); free(stk.ptr_names);
}
