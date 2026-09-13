/*
 * ir_cgen 主模块：C 代码生成器主入口、工具函数、函数生成
 * 编译期模块：out/g_globals/fn_locals/g_cur_fn/fin_lab_* 均为单次编译状态，
 * 未来并发编译需实例化；运行时多线程由 _Thread_local 执行器状态保证。
 */
#include "ir_cgen_internal.h"



FILE* out;
NameSet g_globals;      // 全局变量（main 指令流引用）
BytecodeFunc* g_main_fn = NULL;  // main 函数（用于全局变量类型查找）
NameSet fn_locals;      // 当前函数局部变量（非参数、非全局）
BytecodeFunc* g_cur_fn; // 当前生成所在函数（NULL=main）

/* FFI 外部函数声明列表（编译通道用） */
static FFIDecl* g_ffi_decls = NULL;
static int g_ffi_count = 0;
static int g_ffi_cap = 0;

void ffi_decl_add(const char* name, const char* libname, int ret_type, int* param_types, int param_count) {
    if(g_ffi_count >= g_ffi_cap) {
        g_ffi_cap = g_ffi_cap ? g_ffi_cap * 2 : 8;
        g_ffi_decls = (FFIDecl*)realloc(g_ffi_decls, (size_t)g_ffi_cap * sizeof(FFIDecl));
    }
    FFIDecl* d = &g_ffi_decls[g_ffi_count++];
    d->name = name ? strdup(name) : NULL;
    d->libname = libname ? strdup(libname) : NULL;
    d->ret_type = ret_type;
    d->param_count = param_count;
    if(param_count > 0 && param_types) {
        d->param_types = (int*)malloc((size_t)param_count * sizeof(int));
        memcpy(d->param_types, param_types, (size_t)param_count * sizeof(int));
    } else {
        d->param_types = NULL;
    }
}

int ffi_decl_count(void) { return g_ffi_count; }
FFIDecl* ffi_decl_get(int idx) { return (idx >= 0 && idx < g_ffi_count) ? &g_ffi_decls[idx] : NULL; }

/* 逃逸分析结果：
 * g_stack_alloc[i]=1：指令 i 处的 OPC_ARRAY_LIT 栈分配（ValueArray 结构体）
 * g_items_stack_alloc[i]=1：items 缓冲区也栈分配（完全免堆）
 * g_map_stack_alloc[i]=1：指令 i 处的 OPC_MAP_LIT 栈分配（ValueMap 结构体）
 * 每次 analyze_escape* 后有效，emit_insns 消费，函数结束后释放。 */
uint8_t* g_stack_alloc = NULL;
int g_stack_alloc_len = 0;
uint8_t* g_items_stack_alloc = NULL;
int g_items_stack_alloc_len = 0;
uint8_t* g_map_stack_alloc = NULL;
int g_map_stack_alloc_len = 0;

/* 标量替换（Scalar Replacement）：
 * g_scalar_var[v]=1：局部变量 v 持有一个被标量替换的数组/map，
 *   其创建被拆解为一组标量局部变量，INDEX_GET/INDEX_SET/len 被替换为标量读写。
 * g_scalar_kind[v]：0=数组，1=map
 * g_scalar_count[v]：元素个数（数组）或键值对个数（map）
 * g_scalar_keys[v][k]：map 第 k 个键的常量表索引（仅 map）
 * 标量变量命名：__sr_v{v}_e{k}（变量 v 的第 k 个元素） */
uint8_t* g_scalar_var = NULL;
uint8_t* g_scalar_kind = NULL;
int* g_scalar_count = NULL;
int** g_scalar_keys = NULL;
int g_scalar_sym_cnt = 0;

/* 闭包装箱分析结果（每次 emit_func_def 重新计算）：
 * g_boxed：当前函数中被内部 lambda 捕获、需要堆装箱（Value*）的局部变量/参数名。
 * g_cur_caps：当前 lambda 函数自身的捕获变量名列表（顺序即 __caps 数组下标）。
 *   仅当 g_cur_fn 是有捕获的 lambda 时非空。 */
NameSet g_boxed;
NameSet g_cur_caps;

/* 判断指令 i 处的 ARRAY_LIT/MAP_LIT 是否被标量替换。
 * 标量替换模式：LIT + STORE_VAR(v) + POP，且 g_scalar_var[v]==1。
 * 被标量替换的字面量不再生成 __arr_stk_N / __items_stk_N / __map_stk_N 栈声明。 */
int is_scalar_replaced_lit(const BytecodeFunc* fn, int i)
{
    if(!g_scalar_var) return 0;
    if(i + 2 >= fn->code_len) return 0;
    if(fn->code[i].op != OPC_ARRAY_LIT && fn->code[i].op != OPC_MAP_LIT) return 0;
    if(fn->code[i+1].op != OPC_STORE_VAR) return 0;
    if(fn->code[i+2].op != OPC_POP) return 0;
    int v = fn->code[i+1].a;
    if(v < 0 || v >= fn->sym_cnt) return 0;
    return g_scalar_var[v];
}

// 将 CastKind 转换为 C 类型名（用于精确类型声明）
static const char* castkind_to_c_type(int ck) {
    switch(ck) {
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
        case CAST_DOUBLE: return "double";
        case CAST_LONG_DOUBLE: return "long double";
        case CAST_BOOL: return "int";
        case CAST_SIZE_T: return "size_t";
        case CAST_SSIZE_T: return "ssize_t";
        default: return NULL;  // 其他类型保持 Value
    }
}

/* 检查变量是否有精确类型标记，返回 CastKind（-1 表示无） */
static int get_var_type_tag(const BytecodeFunc* fn, const char* name) {
    if(!fn || !fn->var_type_tags) return -1;
    for(int i = 0; i < fn->sym_cnt; i++) {
        if(strcmp(fn->syms[i], name) == 0) return fn->var_type_tags[i];
    }
    return -1;
}

/* 检查变量是否是 struct 类型，返回 struct 类型名（NULL 表示不是） */
static const char* get_var_struct_name(const BytecodeFunc* fn, const char* name) {
    if(!fn || !fn->var_struct_names) return NULL;
    for(int i = 0; i < fn->sym_cnt; i++) {
        if(strcmp(fn->syms[i], name) == 0) return fn->var_struct_names[i];
    }
    /* 如果当前函数中找不到，尝试在 main 函数中查找（全局变量） */
    if(g_main_fn && g_main_fn != fn && g_main_fn->var_struct_names) {
        for(int i = 0; i < g_main_fn->sym_cnt; i++) {
            if(strcmp(g_main_fn->syms[i], name) == 0) return g_main_fn->var_struct_names[i];
        }
    }
    return NULL;
}

