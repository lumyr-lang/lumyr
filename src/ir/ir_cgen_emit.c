/*
 * ir_cgen 模块：指令发射主循环
 * 自动从 ir_cgen.c 拆分
 */
#include "ir_cgen_internal.h"

/* 全局方法名到 vtable 索引的查找函数（定义在 ir_cgen.c 中） */
int find_method_index(const char* name);

/* 非虚方法红黑树（定义在 ir_cgen.c 中，用于静态分派优化） */
extern struct RBTree* g_nonvirtual_methods;
/* 红黑树查找函数（定义在 rbtree.c 中） */
void* rbtree_find(struct RBTree* tree, const char* class_name, const char* method_name);

/* 用于检查函数名是否是某个 class 的方法的回调函数 */
typedef struct {
    const char* method_name;
    int found;
} ClassMethodCheckCtx;

static void check_class_method_cb(const char* name, TypeDef* td, void* user_data)
{
    (void)name;
    ClassMethodCheckCtx* ctx = (ClassMethodCheckCtx*)user_data;
    if(ctx->found) return;
    if(td && td->is_class) {
        for(int i = 0; i < td->nmethods; i++) {
            if(strcmp(td->method_names[i], ctx->method_name) == 0) {
                ctx->found = 1;
                return;
            }
        }
    }
}

/* 用于生成 class 方法动态分派 if-else 链的回调函数 */
typedef struct {
    FILE* out;
    const char* method_name;
    int fixed;
    int nbind;
    int* first_class;
} ClassDispatchCtx;

static void emit_class_dispatch_cb(const char* name, TypeDef* td, void* user_data)
{
    (void)name;
    ClassDispatchCtx* ctx = (ClassDispatchCtx*)user_data;
    if(td && td->is_class) {
        /* 检查这个 class 或其父类是否有这个方法（继承链查找） */
        TypeDef* method_class = NULL;
        TypeDef* cur = td;
        while(cur) {
            int has_method = 0;
            for(int i = 0; i < cur->nmethods; i++) {
                if(strcmp(cur->method_names[i], ctx->method_name) == 0) {
                    has_method = 1;
                    break;
                }
            }
            if(has_method) {
                method_class = cur;
                break;
            }
            /* 查找父类 */
            if(cur->parent) {
                cur = class_lookup(cur->parent);
            } else {
                cur = NULL;
            }
        }
        if(method_class) {
            if(*(ctx->first_class)) {
                fprintf(ctx->out, "                if(strcmp(__cn_str, \"%s\") == 0) {\n", td->name);
                *(ctx->first_class) = 0;
            } else {
                fprintf(ctx->out, "                else if(strcmp(__cn_str, \"%s\") == 0) {\n", td->name);
            }
            /* 生成调用这个 class 方法的代码（可能是父类的方法） */
            fprintf(ctx->out, "                    __stk[__sp++] = lumyr_func_%s_%s_%d(", method_class->name, ctx->method_name, ctx->fixed);
            for(int k = 0; k < ctx->fixed; k++) {
                if(k) fprintf(ctx->out, ", ");
                if(k < ctx->nbind) fprintf(ctx->out, "__args[%d]", k);
                else fprintf(ctx->out, "val_none()");
            }
            fprintf(ctx->out, ");\n");
            fprintf(ctx->out, "                }\n");
        }
    }
}

static int g_nested_ptr_counter = 0;

/* 前向声明 */
static const char* emit_tag_to_ctype(int tag);

/* 查找变量的类型标记（-1 表示无精确类型） */
static int emit_get_var_tag(const BytecodeFunc* fn, const char* name) {
    if(!fn || !fn->var_type_tags || !name) return -1;
    for(int i = 0; i < fn->sym_cnt; i++) {
        if(fn->syms[i] && strcmp(fn->syms[i], name) == 0) return fn->var_type_tags[i];
    }
    return -1;
}

/* 查找变量的 struct 类型名（NULL 表示不是 struct） */
static const char* emit_get_var_struct_name(const BytecodeFunc* fn, const char* name) {
    if(!fn || !fn->var_struct_names || !name) return NULL;
    for(int i = 0; i < fn->sym_cnt; i++) {
        if(fn->syms[i] && strcmp(fn->syms[i], name) == 0) return fn->var_struct_names[i];
    }
    /* 如果当前函数中找不到，尝试在 main 函数中查找（全局变量） */
    if(g_main_fn && g_main_fn != fn && g_main_fn->var_struct_names) {
        for(int i = 0; i < g_main_fn->sym_cnt; i++) {
            if(g_main_fn->syms[i] && strcmp(g_main_fn->syms[i], name) == 0) return g_main_fn->var_struct_names[i];
        }
    }
    return NULL;
}

/* 解析变量的结构体类型：
   - 如果名字以 "class:" 开头，说明是 class 类型，返回 1，type_name 指向 class 名
   - 否则是 struct 类型，返回 0，type_name 指向 struct 名
   返回的 type_name 指向原始字符串中的名字部分（不需要释放） */
static int emit_parse_struct_type(const char* name, const char** type_name) {
    if(!name) { *type_name = NULL; return 0; }
    if(strncmp(name, "class:", 6) == 0) {
        *type_name = name + 6;
        return 1;  /* class 类型 */
    }
    *type_name = name;
    return 0;  /* struct 类型 */
}

/* 生成把 C struct 转换为 Value(Map) 的代码 */
void emit_struct_to_value(const char* struct_name, const char* var_expr) {
    /* 处理 class: 前缀：class 类型用 type_lookup，struct 类型用 struct_lookup */
    const char* type_name = struct_name;
    TypeDef* td = NULL;
    if(struct_name && strncmp(struct_name, "class:", 6) == 0) {
        type_name = struct_name + 6;
        td = type_lookup(type_name);
    } else {
        td = struct_lookup(struct_name);
    }
    if(!td || td->nprops <= 0) {
        fprintf(out, "    { Value __v = {0}; __stk[__sp++] = __v; }\n");
        return;
    }
    int nkv = td->nprops + 2;  // +2 for __mapname__ and __structname__
    fprintf(out, "    {\n");
    fprintf(out, "        Value __kv[%d];\n", 2 * nkv);
    for(int i = 0; i < td->nprops; i++) {
        int ck = td->field_cast_kinds ? td->field_cast_kinds[i] : CAST_LONGLONG;
        const char* fname = td->props[i];
        const char* nested_sname = (td->field_struct_names) ? td->field_struct_names[i] : NULL;
        fprintf(out, "        __kv[%d] = lumyr_make_string(\"%s\");\n", 2*i, fname);
        if(nested_sname) {
            /* 嵌套 struct 字段：递归转换成 Value(map)，存入临时变量 */
            fprintf(out, "        { lumyr_struct_%s* __nested_p = &%s->%s;\n", nested_sname, var_expr, fname);
            fprintf(out, "          Value __nested_kv[4];\n");
            fprintf(out, "          __nested_kv[0] = lumyr_make_string(\"__mapname__\");\n");
            fprintf(out, "          __nested_kv[1] = lumyr_make_string(\"%s\");\n", nested_sname);
            fprintf(out, "          __nested_kv[2] = lumyr_make_string(\"__structname__\");\n");
            fprintf(out, "          __nested_kv[3] = lumyr_make_string(\"%s\");\n", nested_sname);
            fprintf(out, "          __kv[%d] = lumyr_map_lit(__nested_kv, 2);\n", 2*i+1);
            fprintf(out, "        }\n");
        } else if(ck == CAST_DOUBLE || ck == CAST_FLOAT || ck == CAST_LONG_DOUBLE) {
            fprintf(out, "        __kv[%d] = lumyr_make_double((double)%s->%s);\n", 2*i+1, var_expr, fname);
        } else if(ck == CAST_STRING) {
            fprintf(out, "        __kv[%d] = lumyr_make_string(%s->%s);\n", 2*i+1, var_expr, fname);
        } else if(ck == CAST_BOOL) {
            fprintf(out, "        __kv[%d] = lumyr_make_bool((int)%s->%s);\n", 2*i+1, var_expr, fname);
        } else {
            fprintf(out, "        __kv[%d] = lumyr_make_int((long long)%s->%s);\n", 2*i+1, var_expr, fname);
        }
    }
    fprintf(out, "        __kv[%d] = lumyr_make_string(\"__mapname__\");\n", 2*td->nprops);
    fprintf(out, "        __kv[%d] = lumyr_make_string(\"%s\");\n", 2*td->nprops+1, struct_name);
    fprintf(out, "        __kv[%d] = lumyr_make_string(\"__structname__\");\n", 2*td->nprops+2);
    fprintf(out, "        __kv[%d] = lumyr_make_string(\"%s\");\n", 2*td->nprops+3, struct_name);
    fprintf(out, "        Value __v = lumyr_map_lit(__kv, %d);\n", nkv);
    fprintf(out, "        __stk[__sp++] = __v;\n");
    fprintf(out, "    }\n");
}

/* 生成把 Value(Map 或 VAL_STRUCT_PTR) 转换为 C struct 的代码 */
void emit_value_to_struct(const char* struct_name, const char* var_expr, const char* value_expr) {
    TypeDef* td = struct_lookup(struct_name);
    if(!td || td->nprops <= 0) return;
    /* 运行时类型检查：如果 value_expr 是 VAL_STRUCT_PTR 类型，直接结构体拷贝 */
    fprintf(out, "        if(%s.type == VAL_STRUCT_PTR) { memcpy(%s, %s.v.struct_ptr, sizeof(lumyr_struct_%s)); } else {\n", value_expr, var_expr, value_expr, struct_name);
    for(int i = 0; i < td->nprops; i++) {
        int ck = td->field_cast_kinds ? td->field_cast_kinds[i] : CAST_LONGLONG;
        const char* fname = td->props[i];
        const char* nested_sname = td->field_struct_names ? td->field_struct_names[i] : NULL;
        if(nested_sname) {
            /* 嵌套 struct 字段：先把地址赋值给临时指针，再递归转换 */
            static char tmp_ptr[256];
            snprintf(tmp_ptr, sizeof(tmp_ptr), "__nested_%s_%d", fname, g_nested_ptr_counter++);
            fprintf(out, "        lumyr_struct_%s* %s = &%s->%s;\n", nested_sname, tmp_ptr, var_expr, fname);
            static char nested_val[256];
            snprintf(nested_val, sizeof(nested_val), "lumyr_map_get(%s, lumyr_make_string(\"%s\"))", value_expr, fname);
            emit_value_to_struct(nested_sname, tmp_ptr, nested_val);
        } else {
            const char* ctype = emit_tag_to_ctype(ck);
            if(!ctype) ctype = "int64_t";
            if(ck == CAST_DOUBLE || ck == CAST_FLOAT || ck == CAST_LONG_DOUBLE) {
                fprintf(out, "        %s->%s = (%s)lumyr_map_get(%s, lumyr_make_string(\"%s\")).v.d;\n", var_expr, fname, ctype, value_expr, fname);
            } else if(ck == CAST_STRING) {
                fprintf(out, "        %s->%s = (%s)lumyr_str_cstr(&lumyr_map_get(%s, lumyr_make_string(\"%s\")));\n", var_expr, fname, ctype, value_expr, fname);
            } else if(ck == CAST_BOOL) {
                fprintf(out, "        %s->%s = (%s)lumyr_map_get(%s, lumyr_make_string(\"%s\")).v.b;\n", var_expr, fname, ctype, value_expr, fname);
            } else {
                fprintf(out, "        %s->%s = (%s)lumyr_map_get(%s, lumyr_make_string(\"%s\")).v.i;\n", var_expr, fname, ctype, value_expr, fname);
            }
        }
    }
    fprintf(out, "        }\n");
}

/* CastKind 转 C 类型名（NULL 表示保持 Value） */
static const char* emit_tag_to_ctype(int tag) {
    switch(tag) {
        case CAST_INT: case CAST_INT32: return "int";
        case CAST_LONGLONG: case CAST_INT64: return "long long";
        case CAST_LONG: return "long";
        case CAST_SHORT: case CAST_INT16: return "short";
        case CAST_CHAR: case CAST_INT8: return "char";
        case CAST_UCHAR: case CAST_UINT8: case CAST_BYTE: return "unsigned char";
        case CAST_USHORT: case CAST_UINT16: return "unsigned short";
        case CAST_UINT32: return "unsigned int";
        case CAST_ULONG: case CAST_UINT64: return "unsigned long long";
        case CAST_FLOAT: return "float";
        case CAST_DOUBLE: case CAST_LONG_DOUBLE: return "double";
        case CAST_BOOL: return "int";
        case CAST_STRING: return "char*";
        default: return NULL;
    }
}

/* 生成精确类型变量读取并转为 Value 的代码表达式 */
static const char* emit_precise_load(int tag, const char* var_expr) {
    static char buf[512];
    switch(tag) {
        case CAST_INT: case CAST_INT32: snprintf(buf, sizeof(buf), "lumyr_make_int((long long)(int)%s)", var_expr); break;
        case CAST_LONGLONG: case CAST_INT64: case CAST_LONG: snprintf(buf, sizeof(buf), "lumyr_make_int((long long)%s)", var_expr); break;
        case CAST_SHORT: case CAST_INT16: snprintf(buf, sizeof(buf), "lumyr_make_int((long long)(short)%s)", var_expr); break;
        case CAST_CHAR: case CAST_INT8: snprintf(buf, sizeof(buf), "lumyr_make_int((long long)(char)%s)", var_expr); break;
        case CAST_UCHAR: case CAST_UINT8: case CAST_BYTE: snprintf(buf, sizeof(buf), "lumyr_make_int((long long)(unsigned char)%s)", var_expr); break;
        case CAST_USHORT: case CAST_UINT16: snprintf(buf, sizeof(buf), "lumyr_make_int((long long)(unsigned short)%s)", var_expr); break;
        case CAST_UINT32: snprintf(buf, sizeof(buf), "lumyr_make_int((long long)(unsigned int)%s)", var_expr); break;
        case CAST_ULONG: case CAST_UINT64: snprintf(buf, sizeof(buf), "lumyr_make_int((long long)(unsigned long long)%s)", var_expr); break;
        case CAST_FLOAT: snprintf(buf, sizeof(buf), "lumyr_make_double((double)(float)%s)", var_expr); break;
        case CAST_DOUBLE: case CAST_LONG_DOUBLE: snprintf(buf, sizeof(buf), "lumyr_make_double((double)%s)", var_expr); break;
        case CAST_BOOL: snprintf(buf, sizeof(buf), "lumyr_make_int(%s ? 1 : 0)", var_expr); break;
        default: snprintf(buf, sizeof(buf), "%s", var_expr); break;
    }
    return buf;
}

/* 生成 Value 转为精确类型的代码表达式（参数为 Value 变量名） */
static const char* emit_precise_store(int tag, const char* val_expr) {
    static char buf[512];
    switch(tag) {
        case CAST_INT: case CAST_INT32: snprintf(buf, sizeof(buf), "(int)((%s).type == VAL_DOUBLE ? (long long)(%s).v.d : (%s).v.i)", val_expr, val_expr, val_expr); break;
        case CAST_LONGLONG: case CAST_INT64: case CAST_LONG: snprintf(buf, sizeof(buf), "((%s).type == VAL_DOUBLE ? (long long)(%s).v.d : (%s).v.i)", val_expr, val_expr, val_expr); break;
        case CAST_SHORT: case CAST_INT16: snprintf(buf, sizeof(buf), "(short)((%s).type == VAL_DOUBLE ? (long long)(%s).v.d : (%s).v.i)", val_expr, val_expr, val_expr); break;
        case CAST_CHAR: case CAST_INT8: snprintf(buf, sizeof(buf), "(char)((%s).type == VAL_DOUBLE ? (long long)(%s).v.d : (%s).v.i)", val_expr, val_expr, val_expr); break;
        case CAST_UCHAR: case CAST_UINT8: case CAST_BYTE: snprintf(buf, sizeof(buf), "(unsigned char)((%s).type == VAL_DOUBLE ? (long long)(%s).v.d : (%s).v.i)", val_expr, val_expr, val_expr); break;
        case CAST_USHORT: case CAST_UINT16: snprintf(buf, sizeof(buf), "(unsigned short)((%s).type == VAL_DOUBLE ? (long long)(%s).v.d : (%s).v.i)", val_expr, val_expr, val_expr); break;
        case CAST_UINT32: snprintf(buf, sizeof(buf), "(unsigned int)((%s).type == VAL_DOUBLE ? (long long)(%s).v.d : (%s).v.i)", val_expr, val_expr, val_expr); break;
        case CAST_ULONG: case CAST_UINT64: snprintf(buf, sizeof(buf), "(unsigned long long)((%s).type == VAL_DOUBLE ? (long long)(%s).v.d : (%s).v.i)", val_expr, val_expr, val_expr); break;
        case CAST_FLOAT: snprintf(buf, sizeof(buf), "(float)((%s).type == VAL_INT ? (double)(%s).v.i : (%s).v.d)", val_expr, val_expr, val_expr); break;
        case CAST_DOUBLE: case CAST_LONG_DOUBLE: snprintf(buf, sizeof(buf), "((%s).type == VAL_INT ? (double)(%s).v.i : (%s).v.d)", val_expr, val_expr, val_expr); break;
        case CAST_BOOL: snprintf(buf, sizeof(buf), "((%s).type == VAL_INT ? (%s).v.i != 0 : (%s).type == VAL_DOUBLE ? (%s).v.d != 0 : 0)", val_expr, val_expr, val_expr, val_expr); break;
        default: snprintf(buf, sizeof(buf), "%s", val_expr); break;
    }
    return buf;
}

static int s_gen_map_label_id = 0;  /* 生成器 map/filter goto 标签唯一 ID */

int fin_lab_cnt = 0;
int* fin_lab_pcs = NULL;
int fin_lab_cap = 0;

int fin_lab_idx_of(int pc)
{
    for(int i = 0; i < fin_lab_cnt; i++)
        if(fin_lab_pcs[i] == pc) return i;
    if(fin_lab_cnt >= fin_lab_cap) {
        int newcap = fin_lab_cap > 0 ? fin_lab_cap * 2 : 64;
        int* np = (int*)realloc(fin_lab_pcs, (size_t)newcap * sizeof(int));
        if(!np) { fprintf(stderr, "codegen: fin_lab 表扩容内存不足\n"); exit(EXIT_FAILURE); }
        fin_lab_pcs = np;
        fin_lab_cap = newcap;
    }
    fin_lab_pcs[fin_lab_cnt] = pc; return fin_lab_cnt++;
    exit(EXIT_FAILURE);
}

// 判断指令 idx 是否为跳转目标
int is_jump_target(BytecodeFunc* fn, int idx)
{
    for(int i = 0; i < fn->code_len; i++) {
        Instruction in = fn->code[i];
        if((in.op == OPC_JMP || in.op == OPC_JMP_IF_FALSE || in.op == OPC_JMP_IF_TRUE
            || in.op == OPC_JMP_IF_NULL || in.op == OPC_TRY || in.op == OPC_ENDTRY) && in.a == idx)
            return 1;
        if((in.op == OPC_TRY || in.op == OPC_FIN_PUSH || in.op == OPC_PEND_RETURN) && in.b == idx)
            return 1;
    }
    return 0;
}

