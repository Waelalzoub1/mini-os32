/* Host shim: lets build/cc.o (the OS C compiler) run as a Linux i386 static
 * binary.  Provides the 17 externals cc.o needs, backed by raw Linux int 0x80
 * syscalls, so no 32-bit glibc is required.  File names resolve relative to
 * the current directory (run it from fs/).  readline reads a line from stdin
 * and strips the newline, matching the in-OS console. */

typedef unsigned int u32;

static int lsys3(int n, int a, int b, int c) {
    int r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "b"(a), "c"(b), "d"(c) : "memory");
    return r;
}
#define L_EXIT  1
#define L_READ  3
#define L_WRITE 4
#define L_OPEN  5
#define L_CLOSE 6
#define L_CREAT 8

int strlen(const char *s) { int n = 0; while (s[n]) n++; return n; }
int strcmp(const char *a, const char *b) {
    int i = 0;
    while (a[i] && a[i] == b[i]) i++;
    return (int)(unsigned char)a[i] - (int)(unsigned char)b[i];
}
int strncmp(const char *a, const char *b, int n) {
    int i = 0;
    while (i < n && a[i] && a[i] == b[i]) i++;
    if (i == n) return 0;
    return (int)(unsigned char)a[i] - (int)(unsigned char)b[i];
}
char *strcpy(char *d, const char *s) {
    int i = 0;
    while ((d[i] = s[i])) i++;
    return d;
}
char *strncpy(char *d, const char *s, int n) {
    int i = 0;
    while (i < n && s[i]) { d[i] = s[i]; i++; }
    while (i < n) d[i++] = 0;
    return d;
}
void *memcpy(void *dst, const void *src, int n) {
    char *d = dst; const char *s = src;
    for (int i = 0; i < n; i++) d[i] = s[i];
    return dst;
}
void *memset(void *dst, int v, int n) {
    char *d = dst;
    for (int i = 0; i < n; i++) d[i] = (char)v;
    return dst;
}

int puts(const char *s) { return lsys3(L_WRITE, 1, (int)s, strlen(s)); }
int putc(char c) { return lsys3(L_WRITE, 1, (int)&c, 1); }

int readline(char *buf, int max) {
    int n = 0;
    while (n < max - 1) {
        char c;
        int r = lsys3(L_READ, 0, (int)&c, 1);
        if (r <= 0) break;
        if (c == '\n') break;
        buf[n++] = c;
    }
    buf[n] = 0;
    return n;
}

void sys_exit(int code) { lsys3(L_EXIT, code, 0, 0); for (;;) {} }

int sys_load(const char *name, void *buf, int max) {
    int fd = lsys3(L_OPEN, (int)name, 0 /*O_RDONLY*/, 0);
    if (fd < 0) return -1;
    int total = 0;
    for (;;) {
        int r = lsys3(L_READ, fd, (int)buf + total, max - total);
        if (r < 0) { lsys3(L_CLOSE, fd, 0, 0); return -1; }
        if (r == 0) break;
        total += r;
        if (total >= max) break;
    }
    lsys3(L_CLOSE, fd, 0, 0);
    return total;
}

int sys_save(const char *name, const void *buf, int size) {
    int fd = lsys3(L_CREAT, (int)name, 0644, 0);
    if (fd < 0) return -1;
    int off = 0;
    while (off < size) {
        int r = lsys3(L_WRITE, fd, (int)buf + off, size - off);
        if (r <= 0) { lsys3(L_CLOSE, fd, 0, 0); return -1; }
        off += r;
    }
    lsys3(L_CLOSE, fd, 0, 0);
    return 0;
}

/* ---- path helpers, verbatim semantics from user/libc.c ---- */
#define FS_NAME_MAX 15

const char *cwd_get(void) { return ""; }

static void path_pop(char *out, int *n) {
    int i = *n;
    if (i > 0 && out[i - 1] == '/') i--;
    while (i > 0 && out[i - 1] != '/') i--;
    *n = i;
    out[i] = 0;
}

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
    if (n > 0 && out[n - 1] == '/') { n--; out[n] = 0; }
    return n > 0;
}

int path_fs(const char *base, const char *name, char *out, int outsz) {
    if (!path_resolve(base, name, out, outsz)) return 0;
    return strlen(out) <= FS_NAME_MAX;
}

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

extern int main(void);
void _start(void) { sys_exit(main()); }