/* 生成读取精确类型变量并转换为 Value 的代码 */
static const char* gen_precise_load(int type_tag, const char* var_expr) {
    static char buf[512];
    switch(type_tag) {
        case CAST_INT: case CAST_INT32:
            snprintf(buf, sizeof(buf), "lumyr_make_int((long long)(int)%s)", var_expr); break;
        case CAST_LONGLONG: case CAST_INT64: case CAST_LONG:
            snprintf(buf, sizeof(buf), "lumyr_make_int((long long)%s)", var_expr); break;
        case CAST_SHORT: case CAST_INT16:
            snprintf(buf, sizeof(buf), "lumyr_make_int((long long)(short)%s)", var_expr); break;
        case CAST_CHAR: case CAST_INT8:
            snprintf(buf, sizeof(buf), "lumyr_make_int((long long)(char)%s)", var_expr); break;
        case CAST_UCHAR: case CAST_UINT8: case CAST_BYTE:
            snprintf(buf, sizeof(buf), "lumyr_make_int((long long)(unsigned char)%s)", var_expr); break;
        case CAST_USHORT: case CAST_UINT16:
            snprintf(buf, sizeof(buf), "lumyr_make_int((long long)(unsigned short)%s)", var_expr); break;
        case CAST_UINT32:
            snprintf(buf, sizeof(buf), "lumyr_make_int((long long)(unsigned int)%s)", var_expr); break;
        case CAST_ULONG: case CAST_UINT64:
            snprintf(buf, sizeof(buf), "lumyr_make_int((long long)(unsigned long long)%s)", var_expr); break;
        case CAST_FLOAT:
            snprintf(buf, sizeof(buf), "lumyr_make_double((double)(float)%s)", var_expr); break;
        case CAST_DOUBLE: case CAST_LONG_DOUBLE:
            snprintf(buf, sizeof(buf), "lumyr_make_double((double)%s)", var_expr); break;
        case CAST_BOOL:
            snprintf(buf, sizeof(buf), "lumyr_make_int(%s ? 1 : 0)", var_expr); break;
        default:
            snprintf(buf, sizeof(buf), "%s", var_expr); break;
    }
    return buf;
}

/* 生成存储 Value 到精确类型变量的代码（表达式形式，返回转换后的值） */
static const char* gen_precise_store(int type_tag, const char* value_expr) {
    static char buf[512];
    switch(type_tag) {
        case CAST_INT: case CAST_INT32:
            snprintf(buf, sizeof(buf), "(int)((%s).type == VAL_DOUBLE ? (long long)(%s).v.d : (%s).v.i)", value_expr, value_expr, value_expr); break;
        case CAST_LONGLONG: case CAST_INT64: case CAST_LONG:
            snprintf(buf, sizeof(buf), "((%s).type == VAL_DOUBLE ? (long long)(%s).v.d : (%s).v.i)", value_expr, value_expr, value_expr); break;
        case CAST_SHORT: case CAST_INT16:
            snprintf(buf, sizeof(buf), "(short)((%s).type == VAL_DOUBLE ? (long long)(%s).v.d : (%s).v.i)", value_expr, value_expr, value_expr); break;
        case CAST_CHAR: case CAST_INT8:
            snprintf(buf, sizeof(buf), "(char)((%s).type == VAL_DOUBLE ? (long long)(%s).v.d : (%s).v.i)", value_expr, value_expr, value_expr); break;
        case CAST_UCHAR: case CAST_UINT8: case CAST_BYTE:
            snprintf(buf, sizeof(buf), "(unsigned char)((%s).type == VAL_DOUBLE ? (long long)(%s).v.d : (%s).v.i)", value_expr, value_expr, value_expr); break;
        case CAST_USHORT: case CAST_UINT16:
            snprintf(buf, sizeof(buf), "(unsigned short)((%s).type == VAL_DOUBLE ? (long long)(%s).v.d : (%s).v.i)", value_expr, value_expr, value_expr); break;
        case CAST_UINT32:
            snprintf(buf, sizeof(buf), "(unsigned int)((%s).type == VAL_DOUBLE ? (long long)(%s).v.d : (%s).v.i)", value_expr, value_expr, value_expr); break;
        case CAST_ULONG: case CAST_UINT64:
            snprintf(buf, sizeof(buf), "(unsigned long long)((%s).type == VAL_DOUBLE ? (long long)(%s).v.d : (%s).v.i)", value_expr, value_expr, value_expr); break;
        case CAST_FLOAT:
            snprintf(buf, sizeof(buf), "(float)((%s).type == VAL_INT ? (double)(%s).v.i : (%s).v.d)", value_expr, value_expr, value_expr); break;
        case CAST_DOUBLE: case CAST_LONG_DOUBLE:
            snprintf(buf, sizeof(buf), "((%s).type == VAL_INT ? (double)(%s).v.i : (%s).v.d)", value_expr, value_expr, value_expr); break;
        case CAST_BOOL:
            snprintf(buf, sizeof(buf), "((%s).type == VAL_INT ? (%s).v.i != 0 : (%s).type == VAL_DOUBLE ? (%s).v.d != 0 : 0)", value_expr, value_expr, value_expr, value_expr); break;
        default:
            snprintf(buf, sizeof(buf), "%s", value_expr); break;
    }
    return buf;
}

// ---------------- NameSet ----------------

int ns_has(const NameSet* s, const char* name)
{
    for(int i = 0; i < s->count; i++)
        if(strcmp(s->names[i], name) == 0) return 1;
    return 0;
}

void ns_add(NameSet* s, const char* name)
{
    if(!name || ns_has(s, name)) return;
    if(s->count >= s->cap) {
        int newcap = s->cap > 0 ? s->cap * 2 : 64;
        char** nn = (char**)realloc(s->names, (size_t)newcap * sizeof(char*));
        if(!nn) { fprintf(stderr, "codegen: 符号表扩容内存不足\n"); exit(EXIT_FAILURE); }
        s->names = nn;
        s->cap = newcap;
    }
    s->names[s->count++] = (char*)name;
}

int fn_has_param(const BytecodeFunc* fn, const char* name)
{
    int total = fn->param_cnt + (fn->has_variadic ? 1 : 0);
    for(int i = 0; i < total; i++)
        if(fn->params[i] && strcmp(fn->params[i], name) == 0) return 1;
    return 0;
}

// 查找参数名在参数数组中的下标；未找到返回 -1
int fn_param_index(const BytecodeFunc* fn, const char* name)
{
    int total = fn->param_cnt + (fn->has_variadic ? 1 : 0);
    for(int i = 0; i < total; i++)
        if(fn->params[i] && strcmp(fn->params[i], name) == 0) return i;
    return -1;
}

// 查找当前 lambda 捕获变量名在 __caps 数组中的下标；非捕获变量返回 -1
int cap_index_of(const char* name)
{
    for(int i = 0; i < g_cur_caps.count; i++)
        if(strcmp(g_cur_caps.names[i], name) == 0) return i;
    return -1;
}

// 变量名解析（读写表达式，右值/左值均可）：
//   当前 lambda 捕获变量 → *__caps[idx]
//   装箱局部/参数       → *lmloc_<name>
//   普通局部/参数       → lmloc_<name>
//   全局               → lmvar_<name>
const char* cvar_rw(const char* name)
{
    static char buf[512];
    int ci = cap_index_of(name);
    if(ci >= 0) {
        snprintf(buf, sizeof(buf), "*__caps[%d]", ci);
        return buf;
    }
    if(g_cur_fn && (fn_has_param(g_cur_fn, name) || ns_has(&fn_locals, name))) {
        /* ref 参数：通过指针解引用访问 */
        int _ref_idx = fn_param_index(g_cur_fn, name);
        if(_ref_idx >= 0 && g_cur_fn->param_is_ref && g_cur_fn->param_is_ref[_ref_idx]) {
            snprintf(buf, sizeof(buf), g_is_generator ? "*g->lmloc_%s" : "*lmloc_%s", name);
        }
        else if(ns_has(&g_boxed, name))
            snprintf(buf, sizeof(buf), g_is_generator ? "*g->lmloc_%s" : "*lmloc_%s", name);
        else
            snprintf(buf, sizeof(buf), g_is_generator ? "g->lmloc_%s" : "lmloc_%s", name);
    } else {
        snprintf(buf, sizeof(buf), "lmvar_%s", name);
    }
    return buf;
}