void emit_insns(BytecodeFunc* fn)
{
    int i = 0;
    /* STW 检查点密度计数器：每 STW_CHECK_INTERVAL 条直线指令插入一次 gc_stw_check，
     * 确保长直线代码中 GC 能在微秒级暂停线程，消除 torn Value 竞态。
     * 已有 STW 检查的指令（BUILTIN/CALL/CALLV/后向JMP）重置计数器。 */
    #define STW_CHECK_INTERVAL 8
    int stw_counter = 0;
    while(i < fn->code_len) {
        if(is_jump_target(fn, i)) fprintf(out, "L%d:;\n", i);
        Instruction in = fn->code[i];
        const char* nm = (in.a >= 0 && in.a < fn->sym_cnt) ? fn->syms[in.a] : NULL;

        /* ===== 标量替换：创建模式 ARRAY_LIT/MAP_LIT + STORE_VAR + POP ===== */
        if((in.op == OPC_ARRAY_LIT || in.op == OPC_MAP_LIT) &&
           i + 2 < fn->code_len &&
           fn->code[i+1].op == OPC_STORE_VAR &&
           fn->code[i+2].op == OPC_POP) {
            int v = fn->code[i+1].a;
            if(g_scalar_var && g_scalar_var[v]) {
                int cnt = g_scalar_count[v];
                if(g_scalar_kind[v] == 0) {
                    /* 数组标量初始化：元素在栈上 __stk[__sp-cnt..__sp-1] */
                    fprintf(out, "    { /* scalar-repl array init v=%d cnt=%d */\n", v, cnt);
                    for(int k = 0; k < cnt; k++)
                        fprintf(out, "        __sr_v%d_e%d = __stk[__sp - %d + %d];\n", v, k, cnt, k);
                    fprintf(out, "        __sp -= %d;\n", cnt);
                    fprintf(out, "    }\n");
                } else {
                    /* map 标量初始化：键值对在栈上，值在奇数位置 */
                    fprintf(out, "    { /* scalar-repl map init v=%d cnt=%d */\n", v, cnt);
                    for(int k = 0; k < cnt; k++)
                        fprintf(out, "        __sr_v%d_e%d = __stk[__sp - %d + %d];\n", v, k, 2*cnt, 2*k + 1);
                    fprintf(out, "        __sp -= %d;\n", 2 * cnt);
                    fprintf(out, "    }\n");
                }
                i += 3;  /* 跳过 LIT + STORE_VAR + POP */
                continue;
            }
        }

        /* ===== 标量替换：使用模式 LOAD_VAR(scalar var) + ... ===== */
        if(in.op == OPC_LOAD_VAR && g_scalar_var && g_scalar_var[in.a]) {
            int v = in.a;
            int cnt = g_scalar_count[v];
            /* 模式1：LOAD_VAR + BUILTIN_LEN → 常量 len */
            if(i + 1 < fn->code_len && fn->code[i+1].op == OPC_BUILTIN &&
               fn->code[i+1].a == BUILTIN_LEN) {
                fprintf(out, "    { Value __c; __c.type = VAL_INT; __c.v.i = %d; __stk[__sp++] = __c; }\n", cnt);
                i += 2;
                continue;
            }
            /* 模式2/3：LOAD_VAR + LOAD_CONST(idx) + INDEX_GET/INDEX_SET */
            if(i + 1 < fn->code_len && fn->code[i+1].op == OPC_LOAD_CONST) {
                int ci = fn->code[i+1].a;
                int elem_idx = -1;
                if(g_scalar_kind[v] == 0) {
                    /* 数组：下标是整数常量 */
                    if(ci >= 0 && ci < fn->const_cnt && fn->consts[ci].type == VAL_INT) {
                        long long idx = fn->consts[ci].v.i;
                        if(idx >= 0 && idx < cnt) elem_idx = (int)idx;
                    }
                } else {
                    /* map：键是字符串常量，匹配 g_scalar_keys[v] */
                    if(ci >= 0 && ci < fn->const_cnt && fn->consts[ci].type == VAL_STRING && g_scalar_keys[v]) {
                        const char* key_str = lumyr_str_cstr(&fn->consts[ci]);
                        for(int k = 0; k < cnt; k++) {
                            int kci = g_scalar_keys[v][k];
                            if(kci >= 0 && kci < fn->const_cnt && fn->consts[kci].type == VAL_STRING) {
                                if(strcmp(lumyr_str_cstr(&fn->consts[kci]), key_str) == 0) {
                                    elem_idx = k;
                                    break;
                                }
                            }
                        }
                    }
                }
                if(elem_idx >= 0) {
                    /* INDEX_GET：LOAD_VAR + LOAD_CONST + INDEX_GET */
                    if(i + 2 < fn->code_len && fn->code[i+2].op == OPC_INDEX_GET) {
                        fprintf(out, "    __stk[__sp++] = __sr_v%d_e%d;\n", v, elem_idx);
                        i += 3;
                        continue;
                    }
                    /* INDEX_SET：LOAD_VAR + LOAD_CONST + <value> + INDEX_SET（value 单条指令） */
                    if(i + 3 < fn->code_len && fn->code[i+3].op == OPC_INDEX_SET) {
                        Instruction val_in = fn->code[i+2];
                        /* 生成值指令（内联支持 LOAD_CONST / LOAD_VAR / GETFUNC） */
                        if(val_in.op == OPC_LOAD_CONST) {
                            fprintf(out, "    __stk[__sp++] = ");
                            emit_const(out, &fn->consts[val_in.a]);
                            fprintf(out, ";\n");
                        } else if(val_in.op == OPC_LOAD_VAR) {
                            const char* vnm = (val_in.a >= 0 && val_in.a < fn->sym_cnt) ? fn->syms[val_in.a] : NULL;
                            fprintf(out, "    __stk[__sp++] = %s;\n", cvar_rw(vnm));
                        } else if(val_in.op == OPC_GETFUNC) {
                            const char* vnm = (val_in.a >= 0 && val_in.a < fn->sym_cnt) ? fn->syms[val_in.a] : NULL;
                            fprintf(out, "    { Value __f = {0}; __f.type = VAL_FUNC; __f.v.func.ffi_func = NULL; __f.v.func.is_ffi = 0; __f.v.func.func_obj = (void*)&lum_wrap_%s_rf; __stk[__sp++] = __f; }\n", vnm ? vnm : "unknown");
                        } else {
                            /* 不支持的值表达式类型：回退到正常处理（不应发生，分析阶段已过滤） */
                            fprintf(out, "    __stk[__sp++] = val_none(); /* scalar-repl fallback */\n");
                        }
                        /* 标量存储：弹出值，存入标量，再压回（INDEX_SET 返回值语义） */
                        fprintf(out, "    { Value __v = __stk[--__sp]; __sr_v%d_e%d = __v; __stk[__sp++] = __v; }\n", v, elem_idx);
                        i += 4;
                        continue;
                    }
                }
                /* elem_idx < 0 或模式不匹配：回退正常处理 */
            }
        }

        /* STW 检查点密度：每 STW_CHECK_INTERVAL 条直线指令插入一次 gc_stw_check。
         * 已有检查的指令（BUILTIN/CALL/CALLV/后向JMP）重置计数器，避免重复检查。
         * OPC_NOP 不计数（不生成实际代码）。 */
        {
            int has_own_check = (in.op == OPC_BUILTIN || in.op == OPC_CALL ||
                                 in.op == OPC_CALLV ||
                                 (in.op == OPC_JMP && in.a < i));
            if (has_own_check) {
                stw_counter = 0;
            } else if (in.op != OPC_NOP) {
                stw_counter++;
                if (stw_counter >= STW_CHECK_INTERVAL) {
                    fprintf(out, "    gc_stw_check_fast();\n");
                    stw_counter = 0;
                }
            }
        }

        switch(in.op) {
            case OPC_NOP:
                break;
            case OPC_LOAD_CONST:
                fprintf(out, "    __stk[__sp++] = ");
                emit_const(out, &fn->consts[in.a]);
                fprintf(out, ";\n");
                break;
            case OPC_GETFUNC: {
                if(!ir_func_table_lookup(nm)) { fprintf(stderr, "codegen: 未定义函数: %s\n", nm); exit(EXIT_FAILURE); }
                fprintf(out, "    { Value __f = {0}; __f.type = VAL_FUNC; __f.v.func.ffi_func = NULL; __f.v.func.is_ffi = 0; __f.v.func.func_obj = (void*)&lum_wrap_%s_rf; __stk[__sp++] = __f; }\n", nm);
                break;
            }
            case OPC_MKCLOSURE: {
                /* 编译通道闭包实例化：为有捕获的 lambda 创建堆 RuntimeFunc，
                 * captures 为 Value** cell 指针数组（末尾 NULL 哨兵），capture_count=-2。
                 * cell 指针来源：本函数装箱局部/参数 → lmloc_<name>；
                 *               本 lambda 自身透传的外层捕获 → __caps[idx]。 */
                BytecodeFunc* lfn = ir_func_table_lookup(nm);
                if(!lfn) {
                    fprintf(stderr, "codegen: 未定义闭包函数: %s\n", nm);
                    exit(EXIT_FAILURE);
                }
                int ncap = lambda_capture_count(nm);
                fprintf(out, "    { /* closure %s */\n", nm);
                fprintf(out, "        int __ncap = %d;\n", ncap);
                fprintf(out, "        Value** __cc = (Value**)malloc(sizeof(Value*) * (%d + 1));\n", ncap);
                for(int j = 0; j < ncap; j++) {
                    const char* capnm = lambda_capture_name(nm, j);
                    fprintf(out, "        __cc[%d] = %s;\n", j, cell_ptr_expr(capnm));
                }
                fprintf(out, "        __cc[%d] = NULL;\n", ncap);
                fprintf(out, "        RuntimeFunc* __rf = (RuntimeFunc*)malloc(sizeof(RuntimeFunc));\n");
                fprintf(out, "        __rf->entry = (FuncEntry*)lum_wrap_%s;\n", nm);
                fprintf(out, "        __rf->param_count = %d;\n", lfn->param_cnt);
                fprintf(out, "        __rf->has_variadic = %d;\n", lfn->has_variadic ? 1 : 0);
                fprintf(out, "        __rf->captures = (Value*)__cc;\n");
                fprintf(out, "        __rf->capture_count = -2;\n");
                fprintf(out, "        Value __fv = {0}; __fv.type = VAL_FUNC; __fv.v.func.ffi_func = NULL; __fv.v.func.is_ffi = 0; __fv.v.func.func_obj = __rf;\n");
                fprintf(out, "        __stk[__sp++] = __fv;\n");
                fprintf(out, "    }\n");
                break;
            }
            case OPC_LOAD_VAR: {
                const char* _sname = emit_get_var_struct_name(fn, nm);
                /* struct/class 变量：直接传递 VAL_STRUCT_PTR 类型的 Value，真正隔离，不转换成 map
                   lumyr_index_get 已支持 VAL_STRUCT_PTR，无类型标注的参数也能正确处理 */
                if(_sname) {
                    fprintf(out, "    __stk[__sp++] = %s;\n", cvar_rw(nm));
                } else {
                    int _tag = emit_get_var_tag(fn, nm);
                    if(_tag >= 0 && emit_tag_to_ctype(_tag)) {
                        fprintf(out, "    __stk[__sp++] = %s;\n", emit_precise_load(_tag, cvar_rw(nm)));
                    } else {
                        fprintf(out, "    __stk[__sp++] = %s;\n", cvar_rw(nm));
                    }
                }
                break;
            }
            case OPC_LOAD_VAR_REF: {
                /* ref 参数：直接传递 Value（struct 不转 Map，保持 VAL_STRUCT_PTR） */
                fprintf(out, "    __stk[__sp++] = %s;\n", cvar_rw(nm));
                break;
            }
            case OPC_LOAD_STRUCT_PTR: {
                /* 加载 struct 变量的指针（用于方法 self 参数）
                   class 类型：直接传递 VAL_STRUCT_PTR 类型的 Value（引用类型）
                   struct 类型：传递指针整数（值类型，旧设计） */
                const char* _sname = emit_get_var_struct_name(fn, nm);
                if(_sname && strncmp(_sname, "class:", 6) == 0) {
                    /* class 类型：直接传递 Value，类型是 VAL_STRUCT_PTR */
                    fprintf(out, "    __stk[__sp++] = %s;\n", cvar_rw(nm));
                } else if(_sname) {
                    /* struct 类型：传递指针整数 */
                    fprintf(out, "    { Value __pv = {0}; __pv.type = VAL_INT; __pv.v.i = (long long)%s.v.struct_ptr; __stk[__sp++] = __pv; }\n", cvar_rw(nm));
                } else {
                    fprintf(out, "    __stk[__sp++] = %s;\n", cvar_rw(nm));
                }
                break;
            }
            case OPC_STORE_VAR: {
                const char* _sname = emit_get_var_struct_name(fn, nm);
                /* 检查是否是 class 类型（以 class: 开头），class 是引用类型，直接赋值 Value */
                int _is_class = 0;
                const char* _pure_name = _sname;
                if(_sname && strncmp(_sname, "class:", 6) == 0) {
                    _is_class = 1;
                    _pure_name = _sname + 6;
                }
                if(_sname && !_is_class) {
                    fprintf(out, "    { Value __v = __stk[--__sp];\n");
                    char _store_buf[256];
                    snprintf(_store_buf, sizeof(_store_buf), "((lumyr_struct_%s*)%s.v.struct_ptr)", _sname, cvar_rw(nm));
                    /* 运行时类型检查：如果右边是 VAL_STRUCT_PTR，直接结构体拷贝；否则从 map 转换 */
                    fprintf(out, "        if(__v.type == VAL_STRUCT_PTR) {\n");
                    fprintf(out, "            memcpy(%s, __v.v.struct_ptr, sizeof(lumyr_struct_%s));\n", _store_buf, _sname);
                    fprintf(out, "        } else {\n");
                    emit_value_to_struct(_sname, _store_buf, "__v");
                    fprintf(out, "        }\n");
                    fprintf(out, "        __stk[__sp++] = __v;\n    }\n");
                } else {
                    int _tag = emit_get_var_tag(fn, nm);
                    if(_tag >= 0 && emit_tag_to_ctype(_tag)) {
                        fprintf(out, "    { Value __v = __stk[--__sp]; %s = %s; __stk[__sp++] = __v; }\n",
                                cvar_rw(nm), emit_precise_store(_tag, "__v"));
                    } else {
                        fprintf(out, "    { Value __v = __stk[--__sp]; %s = __v; __stk[__sp++] = __v; }\n", cvar_rw(nm));
                    }
                }
                break;
            }
            case OPC_ADD: fprintf(out, "    { Value __l = __stk[__sp-2], __r = __stk[__sp-1]; __stk[__sp-2] = lumyr_add(__l, __r); __sp--; }\n"); break;
            case OPC_SUB: fprintf(out, "    { Value __l = __stk[__sp-2], __r = __stk[__sp-1]; __stk[__sp-2] = lumyr_sub(__l, __r); __sp--; }\n"); break;
            case OPC_MUL: fprintf(out, "    { Value __l = __stk[__sp-2], __r = __stk[__sp-1]; __stk[__sp-2] = lumyr_mul(__l, __r); __sp--; }\n"); break;
            case OPC_DIV: fprintf(out, "    { Value __l = __stk[__sp-2], __r = __stk[__sp-1]; __stk[__sp-2] = lumyr_div(__l, __r); __sp--; }\n"); break;
            case OPC_MOD: fprintf(out, "    { Value __l = __stk[__sp-2], __r = __stk[__sp-1]; __stk[__sp-2] = lumyr_mod(__l, __r); __sp--; }\n"); break;
            case OPC_GT:  fprintf(out, "    { Value __l = __stk[__sp-2], __r = __stk[__sp-1]; __stk[__sp-2] = lumyr_gt(__l, __r); __sp--; }\n"); break;
            case OPC_LT:  fprintf(out, "    { Value __l = __stk[__sp-2], __r = __stk[__sp-1]; __stk[__sp-2] = lumyr_lt(__l, __r); __sp--; }\n"); break;
            case OPC_GE:  fprintf(out, "    { Value __l = __stk[__sp-2], __r = __stk[__sp-1]; __stk[__sp-2] = lumyr_ge(__l, __r); __sp--; }\n"); break;
            case OPC_LE:  fprintf(out, "    { Value __l = __stk[__sp-2], __r = __stk[__sp-1]; __stk[__sp-2] = lumyr_le(__l, __r); __sp--; }\n"); break;
            case OPC_EQ:  fprintf(out, "    { Value __l = __stk[__sp-2], __r = __stk[__sp-1]; __stk[__sp-2] = lumyr_eq(__l, __r); __sp--; }\n"); break;
            case OPC_NE:  fprintf(out, "    { Value __l = __stk[__sp-2], __r = __stk[__sp-1]; __stk[__sp-2] = lumyr_ne(__l, __r); __sp--; }\n"); break;
            case OPC_NEG: fprintf(out, "    { Value __v = __stk[__sp-1]; __stk[__sp-1] = lumyr_unary_minus(__v); }\n"); break;
            case OPC_POS: fprintf(out, "    { Value __v = __stk[__sp-1]; __stk[__sp-1] = lumyr_unary_plus(__v); }\n"); break;
            case OPC_PRE_INC:  fprintf(out, "    { Value* __vp = %s; __stk[__sp++] = lumyr_pre_inc(__vp); }\n", cvar_ptr(nm)); break;
            case OPC_POST_INC: fprintf(out, "    { Value* __vp = %s; __stk[__sp++] = lumyr_post_inc(__vp); }\n", cvar_ptr(nm)); break;
            case OPC_PRE_DEC:  fprintf(out, "    { Value* __vp = %s; __stk[__sp++] = lumyr_pre_dec(__vp); }\n", cvar_ptr(nm)); break;
            case OPC_POST_DEC: fprintf(out, "    { Value* __vp = %s; __stk[__sp++] = lumyr_post_dec(__vp); }\n", cvar_ptr(nm)); break;
            case OPC_CAST_INT:    fprintf(out, "    { Value __v = __stk[__sp-1]; __stk[__sp-1] = lumyr_cast_int(__v); }\n"); break;
            case OPC_CAST_DOUBLE: fprintf(out, "    { Value __v = __stk[__sp-1]; __stk[__sp-1] = lumyr_cast_double(__v); }\n"); break;
            case OPC_CAST_CHAR:   fprintf(out, "    { Value __v = __stk[__sp-1]; __stk[__sp-1] = lumyr_cast_char(__v); }\n"); break;
            case OPC_CAST_BOOL:   fprintf(out, "    { Value __v = __stk[__sp-1]; __stk[__sp-1] = lumyr_cast_bool(__v); }\n"); break;
            case OPC_CAST_STRING: fprintf(out, "    { Value __v = __stk[__sp-1]; __stk[__sp-1] = lumyr_cast_string(__v); }\n"); break;
            case OPC_CAST_ASCII:  fprintf(out, "    { Value __v = __stk[__sp-1]; __stk[__sp-1] = lumyr_cast_ascii(__v); }\n"); break;
            case OPC_CAST_BYTE:   fprintf(out, "    { Value __v = __stk[__sp-1]; __stk[__sp-1] = lumyr_cast_byte(__v); }\n"); break;
            case OPC_CAST_INT8:   fprintf(out, "    { Value __v = __stk[__sp-1]; __stk[__sp-1] = lumyr_cast_int8(__v); }\n"); break;
            case OPC_CAST_INT16:  fprintf(out, "    { Value __v = __stk[__sp-1]; __stk[__sp-1] = lumyr_cast_int16(__v); }\n"); break;
            case OPC_CAST_INT32:  fprintf(out, "    { Value __v = __stk[__sp-1]; __stk[__sp-1] = lumyr_cast_int32(__v); }\n"); break;
            case OPC_CAST_INT64:  fprintf(out, "    { Value __v = __stk[__sp-1]; __stk[__sp-1] = lumyr_cast_int64(__v); }\n"); break;
            case OPC_CAST_UINT8:  fprintf(out, "    { Value __v = __stk[__sp-1]; __stk[__sp-1] = lumyr_cast_uint8(__v); }\n"); break;
            case OPC_CAST_UINT16: fprintf(out, "    { Value __v = __stk[__sp-1]; __stk[__sp-1] = lumyr_cast_uint16(__v); }\n"); break;
            case OPC_CAST_UINT32: fprintf(out, "    { Value __v = __stk[__sp-1]; __stk[__sp-1] = lumyr_cast_uint32(__v); }\n"); break;
            case OPC_CAST_UINT64: fprintf(out, "    { Value __v = __stk[__sp-1]; __stk[__sp-1] = lumyr_cast_uint64(__v); }\n"); break;
            case OPC_CAST_LONG: fprintf(out, "    { Value __v = __stk[__sp-1]; __stk[__sp-1] = lumyr_cast_long(__v); }\n"); break;
            case OPC_CAST_LONGLONG: fprintf(out, "    { Value __v = __stk[__sp-1]; __stk[__sp-1] = lumyr_cast_longlong(__v); }\n"); break;
            case OPC_CAST_FLOAT: fprintf(out, "    { Value __v = __stk[__sp-1]; __stk[__sp-1] = lumyr_cast_float(__v); }\n"); break;
            case OPC_LOGIC_NOT:   fprintf(out, "    { Value __v = __stk[__sp-1]; __stk[__sp-1] = lumyr_logic_not(__v); }\n"); break;
            case OPC_ARRAY_LIT: {
                int n = in.b;
                if(g_stack_alloc && g_stack_alloc[i]) {
                    if(g_items_stack_alloc && g_items_stack_alloc[i]) {
                        /* 完全栈分配：ValueArray 结构体 + items 缓冲区均在 C 栈上，免 GC */
                        fprintf(out, "    {\n");
                        fprintf(out, "        Value __arr = val_array_from_stack_items(&__arr_stk_%d, __items_stk_%d, %d);\n", i, i, n);
                        for(int k = 0; k < n; k++) {
                            fprintf(out, "        gc_write_barrier(__stk[__sp - %d + %d]);\n", n, k);
                            fprintf(out, "        __arr.v.array->items[%d] = __stk[__sp - %d + %d];\n", k, n, k);
                        }
                        fprintf(out, "        __sp = __sp - %d + 1;\n", n);
                        fprintf(out, "        __stk[__sp - 1] = __arr;\n");
                        fprintf(out, "    }\n");
                    } else {
                        /* 半栈分配：ValueArray 结构体在 C 栈上，items 仍走 gc_alloc */
                        fprintf(out, "    {\n");
                        fprintf(out, "        Value __arr = val_array_from_stack(&__arr_stk_%d, %d);\n", i, n);
                        for(int k = 0; k < n; k++) {
                            fprintf(out, "        gc_write_barrier(__stk[__sp - %d + %d]);\n", n, k);
                            fprintf(out, "        __arr.v.array->items[%d] = __stk[__sp - %d + %d];\n", k, n, k);
                        }
                        fprintf(out, "        __sp = __sp - %d + 1;\n", n);
                        fprintf(out, "        __stk[__sp - 1] = __arr;\n");
                        fprintf(out, "    }\n");
                    }
                } else {
                    fprintf(out, "    {\n");
                    fprintf(out, "        Value __arr = val_array(%d);\n", n);
                    for(int k = 0; k < n; k++) {
                        fprintf(out, "        gc_write_barrier(__stk[__sp - %d + %d]);\n", n, k);
                        fprintf(out, "        __arr.v.array->items[%d] = __stk[__sp - %d + %d];\n", k, n, k);
                    }
                    fprintf(out, "        __sp = __sp - %d + 1;\n", n);
                    fprintf(out, "        __stk[__sp - 1] = __arr;\n");
                    fprintf(out, "    }\n");
                }
                break;
            }
            case OPC_MAP_LIT: {
                int n = in.b;
                if(g_map_stack_alloc && g_map_stack_alloc[i]) {
                    /* 栈分配：ValueMap 结构体在 C 栈上，buckets/entries 仍堆分配 */
                    fprintf(out, "    {\n");
                    fprintf(out, "        Value __m = val_map_from_stack(&__map_stk_%d);\n", i);
                    fprintf(out, "        for(int __k = 0; __k < %d; __k++) lumyr_map_set(&__m, __stk[__sp - %d + __k * 2], __stk[__sp - %d + __k * 2 + 1]);\n", n, 2 * n, 2 * n);
                    fprintf(out, "        __sp = __sp - %d + 1;\n", 2 * n);
                    fprintf(out, "        __stk[__sp - 1] = __m;\n");
                    fprintf(out, "    }\n");
                } else {
                    fprintf(out, "    {\n");
                    fprintf(out, "        Value __m = lumyr_map_lit(&__stk[__sp - %d], %d);\n", 2 * n, n);
                    fprintf(out, "        __sp = __sp - %d + 1;\n", 2 * n);
                    fprintf(out, "        __stk[__sp - 1] = __m;\n");
                    fprintf(out, "    }\n");
                }
                break;
            }
            case OPC_INDEX_GET:
                fprintf(out, "    { Value __c = __stk[__sp-2], __idx = __stk[__sp-1]; __stk[__sp-2] = lumyr_index_get(__c, __idx); __sp--; }\n");
                break;
            case OPC_INDEX_SET:
                fprintf(out, "    { Value __arr = __stk[__sp-3], __idx = __stk[__sp-2], __val = __stk[__sp-1]; __stk[__sp-3] = lumyr_array_set(__arr, __idx, __val); __sp -= 2; }\n");
                break;
            case OPC_LOAD_FIELD: {
                const char* vname = fn->syms[in.a];
                const char* fname = lumyr_str_cstr(&fn->consts[in.b]);
                const char* sname = emit_get_var_struct_name(fn, vname);
                /* 方法 self 参数：self 的值是指针地址，转换为指针再访问字段 */
                if(sname && strcmp(vname, "self") == 0) {
                    const char* ss;
                    int is_class = emit_parse_struct_type(sname, &ss);
                    const char* struct_prefix = is_class ? "lumyr_class_" : "lumyr_struct_";
                    TypeDef* td = is_class ? type_lookup(ss) : struct_lookup(ss);
                    int ck = CAST_LONGLONG;
                    if(td) {
                        for(int fi = 0; fi < td->nprops; fi++) {
                            if(strcmp(td->props[fi], fname) == 0) {
                                ck = td->field_cast_kinds ? td->field_cast_kinds[fi] : CAST_LONGLONG;
                                break;
                            }
                        }
                    }
                    /* 特殊处理 __classname__ 属性 */
                    if(strcmp(fname, "__classname__") == 0) {
                        /* 计算继承链深度 */
                        int _cn_depth2 = 0;
                        TypeDef* _cn_cur2 = td;
                        while(_cn_cur2 && _cn_cur2->parent) {
                            _cn_depth2++;
                            _cn_cur2 = type_lookup(_cn_cur2->parent);
                        }
                        fprintf(out, "    __stk[__sp++] = lumyr_make_string(((%s%s*)lmloc_self.v.struct_ptr)->", struct_prefix, ss);
                        for(int _cnd2 = 0; _cnd2 < _cn_depth2; _cnd2++) fprintf(out, "super.");
                        fprintf(out, "__classname__);\n");
                    } else {
                        /* 判断是否是父类字段 */
                        int _is_pf = 0;
                        if(td && td->parent) {
                            TypeDef* _ptd = type_lookup(td->parent);
                            if(_ptd) {
                                for(int _pfi = 0; _pfi < _ptd->nprops; _pfi++) {
                                    if(strcmp(_ptd->props[_pfi], fname) == 0) { _is_pf = 1; break; }
                                }
                            }
                        }
                        const char* _pf = _is_pf ? "super." : "";
                        if(ck == CAST_DOUBLE || ck == CAST_FLOAT || ck == CAST_LONG_DOUBLE) {
                            fprintf(out, "    __stk[__sp++] = lumyr_make_double((double)((%s%s*)lmloc_self.v.struct_ptr)->%s%s);\n", struct_prefix, ss, _pf, fname);
                        } else if(ck == CAST_STRING) {
                            fprintf(out, "    __stk[__sp++] = lumyr_make_string(((%s%s*)lmloc_self.v.struct_ptr)->%s%s);\n", struct_prefix, ss, _pf, fname);
                        } else if(ck == CAST_BOOL) {
                            fprintf(out, "    __stk[__sp++] = lumyr_make_bool((int)((%s%s*)lmloc_self.v.struct_ptr)->%s%s);\n", struct_prefix, ss, _pf, fname);
                        } else {
                            fprintf(out, "    __stk[__sp++] = lumyr_make_int((long long)((%s%s*)lmloc_self.v.struct_ptr)->%s%s);\n", struct_prefix, ss, _pf, fname);
                        }
                    }  /* end of else (not __classname__) */
                    break;
                }
                if(sname && (!(in.a < fn->param_cnt && !fn->is_method) ||
                              (in.a < fn->param_cnt && fn->param_is_ref && fn->param_is_ref[in.a]))) {
                    /* struct/class 局部变量、方法 self 参数、ref 参数 用高性能指针访问；
                       普通函数参数传递的是 Value(map)，回退到 lumyr_index_get */
                    const char* ss;
                    int is_class = emit_parse_struct_type(sname, &ss);
                    const char* struct_prefix = is_class ? "lumyr_class_" : "lumyr_struct_";
                    TypeDef* td = is_class ? type_lookup(ss) : struct_lookup(ss);
                    int ck = CAST_LONGLONG;
                    if(td) {
                        for(int fi = 0; fi < td->nprops; fi++) {
                            if(strcmp(td->props[fi], fname) == 0) {
                                ck = td->field_cast_kinds ? td->field_cast_kinds[fi] : CAST_LONGLONG;
                                break;
                            }
                        }
                    }
                    /* 特殊处理 __classname__ 属性（class 的特殊只读属性） */
                    if(is_class && strcmp(fname, "__classname__") == 0) {
                        /* 计算继承链深度，__classname__ 在最顶层父类中 */
                        int _cn_depth = 0;
                        TypeDef* _cn_cur = td;
                        while(_cn_cur && _cn_cur->parent) {
                            _cn_depth++;
                            _cn_cur = type_lookup(_cn_cur->parent);
                        }
                        /* 直接计算是否是 ref 参数（_is_ref_param 在后面才声明） */
                        int _cn_is_ref = (in.a < fn->param_cnt && fn->param_is_ref && fn->param_is_ref[in.a]);
                        const char* _var_acc = cvar_rw(vname);
                        if(_cn_is_ref) {
                            fprintf(out, "    __stk[__sp++] = lumyr_make_string(((%s%s*)(%s).v.struct_ptr)->", struct_prefix, ss, _var_acc);
                        } else {
                            fprintf(out, "    __stk[__sp++] = lumyr_make_string(((%s%s*)%s.v.struct_ptr)->", struct_prefix, ss, _var_acc);
                        }
                        for(int _cnd = 0; _cnd < _cn_depth; _cnd++) fprintf(out, "super.");
                        fprintf(out, "__classname__);\n");
                        break;
                    }
                    const char* nested_sname = NULL;
                    if(td && td->field_struct_names) {
                        for(int fi = 0; fi < td->nprops; fi++) {
                            if(strcmp(td->props[fi], fname) == 0) {
                                nested_sname = td->field_struct_names[fi];
                                break;
                            }
                        }
                    }
                    /* 区分函数参数（Value 类型，指针存在 v.i 中）和 struct 局部变量（指针类型） */
                    const char* _struct_access = NULL;
                    char _struct_access_buf[256];
                    /* ref 参数：cvar_rw 返回 *lmloc_xxx，需要加括号避免优先级问题 */
                    int _is_ref_param = (in.a < fn->param_cnt && fn->param_is_ref && fn->param_is_ref[in.a]);
                    const char* _var_access = cvar_rw(vname);
                    if(_is_ref_param) {
                        snprintf(_struct_access_buf, sizeof(_struct_access_buf), "((%s%s*)(%s).v.struct_ptr)", struct_prefix, ss, _var_access);
                    } else {
                        snprintf(_struct_access_buf, sizeof(_struct_access_buf), "((%s%s*)%s.v.struct_ptr)", struct_prefix, ss, _var_access);
                    }
                    _struct_access = _struct_access_buf;
                    if(nested_sname) {
                        /* 嵌套 struct 字段：先把地址赋值给临时指针，再转换为 Value(Map) */
                        static char tmp_ptr[256];
                        snprintf(tmp_ptr, sizeof(tmp_ptr), "__nested_load_%s_%d", fname, g_nested_ptr_counter++);
                        fprintf(out, "    lumyr_struct_%s* %s = &%s->%s;\n", nested_sname, tmp_ptr, _struct_access, fname);
                        emit_struct_to_value(nested_sname, tmp_ptr);
                    } else {
                        /* 判断是否是父类字段 */
                        int _is_pf = 0;
                        if(td && td->parent) {
                            TypeDef* _ptd = type_lookup(td->parent);
                            if(_ptd) {
                                for(int _pfi = 0; _pfi < _ptd->nprops; _pfi++) {
                                    if(strcmp(_ptd->props[_pfi], fname) == 0) { _is_pf = 1; break; }
                                }
                            }
                        }
                        const char* _pf = _is_pf ? "super." : "";
                        if(ck == CAST_DOUBLE || ck == CAST_FLOAT || ck == CAST_LONG_DOUBLE) {
                            fprintf(out, "    __stk[__sp++] = lumyr_make_double((double)%s->%s%s);\n", _struct_access, _pf, fname);
                        } else if(ck == CAST_STRING) {
                            fprintf(out, "    __stk[__sp++] = lumyr_make_string(%s->%s%s);\n", _struct_access, _pf, fname);
                        } else if(ck == CAST_BOOL) {
                            fprintf(out, "    __stk[__sp++] = lumyr_make_bool((int)%s->%s%s);\n", _struct_access, _pf, fname);
                        } else {
                            fprintf(out, "    __stk[__sp++] = lumyr_make_int((long long)%s->%s%s);\n", _struct_access, _pf, fname);
                        }
                    }
                } else if(sname) {
                    /* 有类型标记的普通函数参数：直接调用专门的 struct/class 属性访问函数，避免运行时类型判断 */
                    const char* _ss;
                    int _is_class = emit_parse_struct_type(sname, &_ss);
                    if(_is_class) {
                        fprintf(out, "    { Value __c = %s; __stk[__sp++] = lumyr_class_get_field(__c, \"%s\"); }\n", cvar_rw(vname), fname);
                    } else {
                        fprintf(out, "    { Value __c = %s; __stk[__sp++] = lumyr_struct_get_field(__c, \"%s\"); }\n", cvar_rw(vname), fname);
                    }
                } else {
                    fprintf(out, "    { Value __c = %s; __stk[__sp++] = lumyr_index_get(__c, lumyr_make_string(\"%s\")); }\n", cvar_rw(vname), fname);
                }
                break;
            }
            case OPC_STORE_FIELD: {
                const char* vname = fn->syms[in.a];
                const char* fname = lumyr_str_cstr(&fn->consts[in.b]);
                const char* sname = emit_get_var_struct_name(fn, vname);
                /* 方法 self 参数：self 的值是指针地址，转换为指针再写入字段 */
                if(sname && strcmp(vname, "self") == 0) {
                    const char* ss;
                    int is_class = emit_parse_struct_type(sname, &ss);
                    const char* struct_prefix = is_class ? "lumyr_class_" : "lumyr_struct_";
                    TypeDef* td = is_class ? type_lookup(ss) : struct_lookup(ss);
                    int ck = CAST_LONGLONG;
                    if(td) {
                        for(int fi = 0; fi < td->nprops; fi++) {
                            if(strcmp(td->props[fi], fname) == 0) {
                                ck = td->field_cast_kinds ? td->field_cast_kinds[fi] : CAST_LONGLONG;
                                break;
                            }
                        }
                    }
                    const char* ctype = emit_tag_to_ctype(ck);
                    if(!ctype) ctype = "int64_t";
                    fprintf(out, "    { Value __v = __stk[--__sp]; Value __self = lmloc_self;\n");
                    /* 特殊处理 __classname__ 属性 */
                    if(strcmp(fname, "__classname__") == 0) {
                        /* 计算继承链深度 */
                        int _cn_depth2 = 0;
                        TypeDef* _cn_cur2 = td;
                        while(_cn_cur2 && _cn_cur2->parent) {
                            _cn_depth2++;
                            _cn_cur2 = type_lookup(_cn_cur2->parent);
                        }
                        fprintf(out, "    __stk[__sp++] = lumyr_make_string(((%s%s*)lmloc_self.v.struct_ptr)->", struct_prefix, ss);
                        for(int _cnd2 = 0; _cnd2 < _cn_depth2; _cnd2++) fprintf(out, "super.");
                        fprintf(out, "__classname__);\n");
                    } else {
                        /* 判断是否是父类字段 */
                        int _is_pf = 0;
                        if(td && td->parent) {
                            TypeDef* _ptd = type_lookup(td->parent);
                            if(_ptd) {
                                for(int _pfi = 0; _pfi < _ptd->nprops; _pfi++) {
                                    if(strcmp(_ptd->props[_pfi], fname) == 0) { _is_pf = 1; break; }
                                }
                            }
                        }
                        const char* _pf = _is_pf ? "super." : "";
                        if(ck == CAST_DOUBLE || ck == CAST_FLOAT || ck == CAST_LONG_DOUBLE) {
                            fprintf(out, "        ((%s%s*)__self.v.struct_ptr)->%s%s = (%s)__v.v.d;\n", struct_prefix, ss, _pf, fname, ctype);
                        } else if(ck == CAST_STRING) {
                            fprintf(out, "        ((%s%s*)__self.v.struct_ptr)->%s%s = strdup(lumyr_str_cstr(&__v));\n", struct_prefix, ss, _pf, fname);
                        } else if(ck == CAST_BOOL) {
                            fprintf(out, "        ((%s%s*)__self.v.struct_ptr)->%s%s = (%s)__v.v.b;\n", struct_prefix, ss, _pf, fname, ctype);
                        } else {
                            fprintf(out, "        ((%s%s*)__self.v.struct_ptr)->%s%s = (%s)__v.v.i;\n", struct_prefix, ss, _pf, fname, ctype);
                        }
                    }
                    fprintf(out, "        __stk[__sp++] = __v;\n    }\n");
                    break;
                }
                if(sname && (!(in.a < fn->param_cnt && !fn->is_method) ||
                              (in.a < fn->param_cnt && fn->param_is_ref && fn->param_is_ref[in.a]))) {
                    /* struct/class 局部变量、方法 self 参数、ref 参数 用高性能指针访问；
                       普通函数参数传递的是 Value(map)，回退到 lumyr_array_set */
                    const char* ss;
                    int is_class = emit_parse_struct_type(sname, &ss);
                    const char* struct_prefix = is_class ? "lumyr_class_" : "lumyr_struct_";
                    TypeDef* td = is_class ? type_lookup(ss) : struct_lookup(ss);
                    int ck = CAST_LONGLONG;
                    if(td) {
                        for(int fi = 0; fi < td->nprops; fi++) {
                            if(strcmp(td->props[fi], fname) == 0) {
                                ck = td->field_cast_kinds ? td->field_cast_kinds[fi] : CAST_LONGLONG;
                                break;
                            }
                        }
                    }
                    const char* ctype = emit_tag_to_ctype(ck);
                    if(!ctype) ctype = "int64_t";
                    /* 区分函数参数（Value 类型，指针存在 v.i 中）和 struct 局部变量（指针类型） */
                    const char* _store_access = NULL;
                    char _store_access_buf[256];
                    /* ref 参数：cvar_rw 返回 *lmloc_xxx，需要加括号避免优先级问题 */
                    int _is_ref_param = (in.a < fn->param_cnt && fn->param_is_ref && fn->param_is_ref[in.a]);
                    const char* _var_access = cvar_rw(vname);
                    if(_is_ref_param) {
                        snprintf(_store_access_buf, sizeof(_store_access_buf), "((%s%s*)(%s).v.struct_ptr)", struct_prefix, ss, _var_access);
                    } else {
                        snprintf(_store_access_buf, sizeof(_store_access_buf), "((%s%s*)%s.v.struct_ptr)", struct_prefix, ss, _var_access);
                    }
                    _store_access = _store_access_buf;
                    fprintf(out, "    { Value __v = __stk[--__sp];\n");
                    /* 特殊处理 __classname__ 属性 */
                    if(strcmp(fname, "__classname__") == 0) {
                        /* 计算继承链深度 */
                        int _cn_depth2 = 0;
                        TypeDef* _cn_cur2 = td;
                        while(_cn_cur2 && _cn_cur2->parent) {
                            _cn_depth2++;
                            _cn_cur2 = type_lookup(_cn_cur2->parent);
                        }
                        fprintf(out, "    __stk[__sp++] = lumyr_make_string(((%s%s*)lmloc_self.v.struct_ptr)->", struct_prefix, ss);
                        for(int _cnd2 = 0; _cnd2 < _cn_depth2; _cnd2++) fprintf(out, "super.");
                        fprintf(out, "__classname__);\n");
                    } else {
                        /* 判断是否是父类字段 */
                        int _is_pf = 0;
                        if(td && td->parent) {
                            TypeDef* _ptd = type_lookup(td->parent);
                            if(_ptd) {
                                for(int _pfi = 0; _pfi < _ptd->nprops; _pfi++) {
                                    if(strcmp(_ptd->props[_pfi], fname) == 0) { _is_pf = 1; break; }
                                }
                            }
                        }
                        const char* _pf = _is_pf ? "super." : "";
                        if(ck == CAST_DOUBLE || ck == CAST_FLOAT || ck == CAST_LONG_DOUBLE) {
                            fprintf(out, "        %s->%s%s = (%s)__v.v.d;\n", _store_access, _pf, fname, ctype);
                        } else if(ck == CAST_STRING) {
                            fprintf(out, "        %s->%s%s = strdup(lumyr_str_cstr(&__v));\n", _store_access, _pf, fname);
                        } else if(ck == CAST_BOOL) {
                            fprintf(out, "        %s->%s%s = (%s)__v.v.b;\n", _store_access, _pf, fname, ctype);
                        } else {
                            fprintf(out, "        %s->%s%s = (%s)__v.v.i;\n", _store_access, _pf, fname, ctype);
                        }
                    }
                    fprintf(out, "        __stk[__sp++] = __v;\n    }\n");
                } else if(sname) {
                    /* 有类型标记的普通函数参数：直接调用专门的 struct/class 属性写入函数，避免运行时类型判断 */
                    const char* _ss;
                    int _is_class = emit_parse_struct_type(sname, &_ss);
                    if(_is_class) {
                        fprintf(out, "    { Value __v = __stk[--__sp]; Value __c = %s; lumyr_class_set_field(__c, \"%s\", __v); __stk[__sp++] = __v; }\n", cvar_rw(vname), fname);
                    } else {
                        fprintf(out, "    { Value __v = __stk[--__sp]; Value __c = %s; lumyr_struct_set_field(__c, \"%s\", __v); __stk[__sp++] = __v; }\n", cvar_rw(vname), fname);
                    }
                } else {
                    fprintf(out, "    { Value __v = __stk[--__sp]; Value __c = %s; lumyr_array_set(__c, lumyr_make_string(\"%s\"), __v); __stk[__sp++] = __v; }\n", cvar_rw(vname), fname);
                }
                break;
            }
            case OPC_STORE_NESTED_FIELD: {
                const char* vname = fn->syms[in.a];
                const char* combined = lumyr_str_cstr(&fn->consts[in.b]);
                const char* sname = emit_get_var_struct_name(fn, vname);
                /* 解析组合字段名 "top_left.x" */
                char nested_fname[128], field_name[128];
                const char* dot = strchr(combined, '.');
                if(dot && sname) {
                    size_t nlen = (size_t)(dot - combined);
                    if(nlen >= sizeof(nested_fname)) nlen = sizeof(nested_fname) - 1;
                    memcpy(nested_fname, combined, nlen);
                    nested_fname[nlen] = '\0';
                    strncpy(field_name, dot + 1, sizeof(field_name) - 1);
                    field_name[sizeof(field_name) - 1] = '\0';
                    /* 查找嵌套 struct 的类型和字段类型 */
                    TypeDef* td = struct_lookup(sname);
                    const char* nested_sname = NULL;
                    if(td && td->field_struct_names) {
                        for(int fi = 0; fi < td->nprops; fi++) {
                            if(strcmp(td->props[fi], nested_fname) == 0) {
                                nested_sname = td->field_struct_names[fi];
                                break;
                            }
                        }
                    }
                    int ck = CAST_LONGLONG;
                    if(nested_sname) {
                        TypeDef* nested_td = struct_lookup(nested_sname);
                        if(nested_td && nested_td->field_cast_kinds) {
                            for(int fi = 0; fi < nested_td->nprops; fi++) {
                                if(strcmp(nested_td->props[fi], field_name) == 0) {
                                    ck = nested_td->field_cast_kinds[fi];
                                    break;
                                }
                            }
                        }
                    }
                    const char* ctype = emit_tag_to_ctype(ck);
                    if(!ctype) ctype = "int64_t";
                    /* struct 变量通过 v.struct_ptr 访问，支持全局变量和局部变量 */
                    char _nested_access[256];
                    snprintf(_nested_access, sizeof(_nested_access), "((lumyr_struct_%s*)%s.v.struct_ptr)", sname, cvar_rw(vname));
                    fprintf(out, "    { Value __v = __stk[--__sp];\n");
                    if(ck == CAST_DOUBLE || ck == CAST_FLOAT || ck == CAST_LONG_DOUBLE) {
                        fprintf(out, "        %s->%s.%s = (%s)__v.v.d;\n", _nested_access, nested_fname, field_name, ctype);
                    } else if(ck == CAST_STRING) {
                        fprintf(out, "        %s->%s.%s = (%s)lumyr_str_cstr(&__v);\n", _nested_access, nested_fname, field_name, ctype);
                    } else if(ck == CAST_BOOL) {
                        fprintf(out, "        %s->%s.%s = (%s)__v.v.b;\n", _nested_access, nested_fname, field_name, ctype);
                    } else {
                        fprintf(out, "        %s->%s.%s = (%s)__v.v.i;\n", _nested_access, nested_fname, field_name, ctype);
                    }
                    fprintf(out, "        __stk[__sp++] = __v;\n    }\n");
                } else {
                    /* 降级为普通索引赋值 */
                    fprintf(out, "    { Value __v = __stk[--__sp]; Value __c = %s; lumyr_array_set(__c, lumyr_make_string(\"%s\"), __v); __stk[__sp++] = __v; }\n", cvar_rw(vname), combined);
                }
                break;
            }
            case OPC_BUILTIN:
                fprintf(out, "    gc_stw_check_fast();\n");
                switch(in.a) {
                    /* 生成器相关内置函数 */
                    case BUILTIN_NEXT: {
                        fprintf(out, "    { Value __g = __stk[--__sp]; if(__g.type == VAL_GENERATOR) { struct { Value (*next)(void*, Value); } *__gen = (void*)__g.v.generator; __stk[__sp++] = __gen->next(__g.v.generator, val_none()); } else { __stk[__sp++] = val_none(); } }\n");
                        break;
                    }
                    case BUILTIN_SEND: {
                        fprintf(out, "    { Value __v = __stk[--__sp]; Value __g = __stk[--__sp]; if(__g.type == VAL_GENERATOR) { struct { Value (*next)(void*, Value); } *__gen = (void*)__g.v.generator; __stk[__sp++] = __gen->next(__g.v.generator, __v); } else { __stk[__sp++] = val_none(); } }\n");
                        break;
                    }
                    case BUILTIN_CLOSE: {
                        fprintf(out, "    { Value __g = __stk[--__sp]; if(__g.type == VAL_GENERATOR) { struct { void* next; int __state; } *__gen = (void*)__g.v.generator; __gen->__state = -1; } __stk[__sp++] = val_none(); }\n");
                        break;
                    }
                    case BUILTIN_GEN_THROW: {
                        /* GenThrow(gen, err)：向生成器抛出异常，在 yield 位置抛出 */
                        fprintf(out, "    { Value __err = __stk[--__sp]; Value __g = __stk[--__sp];\n");
                        fprintf(out, "      if(__g.type != VAL_GENERATOR) { fprintf(stderr, \"Runtime Error: GenThrow() 需要生成器对象\\n\"); exit(EXIT_FAILURE); }\n");
                        fprintf(out, "      struct { Value (*next)(void*, Value); int __state; Value __send_val; Value __pending_exc; int __has_pending_exc; } *__gen = (void*)__g.v.generator;\n");
                        fprintf(out, "      if(__gen->__state == -1) { fprintf(stderr, \"Runtime Error: GenThrow() 生成器已结束\\n\"); exit(EXIT_FAILURE); }\n");
                        fprintf(out, "      __gen->__pending_exc = __err; __gen->__has_pending_exc = 1;\n");
                        fprintf(out, "      __stk[__sp++] = __gen->next(__g.v.generator, val_none()); }\n");
                        break;
                    }
                    case BUILTIN_RECEIVE: {
                        fprintf(out, "    { __stk[__sp++] = __g_gen_in_generator ? __g_gen_send_val : val_none(); }\n");
                        break;
                    }
                    case BUILTIN_LEN:
                        fprintf(out, "    { Value __v = __stk[__sp-1]; __stk[__sp-1] = lumyr_len(__v); }\n");
                        break;
                    case BUILTIN_TYPE:
                        fprintf(out, "    { Value __v = __stk[__sp-1]; __stk[__sp-1] = lumyr_type(__v); }\n");
                        break;
                    case BUILTIN_INPUT:
                        fprintf(out, "    { __stk[__sp++] = lumyr_input(); }\n");
                        break;
                                        case BUILTIN_RANGE:
                        fprintf(out, "    { Value __r = lumyr_range_n(&__stk[__sp - %d], %d); __stk[__sp - %d] = __r; __sp = __sp - %d + 1; }\n",
                                in.b, in.b, in.b, in.b);
                        break;
                    case BUILTIN_SUBSTR:
                        fprintf(out, "    { Value __s = __stk[__sp-3], __st = __stk[__sp-2], __n = __stk[__sp-1]; __stk[__sp-3] = lumyr_substr(__s, __st, __n); __sp -= 2; }\n");
                        break;
                    case BUILTIN_TOUPPER:
                        fprintf(out, "    { Value __v = __stk[__sp-1]; __stk[__sp-1] = lumyr_toupper(__v); }\n");
                        break;
                    case BUILTIN_TOLOWER:
                        fprintf(out, "    { Value __v = __stk[__sp-1]; __stk[__sp-1] = lumyr_tolower(__v); }\n");
                        break;
                    case BUILTIN_SPLIT:
                        fprintf(out, "    { Value __s = __stk[__sp-2], __sep = __stk[__sp-1]; __stk[__sp-2] = lumyr_split(__s, __sep); __sp--; }\n");
                        break;
                    case BUILTIN_DEL:
                        fprintf(out, "    { Value __idx = __stk[--__sp]; lumyr_del(&__stk[__sp-1], __idx); }\n");
                        break;
                    case BUILTIN_INSERT:
                        fprintf(out, "    { Value __val = __stk[--__sp], __idx = __stk[--__sp]; lumyr_insert(&__stk[__sp-1], __idx, __val); }\n");
                        break;
                    case BUILTIN_FLOOR:
                        fprintf(out, "    { Value __v = __stk[__sp-1]; __stk[__sp-1] = lumyr_floor(__v); }\n");
                        break;
                    case BUILTIN_CEIL:
                        fprintf(out, "    { Value __v = __stk[__sp-1]; __stk[__sp-1] = lumyr_ceil(__v); }\n");
                        break;
                    case BUILTIN_ABS:
                        fprintf(out, "    { Value __v = __stk[__sp-1]; __stk[__sp-1] = lumyr_abs(__v); }\n");
                        break;
                    case BUILTIN_SQRT:
                        fprintf(out, "    { Value __v = __stk[__sp-1]; __stk[__sp-1] = lumyr_sqrt(__v); }\n");
                        break;
                    case BUILTIN_MAX:
                        fprintf(out, "    { Value __r = lumyr_max(&__stk[__sp - %d], %d); __stk[__sp - %d] = __r; __sp = __sp - %d + 1; }\n",
                                in.b, in.b, in.b, in.b);
                        break;
                    case BUILTIN_MIN:
                        fprintf(out, "    { Value __r = lumyr_min(&__stk[__sp - %d], %d); __stk[__sp - %d] = __r; __sp = __sp - %d + 1; }\n",
                                in.b, in.b, in.b, in.b);
                        break;
                    case BUILTIN_JOIN:
                        fprintf(out, "    { Value __arr = __stk[__sp-2], __sep = __stk[__sp-1]; __stk[__sp-2] = lumyr_join(__arr, __sep); __sp--; }\n");
                        break;
                    case BUILTIN_CONTAINS:
                        fprintf(out, "    { Value __hay = __stk[__sp-2], __needle = __stk[__sp-1]; __stk[__sp-2] = lumyr_contains(__hay, __needle); __sp--; }\n");
                        break;
                    case BUILTIN_REPEAT:
                        fprintf(out, "    { Value __s = __stk[__sp-2], __n = __stk[__sp-1]; __stk[__sp-2] = lumyr_repeat(__s, __n); __sp--; }\n");
                        break;
                    case BUILTIN_REPLACE:
                        fprintf(out, "    { Value __s = __stk[__sp-3], __from = __stk[__sp-2], __to = __stk[__sp-1]; __stk[__sp-3] = lumyr_replace(__s, __from, __to); __sp -= 2; }\n");
                        break;
                    case BUILTIN_SUM:
                        fprintf(out, "    { Value __v = __stk[__sp-1]; __stk[__sp-1] = lumyr_sum(__v); }\n");
                        break;
                    case BUILTIN_FORMAT:
                        fprintf(out, "    { Value __r = lumyr_format(&__stk[__sp - %d], %d); __stk[__sp - %d] = __r; __sp = __sp - %d + 1; }\n",
                                in.b, in.b, in.b, in.b);
                        break;
                    case BUILTIN_SORT:
                        fprintf(out, "    { Value __v = __stk[__sp-1]; __stk[__sp-1] = lumyr_sort(__v); }\n");
                        break;
                    case BUILTIN_REVERSE:
                        fprintf(out, "    { Value __v = __stk[__sp-1]; __stk[__sp-1] = lumyr_reverse(__v); }\n");
                        break;
                    case BUILTIN_STRIP:
                        fprintf(out, "    { Value __v = __stk[__sp-1]; __stk[__sp-1] = lumyr_strip(__v); }\n");
                        break;
                    case BUILTIN_STARTSWITH:
                        fprintf(out, "    { Value __l = __stk[__sp-2], __r = __stk[__sp-1]; __stk[__sp-2] = lumyr_startswith(__l, __r); __sp--; }\n");
                        break;
                    case BUILTIN_ENDSWITH:
                        fprintf(out, "    { Value __l = __stk[__sp-2], __r = __stk[__sp-1]; __stk[__sp-2] = lumyr_endswith(__l, __r); __sp--; }\n");
                        break;
                    case BUILTIN_READ_FILE:
                        fprintf(out, "    { Value __r = lumyr_read_file(&__stk[__sp - %d], %d); __stk[__sp - %d] = __r; __sp = __sp - %d + 1; }\n", in.b, in.b, in.b, in.b);
                        break;
                    case BUILTIN_WRITE_FILE:
                        fprintf(out, "    { Value __r = lumyr_write_file(&__stk[__sp - %d], %d); __stk[__sp - %d] = __r; __sp = __sp - %d + 1; }\n", in.b, in.b, in.b, in.b);
                        break;
                    case BUILTIN_FILE_EXISTS:
                        fprintf(out, "    { Value __r = lumyr_file_exists(&__stk[__sp - %d], %d); __stk[__sp - %d] = __r; __sp = __sp - %d + 1; }\n", in.b, in.b, in.b, in.b);
                        break;
                    case BUILTIN_KEYS:
                        fprintf(out, "    { Value __r = lumyr_map_keys(__stk[__sp - %d]); __stk[__sp - %d] = __r; __sp = __sp - %d + 1; }\n", in.b, in.b, in.b);
                        break;
                    case BUILTIN_THREAD: {
                        int argc = in.b;
                        fprintf(out, "    {\n");
                        fprintf(out, "        int __lmin_argc = %d;\n", argc);
                        fprintf(out, "        Value __fn = __stk[__sp - __lmin_argc];\n");
                        fprintf(out, "        if(__fn.type != VAL_FUNC) runtime_error(\"thread() 第一个参数必须是函数\");\n");
                        fprintf(out, "        Value (*__cf)(Value*, int) = (Value(*)(Value*, int))((RuntimeFunc*)__fn.v.func.func_obj)->entry;\n");
                        if(argc > 1)
                            fprintf(out, "        int __tid = lumyr_thread_start_c(__cf, &__stk[__sp - __lmin_argc + 1], %d);\n", argc - 1);
                        else
                            fprintf(out, "        int __tid = lumyr_thread_start_c(__cf, NULL, 0);\n");
                        fprintf(out, "        __stk[__sp - __lmin_argc] = lumyr_make_int(__tid);\n");
                        fprintf(out, "        __sp = __sp - __lmin_argc + 1;\n");
                        fprintf(out, "    }\n");
                        break;
                    }
                    case BUILTIN_THREAD_JOIN: {
                        fprintf(out, "    { Value __idv = __stk[--__sp]; if(__idv.type != VAL_INT) runtime_error(\"thread_join() 参数必须是线程id（整数）\"); __stk[__sp++] = lumyr_thread_join((int)__idv.v.i); }\n");
                        break;
                    }
                    case BUILTIN_MUTEX:    fprintf(out, "    __stk[__sp++] = lumyr_make_int(lumyr_mutex_create());\n"); break;
                    case BUILTIN_RMUTEX:   fprintf(out, "    __stk[__sp++] = lumyr_make_int(lumyr_rmutex_create());\n"); break;
                    case BUILTIN_RWLOCK:   fprintf(out, "    __stk[__sp++] = lumyr_make_int(lumyr_rwlock_create());\n"); break;
                    case BUILTIN_SPINLOCK: fprintf(out, "    __stk[__sp++] = lumyr_make_int(lumyr_spinlock_create());\n"); break;
                    case BUILTIN_LOCK:
                        fprintf(out, "    { Value __v = __stk[--__sp]; if(__v.type != VAL_INT) runtime_error(\"lock() 参数必须是锁id（整数）\"); lumyr_lock((int)__v.v.i); __stk[__sp++] = __v; }\n");
                        break;
                    case BUILTIN_UNLOCK:
                        fprintf(out, "    { Value __v = __stk[--__sp]; if(__v.type != VAL_INT) runtime_error(\"unlock() 参数必须是锁id（整数）\"); lumyr_unlock((int)__v.v.i); __stk[__sp++] = __v; }\n");
                        break;
                    case BUILTIN_TRYLOCK:
                        fprintf(out, "    { Value __v = __stk[--__sp]; if(__v.type != VAL_INT) runtime_error(\"trylock() 参数必须是锁id（整数）\"); __stk[__sp++] = lumyr_make_bool(lumyr_trylock((int)__v.v.i)); }\n");
                        break;
                    case BUILTIN_RDLOCK:
                        fprintf(out, "    { Value __v = __stk[--__sp]; if(__v.type != VAL_INT) runtime_error(\"rdlock() 参数必须是锁id（整数）\"); lumyr_rdlock((int)__v.v.i); __stk[__sp++] = __v; }\n");
                        break;
                    case BUILTIN_WRLOCK:
                        fprintf(out, "    { Value __v = __stk[--__sp]; if(__v.type != VAL_INT) runtime_error(\"wrlock() 参数必须是锁id（整数）\"); lumyr_wrlock((int)__v.v.i); __stk[__sp++] = __v; }\n");
                        break;
                    case BUILTIN_TRYRDLOCK:
                        fprintf(out, "    { Value __v = __stk[--__sp]; if(__v.type != VAL_INT) runtime_error(\"tryrdlock() 参数必须是锁id（整数）\"); __stk[__sp++] = lumyr_make_bool(lumyr_tryrdlock((int)__v.v.i)); }\n");
                        break;
                    case BUILTIN_TRYWRLOCK:
                        fprintf(out, "    { Value __v = __stk[--__sp]; if(__v.type != VAL_INT) runtime_error(\"trywrlock() 参数必须是锁id（整数）\"); __stk[__sp++] = lumyr_make_bool(lumyr_trywrlock((int)__v.v.i)); }\n");
                        break;
                    case BUILTIN_CONDVAR:
                        fprintf(out, "    __stk[__sp++] = lumyr_make_int(lumyr_condvar_create());\n");
                        break;
                    case BUILTIN_COND_WAIT:
                        fprintf(out, "    { Value __lk = __stk[--__sp]; Value __cd = __stk[--__sp]; if(__lk.type != VAL_INT) runtime_error(\"cond_wait() 锁参数必须是锁id（整数）\"); if(__cd.type != VAL_INT) runtime_error(\"cond_wait() 条件参数必须是条件id（整数）\"); lumyr_cond_wait((int)__cd.v.i, (int)__lk.v.i); __stk[__sp++] = __lk; }\n");
                        break;
                    case BUILTIN_COND_SIGNAL:
                        fprintf(out, "    { Value __v = __stk[--__sp]; if(__v.type != VAL_INT) runtime_error(\"cond_signal() 参数必须是条件id（整数）\"); lumyr_cond_signal((int)__v.v.i); __stk[__sp++] = __v; }\n");
                        break;
                    case BUILTIN_COND_BROADCAST:
                        fprintf(out, "    { Value __v = __stk[--__sp]; if(__v.type != VAL_INT) runtime_error(\"cond_broadcast() 参数必须是条件id（整数）\"); lumyr_cond_broadcast((int)__v.v.i); __stk[__sp++] = __v; }\n");
                        break;
                    case BUILTIN_COND_TIMEDWAIT:
                        fprintf(out, "    { Value __ms = __stk[--__sp]; Value __lk = __stk[--__sp]; Value __cd = __stk[--__sp]; if(__ms.type != VAL_INT) runtime_error(\"cond_wait_timeout() 超时参数必须是整数毫秒\"); if(__lk.type != VAL_INT) runtime_error(\"cond_wait_timeout() 锁参数必须是锁id（整数）\"); if(__cd.type != VAL_INT) runtime_error(\"cond_wait_timeout() 条件参数必须是条件id（整数）\"); __stk[__sp++] = lumyr_make_bool(lumyr_cond_timedwait((int)__cd.v.i, (int)__lk.v.i, __ms.v.i)); }\n");
                        break;
                    case BUILTIN_THREADLOCAL_GET:
                        fprintf(out, "    { Value __n = __stk[--__sp]; if(__n.type != VAL_STRING) runtime_error(\"threadlocal_get() 名字参数必须是字符串\"); __stk[__sp++] = lumyr_tls_get(lumyr_str_cstr(&__n)); }\n");
                        break;
                    case BUILTIN_THREADLOCAL_SET:
                        fprintf(out, "    { Value __v = __stk[--__sp]; Value __n = __stk[--__sp]; if(__n.type != VAL_STRING) runtime_error(\"threadlocal_set() 名字参数必须是字符串\"); lumyr_tls_set(lumyr_str_cstr(&__n), __v); __stk[__sp++] = __v; }\n");
                        break;
                    case BUILTIN_HTTP_GET:
                    case BUILTIN_HTTP_POST:
                    case BUILTIN_HTTP_PUT:
                    case BUILTIN_ARRAY_ADD:
                        if(in.b == 3)
                            fprintf(out, "    { Value __v = __stk[--__sp]; Value __k = __stk[--__sp]; Value __m = __stk[--__sp]; __stk[__sp++] = lumyr_map_add(__m, __k, __v); }\n");
                        else
                            fprintf(out, "    { Value __v = __stk[--__sp]; lumyr_array_add(&__stk[__sp-1], __v); }\n");
                        break;
                    case BUILTIN_ARRAY_REMOVE:
                        fprintf(out, "    { Value __idx = __stk[--__sp]; lumyr_del(&__stk[__sp-1], __idx); }\n");
                        break;
                    case BUILTIN_ARRAY_INDEXOF:
                        fprintf(out, "    { Value __x = __stk[--__sp]; Value __arr = __stk[--__sp]; __stk[__sp++] = lumyr_index_of(__arr, __x); }\n");
                        break;
                    case BUILTIN_ARRAY_GET:
                        fprintf(out, "    { Value __i = __stk[--__sp]; Value __arr = __stk[--__sp]; __stk[__sp++] = lumyr_array_get_safe(__arr, __i); }\n");
                        break;
                    case BUILTIN_ARRAY_SET:
                        fprintf(out, "    { Value __v = __stk[--__sp]; Value __i = __stk[--__sp]; Value __arr = __stk[--__sp]; __stk[__sp++] = lumyr_array_set_method(__arr, __i, __v); }\n");
                        break;
                    case BUILTIN_ARRAY_FIRST:
                        fprintf(out, "    { Value __arr = __stk[__sp-1]; __stk[__sp-1] = lumyr_array_first(__arr); }\n");
                        break;
                    case BUILTIN_ARRAY_LAST:
                        fprintf(out, "    { Value __arr = __stk[__sp-1]; __stk[__sp-1] = lumyr_array_last(__arr); }\n");
                        break;
                    case BUILTIN_ARRAY_CLEAR:
                        fprintf(out, "    { lumyr_array_clear(&__stk[__sp-1]); }\n");
                        break;
                    case BUILTIN_MAP_HAS:
                        fprintf(out, "    { Value __k = __stk[--__sp]; Value __m = __stk[--__sp]; __stk[__sp++] = lumyr_make_bool(lumyr_map_has(__m, __k)); }\n");
                        break;
                    case BUILTIN_JSON:
                        if(in.b >= 2)
                            fprintf(out, "    { Value __e = __stk[--__sp]; Value __v = __stk[--__sp]; __stk[__sp++] = lumyr_json_parse_enc(lumyr_str_cstr(&__v), __e); }\n");
                        else
                            fprintf(out, "    { Value __v = __stk[--__sp]; __stk[__sp++] = lumyr_json_parse_enc(lumyr_str_cstr(&__v), val_none()); }\n");
                        break;
                    case BUILTIN_STRINGIFY:
                        if(in.b >= 2)
                            fprintf(out, "    { Value __e = __stk[--__sp]; Value __v = __stk[--__sp]; char* __js = lumyr_json_stringify_enc(__v, __e); Value __r = lumyr_make_string(__js); free(__js); __stk[__sp++] = __r; }\n");
                        else
                            fprintf(out, "    { Value __v = __stk[--__sp]; char* __js = lumyr_json_stringify_enc(__v, val_none()); Value __r = lumyr_make_string(__js); free(__js); __stk[__sp++] = __r; }\n");
                        break;
                    case BUILTIN_ARRAY_FLAT:
                        if(in.b >= 2)
                            fprintf(out, "    { Value __d = __stk[--__sp]; Value __v = __stk[--__sp]; __stk[__sp++] = lumyr_array_flat(__v, lumyr_extract_int(__d)); }\n");
                        else
                            fprintf(out, "    { Value __v = __stk[--__sp]; __stk[__sp++] = lumyr_array_flat(__v, 1); }\n");
                        break;
                    case BUILTIN_QS:
                        if(in.b >= 2)
                            fprintf(out, "    { Value __e = __stk[--__sp]; Value __v = __stk[--__sp]; if(__v.type == VAL_MAP || __v.type == VAL_ARRAY) { char* __q = lumyr_qs_stringify_enc(__v, __e); __stk[__sp++] = lumyr_make_string(__q); free(__q); } else if(__v.type == VAL_STRING) { __stk[__sp++] = lumyr_qs_parse_enc(lumyr_str_cstr(&__v), __e); } else runtime_error(\"qs() 参数必须是字典/数组（序列化）或字符串（解析）\"); }\n");
                        else
                            fprintf(out, "    { Value __v = __stk[--__sp]; if(__v.type == VAL_MAP || __v.type == VAL_ARRAY) { char* __q = lumyr_qs_stringify_enc(__v, val_none()); __stk[__sp++] = lumyr_make_string(__q); free(__q); } else if(__v.type == VAL_STRING) { __stk[__sp++] = lumyr_qs_parse_enc(lumyr_str_cstr(&__v), val_none()); } else runtime_error(\"qs() 参数必须是字典/数组（序列化）或字符串（解析）\"); }\n");
                        break;
                    case BUILTIN_ARRAY_ADDALL:
                        fprintf(out, "    { Value __b = __stk[--__sp]; lumyr_array_addall(&__stk[__sp-1], __b); }\n");
                        break;
                    case BUILTIN_BYTES:
                        if(in.b >= 2)
                            fprintf(out, "    { Value __e = __stk[--__sp]; Value __v = __stk[--__sp]; __stk[__sp++] = lumyr_to_bytes(__v, __e); }\n");
                        else
                            fprintf(out, "    { Value __v = __stk[--__sp]; __stk[__sp++] = lumyr_to_bytes(__v, val_none()); }\n");
                        break;
                    case BUILTIN_STR:
                        if(in.b >= 2)
                            fprintf(out, "    { Value __e = __stk[--__sp]; Value __v = __stk[--__sp]; __stk[__sp++] = lumyr_from_bytes(__v, __e); }\n");
                        else
                            fprintf(out, "    { Value __v = __stk[--__sp]; __stk[__sp++] = lumyr_from_bytes(__v, val_none()); }\n");
                        break;
                    case BUILTIN_ENCODE:
                        if(in.b >= 2)
                            fprintf(out, "    { Value __e = __stk[--__sp]; Value __v = __stk[--__sp]; __stk[__sp++] = lumyr_to_bytes(__v, __e); }\n");
                        else
                            fprintf(out, "    { Value __v = __stk[--__sp]; __stk[__sp++] = lumyr_to_bytes(__v, val_none()); }\n");
                        break;
                    case BUILTIN_DECODE:
                        if(in.b >= 2)
                            fprintf(out, "    { Value __e = __stk[--__sp]; Value __v = __stk[--__sp]; __stk[__sp++] = lumyr_from_bytes(__v, __e); }\n");
                        else
                            fprintf(out, "    { Value __v = __stk[--__sp]; __stk[__sp++] = lumyr_from_bytes(__v, val_none()); }\n");
                        break;
                    case BUILTIN_ENCODE_URL:
                        fprintf(out, "    { Value __v = __stk[--__sp]; char* __r = lumyr_url_encode(__v.type==VAL_STRING?(lumyr_str_cstr(&__v)?lumyr_str_cstr(&__v):\"\"):\"\"); __stk[__sp++] = lumyr_make_string(__r); free(__r); }\n");
                        break;
                    case BUILTIN_DECODE_URL:
                        fprintf(out, "    { Value __v = __stk[--__sp]; char* __r = lumyr_url_decode(__v.type==VAL_STRING?(lumyr_str_cstr(&__v)?lumyr_str_cstr(&__v):\"\"):\"\"); __stk[__sp++] = lumyr_make_string(__r); free(__r); }\n");
                        break;
                    case BUILTIN_MD5:
                        fprintf(out, "    { Value __v = __stk[--__sp]; const char* __i = __v.type==VAL_STRING?(lumyr_str_cstr(&__v)?lumyr_str_cstr(&__v):\"\"):\"\"; char* __r = lumyr_md5_hex(__i, (int)strlen(__i)); __stk[__sp++] = lumyr_make_string(__r); free(__r); }\n");
                        break;
                    case BUILTIN_ENCODE_BASE64:
                        fprintf(out, "    { Value __v = __stk[--__sp]; const char* __i = __v.type==VAL_STRING?(lumyr_str_cstr(&__v)?lumyr_str_cstr(&__v):\"\"):\"\"; char* __r = lumyr_base64_encode(__i, (int)strlen(__i)); __stk[__sp++] = lumyr_make_string(__r); free(__r); }\n");
                        break;
                    case BUILTIN_DECODE_BASE64:
                        fprintf(out, "    { Value __v = __stk[--__sp]; int __ol=0; char* __r = lumyr_base64_decode(__v.type==VAL_STRING?(lumyr_str_cstr(&__v)?lumyr_str_cstr(&__v):\"\"):\"\", &__ol); __stk[__sp++] = lumyr_make_string(__r); free(__r); }\n");
                        break;
                    case BUILTIN_REGEX_MATCH:
                        fprintf(out, "    { Value __p=__stk[--__sp]; Value __s=__stk[--__sp]; __stk[__sp++]=lumyr_make_bool(lumyr_regex_match(__s.type==VAL_STRING?(lumyr_str_cstr(&__s)?lumyr_str_cstr(&__s):\"\"):\"\", __p.type==VAL_STRING?(lumyr_str_cstr(&__p)?lumyr_str_cstr(&__p):\"\"):\"\")); }\n");
                        break;
                    case BUILTIN_REGEX_SEARCH:
                        fprintf(out, "    { Value __p=__stk[--__sp]; Value __s=__stk[--__sp]; __stk[__sp++]=lumyr_regex_search(__s.type==VAL_STRING?(lumyr_str_cstr(&__s)?lumyr_str_cstr(&__s):\"\"):\"\", __p.type==VAL_STRING?(lumyr_str_cstr(&__p)?lumyr_str_cstr(&__p):\"\"):\"\"); }\n");
                        break;
                    case BUILTIN_REGEX_REPLACE:
                        fprintf(out, "    { Value __r=__stk[--__sp]; Value __p=__stk[--__sp]; Value __s=__stk[--__sp]; char* __o=lumyr_regex_replace(__s.type==VAL_STRING?(lumyr_str_cstr(&__s)?lumyr_str_cstr(&__s):\"\"):\"\", __p.type==VAL_STRING?(lumyr_str_cstr(&__p)?lumyr_str_cstr(&__p):\"\"):\"\", __r.type==VAL_STRING?(lumyr_str_cstr(&__r)?lumyr_str_cstr(&__r):\"\"):\"\"); __stk[__sp++]=lumyr_make_string(__o); free(__o); }\n");
                        break;
                    case BUILTIN_NOW:
                        fprintf(out, "    __stk[__sp++] = lumyr_now();\n");
                        break;
                    case BUILTIN_TIMESTAMP:
                        fprintf(out, "    __stk[__sp++] = lumyr_make_double(lumyr_timestamp());\n");
                        break;
                    case BUILTIN_TIMESTAMP_MS:
                        fprintf(out, "    __stk[__sp++] = lumyr_make_int(lumyr_timestamp_ms());\n");
                        break;
                    case BUILTIN_SLEEP:
                        fprintf(out, "    { Value __v=__stk[--__sp]; lumyr_sleep_ms((long long)lumyr_extract_int(__v)); __stk[__sp++]=val_none(); }\n");
                        break;
                    case BUILTIN_DATE:
                        fprintf(out, "    { char* __r=lumyr_date_str(); __stk[__sp++]=lumyr_make_string(__r); free(__r); }\n");
                        break;
                    case BUILTIN_TIME:
                        fprintf(out, "    { char* __r=lumyr_time_str(); __stk[__sp++]=lumyr_make_string(__r); free(__r); }\n");
                        break;
                    case BUILTIN_DATETIME:
                        fprintf(out, "    { char* __r=lumyr_datetime_str(); __stk[__sp++]=lumyr_make_string(__r); free(__r); }\n");
                        break;
                    case BUILTIN_FORMAT_TIME:
                        if(in.b >= 2)
                            fprintf(out, "    { Value __t=__stk[--__sp]; Value __f=__stk[--__sp]; double __ts=__t.type==VAL_DOUBLE?__t.v.d:(double)lumyr_extract_int(__t); char* __r=lumyr_format_time(__f.type==VAL_STRING?(lumyr_str_cstr(&__f)?lumyr_str_cstr(&__f):\"\"):\"\", __ts); __stk[__sp++]=lumyr_make_string(__r); free(__r); }\n");
                        else
                            fprintf(out, "    { Value __f=__stk[--__sp]; char* __r=lumyr_format_time(__f.type==VAL_STRING?(lumyr_str_cstr(&__f)?lumyr_str_cstr(&__f):\"\"):\"\", -1.0); __stk[__sp++]=lumyr_make_string(__r); free(__r); }\n");
                        break;
                    case BUILTIN_LOG_DEBUG:
                    case BUILTIN_LOG_INFO:
                    case BUILTIN_LOG_WARN:
                    case BUILTIN_LOG_ERROR:
                    case BUILTIN_LOG_FATAL:
                        if(in.b >= 2)
                            fprintf(out, "    { Value __m=__stk[--__sp]; __stk[--__sp]; char* __s=value_to_str(__m); lumyr_log(%d, __s); free(__s); __stk[__sp++]=val_none(); }\n", in.a - BUILTIN_LOG_DEBUG);
                        else
                            fprintf(out, "    { Value __m=__stk[--__sp]; char* __s=value_to_str(__m); lumyr_log(%d, __s); free(__s); __stk[__sp++]=val_none(); }\n", in.a - BUILTIN_LOG_DEBUG);
                        break;
                    case BUILTIN_GC_COUNT:
                        fprintf(out, "    __stk[__sp++] = lumyr_make_int((long long)gc_count());\n");
                        break;
                    case BUILTIN_GC_BYTES:
                        fprintf(out, "    __stk[__sp++] = lumyr_make_int((long long)gc_bytes());\n");
                        break;
                    case BUILTIN_GC_COLLECT:
                        fprintf(out, "    { gc_collect_now(); __stk[__sp++] = val_none(); }\n");
                        break;
                    case BUILTIN_GC_STW_NS:
                        fprintf(out, "    __stk[__sp++] = lumyr_make_int((long long)gc_stw_time_ns());\n");
                        break;
                    case BUILTIN_HTTP_DELETE:
                    case BUILTIN_HTTP_HEAD:
                    case BUILTIN_HTTP_PATCH: {
                        const char* m = "GET";
                        switch(in.a) {
                            case BUILTIN_HTTP_POST:   m = "POST"; break;
                            case BUILTIN_HTTP_PUT:    m = "PUT"; break;
                            case BUILTIN_ARRAY_ADD:
                        if(in.b == 3)
                            fprintf(out, "    { Value __v = __stk[--__sp]; Value __k = __stk[--__sp]; Value __m = __stk[--__sp]; __stk[__sp++] = lumyr_map_add(__m, __k, __v); }\n");
                        else
                            fprintf(out, "    { Value __v = __stk[--__sp]; lumyr_array_add(&__stk[__sp-1], __v); }\n");
                        break;
                    case BUILTIN_ARRAY_REMOVE:
                        fprintf(out, "    { Value __idx = __stk[--__sp]; lumyr_del(&__stk[__sp-1], __idx); }\n");
                        break;
                    case BUILTIN_ARRAY_INDEXOF:
                        fprintf(out, "    { Value __x = __stk[--__sp]; Value __arr = __stk[--__sp]; __stk[__sp++] = lumyr_index_of(__arr, __x); }\n");
                        break;
                    case BUILTIN_ARRAY_GET:
                        fprintf(out, "    { Value __i = __stk[--__sp]; Value __arr = __stk[--__sp]; __stk[__sp++] = lumyr_array_get_safe(__arr, __i); }\n");
                        break;
                    case BUILTIN_ARRAY_SET:
                        fprintf(out, "    { Value __v = __stk[--__sp]; Value __i = __stk[--__sp]; Value __arr = __stk[--__sp]; __stk[__sp++] = lumyr_array_set_method(__arr, __i, __v); }\n");
                        break;
                    case BUILTIN_ARRAY_FIRST:
                        fprintf(out, "    { Value __arr = __stk[__sp-1]; __stk[__sp-1] = lumyr_array_first(__arr); }\n");
                        break;
                    case BUILTIN_ARRAY_LAST:
                        fprintf(out, "    { Value __arr = __stk[__sp-1]; __stk[__sp-1] = lumyr_array_last(__arr); }\n");
                        break;
                    case BUILTIN_ARRAY_CLEAR:
                        fprintf(out, "    { lumyr_array_clear(&__stk[__sp-1]); }\n");
                        break;
                    case BUILTIN_MAP_HAS:
                        fprintf(out, "    { Value __k = __stk[--__sp]; Value __m = __stk[--__sp]; __stk[__sp++] = lumyr_make_bool(lumyr_map_has(__m, __k)); }\n");
                        break;
                    case BUILTIN_JSON:
                        if(in.b >= 2)
                            fprintf(out, "    { Value __e = __stk[--__sp]; Value __v = __stk[--__sp]; __stk[__sp++] = lumyr_json_parse_enc(lumyr_str_cstr(&__v), __e); }\n");
                        else
                            fprintf(out, "    { Value __v = __stk[--__sp]; __stk[__sp++] = lumyr_json_parse_enc(lumyr_str_cstr(&__v), val_none()); }\n");
                        break;
                    case BUILTIN_STRINGIFY:
                        if(in.b >= 2)
                            fprintf(out, "    { Value __e = __stk[--__sp]; Value __v = __stk[--__sp]; char* __js = lumyr_json_stringify_enc(__v, __e); Value __r = lumyr_make_string(__js); free(__js); __stk[__sp++] = __r; }\n");
                        else
                            fprintf(out, "    { Value __v = __stk[--__sp]; char* __js = lumyr_json_stringify_enc(__v, val_none()); Value __r = lumyr_make_string(__js); free(__js); __stk[__sp++] = __r; }\n");
                        break;
                    case BUILTIN_ARRAY_FLAT:
                        if(in.b >= 2)
                            fprintf(out, "    { Value __d = __stk[--__sp]; Value __v = __stk[--__sp]; __stk[__sp++] = lumyr_array_flat(__v, lumyr_extract_int(__d)); }\n");
                        else
                            fprintf(out, "    { Value __v = __stk[--__sp]; __stk[__sp++] = lumyr_array_flat(__v, 1); }\n");
                        break;
                    case BUILTIN_QS:
                        if(in.b >= 2)
                            fprintf(out, "    { Value __e = __stk[--__sp]; Value __v = __stk[--__sp]; if(__v.type == VAL_MAP || __v.type == VAL_ARRAY) { char* __q = lumyr_qs_stringify_enc(__v, __e); __stk[__sp++] = lumyr_make_string(__q); free(__q); } else if(__v.type == VAL_STRING) { __stk[__sp++] = lumyr_qs_parse_enc(lumyr_str_cstr(&__v), __e); } else runtime_error(\"qs() 参数必须是字典/数组（序列化）或字符串（解析）\"); }\n");
                        else
                            fprintf(out, "    { Value __v = __stk[--__sp]; if(__v.type == VAL_MAP || __v.type == VAL_ARRAY) { char* __q = lumyr_qs_stringify_enc(__v, val_none()); __stk[__sp++] = lumyr_make_string(__q); free(__q); } else if(__v.type == VAL_STRING) { __stk[__sp++] = lumyr_qs_parse_enc(lumyr_str_cstr(&__v), val_none()); } else runtime_error(\"qs() 参数必须是字典/数组（序列化）或字符串（解析）\"); }\n");
                        break;
                    case BUILTIN_ARRAY_ADDALL:
                        fprintf(out, "    { Value __b = __stk[--__sp]; lumyr_array_addall(&__stk[__sp-1], __b); }\n");
                        break;
                    case BUILTIN_BYTES:
                        if(in.b >= 2)
                            fprintf(out, "    { Value __e = __stk[--__sp]; Value __v = __stk[--__sp]; __stk[__sp++] = lumyr_to_bytes(__v, __e); }\n");
                        else
                            fprintf(out, "    { Value __v = __stk[--__sp]; __stk[__sp++] = lumyr_to_bytes(__v, val_none()); }\n");
                        break;
                    case BUILTIN_STR:
                        if(in.b >= 2)
                            fprintf(out, "    { Value __e = __stk[--__sp]; Value __v = __stk[--__sp]; __stk[__sp++] = lumyr_from_bytes(__v, __e); }\n");
                        else
                            fprintf(out, "    { Value __v = __stk[--__sp]; __stk[__sp++] = lumyr_from_bytes(__v, val_none()); }\n");
                        break;
                    case BUILTIN_ENCODE:
                        if(in.b >= 2)
                            fprintf(out, "    { Value __e = __stk[--__sp]; Value __v = __stk[--__sp]; __stk[__sp++] = lumyr_to_bytes(__v, __e); }\n");
                        else
                            fprintf(out, "    { Value __v = __stk[--__sp]; __stk[__sp++] = lumyr_to_bytes(__v, val_none()); }\n");
                        break;
                    case BUILTIN_DECODE:
                        if(in.b >= 2)
                            fprintf(out, "    { Value __e = __stk[--__sp]; Value __v = __stk[--__sp]; __stk[__sp++] = lumyr_from_bytes(__v, __e); }\n");
                        else
                            fprintf(out, "    { Value __v = __stk[--__sp]; __stk[__sp++] = lumyr_from_bytes(__v, val_none()); }\n");
                        break;
                    case BUILTIN_ENCODE_URL:
                        fprintf(out, "    { Value __v = __stk[--__sp]; char* __r = lumyr_url_encode(__v.type==VAL_STRING?(lumyr_str_cstr(&__v)?lumyr_str_cstr(&__v):\"\"):\"\"); __stk[__sp++] = lumyr_make_string(__r); free(__r); }\n");
                        break;
                    case BUILTIN_DECODE_URL:
                        fprintf(out, "    { Value __v = __stk[--__sp]; char* __r = lumyr_url_decode(__v.type==VAL_STRING?(lumyr_str_cstr(&__v)?lumyr_str_cstr(&__v):\"\"):\"\"); __stk[__sp++] = lumyr_make_string(__r); free(__r); }\n");
                        break;
                    case BUILTIN_MD5:
                        fprintf(out, "    { Value __v = __stk[--__sp]; const char* __i = __v.type==VAL_STRING?(lumyr_str_cstr(&__v)?lumyr_str_cstr(&__v):\"\"):\"\"; char* __r = lumyr_md5_hex(__i, (int)strlen(__i)); __stk[__sp++] = lumyr_make_string(__r); free(__r); }\n");
                        break;
                    case BUILTIN_ENCODE_BASE64:
                        fprintf(out, "    { Value __v = __stk[--__sp]; const char* __i = __v.type==VAL_STRING?(lumyr_str_cstr(&__v)?lumyr_str_cstr(&__v):\"\"):\"\"; char* __r = lumyr_base64_encode(__i, (int)strlen(__i)); __stk[__sp++] = lumyr_make_string(__r); free(__r); }\n");
                        break;
                    case BUILTIN_DECODE_BASE64:
                        fprintf(out, "    { Value __v = __stk[--__sp]; int __ol=0; char* __r = lumyr_base64_decode(__v.type==VAL_STRING?(lumyr_str_cstr(&__v)?lumyr_str_cstr(&__v):\"\"):\"\", &__ol); __stk[__sp++] = lumyr_make_string(__r); free(__r); }\n");
                        break;
                    case BUILTIN_REGEX_MATCH:
                        fprintf(out, "    { Value __p=__stk[--__sp]; Value __s=__stk[--__sp]; __stk[__sp++]=lumyr_make_bool(lumyr_regex_match(__s.type==VAL_STRING?(lumyr_str_cstr(&__s)?lumyr_str_cstr(&__s):\"\"):\"\", __p.type==VAL_STRING?(lumyr_str_cstr(&__p)?lumyr_str_cstr(&__p):\"\"):\"\")); }\n");
                        break;
                    case BUILTIN_REGEX_SEARCH:
                        fprintf(out, "    { Value __p=__stk[--__sp]; Value __s=__stk[--__sp]; __stk[__sp++]=lumyr_regex_search(__s.type==VAL_STRING?(lumyr_str_cstr(&__s)?lumyr_str_cstr(&__s):\"\"):\"\", __p.type==VAL_STRING?(lumyr_str_cstr(&__p)?lumyr_str_cstr(&__p):\"\"):\"\"); }\n");
                        break;
                    case BUILTIN_REGEX_REPLACE:
                        fprintf(out, "    { Value __r=__stk[--__sp]; Value __p=__stk[--__sp]; Value __s=__stk[--__sp]; char* __o=lumyr_regex_replace(__s.type==VAL_STRING?(lumyr_str_cstr(&__s)?lumyr_str_cstr(&__s):\"\"):\"\", __p.type==VAL_STRING?(lumyr_str_cstr(&__p)?lumyr_str_cstr(&__p):\"\"):\"\", __r.type==VAL_STRING?(lumyr_str_cstr(&__r)?lumyr_str_cstr(&__r):\"\"):\"\"); __stk[__sp++]=lumyr_make_string(__o); free(__o); }\n");
                        break;
                    case BUILTIN_NOW:
                        fprintf(out, "    __stk[__sp++] = lumyr_now();\n");
                        break;
                    case BUILTIN_TIMESTAMP:
                        fprintf(out, "    __stk[__sp++] = lumyr_make_double(lumyr_timestamp());\n");
                        break;
                    case BUILTIN_TIMESTAMP_MS:
                        fprintf(out, "    __stk[__sp++] = lumyr_make_int(lumyr_timestamp_ms());\n");
                        break;
                    case BUILTIN_SLEEP:
                        fprintf(out, "    { Value __v=__stk[--__sp]; lumyr_sleep_ms((long long)lumyr_extract_int(__v)); __stk[__sp++]=val_none(); }\n");
                        break;
                    case BUILTIN_DATE:
                        fprintf(out, "    { char* __r=lumyr_date_str(); __stk[__sp++]=lumyr_make_string(__r); free(__r); }\n");
                        break;
                    case BUILTIN_TIME:
                        fprintf(out, "    { char* __r=lumyr_time_str(); __stk[__sp++]=lumyr_make_string(__r); free(__r); }\n");
                        break;
                    case BUILTIN_DATETIME:
                        fprintf(out, "    { char* __r=lumyr_datetime_str(); __stk[__sp++]=lumyr_make_string(__r); free(__r); }\n");
                        break;
                    case BUILTIN_FORMAT_TIME:
                        if(in.b >= 2)
                            fprintf(out, "    { Value __t=__stk[--__sp]; Value __f=__stk[--__sp]; double __ts=__t.type==VAL_DOUBLE?__t.v.d:(double)lumyr_extract_int(__t); char* __r=lumyr_format_time(__f.type==VAL_STRING?(lumyr_str_cstr(&__f)?lumyr_str_cstr(&__f):\"\"):\"\", __ts); __stk[__sp++]=lumyr_make_string(__r); free(__r); }\n");
                        else
                            fprintf(out, "    { Value __f=__stk[--__sp]; char* __r=lumyr_format_time(__f.type==VAL_STRING?(lumyr_str_cstr(&__f)?lumyr_str_cstr(&__f):\"\"):\"\", -1.0); __stk[__sp++]=lumyr_make_string(__r); free(__r); }\n");
                        break;
                    case BUILTIN_LOG_DEBUG:
                    case BUILTIN_LOG_INFO:
                    case BUILTIN_LOG_WARN:
                    case BUILTIN_LOG_ERROR:
                    case BUILTIN_LOG_FATAL:
                        if(in.b >= 2)
                            fprintf(out, "    { Value __m=__stk[--__sp]; __stk[--__sp]; char* __s=value_to_str(__m); lumyr_log(%d, __s); free(__s); __stk[__sp++]=val_none(); }\n", in.a - BUILTIN_LOG_DEBUG);
                        else
                            fprintf(out, "    { Value __m=__stk[--__sp]; char* __s=value_to_str(__m); lumyr_log(%d, __s); free(__s); __stk[__sp++]=val_none(); }\n", in.a - BUILTIN_LOG_DEBUG);
                        break;
                    case BUILTIN_HTTP_DELETE: m = "DELETE"; break;
                            case BUILTIN_HTTP_HEAD:   m = "HEAD"; break;
                            case BUILTIN_HTTP_PATCH:  m = "PATCH"; break;
                            default: break;
                        }
                        if(in.b == 1)
                            fprintf(out, "    { Value __r = lumyr_http_request(\"%s\", __stk[__sp-1], val_none(), val_none()); __stk[__sp-1] = __r; __sp = __sp - 1 + 1; }\n", m);
                        else if(in.b == 2)
                            fprintf(out, "    { Value __r = lumyr_http_request(\"%s\", __stk[__sp-2], __stk[__sp-1], val_none()); __stk[__sp-2] = __r; __sp = __sp - 2 + 1; }\n", m);
                        else
                            fprintf(out, "    { Value __r = lumyr_http_request(\"%s\", __stk[__sp-3], __stk[__sp-2], __stk[__sp-1]); __stk[__sp-3] = __r; __sp = __sp - 3 + 1; }\n", m);
                        break;
                    }
                    case BUILTIN_VALUES:
                        fprintf(out, "    { Value __r = lumyr_map_values(__stk[__sp - %d]); __stk[__sp - %d] = __r; __sp = __sp - %d + 1; }\n", in.b, in.b, in.b);
                        break;
                    case BUILTIN_CHAIN: {
                        fprintf(out, "    { Value g2 = __stk[--__sp]; Value g1 = __stk[--__sp]; __stk[__sp++] = lumyr_wrap_create(WRAP_CHAIN, g1, g2, val_none(), 0); }\n");
                        break;
                    }
                    case BUILTIN_ZIP: {
                        fprintf(out, "    { Value g2 = __stk[--__sp]; Value g1 = __stk[--__sp]; __stk[__sp++] = lumyr_wrap_create(WRAP_ZIP, g1, g2, val_none(), 0); }\n");
                        break;
                    }
                    case BUILTIN_SKIP: {
                        fprintf(out, "    { Value n = __stk[--__sp]; Value g = __stk[--__sp]; __stk[__sp++] = lumyr_wrap_create(WRAP_SKIP, g, val_none(), val_none(), lumyr_extract_int(n)); }\n");
                        break;
                    }
                    case BUILTIN_TAKE: {
                        fprintf(out, "    { Value n = __stk[--__sp]; Value g = __stk[--__sp]; __stk[__sp++] = lumyr_wrap_create(WRAP_TAKE, g, val_none(), val_none(), lumyr_extract_int(n)); }\n");
                        break;
                    }
                    case BUILTIN_ENUMERATE: {
                        fprintf(out, "    { Value g = __stk[--__sp]; __stk[__sp++] = lumyr_wrap_create(WRAP_ENUMERATE, g, val_none(), val_none(), 0); }\n");
                        break;
                    }
                    case BUILTIN_MAP:
                    case BUILTIN_FILTER:
                    case BUILTIN_REDUCE: {
                        int argc = in.b;
                        fprintf(out, "    {\n");
                        fprintf(out, "        Value __fn, __arr, __init = val_none();\n");
                        if(argc == 3)
                            fprintf(out, "        __init = __stk[--__sp]; __fn = __stk[--__sp]; __arr = __stk[--__sp];\n");
                        else
                            fprintf(out, "        __fn = __stk[--__sp]; __arr = __stk[--__sp];\n");
                        if(in.a == BUILTIN_MAP) {
                            /* 字典 map：fn(value, key) → 新字典（键不变值映射） */
                            fprintf(out, "        if(__arr.type == VAL_MAP) {\n");
                            fprintf(out, "            if(__fn.type != VAL_FUNC) runtime_error(\"map() 第二个参数必须是函数\");\n");
                            fprintf(out, "            Value (*__cfm)(Value*, int) = (Value(*)(Value*, int))((RuntimeFunc*)__fn.v.func.func_obj)->entry;\n");
                            fprintf(out, "            Value __mout = val_map();\n");
                            fprintf(out, "            MapIter __it; map_iter_init(&__it, __arr.v.map);\n");
                            fprintf(out, "            Value __mk, __mv;\n");
                            fprintf(out, "            while(map_iter_next(&__it, &__mk, &__mv)) {\n");
                            fprintf(out, "                Value __a2[2]; __a2[0] = __mv; __a2[1] = __mk;\n");
                            fprintf(out, "                Value __r = __cfm(__a2, 2);\n");
                            fprintf(out, "                lumyr_map_set(&__mout, __mk, __r);\n");
                            fprintf(out, "            }\n");
                            fprintf(out, "            __stk[__sp++] = __mout;\n");
                            fprintf(out, "        } else {\n");
                        }
                        fprintf(out, "        if(__arr.type == VAL_GENERATOR) {\n");
                        fprintf(out, "            if(__fn.type != VAL_FUNC) runtime_error(\"map()/filter() 第二个参数必须是函数\");\n");
                        const char* __wtype = (in.a == BUILTIN_MAP) ? "WRAP_MAP" : "WRAP_FILTER";
                        fprintf(out, "            __stk[__sp++] = lumyr_wrap_create(%s, __arr, val_none(), __fn, 0);\n", __wtype);
                        int __gmlid = s_gen_map_label_id++;
                        fprintf(out, "            goto __gen_map_done_%d;\n", __gmlid);
                        fprintf(out, "        }\n");
                        fprintf(out, "        if(__arr.type != VAL_ARRAY) runtime_error(\"map()/filter()/reduce() 第一个参数必须是数组或生成器\");\n");
                        fprintf(out, "        if(__fn.type != VAL_FUNC) runtime_error(\"map()/filter()/reduce() 第二个参数必须是函数\");\n");
                        fprintf(out, "        Value (*__cf)(Value*, int) = (Value(*)(Value*, int))((RuntimeFunc*)__fn.v.func.func_obj)->entry;\n");
                        fprintf(out, "        int __n = __arr.v.array->len;\n");
                        if(in.a == BUILTIN_MAP) {
                            fprintf(out, "        Value __out = val_array(__n);\n");
                            fprintf(out, "        for(int __i = 0; __i < __n; __i++) {\n");
                            fprintf(out, "            Value __a1[1]; __a1[0] = __arr.v.array->items[__i];\n");
                            fprintf(out, "            Value __r = __cf(__a1, 1);\n");
                            fprintf(out, "            gc_write_barrier(__r);\n");
                            fprintf(out, "            __out.v.array->items[__i] = __r;\n");
                            fprintf(out, "        }\n");
                            fprintf(out, "        __stk[__sp++] = __out;\n");
                        } else if(in.a == BUILTIN_FILTER) {
                            fprintf(out, "        Value __out = val_array(__n); int __cnt = 0;\n");
                            fprintf(out, "        for(int __i = 0; __i < __n; __i++) {\n");
                            fprintf(out, "            Value __a1[1]; __a1[0] = __arr.v.array->items[__i];\n");
                            fprintf(out, "            Value __r = __cf(__a1, 1);\n");
                            fprintf(out, "            if(lumyr_to_bool(__r)) { gc_write_barrier(__arr.v.array->items[__i]); __out.v.array->items[__cnt++] = __arr.v.array->items[__i]; }\n");
                            fprintf(out, "        }\n");
                            fprintf(out, "        __out.v.array->len = __cnt;\n");
                            fprintf(out, "        __stk[__sp++] = __out;\n");
                        } else {
                            fprintf(out, "        Value __acc = __init;\n");
                            fprintf(out, "        for(int __i = 0; __i < __n; __i++) {\n");
                            fprintf(out, "            Value __a2[2]; __a2[0] = __acc; __a2[1] = __arr.v.array->items[__i];\n");
                            fprintf(out, "            __acc = __cf(__a2, 2);\n");
                            fprintf(out, "        }\n");
                            fprintf(out, "        __stk[__sp++] = __acc;\n");
                        }
                        if(in.a == BUILTIN_MAP) {
                            fprintf(out, "        }\n");  /* 闭合 map 字典分支的 else */
                        }
                        fprintf(out, "    __gen_map_done_%d:;\n", __gmlid);
                        fprintf(out, "    }\n");
                        break;
                    }
                    case BUILTIN_AVG:
                        fprintf(out, "    { Value __v = __stk[__sp-1]; __stk[__sp-1] = lumyr_avg(__v); }\n");
                        break;
                }
                break;
            case OPC_PRINT: {
                /* 多参数打印：in.a = 参数数量；循环打印后弹出所有参数 */
                fprintf(out, "    { int __pcnt = %d; if(__pcnt <= 0) __pcnt = 1; int __pbase = __sp - __pcnt;\n", in.a);
                fprintf(out, "      for(int __pi = 0; __pi < __pcnt; __pi++) { if(__pi > 0) printf(\" \"); lumyr_print_inline(__stk[__pbase + __pi]); }\n");
                fprintf(out, "      printf(\"\\n\"); __sp -= __pcnt; }\n");
                break;
            }
            case OPC_TO_BOOL:
                fprintf(out, "    __stk[__sp-1] = lumyr_make_bool(lumyr_to_bool(__stk[__sp-1]));\n");
                break;
            case OPC_DUP:
                fprintf(out, "    __stk[__sp] = __stk[__sp-1]; __sp++;\n");
                break;
            case OPC_POP:
                fprintf(out, "    __sp--;\n");
                break;
            case OPC_TRY:
                /* C 级错误处理：全局 jmp_buf 栈（longjmp 后自动变量不可靠，索引从 __g_depth 反推）
                   自闭合结构：setjmp 成功 → goto L(body)；失败 → 恢复本层并 goto L(catch) */
                fprintf(out, "    { int __d = __g_depth; __g_ensure(__d + 2); __g_tgt[__d] = %d; __g_sp0[__d] = __sp; __g_prev[__d] = g_err_jmp;\n", in.a);
                fprintf(out, "      __g_tn[__d] = g_trace_n; __g_fn[__d] = __g_fin_n;\n");
                fprintf(out, "      g_err_jmp = &__g_jbs[__d];\n");
                fprintf(out, "      CFrame* __cf_save = gc_cframe_top();\n");
                fprintf(out, "      if(setjmp(__g_jbs[__d]) == 0) { __g_depth = __d + 1; goto L%d; }\n", i + 1);
                fprintf(out, "      int __d2 = (int)(g_err_jmp - __g_jbs);\n");
                fprintf(out, "      if(__d2 < 0) __d2 = __g_depth - 1;\n");
                fprintf(out, "      __sp = __g_sp0[__d2]; __g_depth = __d2; g_err_jmp = __g_prev[__d2];\n");
                fprintf(out, "      gc_cframe_restore(__cf_save);\n");
                fprintf(out, "      goto L%d;\n", in.a);
                fprintf(out, "    }\n");
                fprintf(out, "    L%d:;\n", i + 1);
                break;
            case OPC_ENDTRY:
                /* 无 finally 布局：正常路径恢复外层处理器（setjmp 结构已在 TRY 处闭合） */
                fprintf(out, "    if(__g_depth > 0) { __g_depth--; g_err_jmp = __g_prev[__g_depth]; }\n");
                break;
            case OPC_GET_ERR: {
                /* 错误对象：type/message + 调用栈回溯；随后截断残留到本 TRY 层 */
                fprintf(out, "    { char* __st = lumyr_build_stack_trace();\n");
                fprintf(out, "      __stk[__sp++] = lumyr_make_error(g_err_type, g_err_msg, __st);\n");
                fprintf(out, "      free(__st);\n");
                fprintf(out, "      g_trace_n = __g_tn[__g_depth]; __g_fin_n = __g_fn[__g_depth];\n");
                fprintf(out, "    }\n");
                break;
            }
            case OPC_THROW:
                /* throw：包装成错误对象抛出（字符串→type Error；错误对象→原样；map→type/message） */
                fprintf(out, "    { Value __v = __stk[--__sp];\n");
                fprintf(out, "      const char* __tp = \"Error\"; char* __msg = NULL;\n");
                fprintf(out, "      if(__v.type == VAL_ERROR) { __tp = __v.v.err.type ? __v.v.err.type : \"Error\"; __msg = strdup(__v.v.err.message ? __v.v.err.message : \"\"); }\n");
                fprintf(out, "      else if(__v.type == VAL_MAP) {\n");
                fprintf(out, "        if(lumyr_map_has(__v, lumyr_make_string(\"type\"))) { Value __tv = lumyr_map_get(__v, lumyr_make_string(\"type\")); if(__tv.type == VAL_STRING) __tp = lumyr_str_cstr(&__tv); }\n");
                fprintf(out, "        if(lumyr_map_has(__v, lumyr_make_string(\"message\"))) { Value __mv = lumyr_map_get(__v, lumyr_make_string(\"message\")); if(__mv.type == VAL_STRING) __msg = strdup(lumyr_str_cstr(&__mv)); }\n");
                fprintf(out, "      }\n");
                fprintf(out, "      if(!__msg) __msg = value_to_str(__v);\n");
                fprintf(out, "      g_err_type_set(__tp);\n");
                fprintf(out, "      g_err_msg_set(__msg);\n");
                fprintf(out, "      free(__msg);\n");
                fprintf(out, "      if(g_err_jmp) longjmp(*g_err_jmp, 1);\n");
                fprintf(out, "      fprintf(stderr, \"Runtime Error: %%s\\n\", g_err_msg); exit(EXIT_FAILURE);\n");
                fprintf(out, "    }\n");
                break;
            case OPC_FIN_PUSH: {
                /* finally 完成动作：1=JMP 2=RETHROW 3=BREAK 4=CONT 5=RETURN（b=目标 pc→label 编号） */
                int fidx = in.b ? fin_lab_idx_of(in.b) : 0;
                fprintf(out, "    __g_ensure(__g_fin_n + 2); __g_fin_act[__g_fin_n] = %d; __g_fin_tgt[__g_fin_n] = %d; __g_fin_dep[__g_fin_n] = (__g_depth > 0) ? __g_depth - 1 : 0; __g_fin_n++;\n", in.a, fidx);
                break;
            }
            case OPC_FINISH:
                /* 完成动作的目标在运行时才知道（__g_fin_tgt 存的是 label 编号），用跳转表 */
                fprintf(out, "    if(__g_fin_n <= 0) runtime_error(\"finally 完成栈为空\");\n");
                fprintf(out, "    { int __fa = __g_fin_act[--__g_fin_n];\n");
                fprintf(out, "      if(__fa == 1 || __fa == 3 || __fa == 4) { __g_depth = __g_fin_dep[__g_fin_n]; g_err_jmp = __g_prev[__g_fin_dep[__g_fin_n]]; goto *__g_fin_labs[__g_fin_tgt[__g_fin_n]]; }\n");
                fprintf(out, "      else if(__fa == 2) { if(g_err_jmp) longjmp(*g_err_jmp, 1); fprintf(stderr, \"Runtime Error: %%s\\n\", g_err_msg); exit(EXIT_FAILURE); }\n");
                if(!g_cur_fn)
                    fprintf(out, "      else if(__fa == 5) { __g_depth = __g_d0; g_err_jmp = __g_gj0; __g_fin_n = __g_fin0; gc_pop_cframe(); return 0; }\n");
                else
                    fprintf(out, "      else if(__fa == 5) { __g_depth = __g_d0; g_err_jmp = __g_gj0; __g_fin_n = __g_fin0; if(g_trace_n > 0) g_trace_n--; gc_pop_cframe(); { Value __v = __g_pend_val; gc_protect_push(__v); gc_protect_pop(); return __v; } }\n");
                fprintf(out, "      else runtime_error(\"finally 完成动作未知\");\n");
                fprintf(out, "    }\n");
                break;
            case OPC_PEND_RETURN:
                /* 挂起返回：弹1存 __g_pend_val → 压 RETURN 动作 → 跳 finally */
                fprintf(out, "    __g_pend_val = __stk[--__sp];\n");
                fprintf(out, "    __g_fin_act[__g_fin_n] = 5; __g_fin_tgt[__g_fin_n] = 0; __g_fin_n++;\n");
                if(in.b) fprintf(out, "    goto L%d;\n", in.b);
                break;
            case OPC_JMP:
                /* 循环回边（向后跳转）插入 STW 安全点：编译通道无解释循环安全点，
                 * 长循环中需主动检查 GC 是否运行，避免标记期间并发修改栈值 */
                if(in.a < i) fprintf(out, "    gc_stw_check_fast();\n");
                fprintf(out, "    goto L%d;\n", in.a);
                break;
            case OPC_JMP_IF_FALSE:
                fprintf(out, "    if (!lumyr_to_bool(__stk[--__sp])) goto L%d;\n", in.a);
                break;
            case OPC_JMP_IF_TRUE:
                fprintf(out, "    if (lumyr_to_bool(__stk[--__sp])) goto L%d;\n", in.a);
                break;
            case OPC_JMP_IF_NULL:
                fprintf(out, "    if (__stk[--__sp].type == VAL_NONE) goto L%d;\n", in.a);
                break;
            case OPC_CLASS_NEW: {
                /* 创建 class 实例（C 结构体）：malloc + 清零 + 设置 __classname__ + 用参数初始化字段 + 包装成 Value */
                /* 辅助函数：获取 vtable 指针和 __classname__ 的访问路径（考虑多层继承）
                   最顶层 class（无父类）：直接访问 __obj->vtable
                   有父类的 class：通过 super 链访问，如 __obj->super.vtable 或 __obj->super.super.vtable */
                const char* class_name = fn->syms[in.a];
                int argc = in.b;
                fprintf(out, "    {\n");
                fprintf(out, "        lumyr_class_%s* __obj = (lumyr_class_%s*)malloc(sizeof(lumyr_class_%s));\n", class_name, class_name, class_name);
                fprintf(out, "        memset(__obj, 0, sizeof(lumyr_class_%s));\n", class_name);
                {
                    /* 初始化 vtable 指针和 __classname__：
                       有父类的 class，vtable 和 __classname__ 在 super 中
                       没有父类的 class，vtable 和 __classname__ 直接在结构体中 */
                    TypeDef* _td_vt = type_lookup(class_name);
                    /* 计算继承链深度，生成正确的 vtable 访问路径 */
                    int _depth = 0;
                    TypeDef* _cur = _td_vt;
                    while(_cur && _cur->parent) {
                        _depth++;
                        _cur = type_lookup(_cur->parent);
                    }
                    if(_depth == 0) {
                        fprintf(out, "        __obj->vtable = &lumyr_class_%s_vtable;\n", class_name);
                    } else {
                        fprintf(out, "        __obj->");
                        for(int _d = 0; _d < _depth; _d++) fprintf(out, "super.");
                        fprintf(out, "vtable = &lumyr_class_%s_vtable;\n", class_name);
                    }
                }
                {
                    /* 子类结构体的 __classname__ 在 super 中，父类结构体直接有 __classname__ */
                    TypeDef* _td = type_lookup(class_name);
                    /* 计算继承链深度，生成正确的 __classname__ 访问路径 */
                    int _depth2 = 0;
                    TypeDef* _cur2 = _td;
                    while(_cur2 && _cur2->parent) {
                        _depth2++;
                        _cur2 = type_lookup(_cur2->parent);
                    }
                    if(_depth2 == 0) {
                        fprintf(out, "        __obj->__classname__ = \"%s\";\n", class_name);
                    } else {
                        fprintf(out, "        __obj->");
                        for(int _d2 = 0; _d2 < _depth2; _d2++) fprintf(out, "super.");
                        fprintf(out, "__classname__ = \"%s\";\n", class_name);
                    }
                }
                if(argc > 0) {
                    /* 有参数：从栈上弹出参数并按顺序初始化字段
                       参数压栈顺序是从左到右，所以第一个参数在栈底，最后一个参数在栈顶
                       需要先把所有参数取出来（从栈顶到栈底），然后按顺序初始化字段 */
                    TypeDef* td = type_lookup(class_name);
                    if(td && td->nprops > 0) {
                        int n = argc < td->nprops ? argc : td->nprops;
                        /* 先从栈上弹出所有参数，保存到局部数组 */
                        fprintf(out, "        Value __ctor_args[%d];\n", n);
                        fprintf(out, "        for(int __i = %d; __i >= 0; __i--) { __ctor_args[__i] = __stk[--__sp]; }\n", n - 1);
                        /* 按顺序初始化字段（父类字段用 super 前缀） */
                        for(int fi = 0; fi < n; fi++) {
                            const char* fname = td->props[fi];
                            int ck = td->field_cast_kinds ? td->field_cast_kinds[fi] : CAST_LONGLONG;
                            /* 判断是否是父类字段 */
                            int is_parent_field = 0;
                            if(td->parent) {
                                TypeDef* parent_td = type_lookup(td->parent);
                                if(parent_td) {
                                    for(int pfi = 0; pfi < parent_td->nprops; pfi++) {
                                        if(strcmp(parent_td->props[pfi], fname) == 0) {
                                            is_parent_field = 1;
                                            break;
                                        }
                                    }
                                }
                            }
                            const char* field_access = is_parent_field ? "super." : "";
                            if(ck == CAST_DOUBLE || ck == CAST_FLOAT || ck == CAST_LONG_DOUBLE) {
                                fprintf(out, "        __obj->%s%s = (double)__ctor_args[%d].v.d;\n", field_access, fname, fi);
                            } else if(ck == CAST_STRING) {
                                fprintf(out, "        __obj->%s%s = strdup(lumyr_str_cstr(&__ctor_args[%d]));\n", field_access, fname, fi);
                            } else if(ck == CAST_BOOL) {
                                fprintf(out, "        __obj->%s%s = (int)__ctor_args[%d].v.b;\n", field_access, fname, fi);
                            } else {
                                fprintf(out, "        __obj->%s%s = (long long)__ctor_args[%d].v.i;\n", field_access, fname, fi);
                            }
                        }
                    } else {
                        /* 没有找到类型定义，直接弹出参数丢弃 */
                        fprintf(out, "        __sp -= %d;\n", argc);
                    }
                }
                fprintf(out, "        Value __val = {0};\n");
                fprintf(out, "        __val.type = VAL_STRUCT_PTR;\n");
                fprintf(out, "        __val.v.struct_ptr = __obj;\n");
                fprintf(out, "        __stk[__sp++] = __val;\n");
                fprintf(out, "    }\n");
                break;
            }
            case OPC_CALL: {
                /* STW 安全点：函数调用前检查 GC，避免参数弹出期间并发标记读到 torn Value */
                fprintf(out, "    gc_stw_check_fast();\n");

                /* __super_call_<当前类名>_<方法名>(self, args...)：CC 模式编译期静态绑定 */
                if(nm && strncmp(nm, "__super_call_", 13) == 0) {
                    /* 解析函数名，获取当前类名和方法名 */
                    const char* rest = nm + 13;
                    char current_class_name[128] = {0};
                    char method_name[128] = {0};
                    const char* underscore = strchr(rest, '_');
                    if(underscore) {
                        int class_len = underscore - rest;
                        if(class_len > 0 && class_len < 127) {
                            strncpy(current_class_name, rest, class_len);
                            current_class_name[class_len] = '\0';
                        }
                        strncpy(method_name, underscore + 1, 127);
                        method_name[127] = '\0';
                    }
                    /* 根据当前类名查找父类 */
                    const char* parent_name = NULL;
                    if(current_class_name[0]) {
                        TypeDef* td = class_lookup(current_class_name);
                        if(td && td->parent) parent_name = td->parent;
                    }
                    /* 生成直接的父类方法调用：lumyr_func_<父类名>_<方法名>_<参数个数>(self, args...) */
                    int arg_count = in.b;  /* 实际参数个数（包括 self） */
                    if(parent_name && method_name[0]) {
                        fprintf(out, "    {\n");
                        fprintf(out, "        Value __super_args[%d];\n", arg_count > 0 ? arg_count : 1);
                        fprintf(out, "        for (int __k = 0; __k < %d; __k++) __super_args[__k] = __stk[__sp - %d + __k];\n", arg_count, arg_count);
                        fprintf(out, "        __sp -= %d;\n", in.b);
                        fprintf(out, "        __stk[__sp++] = lumyr_func_%s_%s_%d(", parent_name, method_name, arg_count);
                        for(int ak = 0; ak < arg_count; ak++) {
                            if(ak > 0) fprintf(out, ", ");
                            fprintf(out, "__super_args[%d]", ak);
                        }
                        fprintf(out, ");\n");
                        fprintf(out, "    }\n");
                    } else {
                        fprintf(out, "    {\n");
                        fprintf(out, "        runtime_error(\"super 调用失败：无法找到父类方法\");\n");
                        fprintf(out, "    }\n");
                    }
                    i++;
                    continue;
                }
                /* __super_ctor_<当前类名>(self, args...)：CC 模式编译期静态绑定 */
                if(nm && strncmp(nm, "__super_ctor_", 13) == 0) {
                    /* 解析函数名，获取当前类名 */
                    const char* current_class_name = nm + 13;
                    /* 根据当前类名查找父类 */
                    const char* parent_name = NULL;
                    if(current_class_name[0]) {
                        TypeDef* td = class_lookup(current_class_name);
                        if(td && td->parent) parent_name = td->parent;
                    }
                    /* 生成直接的父类构造函数调用 */
                    int arg_count = in.b;  /* 实际参数个数（包括 self） */
                    if(parent_name) {
                        fprintf(out, "    {\n");
                        fprintf(out, "        Value __super_ctor_args[%d];\n", arg_count > 0 ? arg_count : 1);
                        fprintf(out, "        for (int __k = 0; __k < %d; __k++) __super_ctor_args[__k] = __stk[__sp - %d + __k];\n", arg_count, arg_count);
                        fprintf(out, "        __sp -= %d;\n", in.b);
                        /* 调用父类构造函数：lumyr_func_<父类名>___init__(self, args...) */
                        fprintf(out, "        __stk[__sp++] = lumyr_func_%s___init__(", parent_name);
                        for(int ak = 0; ak < arg_count; ak++) {
                            if(ak > 0) fprintf(out, ", ");
                            fprintf(out, "__super_ctor_args[%d]", ak);
                        }
                        fprintf(out, ");\n");
                        fprintf(out, "    }\n");
                    } else {
                        /* 如果没有自定义构造函数，直接返回 self */
                        fprintf(out, "    {\n");
                        fprintf(out, "        Value __super_self = __stk[__sp - 1];\n");
                        fprintf(out, "        __sp -= %d;\n", in.b);
                        fprintf(out, "        __stk[__sp++] = __super_self;\n");
                        fprintf(out, "    }\n");
                    }
                    i++;
                    continue;
                }

                /* 帧链优先（VM 语义）：名字是局部/全局变量时按函数值动态调用，
                   与具名全局函数冲突时以变量为准（局部闭包遮蔽全局函数） */
                int is_var = (g_cur_fn && (fn_has_param(g_cur_fn, nm) || ns_has(&fn_locals, nm) ||
                                           cap_index_of(nm) >= 0)) ||
                             ns_has(&g_globals, nm);
                if(is_var) {
                    int argc = in.b;
                    fprintf(out, "    {\n");
                    fprintf(out, "        Value __f = %s;\n", cvar_rw(nm));
                    fprintf(out, "        if(__f.type != VAL_FUNC) runtime_error(\"尝试调用非函数: %s\");\n", nm);
                    fprintf(out, "        int __lmin_argc = %d;\n", argc);
                    fprintf(out, "        Value __args[%d];\n", argc > 0 ? argc : 1);
                    fprintf(out, "        for (int __k = 0; __k < __lmin_argc; __k++) __args[__k] = __stk[__sp - __lmin_argc + __k];\n");
                    fprintf(out, "        __sp -= __lmin_argc;\n");
                    fprintf(out, "        RuntimeFunc* __rf = (RuntimeFunc*)__f.v.func.func_obj;\n");
                    fprintf(out, "        __stk[__sp++] = ((Value(*)(Value*, int, void*))__rf->entry)(__args, __lmin_argc, (void*)__rf->captures);\n");
                    fprintf(out, "    }\n");
                    break;
                }
                /* FFI 外部函数调用：直接生成 C 函数调用代码 */
                int ffi_idx = -1;
                for(int fi = 0; fi < ffi_decl_count(); fi++) {
                    if(strcmp(ffi_decl_get(fi)->name, nm) == 0) { ffi_idx = fi; break; }
                }
                if(ffi_idx >= 0) {
                    FFIDecl* ffi = ffi_decl_get(ffi_idx);
                    int argc = in.b;
                    for(int __di = 0; __di < ffi->param_count; __di++) {
                    }
                    fprintf(out, "    {\n");
                    fprintf(out, "        int __lmin_argc = %d;\n", argc);
                    fprintf(out, "        Value __args[%d];\n", argc > 0 ? argc : 1);
                    fprintf(out, "        for (int __k = 0; __k < __lmin_argc; __k++) __args[__k] = __stk[__sp - __lmin_argc + __k];\n");
                    fprintf(out, "        __sp -= __lmin_argc;\n");
                    /* 生成函数调用 */
                    if(ffi->ret_type == 0) {
                        /* void 返回值 */
                        fprintf(out, "        %s(", nm);
                    } else {
                        /* 有返回值：先声明 C 类型变量，再调用，再转换为 Value */
                        const char* ret_c = lumyr_ffi_type_to_cname((FFIType)ffi->ret_type);
                        fprintf(out, "        %s __ffi_ret = %s(", ret_c, nm);
                    }
                    for(int k = 0; k < argc && k < ffi->param_count; k++) {
                        if(k) fprintf(out, ", ");
                        {
                        FFIType ptype = (FFIType)ffi->param_types[k];
                        switch(ptype) {
                            /* 有符号整数：精确 C 类型，零转换开销 */
                            case FFI_INT8:
                                fprintf(out, "(int8_t)value_as_number(__args[%d])", k);
                                break;
                            case FFI_INT16:
                                fprintf(out, "(int16_t)value_as_number(__args[%d])", k);
                                break;
                            case FFI_INT32:
                                fprintf(out, "(int32_t)value_as_number(__args[%d])", k);
                                break;
                            case FFI_INT64:
                                fprintf(out, "(int64_t)value_as_number(__args[%d])", k);
                                break;
                            case FFI_INT:
                                fprintf(out, "(int)value_as_number(__args[%d])", k);
                                break;
                            case FFI_LONG:
                                fprintf(out, "(long)value_as_number(__args[%d])", k);
                                break;
                            case FFI_CHAR:
                                fprintf(out, "(char)value_as_number(__args[%d])", k);
                                break;
                            case FFI_SSIZE_T:
                                fprintf(out, "(ssize_t)value_as_number(__args[%d])", k);
                                break;
                            /* 无符号整数：精确 C 类型 */
                            case FFI_UINT8:
                                fprintf(out, "(uint8_t)value_as_number(__args[%d])", k);
                                break;
                            case FFI_UINT16:
                                fprintf(out, "(uint16_t)value_as_number(__args[%d])", k);
                                break;
                            case FFI_UINT32:
                                fprintf(out, "(uint32_t)value_as_number(__args[%d])", k);
                                break;
                            case FFI_UINT64:
                                fprintf(out, "(uint64_t)value_as_number(__args[%d])", k);
                                break;
                            case FFI_ULONG:
                                fprintf(out, "(unsigned long)value_as_number(__args[%d])", k);
                                break;
                            case FFI_UCHAR:
                                fprintf(out, "(unsigned char)value_as_number(__args[%d])", k);
                                break;
                            case FFI_SIZE_T:
                                fprintf(out, "(size_t)value_as_number(__args[%d])", k);
                                break;
                            /* 浮点：精确 C 类型 */
                            case FFI_FLOAT:
                                fprintf(out, "(float)value_as_number(__args[%d])", k);
                                break;
                            case FFI_DOUBLE:
                                fprintf(out, "(double)value_as_number(__args[%d])", k);
                                break;
                            /* 布尔 */
                            case FFI_BOOL:
                                fprintf(out, "lumyr_to_bool(__args[%d])", k);
                                break;
                            /* 字符串：零拷贝转换为 C 字符串 */
                            case FFI_STRING:
                                fprintf(out, "lumyr_str_cstr(&__args[%d])", k);
                                break;
                            /* 指针/句柄：直接传递指针值 */
                            case FFI_PTR:
                                fprintf(out, "(void*)(intptr_t)value_as_number(__args[%d])", k);
                                break;
                            /* 回调函数：注册 Lumyr 函数，传递槽位 ID 作为函数指针 */
                            case FFI_CALLBACK:
                                fprintf(out, "(void*)(intptr_t)lumyr_ffi_register_callback(__args[%d], 4)", k);
                                break;
                            default:
                                fprintf(out, "(int64_t)value_as_number(__args[%d])", k);
                                break;
                        }
                        }
                    }
                    fprintf(out, ");\n");
                    /* 返回值转换为 Value */
                    if(ffi->ret_type != 0) {
                        {
                        FFIType rtype = (FFIType)ffi->ret_type;
                        switch(rtype) {
                            case FFI_INT:
                            case FFI_PTR:
                                /* 指针/句柄返回：用 int 存储指针值 */
                                fprintf(out, "        __stk[__sp++] = val_int((int64_t)(intptr_t)__ffi_ret);\n");
                                break;
                            case FFI_DOUBLE:
                                fprintf(out, "        __stk[__sp++] = val_double(__ffi_ret);\n");
                                break;
                            case FFI_BOOL:
                                fprintf(out, "        __stk[__sp++] = val_bool(__ffi_ret);\n");
                                break;
                            case FFI_STRING:
                                fprintf(out, "        __stk[__sp++] = lumyr_make_string(__ffi_ret);\n");
                                break;
                            default:
                                fprintf(out, "        __stk[__sp++] = val_int((int64_t)__ffi_ret);\n");
                                break;
                        }
                        }
                    }
                    fprintf(out, "    }\n");
                    break;
                }
                BytecodeFunc* callee = ir_func_table_lookup(nm);
                if(!callee) {
                    fprintf(stderr, "codegen: 未定义函数: %s\n", nm);
                    exit(EXIT_FAILURE);
                }
                /* 检查函数名是否是某个 class 的方法 */
                ClassMethodCheckCtx check_ctx = { nm, 0 };
                type_foreach(check_class_method_cb, &check_ctx);
                int is_class_method_call = check_ctx.found;
                /* 生成器函数调用：创建状态机实例，包装成 VAL_GENERATOR */
                if(callee->is_generator) {
                    int gargc = in.b;
                    int gfixed = callee->param_cnt;
                    int gnbind = (gargc < gfixed) ? gargc : gfixed;
                    fprintf(out, "    {\n");
                    fprintf(out, "        int __lmin_argc = %d;\n", gargc);
                    fprintf(out, "        Value __args[%d];\n", gargc > 0 ? gargc : 1);
                    fprintf(out, "        for (int __k = 0; __k < __lmin_argc; __k++) __args[__k] = __stk[__sp - __lmin_argc + __k];\n");
                    fprintf(out, "        __sp -= __lmin_argc;\n");
                    fprintf(out, "        lumyr_gen_%s* __gen = lumyr_gen_%s_create(", nm, nm);
                    for(int k = 0; k < gfixed; k++) {
                        if(k) fprintf(out, ", ");
                        if(k < gnbind) fprintf(out, "__args[%d]", k);
                        else fprintf(out, "val_none()");
                    }
                    fprintf(out, ");\n");
                    fprintf(out, "        Value __gv; __gv.type = VAL_GENERATOR; __gv.v.generator = (void*)__gen;\n");
                    fprintf(out, "        __stk[__sp++] = __gv;\n");
                    fprintf(out, "    }\n");
                    break;
                }
                int argc = in.b;
                int fixed = callee->param_cnt;
                int nbind = (argc < fixed) ? argc : fixed;
                int restn = argc - fixed;
                if(restn < 0) restn = 0;
                fprintf(out, "    {\n");
                fprintf(out, "        int __lmin_argc = %d;\n", argc);
                fprintf(out, "        Value __args[%d];\n", argc > 0 ? argc : 1);
                fprintf(out, "        for (int __k = 0; __k < __lmin_argc; __k++) __args[__k] = __stk[__sp - __lmin_argc + __k];\n");
                fprintf(out, "        __sp -= __lmin_argc;\n");
                /* type 类型参数值传递：运行时检查并浅拷贝（方法调用的 self 除外，self 引用传递） */
                {
                    int _self_skip = (callee->is_method) ? 1 : 0;
                    for(int _tk = _self_skip; _tk < nbind; _tk++) {
                        /* ref 参数是引用传递，不做浅拷贝 */
                        int _is_ref = (callee->param_is_ref && _tk < callee->param_cnt) ? callee->param_is_ref[_tk] : 0;
                        if(!_is_ref) {
                            fprintf(out, "        if(__args[%d].type == VAL_MAP && lumyr_map_has(__args[%d], lumyr_make_string(\"__mapname__\"))) {\n", _tk, _tk);
                            fprintf(out, "            __args[%d] = lumyr_map_shallow_copy(__args[%d]);\n", _tk, _tk);
                            fprintf(out, "        } else if(__args[%d].type == VAL_STRUCT_PTR && !lumyr_is_class_instance(__args[%d])) {\n", _tk, _tk);
                            fprintf(out, "            __args[%d] = lumyr_struct_shallow_copy(__args[%d]);\n", _tk, _tk);
                            fprintf(out, "        }\n");
                        }
                    }
                }
                if(callee->has_variadic) {
                    fprintf(out, "        Value __rest = val_array(%d);\n", restn);
                    for(int k = 0; k < restn; k++) {
                        fprintf(out, "        gc_write_barrier(__args[%d]);\n", fixed + k);
                        fprintf(out, "        __rest.v.array->items[%d] = __args[%d];\n", k, fixed + k);
                    }
                    /* class 方法使用 classname_methodname_paramcount 的命名方式 */
                    const char* call_func_name_var = nm;
                    static char call_class_func_name_var[256];
                    if(callee->class_name) {
                        snprintf(call_class_func_name_var, sizeof(call_class_func_name_var), "%s_%s_%d", callee->class_name, nm, callee->param_cnt);
                        call_func_name_var = call_class_func_name_var;
                    }
                    fprintf(out, "        __stk[__sp++] = lumyr_func_%s(", call_func_name_var);
                    for(int k = 0; k < fixed; k++) {
                        if(k) fprintf(out, ", ");
                        if(k < nbind) fprintf(out, "__args[%d]", k);
                        else fprintf(out, "val_none()");
                    }
                    if(fixed > 0) fprintf(out, ", ");
                    fprintf(out, "__rest);\n");
                } else if(is_class_method_call) {
                    /* class 方法调用：
                       - 静态分派优化：如果方法没有被重写，直接调用对应类的方法（连 vtable 都不需要）
                       - 动态分派：VAL_STRUCT_PTR 通过 vtable 函数指针调用（O(1)），VAL_MAP 通过 if-else 链 */
                    /* 检查方法是否是非虚方法（没有被任何子类重写） */
                    void* _nonvirtual_class = rbtree_find(g_nonvirtual_methods, NULL, nm);
                    if(_nonvirtual_class) {
                        /* 静态分派：方法未被重写，直接调用对应类的方法 */
                        const char* _nv_class_name = (const char*)_nonvirtual_class;
                        fprintf(out, "        /* 静态分派：方法未被重写，直接调用 */\n");
                        fprintf(out, "        __stk[__sp++] = lumyr_func_%s_%s_%d(", _nv_class_name, nm, fixed);
                        for(int k = 0; k < fixed; k++) {
                            if(k) fprintf(out, ", ");
                            if(k < nbind) fprintf(out, "__args[%d]", k);
                            else fprintf(out, "val_none()");
                        }
                        fprintf(out, ");\n");
                    } else {
                        /* 动态分派 */
                        fprintf(out, "        /* class 方法动态分派 */\n");
                        fprintf(out, "        Value __self = __args[0];\n");
                    fprintf(out, "        if(__self.type == VAL_STRUCT_PTR && __self.v.struct_ptr) {\n");
                    fprintf(out, "            /* C 结构体实例：通过 vtable 函数指针直接调用（O(1)） */\n");
                    /* 编译期查找方法名对应的 vtable 索引 */
                    int method_idx = find_method_index(nm);
                    if(method_idx >= 0) {
                        fprintf(out, "            lumyr_vtable* __vt = (lumyr_vtable*)*(void**)__self.v.struct_ptr;\n");
                        fprintf(out, "            if(__vt && __vt->methods[%d]) {\n", method_idx);
                        /* 生成函数指针类型转换和调用 */
                        fprintf(out, "                __stk[__sp++] = ((Value(*)(Value");
                        for(int k = 1; k < fixed; k++) {
                            fprintf(out, ", Value");
                        }
                        fprintf(out, "))__vt->methods[%d])(__self", method_idx);
                        for(int k = 1; k < fixed; k++) {
                            if(k < nbind) {
                                fprintf(out, ", __args[%d]", k);
                            } else {
                                fprintf(out, ", val_none()");
                            }
                        }
                        fprintf(out, ");\n");
                        fprintf(out, "            } else {\n");
                        fprintf(out, "                runtime_error(\"未找到方法: %s\");\n", nm);
                        fprintf(out, "            }\n");
                    } else {
                        fprintf(out, "            runtime_error(\"未找到方法: %s\");\n", nm);
                    }
                    fprintf(out, "        } else if(__self.type == VAL_MAP && lumyr_map_has(__self, lumyr_make_string(\"__classname__\"))) {\n");
                    fprintf(out, "            /* 旧的 map 实例：通过 if-else 链 + strcmp 实现（向后兼容） */\n");
                    fprintf(out, "            Value __cn = lumyr_map_get(__self, lumyr_make_string(\"__classname__\"));\n");
                    fprintf(out, "            const char* __cn_str = (__cn.type == VAL_STRING) ? lumyr_str_cstr(&__cn) : NULL;\n");
                    fprintf(out, "            if(__cn_str) {\n");
                    /* 遍历所有的 class，生成 if-else 链（支持继承链查找） */
                    int first_class = 1;
                    ClassDispatchCtx dispatch_ctx = { out, nm, fixed, nbind, &first_class };
                    type_foreach(emit_class_dispatch_cb, &dispatch_ctx);
                    /* 如果没有匹配的 class，报错 */
                    fprintf(out, "                else {\n");
                    fprintf(out, "                    runtime_error(\"未找到方法: %s\");\n", nm);
                    fprintf(out, "                }\n");
                    fprintf(out, "            } else {\n");
                    fprintf(out, "                runtime_error(\"self 不是 class 实例\");\n");
                    fprintf(out, "            }\n");
                    fprintf(out, "        } else {\n");
                    fprintf(out, "            runtime_error(\"self 不是 class 实例\");\n");
                    fprintf(out, "        }\n");
                    }  /* end of dynamic dispatch */
                } else {
                    /* 普通函数调用 */
                    fprintf(out, "        __stk[__sp++] = lumyr_func_%s(", nm);
                    for(int k = 0; k < fixed; k++) {
                        if(k) fprintf(out, ", ");
                        if(k < nbind) {
                            /* ref 参数：传递指针（引用传递） */
                            if(callee->param_is_ref && callee->param_is_ref[k])
                                fprintf(out, "&__args[%d]", k);
                            else
                                fprintf(out, "__args[%d]", k);
                        }
                        else fprintf(out, "val_none()");
                    }
                    fprintf(out, ");\n");
                    /* ref 参数：函数返回后把修改写回栈上原来的位置 */
                    for(int k = 0; k < fixed && k < nbind; k++) {
                        if(callee->param_is_ref && callee->param_is_ref[k]) {
                            fprintf(out, "        __stk[__sp - 1 - %d] = __args[%d];\n", nbind - k, k);
                        }
                    }
                }
                fprintf(out, "    }\n");
                break;
            }
            case OPC_CALLV: {
                /* STW 安全点：函数调用前检查 GC */
                fprintf(out, "    gc_stw_check_fast();\n");
                // 动态调用链 f(1)(2)：栈上函数值调用（wrap 指针签名 Value(*)(Value*, int, void*)）
                int argc = in.b;
                fprintf(out, "    {\n");
                fprintf(out, "        Value __f = __stk[__sp - %d - 1];\n", argc);
                fprintf(out, "        if(__f.type != VAL_FUNC) runtime_error(\"尝试调用非函数值\");\n");
                fprintf(out, "        int __lmin_argc = %d;\n", argc);
                fprintf(out, "        Value __args[%d];\n", argc > 0 ? argc : 1);
                fprintf(out, "        for (int __k = 0; __k < __lmin_argc; __k++) __args[__k] = __stk[__sp - %d + __k];\n", argc);
                fprintf(out, "        __sp -= %d + 1;\n", argc);
                fprintf(out, "        RuntimeFunc* __rf = (RuntimeFunc*)__f.v.func.func_obj;\n");
                fprintf(out, "        __stk[__sp++] = ((Value(*)(Value*, int, void*))__rf->entry)(__args, __lmin_argc, (void*)__rf->captures);\n");
                fprintf(out, "    }\n");
                break;
            }
            case OPC_YIELD:
                if(g_is_generator) {
                    g_gen_yield_count++;
                    emit_gen_yield(g_cur_fn, g_gen_yield_count);
                } else {
                    fprintf(out, "    /* YIELD in non-generator function: ignored */\n");
                }
                break;
            case OPC_RETURN:
                if(g_cur_fn) {
                    fprintf(out, "    __g_depth = __g_d0; g_err_jmp = __g_gj0; __g_fin_n = __g_fin0;\n");
                    fprintf(out, "    if(g_trace_n > 0) g_trace_n--;\n");
                    if(g_is_generator) {
                        /* 生成器函数：保存返回值到栈顶，然后 goto __gen_end（由 footer 设置 state=-1 并返回） */
                        fprintf(out, "    { Value __v = __stk[--__sp]; gc_pop_cframe(); __stk[__sp++] = __v; goto __gen_end; }\n");
                    } else {
                        fprintf(out, "    { Value __v = __stk[--__sp]; gc_pop_cframe(); gc_protect_push(__v); gc_protect_pop(); return __v; }\n");
                    }
                } else {
                    fprintf(out, "    gc_pop_cframe();\n");
                    fprintf(out, "    return 0;\n");
                }
                break;
            case OPC_RETURN_NIL:
                if(g_cur_fn) {
                    fprintf(out, "    __g_depth = __g_d0; g_err_jmp = __g_gj0; __g_fin_n = __g_fin0;\n");
                    fprintf(out, "    if(g_trace_n > 0) g_trace_n--;\n");
                    fprintf(out, "    gc_pop_cframe();\n");
                    if(g_is_generator) {
                        /* 生成器函数：goto __gen_end（由 footer 设置 state=-1 并返回 null） */
                        fprintf(out, "    goto __gen_end;\n");
                    } else {
                        /* 构造函数：返回 self（第一个参数），而不是 val_none() */
                        const char* ctor_fn_name = g_cur_fn ? g_cur_fn->name : NULL;
                        size_t ctor_flen = ctor_fn_name ? strlen(ctor_fn_name) : 0;
                        if(ctor_flen >= 9 && strcmp(ctor_fn_name + ctor_flen - 9, "___init__") == 0) {
                            fprintf(out, "    return lmloc_self;\n");
                        } else {
                            fprintf(out, "    return val_none();\n");
                        }
                    }
                } else {
                    fprintf(out, "    gc_pop_cframe();\n");
                    fprintf(out, "    return 0;\n");
                }
                break;
            case OPC_HALT:
                fprintf(out, "    gc_pop_cframe();\n");
                fprintf(out, "    return 0;\n");
                break;
            default:
                break;
        }
        i++;
    }
}

