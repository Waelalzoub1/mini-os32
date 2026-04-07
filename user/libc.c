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
int sys_udp_send(int dst_ip, int dst_port, void *buf, int len) { return syscall4(SYS_UDP_SEND, dst_ip, dst_port, (int)buf, len); }
int sys_udp_recv(int my_port, void *buf, int maxlen, int *src_ip) { return syscall4(SYS_UDP_RECV, my_port, (int)buf, maxlen, (int)src_ip); }
int sys_udp_recv_nb(int my_port, void *buf, int maxlen, int *src_ip) { return syscall4(SYS_UDP_RECV_NB, my_port, (int)buf, maxlen, (int)src_ip); }
int sys_net_myip(void) { return syscall1(SYS_NET_IP, 0); }

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
