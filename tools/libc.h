#pragma once
#include <stddef.h>
#include <stdint.h>

int sys_write(int fd, const void *buf, int len);
int sys_read(int fd, void *buf, int len);
int sys_list(void *buf, int max);
int sys_load(const char *name, void *buf, int max);
int sys_save(const char *name, const void *buf, int size);
int sys_exec(const char *name);
int sys_rename(const char *oldn, const char *newn);
int sys_delete(const char *name);
int sys_getkey(void);
int sys_cls(void);
int sys_setcursor(int x, int y);
int sys_vmode(int mode);
int sys_blit(const void *buf);
int sys_palette(int idx, int rgb);
int sys_open(const char *name, int mode);
int sys_fread(int fd, void *buf, int len);
int sys_fwrite(int fd, const void *buf, int len);
int sys_close(int fd);
int sys_seek(int fd, int pos);
void sys_exit(int code);
void *sys_sbrk(int inc);

int puts(const char *s);
int putc(char c);
int readline(char *buf, int max);

void *memcpy(void *dst, const void *src, size_t n);
void *memset(void *dst, int v, size_t n);
void *memmove(void *dst, const void *src, size_t n);
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
char *strcpy(char *dst, const char *src);
char *strncpy(char *dst, const char *src, size_t n);
int atoi(const char *s);

