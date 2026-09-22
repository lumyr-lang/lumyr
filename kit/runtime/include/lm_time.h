// lm_time.h —— 日期时间 + 日志
#ifndef LM_TIME_H
#define LM_TIME_H

#include "lm_value.h"

// ===== 日期时间 =====
// now()：当前时间 map {year,month,day,hour,minute,second,weekday,yday,isdst}
Value lumyr_now(void);
// timestamp()：Unix 时间戳（秒，double）
double lumyr_timestamp(void);
// timestamp_ms()：Unix 时间戳（毫秒，long long）
long long lumyr_timestamp_ms(void);
// sleep_ms(ms)：休眠毫秒
void lumyr_sleep_ms(long long ms);
// date_str()："2026-09-07"
char* lumyr_date_str(void);
// time_str()："15:30:45"
char* lumyr_time_str(void);
// datetime_str()："2026-09-07 15:30:45"
char* lumyr_datetime_str(void);
// format_time(fmt, ts?)：按 strftime 格式化；ts 为 -1 用当前时间
char* lumyr_format_time(const char* fmt, double ts);

// ===== 日志 =====
// log_level: 0=debug, 1=info, 2=warn, 3=error, 4=fatal
void lumyr_log(int level, const char* msg);

// ===== date 族对象（VAL_DATE/VAL_DATETIME/VAL_TIME/VAL_TIMEDELTA） =====
// 双模式存储：epoch 秒为主 + 缓存字段懒计算（gmtime_r，UTC）
// 构造（epoch 秒）
Value lumyr_make_date(int64_t epoch);              // date：epoch 规整到当天 00:00 (UTC)
Value lumyr_make_datetime(int64_t epoch, int32_t nsec);
Value lumyr_make_time_obj(int32_t sec, int32_t nsec);   // time：当天秒数 [0,86400)
Value lumyr_make_timedelta(int64_t sec, int32_t nsec); // timedelta：可负，nsec 规整到 [0,1e9) 同号
// 构造（日历字段，UTC，用 timegm）
Value lumyr_make_date_ymd(int y, int mo, int d);
Value lumyr_make_datetime_ymd(int y, int mo, int d, int h, int mi, int s, int ns);
Value lumyr_make_time_hms(int h, int mi, int s, int ns);
// 当前时间对象
Value lumyr_date_now(void);      // → VAL_DATETIME（当前 UTC）
Value lumyr_date_today(void);    // → VAL_DATE（UTC 当天）
// 字段访问：year/month/day/hour/minute/second/weekday/yearday；
// timedelta：days/seconds/total_seconds（统一入口，未知返回 0）
Value lumyr_date_field(Value v, const char* name);
// ISO 字符串（value_to_str/print 用，malloc 返回）
char* lumyr_date_to_iso(Value v);
// 格式化：strftime 风格（%Y %m %d %H %M %S 等，UTC）
char* lumyr_date_format(Value v, const char* fmt);
// diff(a, b) = a - b → VAL_TIMEDELTA
Value lumyr_date_diff(Value a, Value b);
// add(v, n, unit)：v + n*unit，返回同类型新对象
//   unit：second(s)/minute(s)/hour(s)/day(s)/week(s)/month(s)/year(s)
//   month/year 对 date/datetime 走日历，其他按秒近似
Value lumyr_date_add(Value v, int64_t n, const char* unit);

#endif // LM_TIME_H
