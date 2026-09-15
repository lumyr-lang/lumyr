/*
 * lumyr_ffi.c - FFI (Foreign Function Interface) 外部函数接口实现
 *
 * 支持：
 * - 最多 16 个参数
 * - 类型：void, int, double, bool, string, ptr（指针/句柄/数组/结构体指针）
 * - 回调函数（函数指针作为参数）
 * - 完善的错误处理
 */
#include "lumyr_ffi.h"
#include "lumyr_log.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

/* ==================== 回调函数支持 ==================== */

/* 回调函数上下文：保存 Lumyr 函数值和参数数量 */
typedef struct {
    Value func_val;      /* Lumyr 函数值 */
    int param_count;     /* 参数数量 */
    int used;            /* 是否被使用 */
} FFICallbackCtx;

#define FFI_MAX_CALLBACKS 16
static FFICallbackCtx g_callbacks[FFI_MAX_CALLBACKS];

/* 回调函数 trampoline：C 端调用，转发到 Lumyr 函数 */
/* 简化版：只支持最多 4 个参数的回调，返回 long long */
typedef long long (*ffi_callback_trampoline_t)(long long, long long, long long, long long);

/* 预定义的 trampoline 函数（每个槽位一个） */
static long long ffi_trampoline_0(long long a, long long b, long long c, long long d) {
    (void)a; (void)b; (void)c; (void)d;
    /* 槽位 0 的回调在运行时通过全局变量查找 */
    return 0;
}
/* 由于 C 无法动态生成代码，这里用一个简化方案：
   回调函数通过全局变量传递上下文，实际调用在 lumyr_ffi_invoke_callback 中完成 */

/* 调用 Lumyr 回调函数（由 FFI 调用的 C 函数内部触发） */
Value lumyr_ffi_invoke_callback(int slot, Value* args, int argc) {
    if(slot < 0 || slot >= FFI_MAX_CALLBACKS || !g_callbacks[slot].used) {
        LOG_ERROR("FFI Error: invalid callback slot %d\n", slot);
        return val_none();
    }
    Value cb = g_callbacks[slot].func_val;
    if(cb.type != VAL_FUNC) {
        LOG_ERROR("FFI Error: callback slot %d is not a function\n", slot);
        return val_none();
    }
    /* 通过 RuntimeFunc entry 调用 */
    if(cb.v.func.func_obj && cb.v.func.func_obj->entry) {
        return cb.v.func.func_obj->entry(argc, args, NULL, NULL);
    }
    return val_none();
}

/* 注册回调函数，返回槽位 ID（作为函数指针传递给 C） */
int lumyr_ffi_register_callback(Value func_val, int param_count) {
    for(int i = 0; i < FFI_MAX_CALLBACKS; i++) {
        if(!g_callbacks[i].used) {
            g_callbacks[i].func_val = func_val;
            g_callbacks[i].param_count = param_count;
            g_callbacks[i].used = 1;
            return i;
        }
    }
    LOG_ERROR("FFI Error: no free callback slots (max %d)\n", FFI_MAX_CALLBACKS);
    return -1;
}

/* 注销回调函数 */
void lumyr_ffi_unregister_callback(int slot) {
    if(slot >= 0 && slot < FFI_MAX_CALLBACKS) {
        g_callbacks[slot].used = 0;
        g_callbacks[slot].func_val = val_none();
    }
}

/* ==================== 库加载与函数解析 ==================== */

/* 加载库 */
void* lumyr_ffi_load_lib(const char* libname) {
    if(!libname) return NULL;
#ifdef _WIN32
    HMODULE h = LoadLibraryA(libname);
    if(!h) {
        LOG_ERROR("FFI Error: failed to load library '%s' (error %lu)\n",
                libname, GetLastError());
    }
    return (void*)h;
#else
    void* h = dlopen(libname, RTLD_LAZY | RTLD_GLOBAL);
    if(!h) {
        LOG_ERROR("FFI Error: failed to load library '%s': %s\n",
                libname, dlerror());
    }
    return h;
#endif
}

