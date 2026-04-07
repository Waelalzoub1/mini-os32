void *memcpy(void *dst, void *src, int n) {
    char *d = (char *)dst;
    char *s = (char *)src;
    int i = 0;
    while (i < n) { d[i] = s[i]; i = i + 1; }
    return dst;
}

void *memset(void *dst, int v, int n) {
    char *d = (char *)dst;
    int i = 0;
    while (i < n) { d[i] = (char)v; i = i + 1; }
    return dst;
}

void *memmove(void *dst, void *src, int n) {
    char *d = (char *)dst;
    char *s = (char *)src;
    if (d < s) {
        int i = 0;
        while (i < n) { d[i] = s[i]; i = i + 1; }
    } else {
        int i = n - 1;
        while (i >= 0) { d[i] = s[i]; i = i - 1; }
    }
    return dst;
}

int strlen(char *s) {
    int n = 0;
    while (s[n]) n = n + 1;
    return n;
}

int strcmp(char *a, char *b) {
    int i = 0;
    while (a[i] && a[i] == b[i]) i = i + 1;
    return (int)a[i] - (int)b[i];
}

int strncmp(char *a, char *b, int n) {
    int i = 0;
    while (i < n && a[i] && a[i] == b[i]) i = i + 1;
    if (i == n) return 0;
    return (int)a[i] - (int)b[i];
}

char *strcpy(char *dst, char *src) {
    int i = 0;
    while (1) {
        dst[i] = src[i];
        if (src[i] == 0) break;
        i = i + 1;
    }
    return dst;
}

char *strncpy(char *dst, char *src, int n) {
    int i = 0;
    while (i < n && src[i]) { dst[i] = src[i]; i = i + 1; }
    while (i < n) { dst[i] = 0; i = i + 1; }
    return dst;
}

int memcmp(void *a, void *b, int n) {
    unsigned char *p = (unsigned char *)a;
    unsigned char *q = (unsigned char *)b;
    int i = 0;
    while (i < n) {
        if (p[i] != q[i]) return (int)p[i] - (int)q[i];
        i = i + 1;
    }
    return 0;
}
