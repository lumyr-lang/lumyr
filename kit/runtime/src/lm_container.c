// lm_container.c —— 容器与数值扩展类型（tuple/set/bytes/complex）
#include "lm_container.h"
#include "gc_runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ===== 容器并发结构修改检测（误用即显式报错，不静默、不堆损坏）===== */

void lumyr_enter_write(volatile int* flag) {
    if(!__sync_bool_compare_and_swap(flag, 0, 1))
        runtime_error("容器并发结构修改：多个线程同时修改同一容器，请用 mutex 同步 / "
                      "concurrent container structural modification: synchronize with a mutex");
}

void lumyr_leave_write(volatile int* flag) {
    __sync_synchronize();  /* 保证全部结构写入先于清标志对其他线程可见 */
    *flag = 0;
}

/* ===== tuple（VAL_TUPLE）：不可变固定长度异构序列 ===== */

Value lumyr_tuple_make(int argc, const Value* args) {
    TupleObj* o = (TupleObj*)gc_alloc(sizeof(TupleObj), VAL_TUPLE);
    if(!o) {
        Value z; z.type = VAL_NONE; z.str_inline = 0; return z;
    }
    o->len = argc;
    o->stack_alloc = 0;
    if(argc > 0) {
        o->items = (Value*)gc_alloc(sizeof(Value) * (argc > 0 ? argc : 1), VAL_TUPLE);
        if(!o->items) { o->len = 0; }
        else { memcpy(o->items, args, sizeof(Value) * argc); }
    } else {
        o->items = NULL;
    }
    Value r;
    r.type = VAL_TUPLE;
    r.str_inline = 0;
    r.v.tuple_obj = o;
    return r;
}

int lumyr_tuple_len(Value v) {
    if(v.type != VAL_TUPLE) return 0;
    TupleObj* o = (TupleObj*)v.v.tuple_obj;
    return o ? o->len : 0;
}

Value lumyr_tuple_get(Value v, int idx) {
    if(v.type != VAL_TUPLE) return lumyr_make_int(0);
    TupleObj* o = (TupleObj*)v.v.tuple_obj;
    if(!o || idx < 0 || idx >= o->len) return lumyr_make_int(0);
    return o->items[idx];
}

_Bool lumyr_tuple_eq(Value a, Value b) {
    if(a.type != VAL_TUPLE || b.type != VAL_TUPLE) return 0;
    TupleObj* oa = (TupleObj*)a.v.tuple_obj;
    TupleObj* ob = (TupleObj*)b.v.tuple_obj;
    if(!oa || !ob) return oa == ob;
    if(oa->len != ob->len) return 0;
    for(int i = 0; i < oa->len; i++) {
        Value e = lumyr_eq(oa->items[i], ob->items[i]);
        if(!lumyr_to_bool(e)) return 0;
    }
    return 1;
}

char* lumyr_tuple_to_str(Value v) {
    if(v.type != VAL_TUPLE) return strdup("()");
    TupleObj* o = (TupleObj*)v.v.tuple_obj;
    if(!o || o->len == 0) return strdup("()");
    // 拼接 (e1, e2, ...)，递归 value_to_str
    size_t cap = 64;
    char* buf = (char*)malloc(cap);
    int p = 0;
    buf[p++] = '(';
    for(int i = 0; i < o->len; i++) {
        if(i > 0) { buf[p++] = ','; buf[p++] = ' '; }
        char* es = value_to_str(o->items[i]);
        if(!es) es = strdup("");
        size_t el = strlen(es);
        while((size_t)p + el + 3 >= cap) {
            cap *= 2;
            buf = (char*)realloc(buf, cap);
        }
        memcpy(buf + p, es, el);
        p += (int)el;
        free(es);
    }
    buf[p++] = ')';
    buf[p] = '\0';
    return buf;
}

/* ===== set（VAL_SET）：无序唯一元素集合（底层 ValueMap） ===== */

static Value set_alloc(void) {
    SetObj* o = (SetObj*)gc_alloc(sizeof(SetObj), VAL_SET);
    if(!o) {
        Value z; z.type = VAL_NONE; z.str_inline = 0; return z;
    }
    o->map = NULL;
    o->stack_alloc = 0;
    // 底层用空 map
    Value mv = val_map();
    o->map = mv.v.map;
    Value r;
    r.type = VAL_SET;
    r.str_inline = 0;
    r.v.set_obj = o;
    return r;
}

