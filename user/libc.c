#include "libc.h"
#include <stdarg.h>

static inline int syscall3(int n, int a, int b, int c) {
    int ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(n), "b"(a), "c"(b), "d"(c));
    return ret;
}

static inline int syscall2(int n, int a, int b) {
    int ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(n), "b"(a), "c"(b));
    return ret;
}

static inline int syscall1(int n, int a) {
    int ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(n), "b"(a));
    return ret;
}

static inline int syscall4(int n, int a, int b, int c, int d) {
    int ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(n), "b"(a), "c"(b), "d"(c), "S"(d));
    return ret;
}

int sys_write(int fd, const void *buf, int len) { return syscall3(SYS_WRITE, fd, (int)buf, len); }
int sys_read(int fd, void *buf, int len) { return syscall3(SYS_READ, fd, (int)buf, len); }
int sys_list(void *buf, int max) { return syscall2(SYS_LIST, (int)buf, max); }
int sys_getcwd(char *buf, int max) { return syscall2(SYS_GETCWD, (int)buf, max); }
int sys_setcwd(const char *path) { return syscall1(SYS_SETCWD, (int)path); }
void sys_poweroff(void) { syscall1(SYS_POWEROFF, 0); for(;;){} }
int sys_meminfo(void *buf, int max) { return syscall2(SYS_MEMINFO, (int)buf, max); }
int sys_storage(void *buf, int max) { return syscall2(SYS_STORAGE, (int)buf, max); }
int sys_sync(void) { return syscall1(SYS_SYNC, 0); }
int sys_io_in(int port, int width) { return syscall2(SYS_IO_IN, port, width); }
int sys_io_out(int port, int width, int value) { return syscall3(SYS_IO_OUT, port, width, value); }
int sys_pci_read(int bdf, int off) { return syscall2(SYS_PCI_READ, bdf, off); }
int sys_pci_write(int bdf, int off, int value) { return syscall3(SYS_PCI_WRITE, bdf, off, value); }
int sys_map_phys(unsigned phys_lo, unsigned phys_hi, int size) { return syscall3(SYS_MAP_PHYS, (int)phys_lo, (int)phys_hi, size); }
int sys_dma_alloc(int size, unsigned out[2]) { return syscall2(SYS_DMA_ALLOC, size, (int)out); }
int sys_irq_wait(int irq, int timeout_ms) { return syscall2(SYS_IRQ_WAIT, irq, timeout_ms); }
int sys_load(const char *name, void *buf, int max) { return syscall3(SYS_LOAD, (int)name, (int)buf, max); }
int sys_save(const char *name, const void *buf, int size) { return syscall3(SYS_SAVE, (int)name, (int)buf, size); }
int sys_exec(const char *name) { return syscall1(SYS_EXEC, (int)name); }
int sys_rename(const char *oldn, const char *newn) { return syscall2(SYS_RENAME, (int)oldn, (int)newn); }
int sys_delete(const char *name) { return syscall1(SYS_DELETE, (int)name); }
int sys_getkey(void) { return syscall1(SYS_GETKEY, 0); }
int sys_getkey_nb(void) { return syscall1(SYS_GETKEY_NB, 0); }
int sys_keystate(int sc) { return syscall1(SYS_KEYSTATE, sc); }
int sys_ticks(void) { return syscall1(SYS_TICKS, 0); }
int sys_thread_create(void *entry, void *stack) { return syscall2(SYS_TCREATE, (int)entry, (int)stack); }
int sys_thread_exit(void) { return syscall1(SYS_TEXIT, 0); }
int sys_sleep(int ms) { return syscall1(SYS_SLEEP, ms); }
int sys_sleepf(int ms) { return syscall1(SYS_SLEEPF, ms); }
int sys_cls(void) { return syscall1(SYS_CLS, 0); }
int sys_setcursor(int x, int y) { return syscall2(SYS_SETCURSOR, x, y); }
int sys_vmode(int mode) { return syscall1(SYS_VMODE, mode); }
int sys_blit(const void *buf) { return syscall1(SYS_BLIT, (int)buf); }
int sys_palette(int idx, int rgb) { return syscall2(SYS_PALETTE, idx, rgb); }
int sys_gfx_fbinfo(void *buf, int max) { return syscall2(SYS_GFX_FBINFO, (int)buf, max); }
int sys_open(const char *name, int mode) { return syscall2(SYS_OPEN, (int)name, mode); }
int sys_fread(int fd, void *buf, int len) { return syscall3(SYS_FREAD, fd, (int)buf, len); }
int sys_fwrite(int fd, const void *buf, int len) { return syscall3(SYS_FWRITE, fd, (int)buf, len); }
int sys_close(int fd) { return syscall1(SYS_CLOSE, fd); }
int sys_seek(int fd, int pos) { return syscall2(SYS_SEEK, fd, pos); }
int sys_gfxinfo(void *buf, int max) { return syscall2(SYS_GFXINFO, (int)buf, max); }
int sys_vbemodes(void *buf, int max) { return syscall2(SYS_VBEMODES, (int)buf, max); }
void sys_exit(int code) { syscall1(SYS_EXIT, code); for(;;){} }
void *sys_sbrk(int inc) { return (void*)syscall1(SYS_SBRK, inc); }

