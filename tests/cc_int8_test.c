#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stddef.h>
#include "lm_runtime.h"
#include "lm_map.h"
#include "lm_thread.h"
#include "lm_lock.h"
#include "lm_tls.h"
#include "lm_http.h"
#include "lm_json.h"
#include "lm_charset.h"
#include "lm_crypto.h"
#include "lm_regex.h"
#include "lm_time.h"
#include "lm_qs.h"
#include "lumyr_value.h"
#include "lm_class.h"
#include "lm_struct.h"

static Value __g_gen_send_val = {0};
static int __g_gen_in_generator = 0;


__attribute__((weak)) void lumyr_interp_scan_captures(const RuntimeFunc* rf, void (*mark)(Value)) { (void)rf; (void)mark; }

/* ===== 生成器组合操作运行时支持 ===== */
typedef enum { WRAP_NONE=0, WRAP_MAP=1, WRAP_FILTER=2, WRAP_SKIP=3, WRAP_TAKE=4, WRAP_ENUMERATE=5, WRAP_CHAIN=6, WRAP_ZIP=7 } WrapType;
typedef struct lumyr_gen_wrap {
    Value (*next)(void*, Value);
    int __state;
    int is_wrapped;
    int wrap_type;
    void* wrapped_gen;
    void* wrapped_gen2;
    Value wrap_fn;
    int wrap_arg;
    int wrap_index;
} lumyr_gen_wrap;

static Value lumyr_gen_wrap_next(void* __gptr, Value __send_val) {
    lumyr_gen_wrap* g = (lumyr_gen_wrap*)__gptr;
    if(g->__state == -1) return val_none();
    Value (*wnext)(void*, Value) = *(Value(**)(void*,Value))g->wrapped_gen;
    switch(g->wrap_type) {
        case WRAP_MAP: {
            Value v = wnext(g->wrapped_gen, val_none());
            if(v.type == VAL_NONE) { g->__state = -1; return val_none(); }
            Value args[1]; args[0] = v;
            Value (*fn)(Value*,int) = (Value(*)(Value*,int))((RuntimeFunc*)g->wrap_fn.v.func.func_obj)->entry;
            return fn(args, 1);
        }
        case WRAP_FILTER: {
            while(1) {
                Value v = wnext(g->wrapped_gen, val_none());
                if(v.type == VAL_NONE) { g->__state = -1; return val_none(); }
                Value args[1]; args[0] = v;
                Value (*fn)(Value*,int) = (Value(*)(Value*,int))((RuntimeFunc*)g->wrap_fn.v.func.func_obj)->entry;
                Value r = fn(args, 1);
                if(lumyr_to_bool(r)) return v;
            }
        }
        case WRAP_SKIP: {
            while(g->wrap_index < g->wrap_arg) {
                Value v = wnext(g->wrapped_gen, val_none());
                if(v.type == VAL_NONE) { g->__state = -1; return val_none(); }
                g->wrap_index++;
            }
            Value v = wnext(g->wrapped_gen, val_none());
            if(v.type == VAL_NONE) { g->__state = -1; return val_none(); }
            return v;
        }
        case WRAP_TAKE: {
            if(g->wrap_index >= g->wrap_arg) { g->__state = -1; return val_none(); }
            Value v = wnext(g->wrapped_gen, val_none());
            if(v.type == VAL_NONE) { g->__state = -1; return val_none(); }
            g->wrap_index++;
            return v;
        }
        case WRAP_ENUMERATE: {
            Value v = wnext(g->wrapped_gen, val_none());
            if(v.type == VAL_NONE) { g->__state = -1; return val_none(); }
            Value arr = val_array(2);
            arr.v.array->items[0] = lumyr_make_int(g->wrap_index);
            arr.v.array->items[1] = v;
            g->wrap_index++;
            return arr;
        }
        case WRAP_CHAIN: {
            if(g->wrap_index == 0) {
                Value v = wnext(g->wrapped_gen, val_none());
                if(v.type != VAL_NONE) return v;
                g->wrap_index = 1;
            }
            Value (*wnext2)(void*,Value) = *(Value(**)(void*,Value))g->wrapped_gen2;
            Value v = wnext2(g->wrapped_gen2, val_none());
            if(v.type == VAL_NONE) { g->__state = -1; return val_none(); }
            return v;
        }
        case WRAP_ZIP: {
            Value (*wnext2)(void*,Value) = *(Value(**)(void*,Value))g->wrapped_gen2;
            Value v1 = wnext(g->wrapped_gen, val_none());
            Value v2 = wnext2(g->wrapped_gen2, val_none());
            if(v1.type == VAL_NONE || v2.type == VAL_NONE) { g->__state = -1; return val_none(); }
            Value arr = val_array(2);
            arr.v.array->items[0] = v1;
            arr.v.array->items[1] = v2;
            return arr;
        }
        default: g->__state = -1; return val_none();
    }
}