Value lumyr_set_make(int argc, const Value* args) {
    Value r = set_alloc();
    if(r.type != VAL_SET) return r;
    SetObj* o = (SetObj*)r.v.set_obj;
    Value mv; mv.type = VAL_MAP; mv.str_inline = 0; mv.v.map = o->map;
    for(int i = 0; i < argc; i++) {
        lumyr_map_set(&mv, args[i], lumyr_make_int(1));
    }
    return r;
}

int lumyr_set_len(Value v) {
    if(v.type != VAL_SET) return 0;
    SetObj* o = (SetObj*)v.v.set_obj;
    return (o && o->map) ? o->map->len : 0;
}

_Bool lumyr_set_has(Value v, Value elem) {
    if(v.type != VAL_SET) return 0;
    SetObj* o = (SetObj*)v.v.set_obj;
    if(!o || !o->map) return 0;
    Value mv; mv.type = VAL_MAP; mv.str_inline = 0; mv.v.map = o->map;
    return lumyr_map_has(mv, elem) ? 1 : 0;
}

Value lumyr_set_add(Value* v, Value elem) {
    if(!v || v->type != VAL_SET) return lumyr_make_int(0);
    SetObj* o = (SetObj*)v->v.set_obj;
    if(!o || !o->map) return *v;
    Value mv; mv.type = VAL_MAP; mv.str_inline = 0; mv.v.map = o->map;
    lumyr_map_set(&mv, elem, lumyr_make_int(1));
    return *v;
}

Value lumyr_set_remove(Value* v, Value elem) {
    if(!v || v->type != VAL_SET) return lumyr_make_int(0);
    SetObj* o = (SetObj*)v->v.set_obj;
    if(!o || !o->map) return *v;
    Value mv; mv.type = VAL_MAP; mv.str_inline = 0; mv.v.map = o->map;
    lumyr_map_del(&mv, elem);
    return *v;
}

Value lumyr_set_to_array(Value v) {
    if(v.type != VAL_SET) return lumyr_make_int(0);
    SetObj* o = (SetObj*)v.v.set_obj;
    if(!o || !o->map) return lumyr_make_int(0);
    Value mv; mv.type = VAL_MAP; mv.str_inline = 0; mv.v.map = o->map;
    return lumyr_map_keys(mv);
}

static Value set_from_map(ValueMap* m) {
    Value r = set_alloc();
    if(r.type != VAL_SET) return r;
    SetObj* o = (SetObj*)r.v.set_obj;
    Value mv; mv.type = VAL_MAP; mv.str_inline = 0; mv.v.map = o->map;
    MapIter it;
    map_iter_init(&it, m);
    Value k, val;
    while(map_iter_next(&it, &k, &val)) {
        lumyr_map_set(&mv, k, lumyr_make_int(1));
    }
    return r;
}

Value lumyr_set_union(Value a, Value b) {
    if(a.type != VAL_SET || b.type != VAL_SET) return lumyr_make_int(0);
    SetObj* oa = (SetObj*)a.v.set_obj;
    SetObj* ob = (SetObj*)b.v.set_obj;
    if(!oa || !ob) return lumyr_make_int(0);
    Value r = set_alloc();
    SetObj* o = (SetObj*)r.v.set_obj;
    Value mv; mv.type = VAL_MAP; mv.str_inline = 0; mv.v.map = o->map;
    MapIter it;
    map_iter_init(&it, oa->map);
    Value k, val;
    while(map_iter_next(&it, &k, &val)) lumyr_map_set(&mv, k, lumyr_make_int(1));
    map_iter_init(&it, ob->map);
    while(map_iter_next(&it, &k, &val)) lumyr_map_set(&mv, k, lumyr_make_int(1));
    return r;
}

Value lumyr_set_intersect(Value a, Value b) {
    if(a.type != VAL_SET || b.type != VAL_SET) return lumyr_make_int(0);
    SetObj* oa = (SetObj*)a.v.set_obj;
    SetObj* ob = (SetObj*)b.v.set_obj;
    if(!oa || !ob) return lumyr_make_int(0);
    Value mvb; mvb.type = VAL_MAP; mvb.str_inline = 0; mvb.v.map = ob->map;
    Value r = set_alloc();
    SetObj* o = (SetObj*)r.v.set_obj;
    Value mv; mv.type = VAL_MAP; mv.str_inline = 0; mv.v.map = o->map;
    MapIter it;
    map_iter_init(&it, oa->map);
    Value k, val;
    while(map_iter_next(&it, &k, &val)) {
        if(lumyr_map_has(mvb, k)) lumyr_map_set(&mv, k, lumyr_make_int(1));
    }
    return r;
}