/* ---- paths ---------------------------------------------------------- */

static char lib_cwd[PATH_MAX_LEN];
static int  lib_cwd_read = 0;

const char *cwd_get(void) {
    if (!lib_cwd_read) {
        lib_cwd[0] = 0;
        sys_getcwd(lib_cwd, sizeof(lib_cwd));
        lib_cwd_read = 1;
    }
    return lib_cwd;
}

static void path_pop(char *out, int *n) {
    int i = *n;
    if (i > 0 && out[i - 1] == '/') i--;
    while (i > 0 && out[i - 1] != '/') i--;
    *n = i;
    out[i] = 0;
}

/* Join `name` onto directory `base` ("" means root) and fold away "." and
 * "..".  A leading '/' on `name` ignores `base`.  0 on overflow or if empty. */
int path_resolve(const char *base, const char *name, char *out, int outsz) {
    if (!name || !*name) return 0;
    int n = 0;
    out[0] = 0;
    if (name[0] != '/' && base && *base) {
        n = strlen(base);
        if (n + 1 >= outsz) return 0;
        memcpy(out, base, n);
        if (out[n - 1] != '/') out[n++] = '/';
        out[n] = 0;
    }
    const char *p = name;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *start = p;
        int len = 0;
        while (*p && *p != '/') { p++; len++; }
        if (len == 1 && start[0] == '.') continue;
        if (len == 2 && start[0] == '.' && start[1] == '.') { path_pop(out, &n); continue; }
        if (n + len + 1 >= outsz) return 0;
        memcpy(out + n, start, len);
        n += len;
        out[n++] = '/';
        out[n] = 0;
    }
    if (n > 0 && out[n - 1] == '/') { n--; out[n] = 0; }   /* naming a file, not a dir */
    return n > 0;
}

/* As path_resolve, but also rejects keys the filesystem would truncate --
 * an over-long join would otherwise quietly hit some other file. */
int path_fs(const char *base, const char *name, char *out, int outsz) {
    if (!path_resolve(base, name, out, outsz)) return 0;
    return strlen(out) <= FS_NAME_MAX;
}

/* Directory part of a path, keeping the trailing '/' ("" when at root). */
void path_dir(const char *path, char *out, int outsz) {
    int n = strlen(path);
    while (n > 0 && path[n - 1] != '/') n--;
    if (n >= outsz) n = outsz - 1;
    memcpy(out, path, n);
    out[n] = 0;
}

const char *path_base(const char *path) {
    const char *b = path;
    for (const char *p = path; *p; p++) if (*p == '/') b = p + 1;
    return b;
}

int puts(const char *s) { return sys_write(1, s, strlen(s)); }
int putc(char c) { return sys_write(1, &c, 1); }
int readline(char *buf, int max) { return sys_read(0, buf, max); }