// 变量名解析（返回 Value* 指针，用于 PRE/POST_INC/DEC 等取址场景）：
//   捕获变量   → __caps[idx]
//   装箱变量   → lmloc_<name>（本身已是 Value*）
//   普通变量   → &lmloc_<name> / &lmvar_<name>
const char* cvar_ptr(const char* name)
{
    static char buf[512];
    int ci = cap_index_of(name);
    if(ci >= 0) {
        snprintf(buf, sizeof(buf), "__caps[%d]", ci);
        return buf;
    }
    if(g_cur_fn && (fn_has_param(g_cur_fn, name) || ns_has(&fn_locals, name))) {
        if(ns_has(&g_boxed, name))
            snprintf(buf, sizeof(buf), g_is_generator ? "g->lmloc_%s" : "lmloc_%s", name);
        else
            snprintf(buf, sizeof(buf), g_is_generator ? "&g->lmloc_%s" : "&lmloc_%s", name);
    } else {
        snprintf(buf, sizeof(buf), "&lmvar_%s", name);
    }
    return buf;
}

// OPC_MKCLOSURE 用：返回当前函数中某捕获变量名对应的 cell 指针表达式（Value*）。
//   若该变量是当前 lambda 自己的捕获变量 → __caps[idx]（透传外层 cell）
//   否则必须是当前函数已装箱的局部/参数 → lmloc_<name>（已是 Value*）
const char* cell_ptr_expr(const char* name)
{
    static char buf[512];
    int ci = cap_index_of(name);
    if(ci >= 0) {
        snprintf(buf, sizeof(buf), "__caps[%d]", ci);
        return buf;
    }
    snprintf(buf, sizeof(buf), "lmloc_%s", name);
    return buf;
}

// 按函数名查找函数（用于 lum_wrap / RuntimeFunc.entry）
BytecodeFunc* func_table_lookup(const char* name)
{
    return ir_func_table_lookup(name);
}

// 判断函数是否为有捕获的 lambda
int lambda_has_captures(const char* name)
{
    return name && strncmp(name, "_lambda_", 8) == 0 && lambda_capture_count(name) > 0;
}

// 装箱分析：扫描当前函数字节码中的 OPC_MKCLOSURE，收集被内部 lambda 捕获的变量名。
// 与本函数参数/局部取交集 → 这些变量需要堆装箱。
// 同时初始化 g_cur_caps（若当前函数本身是有捕获的 lambda）。
void emit_c_string_lit(FILE* f, const char* s)
{
    fputc('"', f);
    for(const char* p = s; *p; ++p) {
        if(*p == '"') fputs("\\\"", f);
        else if(*p == '\\') fputs("\\\\", f);
        else if(*p == '\n') fputs("\\n", f);
        else if(*p == '\t') fputs("\\t", f);
        else fputc(*p, f);
    }
    fputc('"', f);
}

void emit_c_char_lit(FILE* f, char ch)
{
    fputc('\'', f);
    if(ch == '\'') fputs("\\'", f);
    else if(ch == '\\') fputs("\\\\", f);
    else if(ch == '\n') fputs("\\n", f);
    else if(ch == '\t') fputs("\\t", f);
    else fputc(ch, f);
    fputc('\'', f);
}

void emit_const(FILE* f, const Value* v)
{
    switch(v->type) {
        case VAL_INT:    fprintf(f, "lumyr_make_int(%lld)", v->v.i); break;
        case VAL_DOUBLE: fprintf(f, "lumyr_make_double(%.17g)", v->v.d); break;
        case VAL_BOOL:   fprintf(f, "lumyr_make_bool(%d)", v->v.b ? 1 : 0); break;
        case VAL_CHAR:   fprintf(f, "lumyr_make_char("); emit_c_char_lit(f, v->v.c); fprintf(f, ")"); break;
        case VAL_BYTE:   fprintf(f, "lumyr_make_byte(%d)", (int)(v->v.i & 0xFF)); break;
        case VAL_STRING: fprintf(f, "lumyr_make_string("); emit_c_string_lit(f, lumyr_str_cstr(v)); fprintf(f, ")"); break;
        default:         fprintf(f, "val_none()"); break;
    }
}

// ---------------- 收集 ----------------

// 收集指令流里的变量引用名（LOAD/STORE/INC/DEC）
void scan_var_refs(BytecodeFunc* fn, NameSet* set, int include_load)
{
    for(int i = 0; i < fn->code_len; i++) {
        Instruction in = fn->code[i];
        switch(in.op) {
            case OPC_STORE_VAR:
            case OPC_PRE_INC:
            case OPC_POST_INC:
            case OPC_PRE_DEC:
            case OPC_POST_DEC:
                if(in.a >= 0 && in.a < fn->sym_cnt) ns_add(set, fn->syms[in.a]);
                break;
            case OPC_LOAD_VAR:
                if(include_load && in.a >= 0 && in.a < fn->sym_cnt) ns_add(set, fn->syms[in.a]);
                break;
            default:
                break;
        }
    }
}

void collect_func_locals(BytecodeFunc* fn)
{
    NameSet raw; memset(&raw, 0, sizeof(raw));
    scan_var_refs(fn, &raw, 0);   // STORE/INC/DEC 的名字
    memset(&fn_locals, 0, sizeof(fn_locals));
    for(int i = 0; i < raw.count; i++) {
        const char* n = raw.names[i];
        /* 词法遮蔽（对齐 VM）：函数内写过的名字（非参数）一律为局部变量，
           即使与全局同名也遮蔽 —— 与 C 语言"函数内局部变量遮蔽全局"一致。
           读全局（只读未写）仍走 lmvar_（cvar 回退），见 cvar() 的解析。 */
        if(!fn_has_param(fn, n)) ns_add(&fn_locals, n);
    }
}

// ---------------- 指令翻译 ----------------

/* finally 完成跳转表：FIN_PUSH 的目标 pc → label 编号（生成函数头 static void* 数组） */
/* 记录已经生成过原型的函数名，避免重复定义 */
static char** g_proto_generated = NULL;
static int g_proto_generated_count = 0;

static int is_proto_generated(const char* name) {
    for(int i = 0; i < g_proto_generated_count; i++) {
        if(strcmp(g_proto_generated[i], name) == 0) return 1;
    }
    return 0;
}

static void mark_proto_generated(const char* name) {
    g_proto_generated = realloc(g_proto_generated, (g_proto_generated_count + 1) * sizeof(char*));
    g_proto_generated[g_proto_generated_count++] = strdup(name);
}