Value lumyr_set_diff(Value a, Value b) {
    if(a.type != VAL_SET || b.type != VAL_SET) return lumyr_make_int(0);
    SetObj* oa = (SetObj*)a.v.set_obj;
    SetObj* ob = (SetObj*)b.v.set_obj;
    if(!oa || !ob) return lumyr_make_int(0);
    Value mvb; mvb.type = VAL_MAP; mvb.str_inline = 0; mvb.v.map = ob->map;
    Value r = set_alloc();
    SetObj* o = (SetObj*)r.v.set_obj;
    Value mv; mv.type = VAL_MAP; mv.str_inline = 0; mv.v.map = o->map;
    MapIter it;
    map_iter_init(&it, oa->map);
    Value k, val;
    while(map_iter_next(&it, &k, &val)) {
        if(!lumyr_map_has(mvb, k)) lumyr_map_set(&mv, k, lumyr_make_int(1));
    }
    return r;
}

_Bool lumyr_set_eq(Value a, Value b) {
    if(a.type != VAL_SET || b.type != VAL_SET) return 0;
    SetObj* oa = (SetObj*)a.v.set_obj;
    SetObj* ob = (SetObj*)b.v.set_obj;
    if(!oa || !ob) return oa == ob;
    if(oa->map->len != ob->map->len) return 0;
    Value mvb; mvb.type = VAL_MAP; mvb.str_inline = 0; mvb.v.map = ob->map;
    MapIter it;
    map_iter_init(&it, oa->map);
    Value k, val;
    while(map_iter_next(&it, &k, &val)) {
        if(!lumyr_map_has(mvb, k)) return 0;
    }
    return 1;
}

char* lumyr_set_to_str(Value v) {
    if(v.type != VAL_SET) return strdup("{}");
    SetObj* o = (SetObj*)v.v.set_obj;
    if(!o || !o->map || o->map->len == 0) return strdup("{}");
    Value mv; mv.type = VAL_MAP; mv.str_inline = 0; mv.v.map = o->map;
    MapIter it;
    map_iter_init(&it, o->map);
    size_t cap = 64;
    char* buf = (char*)malloc(cap);
    int p = 0;
    buf[p++] = '{';
    Value k, val;
    int first = 1;
    while(map_iter_next(&it, &k, &val)) {
        if(!first) { buf[p++] = ','; buf[p++] = ' '; }
        first = 0;
        char* es = value_to_str(k);
        if(!es) es = strdup("");
        size_t el = strlen(es);
        while((size_t)p + el + 3 >= cap) { cap *= 2; buf = (char*)realloc(buf, cap); }
        memcpy(buf + p, es, el);
        p += (int)el;
        free(es);
    }
    buf[p++] = '}';
    buf[p] = '\0';
    return buf;
}

/* ===== bytes（VAL_BYTES）：不可变字节串 ===== */

static Value bytes_alloc(const uint8_t* data, int len) {
    BytesObj* o = (BytesObj*)gc_alloc(sizeof(BytesObj), VAL_BYTES);
    if(!o) {
        Value z; z.type = VAL_NONE; z.str_inline = 0; return z;
    }
    o->len = len;
    o->elem_type = VAL_UINT8;   /* 默认普通 uint8 字节 */
    o->stack_alloc = 0;
    if(len > 0) {
        o->data = (uint8_t*)gc_alloc(sizeof(uint8_t) * (len > 0 ? len : 1), VAL_BYTES);
        if(o->data) memcpy(o->data, data, len);
        else o->len = 0;
    } else {
        o->data = NULL;
    }
    Value r;
    r.type = VAL_BYTES;
    r.str_inline = 0;
    r.v.bytes_obj = o;
    return r;
}

Value lumyr_bytes_from_buf(const uint8_t* data, int len) {
    return bytes_alloc(data, len);
}