static int is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f'; }
static int is_digit(char c) { return c >= '0' && c <= '9'; }
static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static const char *skip_ws(const char *p) { while (*p && is_space(*p)) p++; return p; }

static int parse_int(const char **pp, int *out) {
    const char *p = skip_ws(*pp);
    int neg = 0;
    if (*p == '+' || *p == '-') { neg = (*p == '-'); p++; }
    if (!is_digit(*p)) return 0;
    int v = 0;
    while (is_digit(*p)) { v = v * 10 + (*p - '0'); p++; }
    *out = neg ? -v : v;
    *pp = p;
    return 1;
}

static int parse_uint(const char **pp, unsigned int *out) {
    const char *p = skip_ws(*pp);
    if (!is_digit(*p)) return 0;
    unsigned int v = 0;
    while (is_digit(*p)) { v = v * 10 + (unsigned int)(*p - '0'); p++; }
    *out = v;
    *pp = p;
    return 1;
}

static int parse_hex(const char **pp, unsigned int *out) {
    const char *p = skip_ws(*pp);
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
    int hv = hex_val(*p);
    if (hv < 0) return 0;
    unsigned int v = 0;
    while ((hv = hex_val(*p)) >= 0) { v = (v << 4) | (unsigned int)hv; p++; }
    *out = v;
    *pp = p;
    return 1;
}

/* Accepts [+-]ddd.ddd[eE][+-]dd.  The fractional digits are gathered as one
 * integer and divided once at the end: dividing per digit would round six or
 * seven times over and drift in the last place a float can hold. */
static int parse_float(const char **pp, float *out) {
    const char *p = skip_ws(*pp);
    int neg = 0, any = 0, fi = 0, fn = 0, i = 0;
    double v = 0.0, den = 1.0;

    if (*p == '+' || *p == '-') { neg = (*p == '-'); p++; }

    while (is_digit(*p)) { v = v * 10.0 + (double)(*p++ - '0'); any = 1; }
    if (*p == '.') {
        p++;
        while (is_digit(*p)) {
            /* A float holds ~7 digits; past 9 the extra ones only overflow fi. */
            if (fn < 9) { fi = fi * 10 + (*p - '0'); fn++; }
            p++;
            any = 1;
        }
    }
    if (!any) return 0;

    if (fn > 0) {
        for (i = 0; i < fn; i++) den = den * 10.0;
        v = v + (double)fi / den;
    }

    if (*p == 'e' || *p == 'E') {
        const char *q = p + 1;
        int eneg = 0, ev = 0, edig = 0;
        if (*q == '+' || *q == '-') { eneg = (*q == '-'); q++; }
        while (is_digit(*q)) { ev = ev * 10 + (*q++ - '0'); edig = 1; }
        if (edig) {
            p = q;
            /* A float tops out near 1e38; anything past that saturates to
             * inf or 0 anyway, so cap the loop rather than spin. */
            if (ev > 60) ev = 60;
            while (ev-- > 0) v = eneg ? v / 10.0 : v * 10.0;
        }
    }

    *out = (float)(neg ? -v : v);
    *pp = p;
    return 1;
}

