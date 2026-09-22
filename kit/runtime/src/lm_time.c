// lm_time.c —— 日期时间 + 日志
#include "lm_time.h"
#include "gc_runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <unistd.h>

Value lumyr_now(void) {
    time_t t = time(NULL);
    struct tm* lt = localtime(&t);
    Value m = val_map();
    lumyr_map_set(&m, lumyr_make_string("year"), lumyr_make_int(lt->tm_year + 1900));
    lumyr_map_set(&m, lumyr_make_string("month"), lumyr_make_int(lt->tm_mon + 1));
    lumyr_map_set(&m, lumyr_make_string("day"), lumyr_make_int(lt->tm_mday));
    lumyr_map_set(&m, lumyr_make_string("hour"), lumyr_make_int(lt->tm_hour));
    lumyr_map_set(&m, lumyr_make_string("minute"), lumyr_make_int(lt->tm_min));
    lumyr_map_set(&m, lumyr_make_string("second"), lumyr_make_int(lt->tm_sec));
    lumyr_map_set(&m, lumyr_make_string("weekday"), lumyr_make_int(lt->tm_wday)); // 0=周日
    lumyr_map_set(&m, lumyr_make_string("yday"), lumyr_make_int(lt->tm_yday + 1));
    lumyr_map_set(&m, lumyr_make_string("isdst"), lumyr_make_bool(lt->tm_isdst > 0));
    return m;
}

double lumyr_timestamp(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + (double)tv.tv_usec / 1000000.0;
}

long long lumyr_timestamp_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

void lumyr_sleep_ms(long long ms) {
    if(ms <= 0) return;
    /* usleep 阻塞期间不执行 VM 代码、不修改 GC 根，栈稳定，标记安全点。
     * 否则长时间 sleep 会导致 GC 等待所有线程 at_safepoint 超时。 */
    gc_enter_native_block();
    usleep((useconds_t)(ms * 1000));
    gc_leave_native_block();
}

static void fmt_buf(char* buf, size_t sz, const char* fmt, time_t t) {
    struct tm* lt = localtime(&t);
    strftime(buf, sz, fmt, lt);
}

char* lumyr_date_str(void) {
    char buf[32];
    fmt_buf(buf, sizeof(buf), "%Y-%m-%d", time(NULL));
    return strdup(buf);
}

char* lumyr_time_str(void) {
    char buf[32];
    fmt_buf(buf, sizeof(buf), "%H:%M:%S", time(NULL));
    return strdup(buf);
}

