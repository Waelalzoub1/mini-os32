#include "stdarg.h"

int putc(char c) { return sys_write(1, &c, 1); }

int puts(char *s) {
    int n = 0;
    while (s[n]) n = n + 1;
    return sys_write(1, s, n);
}

int readline(char *buf, int max) { return sys_read(0, buf, max); }

int is_space(char c) {
    if (c == ' ') return 1;
    if (c == '\t') return 1;
    if (c == '\n') return 1;
    if (c == '\r') return 1;
    if (c == '\v') return 1;
    if (c == '\f') return 1;
    return 0;
}

int is_digit(char c) {
    if (c < '0') return 0;
    if (c > '9') return 0;
    return 1;
}

int hex_val(char c) {
    if (c >= '0') {
        if (c <= '9') return c - '0';
    }
    if (c >= 'a') {
        if (c <= 'f') return c - 'a' + 10;
    }
    if (c >= 'A') {
        if (c <= 'F') return c - 'A' + 10;
    }
    return -1;
}

char *skip_ws(char *p) {
    while (*p) {
        if (is_space(*p) == 0) break;
        p = p + 1;
    }
    return p;
}

int parse_int(char **pp, int *out) {
    char *p = skip_ws(*pp);
    int neg = 0;
    if (*p == '+') { p = p + 1; }
    else if (*p == '-') { neg = 1; p = p + 1; }
    if (is_digit(*p) == 0) return 0;
    int v = 0;
    while (is_digit(*p)) { v = v * 10 + (*p - '0'); p = p + 1; }
    if (neg) *out = -v;
    else *out = v;
    *pp = p;
    return 1;
}

int parse_uint(char **pp, int *out) {
    char *p = skip_ws(*pp);
    if (is_digit(*p) == 0) return 0;
    int v = 0;
    while (is_digit(*p)) { v = v * 10 + (*p - '0'); p = p + 1; }
    *out = v;
    *pp = p;
    return 1;
}

int parse_hex(char **pp, int *out) {
    char *p = skip_ws(*pp);
    if (p[0] == '0') {
        if (p[1] == 'x') p = p + 2;
        else if (p[1] == 'X') p = p + 2;
    }
    int hv = hex_val(*p);
    if (hv < 0) return 0;
    int v = 0;
    while ((hv = hex_val(*p)) >= 0) { v = v * 16 + hv; p = p + 1; }
    *out = v;
    *pp = p;
    return 1;
}

void print_int(int v) {
    char buf[12];
    int i = 0;
    if (v == 0) { putc('0'); return; }
    if (v < 0) { putc('-'); v = -v; }
    while (v > 0) {
        int q = v / 10;
        int d = v - q * 10;
        buf[i] = (char)('0' + d);
        i = i + 1;
        v = q;
    }
    while (i > 0) { i = i - 1; putc(buf[i]); }
}

void print_uint(int v) {
    char buf[12];
    int i = 0;
    if (v == 0) { putc('0'); return; }
    while (v > 0) {
        int q = v / 10;
        int d = v - q * 10;
        buf[i] = (char)('0' + d);
        i = i + 1;
        v = q;
    }
    while (i > 0) { i = i - 1; putc(buf[i]); }
}

void print_hex(int v) {
    char buf[12];
    int i = 0;
    if (v == 0) { putc('0'); return; }
    while (v > 0) {
        int q = v / 16;
        int d = v - q * 16;
        if (d < 10) buf[i] = (char)('0' + d);
        else buf[i] = (char)('a' + (d - 10));
        i = i + 1;
        v = q;
    }
    while (i > 0) { i = i - 1; putc(buf[i]); }
}

typedef struct {
    char *buf;
    int max;
    int len;
} Out;

static void out_ch(Out *o, char c) {
    if (o->buf) {
        if (o->len + 1 < o->max) o->buf[o->len] = c;
    } else {
        putc(c);
    }
    o->len = o->len + 1;
}

static void out_str(Out *o, char *s) {
    if (s == 0) s = "(null)";
    while (*s) { out_ch(o, *s); s = s + 1; }
}

static void out_int(Out *o, int v) {
    char buf[12];
    int i = 0;
    if (v == 0) { out_ch(o, '0'); return; }
    if (v < 0) { out_ch(o, '-'); v = -v; }
    while (v > 0) { int q = v / 10; int d = v - q * 10; buf[i] = (char)('0' + d); i = i + 1; v = q; }
    while (i > 0) { i = i - 1; out_ch(o, buf[i]); }
}

static void out_uint(Out *o, unsigned int v) {
    char buf[12];
    int i = 0;
    if (v == 0) { out_ch(o, '0'); return; }
    while (v > 0) { unsigned int q = v / 10; unsigned int d = v - q * 10; buf[i] = (char)('0' + d); i = i + 1; v = q; }
    while (i > 0) { i = i - 1; out_ch(o, buf[i]); }
}

static void out_hex(Out *o, unsigned int v, int upper) {
    char buf[12];
    int i = 0;
    if (v == 0) { out_ch(o, '0'); return; }
    while (v > 0) {
        unsigned int q = v / 16;
        unsigned int d = v - q * 16;
        if (d < 10) buf[i] = (char)('0' + d);
        else {
            if (upper) buf[i] = (char)('A' + (d - 10));
            else buf[i] = (char)('a' + (d - 10));
        }
        i = i + 1;
        v = q;
    }
    while (i > 0) { i = i - 1; out_ch(o, buf[i]); }
}