int scanf(const char *fmt, ...) {
    char buf[256];
    int nread = readline(buf, sizeof(buf));
    if (nread <= 0) return -1;

    const char *p = buf;
    int assigned = 0;
    va_list ap;
    va_start(ap, fmt);

    while (*fmt) {
        if (is_space(*fmt)) {
            while (is_space(*fmt)) fmt++;
            p = skip_ws(p);
            continue;
        }
        if (*fmt != '%') {
            if (*p != *fmt) break;
            p++; fmt++;
            continue;
        }
        fmt++; /* skip % */
        if (*fmt == '%') {
            if (*p != '%') break;
            p++; fmt++;
            continue;
        }

        /* There is no `double` or `long` in the in-OS compiler, so the l in
         * "%lf" / "%ld" is redundant here -- accept and ignore it so copied
         * code compiles unchanged. */
        if (*fmt == 'l') fmt++;
        if (*fmt == 'f' || *fmt == 'e' || *fmt == 'E' || *fmt == 'g' || *fmt == 'G') {
            float *out = va_arg(ap, float *);
            if (!parse_float(&p, out)) break;
            assigned++;
            fmt++;
            continue;
        }

        /* Basic specifiers: d, u, x, c, s */
        if (*fmt == 'd') {
            int *out = va_arg(ap, int *);
            if (!parse_int(&p, out)) break;
            assigned++;
            fmt++;
            continue;
        }
        if (*fmt == 'u') {
            unsigned int *out = va_arg(ap, unsigned int *);
            if (!parse_uint(&p, out)) break;
            assigned++;
            fmt++;
            continue;
        }
        if (*fmt == 'x' || *fmt == 'X') {
            unsigned int *out = va_arg(ap, unsigned int *);
            if (!parse_hex(&p, out)) break;
            assigned++;
            fmt++;
            continue;
        }
        if (*fmt == 'c') {
            char *out = va_arg(ap, char *);
            if (*p == 0) break;
            *out = *p++;
            assigned++;
            fmt++;
            continue;
        }
        if (*fmt == 's') {
            char *out = va_arg(ap, char *);
            p = skip_ws(p);
            if (*p == 0) break;
            while (*p && !is_space(*p)) { *out++ = *p++; }
            *out = 0;
            assigned++;
            fmt++;
            continue;
        }

        /* Unknown specifier: stop */
        break;
    }

    va_end(ap);
    return assigned;
}

static void out_int(int v) {
    char buf[12];
    int i = 0;
    if (v == 0) { putc('0'); return; }
    if (v < 0) { putc('-'); v = -v; }
    while (v > 0) { buf[i++] = (char)('0' + (v % 10)); v /= 10; }
    while (i--) putc(buf[i]);
}

static void out_uint(unsigned int v) {
    char buf[12];
    int i = 0;
    if (v == 0) { putc('0'); return; }
    while (v > 0) { buf[i++] = (char)('0' + (v % 10)); v /= 10; }
    while (i--) putc(buf[i]);
}

static void out_hex(unsigned int v) {
    char buf[8];
    int i = 0;
    if (v == 0) { putc('0'); return; }
    while (v > 0) {
        int d = v & 0xF;
        buf[i++] = (char)(d < 10 ? '0' + d : 'a' + (d - 10));
        v >>= 4;
    }
    while (i--) putc(buf[i]);
}

/* ---- float formatting ------------------------------------------------
 *
 * Deliberately the same algorithm as fs/stdio.c so a program prints the same
 * text whichever libc it links.  The one difference is the argument type:
 * gcc promotes a float vararg to double, while the in-OS cc has no double
 * and passes the 4-byte float through untouched.
 *
 * Classified from the bits rather than by comparison, matching fs/stdio.c.
 * gcc would get `v != v` right, but cc compares floats with the x87 fcompp,
 * which reports an unordered result as equal -- so the bit test is the one
 * spelling that means the same thing in both libcs.  Sign comes from the
 * bits too, so -0.0 keeps its sign.
 */
static unsigned long long double_bits(double v) {
    unsigned long long b;
    memcpy(&b, &v, 8);
    return b;
}

static int is_nan(double v) {
    unsigned long long b = double_bits(v);
    if ((b & 0x7FF0000000000000ULL) != 0x7FF0000000000000ULL) return 0;
    return (b & 0x000FFFFFFFFFFFFFULL) != 0;
}

static int is_inf(double v) {
    return (double_bits(v) & 0x7FFFFFFFFFFFFFFFULL) == 0x7FF0000000000000ULL;
}

static int is_neg(double v) { return (double_bits(v) >> 63) != 0; }

static int u32_to_buf(char *out, unsigned int v) {
    char tmp[12];
    int t = 0, n = 0;
    if (v == 0) { out[0] = '0'; return 1; }
    while (v > 0) { tmp[t++] = (char)('0' + (v % 10)); v /= 10; }
    while (t > 0) out[n++] = tmp[--t];
    return n;
}

