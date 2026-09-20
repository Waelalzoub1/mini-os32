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

/* Accepts [+-]ddd.ddd[eE][+-]dd.  The fractional digits are gathered as one
 * integer and divided once at the end: dividing per digit would round six or
 * seven times over and drift in the last place a float can hold. */
int parse_float(char **pp, float *out) {
    char *p = skip_ws(*pp);
    int neg = 0;
    int any = 0;
    int fi = 0;
    int fn = 0;
    float v = 0.0;
    float den = 1.0;
    int i = 0;

    if (*p == '+') { p = p + 1; }
    else if (*p == '-') { neg = 1; p = p + 1; }

    while (is_digit(*p)) {
        v = v * 10.0 + (float)(*p - '0');
        p = p + 1;
        any = 1;
    }
    if (*p == '.') {
        p = p + 1;
        while (is_digit(*p)) {
            /* A float holds ~7 digits; past 9 the extra ones only overflow fi. */
            if (fn < 9) { fi = fi * 10 + (*p - '0'); fn = fn + 1; }
            p = p + 1;
            any = 1;
        }
    }
    if (any == 0) return 0;

    if (fn > 0) {
        while (i < fn) { den = den * 10.0; i = i + 1; }
        v = v + (float)fi / den;
    }

    if (*p == 'e' || *p == 'E') {
        char *q = p + 1;
        int eneg = 0;
        int ev = 0;
        int edig = 0;
        if (*q == '+') { q = q + 1; }
        else if (*q == '-') { eneg = 1; q = q + 1; }
        while (is_digit(*q)) { ev = ev * 10 + (*q - '0'); q = q + 1; edig = 1; }
        if (edig) {
            p = q;
            /* A float tops out near 1e38; anything past that saturates to
             * inf or 0 anyway, so cap the loop rather than spin. */
            if (ev > 60) ev = 60;
            while (ev > 0) {
                if (eneg) v = v / 10.0;
                else v = v * 10.0;
                ev = ev - 1;
            }
        }
    }

    if (neg) v = 0.0 - v;
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


/* ---- float formatting ------------------------------------------------
 *
 * Values are 32-bit floats: about 7 significant digits, so anything past
 * roughly 1e9 cannot be shown exactly in fixed notation and falls back to
 * scientific.  Everything is built into a caller-supplied buffer so %g can
 * strip trailing zeros, which out_ch cannot do once written.
 */

/* Classified from the bits rather than by comparison.  The usual `v != v`
 * test does not work here: cc compares floats with the x87 fcompp, which
 * reports an unordered result as equal, so every comparison against a nan
 * comes back the same as one against an equal value. */
static unsigned int float_bits(float v) {
    unsigned int *p;
    p = (unsigned int *)&v;
    return *p;
}

static int is_nan(float v) {
    unsigned int b = float_bits(v);
    if ((b & 0x7F800000) != 0x7F800000) return 0;
    return (b & 0x007FFFFF) != 0;
}

static int is_inf(float v) {
    unsigned int b = float_bits(v);
    return (b & 0x7FFFFFFF) == 0x7F800000;
}

/* Sign has to come from the bits too, for the same reason: `v < 0.0` is
 * false for a nan, and -0.0 should print with its sign. */
static int is_neg(float v) { return (float_bits(v) & 0x80000000) != 0; }

static int u32_to_buf(char *out, unsigned int v) {
    char tmp[12];
    int t = 0;
    int n = 0;
    if (v == 0) { out[0] = '0'; return 1; }
    while (v > 0) { tmp[t] = (char)('0' + (v % 10)); t = t + 1; v = v / 10; }
    while (t > 0) { t = t - 1; out[n] = tmp[t]; n = n + 1; }
    return n;
}

/* Fixed notation.  Returns the length written; `strip` removes trailing
 * zeros and a bare trailing '.', which is what %g needs. */
static int float_fixed(char *out, float v, int prec, int strip) {
    int n = 0;
    int i = 0;
    float r = 0.5;
    int ip = 0;
    float frac = 0.0;
    int d = 0;

    if (is_nan(v)) { out[0] = 'n'; out[1] = 'a'; out[2] = 'n'; return 3; }
    if (is_neg(v)) { out[n] = '-'; n = n + 1; v = 0.0 - v; }
    if (is_inf(v)) { out[n] = 'i'; out[n+1] = 'n'; out[n+2] = 'f'; return n + 3; }

    while (i < prec) { r = r / 10.0; i = i + 1; }
    v = v + r;                       /* round at the last shown digit */

    ip = (int)v;
    frac = v - (float)ip;
    n = n + u32_to_buf(out + n, (unsigned int)ip);

    if (prec > 0) {
        out[n] = '.';
        n = n + 1;
        i = 0;
        while (i < prec) {
            frac = frac * 10.0;
            d = (int)frac;
            if (d < 0) { d = 0; }
            if (d > 9) { d = 9; }
            out[n] = (char)('0' + d);
            n = n + 1;
            frac = frac - (float)d;
            i = i + 1;
        }
    }

    if (strip && prec > 0) {
        while (n > 0 && out[n - 1] == '0') { n = n - 1; }
        if (n > 0 && out[n - 1] == '.') { n = n - 1; }
    }
    return n;
}

/* Scientific notation: d.dddde[+-]NN */
static int float_exp(char *out, float v, int prec, int strip) {
    int n = 0;
    int e = 0;
    if (is_nan(v)) { out[0] = 'n'; out[1] = 'a'; out[2] = 'n'; return 3; }
    if (is_neg(v)) { out[n] = '-'; n = n + 1; v = 0.0 - v; }
    if (is_inf(v)) { out[n] = 'i'; out[n+1] = 'n'; out[n+2] = 'f'; return n + 3; }

    if (v != 0.0) {
        float r = 0.5;
        int i = 0;
        while (v >= 10.0) { v = v / 10.0; e = e + 1; }
        while (v < 1.0) { v = v * 10.0; e = e - 1; }
        /* Rounding at `prec` digits can carry the mantissa up to 10.0, which
         * belongs one decade higher -- otherwise 9.9999 prints as "10e-05". */
        while (i < prec) { r = r / 10.0; i = i + 1; }
        if (v + r >= 10.0) { v = v / 10.0; e = e + 1; }
    }
    n = n + float_fixed(out + n, v, prec, strip);
    out[n] = 'e';
    n = n + 1;
    if (e < 0) { out[n] = '-'; e = 0 - e; } else { out[n] = '+'; }
    n = n + 1;
    if (e < 10) { out[n] = '0'; n = n + 1; }
    n = n + u32_to_buf(out + n, (unsigned int)e);
    return n;
}

/* %f: fixed, but very large or very small magnitudes have no useful fixed
 * form at float precision, so they use scientific instead. */
static void out_float_f(Out *o, float v, int prec) {
    char buf[64];
    int n;
    float a = v;
    if (is_neg(a)) { a = 0.0 - a; }
    if (is_nan(a) || is_inf(a) || (a != 0.0 && (a >= 1000000000.0 || a < 0.0001))) {
        n = float_exp(buf, v, prec, 0);
    } else {
        n = float_fixed(buf, v, prec, 0);
    }
    buf[n] = 0;
    out_str(o, buf);
}

/* %g: shortest of fixed and scientific, trailing zeros removed. */
static void out_float_g(Out *o, float v, int prec) {
    char buf[64];
    int n = 0;
    int e = 0;
    float a = v;
    if (prec <= 0) { prec = 6; }
    if (is_neg(a)) { a = 0.0 - a; }
    if (is_nan(a) || is_inf(a)) {
        n = float_exp(buf, v, prec - 1, 1);   /* prints "nan" / "-inf" */
        buf[n] = 0;
        out_str(o, buf);
        return;
    }
    if (a != 0.0) {
        float r = 0.5;
        int i = 0;
        while (a >= 10.0) { a = a / 10.0; e = e + 1; }
        while (a < 1.0) { a = a * 10.0; e = e - 1; }
        /* A value just under a power of ten -- 0.0001 is 9.9999997e-05 as a
         * float -- normalises one decade low.  Correct the exponent before
         * choosing a form, or 0.0001 comes out as scientific. */
        while (i < prec - 1) { r = r / 10.0; i = i + 1; }
        if (a + r >= 10.0) { e = e + 1; }
    }
    /* e >= 9 as well as e >= prec: float_fixed builds the integer part in a
     * 32-bit int, so a wide precision like %.12g must not steer 1e10 there. */
    if (e < -4 || e >= prec || e >= 9) {
        n = float_exp(buf, v, prec - 1, 1);
    } else {
        n = float_fixed(buf, v, prec - 1 - e, 1);
    }
    buf[n] = 0;
    out_str(o, buf);
}

static int vformat_ap(Out *o, char *fmt, int *ap) {
    int v = 0;
    char *s = 0;
    int prec = -1;
    float *fp = 0;
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
        /* optional ".N" precision, used by %f and %g */
        prec = -1;
        if (*fmt == '.') {
            fmt = fmt + 1;
            prec = 0;
            while (*fmt >= '0' && *fmt <= '9') {
                prec = prec * 10 + (*fmt - '0');
                fmt = fmt + 1;
            }
            /* The formatters build into a 64-byte buffer, and a float has no
             * information past ~9 digits anyway. */
            if (prec > 17) prec = 17;
        }
        if (*fmt == 'f' || *fmt == 'F') {
            fp = (float *)ap;
            ap = ap + 1;
            out_float_f(o, *fp, prec < 0 ? 6 : prec);
            fmt = fmt + 1;
            continue;
        }
        if (*fmt == 'g' || *fmt == 'G') {
            fp = (float *)ap;
            ap = ap + 1;
            out_float_g(o, *fp, prec < 0 ? 6 : prec);
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
    float *out_f = 0;

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
        /* There is no `double` or `long`, so the l in "%lf" / "%ld" is
         * redundant here -- accept and ignore it so copied code compiles. */
        if (*fmt == 'l') fmt = fmt + 1;
        if (*fmt == 'f' || *fmt == 'e' || *fmt == 'E' || *fmt == 'g' || *fmt == 'G') {
            out_f = (float *)(*ap);
            ap = ap + 1;
            if (parse_float(&p, out_f) == 0) break;
            assigned = assigned + 1;
            fmt = fmt + 1;
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
