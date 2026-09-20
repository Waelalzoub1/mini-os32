#include "errno.h"

typedef struct Block {
    int size;
    struct Block *next;
} Block;

static Block *free_list = 0;
int errno = 0;

static void *memcpy_local(void *dst, void *src, int n) {
    char *d = (char *)dst;
    char *s = (char *)src;
    int i = 0;
    while (i < n) { d[i] = s[i]; i = i + 1; }
    return dst;
}

static void *memset_local(void *dst, int v, int n) {
    char *d = (char *)dst;
    int i = 0;
    while (i < n) { d[i] = (char)v; i = i + 1; }
    return dst;
}

void *malloc(int size) {
    if (size <= 0) return 0;
    int want = (size + 3) & ~3;
    Block **pp = &free_list;
    while (*pp) {
        if ((*pp)->size >= want) {
            Block *b = *pp;
            *pp = b->next;
            return (void *)(b + 1);
        }
        pp = &(*pp)->next;
    }
    int total = want + (int)sizeof(Block);
    Block *b = (Block *)sys_sbrk(total);
    if ((int)b < 0) { errno = ENOMEM; return 0; }
    b->size = want;
    b->next = 0;
    return (void *)(b + 1);
}

void free(void *p) {
    if (p == 0) return;
    Block *b = ((Block *)p) - 1;
    b->next = free_list;
    free_list = b;
}

void *calloc(int n, int size) {
    int total;
    if (n <= 0 || size <= 0) return 0;
    if (n > 0x7fffffff / size) return 0;
    total = n * size;
    void *p = malloc(total);
    if (p) memset_local(p, 0, total);
    return p;
}

void *realloc(void *p, int size) {
    if (p == 0) return malloc(size);
    Block *b = ((Block *)p) - 1;
    if (b->size >= size) return p;
    void *np = malloc(size);
    if (np) {
        memcpy_local(np, p, b->size);
        free(p);
    }
    return np;
}

static int is_space(char c) {
    if (c == ' ') return 1;
    if (c == '\t') return 1;
    if (c == '\n') return 1;
    if (c == '\r') return 1;
    if (c == '\v') return 1;
    if (c == '\f') return 1;
    return 0;
}

static int digit_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'z') return c - 'a' + 10;
    if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
    return -1;
}

int strtol(char *s, char **endptr, int base) {
    char *p = s;
    int neg = 0;
    int v = 0;
    int any = 0;

    if (base != 0 && (base < 2 || base > 36)) {
        if (endptr) *endptr = s;
        errno = EINVAL;
        return 0;
    }

    while (is_space(*p)) p = p + 1;
    if (*p == '+') p = p + 1;
    else if (*p == '-') { neg = 1; p = p + 1; }

    if (base == 0) {
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { base = 16; p = p + 2; }
        else if (p[0] == '0') { base = 8; p = p + 1; }
        else base = 10;
    } else if (base == 16) {
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p = p + 2;
    }

    while (1) {
        int d = digit_val(*p);
        if (d < 0 || d >= base) break;
        v = v * base + d;
        p = p + 1;
        any = 1;
    }

    if (!any) {
        if (endptr) *endptr = s;
        errno = EINVAL;
        return 0;
    }
    if (endptr) *endptr = p;
    if (neg) v = -v;
    return v;
}

int atoi(char *s) {
    return strtol(s, 0, 10);
}