/* Fixed notation.  Returns the length written; `strip` removes trailing
 * zeros and a bare trailing '.', which is what %g needs. */
static int float_fixed(char *out, double v, int prec, int strip) {
    int n = 0, i = 0, d = 0, ip = 0;
    double r = 0.5, frac = 0.0;

    if (is_nan(v)) { out[0] = 'n'; out[1] = 'a'; out[2] = 'n'; return 3; }
    if (is_neg(v)) { out[n++] = '-'; v = -v; }
    if (is_inf(v)) { out[n] = 'i'; out[n+1] = 'n'; out[n+2] = 'f'; return n + 3; }

    for (i = 0; i < prec; i++) r = r / 10.0;
    v = v + r;                       /* round at the last shown digit */

    ip = (int)v;
    frac = v - (double)ip;
    n += u32_to_buf(out + n, (unsigned int)ip);

    if (prec > 0) {
        out[n++] = '.';
        for (i = 0; i < prec; i++) {
            frac = frac * 10.0;
            d = (int)frac;
            if (d < 0) d = 0;
            if (d > 9) d = 9;
            out[n++] = (char)('0' + d);
            frac = frac - (double)d;
        }
    }

    if (strip && prec > 0) {
        while (n > 0 && out[n - 1] == '0') n--;
        if (n > 0 && out[n - 1] == '.') n--;
    }
    return n;
}

/* Scientific notation: d.dddde[+-]NN */
static int float_exp(char *out, double v, int prec, int strip) {
    int n = 0, e = 0, i = 0;
    double r = 0.5;

    if (is_nan(v)) { out[0] = 'n'; out[1] = 'a'; out[2] = 'n'; return 3; }
    if (is_neg(v)) { out[n++] = '-'; v = -v; }
    if (is_inf(v)) { out[n] = 'i'; out[n+1] = 'n'; out[n+2] = 'f'; return n + 3; }

    if (v != 0.0) {
        while (v >= 10.0) { v = v / 10.0; e++; }
        while (v < 1.0) { v = v * 10.0; e--; }
        /* Rounding at `prec` digits can carry the mantissa up to 10.0, which
         * belongs one decade higher -- otherwise 9.9999 prints as "10e-05". */
        for (i = 0; i < prec; i++) r = r / 10.0;
        if (v + r >= 10.0) { v = v / 10.0; e++; }
    }
    n += float_fixed(out + n, v, prec, strip);
    out[n++] = 'e';
    if (e < 0) { out[n] = '-'; e = -e; } else { out[n] = '+'; }
    n++;
    if (e < 10) out[n++] = '0';
    n += u32_to_buf(out + n, (unsigned int)e);
    return n;
}

static void out_str_n(const char *s, int n) { while (n--) putc(*s++); }

/* %f: fixed, but very large or very small magnitudes have no useful fixed
 * form at float precision, so they use scientific instead. */
static void out_float_f(double v, int prec) {
    char buf[64];
    int n;
    double a = is_neg(v) ? -v : v;
    if (is_nan(a) || is_inf(a) || (a != 0.0 && (a >= 1000000000.0 || a < 0.0001)))
        n = float_exp(buf, v, prec, 0);
    else
        n = float_fixed(buf, v, prec, 0);
    out_str_n(buf, n);
}

/* %g: shortest of fixed and scientific, trailing zeros removed. */
static void out_float_g(double v, int prec) {
    char buf[64];
    int n = 0, e = 0, i = 0;
    double a = is_neg(v) ? -v : v;
    double r = 0.5;

    if (prec <= 0) prec = 6;
    if (is_nan(a) || is_inf(a)) {
        n = float_exp(buf, v, prec - 1, 1);   /* prints "nan" / "-inf" */
        out_str_n(buf, n);
        return;
    }
    if (a != 0.0) {
        while (a >= 10.0) { a = a / 10.0; e++; }
        while (a < 1.0) { a = a * 10.0; e--; }
        /* A value just under a power of ten -- 0.0001 is 9.9999997e-05 as a
         * float -- normalises one decade low.  Correct the exponent before
         * choosing a form, or 0.0001 comes out as scientific. */
        for (i = 0; i < prec - 1; i++) r = r / 10.0;
        if (a + r >= 10.0) e++;
    }
    /* e >= 9 as well as e >= prec: float_fixed builds the integer part in a
     * 32-bit int, so a wide precision like %.12g must not steer 1e10 there. */
    if (e < -4 || e >= prec || e >= 9)
        n = float_exp(buf, v, prec - 1, 1);
    else
        n = float_fixed(buf, v, prec - 1 - e, 1);
    out_str_n(buf, n);
}

