// lm_calendar.h —— 综合日历对象（VAL_CALENDAR）
// 月视图 + 农历转换 + 日历算术
#ifndef LM_CALENDAR_H
#define LM_CALENDAR_H

#include "lm_value.h"

// 构造：calendar(year, month [, tz]) 或 calendar(date)
Value lumyr_calendar_make(int year, int month, int32_t tz_offset_min);
Value lumyr_calendar_from_date(Value date_val, int32_t tz_offset_min);

// 字段访问（统一入口）
Value lumyr_calendar_field(Value v, const char* name);

// ISO/字符串化（malloc，调用方 free）
char* lumyr_calendar_to_str(Value v);

// 方法
Value lumyr_calendar_add(Value v, int64_t n, const char* unit);
Value lumyr_calendar_first_date(Value v);
Value lumyr_calendar_last_date(Value v);
Value lumyr_calendar_contains(Value v, Value date_val);

#endif // LM_CALENDAR_H
