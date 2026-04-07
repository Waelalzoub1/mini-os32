#ifndef STDIO_H
#define STDIO_H

#include "stdarg.h"

int putc(char c);
int puts(char *s);
int readline(char *buf, int max);
int printf(char *fmt, ...);
int snprintf(char *buf, int max, char *fmt, ...);
int vsnprintf(char *buf, int max, char *fmt, va_list ap);
int scanf(char *fmt, ...);
int sys_getkey_nb(void);
int sys_keystate(int sc);
int sys_ticks(void);

/* scancode constants for sys_keystate() */
#define SC_Q     0x10
#define SC_W     0x11
#define SC_E     0x12
#define SC_A     0x1E
#define SC_S     0x1F
#define SC_D     0x20
#define SC_UP    0x100
#define SC_DOWN  0x101
#define SC_LEFT  0x102
#define SC_RIGHT 0x103
int sys_thread_create(void *entry, void *stack);
int sys_thread_exit(void);
int sys_sleep(int ms);
int sys_sleepf(int ms);
#endif
