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
// 双模式存储：epoch 为主，缓存字段懒计算；时区支持（默认本地电脑时区）

// macOS/Linux glibc 均提供 timegm；保险起见给个 extern 声明
extern time_t timegm(struct tm*);

// 哨兵值：表示"本地电脑时区"
#define TZ_LOCAL INT32_MIN

// 按 tz_offset_min 构造 struct tm（从 UTC epoch 转为本地时间字段）
static void epoch_to_tm(int64_t epoch, int32_t tz_offset_min, struct tm* out) {
    time_t t = (time_t)epoch;
    if(tz_offset_min == TZ_LOCAL) {
        localtime_r(&t, out);
    } else {
        gmtime_r(&t, out);
        // 手动加时区偏移（分钟 → 秒）
        time_t adjusted = t + (time_t)tz_offset_min * 60;
        gmtime_r(&adjusted, out);
    }
}

// 按 tz_offset_min 将 struct tm（本地时间）转为 UTC epoch
static int64_t tm_to_epoch(struct tm* tmv, int32_t tz_offset_min) {
    if(tz_offset_min == TZ_LOCAL) {
        return (int64_t)mktime(tmv);
    }
    // UTC epoch = timegm(tmv) - offset*60（因为 tmv 是 UTC+offset 时区的本地时间）
    return (int64_t)timegm(tmv) - (int64_t)tz_offset_min * 60;
}

// 填充缓存字段（year/month/day/hour/min/sec/weekday/yearday），按 epoch + kind + tz 解释
static void date_fill_cache(DateObj* o) {
    if(o->cached) return;
    if(o->kind == VAL_TIME) {
        // time 的 epoch 是当天秒数 [0,86400)，不是 Unix 时间戳，不进行时区转换
        o->hour = (int32_t)(o->epoch / 3600);
        o->min = (int32_t)((o->epoch % 3600) / 60);
        o->sec = (int32_t)(o->epoch % 60);
        o->year = 1970; o->month = 1; o->day = 1;
        o->weekday = 4; o->yearday = 1;  // 1970-01-01 是周四
    } else {
        struct tm tmv;
        epoch_to_tm(o->epoch, o->tz_offset_min, &tmv);
        o->year = (int32_t)(tmv.tm_year + 1900);
        o->month = (int32_t)(tmv.tm_mon + 1);
        o->day = (int32_t)tmv.tm_mday;
        o->hour = (int32_t)tmv.tm_hour;
        o->min = (int32_t)tmv.tm_min;
        o->sec = (int32_t)tmv.tm_sec;
        o->weekday = (int32_t)tmv.tm_wday;     // 0=周日 .. 6=周六
        o->yearday = (int32_t)(tmv.tm_yday + 1); // 1-366
    }
    o->cached = 1;
}

// 规整 timedelta 的 nsec 到 [0, 1e9) 且与 epoch 同号
static void td_normalize(int64_t* sec, int32_t* nsec) {
    const int32_t NS = 1000000000;
    while(*nsec >= NS) { *nsec -= NS; *sec += 1; }
    while(*nsec < 0)   { *nsec += NS; *sec -= 1; }
}

// 统一构造：分配 DateObj，填充 epoch/nsec/kind/tz，cached=0
static Value date_alloc(int64_t epoch, int32_t nsec, ValueType kind, int32_t tz_offset_min) {
    DateObj* o = (DateObj*)gc_alloc(sizeof(DateObj), kind);
    if(!o) {
        Value z; z.type = VAL_NONE; z.str_inline = 0; return z;
    }
    o->epoch = epoch;
    o->nsec = nsec;
    o->cached = 0;
    o->kind = kind;
    o->tz_offset_min = tz_offset_min;
    o->year = o->month = o->day = 0;
    o->hour = o->min = o->sec = 0;
    o->weekday = o->yearday = 0;
    Value r;
    r.type = kind;
    r.str_inline = 0;
    r.v.date_obj = o;
    return r;
}

// date：epoch 规整到当天 00:00（按时区）
Value lumyr_make_date(int64_t epoch, int32_t tz_offset_min) {
    // 按时区将 epoch 转为本地时间，取当天 00:00
    struct tm tmv;
    epoch_to_tm(epoch, tz_offset_min, &tmv);
    tmv.tm_hour = 0; tmv.tm_min = 0; tmv.tm_sec = 0;
    int64_t midnight = tm_to_epoch(&tmv, tz_offset_min);
    return date_alloc(midnight, 0, VAL_DATE, tz_offset_min);
}

Value lumyr_make_datetime(int64_t epoch, int32_t nsec, int32_t tz_offset_min) {
    return date_alloc(epoch, nsec, VAL_DATETIME, tz_offset_min);
}

// time：当天秒数 [0,86400)
Value lumyr_make_time_obj(int32_t sec, int32_t nsec, int32_t tz_offset_min) {
    while(sec >= 86400) sec -= 86400;
    while(sec < 0)      sec += 86400;
    return date_alloc((int64_t)sec, nsec, VAL_TIME, tz_offset_min);
}

Value lumyr_make_timedelta(int64_t sec, int32_t nsec) {
    td_normalize(&sec, &nsec);
    return date_alloc(sec, nsec, VAL_TIMEDELTA, 0);
}

// ymd → date（按 tz_offset_min 解释）
Value lumyr_make_date_ymd_tz(int y, int mo, int d, int32_t tz_offset_min) {
    struct tm tmv;
    memset(&tmv, 0, sizeof(tmv));
    tmv.tm_year = y - 1900;
    tmv.tm_mon = mo - 1;
    tmv.tm_mday = d;
    int64_t t = tm_to_epoch(&tmv, tz_offset_min);
    return lumyr_make_date(t, tz_offset_min);
}