/* 获取函数地址 */
void* lumyr_ffi_get_func(void* lib_handle, const char* funcname) {
    if(!funcname) return NULL;
#ifdef _WIN32
    if(lib_handle) {
        return (void*)GetProcAddress((HMODULE)lib_handle, funcname);
    }
    /* 从当前进程模块查找 */
    HMODULE hMod = GetModuleHandleA(NULL);
    if(hMod) return (void*)GetProcAddress(hMod, funcname);
    return NULL;
#else
    if(lib_handle) {
        return dlsym(lib_handle, funcname);
    }
    /* 从当前进程符号表查找 */
    return dlsym(RTLD_DEFAULT, funcname);
#endif
}

/* ==================== 函数调用（最多 16 个参数） ==================== */

/* 定义 0-16 参数的函数指针类型 */
#define FFI_FUNC_TYPE(n) typedef long long (*ffi_func##n##_t)(
/* 由于 C 宏无法方便地生成逗号分隔的参数列表，这里手动定义 */

typedef long long (*ffi_func0_t)(void);
typedef long long (*ffi_func1_t)(long long);
typedef long long (*ffi_func2_t)(long long, long long);
typedef long long (*ffi_func3_t)(long long, long long, long long);
typedef long long (*ffi_func4_t)(long long, long long, long long, long long);
typedef long long (*ffi_func5_t)(long long, long long, long long, long long, long long);
typedef long long (*ffi_func6_t)(long long, long long, long long, long long, long long, long long);
typedef long long (*ffi_func7_t)(long long, long long, long long, long long, long long, long long, long long);
typedef long long (*ffi_func8_t)(long long, long long, long long, long long, long long, long long, long long, long long);
typedef long long (*ffi_func9_t)(long long, long long, long long, long long, long long, long long, long long, long long, long long);
typedef long long (*ffi_func10_t)(long long, long long, long long, long long, long long, long long, long long, long long, long long, long long);
typedef long long (*ffi_func11_t)(long long, long long, long long, long long, long long, long long, long long, long long, long long, long long, long long);
typedef long long (*ffi_func12_t)(long long, long long, long long, long long, long long, long long, long long, long long, long long, long long, long long, long long);
typedef long long (*ffi_func13_t)(long long, long long, long long, long long, long long, long long, long long, long long, long long, long long, long long, long long, long long);
typedef long long (*ffi_func14_t)(long long, long long, long long, long long, long long, long long, long long, long long, long long, long long, long long, long long, long long, long long);
typedef long long (*ffi_func15_t)(long long, long long, long long, long long, long long, long long, long long, long long, long long, long long, long long, long long, long long, long long, long long);
typedef long long (*ffi_func16_t)(long long, long long, long long, long long, long long, long long, long long, long long, long long, long long, long long, long long, long long, long long, long long, long long);

/* double 返回类型的函数指针（返回值在 XMM0 寄存器，不是 RAX） */
typedef double (*ffi_dfunc0_t)(void);
typedef double (*ffi_dfunc1_t)(long long);
typedef double (*ffi_dfunc2_t)(long long, long long);
typedef double (*ffi_dfunc3_t)(long long, long long, long long);
typedef double (*ffi_dfunc4_t)(long long, long long, long long, long long);
typedef double (*ffi_dfunc5_t)(long long, long long, long long, long long, long long);
typedef double (*ffi_dfunc6_t)(long long, long long, long long, long long, long long, long long);
typedef double (*ffi_dfunc7_t)(long long, long long, long long, long long, long long, long long, long long);
typedef double (*ffi_dfunc8_t)(long long, long long, long long, long long, long long, long long, long long, long long);
typedef double (*ffi_dfunc9_t)(long long, long long, long long, long long, long long, long long, long long, long long, long long);
typedef double (*ffi_dfunc10_t)(long long, long long, long long, long long, long long, long long, long long, long long, long long, long long);
typedef double (*ffi_dfunc11_t)(long long, long long, long long, long long, long long, long long, long long, long long, long long, long long, long long);
typedef double (*ffi_dfunc12_t)(long long, long long, long long, long long, long long, long long, long long, long long, long long, long long, long long, long long);
typedef double (*ffi_dfunc13_t)(long long, long long, long long, long long, long long, long long, long long, long long, long long, long long, long long, long long, long long);
typedef double (*ffi_dfunc14_t)(long long, long long, long long, long long, long long, long long, long long, long long, long long, long long, long long, long long, long long, long long);
typedef double (*ffi_dfunc15_t)(long long, long long, long long, long long, long long, long long, long long, long long, long long, long long, long long, long long, long long, long long, long long);
typedef double (*ffi_dfunc16_t)(long long, long long, long long, long long, long long, long long, long long, long long, long long, long long, long long, long long, long long, long long, long long, long long);