// 元素类型 → 字节宽度
int lumyr_elem_size(ValueType t) {
    switch(t) {
    case VAL_INT8: case VAL_UINT8: case VAL_UCHAR: case VAL_BYTE: case VAL_CHAR: case VAL_BOOL:
        return 1;
    case VAL_INT16: case VAL_UINT16: case VAL_SHORT: case VAL_USHORT:
        return 2;
    case VAL_INT32: case VAL_UINT32: case VAL_UINT: case VAL_FLOAT:
        return 4;
    case VAL_INT64: case VAL_UINT64: case VAL_LONG: case VAL_LONG_LONG:
    case VAL_ULONG: case VAL_SIZE_T: case VAL_SSIZE_T: case VAL_DOUBLE: case VAL_LONG_DOUBLE:
        return 8;
    default:
        return 1;
    }
}

// CastKind → ValueType（类型化 bytes 用）
ValueType lumyr_cast_kind_to_value_type(int cast_kind) {
    switch((CastKind)cast_kind) {
    case CAST_INT8:     return VAL_INT8;
    case CAST_UINT8:    return VAL_UINT8;
    case CAST_BYTE:     return VAL_BYTE;
    case CAST_UCHAR:    return VAL_UCHAR;
    case CAST_CHAR:      return VAL_CHAR;
    case CAST_BOOL:      return VAL_BOOL;
    case CAST_INT16:    return VAL_INT16;
    case CAST_UINT16:   return VAL_UINT16;
    case CAST_SHORT:    return VAL_SHORT;
    case CAST_USHORT:   return VAL_USHORT;
    case CAST_INT32:    return VAL_INT32;
    case CAST_UINT32:   return VAL_UINT32;
    case CAST_UINT:     return VAL_UINT;
    case CAST_INT:      return VAL_INT32;   /* int 默认 32 位 */
    case CAST_INT64:    return VAL_INT64;
    case CAST_UINT64:   return VAL_UINT64;
    case CAST_LONG:     return VAL_LONG;
    case CAST_LONGLONG: return VAL_LONG_LONG;
    case CAST_ULONG:    return VAL_ULONG;
    case CAST_SIZE_T:   return VAL_SIZE_T;
    case CAST_SSIZE_T:  return VAL_SSIZE_T;
    case CAST_FLOAT:    return VAL_FLOAT;
    case CAST_DOUBLE: case CAST_LONG_DOUBLE: return VAL_DOUBLE;
    default:            return VAL_UINT8;
    }
}

// 把单个 Value 打包为 elem_type 的字节（小端序），写入 p[0..es-1]
static void pack_elem(uint8_t* p, int es, ValueType et, Value v) {
    /* 提取整数（尽量宽）与浮点 */
    int64_t iv = 0;
    double dv = 0.0;
    int is_double = 0;
    switch(v.type) {
    case VAL_INT:        iv = v.v.i; break;
    case VAL_INT8:       iv = v.v.i8; break;
    case VAL_INT16:      iv = v.v.i16; break;
    case VAL_SHORT:      iv = v.v.sh; break;
    case VAL_INT32:      iv = v.v.i32; break;
    case VAL_INT64:      iv = v.v.i64; break;
    case VAL_LONG:       iv = v.v.l; break;
    case VAL_LONG_LONG:  iv = v.v.ll; break;
    case VAL_UINT8:       iv = v.v.u8; break;
    case VAL_UCHAR:       iv = v.v.uc; break;
    case VAL_BYTE:        iv = v.v.by; break;
    case VAL_CHAR:        iv = v.v.c; break;
    case VAL_BOOL:        iv = v.v.b ? 1 : 0; break;
    case VAL_UINT16:      iv = v.v.u16; break;
    case VAL_USHORT:      iv = v.v.us; break;
    case VAL_UINT32:      iv = v.v.u32; break;
    case VAL_UINT:        iv = v.v.ui; break;
    case VAL_UINT64:      iv = (int64_t)v.v.u64; break;
    case VAL_ULONG:       iv = (int64_t)v.v.ul; break;
    case VAL_SIZE_T:      iv = (int64_t)v.v.st; break;
    case VAL_SSIZE_T:     iv = v.v.sst; break;
    case VAL_DOUBLE:      dv = v.v.d; is_double = 1; break;
    case VAL_FLOAT:       dv = v.v.f; is_double = 1; break;
    case VAL_LONG_DOUBLE: dv = (double)v.v.ld; is_double = 1; break;
    default:              iv = 0; break;
    }
    switch(et) {
    case VAL_INT8: { int8_t x = (int8_t)iv; memcpy(p, &x, 1); break; }
    case VAL_UINT8: case VAL_UCHAR: case VAL_BYTE: case VAL_CHAR: case VAL_BOOL: {
        uint8_t x = (uint8_t)iv; memcpy(p, &x, 1); break;
    }
    case VAL_INT16: case VAL_SHORT: { int16_t x = (int16_t)iv; memcpy(p, &x, 2); break; }
    case VAL_UINT16: case VAL_USHORT: { uint16_t x = (uint16_t)iv; memcpy(p, &x, 2); break; }
    case VAL_INT32: case VAL_UINT: { int32_t x = (int32_t)iv; memcpy(p, &x, 4); break; }
    case VAL_UINT32: { uint32_t x = (uint32_t)iv; memcpy(p, &x, 4); break; }
    case VAL_INT64: case VAL_LONG: case VAL_LONG_LONG: case VAL_SSIZE_T: {
        int64_t x = iv; memcpy(p, &x, 8); break;
    }
    case VAL_UINT64: case VAL_ULONG: case VAL_SIZE_T: {
        uint64_t x = (uint64_t)iv; memcpy(p, &x, 8); break;
    }
    case VAL_FLOAT: { float x = (float)(is_double ? dv : (double)iv); memcpy(p, &x, 4); break; }
    case VAL_DOUBLE: case VAL_LONG_DOUBLE: { double x = is_double ? dv : (double)iv; memcpy(p, &x, 8); break; }
    default: { uint8_t x = (uint8_t)iv; memcpy(p, &x, 1); break; }
    }
    (void)es;
}