void emit_func_proto(BytecodeFunc* fn)
{
    /* 计算最终的函数名（包含 class 前缀和参数个数） */
    const char* final_name = fn->name;
    static char final_class_name[256];
    if(fn->class_name) {
        snprintf(final_class_name, sizeof(final_class_name), "%s_%s_%d", fn->class_name, fn->name, fn->param_cnt);
        final_name = final_class_name;
    }
    /* 如果已经生成过原型，跳过 */
    if(is_proto_generated(final_name)) return;
    mark_proto_generated(final_name);

    /* 生成器函数：生成状态机结构体、创建函数、next() 函数的原型 */
    if(fn->is_generator) {
        fprintf(out, "typedef struct lumyr_gen_%s lumyr_gen_%s;\n", fn->name, fn->name);
        fprintf(out, "static lumyr_gen_%s* lumyr_gen_%s_create(", fn->name, fn->name);
        int total = fn->param_cnt + (fn->has_variadic ? 1 : 0);
        for(int i = 0; i < total; i++) {
            if(i) fprintf(out, ", ");
            fprintf(out, "Value");
        }
        fprintf(out, ");\n");
        fprintf(out, "static Value lumyr_gen_%s_next(void*, Value);\n", fn->name);
        return;
    }

    int has_caps = lambda_has_captures(fn->name);
    /* class 方法使用 classname_methodname_paramcount 的命名方式，避免命名冲突 */
    const char* func_name = fn->name;
    static char class_func_name[256];
    if(fn->class_name) {
        snprintf(class_func_name, sizeof(class_func_name), "%s_%s_%d", fn->class_name, fn->name, fn->param_cnt);
        func_name = class_func_name;
    }
    fprintf(out, "static Value lumyr_func_%s(", func_name);
    if(has_caps) fprintf(out, "Value** __caps");
    int total = fn->param_cnt + (fn->has_variadic ? 1 : 0);
    for(int i = 0; i < total; i++) {
        if(has_caps || i) fprintf(out, ", ");
        /* ref 参数：声明为 Value* 指针（引用传递） */
        if(i < fn->param_cnt && fn->param_is_ref && fn->param_is_ref[i])
            fprintf(out, "Value*");
        else
            fprintf(out, "Value");
    }
    fprintf(out, ");\n");
}

/* 记录已经生成过定义的函数名，避免重复定义 */
static char** g_def_generated = NULL;
static int g_def_generated_count = 0;

static int is_def_generated(const char* name) {
    for(int i = 0; i < g_def_generated_count; i++) {
        if(strcmp(g_def_generated[i], name) == 0) return 1;
    }
    return 0;
}

static void mark_def_generated(const char* name) {
    g_def_generated = realloc(g_def_generated, (g_def_generated_count + 1) * sizeof(char*));
    g_def_generated[g_def_generated_count++] = strdup(name);
}