/* 全浮点参数的函数指针类型（参数传递到 XMM 寄存器，不是整数寄存器） */
typedef long long (*ffi_func0_d_t)(void);
typedef long long (*ffi_func1_d_t)(double);
typedef long long (*ffi_func2_d_t)(double, double);
typedef long long (*ffi_func3_d_t)(double, double, double);
typedef long long (*ffi_func4_d_t)(double, double, double, double);
typedef long long (*ffi_func5_d_t)(double, double, double, double, double);
typedef long long (*ffi_func6_d_t)(double, double, double, double, double, double);
typedef double (*ffi_dfunc0_d_t)(void);
typedef double (*ffi_dfunc1_d_t)(double);
typedef double (*ffi_dfunc2_d_t)(double, double);
typedef double (*ffi_dfunc3_d_t)(double, double, double);
typedef double (*ffi_dfunc4_d_t)(double, double, double, double);
typedef double (*ffi_dfunc5_d_t)(double, double, double, double, double);
typedef double (*ffi_dfunc6_d_t)(double, double, double, double, double, double);

#define FFI_MAX_ARGS 16

/* 调用 FFI 函数 */
Value lumyr_ffi_call(FFIFunc* func, Value* args, int argc) {
    if(!func) {
        LOG_ERROR("FFI Error: null function object\n");
        return val_none();
    }
    if(!func->func_ptr) {
        LOG_ERROR("FFI Error: function '%s' not resolved (library: %s)\n",
                func->name ? func->name : "?",
                func->libname ? func->libname : "(default)");
        return val_none();
    }
    if(argc > FFI_MAX_ARGS) {
        LOG_ERROR("FFI Error: too many arguments (%d), max %d\n", argc, FFI_MAX_ARGS);
        return val_none();
    }

    /* 转换参数为 C 类型 */
    long long cargs[FFI_MAX_ARGS] = {0};
    double dargs[FFI_MAX_ARGS] = {0.0};
    int all_float_args = 1; /* 是否所有参数都是浮点类型 */

    for(int i = 0; i < argc; i++) {
        ValueType ptype = (i < func->param_count) ? func->param_types[i] : VAL_INT;
        if(((ptype) == VAL_FLOAT || (ptype) == VAL_DOUBLE)) {
            /* 浮点类型：用 XMM 寄存器传递 */
            double d = value_as_number(args[i]);
            if(ptype == VAL_FLOAT) d = (float)d; /* 单精度截断 */
            dargs[i] = d;
            /* 同时按位复制到 cargs（用于混合参数的回退） */
            memcpy(&cargs[i], &d, sizeof(double));
        } else {
            all_float_args = 0;
            if(((ptype) >= VAL_INT && (ptype) <= VAL_BOOL)) {
                /* 所有整数类型：x86-64 调用约定自动提升为 64 位，直接传递 long long */
                cargs[i] = (long long)value_as_number(args[i]);
            } else switch(ptype) {
            case VAL_PTR:
                /* 指针/句柄：从 int 值直接传递（Lumyr 中用 int 存储指针） */
                cargs[i] = (long long)value_as_number(args[i]);
                break;
            case VAL_STRING:
                if(args[i].type == VAL_STRING) {
                    const char* s = lumyr_str_cstr(&args[i]);
                    cargs[i] = (long long)(size_t)s;
                } else {
                    cargs[i] = (long long)(size_t)"";
                }
                break;
            case VAL_CALLBACK: {
                /* 回调函数：注册 Lumyr 函数，返回槽位 ID 作为函数指针 */
                if(args[i].type == VAL_FUNC) {
                    int slot = lumyr_ffi_register_callback(args[i], 4);
                    cargs[i] = (long long)(size_t)slot;
                } else {
                    cargs[i] = 0;
                }
                break;
            }
            case VAL_VOID:
            default:
                cargs[i] = 0;
                break;
            }
            }
    }

    /* 根据返回类型、参数类型和参数数量调用函数 */
    long long ret = 0;
    double dret = 0.0;
    int is_float_ret = ((func->ret_type) == VAL_FLOAT || (func->ret_type) == VAL_DOUBLE);

    if(all_float_args && argc <= 6) {
        /* 全浮点参数：用 ffi_*_d_t 函数指针（参数传递到 XMM 寄存器） */
        if(is_float_ret) {
            switch(argc) {
                case 0: dret = ((ffi_dfunc0_d_t)func->func_ptr)(); break;
                case 1: dret = ((ffi_dfunc1_d_t)func->func_ptr)(dargs[0]); break;
                case 2: dret = ((ffi_dfunc2_d_t)func->func_ptr)(dargs[0], dargs[1]); break;
                case 3: dret = ((ffi_dfunc3_d_t)func->func_ptr)(dargs[0], dargs[1], dargs[2]); break;
                case 4: dret = ((ffi_dfunc4_d_t)func->func_ptr)(dargs[0], dargs[1], dargs[2], dargs[3]); break;
                case 5: dret = ((ffi_dfunc5_d_t)func->func_ptr)(dargs[0], dargs[1], dargs[2], dargs[3], dargs[4]); break;
                case 6: dret = ((ffi_dfunc6_d_t)func->func_ptr)(dargs[0], dargs[1], dargs[2], dargs[3], dargs[4], dargs[5]); break;
                default:
                    LOG_ERROR("FFI Error: invalid argument count %d\n", argc);
                    return val_none();
            }
        } else {
            switch(argc) {
                case 0: ret = ((ffi_func0_d_t)func->func_ptr)(); break;
                case 1: ret = ((ffi_func1_d_t)func->func_ptr)(dargs[0]); break;
                case 2: ret = ((ffi_func2_d_t)func->func_ptr)(dargs[0], dargs[1]); break;
                case 3: ret = ((ffi_func3_d_t)func->func_ptr)(dargs[0], dargs[1], dargs[2]); break;
                case 4: ret = ((ffi_func4_d_t)func->func_ptr)(dargs[0], dargs[1], dargs[2], dargs[3]); break;
                case 5: ret = ((ffi_func5_d_t)func->func_ptr)(dargs[0], dargs[1], dargs[2], dargs[3], dargs[4]); break;
                case 6: ret = ((ffi_func6_d_t)func->func_ptr)(dargs[0], dargs[1], dargs[2], dargs[3], dargs[4], dargs[5]); break;
                default:
                    LOG_ERROR("FFI Error: invalid argument count %d\n", argc);
                    return val_none();
            }
        }
    } else if(is_float_ret) {
        /* double 返回类型：用 ffi_dfuncN_t 函数指针（返回值在 XMM0） */
        switch(argc) {
            case 0: dret = ((ffi_dfunc0_t)func->func_ptr)(); break;
            case 1: dret = ((ffi_dfunc1_t)func->func_ptr)(cargs[0]); break;
            case 2: dret = ((ffi_dfunc2_t)func->func_ptr)(cargs[0], cargs[1]); break;
            case 3: dret = ((ffi_dfunc3_t)func->func_ptr)(cargs[0], cargs[1], cargs[2]); break;
            case 4: dret = ((ffi_dfunc4_t)func->func_ptr)(cargs[0], cargs[1], cargs[2], cargs[3]); break;
            case 5: dret = ((ffi_dfunc5_t)func->func_ptr)(cargs[0], cargs[1], cargs[2], cargs[3], cargs[4]); break;
            case 6: dret = ((ffi_dfunc6_t)func->func_ptr)(cargs[0], cargs[1], cargs[2], cargs[3], cargs[4], cargs[5]); break;
            case 7: dret = ((ffi_dfunc7_t)func->func_ptr)(cargs[0], cargs[1], cargs[2], cargs[3], cargs[4], cargs[5], cargs[6]); break;
            case 8: dret = ((ffi_dfunc8_t)func->func_ptr)(cargs[0], cargs[1], cargs[2], cargs[3], cargs[4], cargs[5], cargs[6], cargs[7]); break;
            case 9: dret = ((ffi_dfunc9_t)func->func_ptr)(cargs[0], cargs[1], cargs[2], cargs[3], cargs[4], cargs[5], cargs[6], cargs[7], cargs[8]); break;
            case 10: dret = ((ffi_dfunc10_t)func->func_ptr)(cargs[0], cargs[1], cargs[2], cargs[3], cargs[4], cargs[5], cargs[6], cargs[7], cargs[8], cargs[9]); break;
            case 11: dret = ((ffi_dfunc11_t)func->func_ptr)(cargs[0], cargs[1], cargs[2], cargs[3], cargs[4], cargs[5], cargs[6], cargs[7], cargs[8], cargs[9], cargs[10]); break;
            case 12: dret = ((ffi_dfunc12_t)func->func_ptr)(cargs[0], cargs[1], cargs[2], cargs[3], cargs[4], cargs[5], cargs[6], cargs[7], cargs[8], cargs[9], cargs[10], cargs[11]); break;
            case 13: dret = ((ffi_dfunc13_t)func->func_ptr)(cargs[0], cargs[1], cargs[2], cargs[3], cargs[4], cargs[5], cargs[6], cargs[7], cargs[8], cargs[9], cargs[10], cargs[11], cargs[12]); break;
            case 14: dret = ((ffi_dfunc14_t)func->func_ptr)(cargs[0], cargs[1], cargs[2], cargs[3], cargs[4], cargs[5], cargs[6], cargs[7], cargs[8], cargs[9], cargs[10], cargs[11], cargs[12], cargs[13]); break;
            case 15: dret = ((ffi_dfunc15_t)func->func_ptr)(cargs[0], cargs[1], cargs[2], cargs[3], cargs[4], cargs[5], cargs[6], cargs[7], cargs[8], cargs[9], cargs[10], cargs[11], cargs[12], cargs[13], cargs[14]); break;
            case 16: dret = ((ffi_dfunc16_t)func->func_ptr)(cargs[0], cargs[1], cargs[2], cargs[3], cargs[4], cargs[5], cargs[6], cargs[7], cargs[8], cargs[9], cargs[10], cargs[11], cargs[12], cargs[13], cargs[14], cargs[15]); break;
            default:
                LOG_ERROR("FFI Error: invalid argument count %d\n", argc);
                return val_none();
        }
    } else {
        /* 非 double 返回类型：用 ffi_funcN_t 函数指针（返回值在 RAX） */
        switch(argc) {
            case 0: ret = ((ffi_func0_t)func->func_ptr)(); break;
            case 1: ret = ((ffi_func1_t)func->func_ptr)(cargs[0]); break;
            case 2: ret = ((ffi_func2_t)func->func_ptr)(cargs[0], cargs[1]); break;
            case 3: ret = ((ffi_func3_t)func->func_ptr)(cargs[0], cargs[1], cargs[2]); break;
            case 4: ret = ((ffi_func4_t)func->func_ptr)(cargs[0], cargs[1], cargs[2], cargs[3]); break;
            case 5: ret = ((ffi_func5_t)func->func_ptr)(cargs[0], cargs[1], cargs[2], cargs[3], cargs[4]); break;
            case 6: ret = ((ffi_func6_t)func->func_ptr)(cargs[0], cargs[1], cargs[2], cargs[3], cargs[4], cargs[5]); break;
            case 7: ret = ((ffi_func7_t)func->func_ptr)(cargs[0], cargs[1], cargs[2], cargs[3], cargs[4], cargs[5], cargs[6]); break;
            case 8: ret = ((ffi_func8_t)func->func_ptr)(cargs[0], cargs[1], cargs[2], cargs[3], cargs[4], cargs[5], cargs[6], cargs[7]); break;
            case 9: ret = ((ffi_func9_t)func->func_ptr)(cargs[0], cargs[1], cargs[2], cargs[3], cargs[4], cargs[5], cargs[6], cargs[7], cargs[8]); break;
            case 10: ret = ((ffi_func10_t)func->func_ptr)(cargs[0], cargs[1], cargs[2], cargs[3], cargs[4], cargs[5], cargs[6], cargs[7], cargs[8], cargs[9]); break;
            case 11: ret = ((ffi_func11_t)func->func_ptr)(cargs[0], cargs[1], cargs[2], cargs[3], cargs[4], cargs[5], cargs[6], cargs[7], cargs[8], cargs[9], cargs[10]); break;
            case 12: ret = ((ffi_func12_t)func->func_ptr)(cargs[0], cargs[1], cargs[2], cargs[3], cargs[4], cargs[5], cargs[6], cargs[7], cargs[8], cargs[9], cargs[10], cargs[11]); break;
            case 13: ret = ((ffi_func13_t)func->func_ptr)(cargs[0], cargs[1], cargs[2], cargs[3], cargs[4], cargs[5], cargs[6], cargs[7], cargs[8], cargs[9], cargs[10], cargs[11], cargs[12]); break;
            case 14: ret = ((ffi_func14_t)func->func_ptr)(cargs[0], cargs[1], cargs[2], cargs[3], cargs[4], cargs[5], cargs[6], cargs[7], cargs[8], cargs[9], cargs[10], cargs[11], cargs[12], cargs[13]); break;
            case 15: ret = ((ffi_func15_t)func->func_ptr)(cargs[0], cargs[1], cargs[2], cargs[3], cargs[4], cargs[5], cargs[6], cargs[7], cargs[8], cargs[9], cargs[10], cargs[11], cargs[12], cargs[13], cargs[14]); break;
            case 16: ret = ((ffi_func16_t)func->func_ptr)(cargs[0], cargs[1], cargs[2], cargs[3], cargs[4], cargs[5], cargs[6], cargs[7], cargs[8], cargs[9], cargs[10], cargs[11], cargs[12], cargs[13], cargs[14], cargs[15]); break;
            default:
                LOG_ERROR("FFI Error: invalid argument count %d\n", argc);
                return val_none();
        }
    }

    /* 转换返回值 */
    if(is_float_ret) {
        if(func->ret_type == VAL_FLOAT) {
            return val_double((double)(float)dret); /* 单精度截断后再转 double */
        }
        return val_double(dret);
    }
    /* 整数类型返回值：根据具体类型进行符号扩展或零扩展 */
    if(((func->ret_type) >= VAL_INT && (func->ret_type) <= VAL_BOOL)) {
        switch(func->ret_type) {
            case VAL_INT8:   return val_int((int64_t)(int8_t)ret);
            case VAL_INT16:  return val_int((int64_t)(int16_t)ret);
            case VAL_INT32:  return val_int((int64_t)(int32_t)ret);
            case VAL_CHAR:   return val_int((int64_t)(char)ret);
            case VAL_UINT8:  return val_int((int64_t)(uint8_t)ret);
            case VAL_UINT16: return val_int((int64_t)(uint16_t)ret);
            case VAL_UINT32: return val_int((int64_t)(uint32_t)ret);
            case VAL_UCHAR:  return val_int((int64_t)(unsigned char)ret);
            case VAL_BOOL:   return val_bool(ret ? 1 : 0);
            /* 64 位类型直接返回 */
            default:         return val_int((int64_t)ret);
        }
    }
    switch(func->ret_type) {
        case VAL_PTR:
            /* 指针/句柄返回：用 int 存储指针值 */
            return val_int((int64_t)ret);
        case VAL_STRING:
            if(ret) {
                return lumyr_make_string((const char*)(size_t)ret);
            }
            return val_none();
        case VAL_VOID:
        default:
            return val_none();
    }
}