static Value lumyr_wrap_create(int wtype, Value g1, Value g2, Value fn, int arg) {
    if(g1.type != VAL_GENERATOR) runtime_error("组合操作第一个参数必须是生成器");
    lumyr_gen_wrap* wg = (lumyr_gen_wrap*)calloc(1, sizeof(lumyr_gen_wrap));
    wg->next = lumyr_gen_wrap_next;
    wg->is_wrapped = 1;
    wg->wrap_type = wtype;
    wg->wrapped_gen = g1.v.generator;
    if(g2.type == VAL_GENERATOR) wg->wrapped_gen2 = g2.v.generator;
    wg->wrap_fn = fn;
    wg->wrap_arg = arg;
    wg->wrap_index = 0;
    Value gv; gv.type = VAL_GENERATOR; gv.v.generator = (void*)wg;
    return gv;
}

/* ===== 生成器组合操作支持结束 ===== */

/* 通用 vtable 类型：class 方法虚函数表 */
typedef struct {
    const char* class_name;  /* class 名，用于运行时获取 class 名 */
    void* methods[64];       /* 方法函数指针数组，最多 64 个方法 */
} lumyr_vtable;

static char lmvar_a = 0;
static char lmvar_b = 0;
static Value lmvar_arr = {0};

/* class vtable 实例（虚函数表，按全局方法索引填充） */


