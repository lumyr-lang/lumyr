// lm_file.h —— 文件对象（VAL_FILE）与目录对象（VAL_FOLDER）
// 不持有 FILE* 句柄：每次方法调用 fopen/fclose，避免 GC 回收时的资源泄漏
#ifndef LM_FILE_H
#define LM_FILE_H

#include "lm_value.h"

// ===== file 类型 =====
// 构造：file("path" [, "r"|"w"|"a"]) 或 <file>"path"
// mode 字符串："r" 只读（默认）/ "w" 覆盖写 / "a" 追加
Value lumyr_file_make(const char* path, const char* mode);

// 字段访问（统一入口）
Value lumyr_file_field(Value v, const char* name);

// ISO/字符串化（malloc，调用方 free）
char* lumyr_file_to_str(Value v);

// 方法
Value lumyr_file_read_all(Value v);
Value lumyr_file_read_lines(Value v);
Value lumyr_file_read_line(Value v, int64_t line_no);
Value lumyr_file_read_lines_range(Value v, int64_t from, int64_t to);
Value lumyr_file_write_all(Value v, const char* content);
Value lumyr_file_write_line(Value v, int64_t line_no, const char* content);
Value lumyr_file_write_lines(Value v, Value arr);
Value lumyr_file_append(Value v, const char* content);
Value lumyr_file_append_line(Value v, const char* content);
Value lumyr_file_flush(Value v);
Value lumyr_file_delete(Value v);

// ===== folder 类型 =====
// 构造：folder("path") 或 <folder>"path"
Value lumyr_folder_make(const char* path);

// 字段访问（统一入口）
Value lumyr_folder_field(Value v, const char* name);

// ISO/字符串化（malloc，调用方 free）
char* lumyr_folder_to_str(Value v);

// 方法
Value lumyr_folder_list(Value v);
Value lumyr_folder_files(Value v);
Value lumyr_folder_dirs(Value v);
Value lumyr_folder_create(Value v);
Value lumyr_folder_remove(Value v);
Value lumyr_folder_walk(Value v);
Value lumyr_folder_copy_to(Value v, const char* dest);
Value lumyr_folder_move_to(Value v, const char* dest);

#endif // LM_FILE_H