void emit_func_def(BytecodeFunc* fn)
{
    /* 计算最终的函数名（包含 class 前缀和参数个数） */
    const char* final_name_def = fn->name;
    static char final_class_name_def[256];
    if(fn->class_name) {
        snprintf(final_class_name_def, sizeof(final_class_name_def), "%s_%s_%d", fn->class_name, fn->name, fn->param_cnt);
        final_name_def = final_class_name_def;
    }
    /* 如果已经生成过定义，跳过 */
    if(is_def_generated(final_name_def)) return;
    mark_def_generated(final_name_def);

    collect_func_locals(fn);

    /* 生成器函数：状态机重写（零成本抽象） */
    if(fn->is_generator) {
        g_cur_fn = fn;
        g_is_generator = 1;
        g_gen_yield_count = 0;

        /* 收集所有 try-catch 块的 catch 标签（用于 GenThrow 时直接 goto catch 块） */
        int try_labels[256];
        int try_label_count = 0;
        for(int ti = 0; ti < fn->code_len && try_label_count < 256; ti++) {
            if(fn->code[ti].op == OPC_TRY) {
                try_labels[try_label_count++] = fn->code[ti].a;
            }
        }
        gen_set_try_labels(try_labels, try_label_count);

        /* 1. 生成状态机结构体 */
        emit_gen_struct(fn);

        /* 2. 生成创建函数 */
        emit_gen_create(fn);

        /* 3. 生成 next() 函数 */
        emit_gen_next_header(fn);

        /* 4. 发射指令（局部变量访问自动加 g-> 前缀） */
        emit_insns(fn);

        /* 5. next() 函数结尾 */
        emit_gen_next_footer(fn);

        g_is_generator = 0;
        g_cur_fn = NULL;
        memset(&fn_locals, 0, sizeof(fn_locals));
        return;
    }

    g_cur_fn = fn;  /* 逃逸分析中用于全局/局部判定 */
    analyze_boxing(fn);  /* 闭包装箱分析：g_boxed / g_cur_caps */
    /* 逃逸分析：数组（含 items）+ map */
    int n = fn->code_len;
    g_stack_alloc = (uint8_t*)calloc(n, sizeof(uint8_t));
    g_items_stack_alloc = (uint8_t*)calloc(n, sizeof(uint8_t));
    g_map_stack_alloc = (uint8_t*)calloc(n, sizeof(uint8_t));
    analyze_escape_for(fn, OPC_ARRAY_LIT, g_stack_alloc, g_items_stack_alloc, 1);
    analyze_escape_for(fn, OPC_MAP_LIT, g_map_stack_alloc, NULL, 0);
    analyze_scalar_replacement(fn);
    /* 收集本函数内 FIN_PUSH 的目标（finally/循环结束 label） */
    fin_lab_cnt = 0;
    for(int i = 0; i < fn->code_len; i++) {
        Instruction in = fn->code[i];
        if(in.op == OPC_FIN_PUSH && in.b) fin_lab_idx_of(in.b);
    }
    int has_caps = lambda_has_captures(fn->name);
    /* class 方法使用 classname_methodname_paramcount 的命名方式，避免命名冲突 */
    const char* func_name = fn->name;
    static char class_func_name[256];
    if(fn->class_name) {
        snprintf(class_func_name, sizeof(class_func_name), "%s_%s_%d", fn->class_name, fn->name, fn->param_cnt);
        func_name = class_func_name;
    }
    fprintf(out, "static Value lumyr_func_%s(", func_name);
    if(has_caps) fprintf(out, "Value** __caps");
    int total = fn->param_cnt + (fn->has_variadic ? 1 : 0);
    for(int i = 0; i < total; i++) {
        if(has_caps || i) fprintf(out, ", ");
        /* ref 参数：声明为 Value* 指针（引用传递） */
        if(i < fn->param_cnt && fn->param_is_ref && fn->param_is_ref[i]) {
            fprintf(out, "Value* lmloc_%s", fn->params[i]);
        }
        /* 装箱参数：入参用 _in 后缀，函数入口处再装箱为 lmloc_<name>(Value*) */
        else if(ns_has(&g_boxed, fn->params[i]))
            fprintf(out, "Value lmloc_%s_in", fn->params[i]);
        else
            fprintf(out, "Value lmloc_%s", fn->params[i]);
    }
    int maxd = bc_analyze_stack(fn, NULL, 0);
    fprintf(out, ")\n{\n");
    fprintf(out, "    Value __stk[%d];\n", maxd + 2);
    fprintf(out, "    int __sp = 0;\n");
    if(fin_lab_cnt > 0) {
        fprintf(out, "    static void* __g_fin_labs[%d] = { ", fin_lab_cnt);
        for(int k = 0; k < fin_lab_cnt; k++)
            fprintf(out, "&&L%d%s", fin_lab_pcs[k], (k + 1 < fin_lab_cnt) ? ", " : "");
        fprintf(out, " };\n");
    }
    fprintf(out, "    int __g_d0 = __g_depth; jmp_buf* __g_gj0 = g_err_jmp; int __g_fin0 = __g_fin_n;\n");
    fprintf(out, "    g_trace_push(\"%s\");\n", fn->name);
    /* 装箱参数：把传入的 by-value Value 拷到堆 cell，后续一律通过 lmloc_<name>(Value*) 访问 */
    for(int i = 0; i < total; i++) {
        if(ns_has(&g_boxed, fn->params[i])) {
            fprintf(out, "    Value* lmloc_%s = (Value*)malloc(sizeof(Value)); *lmloc_%s = lmloc_%s_in;\n",
                    fn->params[i], fn->params[i], fn->params[i]);
        }
    }
    for(int i = 0; i < fn_locals.count; i++) {
        if(ns_has(&g_boxed, fn_locals.names[i])) {
            fprintf(out, "    Value* lmloc_%s = (Value*)malloc(sizeof(Value)); *lmloc_%s = val_none();\n",
                    fn_locals.names[i], fn_locals.names[i]);
        } else {
            const char* sname = get_var_struct_name(fn, fn_locals.names[i]);
            if(sname) {
                /* struct 类型局部变量：VAL_STRUCT_PTR，零拷贝传递，C结构体在栈上 */
                fprintf(out, "    lumyr_struct_%s lmloc_%s__s;\n", sname, fn_locals.names[i]);
                fprintf(out, "    Value lmloc_%s = lumyr_make_struct_ptr(&lmloc_%s__s);\n", fn_locals.names[i], fn_locals.names[i]);
            } else {
                int tt = get_var_type_tag(fn, fn_locals.names[i]);
                const char* ctype = (tt >= 0) ? castkind_to_c_type(tt) : NULL;
                if(ctype) {
                    fprintf(out, "    %s lmloc_%s = 0;\n", ctype, fn_locals.names[i]);
                } else {
                    fprintf(out, "    Value lmloc_%s = val_none();\n", fn_locals.names[i]);
                }
            }
        }
    }
    /* 栈分配数组声明：逃逸分析判定为不逃逸的 OPC_ARRAY_LIT（标量替换的跳过） */
    for(int i = 0; i < fn->code_len; i++) {
        if(g_stack_alloc && g_stack_alloc[i] && !is_scalar_replaced_lit(fn, i)) {
            fprintf(out, "    ValueArray __arr_stk_%d;\n", i);
        }
    }
    /* items 栈缓冲区声明：完全栈分配数组的 items 在 C 栈上（标量替换的跳过） */
    for(int i = 0; i < fn->code_len; i++) {
        if(g_items_stack_alloc && g_items_stack_alloc[i] && !is_scalar_replaced_lit(fn, i)) {
            int ne = fn->code[i].b;
            fprintf(out, "    Value __items_stk_%d[%d];\n", i, ne);
        }
    }
    /* map 栈分配声明：不逃逸的 OPC_MAP_LIT（标量替换的跳过） */
    for(int i = 0; i < fn->code_len; i++) {
        if(g_map_stack_alloc && g_map_stack_alloc[i] && !is_scalar_replaced_lit(fn, i)) {
            fprintf(out, "    ValueMap __map_stk_%d;\n", i);
        }
    }
    /* 标量替换变量声明：每个被标量替换的变量的每个元素一个标量 */
    for(int v = 0; v < fn->sym_cnt; v++) {
        if(g_scalar_var && g_scalar_var[v]) {
            for(int k = 0; k < g_scalar_count[v]; k++) {
                fprintf(out, "    Value __sr_v%d_e%d = val_none();\n", v, k);
            }
        }
    }
    /* GC 根注册：编译通道 CFrame 帧链 push
     * local_ptrs = 参数 + 函数局部变量 + 标量替换变量（均为 C 栈上 Value，取地址） */
    {
        int _total_params = fn->param_cnt + (fn->has_variadic ? 1 : 0);
        int _sr_total = 0;
        for(int v = 0; v < fn->sym_cnt; v++)
            if(g_scalar_var && g_scalar_var[v]) _sr_total += g_scalar_count[v];
        /* 统计需要 GC 扫描的局部变量数（精确类型的变量如 int/double 不需要 GC 扫描） */
        int _gc_locals = 0;
        for(int i = 0; i < fn_locals.count; i++) {
            if(ns_has(&g_boxed, fn_locals.names[i])) { _gc_locals++; continue; }
            if(get_var_struct_name(fn, fn_locals.names[i])) continue;  /* 跳过 struct 类型变量 */
            int tt = get_var_type_tag(fn, fn_locals.names[i]);
            if(tt < 0 || !castkind_to_c_type(tt)) _gc_locals++;
        }
        int _nlocals = _total_params + _gc_locals + _sr_total;
        int _arr_size = _nlocals > 0 ? _nlocals : 1;
        fprintf(out, "    volatile Value* __local_ptrs[%d] = { ", _arr_size);
        int _idx = 0;
        for(int i = 0; i < _total_params; i++) {
            if(_idx) fprintf(out, ", ");
            /* ref 参数：lmloc_<name> 已是 Value*（引用传递）；
               装箱参数：lmloc_<name> 已是 Value*（堆 cell）；
               普通参数取栈地址 */
            if((i < fn->param_cnt && fn->param_is_ref && fn->param_is_ref[i]) ||
               ns_has(&g_boxed, fn->params[i]))
                fprintf(out, "lmloc_%s", fn->params[i]);
            else
                fprintf(out, "&lmloc_%s", fn->params[i]);
            _idx++;
        }
        for(int i = 0; i < fn_locals.count; i++) {
            if(ns_has(&g_boxed, fn_locals.names[i])) {
                if(_idx) fprintf(out, ", ");
                fprintf(out, "lmloc_%s", fn_locals.names[i]);
                _idx++;
                continue;
            }
            if(get_var_struct_name(fn, fn_locals.names[i])) continue;  /* 跳过 struct 类型变量 */
            int tt = get_var_type_tag(fn, fn_locals.names[i]);
            if(tt >= 0 && castkind_to_c_type(tt)) continue;  /* 跳过精确类型变量 */
            if(_idx) fprintf(out, ", ");
            fprintf(out, "&lmloc_%s", fn_locals.names[i]);
            _idx++;
        }
        for(int v = 0; v < fn->sym_cnt; v++) {
            if(g_scalar_var && g_scalar_var[v]) {
                for(int k = 0; k < g_scalar_count[v]; k++) {
                    if(_idx) fprintf(out, ", ");
                    fprintf(out, "&__sr_v%d_e%d", v, k);
                    _idx++;
                }
            }
        }
        if(_nlocals == 0) fprintf(out, "NULL");
        fprintf(out, " };\n");
        fprintf(out, "    CFrame __frame;\n");
        fprintf(out, "    __frame.stack = __stk;\n");
        fprintf(out, "    __frame.sp = &__sp;\n");
        fprintf(out, "    __frame.stack_size = %d;\n", maxd + 2);
        fprintf(out, "    __frame.local_ptrs = (Value**)__local_ptrs;\n");
        fprintf(out, "    __frame.nlocals = %d;\n", _nlocals);
        fprintf(out, "    gc_push_cframe(&__frame);\n");
        fprintf(out, "    gc_stw_check_fast();\n");
    }
    emit_insns(fn);
    g_cur_fn = NULL;
    memset(&g_boxed, 0, sizeof(g_boxed));
    memset(&g_cur_caps, 0, sizeof(g_cur_caps));
    if(g_stack_alloc) { free(g_stack_alloc); g_stack_alloc = NULL; }
    g_stack_alloc_len = 0;
    if(g_items_stack_alloc) { free(g_items_stack_alloc); g_items_stack_alloc = NULL; }
    g_items_stack_alloc_len = 0;
    if(g_map_stack_alloc) { free(g_map_stack_alloc); g_map_stack_alloc = NULL; }
    g_map_stack_alloc_len = 0;
    if(g_scalar_var) { free(g_scalar_var); g_scalar_var = NULL; }
    if(g_scalar_kind) { free(g_scalar_kind); g_scalar_kind = NULL; }
    if(g_scalar_count) { free(g_scalar_count); g_scalar_count = NULL; }
    if(g_scalar_keys) {
        for(int v = 0; v < g_scalar_sym_cnt; v++)
            if(g_scalar_keys[v]) free(g_scalar_keys[v]);
        free(g_scalar_keys);
        g_scalar_keys = NULL;
    }
    g_scalar_sym_cnt = 0;
    fprintf(out, "}\n\n");
}

