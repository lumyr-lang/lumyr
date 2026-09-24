#include <stdio.h>
#include "lumyr_log.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif

#include "yacc.tab.h"
#include "ast/ast.h"
#include "ir/ir_compile.h"

/* 生成 C 代码的链接配置：由 Makefile 编译 main.o 时注入。
   未通过 Makefile 编译时使用 fallback 默认值（开发调试用）。 */
#ifndef LUMYR_GEN_INC
#define LUMYR_GEN_INC "-Ikit/runtime/include"
#endif
#ifndef LUMYR_GEN_LIB
#define LUMYR_GEN_LIB "-Llib"
#endif
#ifndef LUMYR_GEN_LDLIBS
#ifdef _WIN32
#define LUMYR_GEN_LDLIBS "-lcurl -liconv -ltre -lcrypt32 -lws2_32 -lwldap32 -lwinmm -lnormaliz -liphlpapi -lbcrypt -lsecur32"
#elif defined(__APPLE__)
#define LUMYR_GEN_LDLIBS "-lcurl -liconv -lgmp"
#else
#define LUMYR_GEN_LDLIBS "-lcurl -lm -lpthread -lgmp"
#endif
#endif
#include "ir/vm.h"
#include "ir/ir_cgen.h"
#include "parse/import.h"
#include "parse/cond_compile.h"
#include "i18n/lm_i18n.h"
#include "ast/ast_runtime_sym.h"

extern AstNode* root;
extern int yyparse(void);
extern FILE *yyin;
void yyrestart(FILE *f);

// 从路径中提取文件名，去掉扩展名，作为默认输出 basename
static void default_basename(const char* path, char* out, int size) {
    const char* name = strrchr(path, '/');
    name = name ? name + 1 : path;

    strncpy(out, name, size - 1);
    out[size - 1] = '\0';

    char* dot = strrchr(out, '.');
    if(dot) *dot = '\0';
}

/* 反汇编回调函数：用于红黑树遍历反汇编所有函数 */
static void disasm_cb(const char* class_name, const char* method_name, void* data, void* user_data)
{
    (void)class_name; (void)method_name;
    bc_disasm((FILE*)user_data, (BytecodeFunc*)data);
}

