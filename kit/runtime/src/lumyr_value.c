#include "lumyr_value.h"
#include "lumyr_log.h"
#include "lm_map.h"
#include "gc_runtime.h"
#include "lumyr_typed_arrays.h"
#include <string.h>

/* 错误机制全部动态化，无硬上限：
 *   - g_err_msg/g_err_type：动态缓冲（g_err_msg_set/g_err_type_set 按需扩容）
 *   - g_trace：调用栈回溯（g_trace_push 按需扩容）
 *   - __g_*：C 生成通道的 try/catch 处理器栈（__g_ensure 按需扩容；VM 通道用 vm.c 的 vm_jbs）
 * jmp_buf 经 realloc 移动时内容整体拷贝，setjmp 后再 longjmp(__g_jbs[d]) 语义不变。 */
_Thread_local jmp_buf* g_err_jmp = NULL;
_Thread_local char* g_err_msg = NULL;
static _Thread_local int g_err_msg_cap = 0;
_Thread_local char* g_err_type = NULL;
static _Thread_local int g_err_type_cap = 0;
_Thread_local const char** g_trace = NULL;
_Thread_local int g_trace_n = 0;
static _Thread_local int g_trace_cap = 0;
_Thread_local jmp_buf* __g_jbs = NULL;
_Thread_local jmp_buf** __g_prev = NULL;
_Thread_local int __g_depth = 0;
_Thread_local int* __g_sp0 = NULL;
_Thread_local int* __g_tgt = NULL;
_Thread_local int* __g_tn = NULL;        /* 每层 TRY 时的调用栈深度（GET_ERR 截断残留） */
_Thread_local int* __g_fn = NULL;        /* 每层 TRY 时的 finally 完成栈深度 */
_Thread_local int* __g_fin_act = NULL;
_Thread_local int* __g_fin_dep = NULL;   /* finally 完成动作：1=JMP 2=RETHROW 3=BREAK 4=CONT 5=RETURN */
_Thread_local int* __g_fin_tgt = NULL;
_Thread_local int __g_fin_n = 0;
static _Thread_local int __g_cap = 0;
_Thread_local Value __g_pend_val;    /* 挂起返回的值（PEND_RETURN 存，FINISH act5 恢复） */

/* 错误机制扩容：同时扩 __g_* try 栈与 g_trace（调用栈回溯） */
void __g_ensure(int need)
{
    if(need <= __g_cap) return;
    int nc = __g_cap > 0 ? __g_cap * 2 : 64;
    jmp_buf* nj = (jmp_buf*)realloc(__g_jbs, (size_t)nc * sizeof(jmp_buf));
    if(!nj) { LOG_ERROR("错误处理栈扩容内存不足\n"); exit(EXIT_FAILURE); }
    __g_jbs = nj;
    jmp_buf** np = (jmp_buf**)realloc(__g_prev, (size_t)nc * sizeof(jmp_buf*));
    if(!np) { LOG_ERROR("错误处理栈扩容内存不足\n"); exit(EXIT_FAILURE); }
    __g_prev = np;
    int* na = (int*)realloc(__g_sp0, (size_t)nc * sizeof(int));
    if(!na) { LOG_ERROR("错误处理栈扩容内存不足\n"); exit(EXIT_FAILURE); }
    __g_sp0 = na;
    int* nt = (int*)realloc(__g_tgt, (size_t)nc * sizeof(int));
    if(!nt) { LOG_ERROR("错误处理栈扩容内存不足\n"); exit(EXIT_FAILURE); }
    __g_tgt = nt;
    int* nn = (int*)realloc(__g_tn, (size_t)nc * sizeof(int));
    if(!nn) { LOG_ERROR("错误处理栈扩容内存不足\n"); exit(EXIT_FAILURE); }
    __g_tn = nn;
    int* nf = (int*)realloc(__g_fn, (size_t)nc * sizeof(int));
    if(!nf) { LOG_ERROR("错误处理栈扩容内存不足\n"); exit(EXIT_FAILURE); }
    __g_fn = nf;
    int* nfa = (int*)realloc(__g_fin_act, (size_t)nc * sizeof(int));
    if(!nfa) { LOG_ERROR("错误处理栈扩容内存不足\n"); exit(EXIT_FAILURE); }
    __g_fin_act = nfa;
    int* nfd = (int*)realloc(__g_fin_dep, (size_t)nc * sizeof(int));
    if(!nfd) { LOG_ERROR("错误处理栈扩容内存不足\n"); exit(EXIT_FAILURE); }
    __g_fin_dep = nfd;
    int* nft = (int*)realloc(__g_fin_tgt, (size_t)nc * sizeof(int));
    if(!nft) { LOG_ERROR("错误处理栈扩容内存不足\n"); exit(EXIT_FAILURE); }
    __g_fin_tgt = nft;
    const char** ntr = (const char**)realloc(g_trace, (size_t)nc * sizeof(const char*));
    if(!ntr) { LOG_ERROR("调用栈回溯扩容内存不足\n"); exit(EXIT_FAILURE); }
    g_trace = ntr;
    __g_cap = nc;
    g_trace_cap = nc;
}