/* 重解释：把已有 bytes 按目标元素类型重新视图化（共享不可变数据缓冲，仅改 elem_type） */
Value lumyr_bytes_reinterpret(Value src, ValueType et) {
    BytesObj* s = (BytesObj*)src.v.bytes_obj;
    BytesObj* o = (BytesObj*)gc_alloc(sizeof(BytesObj), VAL_BYTES);
    if(!o) { Value z; z.type = VAL_NONE; z.str_inline = 0; return z; }
    o->elem_type = et;
    o->stack_alloc = 0;
    o->data = s ? s->data : NULL;
    o->len = s ? s->len : 0;
    Value r;
    r.type = VAL_BYTES;
    r.str_inline = 0;
    r.v.bytes_obj = o;
    return r;
}

Value lumyr_bytes_typed(int cast_kind, Value src) {
    ValueType et = lumyr_cast_kind_to_value_type(cast_kind);
    int es = lumyr_elem_size(et);
    /* 重解释路径：src 已是 bytes，共享缓冲仅改 elem_type */
    if(src.type == VAL_BYTES) return lumyr_bytes_reinterpret(src, et);
    BytesObj* o = (BytesObj*)gc_alloc(sizeof(BytesObj), VAL_BYTES);
    if(!o) { Value z; z.type = VAL_NONE; z.str_inline = 0; return z; }
    o->elem_type = et;
    o->stack_alloc = 0;
    o->data = NULL;
    o->len = 0;
    if(src.type == VAL_ARRAY) {
        ValueArray* a = src.v.array;
        int n = a ? a->len : 0;
        int blen = n * es;
        o->len = blen;
        if(blen > 0) {
            o->data = (uint8_t*)gc_alloc(blen, VAL_BYTES);
            if(!o->data) { o->len = 0; }
            else for(int i = 0; i < n; i++) pack_elem(o->data + i*es, es, et, a->items[i]);
        }
    } else if(src.type == VAL_STRING) {
        const char* str = lumyr_str_cstr(&src);
        int blen = str ? (int)strlen(str) : 0;
        o->len = blen;
        if(blen > 0) {
            o->data = (uint8_t*)gc_alloc(blen, VAL_BYTES);
            if(o->data) memcpy(o->data, str, blen); else o->len = 0;
        }
    } else {
        /* 标量 → 单元素（宽度 es 字节） */
        o->len = es;
        o->data = (uint8_t*)gc_alloc(es > 0 ? es : 1, VAL_BYTES);
        if(o->data) pack_elem(o->data, es, et, src); else o->len = 0;
    }
    Value r;
    r.type = VAL_BYTES;
    r.str_inline = 0;
    r.v.bytes_obj = o;
    return r;
}