static int vformat_ap(Out *o, char *fmt, int *ap) {
    int v = 0;
    char *s = 0;
    while (*fmt) {
        if (*fmt != '%') {
            out_ch(o, *fmt);
            fmt = fmt + 1;
            continue;
        }
        fmt = fmt + 1;
        if (*fmt == '%') {
            out_ch(o, '%');
            fmt = fmt + 1;
            continue;
        }
        if (*fmt == 'd') {
            v = *ap;
            ap = ap + 1;
            out_int(o, v);
            fmt = fmt + 1;
            continue;
        }
        if (*fmt == 'u') {
            v = *ap;
            ap = ap + 1;
            out_uint(o, (unsigned int)v);
            fmt = fmt + 1;
            continue;
        }
        if (*fmt == 'x') {
            v = *ap;
            ap = ap + 1;
            out_hex(o, (unsigned int)v, 0);
            fmt = fmt + 1;
            continue;
        }
        if (*fmt == 'X') {
            v = *ap;
            ap = ap + 1;
            out_hex(o, (unsigned int)v, 1);
            fmt = fmt + 1;
            continue;
        }
        if (*fmt == 'c') {
            v = *ap;
            ap = ap + 1;
            out_ch(o, (char)v);
            fmt = fmt + 1;
            continue;
        }
        if (*fmt == 's') {
            s = (char *)(*ap);
            ap = ap + 1;
            out_str(o, s);
            fmt = fmt + 1;
            continue;
        }
        out_ch(o, '%');
        if (*fmt) { out_ch(o, *fmt); fmt = fmt + 1; }
    }
    return o->len;
}

int vsnprintf(char *buf, int max, char *fmt, va_list ap) {
    Out o;
    o.buf = buf;
    o.max = max;
    o.len = 0;
    vformat_ap(&o, fmt, (int *)ap);
    if (o.buf && o.max > 0) {
        int n = o.len;
        if (n > o.max - 1) n = o.max - 1;
        if (n < 0) n = 0;
        o.buf[n] = 0;
    }
    return o.len;
}

int snprintf(char *buf, int max, char *fmt, ...) {
    Out o;
    o.buf = buf;
    o.max = max;
    o.len = 0;
    int *ap = (int *)&fmt;
    ap = ap + 1;
    vformat_ap(&o, fmt, ap);
    if (o.buf && o.max > 0) {
        int n = o.len;
        if (n > o.max - 1) n = o.max - 1;
        if (n < 0) n = 0;
        o.buf[n] = 0;
    }
    return o.len;
}

int printf(char *fmt, ...) {
    Out o;
    o.buf = 0;
    o.max = 0;
    o.len = 0;
    int *ap = (int *)&fmt;
    ap = ap + 1;
    vformat_ap(&o, fmt, ap);
    return o.len;
}

int scanf(char *fmt, ...) {
    char buf[256];
    int nread = 0;
    while (1) {
        nread = readline(buf, 256);
        if (nread <= 0) continue;
        char *pp = skip_ws(buf);
        if (*pp == 0) continue;
        break;
    }

    char *p = buf;
    int assigned = 0;
    int *ap = (int *)&fmt;
    ap = ap + 1;
    int *out_i = 0;
    char *out_c = 0;

    while (*fmt) {
        if (is_space(*fmt)) {
            while (is_space(*fmt)) fmt = fmt + 1;
            p = skip_ws(p);
            continue;
        }
        if (*fmt != '%') {
            if (*p != *fmt) break;
            p = p + 1; fmt = fmt + 1;
            continue;
        }
        fmt = fmt + 1;
        if (*fmt == '%') {
            if (*p != '%') break;
            p = p + 1; fmt = fmt + 1;
            continue;
        }
        if (*fmt == 'd') {
            out_i = (int *)(*ap);
            ap = ap + 1;
            if (parse_int(&p, out_i) == 0) break;
            assigned = assigned + 1;
            fmt = fmt + 1;
            continue;
        }
        if (*fmt == 'u') {
            out_i = (int *)(*ap);
            ap = ap + 1;
            if (parse_uint(&p, out_i) == 0) break;
            assigned = assigned + 1;
            fmt = fmt + 1;
            continue;
        }
        if (*fmt == 'x') {
            out_i = (int *)(*ap);
            ap = ap + 1;
            if (parse_hex(&p, out_i) == 0) break;
            assigned = assigned + 1;
            fmt = fmt + 1;
            continue;
        }
        if (*fmt == 'X') {
            out_i = (int *)(*ap);
            ap = ap + 1;
            if (parse_hex(&p, out_i) == 0) break;
            assigned = assigned + 1;
            fmt = fmt + 1;
            continue;
        }
        if (*fmt == 'c') {
            out_c = (char *)(*ap);
            ap = ap + 1;
            if (*p == 0) break;
            *out_c = *p;
            p = p + 1;
            assigned = assigned + 1;
            fmt = fmt + 1;
            continue;
        }
        if (*fmt == 's') {
            out_c = (char *)(*ap);
            ap = ap + 1;
            p = skip_ws(p);
            if (*p == 0) break;
            while (*p) {
                if (is_space(*p)) break;
                *out_c = *p;
                out_c = out_c + 1;
                p = p + 1;
            }
            *out_c = 0;
            assigned = assigned + 1;
            fmt = fmt + 1;
            continue;
        }
        break;
    }
    return assigned;
}
