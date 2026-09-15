/*
 * lumyr_log.c - 统一日志和异常封装实现
 * 支持打印文件名、行号和错误信息主体
 */
#include "lumyr_log.h"
#include <stdarg.h>

/* 全局日志级别（默认 INFO） */
LogLevel g_log_level = LOG_INFO;

/* 设置日志级别 */
void log_set_level(LogLevel level)
{
    g_log_level = level;
}

/* 获取日志级别字符串 */
const char* log_level_str(LogLevel level)
{
    switch(level) {
        case LOG_DEBUG: return "DEBUG";
        case LOG_INFO:  return "INFO";
        case LOG_WARN:  return "WARN";
        case LOG_ERROR: return "ERROR";
        case LOG_FATAL: return "FATAL";
        default:        return "UNKNOWN";
    }
}

/* 统一日志打印函数实现 */
void log_print_impl(LogLevel level, const char* file, int line, const char* fmt, ...)
{
    if(level < g_log_level) return;

    va_list args;
    va_start(args, fmt);

    /* 打印日志级别、文件名、行号 */
    fprintf(stderr, "[%s] %s:%d: ", log_level_str(level), file, line);

    /* 打印错误信息主体 */
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");

    va_end(args);
}

/* 统一异常抛出函数实现 */
void log_throw_impl(const char* file, int line, const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);

    /* 打印错误级别、文件名、行号 */
    fprintf(stderr, "[ERROR] %s:%d: Runtime Error: ", file, line);

    /* 打印错误信息主体 */
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");

    va_end(args);

    /* 退出程序 */
    exit(1);
}
