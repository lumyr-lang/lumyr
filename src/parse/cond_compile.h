/*
 * lumyr 条件编译（文本预处理 pass，yyparse 之前执行）
 *
 * 能力：
 *   1. #if/#elif/#else/#endif/#ifdef/#ifndef 指令求值，死分支文本移除
 *      （替换为等量空行，保证行号不漂移）；
 *   2. 平台查询字面量替换：platform()/arch()/formFactor()/channel()/targetLang()
 *      → 编译器构建期确定的字符串字面量（零运行时开销，VM/CC 双通道一致）；
 *   3. -D 自定义符号（main.c 在预处理前注册）。
 *
 * 预定义符号（编译器构建期经 C 宏探测）：
 *   OS：macos/windows/linux/ios/android/freebsd/browser
 *   形态：desktop/mobile/browser/mcu
 *   通用架构：x86_64/arm64/arm/x86/wasm32/riscv64/riscv32
 *   国产架构：loongarch64/loongarch32（龙芯）/sw64（申威）
 *   单片机架构：avr/msp430/pic/mcs51/xtensa/cortexm（命中即定义 mcu）
 *   位宽：bits64/bits32/bits16/bits8
 *   通道：vm/cc（由 lm_cond_set_channel 设置）
 *   语言：cc 通道时定义 c（未来 js/java/go/objectivec/css 后端在此扩展）
 *
 * 已知限制（与 C 宏语义一致，文档化）：
 *   platform/arch/formFactor/channel/targetLang 视同保留字——
 *   用户定义的同名函数（func platform()）会被一并替换。
 */
#ifndef LM_COND_COMPILE_H
#define LM_COND_COMPILE_H

/* 设置编译通道：0=VM，1=CC。影响 vm/cc 符号与 channel()/targetLang() 字面量。
 * 必须在任何 filter 调用之前设置。 */
void lm_cond_set_channel(int is_cc);

/* 注册 -D 自定义符号（预处理前调用；name 须为合法标识符，否则忽略并告警）。 */
void lm_cond_add_define(const char* name);

/* 快速预扫描：文本是否可能含条件编译触发内容（#if/#ifdef/platform( 等）。 */
int lm_cond_might_have(const char* text);

/* 过滤文本：求值 #if 指令 + 字面量替换。
 *   text：源文本（NUL 结尾）；path：用于错误信息的路径（可为 NULL）。
 *   err_out：输出标志，1=出错（错误已打印 stderr），0=正常。
 * 返回：有改动 → malloc'd 新文本；无改动 → NULL（调用方继续用原文）。 */
char* lm_cond_filter_text(const char* text, const char* path, int* err_out);

/* 便捷包装：读文件并过滤（内部 slurp）。语义同 lm_cond_filter_text。 */
char* lm_cond_filter_file(const char* path, int* err_out);

#endif