int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    /* 设置 stdout 为无缓冲模式，确保 print 输出立即刷新 */
    setvbuf(stdout, NULL, _IONBF, 0);
    
    int codegen_mode = 0;
    int only_emit_c = 0;

    /* Initialize i18n - auto-detect system language */
    lm_i18n_init();
    sym_init();

    const char* src_file = NULL;
    const char* out_base = NULL;
    int i = 1;

    while(i < argc) {
        if(strcmp(argv[i], "-c") == 0) {
            codegen_mode = 1;
            i++;
        } else if(strcmp(argv[i], "-S") == 0) {
            codegen_mode = 1;
            only_emit_c = 1;
            i++;
        } else if(strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            out_base = argv[i + 1];
            i += 2;
        } else if(strncmp(argv[i], "-D", 2) == 0) {
            /* -D 自定义条件编译符号：支持 -DNAME 与 -D NAME 两种写法 */
            const char* def = argv[i] + 2;
            if(*def == '\0' && i + 1 < argc) { def = argv[i + 1]; i++; }
            if(*def) lm_cond_add_define(def);
            i++;
        } else {
            src_file = argv[i];
            i++;
        }
    }

    if(!src_file) {
        fprintf(stderr, LM_TR(MSG_USAGE), argv[0]);
        fprintf(stderr, LM_TR(MSG_USAGE_DEFAULT));
        fprintf(stderr, LM_TR(MSG_USAGE_C));
        fprintf(stderr, LM_TR(MSG_USAGE_S));
        fprintf(stderr, LM_TR(MSG_USAGE_O));
        return 1;
    }

    /* 模块系统（第一阶段）：yyparse 之前做文本预处理，把 import 模块内联、
       把 export 符号收集成 map。无模块语法的源文件走原始 fopen 路径，行为不变。 */
    char pp_tmp[PATH_MAX] = {0};   /* 预处理临时文件路径（若使用） */
    int used_pp_tmp = 0;
    char cond_tmp[PATH_MAX] = {0}; /* 条件编译临时文件路径（若使用） */
    int used_cond_tmp = 0;

    /* 条件编译：设置通道（vm/cc 符号与 channel()/targetLang() 字面量） */
    lm_cond_set_channel(codegen_mode);

    int had_mod = 0;
    char* merged = lm_preprocess_main(src_file, &had_mod);
    if(had_mod < 0) {
        fprintf(stderr, LM_TR(MSG_MODULE_PREPROCESS_FAIL));
        return 1;
    }

    if(had_mod == 0) {
        /* 无模块语法：主文件仍需过条件编译（含 import 的文件已在 import.c 内过滤） */
        int cerr = 0;
        char* filtered = lm_cond_filter_file(src_file, &cerr);
        if(cerr) return 1;
        if(filtered) {
#ifdef _WIN32
            const char* win_tmp = getenv("TEMP");
            if (!win_tmp) win_tmp = getenv("TMP");
            if (!win_tmp) win_tmp = ".";
            snprintf(cond_tmp, sizeof(cond_tmp), "%s/lumyr_cc_XXXXXX", win_tmp);
#else
            snprintf(cond_tmp, sizeof(cond_tmp), "%s/lumyr_cc_XXXXXX", P_tmpdir ? P_tmpdir : "/tmp");
#endif
            int cfd = mkstemp(cond_tmp);
            if(cfd < 0) {
                perror("mkstemp");
                free(filtered);
                return 1;
            }
            if(write(cfd, filtered, strlen(filtered)) < 0) {
                perror("write cond tmp");
                close(cfd);
                unlink(cond_tmp);
                free(filtered);
                return 1;
            }
            close(cfd);
            free(filtered);
            used_cond_tmp = 1;
        }
        /* 注意：不改写 src_file——CC 通道 default_basename 依赖原始源文件名 */
        yyin = fopen(used_cond_tmp ? cond_tmp : src_file, "r");
        if(!yyin) {
            perror("open file failed");
            return 1;
        }
        yyrestart(yyin);
    } else {
        /* 把合并后的源码写入临时文件，再交给 parser（对 VM / 编译通道完全一致） */
#ifdef _WIN32
        const char* win_tmp = getenv("TEMP");
        if (!win_tmp) win_tmp = getenv("TMP");
        if (!win_tmp) win_tmp = ".";
        snprintf(pp_tmp, sizeof(pp_tmp), "%s/lumyr_pp_XXXXXX", win_tmp);
#else
        snprintf(pp_tmp, sizeof(pp_tmp), "%s/lumyr_pp_XXXXXX", P_tmpdir ? P_tmpdir : "/tmp");
#endif
        int fd = mkstemp(pp_tmp);
        if(fd < 0) {
            perror("mkstemp");
            free(merged);
            return 1;
        }
        if(write(fd, merged, strlen(merged)) < 0) {
            perror("write pp tmp");
            close(fd);
            unlink(pp_tmp);
            free(merged);
            return 1;
        }
        close(fd);
        free(merged);

        yyin = fopen(pp_tmp, "r");
        if(!yyin) {
            perror("open pp tmp failed");
            unlink(pp_tmp);
            return 1;
        }
        used_pp_tmp = 1;
        yyrestart(yyin);
    }

    int ret = yyparse();
    if(ret == 0 && root != NULL) {
        if(ast_typecheck(root)) {
            fprintf(stderr, LM_TR(MSG_SEMANTIC_CHECK_FAIL));
            ast_free(root);
            root = NULL;
            ret = 1;
        } else if(codegen_mode) {
            if(only_emit_c) {
                // -S：输出字节码指令文本（反汇编）
                BytecodeFunc* main_fn = ir_compile_main(root);
                /* 用红黑树遍历反汇编所有函数 */
                ir_func_table_foreach(disasm_cb, stdout);
                bc_disasm(stdout, main_fn);
                bytecode_func_free(main_fn);
                ret = 0;
            } else {
                char base[PATH_MAX];
                if(out_base) {
                    strncpy(base, out_base, sizeof(base) - 1);
                    base[sizeof(base) - 1] = '\0';
                } else {
                    default_basename(src_file, base, sizeof(base));
                }

                char c_path[PATH_MAX];
                char exe_path[PATH_MAX];
                // 输出路径由用户通过 -o 指定，默认取源文件名（当前目录）
                snprintf(c_path, sizeof(c_path), "%s.c", base);
                snprintf(exe_path, sizeof(exe_path), "%s", base);

                BytecodeFunc* main_fn = ir_compile_main(root);
                ir_cgen_file(c_path, main_fn);
                bytecode_func_free(main_fn);
                printf(LM_TR(MSG_CODEGEN_GENERATED), c_path);

                if(!only_emit_c) {
                    char cmd[PATH_MAX * 2];
                    /* 允许通过环境变量覆盖生成代码的编译器/ flags（用于 ASan 等调试） */
                    const char* gen_cc = getenv("LM_GEN_CC");
                    const char* gen_cflags = getenv("LM_GEN_CFLAGS");
                    if(!gen_cc) gen_cc = "gcc";
                    if(!gen_cflags) gen_cflags = "-O2";
                    /* 链接配置由 Makefile 注入（GEN_INC/GEN_LIB/LDLIBS），消除跨平台硬编码 */
                    snprintf(cmd, sizeof(cmd),
                             "%s -std=gnu11 %s %s %s %s -o %s -lruntime %s",
                             gen_cc, gen_cflags, LUMYR_GEN_INC, LUMYR_GEN_LIB,
                             c_path, exe_path, LUMYR_GEN_LDLIBS);
                    int sys_ret = system(cmd);

                    if(sys_ret == 0) {
                        printf(LM_TR(MSG_CODEGEN_COMPILED), exe_path);
                    } else {
                        fprintf(stderr, LM_TR(MSG_CODEGEN_COMPILE_FAIL), gen_cc);
                        return 1;
                    }
                }
            }
        } else {
            // 字节码 VM 执行（AST → IR → vm）
            BytecodeFunc* main_fn = ir_compile_main(root);
            /* IR 致命错误（如调用不存在的方法）：中止，不运行 VM */
            if(ir_compile_had_error()) {
                bytecode_func_free(main_fn);
                ast_free(root);
                root = NULL;
                ret = 1;
                if(yyin && yyin != stdin) fclose(yyin);
                if(used_pp_tmp) unlink(pp_tmp);
                if(used_cond_tmp) unlink(cond_tmp);
                return 1;
            }
            vm_run_main(main_fn);
            bytecode_func_free(main_fn);
        }
        ast_free(root);
    }

    if(yyin && yyin != stdin) {
        fclose(yyin);
    }
    if(used_pp_tmp) unlink(pp_tmp);
    if(used_cond_tmp) unlink(cond_tmp);
    return ret;
}