char* lumyr_datetime_str(void) {
    char buf[32];
    fmt_buf(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", time(NULL));
    return strdup(buf);
}

char* lumyr_format_time(const char* fmt, double ts) {
    if(!fmt) return strdup("");
    time_t t = (ts < 0) ? time(NULL) : (time_t)ts;
    char buf[256];
    fmt_buf(buf, sizeof(buf), fmt, t);
    return strdup(buf);
}

static const char* level_names[] = {"DEBUG", "INFO", "WARN", "ERROR", "FATAL"};

void lumyr_log(int level, const char* msg) {
    if(level < 0 || level > 4) level = 1;
    char buf[32];
    fmt_buf(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", time(NULL));
    fprintf(stderr, "[%s] [%s] %s\n", buf, level_names[level], msg ? msg : "");
    fflush(stderr);
}

/* ===== date 族对象（VAL_DATE/VAL_DATETIME/VAL_TIME/VAL_TIMEDELTA） ===== */
// 双模式存储：epoch 为主，缓存字段懒计算（gmtime_r，UTC 时区）

// macOS/Linux glibc 均提供 timegm；保险起见给个 extern 声明
extern time_t timegm(struct tm*);

// 填充缓存字段（year/month/day/hour/min/sec/weekday/yearday），按 epoch + kind 解释
static void date_fill_cache(DateObj* o) {
    if(o->cached) return;
    time_t t = (time_t)o->epoch;
    struct tm tmv;
    gmtime_r(&t, &tmv);
    o->year = (int32_t)(tmv.tm_year + 1900);
    o->month = (int32_t)(tmv.tm_mon + 1);
    o->day = (int32_t)tmv.tm_mday;
    o->hour = (int32_t)tmv.tm_hour;
    o->min = (int32_t)tmv.tm_min;
    o->sec = (int32_t)tmv.tm_sec;
    o->weekday = (int32_t)tmv.tm_wday;     // 0=周日 .. 6=周六
    o->yearday = (int32_t)(tmv.tm_yday + 1); // 1-366
    o->cached = 1;
}

// 规整 timedelta 的 nsec 到 [0, 1e9) 且与 epoch 同号
static void td_normalize(int64_t* sec, int32_t* nsec) {
    const int32_t NS = 1000000000;
    while(*nsec >= NS) { *nsec -= NS; *sec += 1; }
    while(*nsec < 0)   { *nsec += NS; *sec -= 1; }
}

// 统一构造：分配 DateObj，填充 epoch/nsec/kind，cached=0
static Value date_alloc(int64_t epoch, int32_t nsec, ValueType kind) {
    DateObj* o = (DateObj*)gc_alloc(sizeof(DateObj), kind);
    if(!o) {
        Value z; z.type = VAL_NONE; z.str_inline = 0; return z;
    }
    o->epoch = epoch;
    o->nsec = nsec;
    o->cached = 0;
    o->kind = kind;
    o->year = o->month = o->day = 0;
    o->hour = o->min = o->sec = 0;
    o->weekday = o->yearday = 0;
    Value r;
    r.type = kind;
    r.str_inline = 0;
    r.v.date_obj = o;
    return r;
}

// date：epoch 规整到当天 00:00 (UTC)
Value lumyr_make_date(int64_t epoch) {
    // 取模保证落到 00:00（处理负 epoch）
    int64_t rem = epoch % 86400;
    if(rem != 0) {
        epoch -= rem;
        if(rem < 0) epoch -= 86400;  // 负数向负无穷取整
    }
    return date_alloc(epoch, 0, VAL_DATE);
}

Value lumyr_make_datetime(int64_t epoch, int32_t nsec) {
    return date_alloc(epoch, nsec, VAL_DATETIME);
}

// time：当天秒数 [0,86400)
Value lumyr_make_time_obj(int32_t sec, int32_t nsec) {
    while(sec >= 86400) sec -= 86400;
    while(sec < 0)      sec += 86400;
    return date_alloc((int64_t)sec, nsec, VAL_TIME);
}

Value lumyr_make_timedelta(int64_t sec, int32_t nsec) {
    td_normalize(&sec, &nsec);
    return date_alloc(sec, nsec, VAL_TIMEDELTA);
}

// ymd → date（UTC，用 timegm）
Value lumyr_make_date_ymd(int y, int mo, int d) {
    struct tm tmv;
    memset(&tmv, 0, sizeof(tmv));
    tmv.tm_year = y - 1900;
    tmv.tm_mon = mo - 1;
    tmv.tm_mday = d;
    time_t t = timegm(&tmv);
    return lumyr_make_date((int64_t)t);
}

// ymd-hms-ns → datetime（UTC）
Value lumyr_make_datetime_ymd(int y, int mo, int d, int h, int mi, int s, int ns) {
    struct tm tmv;
    memset(&tmv, 0, sizeof(tmv));
    tmv.tm_year = y - 1900;
    tmv.tm_mon = mo - 1;
    tmv.tm_mday = d;
    tmv.tm_hour = h;
    tmv.tm_min = mi;
    tmv.tm_sec = s;
    time_t t = timegm(&tmv);
    return lumyr_make_datetime((int64_t)t, ns);
}

// hms-ns → time
Value lumyr_make_time_hms(int h, int mi, int s, int ns) {
    int32_t sec = (int32_t)(h * 3600 + mi * 60 + s);
    return lumyr_make_time_obj(sec, ns);
}

// 当前 UTC 时间 → VAL_DATETIME
Value lumyr_date_now(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return lumyr_make_datetime((int64_t)tv.tv_sec, (int32_t)(tv.tv_usec * 1000));
}

// 当前 UTC 当天 → VAL_DATE
Value lumyr_date_today(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return lumyr_make_date((int64_t)tv.tv_sec);
}

// 字段访问：year/month/day/hour/minute/second/weekday/yearday
// timedelta：days/seconds/total_seconds（未知返回 0）
Value lumyr_date_field(Value v, const char* name) {
    if(!name) return lumyr_make_int(0);
    // 非 timedelta 都走缓存
    DateObj* o = (DateObj*)v.v.date_obj;
    if(!o) return lumyr_make_int(0);
    if(v.type != VAL_TIMEDELTA) date_fill_cache(o);

    if(strcmp(name, "year") == 0)    return lumyr_make_int(o->year);
    if(strcmp(name, "month") == 0)   return lumyr_make_int(o->month);
    if(strcmp(name, "day") == 0)     return lumyr_make_int(o->day);
    if(strcmp(name, "hour") == 0)    return lumyr_make_int(o->hour);
    if(strcmp(name, "minute") == 0)  return lumyr_make_int(o->min);
    if(strcmp(name, "second") == 0)  return lumyr_make_int(o->sec);
    if(strcmp(name, "weekday") == 0) return lumyr_make_int(o->weekday);
    if(strcmp(name, "yearday") == 0) return lumyr_make_int(o->yearday);

    // timedelta 专用字段
    if(v.type == VAL_TIMEDELTA) {
        if(strcmp(name, "days") == 0) {
            return lumyr_make_int64(o->epoch / 86400);
        }
        if(strcmp(name, "seconds") == 0) {
            // 天内剩余秒数 [0,86399]
            int64_t s = o->epoch % 86400;
            if(s < 0) s += 86400;
            return lumyr_make_int64(s);
        }
        if(strcmp(name, "total_seconds") == 0) {
            double total = (double)o->epoch + (double)o->nsec / 1e9;
            return lumyr_make_double(total);
        }
    }
    return lumyr_make_int(0);
}

// ISO 字符串（malloc，调用者需 free）
char* lumyr_date_to_iso(Value v) {
    DateObj* o = (DateObj*)v.v.date_obj;
    if(!o) return strdup("");
    char buf[64];
    switch(v.type) {
    case VAL_DATE: {
        date_fill_cache(o);
        snprintf(buf, sizeof(buf), "%04d-%02d-%02d", o->year, o->month, o->day);
        break;
    }
    case VAL_DATETIME: {
        date_fill_cache(o);
        if(o->nsec != 0) {
            snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%09d",
                o->year, o->month, o->day, o->hour, o->min, o->sec, o->nsec);
        } else {
            snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d",
                o->year, o->month, o->day, o->hour, o->min, o->sec);
        }
        break;
    }
    case VAL_TIME: {
        // time 的 epoch 是当天秒数；填缓存
        date_fill_cache(o);
        if(o->nsec != 0) {
            snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%09d", o->hour, o->min, o->sec, o->nsec);
        } else {
            snprintf(buf, sizeof(buf), "%02d:%02d:%02d", o->hour, o->min, o->sec);
        }
        break;
    }
    case VAL_TIMEDELTA: {
        int sign = 1;
        int64_t e = o->epoch;
        int32_t n = o->nsec;
        if(e < 0 || (e == 0 && n < 0)) { sign = -1; e = -e; n = -n; }
        int64_t days = e / 86400;
        int64_t rem = e % 86400;
        int hh = (int)(rem / 3600);
        int mm = (int)((rem % 3600) / 60);
        int ss = (int)(rem % 60);
        if(n != 0) {
            snprintf(buf, sizeof(buf), "%s%lld day(s) %02d:%02d:%02d.%09d",
                sign < 0 ? "-" : "", (long long)days, hh, mm, ss, n);
        } else {
            snprintf(buf, sizeof(buf), "%s%lld day(s) %02d:%02d:%02d",
                sign < 0 ? "-" : "", (long long)days, hh, mm, ss);
        }
        break;
    }
    default:
        buf[0] = '\0';
        break;
    }
    return strdup(buf);
}

