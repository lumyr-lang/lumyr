/*
 * vm_exec_stack.c - VM 栈操作指令
 * 4 核心栈设计：STACK_VALUE / INT64 / DOUBLE / PTR
 */
#include "vm_types.h"
#include "stack_manager.h"
#include "lumyr_value.h"
#include "gc_runtime.h"
#include "lm_map.h"

/* 与 GC 内部 GC_VALID_PTR 等价的指针有效性判断（该宏未在头文件公开） */
static inline int typed_ptr_ok(const void* p) {
    return p && (unsigned long long)p >= 4096 &&
           (unsigned long long)p <= 0x00007fffffffffffULL;
}

/* ===== 栈操作 ===== */

/* POP：弹出 VALUE 栈顶 */
int vm_exec_stack_pop(VMExecCtx* ctx, Instruction* in) {
    Value val;
    stack_vm_pop(g_stack_mgr, STACK_VALUE, &val);
    return 1;
}

/* DUP：复制 VALUE 栈顶 */
int vm_exec_stack_dup(VMExecCtx* ctx, Instruction* in) {
    Value val;
    stack_vm_pop(g_stack_mgr, STACK_VALUE, &val);
    stack_vm_push(g_stack_mgr, STACK_VALUE, &val);
    stack_vm_push(g_stack_mgr, STACK_VALUE, &val);
    return 1;
}

/* ARRAY_LIT：弹 b 个 VALUE 栈顶元素（栈顶为最后一个），构造数组并压入 VALUE 栈。
 * 元素顺序保持实参左至右：弹栈逆序，回填 items[i] 时倒序放置。 */
int vm_exec_array_lit(VMExecCtx* ctx, Instruction* in) {
    (void)ctx;
    int n = in->b;
    Value arr = val_array(n);
    if (n > 0) {
        for (int i = n - 1; i >= 0; --i) {
            Value v;
            stack_vm_pop(g_stack_mgr, STACK_VALUE, &v);
            arr.v.array->items[i] = v;
        }
    }
    stack_vm_push(g_stack_mgr, STACK_VALUE, &arr);
    return 1;
}

/* INT64_TO_PTR：弹 INT64 栈顶整数，作为裸地址压 PTR 栈 */
int vm_exec_int64_to_ptr(VMExecCtx* ctx, Instruction* in) {
    (void)ctx; (void)in;
    int64_t v;
    stack_vm_pop(g_stack_mgr, STACK_INT64, &v);
    void* p = (void*)(uintptr_t)v;
    stack_vm_push(g_stack_mgr, STACK_PTR, &p);
    return 1;
}

/* PTR_TO_INT64：弹 PTR 栈顶指针，作为整数地址压 INT64 栈 */
int vm_exec_ptr_to_int64(VMExecCtx* ctx, Instruction* in) {
    (void)ctx; (void)in;
    void* p;
    stack_vm_pop(g_stack_mgr, STACK_PTR, &p);
    int64_t v = (int64_t)(uintptr_t)p;
    stack_vm_push(g_stack_mgr, STACK_INT64, &v);
    return 1;
}

/* 把 Value 下标转为 int64（支持 VAL_INT/INT64/LONG_LONG 等） */
static int64_t value_to_index(Value v) {
    switch(v.type) {
    case VAL_INT:       return (int64_t)v.v.i;
    case VAL_INT64:     return v.v.i64;
    case VAL_LONG_LONG: return (int64_t)v.v.ll;
    case VAL_LONG:      return (int64_t)v.v.l;
    case VAL_INT32:     return (int64_t)v.v.i32;
    case VAL_INT16:     return (int64_t)v.v.i16;
    case VAL_INT8:      return (int64_t)v.v.i8;
    case VAL_SHORT:     return (int64_t)v.v.sh;
    case VAL_UINT:      return (int64_t)v.v.ui;
    case VAL_UINT64:    return (int64_t)v.v.u64;
    case VAL_UINT32:    return (int64_t)v.v.u32;
    case VAL_DOUBLE:    return (int64_t)v.v.d;
    default:            return 0;
    }
}

/* ===== TypedArray 元素转换辅助 ===== */