/* emit_func_wraps 的回调函数：用红黑树遍历生成 lum_wrap_<函数名> */
/* 用于记录已经生成过 wrap 函数的函数名，避免重复定义 */
#define MAX_WRAP_NAMES 1024
static char* wrap_names[MAX_WRAP_NAMES];
static int wrap_name_count = 0;

static int wrap_name_exists(const char* name)
{
    for(int i = 0; i < wrap_name_count; i++) {
        if(strcmp(wrap_names[i], name) == 0) return 1;
    }
    return 0;
}

static void wrap_name_add(const char* name)
{
    if(wrap_name_count < MAX_WRAP_NAMES) {
        wrap_names[wrap_name_count++] = strdup(name);
    }
}

static void emit_func_wrap_cb(const char* class_name, const char* method_name, void* data, void* user_data)
{
    (void)class_name;
    (void)method_name;
    BytecodeFunc* fn = (BytecodeFunc*)data;
    /* 避免同名函数重复定义 */
    if(wrap_name_exists(fn->name)) return;
    wrap_name_add(fn->name);
    FILE* out = (FILE*)user_data;
    int has_caps = lambda_has_captures(fn->name);
    fprintf(out, "static Value lum_wrap_%s(Value* a, int n, void* __ctx)\n{\n", fn->name);
    for(int k = 0; k < fn->param_cnt; k++)
        fprintf(out, "    Value p%d = (n > %d) ? a[%d] : val_none();\n", k, k, k);
    if(fn->has_variadic) {
        fprintf(out, "    Value __rest = val_array(n > %d ? n - %d : 0);\n", fn->param_cnt, fn->param_cnt);
        fprintf(out, "    for(int __k = 0; __k < __rest.v.array->len; __k++) { gc_write_barrier(a[%d + __k]); __rest.v.array->items[__k] = a[%d + __k]; }\n", fn->param_cnt, fn->param_cnt);
    }
    if(fn->is_generator) {
        fprintf(out, "    lumyr_gen_%s* __gen = lumyr_gen_%s_create(", fn->name, fn->name);
        for(int k = 0; k < fn->param_cnt; k++) {
            if(k) fprintf(out, ", ");
            fprintf(out, "p%d", k);
        }
        fprintf(out, ");\n");
        fprintf(out, "    Value __gv; __gv.type = VAL_GENERATOR; __gv.v.generator = (void*)__gen;\n");
        fprintf(out, "    return __gv;\n}\n\n");
    } else {
        fprintf(out, "    Value __wrap_ret = lumyr_func_%s(", fn->name);
        int total = fn->param_cnt + (fn->has_variadic ? 1 : 0);
        if(has_caps)
            fprintf(out, "(Value**)__ctx");
        for(int k = 0; k < total; k++) {
            if(has_caps || k) fprintf(out, ", ");
            if(k < fn->param_cnt) {
                if(fn->param_is_ref && fn->param_is_ref[k])
                    fprintf(out, "&p%d", k);
                else
                    fprintf(out, "p%d", k);
            }
            else {
                fprintf(out, "__rest");
            }
        }
        fprintf(out, ");\n");
        for(int k = 0; k < fn->param_cnt; k++) {
            if(fn->param_is_ref && fn->param_is_ref[k]) {
                fprintf(out, "    if(n > %d) a[%d] = p%d;\n", k, k, k);
            }
        }
        fprintf(out, "    return __wrap_ret;\n}\n\n");
    }
    fprintf(out, "static RuntimeFunc lum_wrap_%s_rf = { (FuncEntry*)lum_wrap_%s, %d, %d, NULL, 0 };\n\n",
            fn->name, fn->name, fn->param_cnt, fn->has_variadic ? 1 : 0);
}

/* 回调函数：用于红黑树遍历生成函数原型 */
static void emit_func_proto_cb(const char* class_name, const char* method_name, void* data, void* user_data)
{
    (void)class_name;
    (void)method_name;
    emit_func_proto((BytecodeFunc*)data);
}

/* 回调函数：用于红黑树遍历生成函数定义 */
static void emit_func_def_cb(const char* class_name, const char* method_name, void* data, void* user_data)
{
    (void)class_name;
    (void)method_name;
    emit_func_def((BytecodeFunc*)data);
}

/* 回调函数：用于红黑树遍历生成 lum_wrap_<name> 前置声明 */
static void emit_wrap_proto_cb(const char* class_name, const char* method_name, void* data, void* user_data)
{
    (void)class_name;
    (void)method_name;
    BytecodeFunc* fn = (BytecodeFunc*)data;
    /* 避免同名函数重复声明 */
    if(wrap_name_exists(fn->name)) return;
    FILE* out = (FILE*)user_data;
    fprintf(out, "static Value lum_wrap_%s(Value*, int, void*);\n", fn->name);
}

/* 回调函数：用于红黑树遍历生成 lum_wrap_<name>_rf 前置声明 */
static void emit_wrap_rf_proto_cb(const char* class_name, const char* method_name, void* data, void* user_data)
{
    (void)class_name;
    (void)method_name;
    BytecodeFunc* fn = (BytecodeFunc*)data;
    /* 避免同名函数重复声明 */
    if(wrap_name_exists(fn->name)) return;
    FILE* out = (FILE*)user_data;
    fprintf(out, "static RuntimeFunc lum_wrap_%s_rf;\n", fn->name);
}

// 生成统一签名包装（Value(*)(Value*, int, void*)）与函数表：高阶函数调用入口
void emit_func_wraps(void)
{
    /* 重置函数名集合 */
    for(int i = 0; i < wrap_name_count; i++) {
        free(wrap_names[i]);
    }
    wrap_name_count = 0;
    /* 用红黑树遍历生成所有 lum_wrap_<函数名> */
    ir_func_table_foreach(emit_func_wrap_cb, out);
    /* 注意：lumyr_cfunc_tbl 数组不再需要，因为改用函数名直接引用 */
}