// ---------------- 逃逸分析（位掩码流敏感） ----------------
/*
 * 设计目标：识别不逃逸的 OPC_ARRAY_LIT，将 ValueArray 结构体分配在 C 栈上。
 *
 * 保守策略（宁可漏优化不可出错）：
 *   1. 超过 64 个数组字面量 → 全部堆分配（位掩码上限）
 *   2. 数组出现在逃逸上下文 → 堆分配
 *      逃逸上下文：return 值、存全局变量、函数调用(OPC_CALL/CALLV)任意参数、
 *                  INDEX_SET 的容器和元素、THREAD/THREADLOCAL_SET 内置函数参数、
 *                  ARRAY_ADD/INSERT/ARRAY_SET 等存入数组的值参数、
 *                  THROW 抛出值、PEND_RETURN 挂起返回值、FINISH 处栈上所有值
 *   3. 循环体内（后向跳转 [target, src] 范围）的数组 → 堆分配
 *      （防止同一栈槽被多次迭代复用导致引用错误）
 *   4. 控制流合并按位 OR，不动点迭代最多 20 轮
 *   5. 不可达指令（栈深 -1）跳过
 *
 * 位掩码：每个 OPC_ARRAY_LIT 分配一个 bit（0..63），栈槽/变量持有 uint64_t 掩码
 *         表示"可能持有哪些数组"。
 */

/* 判断符号下标对应的变量是否为全局变量（非参数、非函数局部） */