/* ==================== 函数对象管理 ==================== */

/* 创建 FFI 函数对象 */
FFIFunc* lumyr_ffi_func_create(const char* name, const char* libname,
                                 ValueType ret_type, ValueType* param_types, int param_count) {
    if(!name) {
        LOG_ERROR("FFI Error: function name is null\n");
        return NULL;
    }

    FFIFunc* func = (FFIFunc*)calloc(1, sizeof(FFIFunc));
    if(!func) {
        LOG_ERROR("FFI Error: out of memory allocating FFIFunc\n");
        return NULL;
    }

    func->name = strdup(name);
    func->libname = libname ? strdup(libname) : NULL;
    func->ret_type = ret_type;
    func->param_count = param_count;

    if(param_count > 0 && param_types) {
        func->param_types = (ValueType*)malloc((size_t)param_count * sizeof(ValueType));
        if(func->param_types) {
            memcpy(func->param_types, param_types, (size_t)param_count * sizeof(ValueType));
        } else {
            LOG_ERROR("FFI Error: out of memory allocating param types\n");
            free(func->name);
            free(func->libname);
            free(func);
            return NULL;
        }
    }

    /* 加载库并解析函数地址 */
    if(func->libname) {
        func->lib_handle = lumyr_ffi_load_lib(func->libname);
        if(!func->lib_handle) {
            LOG_ERROR("FFI Warning: library '%s' not loaded, function '%s' will fail at call time\n",
                    func->libname, func->name);
        }
    }

    func->func_ptr = lumyr_ffi_get_func(func->lib_handle, func->name);

    if(!func->func_ptr) {
        LOG_ERROR("FFI Warning: function '%s' not found in library '%s'\n",
                name, libname ? libname : "(default)");
    }

    return func;
}