/* 调用栈回溯 push（函数入口/调用点） */
void g_trace_push(const char* nm)
{
    __g_ensure(g_trace_n + 1);
    g_trace[g_trace_n++] = nm;
}

/* 错误消息/类型动态缓冲 */
void g_err_msg_set(const char* s)
{
    size_t l = s ? strlen(s) : 0;
    if((int)l + 1 > g_err_msg_cap) {
        int nc = g_err_msg_cap > 0 ? g_err_msg_cap * 2 : 1024;
        while(nc < (int)l + 1) nc *= 2;
        char* nm = (char*)realloc(g_err_msg, (size_t)nc);
        if(!nm) { LOG_ERROR("错误消息缓冲扩容内存不足\n"); exit(EXIT_FAILURE); }
        g_err_msg = nm; g_err_msg_cap = nc;
    }
    memcpy(g_err_msg, s ? s : "", l + 1);
}

void g_err_type_set(const char* s)
{
    size_t l = s ? strlen(s) : 0;
    if((int)l + 1 > g_err_type_cap) {
        int nc = g_err_type_cap > 0 ? g_err_type_cap * 2 : 64;
        while(nc < (int)l + 1) nc *= 2;
        char* nm = (char*)realloc(g_err_type, (size_t)nc);
        if(!nm) { LOG_ERROR("错误类型缓冲扩容内存不足\n"); exit(EXIT_FAILURE); }
        g_err_type = nm; g_err_type_cap = nc;
    }
    memcpy(g_err_type, s ? s : "RuntimeError", l + 1);
}

// 运行时错误：有 try 处理器则恢复（longjmp），否则打印并退出
void runtime_error(const char* msg) {
    if(g_err_jmp) {
        g_err_type_set("RuntimeError");
        g_err_msg_set(msg);
        longjmp(*g_err_jmp, 1);
    }
    LOG_ERROR("Runtime Error: %s\n", msg);
    exit(EXIT_FAILURE);
}

// 错误对象构造：type/message/stack（stack 可为空，内部复制；字符串由 GC 管理）
Value lumyr_make_error(const char* type, const char* msg, const char* stack) {
    Value v;
    v.type = VAL_ERROR;
    const char* t = type ? type : "Error";
    const char* m = msg ? msg : "";
    const char* s = stack ? stack : "";
    size_t tl = strlen(t), ml = strlen(m), sl = strlen(s);
    v.v.err.type = (char*)gc_alloc(tl + 1, VAL_STRING);
    memcpy(v.v.err.type, t, tl + 1);
    v.v.err.message = (char*)gc_alloc(ml + 1, VAL_STRING);
    memcpy(v.v.err.message, m, ml + 1);
    v.v.err.stack = (char*)gc_alloc(sl + 1, VAL_STRING);
    memcpy(v.v.err.stack, s, sl + 1);
    return v;
}

// 当前调用栈回溯文本（malloc，调用方 free）：at func 逐行
char* lumyr_build_stack_trace(void) {
    if(g_trace_n <= 0) { char* e = (char*)malloc(1); e[0] = '\0'; return e; }
    size_t cap = 256;
    for(int i = 0; i < g_trace_n; i++) cap += strlen(g_trace[i]) + 16;
    char* out = (char*)malloc(cap);
    size_t w = 0;
    for(int i = g_trace_n - 1; i >= 0; i--) {
        if(w) out[w++] = '\n';
        const char* nm = g_trace[i] ? g_trace[i] : "<anonymous>";
        w += (size_t)snprintf(out + w, cap - w, "at %s", nm);
    }
    out[w] = '\0';
    return out;
}

