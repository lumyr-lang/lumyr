// lm_xml.c —— xml(s) 内置函数：XML 文本 → 值（DOM 式 map 树）
// 递归下降解析，产出 { tagName, attributes, textContent, children }
// 双通道共享（VM 与 C 编译通道都调用本模块）
#include "lm_value.h"
#include "lm_charset.h"
#include "lm_container.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

typedef struct {
    const char* p;
    const char* end;
    int error;
} XP;

/* ---- 工具 ---- */
static void xp_ws(XP* x) {
    while(x->p < x->end) {
        char c = *x->p;
        if(c == ' ' || c == '\t' || c == '\r' || c == '\n') x->p++;
        else break;
    }
}

static int xp_peek(XP* x, const char* s) {
    size_t n = strlen(s);
    if((size_t)(x->end - x->p) < n) return 0;
    return memcmp(x->p, s, n) == 0;
}

static void xp_skip(XP* x, size_t n) { x->p += n; }

/* 跳过注释 <!-- ... --> */
static int xp_skip_comment(XP* x) {
    if(!xp_peek(x, "<!--")) return 0;
    const char* q = x->p + 4;
    while(q + 2 < x->end) {
        if(q[0] == '-' && q[1] == '-' && q[2] == '>') { x->p = q + 3; return 1; }
        q++;
    }
    x->error = 1; return 0;
}

/* 跳过 <? ... ?>（XML 声明 / 处理指令） */
static int xp_skip_pi(XP* x) {
    if(!xp_peek(x, "<?")) return 0;
    const char* q = x->p + 2;
    while(q + 1 < x->end) {
        if(q[0] == '?' && q[1] == '>') { x->p = q + 2; return 1; }
        q++;
    }
    x->error = 1; return 0;
}

/* 跳过 <!DOCTYPE ...>（简化：直到 '>'） */
static int xp_skip_doctype(XP* x) {
    if(!xp_peek(x, "<!D") && !xp_peek(x, "<!d")) return 0;
    const char* q = x->p + 2;
    while(q < x->end) {
        if(*q == '>') { x->p = q + 1; return 1; }
        q++;
    }
    x->error = 1; return 0;
}

/* 读取 CDATA <![CDATA[...]]> 的文本（不含标记） */
static char* xp_read_cdata(XP* x, int* outLen) {
    if(!xp_peek(x, "<![CDATA[")) return NULL;
    const char* q = x->p + 9;
    const char* start = q;
    while(q + 2 < x->end) {
        if(q[0] == ']' && q[1] == ']' && q[2] == '>') {
            int n = (int)(q - start);
            char* s = (char*)malloc((size_t)n + 1);
            memcpy(s, start, (size_t)n); s[n] = '\0';
            x->p = q + 3; *outLen = n; return s;
        }
        q++;
    }
    x->error = 1; return NULL;
}

/* 标签名 / 属性名：[A-Za-z_][A-Za-z0-9_.:-]* */
static char* xp_read_name(XP* x) {
    const char* start = x->p;
    if(x->p >= x->end) return NULL;
    char c = *x->p;
    if(!(isalpha((unsigned char)c) || c == '_')) return NULL;
    x->p++;
    while(x->p < x->end) {
        c = *x->p;
        if(isalnum((unsigned char)c) || c == '_' || c == '-' || c == '.' || c == ':') x->p++;
        else break;
    }
    int n = (int)(x->p - start);
    char* s = (char*)malloc((size_t)n + 1);
    memcpy(s, start, (size_t)n); s[n] = '\0';
    return s;
}

/* 读带引号的属性值（单/双引号），不做实体解码（保持原样） */
static char* xp_read_attrval(XP* x) {
    if(x->p >= x->end) return NULL;
    char q = *x->p;
    if(q != '"' && q != '\'') return NULL;
    x->p++;
    const char* start = x->p;
    while(x->p < x->end && *x->p != q) x->p++;
    if(x->p >= x->end) return NULL;
    int n = (int)(x->p - start);
    char* s = (char*)malloc((size_t)n + 1);
    memcpy(s, start, (size_t)n); s[n] = '\0';
    x->p++;  // 吃掉闭引号
    return s;
}

/* 前向声明 */
static Value xp_parse_element(XP* x);

