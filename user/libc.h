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
    SYS_GETCWD      = 35,
    SYS_SETCWD      = 36,
    SYS_POWEROFF    = 37,
    SYS_MEMINFO     = 38,
    SYS_STORAGE     = 39,
    SYS_SYNC        = 40,
    SYS_IO_IN       = 41,
    SYS_IO_OUT      = 42,
    SYS_PCI_READ    = 43,
    SYS_PCI_WRITE   = 44,
    SYS_MAP_PHYS    = 45,
    SYS_DMA_ALLOC   = 46,
    SYS_IRQ_WAIT    = 47
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
int sys_getcwd(char *buf, int max);
int sys_setcwd(const char *path);
void sys_poweroff(void);
int sys_meminfo(void *buf, int max);
int sys_storage(void *buf, int max);
int sys_sync(void);

/* Hardware access for user-space drivers.  bdf packs bus<<16|dev<<8|fn.
 * sys_map_phys returns a user pointer (as an offset) to `size` bytes of the
 * device's registers, mapped uncached; -1 on failure.  sys_dma_alloc fills
 * out[0] with the user pointer and out[1] with the physical address a device
 * must be programmed with.  sys_irq_wait unmasks the line and blocks until
 * its count moves; returns the delta, or 0 on timeout. */
int sys_io_in(int port, int width);
int sys_io_out(int port, int width, int value);
int sys_pci_read(int bdf, int off);
int sys_pci_write(int bdf, int off, int value);
int sys_map_phys(unsigned phys_lo, unsigned phys_hi, int size);
int sys_dma_alloc(int size, unsigned out[2]);
int sys_irq_wait(int irq, int timeout_ms);

/* Path handling.  The filesystem is flat -- "src/main.c" is one literal key --
 * so a relative name only means something once joined to a base directory.
 * The base is the shell's cwd, published via sys_setcwd on `cd`. */
#define PATH_MAX_LEN 64
#define FS_NAME_MAX  47   /* longer keys are silently truncated by the kernel */

const char *cwd_get(void);
int   path_resolve(const char *base, const char *name, char *out, int outsz);
int   path_fs(const char *base, const char *name, char *out, int outsz);
void  path_dir(const char *path, char *out, int outsz);
const char *path_base(const char *path);

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