// -------- 值构造 --------
Value val_none(void) {
    Value r;
    r.type = VAL_NONE;
    return r;
}

Value val_int(long long v) {
    Value r;
    r.type = VAL_INT;
    r.v.i = v;
    return r;
}

Value val_double(double v) {
    Value r;
    r.type = VAL_DOUBLE;
    r.v.d = v;
    return r;
}

Value val_bool(_Bool v) {
    Value r;
    r.type = VAL_BOOL;
    r.v.b = v;
    return r;
}

Value val_char(char v) {
    Value r;
    r.type = VAL_CHAR;
    r.v.c = v;
    return r;
}

Value val_string(const char* s) {
    Value r;
    r.type = VAL_STRING;
    r.str_inline = 0;
    if (s == NULL) {
        r.v.s = NULL;
        return r;
    }
    size_t len = strlen(s);
    if (len <= LUMYR_SSO_MAX) {
        r.str_inline = 1;
        r.v.sso.len = (uint8_t)len;
        memcpy(r.v.sso.data, s, len);
        r.v.sso.data[len] = '\0';
    } else {
        r.v.s = (char*)gc_alloc(len + 1, VAL_STRING);
        memcpy(r.v.s, s, len);
        r.v.s[len] = '\0';
    }
    return r;
}

// ❗ 删除 val_func(AstNode* func_ast) 整个函数

Value val_array(int len) {
    Value r;
    r.type = VAL_ARRAY;
    /* GC 安全：构造期间暂停自动 GC，避免中间分配触发 sweep */
    gc_disable();
    Value* items = NULL;
    if(len > 0) {
        items = (Value*)gc_alloc_old(sizeof(Value) * len, VAL_ARRAY);  /* 内部缓冲区老年代 */
        gc_mark_internal_buf(items);  /* 标记为内部缓冲区，保守 C 栈扫描跳过 */
        for(int i = 0; i < len; i++) {
            items[i] = val_none();
        }
    }
    r.v.array = (ValueArray*)gc_alloc(sizeof(ValueArray), VAL_ARRAY);
    r.v.array->items = items;
    r.v.array->len = len;
    r.v.array->cap = len > 0 ? len : 0;
    r.v.array->stack_alloc = 0;  /* 堆分配，显式标记 */
    gc_enable();
    return r;
}

/* int 泛型数组：创建 TypedArray，元素类型为 VAL_INT，包装为 VAL_TYPED_ARRAY 类型的 Value */
Value val_int_array(int len) {
    Value r;
    r.type = VAL_TYPED_ARRAY;
    /* GC 安全：构造期间暂停自动 GC，避免中间分配触发 sweep */
    gc_disable();
    TypedArray* arr = (TypedArray*)gc_alloc(sizeof(TypedArray), VAL_TYPED_ARRAY);
    if(arr) {
        arr->elem_type = VAL_INT;
        arr->len = len > 0 ? len : 0;
        arr->cap = len > 0 ? len : 8;
        arr->stack_alloc = 0;
        if(len > 0) {
            arr->items = (int*)gc_alloc_old(sizeof(int) * arr->cap, VAL_TYPED_ARRAY);
            gc_mark_internal_buf(arr->items);
            for(int i = 0; i < len; i++) {
                ((int*)arr->items)[i] = 0;
            }
        } else {
            arr->items = NULL;
        }
    }
    r.v.typed_array = arr;
    gc_enable();
    return r;
}