// strftime 风格格式化（UTC）
char* lumyr_date_format(Value v, const char* fmt) {
    if(!fmt) return strdup("");
    DateObj* o = (DateObj*)v.v.date_obj;
    if(!o) return strdup("");
    // timedelta 不支持 strftime，返回 ISO
    if(v.type == VAL_TIMEDELTA) return lumyr_date_to_iso(v);
    time_t t = (time_t)o->epoch;
    struct tm tmv;
    gmtime_r(&t, &tmv);
    char buf[256];
    strftime(buf, sizeof(buf), fmt, &tmv);
    return strdup(buf);
}

// diff(a, b) = a - b → VAL_TIMEDELTA
Value lumyr_date_diff(Value a, Value b) {
    DateObj* oa = (DateObj*)a.v.date_obj;
    DateObj* ob = (DateObj*)b.v.date_obj;
    if(!oa || !ob) return lumyr_make_timedelta(0, 0);
    int64_t s = oa->epoch - ob->epoch;
    // 仅 datetime/time 有 nsec 参与差值；date/timedelta 视 nsec=0
    int32_t na = (a.type == VAL_DATETIME || a.type == VAL_TIME) ? oa->nsec : 0;
    int32_t nb = (b.type == VAL_DATETIME || b.type == VAL_TIME) ? ob->nsec : 0;
    int32_t n = na - nb;
    return lumyr_make_timedelta(s, n);
}

