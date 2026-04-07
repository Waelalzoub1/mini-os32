#pragma once
#include <stdint.h>

enum {
    SYS_WRITE = 0,
    SYS_READ = 1,
    SYS_LIST = 2,
    SYS_LOAD = 3,
    SYS_SAVE = 4,
    SYS_EXEC = 5,
    SYS_EXIT = 6,
    SYS_SBRK = 7,
    SYS_RENAME = 8,
    SYS_DELETE = 9,
    SYS_GETKEY = 10,
    SYS_CLS = 11,
    SYS_SETCURSOR = 12,
    SYS_VMODE = 13,
    SYS_BLIT = 14,
    SYS_PALETTE = 15,
    SYS_OPEN = 16,
    SYS_FREAD = 17,
    SYS_FWRITE = 18,
    SYS_CLOSE = 19,
    SYS_SEEK = 20,
    SYS_GFXINFO = 21,
    SYS_VBEMODES = 22,
    SYS_GETKEY_NB = 23,
    SYS_TICKS = 24,
    SYS_TCREATE = 25,
    SYS_TEXIT = 26,
    SYS_SLEEP = 27,
    SYS_SLEEPF = 28,
    SYS_GFX_FBINFO = 29,
    SYS_KEYSTATE = 30,
    SYS_UDP_SEND    = 31,
    SYS_UDP_RECV    = 32,
    SYS_NET_IP      = 33,
    SYS_UDP_RECV_NB = 34
};

int sys_write(int fd, const void *buf, int len);
int sys_read(int fd, void *buf, int len);
int sys_list(void *buf, int max);
int sys_load(const char *name, void *buf, int max);
int sys_save(const char *name, const void *buf, int size);
int sys_exec(const char *name);
int sys_rename(const char *oldn, const char *newn);
int sys_delete(const char *name);
int sys_getkey(void);
int sys_getkey_nb(void);
int sys_keystate(int sc);
int sys_ticks(void);
int sys_thread_create(void *entry, void *stack);
int sys_thread_exit(void);
int sys_sleep(int ms);
int sys_sleepf(int ms);
int sys_cls(void);
int sys_setcursor(int x, int y);
int sys_vmode(int mode);
int sys_blit(const void *buf);
int sys_palette(int idx, int rgb);
int sys_gfx_fbinfo(void *buf, int max);
int sys_open(const char *name, int mode);
int sys_fread(int fd, void *buf, int len);
int sys_fwrite(int fd, const void *buf, int len);
int sys_close(int fd);
int sys_seek(int fd, int pos);
int sys_gfxinfo(void *buf, int max);
int sys_vbemodes(void *buf, int max);
void sys_exit(int code) __attribute__((noreturn));
void *sys_sbrk(int inc);
int sys_udp_send(int dst_ip, int dst_port, void *buf, int len);
int sys_udp_recv(int my_port, void *buf, int maxlen, int *src_ip);
int sys_udp_recv_nb(int my_port, void *buf, int maxlen, int *src_ip);
int sys_net_myip(void);

int puts(const char *s);
int putc(char c);
int readline(char *buf, int max);
int scanf(const char *fmt, ...);
int printf(const char *fmt, ...);

void *memcpy(void *dst, const void *src, int n);
void *memset(void *dst, int v, int n);
void *memmove(void *dst, const void *src, int n);
int strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, int n);
char *strcpy(char *dst, const char *src);
char *strncpy(char *dst, const char *src, int n);
int atoi(const char *s);