/* double 泛型数组：创建 TypedArray，元素类型为 VAL_DOUBLE，包装为 VAL_TYPED_ARRAY 类型的 Value */
Value val_double_array(int len) {
    Value r;
    r.type = VAL_TYPED_ARRAY;
    /* GC 安全：构造期间暂停自动 GC，避免中间分配触发 sweep */
    gc_disable();
    TypedArray* arr = (TypedArray*)gc_alloc(sizeof(TypedArray), VAL_TYPED_ARRAY);
    if(arr) {
        arr->elem_type = VAL_DOUBLE;
        arr->len = len > 0 ? len : 0;
        arr->cap = len > 0 ? len : 8;
        arr->stack_alloc = 0;
        if(len > 0) {
            arr->items = (double*)gc_alloc_old(sizeof(double) * arr->cap, VAL_TYPED_ARRAY);
            gc_mark_internal_buf(arr->items);
            for(int i = 0; i < len; i++) {
                ((double*)arr->items)[i] = 0.0;
            }
        } else {
            arr->items = NULL;
        }
    }
    r.v.typed_array = arr;
    gc_enable();
    return r;
}

/* float 泛型数组：创建 TypedArray，元素类型为 VAL_DOUBLE（float 用 VAL_DOUBLE 存储），包装为 VAL_TYPED_ARRAY 类型的 Value */
Value val_float_array(int len) {
    Value r;
    r.type = VAL_TYPED_ARRAY;
    /* GC 安全：构造期间暂停自动 GC，避免中间分配触发 sweep */
    gc_disable();
    TypedArray* arr = (TypedArray*)gc_alloc(sizeof(TypedArray), VAL_TYPED_ARRAY);
    if(arr) {
        arr->elem_type = VAL_FLOAT;  // float 类型化数组，与 double 区分
        arr->len = len > 0 ? len : 0;
        arr->cap = len > 0 ? len : 8;
        arr->stack_alloc = 0;
        if(len > 0) {
            arr->items = (float*)gc_alloc_old(sizeof(float) * arr->cap, VAL_TYPED_ARRAY);
            gc_mark_internal_buf(arr->items);
            for(int i = 0; i < len; i++) {
                ((float*)arr->items)[i] = 0.0f;
            }
        } else {
            arr->items = NULL;
        }
    }
    r.v.typed_array = arr;
    gc_enable();
    return r;
}

/* uint 泛型数组：创建 TypedArray，元素类型为 VAL_UINT32，包装为 VAL_TYPED_ARRAY 类型的 Value */
Value val_uint_array(int len) {
    Value r;
    r.type = VAL_TYPED_ARRAY;
    /* GC 安全：构造期间暂停自动 GC，避免中间分配触发 sweep */
    gc_disable();
    TypedArray* arr = (TypedArray*)gc_alloc(sizeof(TypedArray), VAL_TYPED_ARRAY);
    if(arr) {
        arr->elem_type = VAL_UINT32;  // uint 类型化数组，与 int 区分
        arr->len = len > 0 ? len : 0;
        arr->cap = len > 0 ? len : 8;
        arr->stack_alloc = 0;
        if(len > 0) {
            arr->items = (unsigned int*)gc_alloc_old(sizeof(unsigned int) * arr->cap, VAL_TYPED_ARRAY);
            gc_mark_internal_buf(arr->items);
            for(int i = 0; i < len; i++) {
                ((unsigned int*)arr->items)[i] = 0;
            }
        } else {
            arr->items = NULL;
        }
    }
    r.v.typed_array = arr;
    gc_enable();
    return r;
}

Value val_bool_array(int len) {
    Value r;
    r.type = VAL_TYPED_ARRAY;
    gc_disable();
    TypedArray* arr = (TypedArray*)gc_alloc(sizeof(TypedArray), VAL_TYPED_ARRAY);
    if(arr) {
        arr->elem_type = VAL_BOOL;  // bool 类型化数组，与其他类型彻底隔离
        arr->len = len > 0 ? len : 0;
        arr->cap = len > 0 ? len : 8;
        arr->stack_alloc = 0;
        if(len > 0) {
            arr->items = (_Bool*)gc_alloc_old(sizeof(_Bool) * arr->cap, VAL_TYPED_ARRAY);
            gc_mark_internal_buf(arr->items);
            for(int i = 0; i < len; i++) {
                ((_Bool*)arr->items)[i] = 0;
            }
        } else {
            arr->items = NULL;
        }
    }
    r.v.typed_array = arr;
    gc_enable();
    return r;
}