// add(v, n, unit)：v + n*unit
// unit: second(s)/minute(s)/hour(s)/day(s)/week(s)/month(s)/year(s)
// month/year 对 date/datetime 走日历，其他按秒近似
Value lumyr_date_add(Value v, int64_t n, const char* unit) {
    if(!unit) return v;
    DateObj* o = (DateObj*)v.v.date_obj;
    if(!o) return v;

    // 解析单位（接受单数/复数）
    int is_seconds = (strcmp(unit, "second") == 0 || strcmp(unit, "seconds") == 0);
    int is_minutes = (strcmp(unit, "minute") == 0 || strcmp(unit, "minutes") == 0);
    int is_hours   = (strcmp(unit, "hour") == 0   || strcmp(unit, "hours") == 0);
    int is_days    = (strcmp(unit, "day") == 0    || strcmp(unit, "days") == 0);
    int is_weeks   = (strcmp(unit, "week") == 0   || strcmp(unit, "weeks") == 0);
    int is_months  = (strcmp(unit, "month") == 0  || strcmp(unit, "months") == 0);
    int is_years   = (strcmp(unit, "year") == 0   || strcmp(unit, "years") == 0);

    // 月/年：对 date/datetime 走日历运算
    if(is_months || is_years) {
        int64_t months = is_years ? n * 12 : n;
        if(v.type == VAL_DATE || v.type == VAL_DATETIME) {
            date_fill_cache(o);
            int64_t total = (int64_t)(o->year) * 12 + (o->month - 1) + months;
            int ny = (int)(total / 12);
            int nm = (int)(total % 12) + 1;
            if(nm < 1) { nm += 12; ny -= 1; }
            if(nm > 12) { nm -= 12; ny += 1; }
            // 修正日（避免 2 月 30 日溢出）
            int mdays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
            int leap = (ny % 4 == 0 && ny % 100 != 0) || (ny % 400 == 0);
            int max_day = (nm == 2 && leap) ? 29 : mdays[nm - 1];
            int dd = o->day > max_day ? max_day : o->day;
            if(v.type == VAL_DATE) {
                return lumyr_make_date_ymd(ny, nm, dd);
            }
            return lumyr_make_datetime_ymd(ny, nm, dd, o->hour, o->min, o->sec, o->nsec);
        }
        // timedelta/time：按 30 天/365 天近似
        int64_t sec = is_years ? n * 365 * 86400 : n * 30 * 86400;
        return date_alloc(o->epoch + sec, o->nsec, v.type);
    }

    // 其余单位按秒
    int64_t mul = 1;
    if(is_seconds)      mul = 1;
    else if(is_minutes) mul = 60;
    else if(is_hours)   mul = 3600;
    else if(is_days)    mul = 86400;
    else if(is_weeks)   mul = 86400 * 7;
    else return v;  // 未知单位

    int64_t new_epoch = o->epoch + n * mul;
    int32_t nsec = o->nsec;
    if(v.type == VAL_TIMEDELTA) {
        return lumyr_make_timedelta(new_epoch, nsec);
    }
    if(v.type == VAL_TIME) {
        return lumyr_make_time_obj((int32_t)new_epoch, nsec);
    }
    if(v.type == VAL_DATE) {
        return lumyr_make_date(new_epoch);
    }
    if(v.type == VAL_DATETIME) {
        return lumyr_make_datetime(new_epoch, nsec);
    }
    return v;
}