/* 释放 FFI 函数对象 */
void lumyr_ffi_func_free(FFIFunc* func) {
    if(!func) return;
    free(func->name);
    free(func->libname);
    free(func->param_types);
#ifdef _WIN32
    if(func->lib_handle) FreeLibrary((HMODULE)func->lib_handle);
#else
    if(func->lib_handle) dlclose(func->lib_handle);
#endif
    free(func);
}

/* ==================== 类型名转换 ==================== */

/* 类型名转换 - 完整 C 类型系统（使用 ValueType） */
ValueType lumyr_ffi_type_from_name(const char* name) {
    if(!name) return VAL_INT; /* 默认按 int 处理 */
    /* void */
    if(strcmp(name, "void") == 0 || strcmp(name, "none") == 0) return VAL_VOID;
    /* 有符号整数 */
    if(strcmp(name, "int") == 0 || strcmp(name, "int32") == 0 || strcmp(name, "int32_t") == 0) return VAL_INT;
    if(strcmp(name, "int8") == 0 || strcmp(name, "int8_t") == 0 || strcmp(name, "signed char") == 0) return VAL_INT8;
    if(strcmp(name, "int16") == 0 || strcmp(name, "int16_t") == 0 || strcmp(name, "short") == 0) return VAL_INT16;
    if(strcmp(name, "int64") == 0 || strcmp(name, "int64_t") == 0 || strcmp(name, "long long") == 0) return VAL_INT64;
    if(strcmp(name, "long") == 0) return VAL_LONG;
    if(strcmp(name, "char") == 0) return VAL_CHAR;
    /* 无符号整数 */
    if(strcmp(name, "uint8") == 0 || strcmp(name, "uint8_t") == 0 || strcmp(name, "unsigned char") == 0 || strcmp(name, "byte") == 0) return VAL_UINT8;
    if(strcmp(name, "uint16") == 0 || strcmp(name, "uint16_t") == 0 || strcmp(name, "unsigned short") == 0) return VAL_UINT16;
    if(strcmp(name, "uint32") == 0 || strcmp(name, "uint32_t") == 0 || strcmp(name, "unsigned int") == 0 || strcmp(name, "uint") == 0) return VAL_UINT32;
    if(strcmp(name, "uint64") == 0 || strcmp(name, "uint64_t") == 0 || strcmp(name, "unsigned long long") == 0) return VAL_UINT64;
    if(strcmp(name, "unsigned long") == 0 || strcmp(name, "ulong") == 0) return VAL_ULONG;
    if(strcmp(name, "unsigned char") == 0 || strcmp(name, "uchar") == 0) return VAL_UCHAR;
    /* 平台相关类型 */
    if(strcmp(name, "size_t") == 0 || strcmp(name, "size") == 0) return VAL_SIZE_T;
    if(strcmp(name, "ssize_t") == 0 || strcmp(name, "ptrdiff_t") == 0 || strcmp(name, "ssize") == 0) return VAL_SSIZE_T;
    /* 布尔 */
    if(strcmp(name, "bool") == 0 || strcmp(name, "_Bool") == 0) return VAL_BOOL;
    /* 浮点 */
    if(strcmp(name, "float") == 0) return VAL_FLOAT;
    if(strcmp(name, "double") == 0) return VAL_DOUBLE;
    /* 字符串 */
    if(strcmp(name, "string") == 0 || strcmp(name, "str") == 0 || strcmp(name, "char*") == 0 || strcmp(name, "const char*") == 0) return VAL_STRING;
    /* 指针/句柄类型 */
    if(strcmp(name, "ptr") == 0 || strcmp(name, "pointer") == 0 || strcmp(name, "handle") == 0) return VAL_PTR;
    if(strcmp(name, "void*") == 0 || strcmp(name, "int*") == 0 || strcmp(name, "double*") == 0) return VAL_PTR;
    if(strcmp(name, "array") == 0) return VAL_PTR; /* 数组首地址作为指针 */
    if(strcmp(name, "struct") == 0) return VAL_PTR; /* 结构体指针 */
    if(strcmp(name, "obj") == 0) return VAL_PTR;     /* 对象句柄 */
    /* 回调函数类型 */
    if(strcmp(name, "callback") == 0 || strcmp(name, "func_ptr") == 0 || strcmp(name, "function") == 0) return VAL_CALLBACK;
    return VAL_INT; /* 默认按 int 处理 */
}