Value val_char_array(int len) {
    Value r;
    r.type = VAL_TYPED_ARRAY;
    gc_disable();
    TypedArray* arr = (TypedArray*)gc_alloc(sizeof(TypedArray), VAL_TYPED_ARRAY);
    if(arr) {
        arr->elem_type = VAL_CHAR;  // char 类型化数组，与其他类型彻底隔离
        arr->len = len > 0 ? len : 0;
        arr->cap = len > 0 ? len : 8;
        arr->stack_alloc = 0;
        if(len > 0) {
            arr->items = (char*)gc_alloc_old(sizeof(char) * arr->cap, VAL_TYPED_ARRAY);
            gc_mark_internal_buf(arr->items);
            for(int i = 0; i < len; i++) {
                ((char*)arr->items)[i] = 0;
            }
        } else {
            arr->items = NULL;
        }
    }
    r.v.typed_array = arr;
    gc_enable();
    return r;
}

Value val_byte_array(int len) {
    Value r;
    r.type = VAL_TYPED_ARRAY;
    gc_disable();
    TypedArray* arr = (TypedArray*)gc_alloc(sizeof(TypedArray), VAL_TYPED_ARRAY);
    if(arr) {
        arr->elem_type = VAL_BYTE;  // byte 类型化数组，与其他类型彻底隔离
        arr->len = len > 0 ? len : 0;
        arr->cap = len > 0 ? len : 8;
        arr->stack_alloc = 0;
        if(len > 0) {
            arr->items = (unsigned char*)gc_alloc_old(sizeof(unsigned char) * arr->cap, VAL_TYPED_ARRAY);
            gc_mark_internal_buf(arr->items);
            for(int i = 0; i < len; i++) {
                ((unsigned char*)arr->items)[i] = 0;
            }
        } else {
            arr->items = NULL;
        }
    }
    r.v.typed_array = arr;
    gc_enable();
    return r;
}

Value val_int8_array(int len) {
    Value r;
    r.type = VAL_TYPED_ARRAY;
    gc_disable();
    TypedArray* arr = (TypedArray*)gc_alloc(sizeof(TypedArray), VAL_TYPED_ARRAY);
    if(arr) {
        arr->elem_type = VAL_INT8;  // int8 类型化数组，与其他类型彻底隔离
        arr->len = len > 0 ? len : 0;
        arr->cap = len > 0 ? len : 8;
        arr->stack_alloc = 0;
        if(len > 0) {
            arr->items = (int8_t*)gc_alloc_old(sizeof(int8_t) * arr->cap, VAL_TYPED_ARRAY);
            gc_mark_internal_buf(arr->items);
            for(int i = 0; i < len; i++) {
                ((int8_t*)arr->items)[i] = 0;
            }
        } else {
            arr->items = NULL;
        }
    }
    r.v.typed_array = arr;
    gc_enable();
    return r;
}

Value val_int16_array(int len) {
    Value r;
    r.type = VAL_TYPED_ARRAY;
    gc_disable();
    TypedArray* arr = (TypedArray*)gc_alloc(sizeof(TypedArray), VAL_TYPED_ARRAY);
    if(arr) {
        arr->elem_type = VAL_INT16;  // int16 类型化数组，与其他类型彻底隔离
        arr->len = len > 0 ? len : 0;
        arr->cap = len > 0 ? len : 8;
        arr->stack_alloc = 0;
        if(len > 0) {
            arr->items = (int16_t*)gc_alloc_old(sizeof(int16_t) * arr->cap, VAL_TYPED_ARRAY);
            gc_mark_internal_buf(arr->items);
            for(int i = 0; i < len; i++) {
                ((int16_t*)arr->items)[i] = 0;
            }
        } else {
            arr->items = NULL;
        }
    }
    r.v.typed_array = arr;
    gc_enable();
    return r;
}

Value val_int32_array(int len) {
    Value r;
    r.type = VAL_TYPED_ARRAY;
    gc_disable();
    TypedArray* arr = (TypedArray*)gc_alloc(sizeof(TypedArray), VAL_TYPED_ARRAY);
    if(arr) {
        arr->elem_type = VAL_INT32;  // int32 类型化数组，与其他类型彻底隔离
        arr->len = len > 0 ? len : 0;
        arr->cap = len > 0 ? len : 8;
        arr->stack_alloc = 0;
        if(len > 0) {
            arr->items = (int32_t*)gc_alloc_old(sizeof(int32_t) * arr->cap, VAL_TYPED_ARRAY);
            gc_mark_internal_buf(arr->items);
            for(int i = 0; i < len; i++) {
                ((int32_t*)arr->items)[i] = 0;
            }
        } else {
            arr->items = NULL;
        }
    }
    r.v.typed_array = arr;
    gc_enable();
    return r;
}