Value lumyr_bytes_make(int argc, const Value* args) {
    if(argc == 0) return bytes_alloc(NULL, 0);
    // 单参数：字符串 或 数组
    if(argc == 1) {
        Value a = args[0];
        if(a.type == VAL_STRING) {
            const char* s = lumyr_str_cstr(&a);
            int sl = lumyr_str_len(&a);
            if(!s || sl <= 0) return bytes_alloc(NULL, 0);
            return bytes_alloc((const uint8_t*)s, sl);
        }
        if(a.type == VAL_ARRAY) {
            ValueArray* arr = a.v.array;
            if(!arr || arr->len <= 0) return bytes_alloc(NULL, 0);
            uint8_t* tmp = (uint8_t*)malloc(arr->len);
            for(int i = 0; i < arr->len; i++) {
                tmp[i] = (uint8_t)lumyr_extract_ll(arr->items[i]);
            }
            Value r = bytes_alloc(tmp, arr->len);
            free(tmp);
            return r;
        }
        if(a.type == VAL_TYPED_ARRAY) {
            TypedArray* ta = a.v.typed_array;
            if(!ta || ta->len <= 0) return bytes_alloc(NULL, 0);
            uint8_t* tmp = (uint8_t*)malloc(ta->len);
            // 仅 uint8 元素直接拷贝，其他按整数提取
            if(ta->elem_type == VAL_UINT8 || ta->elem_type == VAL_BYTE || ta->elem_type == VAL_UCHAR) {
                memcpy(tmp, ta->items, ta->len);
            } else {
                for(int i = 0; i < ta->len; i++) {
                    tmp[i] = (uint8_t)lumyr_extract_ll(((Value*)ta->items)[i]);
                }
            }
            Value r = bytes_alloc(tmp, ta->len);
            free(tmp);
            return r;
        }
    }
    // 多参数：按整数提取
    uint8_t* tmp = (uint8_t*)malloc(argc > 0 ? argc : 1);
    for(int i = 0; i < argc; i++) tmp[i] = (uint8_t)lumyr_extract_ll(args[i]);
    Value r = bytes_alloc(tmp, argc);
    free(tmp);
    return r;
}

Value lumyr_bytes_from_hex(const char* hex) {
    if(!hex) return bytes_alloc(NULL, 0);
    int hl = (int)strlen(hex);
    if(hl % 2 != 0) return bytes_alloc(NULL, 0);
    int bl = hl / 2;
    if(bl <= 0) return bytes_alloc(NULL, 0);
    uint8_t* tmp = (uint8_t*)malloc(bl);
    for(int i = 0; i < bl; i++) {
        char h1 = hex[i*2], h2 = hex[i*2+1];
        int v1 = -1, v2 = -1;
        if(h1 >= '0' && h1 <= '9') v1 = h1 - '0';
        else if(h1 >= 'a' && h1 <= 'f') v1 = h1 - 'a' + 10;
        else if(h1 >= 'A' && h1 <= 'F') v1 = h1 - 'A' + 10;
        if(h2 >= '0' && h2 <= '9') v2 = h2 - '0';
        else if(h2 >= 'a' && h2 <= 'f') v2 = h2 - 'a' + 10;
        else if(h2 >= 'A' && h2 <= 'F') v2 = h2 - 'A' + 10;
        if(v1 < 0 || v2 < 0) { free(tmp); return bytes_alloc(NULL, 0); }
        tmp[i] = (uint8_t)((v1 << 4) | v2);
    }
    Value r = bytes_alloc(tmp, bl);
    free(tmp);
    return r;
}

int lumyr_bytes_len(Value v) {
    if(v.type != VAL_BYTES) return 0;
    BytesObj* o = (BytesObj*)v.v.bytes_obj;
    if(!o) return 0;
    /* 类型化 bytes：返回元素个数（字节长度 / 元素宽度） */
    int es = lumyr_elem_size(o->elem_type);
    if(es <= 1) return o->len;
    return o->len / es;
}