/* 整型族：按元素类型从裸 items[i] 读取为 int64 */
static int64_t typed_read_i64(ValueType et, const void* items, int i) {
    switch(et) {
    case VAL_INT:       return ((const int*)items)[i];
    case VAL_INT8:      return ((const int8_t*)items)[i];
    case VAL_INT16:     return ((const int16_t*)items)[i];
    case VAL_INT32:     return ((const int32_t*)items)[i];
    case VAL_INT64:     return ((const int64_t*)items)[i];
    case VAL_LONG_LONG: return ((const long long*)items)[i];
    case VAL_LONG:      return ((const long*)items)[i];
    case VAL_UINT8:     return ((const uint8_t*)items)[i];
    case VAL_UINT16:    return ((const uint16_t*)items)[i];
    case VAL_UINT32:    return ((const uint32_t*)items)[i];
    case VAL_UINT:      return ((const unsigned int*)items)[i];
    case VAL_UINT64:    return ((const uint64_t*)items)[i];
    case VAL_ULONG:     return ((const unsigned long*)items)[i];
    case VAL_UCHAR:     return ((const unsigned char*)items)[i];
    case VAL_SHORT:     return ((const short*)items)[i];
    case VAL_USHORT:    return ((const unsigned short*)items)[i];
    case VAL_SIZE_T:    return (int64_t)((const size_t*)items)[i];
    case VAL_SSIZE_T:   return (int64_t)((const ssize_t*)items)[i];
    case VAL_BOOL:      return ((const _Bool*)items)[i];
    case VAL_CHAR:      return ((const char*)items)[i];
    default:            return 0;
    }
}

/* 整型族：按元素类型把 int64 截断写入裸 items[i] */
static void typed_write_i64(ValueType et, void* items, int i, int64_t v) {
    switch(et) {
    case VAL_INT:       ((int*)items)[i] = (int)v; break;
    case VAL_INT8:      ((int8_t*)items)[i] = (int8_t)v; break;
    case VAL_INT16:     ((int16_t*)items)[i] = (int16_t)v; break;
    case VAL_INT32:     ((int32_t*)items)[i] = (int32_t)v; break;
    case VAL_INT64:     ((int64_t*)items)[i] = v; break;
    case VAL_LONG_LONG: ((long long*)items)[i] = (long long)v; break;
    case VAL_LONG:      ((long*)items)[i] = (long)v; break;
    case VAL_UINT8:     ((uint8_t*)items)[i] = (uint8_t)v; break;
    case VAL_UINT16:    ((uint16_t*)items)[i] = (uint16_t)v; break;
    case VAL_UINT32:    ((uint32_t*)items)[i] = (uint32_t)v; break;
    case VAL_UINT:      ((unsigned int*)items)[i] = (unsigned int)v; break;
    case VAL_UINT64:    ((uint64_t*)items)[i] = (uint64_t)v; break;
    case VAL_ULONG:     ((unsigned long*)items)[i] = (unsigned long)v; break;
    case VAL_UCHAR:     ((unsigned char*)items)[i] = (unsigned char)v; break;
    case VAL_SHORT:     ((short*)items)[i] = (short)v; break;
    case VAL_USHORT:    ((unsigned short*)items)[i] = (unsigned short)v; break;
    case VAL_SIZE_T:    ((size_t*)items)[i] = (size_t)v; break;
    case VAL_SSIZE_T:   ((ssize_t*)items)[i] = (ssize_t)v; break;
    case VAL_BOOL:      ((_Bool*)items)[i] = (_Bool)v; break;
    case VAL_CHAR:      ((char*)items)[i] = (char)v; break;
    default: break;
    }
}

/* 任意 Value → int64（整型族下标写，浮点截断） */
static int64_t value_to_i64_all(Value v) {
    switch(v.type) {
    case VAL_FLOAT:        return (int64_t)v.v.f;
    case VAL_LONG_DOUBLE:  return (int64_t)v.v.ld;
    case VAL_BOOL:         return (int64_t)v.v.b;
    case VAL_CHAR:         return (int64_t)v.v.c;
    case VAL_BYTE:         return (int64_t)v.v.by;
    case VAL_UINT8:        return (int64_t)v.v.u8;
    case VAL_UCHAR:        return (int64_t)v.v.uc;
    case VAL_UINT16:       return (int64_t)v.v.u16;
    case VAL_USHORT:       return (int64_t)v.v.us;
    case VAL_ULONG:        return (int64_t)v.v.ul;
    case VAL_SIZE_T:       return (int64_t)v.v.st;
    case VAL_SSIZE_T:      return (int64_t)v.v.sst;
    default:               return value_to_index(v);
    }
}

/* 任意 Value → double（浮点族下标写） */
static double value_to_dbl_all(Value v) {
    switch(v.type) {
    case VAL_DOUBLE:       return v.v.d;
    case VAL_FLOAT:        return (double)v.v.f;
    case VAL_LONG_DOUBLE:  return (double)v.v.ld;
    default:               return (double)value_to_i64_all(v);
    }
}