/* 编译通道栈分配数组：初始化调用方提供的栈上 ValueArray（items 仍走 gc_alloc），
 * 设置 stack_alloc=1，返回 Value。GC 标记时跳过 ValueArray 自身（无 GCObject 头），
 * 但仍标记 items 缓冲区及递归标记 items[i]。VM 路径不使用此函数。 */
Value val_array_from_stack(ValueArray* va, int len) {
    Value r;
    r.type = VAL_ARRAY;
    /* GC 安全：构造期间暂停自动 GC，items 分配期间 va 尚未被任何根引用 */
    gc_disable();
    va->stack_alloc = 1;
    va->items_stack_alloc = 0;
    va->len = len;
    va->cap = len > 0 ? len : 0;
    if(len > 0) {
        va->items = (Value*)gc_alloc_old(sizeof(Value) * len, VAL_ARRAY);  /* 内部缓冲区老年代 */
        gc_mark_internal_buf(va->items);
        for(int i = 0; i < len; i++) va->items[i] = val_none();
    } else {
        va->items = NULL;
    }
    r.v.array = va;
    gc_enable();
    return r;
}

/* 编译通道完全栈分配数组：ValueArray 结构体和 items 缓冲区均在 C 栈上，
 * 完全消除 GC 对象。设置 stack_alloc=1 + items_stack_alloc=1。
 * 调用方需保证 items 缓冲区至少 len 个 Value（建议已 memset 为 0）。
 * GC 标记时跳过 ValueArray 和 items 自身（均无 GCObject 头），
 * 但仍递归标记 items[i]（元素可能是堆对象如字符串/嵌套数组）。
 * VM 路径不使用此函数。 */
Value val_array_from_stack_items(ValueArray* va, Value* items, int len) {
    Value r;
    r.type = VAL_ARRAY;
    va->stack_alloc = 1;
    va->items_stack_alloc = 1;
    va->len = len;
    va->cap = len > 0 ? len : 0;
    va->items = items;
    r.v.array = va;
    return r;
}

Value val_map(void) {
    Value r;
    r.type = VAL_MAP;
    /* GC 安全：构造期间暂停自动 GC */
    gc_disable();
    MapEntry** buckets = (MapEntry**)gc_alloc_old(16 * sizeof(MapEntry*), VAL_MAP);  /* 内部缓冲区老年代 */
    gc_mark_internal_buf(buckets);  /* 标记为内部缓冲区，保守 C 栈扫描跳过（与 array items 一致） */
    memset(buckets, 0, 16 * sizeof(MapEntry*));
    unsigned char* tree = (unsigned char*)gc_alloc_old(16 * sizeof(unsigned char), VAL_MAP);  /* 内部缓冲区老年代 */
    gc_mark_internal_buf(tree);
    memset(tree, 0, 16 * sizeof(unsigned char));
    r.v.map = (ValueMap*)gc_alloc(sizeof(ValueMap), VAL_MAP);
    r.v.map->len = 0;
    r.v.map->cap = 16;
    r.v.map->buckets = buckets;
    r.v.map->tree = tree;
    r.v.map->stack_alloc = 0;
    gc_enable();
    return r;
}

/* 编译通道栈分配 map：ValueMap 结构体在 C 栈上，buckets/entries 仍堆分配。
 * 设置 stack_alloc=1，返回 Value。GC 标记时跳过 ValueMap 自身（无 GCObject 头），
 * 但仍标记 buckets/tree 及递归键值。VM 路径不使用此函数。 */