void emit_main(BytecodeFunc* main_fn)
{
    g_main_fn = main_fn;  // 保存 main 函数，用于全局变量类型查找
    // 生成器组合操作（包装生成器）运行时支持
    emit_gen_wrapper_support();

    // 生成 C struct 定义（所有已注册的 struct 类型）
    for(int si = 0; si < type_count(); si++) {
        TypeDef* td = type_get(si);
        if(td && td->is_struct && td->nprops > 0) {
            fprintf(out, "typedef struct {\n");
            for(int fi = 0; fi < td->nprops; fi++) {
                int ck = td->field_cast_kinds ? td->field_cast_kinds[fi] : CAST_LONGLONG;
                const char* ftype;
                if(td->field_struct_names && td->field_struct_names[fi]) {
                    /* 嵌套 struct 字段，使用对应的 C struct 类型 */
                    static char sname[256];
                    snprintf(sname, sizeof(sname), "lumyr_struct_%s", td->field_struct_names[fi]);
                    ftype = sname;
                } else {
                    ftype = castkind_to_c_type(ck);
                    if(!ftype) ftype = "int64_t";
                }
                fprintf(out, "    %s %s;\n", ftype, td->props[fi]);
            }
            fprintf(out, "} lumyr_struct_%s;\n\n", td->name);
        }
    }

    // 生成 C class 结构体定义（所有已注册的 class 类型）
    for(int si = 0; si < type_count(); si++) {
        TypeDef* td = type_get(si);
        if(td && td->is_class && td->nprops > 0) {
            fprintf(out, "typedef struct lumyr_class_%s lumyr_class_%s;\n", td->name, td->name);
            fprintf(out, "struct lumyr_class_%s {\n", td->name);
            /* 如果有父类，父类结构体作为第一个字段，字段名为 super */
            if(td->parent) {
                fprintf(out, "    lumyr_class_%s super;\n", td->parent);
            }
            for(int fi = 0; fi < td->nprops; fi++) {
                int ck = td->field_cast_kinds ? td->field_cast_kinds[fi] : CAST_LONGLONG;
                const char* ftype = castkind_to_c_type(ck);
                if(!ftype) ftype = "int64_t";
                fprintf(out, "    %s %s;\n", ftype, td->props[fi]);
            }
            fprintf(out, "};\n\n");
        }
    }

    // 全局变量：main 指令流里的全部变量引用
    memset(&g_globals, 0, sizeof(g_globals));
    scan_var_refs(main_fn, &g_globals, 1);
    for(int i = 0; i < g_globals.count; i++) {
        const char* sname = get_var_struct_name(main_fn, g_globals.names[i]);
        if(sname) {
            /* struct 类型全局变量：VAL_STRUCT_PTR，零拷贝传递，C结构体在静态存储区 */
            fprintf(out, "static lumyr_struct_%s lmvar_%s__s;\n", sname, g_globals.names[i]);
            fprintf(out, "static Value lmvar_%s;\n", g_globals.names[i]);
        } else {
            int tt = get_var_type_tag(main_fn, g_globals.names[i]);
            const char* ctype = (tt >= 0) ? castkind_to_c_type(tt) : NULL;
            if(ctype) {
                fprintf(out, "static %s lmvar_%s = 0;\n", ctype, g_globals.names[i]);
            } else {
                fprintf(out, "static Value lmvar_%s = {0};\n", g_globals.names[i]);
            }
        }
    }
    fprintf(out, "\n");

    // FFI 外部函数：不生成 extern 声明，直接调用 C 函数（依赖系统头文件或用户自定义头文件中的声明）
    // 常见系统头文件已在文件开头 include，覆盖大部分 C 标准库函数

    // 函数原型（前向引用/递归）
    ir_func_table_foreach(emit_func_proto_cb, out);
    // 高阶包装前置声明（函数体内 GETFUNC 先于 wraps 定义使用）
    ir_func_table_foreach(emit_wrap_proto_cb, out);
    // RuntimeFunc 包装变量前置声明（GETFUNC 引用 &lum_wrap_<name>_rf，定义在 emit_func_wraps）
    ir_func_table_foreach(emit_wrap_rf_proto_cb, out);
    fprintf(out, "\n");

    // 函数定义
    ir_func_table_foreach(emit_func_def_cb, out);

    // 高阶函数统一调用包装 + 函数表（VM 端 VAL_FUNC 指向 RuntimeFunc，C 端指向此包装）
    emit_func_wraps();

    // main
    int maxd = bc_analyze_stack(main_fn, NULL, 0);
    /* main 逃逸分析：g_cur_fn=NULL，所有变量视为全局（存全局即逃逸） */
    g_cur_fn = NULL;
    memset(&fn_locals, 0, sizeof(fn_locals));
    {
        int mn = main_fn->code_len;
        g_stack_alloc = (uint8_t*)calloc(mn, sizeof(uint8_t));
        g_items_stack_alloc = (uint8_t*)calloc(mn, sizeof(uint8_t));
        g_map_stack_alloc = (uint8_t*)calloc(mn, sizeof(uint8_t));
        analyze_escape_for(main_fn, OPC_ARRAY_LIT, g_stack_alloc, g_items_stack_alloc, 1);
        analyze_escape_for(main_fn, OPC_MAP_LIT, g_map_stack_alloc, NULL, 0);
        analyze_scalar_replacement(main_fn);
    }
    fprintf(out, "int main(void){\n");
    fprintf(out, "    Value __stk[%d];\n", maxd + 2);
    fprintf(out, "    int __sp = 0;\n");
    /* 全局 struct 变量初始化：VAL_STRUCT_PTR，零拷贝传递 */
    for(int gi = 0; gi < g_globals.count; gi++) {
        const char* gsname = get_var_struct_name(main_fn, g_globals.names[gi]);
        if(gsname) {
            fprintf(out, "    lmvar_%s = lumyr_make_struct_ptr(&lmvar_%s__s);\n", g_globals.names[gi], g_globals.names[gi]);
        }
    }
    /* 栈分配数组声明 */
    for(int i = 0; i < main_fn->code_len; i++) {
        if(g_stack_alloc && g_stack_alloc[i]) {
            fprintf(out, "    ValueArray __arr_stk_%d;\n", i);
        }
    }
    /* items 栈缓冲区声明 */
    for(int i = 0; i < main_fn->code_len; i++) {
        if(g_items_stack_alloc && g_items_stack_alloc[i]) {
            int ne = main_fn->code[i].b;
            fprintf(out, "    Value __items_stk_%d[%d];\n", i, ne);
        }
    }
    /* map 栈分配声明 */
    for(int i = 0; i < main_fn->code_len; i++) {
        if(g_map_stack_alloc && g_map_stack_alloc[i]) {
            fprintf(out, "    ValueMap __map_stk_%d;\n", i);
        }
    }
    /* 标量替换变量声明（main 中全为全局变量，通常不会触发） */
    for(int v = 0; v < main_fn->sym_cnt; v++) {
        if(g_scalar_var && g_scalar_var[v]) {
            for(int k = 0; k < g_scalar_count[v]; k++) {
                fprintf(out, "    Value __sr_v%d_e%d = val_none();\n", v, k);
            }
        }
    }
    /* 函数边界保存（RETURN/FINISH act=5 恢复用），与 emit_func_def 一致 */
    fprintf(out, "    int __g_d0 = __g_depth; jmp_buf* __g_gj0 = g_err_jmp; int __g_fin0 = __g_fin_n;\n");
    /* main 内 finally 完成动作目标收集（与 emit_func_def 一致，否则 FINISH 引用未定义的 __g_fin_labs） */
    fin_lab_cnt = 0;
    for(int i = 0; i < main_fn->code_len; i++) {
        Instruction in = main_fn->code[i];
        if(in.op == OPC_FIN_PUSH && in.b) fin_lab_idx_of(in.b);
    }
    if(fin_lab_cnt > 0) {
        fprintf(out, "    static void* __g_fin_labs[%d] = { ", fin_lab_cnt);
        for(int k = 0; k < fin_lab_cnt; k++)
            fprintf(out, "&&L%d%s", fin_lab_pcs[k], (k + 1 < fin_lab_cnt) ? ", " : "");
        fprintf(out, " };\n");
    }
    /* GC 根注册：main 的 CFrame push
     * local_ptrs = 全局变量（lmvar_xxx，文件级 static）+ main 内标量替换变量 */
    {
        int _sr_total = 0;
        for(int v = 0; v < main_fn->sym_cnt; v++)
            if(g_scalar_var && g_scalar_var[v]) _sr_total += g_scalar_count[v];
        /* 统计需要 GC 扫描的全局变量数（精确类型的变量如 int/double 不需要 GC 扫描） */
        int _gc_globals = 0;
        for(int i = 0; i < g_globals.count; i++) {
            if(get_var_struct_name(main_fn, g_globals.names[i])) continue;  /* 跳过 struct 类型变量 */
            int tt = get_var_type_tag(main_fn, g_globals.names[i]);
            if(tt < 0 || !castkind_to_c_type(tt)) _gc_globals++;
        }
        int _nlocals = _gc_globals + _sr_total;
        int _arr_size = _nlocals > 0 ? _nlocals : 1;
        fprintf(out, "    volatile Value* __local_ptrs[%d] = { ", _arr_size);
        int _idx = 0;
        for(int i = 0; i < g_globals.count; i++) {
            if(get_var_struct_name(main_fn, g_globals.names[i])) continue;  /* 跳过 struct 类型变量 */
            int tt = get_var_type_tag(main_fn, g_globals.names[i]);
            if(tt >= 0 && castkind_to_c_type(tt)) continue;  /* 跳过精确类型变量 */
            if(_idx) fprintf(out, ", ");
            fprintf(out, "&lmvar_%s", g_globals.names[i]);
            _idx++;
        }
        for(int v = 0; v < main_fn->sym_cnt; v++) {
            if(g_scalar_var && g_scalar_var[v]) {
                for(int k = 0; k < g_scalar_count[v]; k++) {
                    if(_idx) fprintf(out, ", ");
                    fprintf(out, "&__sr_v%d_e%d", v, k);
                    _idx++;
                }
            }
        }
        if(_nlocals == 0) fprintf(out, "NULL");
        fprintf(out, " };\n");
        fprintf(out, "    CFrame __frame;\n");
        fprintf(out, "    __frame.stack = __stk;\n");
        fprintf(out, "    __frame.sp = &__sp;\n");
        fprintf(out, "    __frame.stack_size = %d;\n", maxd + 2);
        fprintf(out, "    __frame.local_ptrs = (Value**)__local_ptrs;\n");
        fprintf(out, "    __frame.nlocals = %d;\n", _nlocals);
        fprintf(out, "    gc_push_cframe(&__frame);\n");
        fprintf(out, "    gc_stw_check_fast();\n");
    }
    g_cur_fn = NULL;
    emit_insns(main_fn);
    if(g_stack_alloc) { free(g_stack_alloc); g_stack_alloc = NULL; }
    g_stack_alloc_len = 0;
    if(g_items_stack_alloc) { free(g_items_stack_alloc); g_items_stack_alloc = NULL; }
    g_items_stack_alloc_len = 0;
    if(g_map_stack_alloc) { free(g_map_stack_alloc); g_map_stack_alloc = NULL; }
    g_map_stack_alloc_len = 0;
    if(g_scalar_var) { free(g_scalar_var); g_scalar_var = NULL; }
    if(g_scalar_kind) { free(g_scalar_kind); g_scalar_kind = NULL; }
    if(g_scalar_count) { free(g_scalar_count); g_scalar_count = NULL; }
    if(g_scalar_keys) {
        for(int v = 0; v < g_scalar_sym_cnt; v++)
            if(g_scalar_keys[v]) free(g_scalar_keys[v]);
        free(g_scalar_keys);
        g_scalar_keys = NULL;
    }
    g_scalar_sym_cnt = 0;
    fprintf(out, "}\n\n");
}