/* 任意 Value → 元素指针（PTR 族下标写）；SSO 字符串复制为 GC 堆字符串 */
static void* value_to_typed_ptr(Value v) {
    switch(v.type) {
    case VAL_STRING:
        if(v.str_inline) {
            char* p = (char*)gc_alloc((size_t)v.v.sso.len + 1, VAL_STRING);
            memcpy(p, v.v.sso.data, (size_t)v.v.sso.len + 1);
            return p;
        }
        return v.v.s;
    case VAL_PTR:        return v.v.struct_ptr;
    case VAL_BIGINT:     return v.v.bigint;
    case VAL_DECIMAL:    return v.v.decimal;
    case VAL_BITDECIMAL: return v.v.bitdecimal;
    default: {
        /* 数值写入 string 数组：转字符串表示，复制为 GC 字符串 */
        char buf[40];
        int n;
        if(v.type == VAL_DOUBLE || v.type == VAL_FLOAT || v.type == VAL_LONG_DOUBLE)
            n = snprintf(buf, sizeof(buf), "%g", value_to_dbl_all(v));
        else
            n = snprintf(buf, sizeof(buf), "%lld", (long long)value_to_i64_all(v));
        if(n < 0) n = 0;
        char* p = (char*)gc_alloc((size_t)n + 1, VAL_STRING);
        memcpy(p, buf, (size_t)n + 1);
        return p;
    }
    }
}

/* 从裸 items[i] 按 elem_type 装箱为 Value */
static Value typed_box_elem(ValueType et, void* items, int i) {
    Value r = val_none();
    int cls = lumyr_etype_stackcls(et);
    if(cls == 1) {
        r.type = et;
        r.v.ll = typed_read_i64(et, items, i);
    } else if(cls == 2) {
        r.type = et;
        if(et == VAL_DOUBLE)      r.v.d = ((const double*)items)[i];
        else if(et == VAL_FLOAT)  r.v.f = ((const float*)items)[i];
        else                      r.v.ld = ((const long double*)items)[i];
    } else if(cls == 3) {
        r.type = et;
        r.str_inline = 0;
        r.v.s = ((char**)items)[i];
    }
    return r;
}

/* INDEX_GET：弹 idx、arr（VALUE 栈），取 arr[idx]，压元素（越界/非数组→none） */
int vm_exec_index_get(VMExecCtx* ctx, Instruction* in) {
    (void)ctx; (void)in;
    Value idx; stack_vm_pop(g_stack_mgr, STACK_VALUE, &idx);
    Value arr; stack_vm_pop(g_stack_mgr, STACK_VALUE, &arr);
    Value r = val_none();
    if(arr.type == VAL_ARRAY) {
        int64_t i = value_to_index(idx);
        if(i >= 0 && i < (int64_t)arr.v.array->len)
            r = arr.v.array->items[i];
    } else if(arr.type == VAL_MAP) {
        r = lumyr_map_get(arr, idx);
    } else if(arr.type == VAL_TYPED_ARRAY) {
        TypedArray* ta = arr.v.typed_array;
        if(typed_ptr_ok(ta)) {
            int64_t i = value_to_index(idx);
            if(i >= 0 && i < (int64_t)ta->len && ta->items)
                r = typed_box_elem(ta->elem_type, ta->items, (int)i);
        }
    }
    stack_vm_push(g_stack_mgr, STACK_VALUE, &r);
    return 1;
}

/* INDEX_SET：弹 val,idx,arr（栈顶为 val），写入数组/字典，压回 val（表达式值） */
int vm_exec_index_set(VMExecCtx* ctx, Instruction* in) {
    (void)ctx; (void)in;
    Value val; stack_vm_pop(g_stack_mgr, STACK_VALUE, &val);
    Value idx; stack_vm_pop(g_stack_mgr, STACK_VALUE, &idx);
    Value arr; stack_vm_pop(g_stack_mgr, STACK_VALUE, &arr);
    if(arr.type == VAL_ARRAY) {
        int64_t i = value_to_index(idx);
        if(i >= 0 && i < (int64_t)arr.v.array->len)
            arr.v.array->items[i] = val;
    } else if(arr.type == VAL_MAP) {
        lumyr_map_set(&arr, idx, val);
    } else if(arr.type == VAL_TYPED_ARRAY && typed_ptr_ok(arr.v.typed_array)) {
        TypedArray* ta = arr.v.typed_array;
        int64_t i = value_to_index(idx);
        if(i >= 0 && i < (int64_t)ta->len && ta->items) {
            ValueType et = ta->elem_type;
            int cls = lumyr_etype_stackcls(et);
            if(cls == 1) {
                typed_write_i64(et, ta->items, (int)i, value_to_i64_all(val));
            } else if(cls == 2) {
                double v = value_to_dbl_all(val);
                if(et == VAL_DOUBLE)          ((double*)ta->items)[i] = v;
                else if(et == VAL_FLOAT)      ((float*)ta->items)[i] = (float)v;
                else                          ((long double*)ta->items)[i] = (long double)v;
            } else {
                ((void**)ta->items)[i] = value_to_typed_ptr(val);
            }
        }
    }
    stack_vm_push(g_stack_mgr, STACK_VALUE, &val);
    return 1;
}