// 按 elem_type 读取第 idx 个元素（小端序）→ Value
static Value bytes_read_elem(BytesObj* o, int idx) {
    int es = lumyr_elem_size(o->elem_type);
    int off = idx * es;
    if(off < 0 || off + es > o->len) return lumyr_make_int(0);
    const uint8_t* p = o->data + off;
    switch(o->elem_type) {
    case VAL_INT8: {
        int8_t x; memcpy(&x, p, 1); return lumyr_make_int((long long)x);
    }
    case VAL_UINT8: case VAL_UCHAR: case VAL_BYTE: case VAL_CHAR: case VAL_BOOL: {
        uint8_t x; memcpy(&x, p, 1); return lumyr_make_int((long long)x);
    }
    case VAL_INT16: case VAL_SHORT: {
        int16_t x; memcpy(&x, p, 2); return lumyr_make_int((long long)x);
    }
    case VAL_UINT16: case VAL_USHORT: {
        uint16_t x; memcpy(&x, p, 2); return lumyr_make_int((long long)x);
    }
    case VAL_INT32: {
        int32_t x; memcpy(&x, p, 4); return lumyr_make_int((long long)x);
    }
    case VAL_UINT: case VAL_UINT32: {
        /* uint32 范围可能超过 INT32_MAX，用 int64 承载避免截断 */
        uint32_t x; memcpy(&x, p, 4); return lumyr_make_int64((int64_t)(uint64_t)x);
    }
    case VAL_INT64: case VAL_LONG: case VAL_LONG_LONG: case VAL_SSIZE_T: {
        int64_t x; memcpy(&x, p, 8); return lumyr_make_int64(x);
    }
    case VAL_UINT64: case VAL_ULONG: case VAL_SIZE_T: {
        uint64_t x; memcpy(&x, p, 8); return lumyr_make_int64((int64_t)x);
    }
    case VAL_FLOAT: {
        float x; memcpy(&x, p, 4); return lumyr_make_double((double)x);
    }
    case VAL_DOUBLE: case VAL_LONG_DOUBLE: {
        double x; memcpy(&x, p, 8); return lumyr_make_double(x);
    }
    default: {
        uint8_t x; memcpy(&x, p, 1); return lumyr_make_int((long long)x);
    }
    }
}

Value lumyr_bytes_get(Value v, int idx) {
    if(v.type != VAL_BYTES) return lumyr_make_int(0);
    BytesObj* o = (BytesObj*)v.v.bytes_obj;
    if(!o) return lumyr_make_int(0);
    int cnt = lumyr_bytes_len(v);
    if(idx < 0 || idx >= cnt) return lumyr_make_int(0);
    return bytes_read_elem(o, idx);
}

char* lumyr_bytes_to_str(Value v) {
    if(v.type != VAL_BYTES) return strdup("b\"\"");
    BytesObj* o = (BytesObj*)v.v.bytes_obj;
    if(!o || o->len <= 0) return strdup("b\"\"");
    // 输出 b"..."，可打印字符原样，不可打印用 \xNN
    size_t cap = (size_t)o->len * 4 + 8;
    char* buf = (char*)malloc(cap);
    int p = 0;
    buf[p++] = 'b';
    buf[p++] = '"';
    for(int i = 0; i < o->len; i++) {
        uint8_t c = o->data[i];
        if(c >= 0x20 && c < 0x7f && c != '"' && c != '\\') {
            buf[p++] = (char)c;
        } else {
            buf[p++] = '\\';
            if(c == '"') buf[p++] = '"';
            else if(c == '\\') buf[p++] = '\\';
            else if(c == '\n') { buf[p++] = 'n'; }
            else if(c == '\t') { buf[p++] = 't'; }
            else if(c == '\r') { buf[p++] = 'r'; }
            else {
                buf[p++] = 'x';
                const char* hx = "0123456789abcdef";
                buf[p++] = hx[(c >> 4) & 0xf];
                buf[p++] = hx[c & 0xf];
            }
        }
    }
    buf[p++] = '"';
    buf[p] = '\0';
    return buf;
}

char* lumyr_bytes_hex(Value v) {
    if(v.type != VAL_BYTES) return strdup("");
    BytesObj* o = (BytesObj*)v.v.bytes_obj;
    if(!o || o->len <= 0) return strdup("");
    char* buf = (char*)malloc((size_t)o->len * 2 + 1);
    const char* hx = "0123456789abcdef";
    for(int i = 0; i < o->len; i++) {
        buf[i*2] = hx[(o->data[i] >> 4) & 0xf];
        buf[i*2+1] = hx[o->data[i] & 0xf];
    }
    buf[o->len * 2] = '\0';
    return buf;
}