Value val_map_from_stack(ValueMap* vm) {
    Value r;
    r.type = VAL_MAP;
    gc_disable();
    MapEntry** buckets = (MapEntry**)gc_alloc_old(16 * sizeof(MapEntry*), VAL_MAP);  /* 内部缓冲区老年代 */
    gc_mark_internal_buf(buckets);  /* 标记为内部缓冲区，保守 C 栈扫描跳过 */
    memset(buckets, 0, 16 * sizeof(MapEntry*));
    unsigned char* tree = (unsigned char*)gc_alloc_old(16 * sizeof(unsigned char), VAL_MAP);  /* 内部缓冲区老年代 */
    gc_mark_internal_buf(tree);
    memset(tree, 0, 16 * sizeof(unsigned char));
    vm->len = 0;
    vm->cap = 16;
    vm->buckets = buckets;
    vm->tree = tree;
    vm->stack_alloc = 1;
    r.v.map = vm;
    gc_enable();
    return r;
}

// -------- 销毁（引用语义 + GC：空操作，由 GC 统一回收） --------
void val_destroy(Value* v) {
    if(!v) return;
    v->type = VAL_NONE;  /* 仅标记，不 free */
}

// -------- 浅拷贝（引用语义：直接复制 Value 结构体，不分配新内存） --------
Value val_clone(const Value* src) {
    return *src;
}

// -------- debug打印 --------
const char* val_typename(ValueType t) {
    switch(t) {
    case VAL_NONE: return "none";
    case VAL_INT: return "int";
    case VAL_DOUBLE: return "double";
    case VAL_BOOL: return "bool";
    case VAL_CHAR: return "char";
    case VAL_STRING: return "string";
    case VAL_FUNC: return "func";
    case VAL_ARRAY: return "array";
    case VAL_MAP: return "map";
    case VAL_ERROR: return "error";
    case VAL_BYTE: return "byte";
    case VAL_GENERATOR: return "generator";
    case VAL_STRUCT_PTR: return "struct_ptr";
    case VAL_CLASS_PTR: return "class_ptr";
    case VAL_TYPED_ARRAY: return "typed_array";
    default: return "unknown";
    }
}

void val_print(const Value* v) {
    if(!v) { printf("(null value)"); return; }
    switch(v->type) {
    case VAL_INT: printf("%lld", v->v.i); break;
    case VAL_DOUBLE: printf("%g", v->v.d); break;
    case VAL_BOOL: printf("%s", v->v.b ? "true" : "false"); break;
    case VAL_CHAR: printf("'%c'", v->v.c); break;
    case VAL_STRING: printf("\"%s\"", lumyr_str_cstr(v)); break;
    case VAL_ERROR: printf("[error:%s] %s", v->v.err.type ? v->v.err.type : "", v->v.err.message ? v->v.err.message : ""); break;
    case VAL_FUNC: printf("<func>"); break;
    case VAL_ARRAY: {
        printf("[");
        for(int i=0;i<v->v.array->len;i++){
            if(i>0)printf(",");
            val_print(&v->v.array->items[i]);
        }
        printf("]");
        break;
    }
    case VAL_NONE: printf("null"); break;
    default: printf("<?type=%d>",(int)v->type);
    }
}

// ==================== 自增自减 ====================
Value lumyr_post_inc(Value* v) {
    Value old = *v;
    switch(v->type) {
        case VAL_INT:    v->v.i += 1; break;
        case VAL_DOUBLE: v->v.d += 1.0; break;
        case VAL_CHAR:   v->v.c += 1; break;
        default: runtime_error("post_inc:类型不支持++");
    }
    return old;
}

Value lumyr_pre_inc(Value* v) {
    switch(v->type) {
        case VAL_INT:    v->v.i += 1; break;
        case VAL_DOUBLE: v->v.d += 1.0; break;
        case VAL_CHAR:   v->v.c += 1; break;
        default: runtime_error("pre_inc:类型不支持++");
    }
    return *v;
}

Value lumyr_post_dec(Value* v) {
    Value old = *v;
    switch(v->type) {
        case VAL_INT:    v->v.i -= 1; break;
        case VAL_DOUBLE: v->v.d -= 1.0; break;
        case VAL_CHAR:   v->v.c -= 1; break;
        default: runtime_error("post_dec:类型不支持--");
    }
    return old;
}

Value lumyr_pre_dec(Value* v) {
    switch(v->type) {
        case VAL_INT:    v->v.i -= 1; break;
        case VAL_DOUBLE: v->v.d -= 1.0; break;
        case VAL_CHAR:   v->v.c -= 1; break;
        default: runtime_error("pre_dec:类型不支持--");
    }
    return *v;
}