// ---------------- 入口 ----------------

void ir_cgen_file(const char* out_c_path, BytecodeFunc* main_fn)
{
    out = fopen(out_c_path, "w");
    if(!out) {
        perror("open output c file failed");
        return;
    }

    // 大项目架构：生成代码只包含业务逻辑，runtime 通过链接静态库提供
    fprintf(out, "#include <stdio.h>\n");
    fprintf(out, "#include <stdlib.h>\n");
    fprintf(out, "#include <string.h>\n");
    fprintf(out, "#include <ctype.h>\n");
    fprintf(out, "#include <math.h>\n");
    fprintf(out, "#include <stdint.h>\n");
    fprintf(out, "#include <stddef.h>\n");
    fprintf(out, "#include \"lm_runtime.h\"\n");
    fprintf(out, "#include \"lm_map.h\"\n");
    fprintf(out, "#include \"lm_thread.h\"\n");
    fprintf(out, "#include \"lm_lock.h\"\n");
    fprintf(out, "#include \"lm_tls.h\"\n");
    fprintf(out, "#include \"lm_http.h\"\n");
    fprintf(out, "#include \"lm_json.h\"\n");
    fprintf(out, "#include \"lm_charset.h\"\n");
    fprintf(out, "#include \"lm_crypto.h\"\n");
    fprintf(out, "#include \"lm_regex.h\"\n");
    fprintf(out, "#include \"lm_time.h\"\n");
    fprintf(out, "#include \"lm_qs.h\"\n");
    fprintf(out, "#include \"lumyr_value.h\"\n\n");
    /* 生成器相关全局变量：当前生成器实例的 send 值（receive() 返回） */
    fprintf(out, "static Value __g_gen_send_val = {0};\n");
    fprintf(out, "static int __g_gen_in_generator = 0;\n\n");
    /* 编译通道使用自己的闭包实现（capture_count==-2，captures 为 Value** cell 指针数组）。
     * 提供 gc_runtime.c 引用的 lumyr_interp_scan_captures 弱定义桩，避免链接缺失符号。 */
    fprintf(out, "\n__attribute__((weak)) void lumyr_interp_scan_captures(const RuntimeFunc* rf, void (*mark)(Value)) { (void)rf; (void)mark; }\n\n");

    emit_main(main_fn);
    fclose(out);
}