int printf(const char *fmt, ...) {
    int count = 0;
    va_list ap;
    va_start(ap, fmt);
    while (*fmt) {
        if (*fmt != '%') {
            putc(*fmt++);
            count++;
            continue;
        }
        fmt++;
        if (*fmt == '%') {
            putc('%');
            fmt++;
            count++;
            continue;
        }
        /* optional ".N" precision, used by %f and %g.  The formatters build
         * into a 64-byte buffer, and a float has no information past ~9
         * digits anyway, so cap it. */
        int prec = -1;
        if (*fmt == '.') {
            fmt++;
            prec = 0;
            while (*fmt >= '0' && *fmt <= '9') prec = prec * 10 + (*fmt++ - '0');
            if (prec > 17) prec = 17;
        }
        if (*fmt == 'f' || *fmt == 'F') {
            out_float_f(va_arg(ap, double), prec < 0 ? 6 : prec);
            fmt++;
            continue;
        }
        if (*fmt == 'g' || *fmt == 'G') {
            out_float_g(va_arg(ap, double), prec < 0 ? 6 : prec);
            fmt++;
            continue;
        }
        if (*fmt == 'd') {
            int v = va_arg(ap, int);
            out_int(v);
            fmt++;
            continue;
        }
        if (*fmt == 'u') {
            unsigned int v = va_arg(ap, unsigned int);
            out_uint(v);
            fmt++;
            continue;
        }
        if (*fmt == 'x' || *fmt == 'X') {
            unsigned int v = va_arg(ap, unsigned int);
            out_hex(v);
            fmt++;
            continue;
        }
        if (*fmt == 'c') {
            char c = (char)va_arg(ap, int);
            putc(c);
            fmt++;
            count++;
            continue;
        }
        if (*fmt == 's') {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            while (*s) { putc(*s++); count++; }
            fmt++;
            continue;
        }
        /* Unknown specifier: print it literally */
        putc('%');
        putc(*fmt ? *fmt : '%');
        if (*fmt) fmt++;
        count += 2;
    }
    va_end(ap);
    return count;
}

void *memcpy(void *dst, const void *src, int n) { char *d=dst; const char *s=src; while (n--) *d++=*s++; return dst; }
void *memset(void *dst, int v, int n) { unsigned char *d=dst; while (n--) *d++=(unsigned char)v; return dst; }
void *memmove(void *dst, const void *src, int n) { char *d=dst; const char *s=src; if (d < s) { while (n--) *d++=*s++; } else { d += n; s += n; while (n--) *--d=*--s; } return dst; }
int strlen(const char *s) { int n=0; while (*s++) n++; return n; }
int strcmp(const char *a, const char *b) { while (*a && *a==*b) { a++; b++; } return (unsigned char)*a - (unsigned char)*b; }
int strncmp(const char *a, const char *b, int n) { while (n && *a && *a==*b) { a++; b++; n--; } if (n==0) return 0; return (unsigned char)*a - (unsigned char)*b; }
char *strcpy(char *dst, const char *src) { char *d=dst; while ((*d++=*src++)) {} return dst; }
char *strncpy(char *dst, const char *src, int n) { char *d=dst; while (n && *src) { *d++=*src++; n--; } while (n--) *d++=0; return dst; }
int atoi(const char *s) { int v=0; int neg=0; if (*s=='-') { neg=1; s++; } while (*s>='0' && *s<='9') { v = v*10 + (*s-'0'); s++; } return neg ? -v : v; }
