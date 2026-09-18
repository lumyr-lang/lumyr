/*
 * lumyr_log.c - 统一日志和异常封装实现
 * 支持打印文件名、行号、错误信息主体和调用链
 */
#include "lumyr_log.h"
#include <stdarg.h>

/* 全局日志级别（默认 INFO） */
LogLevel g_log_level = LOG_DEBUG;

/* 全局调用栈 */
LogCallStackEntry g_log_callstack[LOG_CALLSTACK_MAX_DEPTH];
int g_log_callstack_depth = 0;

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

    /* 如果是 ERROR 或 FATAL 级别，打印调用栈 */
    if(level >= LOG_ERROR && g_log_callstack_depth > 0) {
        fprintf(stderr, "  调用链:\n");
        for(int i = g_log_callstack_depth - 1; i >= 0; i--) {
            LogCallStackEntry* entry = &g_log_callstack[i];
            fprintf(stderr, "    #%d %s (%s:%d)", g_log_callstack_depth - 1 - i,
                    entry->func_name, entry->file, entry->line);
            if(entry->args[0] != '\0') {
                fprintf(stderr, " 参数: %s", entry->args);
            }
            fprintf(stderr, "\n");
        }
    }

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

    /* 打印调用栈 */
    if(g_log_callstack_depth > 0) {
        fprintf(stderr, "  调用链:\n");
        for(int i = g_log_callstack_depth - 1; i >= 0; i--) {
            LogCallStackEntry* entry = &g_log_callstack[i];
            fprintf(stderr, "    #%d %s (%s:%d)", g_log_callstack_depth - 1 - i,
                    entry->func_name, entry->file, entry->line);
            if(entry->args[0] != '\0') {
                fprintf(stderr, " 参数: %s", entry->args);
            }
            fprintf(stderr, "\n");
        }
    }

    va_end(args);

    /* 退出程序 */
    exit(1);
}

/* 调用栈管理函数实现 */
void log_push_call_impl(const char* func_name, const char* file, int line, const char* args_fmt, ...)
{
    if(g_log_callstack_depth >= LOG_CALLSTACK_MAX_DEPTH) {
        /* 调用栈已满，忽略 */
        return;
    }

    LogCallStackEntry* entry = &g_log_callstack[g_log_callstack_depth];
    entry->func_name = func_name;
    entry->file = file;
    entry->line = line;
    entry->args[0] = '\0';

    /* 格式化参数信息 */
    if(args_fmt && args_fmt[0] != '\0') {
        va_list args;
        va_start(args, args_fmt);
        vsnprintf(entry->args, sizeof(entry->args), args_fmt, args);
        va_end(args);
    }

    g_log_callstack_depth++;
}

void log_pop_call_impl(void)
{
    if(g_log_callstack_depth > 0) {
        g_log_callstack_depth--;
    }
}

/* 打印调用栈 */
void log_print_callstack(void)
{
    if(g_log_callstack_depth == 0) {
        fprintf(stderr, "  调用链: (空)\n");
        return;
    }

    fprintf(stderr, "  调用链:\n");
    for(int i = g_log_callstack_depth - 1; i >= 0; i--) {
        LogCallStackEntry* entry = &g_log_callstack[i];
        fprintf(stderr, "    #%d %s (%s:%d)", g_log_callstack_depth - 1 - i,
                entry->func_name, entry->file, entry->line);
        if(entry->args[0] != '\0') {
            fprintf(stderr, " 参数: %s", entry->args);
        }
        fprintf(stderr, "\n");
    }
}