// ymd-hms-ns → datetime（按 tz_offset_min 解释）
Value lumyr_make_datetime_ymd_tz(int y, int mo, int d, int h, int mi, int s, int ns, int32_t tz_offset_min) {
    struct tm tmv;
    memset(&tmv, 0, sizeof(tmv));
    tmv.tm_year = y - 1900;
    tmv.tm_mon = mo - 1;
    tmv.tm_mday = d;
    tmv.tm_hour = h;
    tmv.tm_min = mi;
    tmv.tm_sec = s;
    int64_t t = tm_to_epoch(&tmv, tz_offset_min);
    return lumyr_make_datetime(t, ns, tz_offset_min);
}

// hms-ns → time
Value lumyr_make_time_hms_tz(int h, int mi, int s, int ns, int32_t tz_offset_min) {
    int32_t sec = (int32_t)(h * 3600 + mi * 60 + s);
    return lumyr_make_time_obj(sec, ns, tz_offset_min);
}

// 兼容旧接口（默认本地时区）
Value lumyr_make_date_ymd(int y, int mo, int d) { return lumyr_make_date_ymd_tz(y, mo, d, TZ_LOCAL); }
Value lumyr_make_datetime_ymd(int y, int mo, int d, int h, int mi, int s, int ns) { return lumyr_make_datetime_ymd_tz(y, mo, d, h, mi, s, ns, TZ_LOCAL); }
Value lumyr_make_time_hms(int h, int mi, int s, int ns) { return lumyr_make_time_hms_tz(h, mi, s, ns, TZ_LOCAL); }

// 当前时间 → VAL_DATETIME（本地时区）
Value lumyr_date_now(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return lumyr_make_datetime((int64_t)tv.tv_sec, (int32_t)(tv.tv_usec * 1000), TZ_LOCAL);
}

// 当天 → VAL_DATE（本地时区）
Value lumyr_date_today(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return lumyr_make_date((int64_t)tv.tv_sec, TZ_LOCAL);
}

// 时区偏移字符串（malloc，调用方 free）："+08:00" / "-05:30" / "Local"
char* lumyr_date_tz_str(int32_t tz_offset_min) {
    if(tz_offset_min == TZ_LOCAL) return strdup("Local");
    char buf[8];
    int sign = tz_offset_min >= 0 ? 1 : -1;
    int abs_min = tz_offset_min < 0 ? -tz_offset_min : tz_offset_min;
    int hh = abs_min / 60;
    int mm = abs_min % 60;
    snprintf(buf, sizeof(buf), "%s%02d:%02d", sign < 0 ? "-" : "+", hh, mm);
    return strdup(buf);
}

// 字段访问：year/month/day/hour/minute/second/weekday/yearday
// timedelta：days/seconds/totalSeconds（未知返回 0）
// 通用：timezone 返回时区偏移字符串
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
    if(strcmp(name, "timezone") == 0) {
        char* s = lumyr_date_tz_str(o->tz_offset_min);
        Value r = lumyr_make_string(s ? s : "");
        free(s);
        return r;
    }

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
        if(strcmp(name, "totalSeconds") == 0) {
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
    char buf[80];
    switch(v.type) {
    case VAL_DATE: {
        date_fill_cache(o);
        snprintf(buf, sizeof(buf), "%04d-%02d-%02d", o->year, o->month, o->day);
        break;
    }
    case VAL_DATETIME: {
        date_fill_cache(o);
        char tzbuf[8] = {0};
        if(o->tz_offset_min != TZ_LOCAL) {
            char* tz = lumyr_date_tz_str(o->tz_offset_min);
            strncpy(tzbuf, tz, sizeof(tzbuf)-1); tzbuf[sizeof(tzbuf)-1] = '\0';
            free(tz);
        }
        if(o->nsec != 0) {
            snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%09d%s",
                o->year, o->month, o->day, o->hour, o->min, o->sec, o->nsec, tzbuf);
        } else {
            snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d%s",
                o->year, o->month, o->day, o->hour, o->min, o->sec, tzbuf);
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

// strftime 风格格式化（按对象时区）
char* lumyr_date_format(Value v, const char* fmt) {
    if(!fmt) return strdup("");
    DateObj* o = (DateObj*)v.v.date_obj;
    if(!o) return strdup("");
    // timedelta 不支持 strftime，返回 ISO
    if(v.type == VAL_TIMEDELTA) return lumyr_date_to_iso(v);
    struct tm tmv;
    memset(&tmv, 0, sizeof(tmv));
    if(v.type == VAL_TIME) {
        // time 的 epoch 是当天秒数，不进行时区转换
        tmv.tm_hour = (int)(o->epoch / 3600);
        tmv.tm_min = (int)((o->epoch % 3600) / 60);
        tmv.tm_sec = (int)(o->epoch % 60);
    } else {
        epoch_to_tm(o->epoch, o->tz_offset_min, &tmv);
    }
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
        return date_alloc(o->epoch + sec, o->nsec, v.type, o->tz_offset_min);
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
        return lumyr_make_time_obj((int32_t)new_epoch, nsec, o->tz_offset_min);
    }
    if(v.type == VAL_DATE) {
        return lumyr_make_date(new_epoch, o->tz_offset_min);
    }
    if(v.type == VAL_DATETIME) {
        return lumyr_make_datetime(new_epoch, nsec, o->tz_offset_min);
    }
    return v;
}