char* lumyr_bytes_to_utf8(Value v) {
    if(v.type != VAL_BYTES) return strdup("");
    BytesObj* o = (BytesObj*)v.v.bytes_obj;
    if(!o || o->len <= 0) return strdup("");
    char* buf = (char*)malloc(o->len + 1);
    memcpy(buf, o->data, o->len);
    buf[o->len] = '\0';
    return buf;
}

/* ===== complex（VAL_COMPLEX）：复数 real+imag double ===== */

Value lumyr_complex_make(double real, double imag) {
    ComplexObj* o = (ComplexObj*)gc_alloc(sizeof(ComplexObj), VAL_COMPLEX);
    if(!o) {
        Value z; z.type = VAL_NONE; z.str_inline = 0; return z;
    }
    o->real = real;
    o->imag = imag;
    Value r;
    r.type = VAL_COMPLEX;
    r.str_inline = 0;
    r.v.complex_obj = o;
    return r;
}

double lumyr_complex_real(Value v) {
    if(v.type != VAL_COMPLEX) return 0.0;
    ComplexObj* o = (ComplexObj*)v.v.complex_obj;
    return o ? o->real : 0.0;
}

double lumyr_complex_imag(Value v) {
    if(v.type != VAL_COMPLEX) return 0.0;
    ComplexObj* o = (ComplexObj*)v.v.complex_obj;
    return o ? o->imag : 0.0;
}

double lumyr_complex_abs(Value v) {
    if(v.type != VAL_COMPLEX) return 0.0;
    ComplexObj* o = (ComplexObj*)v.v.complex_obj;
    if(!o) return 0.0;
    return sqrt(o->real * o->real + o->imag * o->imag);
}

Value lumyr_complex_conjugate(Value v) {
    if(v.type != VAL_COMPLEX) return lumyr_make_double(0.0);
    ComplexObj* o = (ComplexObj*)v.v.complex_obj;
    if(!o) return lumyr_make_double(0.0);
    return lumyr_complex_make(o->real, -o->imag);
}

char* lumyr_complex_to_str(Value v) {
    if(v.type != VAL_COMPLEX) return strdup("(0+0j)");
    ComplexObj* o = (ComplexObj*)v.v.complex_obj;
    if(!o) return strdup("(0+0j)");
    char buf[128];
    if(o->imag >= 0) {
        snprintf(buf, sizeof(buf), "(%g+%gj)", o->real, o->imag);
    } else {
        snprintf(buf, sizeof(buf), "(%g%gj)", o->real, o->imag);
    }
    return strdup(buf);
}

Value lumyr_complex_add(Value a, Value b) {
    if(a.type != VAL_COMPLEX || b.type != VAL_COMPLEX) return lumyr_make_double(0.0);
    ComplexObj* oa = (ComplexObj*)a.v.complex_obj;
    ComplexObj* ob = (ComplexObj*)b.v.complex_obj;
    if(!oa || !ob) return lumyr_make_double(0.0);
    return lumyr_complex_make(oa->real + ob->real, oa->imag + ob->imag);
}

Value lumyr_complex_sub(Value a, Value b) {
    if(a.type != VAL_COMPLEX || b.type != VAL_COMPLEX) return lumyr_make_double(0.0);
    ComplexObj* oa = (ComplexObj*)a.v.complex_obj;
    ComplexObj* ob = (ComplexObj*)b.v.complex_obj;
    if(!oa || !ob) return lumyr_make_double(0.0);
    return lumyr_complex_make(oa->real - ob->real, oa->imag - ob->imag);
}

Value lumyr_complex_mul(Value a, Value b) {
    if(a.type != VAL_COMPLEX || b.type != VAL_COMPLEX) return lumyr_make_double(0.0);
    ComplexObj* oa = (ComplexObj*)a.v.complex_obj;
    ComplexObj* ob = (ComplexObj*)b.v.complex_obj;
    if(!oa || !ob) return lumyr_make_double(0.0);
    // (a+bi)(c+di) = (ac-bd) + (ad+bc)i
    double r = oa->real * ob->real - oa->imag * ob->imag;
    double i = oa->real * ob->imag + oa->imag * ob->real;
    return lumyr_complex_make(r, i);
}
