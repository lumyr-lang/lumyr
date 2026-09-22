// lm_calendar.c —— 综合日历对象（VAL_CALENDAR）
// 月视图 + 农历转换 + 日历算术
#include "lm_calendar.h"
#include "gc_runtime.h"
#include "lm_time.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

// 哨兵值：表示"本地电脑时区"
#define TZ_LOCAL INT32_MIN

// ===== 公历（Gregorian）辅助 =====

static int is_leap_year(int year) {
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

static int days_in_month(int year, int month) {
    static const int d[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (month < 1 || month > 12) return 0;
    if (month == 2 && is_leap_year(year)) return 29;
    return d[month - 1];
}

// Howard Hinnant 的 civil→days 算法：返回自 1970-01-01 的天数（可为负）
// 1970-01-01 = 周四；用于 day_of_week 与农历基准换算
static int64_t days_from_civil(int y, int m, int d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int yoe = (int)(y - era * 400);
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

// 0=周日 .. 6=周六
static int day_of_week(int year, int month, int day) {
    int64_t days = days_from_civil(year, month, day);
    // 1970-01-01 = 周四 = 4（0=周日）
    int w = (int)(((days % 7) + 4 + 7) % 7);
    return w;
}

static int quarter_of(int month) {
    if (month < 1) return 1;
    if (month > 12) return 4;
    return (month - 1) / 3 + 1;
}

// 月份名：英文 / 中文
static const char* month_name(int month, int chinese) {
    static const char* en[] = {"January","February","March","April","May","June",
                               "July","August","September","October","November","December"};
    static const char* zh[] = {"一月","二月","三月","四月","五月","六月",
                               "七月","八月","九月","十月","十一月","十二月"};
    if (month < 1 || month > 12) return chinese ? "未知" : "Unknown";
    return chinese ? zh[month - 1] : en[month - 1];
}

// ===== 农历（Chinese lunar）转换 =====
// lunar_info 编码（每年一个 32 位字）：
//   bits 0-3   : 闰月月份（0=无闰月）
//   bits 4-15  : 12 个月大小（1=30 天，0=29 天），bit4=12月, ..., bit15=1月
//   bit  16    : 闰月天数（1=30，0=29）
// 表覆盖 1900-2109（210 项，超出 2099 的 10 项为安全冗余）
static const uint32_t lunar_info[] = {
    0x04bd8,0x04ae0,0x0a570,0x054d5,0x0d260,0x0d950,0x16554,0x056a0,0x09ad0,0x055d2, //1900-1909
    0x04ae0,0x0a5b6,0x0a4d0,0x0d250,0x1d255,0x0b540,0x0d6a0,0x0ada2,0x095b0,0x14977, //1910-1919
    0x04970,0x0a4b0,0x0b4b5,0x06a50,0x06d40,0x1ab54,0x02b60,0x09570,0x052f2,0x04970, //1920-1929
    0x06566,0x0d4a0,0x0ea50,0x06e95,0x05ad0,0x02b60,0x186e3,0x092e0,0x1c8d7,0x0c950, //1930-1939
    0x0d4a0,0x1d8a6,0x0b550,0x056a0,0x1a5b4,0x025d0,0x092d0,0x0d2b2,0x0a950,0x0b557, //1940-1949
    0x06ca0,0x0b550,0x15355,0x04da0,0x0a5b0,0x14573,0x052b0,0x0a9a8,0x0e950,0x06aa0, //1950-1959
    0x0aea6,0x0ab50,0x04b60,0x0aae4,0x0a570,0x05260,0x0f263,0x0d950,0x05b57,0x056a0, //1960-1969
    0x096d0,0x04dd5,0x04ad0,0x0a4d0,0x0d4d4,0x0d250,0x0d558,0x0b540,0x0b6a0,0x195a6, //1970-1979
    0x095b0,0x049b0,0x0a974,0x0a4b0,0x0b27a,0x06a50,0x06d40,0x0af46,0x0ab60,0x09570, //1980-1989
    0x04af5,0x04970,0x064b0,0x074a3,0x0ea50,0x06b58,0x05ac0,0x0ab60,0x096d5,0x092e0, //1990-1999
    0x0c960,0x0d954,0x0d4a0,0x0da50,0x07552,0x056a0,0x0abb7,0x025d0,0x092d0,0x0cab5, //2000-2009
    0x0a950,0x0b4a0,0x0baa4,0x0ad50,0x055d9,0x04ba0,0x0a5b0,0x15176,0x052b0,0x0a930, //2010-2019
    0x07954,0x06aa0,0x0ad50,0x05b52,0x04b60,0x0a6e6,0x0a4e0,0x0d260,0x0ea65,0x0d530, //2020-2029
    0x05aa0,0x076a3,0x096d0,0x04afb,0x04ad0,0x0a4d0,0x1d0b6,0x0d250,0x0d520,0x0dd45, //2030-2039
    0x0b5a0,0x056d0,0x055b2,0x049b0,0x0a577,0x0a4b0,0x0aa50,0x1b255,0x06d20,0x0ada0, //2040-2049
    0x14b63,0x09370,0x049f8,0x04970,0x064b0,0x168a6,0x0ea50,0x06b20,0x1a6c4,0x0aae0, //2050-2059
    0x0a2e0,0x0d2e3,0x0c960,0x0d557,0x0d4a0,0x0da50,0x05d55,0x056a0,0x0a6d0,0x055d4, //2060-2069
    0x052d0,0x0a9b8,0x0a950,0x0b4a0,0x0b6a6,0x0ad50,0x055a0,0x0aba4,0x0a5b0,0x052b0, //2070-2079
    0x0b273,0x06930,0x07337,0x06aa0,0x0ad50,0x14b55,0x04b60,0x0a570,0x054e4,0x0d160, //2080-2089
    0x0e968,0x0d520,0x0daa0,0x16aa6,0x056d0,0x04ae0,0x0a9d4,0x0a2d0,0x0d150,0x0f252, //2090-2099
    0x0d520,0x0d520,0x0d520,0x0d520,0x0d520,0x0d520,0x0d520,0x0d520,0x0d520,0x0d520  //2100-2109 (extra for safety)
};
#define LUNAR_MIN_YEAR 1900
#define LUNAR_MAX_YEAR 2099

static int lunar_leap_month(int year) {
    if (year < LUNAR_MIN_YEAR || year > LUNAR_MAX_YEAR) return 0;
    return (int)(lunar_info[year - LUNAR_MIN_YEAR] & 0xfU);
}

static int lunar_leap_days(int year) {
    if (year < LUNAR_MIN_YEAR || year > LUNAR_MAX_YEAR) return 0;
    if (lunar_leap_month(year) == 0) return 0;
    return (lunar_info[year - LUNAR_MIN_YEAR] & 0x10000U) ? 30 : 29;
}

static int lunar_month_days(int year, int month) {
    if (year < LUNAR_MIN_YEAR || year > LUNAR_MAX_YEAR) return 29;
    if (month < 1 || month > 12) return 29;
    return (lunar_info[year - LUNAR_MIN_YEAR] & (0x10000U >> month)) ? 30 : 29;
}

static int lunar_year_days(int year) {
    if (year < LUNAR_MIN_YEAR || year > LUNAR_MAX_YEAR) return 0;
    uint32_t info = lunar_info[year - LUNAR_MIN_YEAR];
    int sum = 348;  // 12 * 29
    for (uint32_t i = 0x8000; i > 0x8U; i >>= 1) {
        if (info & i) sum++;
    }
    return sum + lunar_leap_days(year);
}

// 公历→农历：基准 1900-01-31 = 农历 1900-01-01（正月初一）
// 算法：先算相对基准的天数 offset，逐年减去年天数定位年，再逐月减定位月。
static void solar_to_lunar(int year, int month, int day,
                           int* lyear, int* lmonth, int* lday, int* lleap) {
    *lyear = year; *lmonth = 1; *lday = 1; *lleap = 0;
    if (year < LUNAR_MIN_YEAR || year > LUNAR_MAX_YEAR) return;
    int64_t offset = days_from_civil(year, month, day) - days_from_civil(1900, 1, 31);
    if (offset < 0) return;
    int y;
    for (y = LUNAR_MIN_YEAR; y <= LUNAR_MAX_YEAR; y++) {
        int64_t yd = lunar_year_days(y);
        if (offset < yd) break;
        offset -= yd;
    }
    if (y > LUNAR_MAX_YEAR) y = LUNAR_MAX_YEAR;
    *lyear = y;
    int leap = lunar_leap_month(y);
    int found = 0;
    for (int m = 1; m <= 12; m++) {
        int mdays = lunar_month_days(y, m);
        if (offset < mdays) {
            *lmonth = m; *lday = (int)offset + 1; *lleap = 0; found = 1; break;
        }
        offset -= mdays;
        // 闰月紧跟在正常月份 m 之后
        if (leap == m) {
            int ld = lunar_leap_days(y);
            if (offset < ld) {
                *lmonth = m; *lday = (int)offset + 1; *lleap = 1; found = 1; break;
            }
            offset -= ld;
        }
    }
    if (!found) {
        *lmonth = 12; *lday = (int)offset + 1; *lleap = 0;
    }
}

// 干支：2 字 CJK 字符串（如 "丙午"）
static void ganzhi_str(int year, char* out, size_t n) {
    static const char* stems[]    = {"甲","乙","丙","丁","戊","己","庚","辛","壬","癸"};
    static const char* branches[] = {"子","丑","寅","卯","辰","巳","午","未","申","酉","戌","亥"};
    int idx = (int)((((int64_t)(year - 4) % 60) + 60) % 60);
    snprintf(out, n, "%s%s", stems[idx % 10], branches[idx % 12]);
}

// 生肖：单字 CJK（如 "马"）
static const char* zodiac_str(int year) {
    static const char* z[] = {"鼠","牛","虎","兔","龙","蛇","马","羊","猴","鸡","狗","猪"};
    int idx = (int)((((int64_t)(year - 4) % 12) + 12) % 12);
    return z[idx];
}

// ===== 值辅助 =====

// 从各种整数 Value 提取 int（lumyr_date_field 返回 VAL_INT，这里做防御性兼容）
static int value_to_int(Value v) {
    switch (v.type) {
    case VAL_INT:       return v.v.i;
    case VAL_INT8:      return (int)v.v.i8;
    case VAL_INT16:     return (int)v.v.i16;
    case VAL_INT32:     return (int)v.v.i32;
    case VAL_INT64:     return (int)v.v.i64;
    case VAL_LONG_LONG: return (int)v.v.ll;
    case VAL_LONG:      return (int)v.v.l;
    case VAL_BOOL:      return v.v.b ? 1 : 0;
    case VAL_CHAR:      return (int)v.v.c;
    default:            return 0;
    }
}

// 构造 weeks 字段：6 行 × 7 列网格，0=空，1-31=日号
static Value build_weeks(int year, int month) {
    int dim = days_in_month(year, month);
    int first = day_of_week(year, month, 1);
    Value grid = val_array(0);
    for (int r = 0; r < 6; r++) {
        Value row = val_array(0);
        for (int c = 0; c < 7; c++) {
            int idx = r * 7 + c;
            int day = idx - first + 1;
            lumyr_array_add(&row, (day >= 1 && day <= dim) ? lumyr_make_int(day) : lumyr_make_int(0));
        }
        lumyr_array_add(&grid, row);
    }
    return grid;
}

// ===== 公共 API =====

Value lumyr_calendar_make(int year, int month, int32_t tz_offset_min) {
    CalendarObj* o = (CalendarObj*)gc_alloc(sizeof(CalendarObj), VAL_CALENDAR);
    if (!o) {
        Value z; z.type = VAL_NONE; z.str_inline = 0; return z;
    }
    o->year = year;
    o->month = month;
    o->tz_offset_min = tz_offset_min;
    Value r;
    r.type = VAL_CALENDAR;
    r.str_inline = 0;
    r.v.calendar_obj = o;
    return r;
}

Value lumyr_calendar_from_date(Value date_val, int32_t tz_offset_min) {
    int y = value_to_int(lumyr_date_field(date_val, "year"));
    int m = value_to_int(lumyr_date_field(date_val, "month"));
    return lumyr_calendar_make(y, m, tz_offset_min);
}

Value lumyr_calendar_field(Value v, const char* name) {
    if (!name) return lumyr_make_int(0);
    if (v.type != VAL_CALENDAR) return lumyr_make_int(0);
    CalendarObj* o = (CalendarObj*)v.v.calendar_obj;
    if (!o) return lumyr_make_int(0);

    if (strcmp(name, "year") == 0)         return lumyr_make_int(o->year);
    if (strcmp(name, "month") == 0)        return lumyr_make_int(o->month);
    if (strcmp(name, "daysInMonth") == 0)  return lumyr_make_int(days_in_month(o->year, o->month));
    if (strcmp(name, "firstWeekday") == 0) return lumyr_make_int(day_of_week(o->year, o->month, 1));
    if (strcmp(name, "leapYear") == 0)     return lumyr_make_bool(is_leap_year(o->year));
    if (strcmp(name, "quarter") == 0)      return lumyr_make_int(quarter_of(o->month));
    if (strcmp(name, "monthName") == 0) {
        return lumyr_make_string(month_name(o->month, 0));
    }
    if (strcmp(name, "weeks") == 0)         return build_weeks(o->year, o->month);
    if (strcmp(name, "timezone") == 0) {
        char* s = lumyr_date_tz_str(o->tz_offset_min);
        Value r = lumyr_make_string(s ? s : "");
        free(s);
        return r;
    }

    // 农历字段（懒计算）
    if (strcmp(name, "lunarYear") == 0 || strcmp(name, "lunarMonth") == 0 ||
        strcmp(name, "lunarDay") == 0 || strcmp(name, "lunarLeapMonth") == 0) {
        int ly, lm, ld, lleap;
        solar_to_lunar(o->year, o->month, 1, &ly, &lm, &ld, &lleap);
        if (strcmp(name, "lunarYear") == 0)       return lumyr_make_int(ly);
        if (strcmp(name, "lunarMonth") == 0)      return lumyr_make_int(lm);
        if (strcmp(name, "lunarDay") == 0)        return lumyr_make_int(ld);
        if (strcmp(name, "lunarLeapMonth") == 0)  return lumyr_make_bool(lleap != 0);
    }
    if (strcmp(name, "zodiac") == 0) {
        return lumyr_make_string(zodiac_str(o->year));
    }
    if (strcmp(name, "ganzhi") == 0) {
        char buf[16];
        ganzhi_str(o->year, buf, sizeof(buf));
        return lumyr_make_string(buf);
    }
    return lumyr_make_int(0);
}

char* lumyr_calendar_to_str(Value v) {
    if (v.type != VAL_CALENDAR) return strdup("");
    CalendarObj* o = (CalendarObj*)v.v.calendar_obj;
    if (!o) return strdup("");
    char buf[64];
    snprintf(buf, sizeof(buf), "%s %d", month_name(o->month, 0), o->year);
    return strdup(buf);
}

Value lumyr_calendar_add(Value v, int64_t n, const char* unit) {
    if (!unit) return v;
    if (v.type != VAL_CALENDAR) return v;
    CalendarObj* o = (CalendarObj*)v.v.calendar_obj;
    if (!o) return v;

    int is_months = (strcmp(unit, "month") == 0 || strcmp(unit, "months") == 0);
    int is_years  = (strcmp(unit, "year")  == 0 || strcmp(unit, "years")  == 0);
    if (!is_months && !is_years) return v;

    int64_t months = is_years ? n * 12 : n;
    int64_t total = (int64_t)o->year * 12 + (o->month - 1) + months;
    int64_t ny = total / 12;
    int64_t rem = total % 12;
    if (rem < 0) { ny -= 1; rem += 12; }  // C 截断转下取整
    return lumyr_calendar_make((int)ny, (int)rem + 1, o->tz_offset_min);
}

Value lumyr_calendar_first_date(Value v) {
    if (v.type != VAL_CALENDAR) return val_none();
    CalendarObj* o = (CalendarObj*)v.v.calendar_obj;
    if (!o) return val_none();
    return lumyr_make_date_ymd_tz(o->year, o->month, 1, o->tz_offset_min);
}

Value lumyr_calendar_last_date(Value v) {
    if (v.type != VAL_CALENDAR) return val_none();
    CalendarObj* o = (CalendarObj*)v.v.calendar_obj;
    if (!o) return val_none();
    int dim = days_in_month(o->year, o->month);
    return lumyr_make_date_ymd_tz(o->year, o->month, dim, o->tz_offset_min);
}

Value lumyr_calendar_contains(Value v, Value date_val) {
    if (v.type != VAL_CALENDAR) return lumyr_make_bool(0);
    CalendarObj* o = (CalendarObj*)v.v.calendar_obj;
    if (!o) return lumyr_make_bool(0);
    int y = value_to_int(lumyr_date_field(date_val, "year"));
    int m = value_to_int(lumyr_date_field(date_val, "month"));
    return lumyr_make_bool(y == o->year && m == o->month);
}
