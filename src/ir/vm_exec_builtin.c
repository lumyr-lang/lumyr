/*
 * VM 内置函数执行模块
 *
 * 本模块提供 VM 内置函数的执行逻辑，包括数组操作、Map操作、
 * 字符串操作、数学运算、线程同步、HTTP请求、JSON处理等。
 */

#include "vm_exec.h"
#include "vm_macros.h"
#include "vm_internal.h"
#include "vm_generator.h"
#include "vm_try_context.h"
#include "vm_types.h"
#include "lumyr_log.h"
#include "ir_compile.h"
#include "lumyr_ffi.h"
#include "ast/stackframe.h"
#include "ast/func_compile.h"
#include "ast/ast_runtime_sym.h"
#include "lm_value.h"
#include "lm_runtime.h"
#include "gc_runtime.h"
#include "lm_thread.h"
#include "ast/ast_types.h"
#include "lm_class.h"
#include "stack_manager.h"
#include "lm_lock.h"
#include "lm_tls.h"
#include "lm_http.h"
#include "lm_json.h"
#include "lm_qs.h"
#include "lm_charset.h"
#include "lm_crypto.h"
#include "lm_regex.h"
#include "lm_time.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>

int vm_exec_builtin(Instruction in, Value* stack, int sp, StackFrame* frame, EvalCtx* ctx)
{
                int argc = in.b;
                switch(in.a) {
                    case BUILTIN_LEN: {
                        Value v = stack[--sp];
                        stack[sp++] = lumyr_len(v);
                        break;
                    }
                    case BUILTIN_TYPE: {
                        Value v = stack[--sp];
                        stack[sp++] = lumyr_type(v);
                        break;
                    }
                    case BUILTIN_INPUT: {
                        stack[sp++] = lumyr_input();
                        break;
                    }
                    case BUILTIN_RANGE: {
                        int n = in.b;
                        Value r = lumyr_range_n(&stack[sp - n], n);
                        sp = sp - n + 1;
                        sp--; stack[sp++] = r;
                        break;
                    }
                    case BUILTIN_SUBSTR: {
                        Value n = stack[--sp];
                        Value st = stack[--sp];
                        Value s = stack[--sp];
                        stack[sp++] = lumyr_substr(s, st, n);
                        break;
                    }
                    case BUILTIN_TOUPPER: {
                        Value v = stack[--sp];
                        stack[sp++] = lumyr_toupper(v);
                        break;
                    }
                    case BUILTIN_TOLOWER: {
                        Value v = stack[--sp];
                        stack[sp++] = lumyr_tolower(v);
                        break;
                    }
                    case BUILTIN_SPLIT: {
                        Value sep = stack[--sp];
                        Value s = stack[--sp];
                        stack[sp++] = lumyr_split(s, sep);
                        break;
                    }
                    case BUILTIN_DEL: {
                        Value idx = stack[--sp];
                        lumyr_del(&stack[sp-1], idx);
                        break;
                    }
                    case BUILTIN_INSERT: {
                        Value val = stack[--sp];
                        Value idx = stack[--sp];
                        lumyr_insert(&stack[sp-1], idx, val);
                        break;
                    }
                    case BUILTIN_FLOOR: { Value v = stack[--sp]; stack[sp++] = lumyr_floor(v); break; }
                    case BUILTIN_CEIL:  { Value v = stack[--sp]; stack[sp++] = lumyr_ceil(v); break; }
                    case BUILTIN_ABS:   { Value v = stack[--sp]; stack[sp++] = lumyr_abs(v); break; }
                    case BUILTIN_SQRT:  { Value v = stack[--sp]; stack[sp++] = lumyr_sqrt(v); break; }
                    case BUILTIN_MAX:
                    case BUILTIN_MIN: {
                        int n = in.b;
                        Value r = (in.a == BUILTIN_MAX) ? lumyr_max(&stack[sp - n], n) : lumyr_min(&stack[sp - n], n);
                        sp = sp - n + 1;
                        sp--; stack[sp++] = r;
                        break;
                    }
                    case BUILTIN_JOIN: {
                        Value sep = stack[--sp];
                        Value arr = stack[--sp];
                        stack[sp++] = lumyr_join(arr, sep);
                        break;
                    }
                    case BUILTIN_CONTAINS: {
                        Value needle = stack[--sp];
                        Value hay = stack[--sp];
                        stack[sp++] = lumyr_contains(hay, needle);
                        break;
                    }
                    case BUILTIN_REPEAT: {
                        Value n = stack[--sp];
                        Value s = stack[--sp];
                        stack[sp++] = lumyr_repeat(s, n);
                        break;
                    }
                    case BUILTIN_REPLACE: {
                        Value to = stack[--sp];
                        Value from = stack[--sp];
                        Value s = stack[--sp];
                        stack[sp++] = lumyr_replace(s, from, to);
                        break;
                    }
                    case BUILTIN_SUM: { Value v = stack[--sp]; stack[sp++] = lumyr_sum(v); break; }
                    case BUILTIN_AVG: { Value v = stack[--sp]; stack[sp++] = lumyr_avg(v); break; }
                    case BUILTIN_FORMAT: {
                        int n = in.b;
                        Value r = lumyr_format(&stack[sp - n], n);
                        sp = sp - n + 1;
                        sp--; stack[sp++] = r;
                        break;
                    }
                    case BUILTIN_SORT:    { Value v = stack[--sp]; stack[sp++] = lumyr_sort(v); break; }
                    case BUILTIN_REVERSE:{ Value v = stack[--sp]; stack[sp++] = lumyr_reverse(v); break; }
                    case BUILTIN_STRIP:   { Value v = stack[--sp]; stack[sp++] = lumyr_strip(v); break; }
                    case BUILTIN_STARTSWITH: { Value r = stack[--sp], l = stack[--sp]; stack[sp++] = lumyr_startswith(l, r); break; }
                    case BUILTIN_ENDSWITH:   { Value r = stack[--sp], l = stack[--sp]; stack[sp++] = lumyr_endswith(l, r); break; }
                    case BUILTIN_READ_FILE:  { int n2 = in.b; Value r = lumyr_read_file(&stack[sp - n2], n2); sp = sp - n2 + 1; sp--; stack[sp++] = r; break; }
                    case BUILTIN_WRITE_FILE: { int n2 = in.b; Value r = lumyr_write_file(&stack[sp - n2], n2); sp = sp - n2 + 1; sp--; stack[sp++] = r; break; }
                    case BUILTIN_FILE_EXISTS:{ int n2 = in.b; Value r = lumyr_file_exists(&stack[sp - n2], n2); sp = sp - n2 + 1; sp--; stack[sp++] = r; break; }
                    case BUILTIN_KEYS:     { int n2 = in.b; Value r = lumyr_map_keys(stack[sp - n2]); sp = sp - n2 + 1; sp--; stack[sp++] = r; break; }
                    case BUILTIN_VALUES:   { int n2 = in.b; Value r = lumyr_map_values(stack[sp - n2]); sp = sp - n2 + 1; sp--; stack[sp++] = r; break; }
                    case BUILTIN_THREAD: {
                        int argc = in.b;
                        if(argc < 1) runtime_error("thread() 至少需要一个函数参数");
                        Value fn = stack[sp - argc];
                        if(fn.type != VAL_FUNC) runtime_error("thread() 第一个参数必须是函数");
                        RuntimeFunc* rf = fn.v.func.func_obj;
                        int nargs = argc - 1;
                        VmThreadArg* a = (VmThreadArg*)malloc(sizeof(VmThreadArg));
                        if(!a) runtime_error("thread: 内存不足");
                        a->rf = rf;
                        a->global_frame = s_global_frame;
                        int tid = lumyr_thread_start(vm_thread_body, (void*)a, (nargs > 0) ? &stack[sp - nargs] : NULL, nargs);
                        sp = sp - argc + 1;
                        sp--; stack[sp++] = lumyr_make_int(tid);
                        break;
                    }
                    case BUILTIN_THREAD_JOIN: {
                        Value idv = stack[--sp];
                        if(idv.type != VAL_INT) runtime_error("thread_join() 参数必须是线程id（整数）");
                        stack[sp++] = lumyr_thread_join((int)idv.v.i);
                        break;
                    }
                    case BUILTIN_MUTEX:    { stack[sp++] = lumyr_make_int(lumyr_mutex_create()); break; }
                    case BUILTIN_RMUTEX:   { stack[sp++] = lumyr_make_int(lumyr_rmutex_create()); break; }
                    case BUILTIN_RWLOCK:   { stack[sp++] = lumyr_make_int(lumyr_rwlock_create()); break; }
                    case BUILTIN_SPINLOCK: { stack[sp++] = lumyr_make_int(lumyr_spinlock_create()); break; }
                    case BUILTIN_LOCK:   { Value v = stack[--sp]; if(v.type != VAL_INT) runtime_error("lock() 参数必须是锁id（整数）"); lumyr_lock((int)v.v.i); stack[sp++] = v; break; }
                    case BUILTIN_UNLOCK: { Value v = stack[--sp]; if(v.type != VAL_INT) runtime_error("unlock() 参数必须是锁id（整数）"); lumyr_unlock((int)v.v.i); stack[sp++] = v; break; }
                    case BUILTIN_TRYLOCK: {
                        Value v = stack[--sp];
                        if(v.type != VAL_INT) runtime_error("trylock() 参数必须是锁id（整数）");
                        stack[sp++] = lumyr_make_bool(lumyr_trylock((int)v.v.i));
                        break;
                    }
                    case BUILTIN_RDLOCK: { Value v = stack[--sp]; if(v.type != VAL_INT) runtime_error("rdlock() 参数必须是锁id（整数）"); lumyr_rdlock((int)v.v.i); stack[sp++] = v; break; }
                    case BUILTIN_WRLOCK: { Value v = stack[--sp]; if(v.type != VAL_INT) runtime_error("wrlock() 参数必须是锁id（整数）"); lumyr_wrlock((int)v.v.i); stack[sp++] = v; break; }
                    case BUILTIN_TRYRDLOCK: {
                        Value v = stack[--sp];
                        if(v.type != VAL_INT) runtime_error("tryrdlock() 参数必须是锁id（整数）");
                        stack[sp++] = lumyr_make_bool(lumyr_tryrdlock((int)v.v.i));
                        break;
                    }
                    case BUILTIN_TRYWRLOCK: {
                        Value v = stack[--sp];
                        if(v.type != VAL_INT) runtime_error("trywrlock() 参数必须是锁id（整数）");
                        stack[sp++] = lumyr_make_bool(lumyr_trywrlock((int)v.v.i));
                        break;
                    }
                    case BUILTIN_CONDVAR: { stack[sp++] = lumyr_make_int(lumyr_condvar_create()); break; }
                    case BUILTIN_COND_WAIT: {
                        Value lk = stack[--sp];
                        Value cd = stack[--sp];
                        if(lk.type != VAL_INT) runtime_error("cond_wait() 锁参数必须是锁id（整数）");
                        if(cd.type != VAL_INT) runtime_error("cond_wait() 条件参数必须是条件id（整数）");
                        lumyr_cond_wait((int)cd.v.i, (int)lk.v.i);
                        stack[sp++] = lk;    // 压回原值（表达式值）
                        break;
                    }
                    case BUILTIN_COND_SIGNAL: { Value v = stack[--sp]; if(v.type != VAL_INT) runtime_error("cond_signal() 参数必须是条件id（整数）"); lumyr_cond_signal((int)v.v.i); stack[sp++] = v; break; }
                    case BUILTIN_COND_BROADCAST: { Value v = stack[--sp]; if(v.type != VAL_INT) runtime_error("cond_broadcast() 参数必须是条件id（整数）"); lumyr_cond_broadcast((int)v.v.i); stack[sp++] = v; break; }
                    case BUILTIN_COND_TIMEDWAIT: {
                        Value ms = stack[--sp];
                        Value lk = stack[--sp];
                        Value cd = stack[--sp];
                        if(ms.type != VAL_INT) runtime_error("cond_wait_timeout() 超时参数必须是整数毫秒");
                        if(lk.type != VAL_INT) runtime_error("cond_wait_timeout() 锁参数必须是锁id（整数）");
                        if(cd.type != VAL_INT) runtime_error("cond_wait_timeout() 条件参数必须是条件id（整数）");
                        stack[sp++] = lumyr_make_bool(lumyr_cond_timedwait((int)cd.v.i, (int)lk.v.i, ms.v.i));
                        break;
                    }
                    case BUILTIN_THREADLOCAL_GET: {
                        Value nm = stack[--sp];
                        if(nm.type != VAL_STRING) runtime_error("threadlocal_get() 名字参数必须是字符串");
                        stack[sp++] = lumyr_tls_get(lumyr_str_cstr(&nm));
                        break;
                    }
                    case BUILTIN_THREADLOCAL_SET: {
                        Value v = stack[--sp];
                        Value nm = stack[--sp];
                        if(nm.type != VAL_STRING) runtime_error("threadlocal_set() 名字参数必须是字符串");
                        lumyr_tls_set(lumyr_str_cstr(&nm), v);
                        stack[sp++] = v;    // 压回原值（表达式值）
                        break;
                    }
                    case BUILTIN_HTTP_GET:
                    case BUILTIN_HTTP_POST:
                    case BUILTIN_HTTP_PUT:
                    case BUILTIN_ARRAY_ADD: {
                        if(in.b == 3) {
                            Value v = stack[--sp];
                            Value k = stack[--sp];
                            Value m = stack[--sp];
                            stack[sp++] = lumyr_map_add(m, k, v);
                        } else {
                            Value v = stack[--sp];
                            lumyr_array_add(&stack[sp-1], v);
                        }
                        break;
                    }
                    case BUILTIN_ARRAY_REMOVE: {
                        Value idx = stack[--sp];
                        lumyr_del(&stack[sp-1], idx);
                        break;
                    }
                    case BUILTIN_ARRAY_INDEXOF: {
                        Value x = stack[--sp];
                        Value arr = stack[--sp];
                        stack[sp++] = lumyr_index_of(arr, x);
                        break;
                    }
                    case BUILTIN_ARRAY_GET: {
                        Value i = stack[--sp];
                        Value arr = stack[--sp];
                        stack[sp++] = lumyr_array_get_safe(arr, i);
                        break;
                    }
                    case BUILTIN_ARRAY_SET: {
                        Value v = stack[--sp];
                        Value i = stack[--sp];
                        Value arr = stack[--sp];
                        stack[sp++] = lumyr_array_set_method(arr, i, v);
                        break;
                    }
                    case BUILTIN_ARRAY_FIRST: {
                        Value arr = stack[--sp];
                        stack[sp++] = lumyr_array_first(arr);
                        break;
                    }
                    case BUILTIN_ARRAY_LAST: {
                        Value arr = stack[--sp];
                        stack[sp++] = lumyr_array_last(arr);
                        break;
                    }
                    case BUILTIN_ARRAY_CLEAR: {
                        lumyr_array_clear(&stack[sp-1]);
                        break;
                    }
                    case BUILTIN_MAP_HAS: {
                        Value k = stack[--sp];
                        Value m = stack[--sp];
                        stack[sp++] = lumyr_make_bool(lumyr_map_has(m, k));
                        break;
                    }
                    case BUILTIN_JSON: {
                        Value enc = val_none();
                        Value v;
                        if(in.b >= 2) { enc = stack[--sp]; v = stack[--sp]; }
                        else { v = stack[--sp]; }
                        stack[sp++] = lumyr_json_parse_enc(lumyr_str_cstr(&v), enc);
                        break;
                    }
                    case BUILTIN_STRINGIFY: {
                        Value enc = val_none();
                        Value v;
                        if(in.b >= 2) { enc = stack[--sp]; v = stack[--sp]; }
                        else { v = stack[--sp]; }
                        char* js = lumyr_json_stringify_enc(v, enc);
                        Value r = lumyr_make_string(js);
                        free(js);
                        stack[sp++] = r;
                        break;
                    }
                    case BUILTIN_ARRAY_FLAT: {
                        Value depth = lumyr_make_int(1);
                        Value v;
                        if(in.b >= 2) { depth = stack[--sp]; v = stack[--sp]; }
                        else { v = stack[--sp]; }
                        stack[sp++] = lumyr_array_flat(v, lumyr_extract_int(depth));
                        break;
                    }
                    case BUILTIN_QS: {
                        Value enc = val_none();
                        Value v;
                        if(in.b >= 2) { enc = stack[--sp]; v = stack[--sp]; }
                        else { v = stack[--sp]; }
                        if(v.type == VAL_MAP || v.type == VAL_ARRAY) {
                            char* q = lumyr_qs_stringify_enc(v, enc);
                            stack[sp++] = lumyr_make_string(q);
                            free(q);
                        } else if(v.type == VAL_STRING) {
                            stack[sp++] = lumyr_qs_parse_enc(lumyr_str_cstr(&v), enc);
                        } else {
                            runtime_error("qs() 参数必须是字典/数组（序列化）或字符串（解析）");
                        }
                        break;
                    }
                    case BUILTIN_ARRAY_ADDALL: {
                        Value b = stack[--sp];
                        lumyr_array_addall(&stack[sp-1], b);
                        break;
                    }
                    case BUILTIN_BYTES: {
                        Value enc = val_none();
                        Value v;
                        if(in.b >= 2) { enc = stack[--sp]; v = stack[--sp]; }
                        else { v = stack[--sp]; }
                        stack[sp++] = lumyr_to_bytes(v, enc);
                        break;
                    }
                    case BUILTIN_STR: {
                        Value enc = val_none();
                        Value v;
                        if(in.b >= 2) { enc = stack[--sp]; v = stack[--sp]; }
                        else { v = stack[--sp]; }
                        stack[sp++] = lumyr_from_bytes(v, enc);
                        break;
                    }
                    case BUILTIN_ENCODE: {
                        Value enc = val_none();
                        Value v;
                        if(in.b >= 2) { enc = stack[--sp]; v = stack[--sp]; }
                        else { v = stack[--sp]; }
                        stack[sp++] = lumyr_to_bytes(v, enc);
                        break;
                    }
                    case BUILTIN_DECODE: {
                        Value enc = val_none();
                        Value v;
                        if(in.b >= 2) { enc = stack[--sp]; v = stack[--sp]; }
                        else { v = stack[--sp]; }
                        stack[sp++] = lumyr_from_bytes(v, enc);
                        break;
                    }
                    case BUILTIN_ENCODE_URL: {
                        Value v = stack[--sp];
                        char* r = lumyr_url_encode(v.type == VAL_STRING ? (lumyr_str_cstr(&v) ? lumyr_str_cstr(&v) : "") : "");
                        stack[sp++] = lumyr_make_string(r);
                        free(r);
                        break;
                    }
                    case BUILTIN_DECODE_URL: {
                        Value v = stack[--sp];
                        char* r = lumyr_url_decode(v.type == VAL_STRING ? (lumyr_str_cstr(&v) ? lumyr_str_cstr(&v) : "") : "");
                        stack[sp++] = lumyr_make_string(r);
                        free(r);
                        break;
                    }
                    case BUILTIN_MD5: {
                        Value v = stack[--sp];
                        const char* inp = v.type == VAL_STRING ? (lumyr_str_cstr(&v) ? lumyr_str_cstr(&v) : "") : "";
                        char* r = lumyr_md5_hex(inp, (int)strlen(inp));
                        stack[sp++] = lumyr_make_string(r);
                        free(r);
                        break;
                    }
                    case BUILTIN_ENCODE_BASE64: {
                        Value v = stack[--sp];
                        const char* inp = v.type == VAL_STRING ? (lumyr_str_cstr(&v) ? lumyr_str_cstr(&v) : "") : "";
                        char* r = lumyr_base64_encode(inp, (int)strlen(inp));
                        stack[sp++] = lumyr_make_string(r);
                        free(r);
                        break;
                    }
                    case BUILTIN_DECODE_BASE64: {
                        Value v = stack[--sp];
                        int olen = 0;
                        char* r = lumyr_base64_decode(v.type == VAL_STRING ? (lumyr_str_cstr(&v) ? lumyr_str_cstr(&v) : "") : "", &olen);
                        stack[sp++] = lumyr_make_string(r);
                        free(r);
                        break;
                    }
                    case BUILTIN_REGEX_MATCH: {
                        Value pat = stack[--sp];
                        Value str = stack[--sp];
                        stack[sp++] = lumyr_make_bool(lumyr_regex_match(
                            str.type == VAL_STRING ? (lumyr_str_cstr(&str) ? lumyr_str_cstr(&str) : "") : "",
                            pat.type == VAL_STRING ? (lumyr_str_cstr(&pat) ? lumyr_str_cstr(&pat) : "") : ""));
                        break;
                    }
                    case BUILTIN_REGEX_SEARCH: {
                        Value pat = stack[--sp];
                        Value str = stack[--sp];
                        stack[sp++] = lumyr_regex_search(
                            str.type == VAL_STRING ? (lumyr_str_cstr(&str) ? lumyr_str_cstr(&str) : "") : "",
                            pat.type == VAL_STRING ? (lumyr_str_cstr(&pat) ? lumyr_str_cstr(&pat) : "") : "");
                        break;
                    }
                    case BUILTIN_REGEX_REPLACE: {
                        Value repl = stack[--sp];
                        Value pat = stack[--sp];
                        Value str = stack[--sp];
                        char* r = lumyr_regex_replace(
                            str.type == VAL_STRING ? (lumyr_str_cstr(&str) ? lumyr_str_cstr(&str) : "") : "",
                            pat.type == VAL_STRING ? (lumyr_str_cstr(&pat) ? lumyr_str_cstr(&pat) : "") : "",
                            repl.type == VAL_STRING ? (lumyr_str_cstr(&repl) ? lumyr_str_cstr(&repl) : "") : "");
                        stack[sp++] = lumyr_make_string(r);
                        free(r);
                        break;
                    }
                    case BUILTIN_NOW: {
                        stack[sp++] = lumyr_now();
                        break;
                    }
                    case BUILTIN_TIMESTAMP: {
                        stack[sp++] = lumyr_make_double(lumyr_timestamp());
                        break;
                    }
                    case BUILTIN_TIMESTAMP_MS: {
                        stack[sp++] = lumyr_make_int(lumyr_timestamp_ms());
                        break;
                    }
                    case BUILTIN_SLEEP: {
                        Value v = stack[--sp];
                        lumyr_sleep_ms((long long)lumyr_extract_int(v));
                        stack[sp++] = val_none();
                        break;
                    }
                    case BUILTIN_DATE: {
                        char* r = lumyr_date_str();
                        stack[sp++] = lumyr_make_string(r);
                        free(r);
                        break;
                    }
                    case BUILTIN_TIME: {
                        char* r = lumyr_time_str();
                        stack[sp++] = lumyr_make_string(r);
                        free(r);
                        break;
                    }
                    case BUILTIN_DATETIME: {
                        char* r = lumyr_datetime_str();
                        stack[sp++] = lumyr_make_string(r);
                        free(r);
                        break;
                    }
                    case BUILTIN_FORMAT_TIME: {
                        Value ts = val_none();
                        Value fmt;
                        if(in.b >= 2) { ts = stack[--sp]; fmt = stack[--sp]; }
                        else { fmt = stack[--sp]; }
                        double tsv = (ts.type == VAL_NONE) ? -1.0 : (ts.type == VAL_DOUBLE ? ts.v.d : (double)lumyr_extract_int(ts));
                        char* r = lumyr_format_time(fmt.type == VAL_STRING ? (lumyr_str_cstr(&fmt) ? lumyr_str_cstr(&fmt) : "") : "", tsv);
                        stack[sp++] = lumyr_make_string(r);
                        free(r);
                        break;
                    }
                    case BUILTIN_LOG_DEBUG:
                    case BUILTIN_LOG_INFO:
                    case BUILTIN_LOG_WARN:
                    case BUILTIN_LOG_ERROR:
                    case BUILTIN_LOG_FATAL: {
                        int lvl = in.a - BUILTIN_LOG_DEBUG;
                        Value msg;
                        if(in.b >= 2) { msg = stack[--sp]; --sp; }  // 方法链：先弹消息，再丢弃 receiver
                        else { msg = stack[--sp]; }
                        char* ms = value_to_str(msg);
                        lumyr_log(lvl, ms);
                        free(ms);
                        stack[sp++] = val_none();
                        break;
                    }
                    case BUILTIN_GC_COUNT: {
                        stack[sp++] = lumyr_make_int((long long)gc_count());
                        break;
                    }
                    case BUILTIN_GC_BYTES: {
                        stack[sp++] = lumyr_make_int((long long)gc_bytes());
                        break;
                    }
                    case BUILTIN_GC_COLLECT: {
                        gc_collect_now();
                        stack[sp++] = val_none();
                        break;
                    }
                    case BUILTIN_GC_STW_NS: {
                        stack[sp++] = lumyr_make_int((long long)gc_stw_time_ns());
                        break;
                    }
                    case BUILTIN_NEXT: {
                        /* next(gen)：恢复生成器执行，返回 yield 值；结束返回 null。
                         * 不弹出 gen_val，让它始终留在 VM 栈上作为 GC 根，
                         * 避免 generator_resume 执行期间触发 GC 时生成器引用被错误回收。 */
                        Value gen_val = stack[sp - 1];
                        if(gen_val.type != VAL_GENERATOR) {
                            LOG_ERROR("Runtime Error: next() 需要生成器对象，实际类型: %d\n", gen_val.type);
                            exit(EXIT_FAILURE);
                        }
                        GeneratorObject* gen = (GeneratorObject*)gen_val.v.generator;
                        Value result;
                        int yielded = generator_resume(gen, &result, NULL, frame, ctx);
                        stack[sp - 1] = result;  /* 用结果覆盖栈顶的生成器 Value */
                        (void)yielded;
                        break;
                    }
                    case BUILTIN_SEND: {
                        /* send(gen, val)：向生成器发送值，恢复执行，返回下一个 yield 值。
                         * gen_val 留在栈上作为 GC 根，send_val 复制到 C 栈。 */
                        Value send_val = stack[--sp];
                        Value gen_val = stack[sp - 1];  /* 不弹出，留在栈上 */
                        if(gen_val.type != VAL_GENERATOR) {
                            LOG_ERROR("Runtime Error: send() 需要生成器对象，实际类型: %d\n", gen_val.type);
                            exit(EXIT_FAILURE);
                        }
                        GeneratorObject* gen = (GeneratorObject*)gen_val.v.generator;
                        if(!gen->started) {
                            LOG_ERROR("Runtime Error: send() 不能用于刚创建的生成器，请先调用 next()\n");
                            exit(EXIT_FAILURE);
                        }
                        Value result;
                        int yielded = generator_resume(gen, &result, &send_val, frame, ctx);
                        stack[sp - 1] = result;  /* 用结果覆盖栈顶 */
                        (void)yielded;
                        break;
                    }
                    case BUILTIN_RECEIVE: {
                        /* receive()：在生成器中获取 send() 发送的值；非生成器上下文返回 null */
                        if(s_current_gen && s_current_gen->has_send_value) {
                            stack[sp++] = s_current_gen->send_value;
                        } else {
                            stack[sp++] = val_none();
                        }
                        break;
                    }
                    case BUILTIN_CLOSE: {
                        /* close(gen)：关闭生成器，标记为已结束 */
                        Value gen_val = stack[--sp];
                        if(gen_val.type != VAL_GENERATOR) {
                            LOG_ERROR("Runtime Error: close() 需要生成器对象，实际类型: %d\n", gen_val.type);
                            exit(EXIT_FAILURE);
                        }
                        GeneratorObject* gen = (GeneratorObject*)gen_val.v.generator;
                        gen->finished = 1;
                        stack[sp++] = val_none();
                        break;
                    }
                    case BUILTIN_GEN_THROW: {
                        /* GenThrow(gen, err)：向生成器抛出异常，在 yield 位置抛出 */
                        Value err_val = stack[--sp];
                        Value gen_val = stack[--sp];
                        if(gen_val.type != VAL_GENERATOR) {
                            LOG_ERROR("Runtime Error: GenThrow() 需要生成器对象，实际类型: %d\n", gen_val.type);
                            exit(EXIT_FAILURE);
                        }
                        GeneratorObject* gen = (GeneratorObject*)gen_val.v.generator;
                        if(gen->finished) {
                            LOG_ERROR("Runtime Error: GenThrow() 生成器已结束\n");
                            exit(EXIT_FAILURE);
                        }
                        /* 设置待抛出的异常，恢复执行时会在 yield 位置抛出 */
                        gen->pending_exception = err_val;
                        gen->has_pending_exception = 1;
                        /* 恢复生成器执行，异常会在恢复时抛出 */
                        Value result;
                        generator_resume(gen, &result, NULL, frame, ctx);
                        stack[sp++] = result;
                        break;
                    }
                    case BUILTIN_CHAIN: {
                        /* chain(g1, g2)：连接两个生成器 */
                        Value g2 = stack[--sp];
                        Value g1 = stack[--sp];
                        if(g1.type != VAL_GENERATOR || g2.type != VAL_GENERATOR) {
                            runtime_error("chain() 参数必须是生成器");
                        }
                        GeneratorObject* wg = (GeneratorObject*)calloc(1, sizeof(GeneratorObject));
                        wg->is_wrapped = 1;
                        wg->wrap_type = WRAP_CHAIN;
                        wg->wrapped_gen = (GeneratorObject*)g1.v.generator;
                        wg->wrapped_gen2 = (GeneratorObject*)g2.v.generator;
                                                paused_gen_add(wg);  /* 注册为 GC 根，从创建到结束始终保持 */
Value gv; gv.type = VAL_GENERATOR; gv.v.generator = (void*)wg;
                        stack[sp++] = gv;
                        break;
                    }
                    case BUILTIN_ZIP: {
                        /* zip(g1, g2)：压缩两个生成器 */
                        Value g2 = stack[--sp];
                        Value g1 = stack[--sp];
                        if(g1.type != VAL_GENERATOR || g2.type != VAL_GENERATOR) {
                            runtime_error("zip() 参数必须是生成器");
                        }
                        GeneratorObject* wg = (GeneratorObject*)calloc(1, sizeof(GeneratorObject));
                        wg->is_wrapped = 1;
                        wg->wrap_type = WRAP_ZIP;
                        wg->wrapped_gen = (GeneratorObject*)g1.v.generator;
                        wg->wrapped_gen2 = (GeneratorObject*)g2.v.generator;
                                                paused_gen_add(wg);  /* 注册为 GC 根，从创建到结束始终保持 */
Value gv; gv.type = VAL_GENERATOR; gv.v.generator = (void*)wg;
                        stack[sp++] = gv;
                        break;
                    }
                    case BUILTIN_SKIP: {
                        /* skip(g, n)：跳过前 n 个元素 */
                        Value nval = stack[--sp];
                        Value gval = stack[--sp];
                        if(gval.type != VAL_GENERATOR) runtime_error("skip() 第一个参数必须是生成器");
                        int n = lumyr_extract_int(nval);
                        GeneratorObject* wg = (GeneratorObject*)calloc(1, sizeof(GeneratorObject));
                        wg->is_wrapped = 1;
                        wg->wrap_type = WRAP_SKIP;
                        wg->wrapped_gen = (GeneratorObject*)gval.v.generator;
                        wg->wrap_arg = n;
                                                paused_gen_add(wg);  /* 注册为 GC 根，从创建到结束始终保持 */
Value gv; gv.type = VAL_GENERATOR; gv.v.generator = (void*)wg;
                        stack[sp++] = gv;
                        break;
                    }
                    case BUILTIN_TAKE: {
                        /* take(g, n)：取前 n 个元素 */
                        Value nval = stack[--sp];
                        Value gval = stack[--sp];
                        if(gval.type != VAL_GENERATOR) runtime_error("take() 第一个参数必须是生成器");
                        int n = lumyr_extract_int(nval);
                        GeneratorObject* wg = (GeneratorObject*)calloc(1, sizeof(GeneratorObject));
                        wg->is_wrapped = 1;
                        wg->wrap_type = WRAP_TAKE;
                        wg->wrapped_gen = (GeneratorObject*)gval.v.generator;
                        wg->wrap_arg = n;
                                                paused_gen_add(wg);  /* 注册为 GC 根，从创建到结束始终保持 */
Value gv; gv.type = VAL_GENERATOR; gv.v.generator = (void*)wg;
                        stack[sp++] = gv;
                        break;
                    }
                    case BUILTIN_ENUMERATE: {
                        /* enumerate(g)：枚举 [index, value] */
                        Value gval = stack[--sp];
                        if(gval.type != VAL_GENERATOR) runtime_error("enumerate() 参数必须是生成器");
                        GeneratorObject* wg = (GeneratorObject*)calloc(1, sizeof(GeneratorObject));
                        wg->is_wrapped = 1;
                        wg->wrap_type = WRAP_ENUMERATE;
                        wg->wrapped_gen = (GeneratorObject*)gval.v.generator;
                                                paused_gen_add(wg);  /* 注册为 GC 根，从创建到结束始终保持 */
Value gv; gv.type = VAL_GENERATOR; gv.v.generator = (void*)wg;
                        stack[sp++] = gv;
                        break;
                    }
                    case BUILTIN_HTTP_DELETE:
                    case BUILTIN_HTTP_HEAD:
                    case BUILTIN_HTTP_PATCH: {
                        int n = in.b;
                        if(n < 1 || n > 3) runtime_error("requests 请求需要 1~3 个参数：url、可选 params、可选 config");
                        const char* m = "GET";
                        switch(in.a) {
                            case BUILTIN_HTTP_POST:   m = "POST"; break;
                            case BUILTIN_HTTP_PUT:    m = "PUT"; break;
                            case BUILTIN_ARRAY_ADD: {
                        if(in.b == 3) {
                            Value v = stack[--sp];
                            Value k = stack[--sp];
                            Value m = stack[--sp];
                            stack[sp++] = lumyr_map_add(m, k, v);
                        } else {
                            Value v = stack[--sp];
                            lumyr_array_add(&stack[sp-1], v);
                        }
                        break;
                    }
                    case BUILTIN_ARRAY_REMOVE: {
                        Value idx = stack[--sp];
                        lumyr_del(&stack[sp-1], idx);
                        break;
                    }
                    case BUILTIN_ARRAY_INDEXOF: {
                        Value x = stack[--sp];
                        Value arr = stack[--sp];
                        stack[sp++] = lumyr_index_of(arr, x);
                        break;
                    }
                    case BUILTIN_ARRAY_GET: {
                        Value i = stack[--sp];
                        Value arr = stack[--sp];
                        stack[sp++] = lumyr_array_get_safe(arr, i);
                        break;
                    }
                    case BUILTIN_ARRAY_SET: {
                        Value v = stack[--sp];
                        Value i = stack[--sp];
                        Value arr = stack[--sp];
                        stack[sp++] = lumyr_array_set_method(arr, i, v);
                        break;
                    }
                    case BUILTIN_ARRAY_FIRST: {
                        Value arr = stack[--sp];
                        stack[sp++] = lumyr_array_first(arr);
                        break;
                    }
                    case BUILTIN_ARRAY_LAST: {
                        Value arr = stack[--sp];
                        stack[sp++] = lumyr_array_last(arr);
                        break;
                    }
                    case BUILTIN_ARRAY_CLEAR: {
                        lumyr_array_clear(&stack[sp-1]);
                        break;
                    }
                    case BUILTIN_MAP_HAS: {
                        Value k = stack[--sp];
                        Value m = stack[--sp];
                        stack[sp++] = lumyr_make_bool(lumyr_map_has(m, k));
                        break;
                    }
                    case BUILTIN_JSON: {
                        Value enc = val_none();
                        Value v;
                        if(in.b >= 2) { enc = stack[--sp]; v = stack[--sp]; }
                        else { v = stack[--sp]; }
                        stack[sp++] = lumyr_json_parse_enc(lumyr_str_cstr(&v), enc);
                        break;
                    }
                    case BUILTIN_STRINGIFY: {
                        Value enc = val_none();
                        Value v;
                        if(in.b >= 2) { enc = stack[--sp]; v = stack[--sp]; }
                        else { v = stack[--sp]; }
                        char* js = lumyr_json_stringify_enc(v, enc);
                        Value r = lumyr_make_string(js);
                        free(js);
                        stack[sp++] = r;
                        break;
                    }
                    case BUILTIN_ARRAY_FLAT: {
                        Value depth = lumyr_make_int(1);
                        Value v;
                        if(in.b >= 2) { depth = stack[--sp]; v = stack[--sp]; }
                        else { v = stack[--sp]; }
                        stack[sp++] = lumyr_array_flat(v, lumyr_extract_int(depth));
                        break;
                    }
                    case BUILTIN_QS: {
                        Value enc = val_none();
                        Value v;
                        if(in.b >= 2) { enc = stack[--sp]; v = stack[--sp]; }
                        else { v = stack[--sp]; }
                        if(v.type == VAL_MAP || v.type == VAL_ARRAY) {
                            char* q = lumyr_qs_stringify_enc(v, enc);
                            stack[sp++] = lumyr_make_string(q);
                            free(q);
                        } else if(v.type == VAL_STRING) {
                            stack[sp++] = lumyr_qs_parse_enc(lumyr_str_cstr(&v), enc);
                        } else {
                            runtime_error("qs() 参数必须是字典/数组（序列化）或字符串（解析）");
                        }
                        break;
                    }
                    case BUILTIN_ARRAY_ADDALL: {
                        Value b = stack[--sp];
                        lumyr_array_addall(&stack[sp-1], b);
                        break;
                    }
                    case BUILTIN_BYTES: {
                        Value enc = val_none();
                        Value v;
                        if(in.b >= 2) { enc = stack[--sp]; v = stack[--sp]; }
                        else { v = stack[--sp]; }
                        stack[sp++] = lumyr_to_bytes(v, enc);
                        break;
                    }
                    case BUILTIN_STR: {
                        Value enc = val_none();
                        Value v;
                        if(in.b >= 2) { enc = stack[--sp]; v = stack[--sp]; }
                        else { v = stack[--sp]; }
                        stack[sp++] = lumyr_from_bytes(v, enc);
                        break;
                    }
                    case BUILTIN_ENCODE: {
                        Value enc = val_none();
                        Value v;
                        if(in.b >= 2) { enc = stack[--sp]; v = stack[--sp]; }
                        else { v = stack[--sp]; }
                        stack[sp++] = lumyr_to_bytes(v, enc);
                        break;
                    }
                    case BUILTIN_DECODE: {
                        Value enc = val_none();
                        Value v;
                        if(in.b >= 2) { enc = stack[--sp]; v = stack[--sp]; }
                        else { v = stack[--sp]; }
                        stack[sp++] = lumyr_from_bytes(v, enc);
                        break;
                    }
                    case BUILTIN_ENCODE_URL: {
                        Value v = stack[--sp];
                        char* r = lumyr_url_encode(v.type == VAL_STRING ? (lumyr_str_cstr(&v) ? lumyr_str_cstr(&v) : "") : "");
                        stack[sp++] = lumyr_make_string(r);
                        free(r);
                        break;
                    }
                    case BUILTIN_DECODE_URL: {
                        Value v = stack[--sp];
                        char* r = lumyr_url_decode(v.type == VAL_STRING ? (lumyr_str_cstr(&v) ? lumyr_str_cstr(&v) : "") : "");
                        stack[sp++] = lumyr_make_string(r);
                        free(r);
                        break;
                    }
                    case BUILTIN_MD5: {
                        Value v = stack[--sp];
                        const char* inp = v.type == VAL_STRING ? (lumyr_str_cstr(&v) ? lumyr_str_cstr(&v) : "") : "";
                        char* r = lumyr_md5_hex(inp, (int)strlen(inp));
                        stack[sp++] = lumyr_make_string(r);
                        free(r);
                        break;
                    }
                    case BUILTIN_ENCODE_BASE64: {
                        Value v = stack[--sp];
                        const char* inp = v.type == VAL_STRING ? (lumyr_str_cstr(&v) ? lumyr_str_cstr(&v) : "") : "";
                        char* r = lumyr_base64_encode(inp, (int)strlen(inp));
                        stack[sp++] = lumyr_make_string(r);
                        free(r);
                        break;
                    }
                    case BUILTIN_DECODE_BASE64: {
                        Value v = stack[--sp];
                        int olen = 0;
                        char* r = lumyr_base64_decode(v.type == VAL_STRING ? (lumyr_str_cstr(&v) ? lumyr_str_cstr(&v) : "") : "", &olen);
                        stack[sp++] = lumyr_make_string(r);
                        free(r);
                        break;
                    }
                    case BUILTIN_REGEX_MATCH: {
                        Value pat = stack[--sp];
                        Value str = stack[--sp];
                        stack[sp++] = lumyr_make_bool(lumyr_regex_match(
                            str.type == VAL_STRING ? (lumyr_str_cstr(&str) ? lumyr_str_cstr(&str) : "") : "",
                            pat.type == VAL_STRING ? (lumyr_str_cstr(&pat) ? lumyr_str_cstr(&pat) : "") : ""));
                        break;
                    }
                    case BUILTIN_REGEX_SEARCH: {
                        Value pat = stack[--sp];
                        Value str = stack[--sp];
                        stack[sp++] = lumyr_regex_search(
                            str.type == VAL_STRING ? (lumyr_str_cstr(&str) ? lumyr_str_cstr(&str) : "") : "",
                            pat.type == VAL_STRING ? (lumyr_str_cstr(&pat) ? lumyr_str_cstr(&pat) : "") : "");
                        break;
                    }
                    case BUILTIN_REGEX_REPLACE: {
                        Value repl = stack[--sp];
                        Value pat = stack[--sp];
                        Value str = stack[--sp];
                        char* r = lumyr_regex_replace(
                            str.type == VAL_STRING ? (lumyr_str_cstr(&str) ? lumyr_str_cstr(&str) : "") : "",
                            pat.type == VAL_STRING ? (lumyr_str_cstr(&pat) ? lumyr_str_cstr(&pat) : "") : "",
                            repl.type == VAL_STRING ? (lumyr_str_cstr(&repl) ? lumyr_str_cstr(&repl) : "") : "");
                        stack[sp++] = lumyr_make_string(r);
                        free(r);
                        break;
                    }
                    case BUILTIN_NOW: {
                        stack[sp++] = lumyr_now();
                        break;
                    }
                    case BUILTIN_TIMESTAMP: {
                        stack[sp++] = lumyr_make_double(lumyr_timestamp());
                        break;
                    }
                    case BUILTIN_TIMESTAMP_MS: {
                        stack[sp++] = lumyr_make_int(lumyr_timestamp_ms());
                        break;
                    }
                    case BUILTIN_SLEEP: {
                        Value v = stack[--sp];
                        lumyr_sleep_ms((long long)lumyr_extract_int(v));
                        stack[sp++] = val_none();
                        break;
                    }
                    case BUILTIN_DATE: {
                        char* r = lumyr_date_str();
                        stack[sp++] = lumyr_make_string(r);
                        free(r);
                        break;
                    }
                    case BUILTIN_TIME: {
                        char* r = lumyr_time_str();
                        stack[sp++] = lumyr_make_string(r);
                        free(r);
                        break;
                    }
                    case BUILTIN_DATETIME: {
                        char* r = lumyr_datetime_str();
                        stack[sp++] = lumyr_make_string(r);
                        free(r);
                        break;
                    }
                    case BUILTIN_FORMAT_TIME: {
                        Value ts = val_none();
                        Value fmt;
                        if(in.b >= 2) { ts = stack[--sp]; fmt = stack[--sp]; }
                        else { fmt = stack[--sp]; }
                        double tsv = (ts.type == VAL_NONE) ? -1.0 : (ts.type == VAL_DOUBLE ? ts.v.d : (double)lumyr_extract_int(ts));
                        char* r = lumyr_format_time(fmt.type == VAL_STRING ? (lumyr_str_cstr(&fmt) ? lumyr_str_cstr(&fmt) : "") : "", tsv);
                        stack[sp++] = lumyr_make_string(r);
                        free(r);
                        break;
                    }
                    case BUILTIN_LOG_DEBUG:
                    case BUILTIN_LOG_INFO:
                    case BUILTIN_LOG_WARN:
                    case BUILTIN_LOG_ERROR:
                    case BUILTIN_LOG_FATAL: {
                        int lvl = in.a - BUILTIN_LOG_DEBUG;
                        Value msg;
                        if(in.b >= 2) { msg = stack[--sp]; --sp; }  // 方法链：先弹消息，再丢弃 receiver
                        else { msg = stack[--sp]; }
                        char* ms = value_to_str(msg);
                        lumyr_log(lvl, ms);
                        free(ms);
                        stack[sp++] = val_none();
                        break;
                    }
                    case BUILTIN_HTTP_DELETE: m = "DELETE"; break;
                            case BUILTIN_HTTP_HEAD:   m = "HEAD"; break;
                            case BUILTIN_HTTP_PATCH:  m = "PATCH"; break;
                            default: break;
                        }
                        Value url    = stack[sp - n];
                        Value params = (n >= 2) ? stack[sp - n + 1] : val_none();
                        Value config = (n >= 3) ? stack[sp - n + 2] : val_none();
                        Value r = lumyr_http_request(m, url, params, config);
                        sp = sp - n + 1;
                        sp--; stack[sp++] = r;
                        break;
                    }
                    case BUILTIN_MAP:
                    case BUILTIN_FILTER:
                    case BUILTIN_REDUCE: {
                        int argc = in.b;
                        Value fn, arr, init = val_none();
                        if(argc == 3) { init = stack[--sp]; fn = stack[--sp]; arr = stack[--sp]; }
                        else { fn = stack[--sp]; arr = stack[--sp]; }
                        if(arr.type == VAL_MAP && in.a == BUILTIN_MAP) {
                            /* 字典 map：fn(value, key) → 新字典（键不变值映射） */
                            if(fn.type != VAL_FUNC) runtime_error("map() 第二个参数必须是函数");
                            RuntimeFunc* mrf = fn.v.func.func_obj;
                            Value mout = val_map();
                            MapIter it; map_iter_init(&it, arr.v.map);
                            Value mk, mv;
                            while(map_iter_next(&it, &mk, &mv)) {
                                Value a2[2];
                                a2[0] = mv;
                                a2[1] = mk;
                                Value r = vm_call_rf(mrf, a2, 2, frame, ctx);
                                lumyr_map_set(&mout, mk, r);
                            }
                            stack[sp++] = mout;
                            break;
                        }
                        /* 生成器支持：创建包装生成器 */
                        if(arr.type == VAL_GENERATOR && (in.a == BUILTIN_MAP || in.a == BUILTIN_FILTER)) {
                            if(fn.type != VAL_FUNC) runtime_error("map()/filter() 第二个参数必须是函数");
                            GeneratorObject* wrapped = (GeneratorObject*)arr.v.generator;
                            GeneratorObject* wg = (GeneratorObject*)calloc(1, sizeof(GeneratorObject));
                            wg->is_wrapped = 1;
                            wg->wrap_type = (in.a == BUILTIN_MAP) ? WRAP_MAP : WRAP_FILTER;
                            wg->wrapped_gen = wrapped;
                            wg->wrap_fn = fn.v.func.func_obj;
                            Value gv; gv.type = VAL_GENERATOR; gv.v.generator = (void*)wg;
                            stack[sp++] = gv;
                            break;
                        }
                        if(arr.type != VAL_ARRAY) runtime_error("map()/filter()/reduce() 第一个参数必须是数组");
                        if(fn.type != VAL_FUNC) runtime_error("map()/filter()/reduce() 第二个参数必须是函数");
                        RuntimeFunc* rf = fn.v.func.func_obj;
                        int n = arr.v.array->len;
                        if(in.a == BUILTIN_MAP) {
                            Value out = val_array(n);
                            for(int i = 0; i < n; i++) {
                                Value a1[1] = { arr.v.array->items[i] };
                                Value r = vm_call_rf(rf, a1, 1, frame, ctx);
                                out.v.array->items[i] = r;
                            }
                            stack[sp++] = out;
                        } else if(in.a == BUILTIN_FILTER) {
                            Value out = val_array(n);
                            int cnt = 0;
                            for(int i = 0; i < n; i++) {
                                Value a1[1] = { arr.v.array->items[i] };
                                Value r = vm_call_rf(rf, a1, 1, frame, ctx);
                                if(lumyr_to_bool(r)) out.v.array->items[cnt++] = arr.v.array->items[i];
                            }
                            out.v.array->len = cnt;
                            stack[sp++] = out;
                        } else {
                            Value acc = init;
                            for(int i = 0; i < n; i++) {
                                Value a2[2] = { acc, arr.v.array->items[i] };
                                acc = vm_call_rf(rf, a2, 2, frame, ctx);
                            }
                            stack[sp++] = acc;
                        }
                        break;
                    }
                    default:
                        runtime_error("未知内置函数");
                        break;
                }
                (void)argc;
    return sp;
}