int main(void){
    Value __stk[5];
    int __sp = 0;
    /* int 类型专用栈（零开销优化） */
    int __int_stack[5];
    int __int_sp = 0;
    /* int8 类型专用栈（零开销优化） */
    int8_t __int8_stack[5];
    int __int8_sp = 0;
    /* int16 类型专用栈（零开销优化） */
    int16_t __int16_stack[5];
    int __int16_sp = 0;
    /* int32 类型专用栈（零开销优化） */
    int32_t __int32_stack[5];
    int __int32_sp = 0;
    /* int64 类型专用栈（零开销优化） */
    int64_t __int64_stack[5];
    int __int64_sp = 0;
    /* 注册 class 字段信息表到运行时红黑树 */

    /* 注册 struct 字段信息表到运行时红黑树 */

    int __g_d0 = __g_depth; jmp_buf* __g_gj0 = g_err_jmp; int __g_fin0 = __g_fin_n;
    volatile Value* __local_ptrs[1] = { &lmvar_arr };
    CFrame __frame;
    __frame.stack = __stk;
    __frame.sp = &__sp;
    __frame.stack_size = 5;
    __frame.local_ptrs = (Value**)__local_ptrs;
    __frame.nlocals = 1;
    gc_push_cframe(&__frame);
    gc_stw_check_fast();
    __stk[__sp++] = lumyr_make_string("=== CC模式 int8 类型测试 ===\\n");
    { int __pcnt = 1; if(__pcnt <= 0) __pcnt = 1; int __pbase = __sp - __pcnt;
      for(int __pi = 0; __pi < __pcnt; __pi++) { if(__pi > 0) printf(" "); lumyr_print_inline(__stk[__pbase + __pi]); }
      printf("\n"); __sp -= __pcnt; }
    __stk[__sp++] = lumyr_make_int8(42);
    { Value __v = __stk[--__sp]; lmvar_a = (char)((__v).type == VAL_DOUBLE ? (long long)(__v).v.d : (__v).v.i); __stk[__sp++] = __v; }
    __sp--;
    __stk[__sp++] = lumyr_make_string("a = ");
    { int __pcnt = 1; if(__pcnt <= 0) __pcnt = 1; int __pbase = __sp - __pcnt;
      for(int __pi = 0; __pi < __pcnt; __pi++) { if(__pi > 0) printf(" "); lumyr_print_inline(__stk[__pbase + __pi]); }
      printf("\n"); __sp -= __pcnt; }
    gc_stw_check_fast();
    __stk[__sp++] = lumyr_make_int((long long)(char)lmvar_a);
    { int __pcnt = 1; if(__pcnt <= 0) __pcnt = 1; int __pbase = __sp - __pcnt;
      for(int __pi = 0; __pi < __pcnt; __pi++) { if(__pi > 0) printf(" "); lumyr_print_inline(__stk[__pbase + __pi]); }
      printf("\n"); __sp -= __pcnt; }
    __stk[__sp++] = lumyr_make_string("\\n");
    { int __pcnt = 1; if(__pcnt <= 0) __pcnt = 1; int __pbase = __sp - __pcnt;
      for(int __pi = 0; __pi < __pcnt; __pi++) { if(__pi > 0) printf(" "); lumyr_print_inline(__stk[__pbase + __pi]); }
      printf("\n"); __sp -= __pcnt; }
    __stk[__sp++] = lumyr_make_int8(100);
    { Value __v = __stk[--__sp]; lmvar_b = (char)((__v).type == VAL_DOUBLE ? (long long)(__v).v.d : (__v).v.i); __stk[__sp++] = __v; }
    __sp--;
    __stk[__sp++] = lumyr_make_string("b = ");
    gc_stw_check_fast();
    { int __pcnt = 1; if(__pcnt <= 0) __pcnt = 1; int __pbase = __sp - __pcnt;
      for(int __pi = 0; __pi < __pcnt; __pi++) { if(__pi > 0) printf(" "); lumyr_print_inline(__stk[__pbase + __pi]); }
      printf("\n"); __sp -= __pcnt; }
    __stk[__sp++] = lumyr_make_int((long long)(char)lmvar_b);
    { int __pcnt = 1; if(__pcnt <= 0) __pcnt = 1; int __pbase = __sp - __pcnt;
      for(int __pi = 0; __pi < __pcnt; __pi++) { if(__pi > 0) printf(" "); lumyr_print_inline(__stk[__pbase + __pi]); }
      printf("\n"); __sp -= __pcnt; }
    __stk[__sp++] = lumyr_make_string("\\n");
    { int __pcnt = 1; if(__pcnt <= 0) __pcnt = 1; int __pbase = __sp - __pcnt;
      for(int __pi = 0; __pi < __pcnt; __pi++) { if(__pi > 0) printf(" "); lumyr_print_inline(__stk[__pbase + __pi]); }
      printf("\n"); __sp -= __pcnt; }
    __stk[__sp++] = lumyr_make_int(1);
    __stk[__sp++] = lumyr_make_int(2);
    __stk[__sp++] = lumyr_make_int(3);
    gc_stw_check_fast();
    { int __n = 3; Value __arr = val_int8_array(__n); TypedArray* __tarr = __arr.v.typed_array; int8_t* __iitems = (int8_t*)__tarr->items;
      for(int __k = 0; __k < __n; __k++) { Value __v = __stk[__sp - __n + __k]; __iitems[__k] = (int8_t)lumyr_extract_int(__v); }
      __sp = __sp - __n + 1; __sp--; __stk[__sp++] = __arr;
      __tarr->len = __n;
    }
    { Value __v = __stk[--__sp]; lmvar_arr = __v; __stk[__sp++] = __v; }
    __sp--;
    __stk[__sp++] = lumyr_make_string("arr[0] = ");
    { int __pcnt = 1; if(__pcnt <= 0) __pcnt = 1; int __pbase = __sp - __pcnt;
      for(int __pi = 0; __pi < __pcnt; __pi++) { if(__pi > 0) printf(" "); lumyr_print_inline(__stk[__pbase + __pi]); }
      printf("\n"); __sp -= __pcnt; }
    __stk[__sp++] = lmvar_arr;
    __stk[__sp++] = lumyr_make_int(0);
    { Value __c = __stk[__sp-2], __idx = __stk[__sp-1]; __stk[__sp-2] = lumyr_index_get(__c, __idx); __sp--; }
    gc_stw_check_fast();
    { int __pcnt = 1; if(__pcnt <= 0) __pcnt = 1; int __pbase = __sp - __pcnt;
      for(int __pi = 0; __pi < __pcnt; __pi++) { if(__pi > 0) printf(" "); lumyr_print_inline(__stk[__pbase + __pi]); }
      printf("\n"); __sp -= __pcnt; }
    __stk[__sp++] = lumyr_make_string("\\n");
    { int __pcnt = 1; if(__pcnt <= 0) __pcnt = 1; int __pbase = __sp - __pcnt;
      for(int __pi = 0; __pi < __pcnt; __pi++) { if(__pi > 0) printf(" "); lumyr_print_inline(__stk[__pbase + __pi]); }
      printf("\n"); __sp -= __pcnt; }
    __stk[__sp++] = lumyr_make_string("arr[1] = ");
    { int __pcnt = 1; if(__pcnt <= 0) __pcnt = 1; int __pbase = __sp - __pcnt;
      for(int __pi = 0; __pi < __pcnt; __pi++) { if(__pi > 0) printf(" "); lumyr_print_inline(__stk[__pbase + __pi]); }
      printf("\n"); __sp -= __pcnt; }
    __stk[__sp++] = lmvar_arr;
    __stk[__sp++] = lumyr_make_int(1);
    { Value __c = __stk[__sp-2], __idx = __stk[__sp-1]; __stk[__sp-2] = lumyr_index_get(__c, __idx); __sp--; }
    gc_stw_check_fast();
    { int __pcnt = 1; if(__pcnt <= 0) __pcnt = 1; int __pbase = __sp - __pcnt;
      for(int __pi = 0; __pi < __pcnt; __pi++) { if(__pi > 0) printf(" "); lumyr_print_inline(__stk[__pbase + __pi]); }
      printf("\n"); __sp -= __pcnt; }
    __stk[__sp++] = lumyr_make_string("\\n");
    { int __pcnt = 1; if(__pcnt <= 0) __pcnt = 1; int __pbase = __sp - __pcnt;
      for(int __pi = 0; __pi < __pcnt; __pi++) { if(__pi > 0) printf(" "); lumyr_print_inline(__stk[__pbase + __pi]); }
      printf("\n"); __sp -= __pcnt; }
    __stk[__sp++] = lumyr_make_string("arr[2] = ");
    { int __pcnt = 1; if(__pcnt <= 0) __pcnt = 1; int __pbase = __sp - __pcnt;
      for(int __pi = 0; __pi < __pcnt; __pi++) { if(__pi > 0) printf(" "); lumyr_print_inline(__stk[__pbase + __pi]); }
      printf("\n"); __sp -= __pcnt; }
    __stk[__sp++] = lmvar_arr;
    __stk[__sp++] = lumyr_make_int(2);
    { Value __c = __stk[__sp-2], __idx = __stk[__sp-1]; __stk[__sp-2] = lumyr_index_get(__c, __idx); __sp--; }
    gc_stw_check_fast();
    { int __pcnt = 1; if(__pcnt <= 0) __pcnt = 1; int __pbase = __sp - __pcnt;
      for(int __pi = 0; __pi < __pcnt; __pi++) { if(__pi > 0) printf(" "); lumyr_print_inline(__stk[__pbase + __pi]); }
      printf("\n"); __sp -= __pcnt; }
    __stk[__sp++] = lumyr_make_string("\\n");
    { int __pcnt = 1; if(__pcnt <= 0) __pcnt = 1; int __pbase = __sp - __pcnt;
      for(int __pi = 0; __pi < __pcnt; __pi++) { if(__pi > 0) printf(" "); lumyr_print_inline(__stk[__pbase + __pi]); }
      printf("\n"); __sp -= __pcnt; }
    __stk[__sp++] = lumyr_make_string("=== 测试完成 ===\\n");
    { int __pcnt = 1; if(__pcnt <= 0) __pcnt = 1; int __pbase = __sp - __pcnt;
      for(int __pi = 0; __pi < __pcnt; __pi++) { if(__pi > 0) printf(" "); lumyr_print_inline(__stk[__pbase + __pi]); }
      printf("\n"); __sp -= __pcnt; }
    gc_pop_cframe();
    return 0;
}