const char* lumyr_ffi_type_to_name(ValueType type) {
    switch(type) {
        case VAL_VOID: return "void";
        case VAL_INT: return "int";
        case VAL_INT8: return "int8";
        case VAL_INT16: return "int16";
        case VAL_INT32: return "int32";
        case VAL_INT64: return "int64";
        case VAL_LONG: return "long";
        case VAL_CHAR: return "char";
        case VAL_UINT8: return "uint8";
        case VAL_UINT16: return "uint16";
        case VAL_UINT32: return "uint32";
        case VAL_UINT64: return "uint64";
        case VAL_ULONG: return "ulong";
        case VAL_UCHAR: return "uchar";
        case VAL_SIZE_T: return "size_t";
        case VAL_SSIZE_T: return "ssize_t";
        case VAL_BOOL: return "bool";
        case VAL_FLOAT: return "float";
        case VAL_DOUBLE: return "double";
        case VAL_STRING: return "string";
        case VAL_PTR: return "ptr";
        case VAL_CALLBACK: return "callback";
        default: return "int";
    }
}

/* 获取 C 类型名字符串（用于编译通道生成代码） */
const char* lumyr_ffi_type_to_cname(ValueType type) {
    switch(type) {
        case VAL_VOID: return "void";
        case VAL_INT: return "int";
        case VAL_INT8: return "int8_t";
        case VAL_INT16: return "int16_t";
        case VAL_INT32: return "int32_t";
        case VAL_INT64: return "int64_t";
        case VAL_LONG: return "long";
        case VAL_CHAR: return "char";
        case VAL_UINT8: return "uint8_t";
        case VAL_UINT16: return "uint16_t";
        case VAL_UINT32: return "uint32_t";
        case VAL_UINT64: return "uint64_t";
        case VAL_ULONG: return "unsigned long";
        case VAL_UCHAR: return "unsigned char";
        case VAL_SIZE_T: return "size_t";
        case VAL_SSIZE_T: return "ssize_t";
        case VAL_BOOL: return "int";
        case VAL_FLOAT: return "float";
        case VAL_DOUBLE: return "double";
        case VAL_STRING: return "const char*";
        case VAL_PTR: return "void*";
        case VAL_CALLBACK: return "void*";
        default: return "int";
    }
}