/* 把一段文本追加到 node 的 textContent（map 里读-改-写） */
static void xp_append_text(Value* node, const char* text, int len) {
    if(len <= 0) return;
    Value cur = lumyr_map_get(*node, lumyr_make_string("textContent"));
    const char* curS = (cur.type == VAL_STRING) ? lumyr_str_cstr(&cur) : "";
    int curLen = (int)strlen(curS);
    char* buf = (char*)malloc((size_t)curLen + (size_t)len + 1);
    memcpy(buf, curS, (size_t)curLen);
    memcpy(buf + curLen, text, (size_t)len);
    buf[curLen + len] = '\0';
    lumyr_map_set(node, lumyr_make_string("textContent"), lumyr_make_string(buf));
    free(buf);
}

/* 解析元素：<name attr="v"> ... </name>  或  <name ... /> */
static Value xp_parse_element(XP* x) {
    if(x->error || x->p >= x->end || *x->p != '<') { x->error = 1; return val_none(); }
    if(xp_peek(x, "</")) { x->error = 1; return val_none(); }  // 不应在此遇到闭标签
    x->p++;  // 吃 <
    char* name = xp_read_name(x);
    if(!name) { x->error = 1; return val_none(); }

    Value node = val_map();
    lumyr_map_set(&node, lumyr_make_string("tagName"), lumyr_make_string(name));
    Value attrs = val_map();
    Value children = val_array(0);
    lumyr_map_set(&node, lumyr_make_string("textContent"), lumyr_make_string(""));

    /* 属性 */
    while(1) {
        xp_ws(x);
        if(x->error) { free(name); return val_none(); }
        if(x->p >= x->end) { free(name); x->error = 1; return val_none(); }
        if(*x->p == '>' || xp_peek(x, "/>")) break;
        char* an = xp_read_name(x);
        if(!an) { free(name); x->error = 1; return val_none(); }
        xp_ws(x);
        if(x->p >= x->end || *x->p != '=') { free(an); free(name); x->error = 1; return val_none(); }
        x->p++;
        xp_ws(x);
        char* av = xp_read_attrval(x);
        if(!av) { free(an); free(name); x->error = 1; return val_none(); }
        lumyr_map_set(&attrs, lumyr_make_string(an), lumyr_make_string(av));
        free(an); free(av);
    }
    lumyr_map_set(&node, lumyr_make_string("attributes"), attrs);

    /* 自闭合 <name ... /> */
    if(xp_peek(x, "/>")) {
        xp_skip(x, 2);
        lumyr_map_set(&node, lumyr_make_string("children"), children);
        free(name);
        return node;
    }
    if(*x->p != '>') { free(name); x->error = 1; return val_none(); }
    x->p++;  // 吃 >

    /* 内容：文本 / CDATA / 注释 / 子元素，直到 </name> */
    while(1) {
        if(x->error) { free(name); return val_none(); }
        if(x->p >= x->end) { free(name); x->error = 1; return val_none(); }

        if(xp_peek(x, "</")) {
            /* 闭标签：校验名字 */
            x->p += 2;
            char* cn = xp_read_name(x);
            xp_ws(x);
            if(!cn || strcmp(cn, name) != 0 || x->p >= x->end || *x->p != '>') {
                if(cn) free(cn); free(name); x->error = 1; return val_none();
            }
            x->p++;  // 吃 >
            free(cn);
            break;
        }
        if(xp_skip_comment(x)) continue;
        int cdLen = 0;
        char* cd = xp_read_cdata(x, &cdLen);
        if(cd) { xp_append_text(&node, cd, cdLen); free(cd); continue; }
        if(*x->p == '<') {
            Value child = xp_parse_element(x);
            if(x->error) { free(name); return val_none(); }
            lumyr_array_add(&children, child);
            continue;
        }
        /* 纯文本到下一个 '<' */
        {
            const char* start = x->p;
            while(x->p < x->end && *x->p != '<') x->p++;
            int n = (int)(x->p - start);
            if(n > 0) xp_append_text(&node, start, n);
        }
    }

    lumyr_map_set(&node, lumyr_make_string("children"), children);
    free(name);
    return node;
}

Value lumyr_xml_parse(const char* s) {
    if(!s) return val_none();
    XP x; x.p = s; x.end = s + strlen(s); x.error = 0;

    /* 前奏：空白 / 声明 / 注释 / DOCTYPE */
    while(1) {
        xp_ws(&x);
        if(x.error) return val_none();
        if(x.p >= x.end) return val_none();
        if(xp_skip_comment(&x)) continue;
        if(xp_skip_pi(&x)) continue;
        if(xp_skip_doctype(&x)) continue;
        break;
    }
    if(x.p >= x.end || *x.p != '<' || xp_peek(&x, "</")) return val_none();

    Value root = xp_parse_element(&x);
    if(x.error) return val_none();
    return root;
}
