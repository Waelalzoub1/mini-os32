#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

#include "libc.h"

/* Minimal stdio forward declarations to avoid signature conflicts. */
typedef struct _IO_FILE FILE;
extern FILE *stdin;
extern FILE *stdout;
FILE *fopen(const char *path, const char *mode);
int fclose(FILE *stream);
size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream);
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream);
char *fgets(char *s, int size, FILE *stream);
int fputc(int c, FILE *stream);
int rename(const char *oldpath, const char *newpath);
int remove(const char *pathname);

int sys_write(int fd, const void *buf, int len) {
    ssize_t n = write(fd, buf, (size_t)len);
    return (n < 0) ? -1 : (int)n;
}

int sys_read(int fd, void *buf, int len) {
    ssize_t n = read(fd, buf, (size_t)len);
    return (n < 0) ? -1 : (int)n;
}

int sys_list(void *buf, int max) { (void)buf; (void)max; return -1; }

int sys_load(const char *name, void *buf, int max) {
    FILE *f = fopen(name, "rb");
    if (!f) return -1;
    size_t n = fread(buf, 1, (size_t)max, f);
    fclose(f);
    return (int)n;
}

int sys_save(const char *name, const void *buf, int size) {
    FILE *f = fopen(name, "wb");
    if (!f) return -1;
    size_t n = fwrite(buf, 1, (size_t)size, f);
    fclose(f);
    return (n == (size_t)size) ? 0 : -1;
}

int sys_exec(const char *name) { (void)name; return -1; }
int sys_rename(const char *oldn, const char *newn) { return rename(oldn, newn); }
int sys_delete(const char *name) { return remove(name); }
int sys_getkey(void) { return -1; }
int sys_cls(void) { return -1; }
int sys_setcursor(int x, int y) { (void)x; (void)y; return -1; }
int sys_vmode(int mode) { (void)mode; return -1; }
int sys_blit(const void *buf) { (void)buf; return -1; }
int sys_palette(int idx, int rgb) { (void)idx; (void)rgb; return -1; }
int sys_open(const char *name, int mode) { (void)name; (void)mode; return -1; }
int sys_fread(int fd, void *buf, int len) { (void)fd; (void)buf; (void)len; return -1; }
int sys_fwrite(int fd, const void *buf, int len) { (void)fd; (void)buf; (void)len; return -1; }
int sys_close(int fd) { (void)fd; return -1; }
int sys_seek(int fd, int pos) { (void)fd; (void)pos; return -1; }

void sys_exit(int code) { exit(code); }
void *sys_sbrk(int inc) { (void)inc; return NULL; }

int putc(char c) { return fputc((unsigned char)c, stdout); }
int puts(const char *s) { return (int)fwrite(s, 1, strlen(s), stdout); }

int readline(char *buf, int max) {
    if (!fgets(buf, max, stdin)) return 0;
    size_t n = strlen(buf);
    if (n && buf[n-1] == '\n') buf[n-1] = 0;
    return (int)n;
}