/* TYPED_ARRAY_LIT：从元素所属 typed 栈逆序弹 a 个元素（in.b=elem ValueType），
 * 构造 TypedArray 压 VALUE 栈。元素裸存储，宽度按 elem_type。 */
int vm_exec_typed_array_lit(VMExecCtx* ctx, Instruction* in) {
    (void)ctx;
    int n = in->a;
    ValueType et = (ValueType)in->b;
    Value r;
    memset(&r, 0, sizeof(r));
    r.type = VAL_TYPED_ARRAY;
    gc_disable();
    TypedArray* ta = (TypedArray*)gc_alloc(sizeof(TypedArray), VAL_TYPED_ARRAY);
    ta->elem_type = et;
    ta->stack_alloc = 0;
    ta->len = n;
    ta->cap = n > 0 ? n : 8;
    if(n > 0) {
        size_t isz = lumyr_etype_itemsz(et);
        ta->items = gc_alloc_old(isz * (size_t)ta->cap, VAL_TYPED_ARRAY);
        gc_mark_internal_buf(ta->items);
        int cls = lumyr_etype_stackcls(et);
        for(int i = n - 1; i >= 0; --i) {
            if(cls == 1) {
                int64_t v; stack_vm_pop(g_stack_mgr, STACK_INT64, &v);
                typed_write_i64(et, ta->items, i, v);
            } else if(cls == 2) {
                double v; stack_vm_pop(g_stack_mgr, STACK_DOUBLE, &v);
                if(et == VAL_DOUBLE)          ((double*)ta->items)[i] = v;
                else if(et == VAL_FLOAT)      ((float*)ta->items)[i] = (float)v;
                else                          ((long double*)ta->items)[i] = (long double)v;
            } else {
                void* p; stack_vm_pop(g_stack_mgr, STACK_PTR, &p);
                /* 字符串元素统一复制为 GC 所有（源可能是 malloc 的数字转串），
                 * bigint/decimal 等保持原 malloc 所有权 */
                if(et == VAL_STRING && p) {
                    size_t l = strlen((const char*)p);
                    char* np = (char*)gc_alloc(l + 1, VAL_STRING);
                    memcpy(np, p, l + 1);
                    p = np;
                }
                ((void**)ta->items)[i] = p;
            }
        }
    } else {
        ta->items = NULL;
    }
    r.v.typed_array = ta;
    gc_enable();
    stack_vm_push(g_stack_mgr, STACK_VALUE, &r);
    return 1;
}

/* TYPED_INDEX_SET：弹 val,idx,arr，按 arr.elem_type 转换写入裸 items，压回 val */
int vm_exec_typed_index_set(VMExecCtx* ctx, Instruction* in) {
    (void)ctx; (void)in;
    Value val; stack_vm_pop(g_stack_mgr, STACK_VALUE, &val);
    Value idx; stack_vm_pop(g_stack_mgr, STACK_VALUE, &idx);
    Value arr; stack_vm_pop(g_stack_mgr, STACK_VALUE, &arr);
    if(arr.type == VAL_TYPED_ARRAY && typed_ptr_ok(arr.v.typed_array)) {
        TypedArray* ta = arr.v.typed_array;
        int64_t i = value_to_index(idx);
        if(i >= 0 && i < (int64_t)ta->len && ta->items) {
            ValueType et = ta->elem_type;
            int cls = lumyr_etype_stackcls(et);
            if(cls == 1) {
                typed_write_i64(et, ta->items, (int)i, value_to_i64_all(val));
            } else if(cls == 2) {
                double v = value_to_dbl_all(val);
                if(et == VAL_DOUBLE)          ((double*)ta->items)[i] = v;
                else if(et == VAL_FLOAT)      ((float*)ta->items)[i] = (float)v;
                else                          ((long double*)ta->items)[i] = (long double)v;
            } else {
                ((void**)ta->items)[i] = value_to_typed_ptr(val);
            }
        }
    }
    stack_vm_push(g_stack_mgr, STACK_VALUE, &val);
    return 1;
}

/* MAP_LIT：弹 2b 个 VALUE（键、值交替，栈顶为最后一个值），构造字典压 VALUE 栈 */
int vm_exec_map_lit(VMExecCtx* ctx, Instruction* in) {
    (void)ctx;
    int pairs = in->b;
    Value r = val_map();
    for(int p = pairs - 1; p >= 0; --p) {
        Value val; stack_vm_pop(g_stack_mgr, STACK_VALUE, &val);
        Value key; stack_vm_pop(g_stack_mgr, STACK_VALUE, &key);
        lumyr_map_set(&r, key, val);
    }
    stack_vm_push(g_stack_mgr, STACK_VALUE, &r);
    return 1;
}
