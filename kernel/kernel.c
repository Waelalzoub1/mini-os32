#include <stdint.h>
#include <stddef.h>

#define KERNEL_CS 0x08
#define KERNEL_DS 0x10
#define USER_CS   0x1B
#define USER_DS   0x23
#define TSS_SEL   0x28

#ifndef KERNEL_SECTORS
#define KERNEL_SECTORS 128
#endif
#ifndef STAGE2_SECTORS
#define STAGE2_SECTORS 8
#endif
#define DIR_SECTORS 2
#define KERNEL_LBA (1 + STAGE2_SECTORS)
#define FS_DIR_LBA (KERNEL_LBA + KERNEL_SECTORS)
#define FS_DATA_LBA (FS_DIR_LBA + DIR_SECTORS)

#define USER_BASE 0x00400000u
/* user pointers are offsets within the user segment */
#define PAGE_SIZE 4096u
#define USER_SPACE_SIZE (16u * 1024u * 1024u)
#define USER_FB_SIZE (8u * 1024u * 1024u)
#define USER_FB_OFFSET (USER_SPACE_SIZE - USER_FB_SIZE)
#define USER_FB_BASE (USER_BASE + USER_FB_OFFSET)
#define USER_STACK_TOP (USER_FB_BASE)
#define USER_STACK_SIZE (1u * 1024u * 1024u)
#define USER_STACK_BOTTOM (USER_STACK_TOP - USER_STACK_SIZE)
#define USER_STACK_GUARD (USER_STACK_BOTTOM - PAGE_SIZE)
#define FILE_BUF_MAX (1024*1024)
#define KERNEL_IDENTITY_LIMIT (128u * 1024u * 1024u)
#define PTE_P 0x001u
#define PTE_W 0x002u
#define PTE_U 0x004u

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

typedef struct {
    char name[16];
    u32 start;
    u32 size;
    u32 flags;
    u32 reserved;
} __attribute__((packed)) dirent_t;

extern void syscall_entry(void);
extern void enter_user(u32 entry, u32 stack);
extern u32 kernel_stack_top;
extern u64 gdt[];
extern u32 isr_stub_table[];
extern u32 irq_stub_table[];
extern char _kernel_end;
 
#define BOOTINFO_ADDR 0x5000
#define VBE_RM_BUF   ((void*)0x9200) /* low memory buffer for BIOS VBE calls */
#define BOOTINFO_MAGIC 0x4F533332u /* "OS32" */

typedef struct {
    u32 magic;
    u32 lfb;
    u32 width;
    u32 height;
    u32 pitch;
    u32 bpp;
    u32 font;
    u32 font_h;
    u32 mode;
    u32 vbe_list;
    u32 vbe_count;
} __attribute__((packed)) bootinfo_t;
extern int bios_vbe_get_mode_info(u16 mode, void *buf);
extern int bios_vbe_set_mode(u16 mode);
extern int bios_vbe_get_controller_info(void *buf);
static int vbe_set_mode(u16 mode);

static volatile u16 *vga = (u16*)0xB8000;
static u8 cur_x = 0, cur_y = 0;
static int fb_enabled = 0;
static int fb_console_enable = 1;
static u8 *fb_font = 0;
static u32 fb_font_h = 16;
static u32 fb_font_stride = 16;
static u32 fb_bytes = 1;
static u8 fb_fg = 15;
static u8 fb_bg = 0;
static u32 fb_fg32 = 0x00FFFFFFu;
static u32 fb_bg32 = 0x00000000u;
static u32 fb_cols = 80, fb_rows = 25;
static u32 fb_scale = 1;

/* current graphics mode info */
static u32 gfx_w = 320;
static u32 gfx_h = 200;
static u32 gfx_pitch = 320;
static u32 gfx_bpp = 8;
static u32 gfx_lfb = 0xA0000;
static u32 gfx_mode_id = 0;
static u32 gfx_fb_size = 0;
static u32 gfx_fb_user_addr = USER_FB_BASE;
static int gfx_fb_user_mapped = 0;
static int text_clear_pending = 0;
static int text_restore_pending = 0;

typedef struct {
    u16 mode;
    u16 w;
    u16 h;
    u16 bpp;
    u16 pitch;
} vbe_list_entry_t;

static vbe_list_entry_t *vbe_list = 0;
static int vbe_list_count = 0;

static dirent_t dir[(DIR_SECTORS * 512) / sizeof(dirent_t)];

static u8 file_buf[FILE_BUF_MAX];
static u32 user_brk_off = 0;
volatile u32 exit_requested = 0;
static u32 exit_code = 0;
static char next_prog[16];
static u32 next_prog_set = 0;
static void *sched_eip = 0;
static u32 sched_esp = 0;
static volatile u32 timer_ticks = 0;
static volatile int kill_program = 0;
static u8 *phys_free = 0;
static u32 *kernel_pd = 0;
static u32 *proc_pd = 0;
static u32 *user_pts[4];
static u32 user_phys_base = 0;
static u32 user_phys_limit = 0;

#define OPEN_MAX 8
typedef struct {
    int used;
    int mode; /* 0=read, 1=write */
    int dir_index;
    u32 start;
    u32 size;
    u32 pos;
    u32 cur_lba;
    int sector_valid;
    int sector_dirty;
    u8 sector[512];
} open_file_t;
static open_file_t ofiles[OPEN_MAX];

typedef struct {
    u32 w;
    u32 h;
    u32 pitch;
    u32 bpp;
} gfxinfo_t;

typedef struct {
    u32 w;
    u32 h;
    u32 pitch;
    u32 bpp;
    u32 size;
    u32 user_addr;
} gfxfbinfo_t;

static inline void outb(u16 port, u8 val) { __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port)); }
static inline u8 inb(u16 port) { u8 ret; __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port)); return ret; }
static inline void outw(u16 port, u16 val) { __asm__ volatile ("outw %0, %1" : : "a"(val), "Nd"(port)); }
static inline u16 inw(u16 port) { u16 ret; __asm__ volatile ("inw %1, %0" : "=a"(ret) : "Nd"(port)); return ret; }
static inline void outl(u16 port, u32 val) { __asm__ volatile ("outl %0, %1" : : "a"(val), "Nd"(port)); }
static inline u32 inl(u16 port) { u32 ret; __asm__ volatile ("inl %1, %0" : "=a"(ret) : "Nd"(port)); return ret; }
static inline void lidt(void *base, u16 size) { struct { u16 limit; u32 base; } __attribute__((packed)) idt = { size, (u32)base }; __asm__ volatile("lidt %0" : : "m"(idt)); }
static inline void ltr(u16 sel) { __asm__ volatile("ltr %0" : : "r"(sel)); }
static const u16 SERIAL_PORT_BASE = 0x3F8;
static int serial_ready = 0;

static void serial_init(void) {
    outb(SERIAL_PORT_BASE + 1, 0x00);
    outb(SERIAL_PORT_BASE + 3, 0x80);
    outb(SERIAL_PORT_BASE + 0, 0x03);
    outb(SERIAL_PORT_BASE + 1, 0x00);
    outb(SERIAL_PORT_BASE + 3, 0x03);
    outb(SERIAL_PORT_BASE + 2, 0xC7);
    outb(SERIAL_PORT_BASE + 4, 0x0B);
    serial_ready = 1;
}

static void serial_putc(char c) {
    if (!serial_ready) return;
    while ((inb(SERIAL_PORT_BASE + 5) & 0x20) == 0) { }
    outb(SERIAL_PORT_BASE + 0, (u8)c);
}

static void *memcpy(void *dst, const void *src, u32 n) { u8 *d = dst; const u8 *s = src; while (n--) *d++ = *s++; return dst; }
static void *memset(void *dst, u8 v, u32 n) { u8 *d = dst; while (n--) *d++ = v; return dst; }
static u32 strlen(const char *s) { u32 n = 0; while (*s++) n++; return n; }
static int strcmp(const char *a, const char *b) { while (*a && (*a == *b)) { a++; b++; } return (u8)*a - (u8)*b; }
static int strncmp(const char *a, const char *b, u32 n) { while (n && *a && (*a == *b)) { a++; b++; n--; } if (n == 0) return 0; return (u8)*a - (u8)*b; }
static int name_eq_ci(const char *a, const char *b) {
    for (int i = 0; i < 16; i++) {
        char ca = a[i], cb = b[i];
        if (ca == 0 && cb == 0) return 1;
        if (ca == 0 || cb == 0) return 0;
        if (ca >= 'a' && ca <= 'z') ca -= 32;
        if (cb >= 'a' && cb <= 'z') cb -= 32;
        if (ca != cb) return 0;
    }
    return 1;
}

static u32 align_up(u32 v, u32 a) { return (v + a - 1) & ~(a - 1); }

static int user_range_ok(u32 off, u32 len) {
    if (off < USER_FB_OFFSET) {
        if (len > USER_FB_OFFSET - off) return 0;
        return 1;
    }
    u32 fb_off = off - USER_FB_OFFSET;
    if (fb_off >= USER_FB_SIZE) return 0;
    if (len > USER_FB_SIZE - fb_off) return 0;
    return 1;
}

static int user_copy_str(u32 off, char *dst, u32 max) {
    if (max == 0) return 0;
    if (off >= USER_SPACE_SIZE) { dst[0] = 0; return 0; }
    u32 limit = USER_SPACE_SIZE - off;
    if (limit > max - 1) limit = max - 1;
    char *src = (char*)(USER_BASE + off);
    for (u32 i = 0; i < limit; i++) {
        char c = src[i];
        dst[i] = c;
        if (c == 0) { return 1; }
    }
    dst[limit] = 0;
    return 0;
}

static u32 alloc_page_phys(void) {
    u32 p = (u32)phys_free;
    phys_free += PAGE_SIZE;
    memset((void*)p, 0, PAGE_SIZE);
    return p;
}

static void map_page(u32 *pd, u32 virt, u32 phys, u32 flags) {
    u32 pd_idx = virt >> 22;
    u32 pt_idx = (virt >> 12) & 0x3FF;
    u32 *pt;
    if (pd[pd_idx] & PTE_P) {
        pt = (u32*)(pd[pd_idx] & ~0xFFFu);
    } else {
        pt = (u32*)alloc_page_phys();
        u32 pde_flags = PTE_P | PTE_W;
        if (flags & PTE_U) pde_flags |= PTE_U;
        pd[pd_idx] = (u32)pt | pde_flags;
    }
    pt[pt_idx] = (phys & ~0xFFFu) | (flags | PTE_P);
}

static void map_range(u32 *pd, u32 base, u32 size, u32 flags) {
    u32 start = base & ~0xFFFu;
    u32 end = (base + size + 0xFFFu) & ~0xFFFu;
    for (u32 addr = start; addr < end; addr += PAGE_SIZE) {
        map_page(pd, addr, addr, flags);
    }
}

static void map_user_framebuffer(u32 *pd) {
    gfx_fb_user_mapped = 0;
    gfx_fb_size = 0;
    if (!gfx_lfb) return;
    u32 bytes = gfx_pitch * gfx_h;
    if (bytes == 0 || bytes > USER_FB_SIZE) return;
    gfx_fb_size = bytes;
    gfx_fb_user_addr = USER_FB_BASE;
    gfx_fb_user_mapped = 1;
    u32 start = gfx_fb_user_addr & ~0xFFFu;
    u32 end = (gfx_fb_user_addr + bytes + 0xFFFu) & ~0xFFFu;
    u32 phys = gfx_lfb - (gfx_fb_user_addr - start);
    for (u32 addr = start; addr < end; addr += PAGE_SIZE) {
        map_page(pd, addr, phys + (addr - start), PTE_W | PTE_U);
    }
}

static void remap_user_framebuffer(void) {
    if (!proc_pd) return;
    map_user_framebuffer(proc_pd);
    if (!gfx_fb_user_mapped) return;
    u32 cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile("mov %0, %%cr3" : : "r"(cr3));
}

static int set_fb_mode(u32 mode) {
    if (vbe_set_mode((u16)mode) < 0) return -1;
    fb_enabled = 1;
    gfx_mode_id = mode;
    remap_user_framebuffer();
    return gfx_fb_user_mapped ? 0 : -1;
}

static void refresh_user_framebuffer_mapping(void) {
    if (!proc_pd) return;
    map_user_framebuffer(proc_pd);
    if (!gfx_fb_user_mapped) return;
    u32 cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile("mov %0, %%cr3" : : "r"(cr3));
}

static void paging_enable(u32 *pd) {
    __asm__ volatile("mov %0, %%cr3" : : "r"(pd));
    u32 cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= 0x80000000u;
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0));
}

static void paging_switch(u32 *pd) {
    __asm__ volatile("mov %0, %%cr3" : : "r"(pd));
}

static void map_user_space(u32 *pd) {
    u32 pd_start = USER_BASE >> 22;
    u32 pd_count = USER_SPACE_SIZE >> 22;
    if ((USER_SPACE_SIZE & 0x3FFFFF) != 0) pd_count++;
    for (u32 i = 0; i < pd_count; i++) {
        pd[pd_start + i] = 0;
    }
    u32 phys = user_phys_base;
    for (u32 off = 0; off < USER_FB_OFFSET; off += PAGE_SIZE) {
        if (off == USER_STACK_GUARD) {
            phys += PAGE_SIZE;
            continue;
        }
        map_page(pd, USER_BASE + off, phys, PTE_W | PTE_U);
        phys += PAGE_SIZE;
    }
}

static void paging_init(void) {
    phys_free = (u8*)align_up((u32)&_kernel_end, PAGE_SIZE);
    kernel_pd = (u32*)alloc_page_phys();
    /* identity map low memory for kernel */
    for (u32 addr = 0; addr < KERNEL_IDENTITY_LIMIT; addr += PAGE_SIZE) {
        map_page(kernel_pd, addr, addr, PTE_W);
    }
    /* map LFB if present */
    if (fb_enabled && gfx_lfb) {
        u32 bytes = gfx_pitch * gfx_h;
        map_range(kernel_pd, gfx_lfb, bytes, PTE_W);
    }
    proc_pd = (u32*)alloc_page_phys();
    memcpy(proc_pd, kernel_pd, PAGE_SIZE);
    user_phys_base = align_up((u32)phys_free, PAGE_SIZE);
    user_phys_limit = user_phys_base + USER_FB_OFFSET;
    if (user_phys_limit < user_phys_base) user_phys_limit = user_phys_base;
    phys_free = (u8*)user_phys_limit;
    map_user_space(proc_pd);
    map_user_framebuffer(proc_pd);
    paging_enable(proc_pd);
}

static void vga_scroll(void) {
    if (fb_enabled) {
        if (cur_y < fb_rows) return;
        if (!gfx_lfb) return;   /* framebuffer not yet mapped — avoid null deref */
        u8 *dst = (u8*)gfx_lfb;
        u32 pitch = gfx_pitch;
        u32 line_h = fb_font_h * fb_scale;
        u32 move_lines = gfx_h - line_h;
        for (u32 y = 0; y < move_lines; y++) {
            memcpy(dst + y * pitch, dst + (y + line_h) * pitch, pitch);
        }
        for (u32 y = move_lines; y < gfx_h; y++) {
            u8 *row = dst + y * pitch;
            if (fb_bytes == 1) {
                memset(row, fb_bg, pitch);
            } else if (fb_bytes == 4) {
                u32 n = pitch / 4;
                for (u32 x = 0; x < n; x++) ((u32*)row)[x] = fb_bg32;
            }
        }
        cur_y = (u8)(fb_rows - 1);
        return;
    }
    if (cur_y < 25) return;
    for (u32 y = 1; y < 25; y++) {
        for (u32 x = 0; x < 80; x++)
            vga[(y - 1) * 80 + x] = vga[y * 80 + x];
    }
    for (u32 x = 0; x < 80; x++)
        vga[24 * 80 + x] = 0x0720;
    cur_y = 24;
}

static void vga_update_cursor(void) {
    if (fb_enabled) return;
    u16 pos = (u16)(cur_y * 80 + cur_x);
    outb(0x3D4, 0x0F);
    outb(0x3D5, (u8)(pos & 0xFF));
    outb(0x3D4, 0x0E);
    outb(0x3D5, (u8)((pos >> 8) & 0xFF));
}

static void vga_putc(char c) {
    serial_putc(c);
    if (fb_enabled) {
        if (c == '\n') { cur_x = 0; cur_y++; vga_scroll(); return; }
        if (c == '\r') { cur_x = 0; return; }
        if (c == 8) {
            if (cur_x == 0) {
                if (cur_y == 0) return;
                cur_y--;
                cur_x = (u8)(fb_cols - 1);
            } else {
                cur_x--;
            }
            /* draw space */
            u32 px = (u32)cur_x * 8 * fb_scale;
            u32 py = (u32)cur_y * fb_font_h * fb_scale;
            u8 *dst = (u8*)gfx_lfb;
            u32 h = fb_font_h * fb_scale;
            u32 w = 8 * fb_scale;
            for (u32 y = 0; y < h; y++) {
                u8 *row = dst + (py + y) * gfx_pitch + px * fb_bytes;
                if (fb_bytes == 1) {
                    memset(row, fb_bg, w);
                } else if (fb_bytes == 4) {
                    for (u32 x = 0; x < w; x++) {
                        *(u32*)(row + x * 4) = fb_bg32;
                    }
                }
            }
            return;
        }
        if (!fb_font) return;
        u32 px = (u32)cur_x * 8 * fb_scale;
        u32 py = (u32)cur_y * fb_font_h * fb_scale;
        const u8 *glyph = fb_font + ((u8)c) * fb_font_stride;
        u8 *dst = (u8*)gfx_lfb;
        u32 row_step = fb_font_h ? (fb_font_stride / fb_font_h) : 1;
        if (row_step == 0) row_step = 1;
        for (u32 y = 0; y < fb_font_h; y++) {
            u8 bits = glyph[y * row_step];
            for (u32 x = 0; x < 8; x++) {
                int on = (bits & (0x80 >> x)) != 0;
                u32 base_y = py + y * fb_scale;
                u32 base_x = px + x * fb_scale;
                for (u32 sy = 0; sy < fb_scale; sy++) {
                    u8 *row = dst + (base_y + sy) * gfx_pitch;
                    for (u32 sx = 0; sx < fb_scale; sx++) {
                        u32 px_off = (base_x + sx) * fb_bytes;
                        if (fb_bytes == 1) {
                            row[px_off] = on ? fb_fg : fb_bg;
                        } else if (fb_bytes == 4) {
                            *(u32*)(row + px_off) = on ? fb_fg32 : fb_bg32;
                        }
                    }
                }
            }
        }
        cur_x++;
        if (cur_x >= fb_cols) { cur_x = 0; cur_y++; vga_scroll(); }
        return;
    }
    if (c == '\n') { cur_x = 0; cur_y++; vga_scroll(); vga_update_cursor(); return; }
    if (c == '\r') { cur_x = 0; vga_update_cursor(); return; }
    if (c == 8) {
        if (cur_x == 0) {
            if (cur_y == 0) return;
            cur_y--;
            cur_x = 79;
        } else {
            cur_x--;
        }
        vga[cur_y * 80 + cur_x] = 0x0720;
        vga_update_cursor();
        return;
    }
    vga[cur_y * 80 + cur_x] = (0x07 << 8) | (u8)c;
    cur_x++;
    if (cur_x >= 80) { cur_x = 0; cur_y++; vga_scroll(); }
    vga_update_cursor();
}

static void vga_puts(const char *s) { while (*s) vga_putc(*s++); }
static void vga_cls(void);

static void vga_write_regs(const u8 *regs) {
    outb(0x3C2, regs[0]);
    for (int i = 0; i < 5; i++) { outb(0x3C4, (u8)i); outb(0x3C5, regs[1 + i]); }
    outb(0x3D4, 0x03); outb(0x3D5, inb(0x3D5) | 0x80);
    outb(0x3D4, 0x11); outb(0x3D5, inb(0x3D5) & ~0x80);
    regs += 6;
    for (int i = 0; i < 25; i++) { outb(0x3D4, (u8)i); outb(0x3D5, regs[i]); }
    regs += 25;
    for (int i = 0; i < 9; i++) { outb(0x3CE, (u8)i); outb(0x3CF, regs[i]); }
    regs += 9;
    /* reset attribute controller flip-flop */
    inb(0x3DA);
    for (int i = 0; i < 21; i++) { outb(0x3C0, (u8)i); outb(0x3C0, regs[i]); }
    outb(0x3C0, 0x20);
}

static const u8 vga_mode13_regs[] = {
    0x63,
    0x03,0x01,0x0F,0x00,0x0E,
    0x5F,0x4F,0x50,0x82,0x54,0x80,0xBF,0x1F,0x00,0x41,0x00,0x00,0x00,0x00,0x00,0x00,
    0x9C,0x0E,0x8F,0x28,0x40,0x96,0xB9,0xA3,0xFF,
    0x00,0x00,0x00,0x00,0x00,0x40,0x05,0x0F,0xFF,
    0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F,
    0x41,0x00,0x0F,0x00,0x00
};

static const u8 vga_text_regs[] = {
    0x67,
    0x03,0x00,0x03,0x00,0x02,
    0x5F,0x4F,0x50,0x82,0x55,0x81,0xBF,0x1F,0x00,0x4F,0x0D,0x0E,0x00,0x00,0x00,0x00,
    0x9C,0x0E,0x8F,0x28,0x1F,0x96,0xB9,0xA3,0xFF,
    0x00,0x00,0x00,0x00,0x00,0x10,0x0E,0x00,0xFF,
    0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F,
    0x08,0x00,0x0F,0x00,0x00
};

static u8 vga_font[8192];
static int vga_font_saved = 0;

static void vga_font_save(void) {
    /* Save current VGA regs we touch */
    outb(0x3C4, 0x02); u8 seq2 = inb(0x3C5);
    outb(0x3C4, 0x04); u8 seq4 = inb(0x3C5);
    outb(0x3CE, 0x04); u8 gc4 = inb(0x3CF);
    outb(0x3CE, 0x05); u8 gc5 = inb(0x3CF);
    outb(0x3CE, 0x06); u8 gc6 = inb(0x3CF);

    /* Enable font access (plane 2 at A0000) */
    outb(0x3C4, 0x02); outb(0x3C5, 0x04);
    outb(0x3C4, 0x04); outb(0x3C5, 0x07);
    outb(0x3CE, 0x04); outb(0x3CF, 0x02);
    outb(0x3CE, 0x05); outb(0x3CF, 0x00);
    outb(0x3CE, 0x06); outb(0x3CF, 0x04);

    memcpy(vga_font, (void*)0xA0000, sizeof(vga_font));
    vga_font_saved = 1;

    /* Restore regs */
    outb(0x3C4, 0x02); outb(0x3C5, seq2);
    outb(0x3C4, 0x04); outb(0x3C5, seq4);
    outb(0x3CE, 0x04); outb(0x3CF, gc4);
    outb(0x3CE, 0x05); outb(0x3CF, gc5);
    outb(0x3CE, 0x06); outb(0x3CF, gc6);
}

static void vga_font_restore(void) {
    if (!vga_font_saved) return;

    outb(0x3C4, 0x02); u8 seq2 = inb(0x3C5);
    outb(0x3C4, 0x04); u8 seq4 = inb(0x3C5);
    outb(0x3CE, 0x04); u8 gc4 = inb(0x3CF);
    outb(0x3CE, 0x05); u8 gc5 = inb(0x3CF);
    outb(0x3CE, 0x06); u8 gc6 = inb(0x3CF);

    outb(0x3C4, 0x02); outb(0x3C5, 0x04);
    outb(0x3C4, 0x04); outb(0x3C5, 0x07);
    outb(0x3CE, 0x04); outb(0x3CF, 0x02);
    outb(0x3CE, 0x05); outb(0x3CF, 0x00);
    outb(0x3CE, 0x06); outb(0x3CF, 0x04);

    memcpy((void*)0xA0000, vga_font, sizeof(vga_font));

    outb(0x3C4, 0x02); outb(0x3C5, seq2);
    outb(0x3C4, 0x04); outb(0x3C5, seq4);
    outb(0x3CE, 0x04); outb(0x3CF, gc4);
    outb(0x3CE, 0x05); outb(0x3CF, gc5);
    outb(0x3CE, 0x06); outb(0x3CF, gc6);
}

static void vga_set_palette(u8 idx, u8 r, u8 g, u8 b);
static void vga_set_mode13(void) {
    vga_write_regs(vga_mode13_regs);
    gfx_w = 320;
    gfx_h = 200;
    gfx_pitch = 320;
    gfx_bpp = 8;
    gfx_lfb = 0xA0000;
}
static void vga_reset_text_palette(void) {
    /* Standard VGA 16-color palette (0-255 RGB). */
    static const u8 pal[16][3] = {
        {   0,   0,   0 }, /* 0 black */
        {   0,   0, 170 }, /* 1 blue */
        {   0, 170,   0 }, /* 2 green */
        {   0, 170, 170 }, /* 3 cyan */
        { 170,   0,   0 }, /* 4 red */
        { 170,   0, 170 }, /* 5 magenta */
        { 170,  85,   0 }, /* 6 brown */
        { 170, 170, 170 }, /* 7 light grey */
        {  85,  85,  85 }, /* 8 dark grey */
        {  85,  85, 255 }, /* 9 bright blue */
        {  85, 255,  85 }, /* 10 bright green */
        {  85, 255, 255 }, /* 11 bright cyan */
        { 255,  85,  85 }, /* 12 bright red */
        { 255,  85, 255 }, /* 13 bright magenta */
        { 255, 255,  85 }, /* 14 yellow */
        { 255, 255, 255 }  /* 15 white */
    };
    for (u8 i = 0; i < 16; i++) {
        vga_set_palette(i, pal[i][0], pal[i][1], pal[i][2]);
    }
}

static void vga_set_text(void) {
    if (fb_enabled) {
        vga_cls();
        return;
    }
    vga_write_regs(vga_text_regs);
    vga_font_restore();
    vga_reset_text_palette();
    gfx_w = 80;
    gfx_h = 25;
    gfx_pitch = 160;
    gfx_bpp = 0;
    gfx_lfb = (u32)vga;
    cur_x = 0;
    cur_y = 0;
    vga_update_cursor();
}

static void fb_update_dims(void) {
    if (fb_font_h == 0) fb_font_h = 16;
    fb_cols = gfx_w / (8 * fb_scale);
    fb_rows = gfx_h / (fb_font_h * fb_scale);
    if (fb_cols == 0) fb_cols = 1;
    if (fb_rows == 0) fb_rows = 1;
}

static void bootinfo_init(void) {
    bootinfo_t *bi = (bootinfo_t*)BOOTINFO_ADDR;
    if (bi->magic != BOOTINFO_MAGIC) return;
    gfx_mode_id = bi->mode;
    if (bi->vbe_list && bi->vbe_count) {
        vbe_list = (vbe_list_entry_t*)bi->vbe_list;
        vbe_list_count = (int)bi->vbe_count;
        if (vbe_list_count < 0) vbe_list_count = 0;
        if (vbe_list_count > 128) vbe_list_count = 128;
    }
    if (!fb_console_enable) return;
    if (bi->lfb == 0) return;
    if (bi->bpp != 8 && bi->bpp != 32) return;
    fb_enabled = 1;
    gfx_w = bi->width;
    gfx_h = bi->height;
    gfx_pitch = bi->pitch;
    gfx_bpp = bi->bpp;
    gfx_lfb = bi->lfb;
    fb_bytes = gfx_bpp / 8;
    if (gfx_bpp == 8) {
        if (gfx_pitch < gfx_w || gfx_pitch > (gfx_w * 2)) {
            gfx_pitch = gfx_w;
        }
    } else if (gfx_bpp == 32) {
        u32 min_pitch = gfx_w * 4;
        if (gfx_pitch < min_pitch || gfx_pitch > (min_pitch * 2)) {
            gfx_pitch = min_pitch;
        }
    }
    /* Use BIOS-copied font (stride may be stored in high 16 bits) */
    fb_font = (u8*)bi->font;
    u32 fh = bi->font_h;
    fb_font_h = (fh & 0xFFFF) ? (fh & 0xFFFF) : 16;
    fb_font_stride = (fh >> 16);
    if (fb_font_stride == 0) fb_font_stride = fb_font_h;
    fb_scale = (gfx_w >= 1024 || gfx_h >= 768) ? 2 : 1;
    fb_update_dims();
    fb_fg = 255;
    fb_bg = 0;
    fb_fg32 = 0x00FFFFFFu;
    fb_bg32 = 0x00000000u;
    if (gfx_bpp == 8) {
        for (u32 i = 0; i < 256; i++) {
            vga_set_palette((u8)i, (u8)i, (u8)i, (u8)i);
        }
    }
    vga_cls();
    /* draw a visible debug pattern (should show even if text fails) */
    u8 *dst = (u8*)gfx_lfb;
    u32 max_y = gfx_h < 64 ? gfx_h : 64;
    u32 max_x = gfx_w < 256 ? gfx_w : 256;
    for (u32 y = 0; y < max_y; y++) {
        for (u32 x = 0; x < max_x; x++) {
            if (gfx_bpp == 8) {
                dst[y * gfx_pitch + x] = (u8)(x & 255);
            } else if (gfx_bpp == 32) {
                u32 *row = (u32*)(dst + y * gfx_pitch);
                u8 v = (u8)(x & 255);
                row[x] = (u32)(v | (v << 8) | (v << 16));
            }
        }
    }
    for (u32 y = 0; y < gfx_h; y++) {
        if (gfx_bpp == 8) {
            dst[y * gfx_pitch + 0] = 255;
            if (gfx_w > 1) dst[y * gfx_pitch + 1] = 255;
        } else if (gfx_bpp == 32) {
            u32 *row = (u32*)(dst + y * gfx_pitch);
            row[0] = 0x00FFFFFFu;
            if (gfx_w > 1) row[1] = 0x00FFFFFFu;
        }
    }
}

typedef struct {
    u16 attributes;
    u8 winA, winB;
    u16 granularity, winSize;
    u16 segA, segB;
    u32 winFuncPtr;
    u16 pitch;
    u16 xres, yres;
    u8 wChar, yChar, planes, bpp, banks, memoryModel, bankSize, imagePages, reserved0;
    u8 redMask, redPosition, greenMask, greenPosition, blueMask, bluePosition;
    u8 rsvMask, rsvPosition, directColorAttributes;
    u32 physBasePtr;
    u32 offScreenMemOffset;
    u16 offScreenMemSize;
    u8 reserved1[206];
} __attribute__((packed)) VbeModeInfo;

static VbeModeInfo vbe_info;

typedef struct {
    char signature[4];
    u16 version;
    u32 oem;
    u32 capabilities;
    u32 video_modes; /* segment:offset */
    u16 total_memory;
    u16 oem_sw_rev;
    u32 oem_vendor;
    u32 oem_product;
    u32 oem_product_rev;
    u8 reserved[222];
    u8 oem_data[256];
} __attribute__((packed)) VbeInfo;

typedef struct {
    u32 mode;
    u32 w;
    u32 h;
    u32 bpp;
    u32 pitch;
} vbe_mode_t;

static VbeInfo vbe_ctrl;
static vbe_mode_t vbe_mode_buf[128];

static int vbe_collect_modes(vbe_mode_t *out, int max) {
    if (max <= 0) return 0;
    VbeInfo *rm_ctrl = (VbeInfo*)VBE_RM_BUF;
    rm_ctrl->signature[0] = 'V';
    rm_ctrl->signature[1] = 'B';
    rm_ctrl->signature[2] = 'E';
    rm_ctrl->signature[3] = '2';
    int ax = bios_vbe_get_controller_info(rm_ctrl);
    if ((ax & 0xFFFF) != 0x004F) return -1;
    memcpy(&vbe_ctrl, rm_ctrl, sizeof(vbe_ctrl));
    if (!(vbe_ctrl.signature[0] == 'V' && vbe_ctrl.signature[1] == 'E' &&
          vbe_ctrl.signature[2] == 'S' && vbe_ctrl.signature[3] == 'A')) {
        return -1;
    }
    u16 off = (u16)(vbe_ctrl.video_modes & 0xFFFF);
    u16 seg = (u16)((vbe_ctrl.video_modes >> 16) & 0xFFFF);
    u16 *list = (u16*)(((u32)seg << 4) + off);
    int count = 0;
    while (count < max) {
        u16 mode = *list++;
        if (mode == 0xFFFF) break;
        ax = bios_vbe_get_mode_info(mode, VBE_RM_BUF);
        if ((ax & 0xFFFF) != 0x004F) continue;
        memcpy(&vbe_info, VBE_RM_BUF, sizeof(vbe_info));
        if ((vbe_info.attributes & 0x90) != 0x90) continue; /* supported + LFB */
        if (vbe_info.bpp != 8) continue;
        if (vbe_info.memoryModel != 4) continue; /* packed pixel */
        out[count].mode = mode;
        out[count].w = vbe_info.xres;
        out[count].h = vbe_info.yres;
        out[count].bpp = vbe_info.bpp;
        out[count].pitch = vbe_info.pitch;
        count++;
    }
    return count;
}

static int vbe_set_mode(u16 mode) {
    int ax = bios_vbe_get_mode_info(mode, VBE_RM_BUF);
    if ((ax & 0xFFFF) != 0x004F) return -1;
    memcpy(&vbe_info, VBE_RM_BUF, sizeof(vbe_info));
    if ((vbe_info.attributes & 0x90) != 0x90) return -1; /* supported + LFB */
    if (vbe_info.bpp != 8 && vbe_info.bpp != 32) return -1;
    ax = bios_vbe_set_mode((u16)(mode | 0x4000));
    if ((ax & 0xFFFF) != 0x004F) return -1;
    gfx_w = vbe_info.xres;
    gfx_h = vbe_info.yres;
    gfx_pitch = vbe_info.pitch;
    gfx_bpp = vbe_info.bpp;
    gfx_lfb = vbe_info.physBasePtr;
    /* safety: clamp bogus pitch values (should be close to width for 8bpp) */
    if (gfx_bpp == 8) {
        if (gfx_pitch < gfx_w || gfx_pitch > (gfx_w * 2)) {
            gfx_pitch = gfx_w;
        }
    } else if (gfx_bpp == 32) {
        u32 min_pitch = gfx_w * 4;
        if (gfx_pitch < min_pitch || gfx_pitch > (min_pitch * 2)) {
            gfx_pitch = min_pitch;
        }
    }
    return 0;
}

static void vga_set_palette(u8 idx, u8 r, u8 g, u8 b) {
    outb(0x3C8, idx);
    outb(0x3C9, (u8)(r >> 2));
    outb(0x3C9, (u8)(g >> 2));
    outb(0x3C9, (u8)(b >> 2));
}

static void pic_disable(void) {
    outb(0x21, 0xFF);
    outb(0xA1, 0xFF);
}

static void pic_remap(void) {
    u8 a1 = inb(0x21);
    u8 a2 = inb(0xA1);
    outb(0x20, 0x11);
    outb(0xA0, 0x11);
    outb(0x21, 0x20); /* IRQ0 -> 0x20 */
    outb(0xA1, 0x28); /* IRQ8 -> 0x28 */
    outb(0x21, 0x04);
    outb(0xA1, 0x02);
    outb(0x21, 0x01);
    outb(0xA1, 0x01);
    outb(0x21, a1);
    outb(0xA1, a2);
}

static void pic_setmask(u8 master, u8 slave) {
    outb(0x21, master);
    outb(0xA1, slave);
}

static void pit_init(u32 hz) {
    if (hz < 18) hz = 18;
    if (hz > 1000) hz = 1000;
    u32 div = 1193182u / hz;
    outb(0x43, 0x36);
    outb(0x40, (u8)(div & 0xFF));
    outb(0x40, (u8)((div >> 8) & 0xFF));
}

static void vga_cls(void) {
    if (fb_enabled) {
        u8 *dst = (u8*)gfx_lfb;
        for (u32 y = 0; y < gfx_h; y++) {
            u8 *row = dst + y * gfx_pitch;
            if (fb_bytes == 1) {
                memset(row, fb_bg, gfx_pitch);
            } else if (fb_bytes == 4) {
                u32 n = gfx_pitch / 4;
                for (u32 x = 0; x < n; x++) ((u32*)row)[x] = fb_bg32;
            }
        }
        cur_x = 0;
        cur_y = 0;
        return;
    }
    for (u32 y = 0; y < 25; y++) {
        for (u32 x = 0; x < 80; x++)
            vga[y * 80 + x] = 0x0720;
    }
    cur_x = 0;
    cur_y = 0;
    vga_update_cursor();
}

static void vga_setcursor(u32 x, u32 y) {
    if (fb_enabled) {
        if (x >= fb_cols) x = fb_cols - 1;
        if (y >= fb_rows) y = fb_rows - 1;
        cur_x = (u8)x;
        cur_y = (u8)y;
        return;
    }
    if (x > 79) x = 79;
    if (y > 24) y = 24;
    cur_x = (u8)x;
    cur_y = (u8)y;
    vga_update_cursor();
}

static void vga_hex(u32 v) {
    for (int i = 7; i >= 0; i--) {
        u8 d = (v >> (i * 4)) & 0xF;
        vga_putc(d < 10 ? '0' + d : 'A' + d - 10);
    }
}

static const char keymap[128] = {
    0,27,'1','2','3','4','5','6','7','8','9','0','-','=',8,9,
    'q','w','e','r','t','y','u','i','o','p','[',']','\n',0,'a','s',
    'd','f','g','h','j','k','l',';','\'','`',0,'\\','z','x','c','v',
    'b','n','m',',','.','/',0,'*',0,' ',0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,'7','8','9','-','4','5','6','+','1','2','3','0','.'
};

static const char keymap_shift[128] = {
    0,27,'!','@','#','$','%','^','&','*','(',')','_','+',8,9,
    'Q','W','E','R','T','Y','U','I','O','P','{','}','\n',0,'A','S',
    'D','F','G','H','J','K','L',':','"','~',0,'|','Z','X','C','V',
    'B','N','M','<','>','?',0,'*',0,' ',0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,'7','8','9','-','4','5','6','+','1','2','3','0','.'
};

enum {
    KEY_UP = 0x100,
    KEY_DOWN = 0x101,
    KEY_LEFT = 0x102,
    KEY_RIGHT = 0x103,
    KEY_DEL = 0x104
};

#define KBD_BUF_SIZE 64
static volatile u16 kbd_buf[KBD_BUF_SIZE];
static volatile u32 kbd_head = 0;
static volatile u32 kbd_tail = 0;
static u8 kbd_shift = 0;
static u8 kbd_ctrl = 0;
static u8 kbd_ext = 0;
static u8 key_down[128];     /* per-scancode held state: regular keys */
static u8 ext_key_down[128]; /* per-scancode held state: 0xE0-prefixed keys */

static void kbd_push(u16 v) {
    u32 next = (kbd_head + 1) % KBD_BUF_SIZE;
    if (next == kbd_tail) return;
    kbd_buf[kbd_head] = v;
    kbd_head = next;
}

static int kbd_pop(void) {
    if (kbd_head == kbd_tail) return 0;
    u16 v = kbd_buf[kbd_tail];
    kbd_tail = (kbd_tail + 1) % KBD_BUF_SIZE;
    return (int)v;
}

static int kbd_handle_scancode(u8 sc) {
    if (sc == 0xE0) { kbd_ext = 1; return 0; }
    if (sc == 0x2A || sc == 0x36) { kbd_shift = 1; kbd_ext = 0; return 0; }
    if (sc == 0xAA || sc == 0xB6) { kbd_shift = 0; kbd_ext = 0; return 0; }
    if (sc == 0x1D) { kbd_ctrl = 1; kbd_ext = 0; return 0; }
    if (sc == 0x9D) { kbd_ctrl = 0; kbd_ext = 0; return 0; }
    /* break code: clear key state and return */
    if (sc & 0x80) {
        u8 base = sc & 0x7F;
        if (kbd_ext) { ext_key_down[base] = 0; kbd_ext = 0; }
        else { key_down[base] = 0; }
        return 0;
    }
    if (kbd_ext) {
        ext_key_down[sc] = 1;
        kbd_ext = 0;
        switch (sc) {
        case 0x48: kbd_push(KEY_UP); return 0;
        case 0x50: kbd_push(KEY_DOWN); return 0;
        case 0x4B: kbd_push(KEY_LEFT); return 0;
        case 0x4D: kbd_push(KEY_RIGHT); return 0;
        case 0x53: kbd_push(KEY_DEL); return 0;
        default: return 0;
        }
    }
    key_down[sc] = 1;
    char c = kbd_shift ? keymap_shift[sc] : keymap[sc];
    if (kbd_ctrl && c >= 'A' && c <= 'Z') c = c - 'A' + 1;
    else if (kbd_ctrl && c >= 'a' && c <= 'z') c = c - 'a' + 1;
    if (kbd_ctrl && c == 3) { /* Ctrl+C */
        kill_program = 1;
        return 1;
    }
    if (c) kbd_push((u8)c);
    return 0;
}

static int kbd_getkey(void) {
    for (;;) {
        int k = kbd_pop();
        if (k) return k;
        if (inb(0x64) & 1) {
            u8 sc = inb(0x60);
            if (kbd_handle_scancode(sc)) {
                exit_requested = 1;
                return 0;
            }
            continue;
        }
        __asm__ volatile("sti; hlt");
    }
}

static int kbd_getkey_nb(void) {
    int k = kbd_pop();
    if (k) return k;
    if (inb(0x64) & 1) {
        u8 sc = inb(0x60);
        if (kbd_handle_scancode(sc)) {
            exit_requested = 1;
            return 0;
        }
        return kbd_pop();
    }
    return 0;
}

static u32 readline(char *buf, u32 max) {
    u32 n = 0;
    while (n + 1 < max) {
        int k = kbd_getkey();
        char c = (k >= 0 && k < 256) ? (char)k : 0;
        if (!c) continue;
        if (c == '\n') { vga_putc('\n'); break; }
        if (c == 8) {
            if (n) { n--; vga_putc(8); vga_putc(' '); vga_putc(8); }
            continue;
        }
        buf[n++] = c;
        vga_putc(c);
    }
    buf[n] = 0;
    return n;
}

static void ata_wait_busy(void) { while (inb(0x1F7) & 0x80) {} }
static void ata_wait_drq(void) { while (!(inb(0x1F7) & 8)) {} }

static void ata_read_sector(u32 lba, u8 *buf) {
    ata_wait_busy();
    outb(0x1F6, 0xE0 | ((lba >> 24) & 0x0F));
    outb(0x1F2, 1);
    outb(0x1F3, (u8)lba);
    outb(0x1F4, (u8)(lba >> 8));
    outb(0x1F5, (u8)(lba >> 16));
    outb(0x1F7, 0x20);
    ata_wait_busy();
    ata_wait_drq();
    for (u32 i = 0; i < 256; i++) {
        u16 w = inw(0x1F0);
        buf[i * 2] = (u8)w;
        buf[i * 2 + 1] = (u8)(w >> 8);
    }
}

static void ata_write_sector(u32 lba, const u8 *buf) {
    ata_wait_busy();
    outb(0x1F6, 0xE0 | ((lba >> 24) & 0x0F));
    outb(0x1F2, 1);
    outb(0x1F3, (u8)lba);
    outb(0x1F4, (u8)(lba >> 8));
    outb(0x1F5, (u8)(lba >> 16));
    outb(0x1F7, 0x30);
    ata_wait_busy();
    ata_wait_drq();
    for (u32 i = 0; i < 256; i++) {
        u16 w = buf[i * 2] | (buf[i * 2 + 1] << 8);
        outw(0x1F0, w);
    }
    outb(0x1F7, 0xE7);
    ata_wait_busy();
}

static void ata_read_sectors(u32 lba, u32 count, u8 *buf) {
    for (u32 i = 0; i < count; i++) ata_read_sector(lba + i, buf + i * 512);
}

static void ata_write_sectors(u32 lba, u32 count, const u8 *buf) {
    for (u32 i = 0; i < count; i++) ata_write_sector(lba + i, buf + i * 512);
}

static void fs_load_dir(void) { ata_read_sectors(FS_DIR_LBA, DIR_SECTORS, (u8*)dir); }
static void fs_sync_dir(void) { ata_write_sectors(FS_DIR_LBA, DIR_SECTORS, (const u8*)dir); }

static void normalize_name(const char *in, char *out) {
    int i = 0;
    for (; i < 15 && in[i]; i++) {
        char c = in[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        out[i] = c;
    }
    out[i] = 0;
}

static int fs_find(const char *name) {
    for (u32 i = 0; i < (u32)(DIR_SECTORS * 512 / sizeof(dirent_t)); i++) {
        if (dir[i].name[0] == 0) continue;
        if (name_eq_ci(dir[i].name, name)) return (int)i;
    }
    return -1;
}

static u32 fs_next_free(void) {
    u32 max = FS_DATA_LBA;
    for (u32 i = 0; i < (u32)(DIR_SECTORS * 512 / sizeof(dirent_t)); i++) {
        if (dir[i].name[0] == 0) continue;
        u32 end = dir[i].start + ((dir[i].size + 511) / 512);
        if (end > max) max = end;
    }
    return max;
}

/* Like fs_next_free but ignores entry excl, so that entry's old sectors are
   treated as free.  Used when overwriting an existing file. */
static u32 fs_next_free_excl(int excl) {
    u32 max = FS_DATA_LBA;
    for (u32 i = 0; i < (u32)(DIR_SECTORS * 512 / sizeof(dirent_t)); i++) {
        if (dir[i].name[0] == 0) continue;
        if ((int)i == excl) continue;
        u32 end = dir[i].start + ((dir[i].size + 511) / 512);
        if (end > max) max = end;
    }
    return max;
}

static int fs_read(const char *name, void *buf, u32 max) {
    int idx = fs_find(name);
    if (idx < 0) return -1;
    u32 size = dir[idx].size;
    if (size > max) return -1;
    u32 full = size / 512;       /* number of complete sectors */
    u32 rem  = size % 512;       /* leftover bytes in the last sector */
    if (full > 0)
        ata_read_sectors(dir[idx].start, full, (u8*)buf);
    if (rem > 0) {
        /* Read the last partial sector into a stack bounce buffer so we
           copy only 'rem' bytes and never overwrite past the caller's buffer. */
        u8 tmp[512];
        ata_read_sector(dir[idx].start + full, tmp);
        memcpy((u8*)buf + full * 512, tmp, rem);
    }
    return (int)size;
}

static int fs_write(const char *name, const void *buf, u32 size) {
    int idx = fs_find(name);
    int is_overwrite = (idx >= 0);
    if (idx < 0) {
        for (u32 i = 0; i < (u32)(DIR_SECTORS * 512 / sizeof(dirent_t)); i++) {
            if (dir[i].name[0] == 0) { idx = (int)i; break; }
        }
    }
    if (idx < 0) return -1;
    u32 new_sectors = (size + 511) / 512;
    u32 start;
    if (is_overwrite) {
        u32 old_sectors = (dir[idx].size + 511) / 512;
        if (new_sectors <= old_sectors) {
            start = dir[idx].start;           /* fits: reuse in-place, no leak */
        } else {
            start = fs_next_free_excl(idx);   /* grow: reclaim old, append past rest */
        }
    } else {
        start = fs_next_free();
    }
    u32 sectors = new_sectors;
    ata_write_sectors(start, sectors, (const u8*)buf);
    memset(dir[idx].name, 0, sizeof(dir[idx].name));
    u32 n = strlen(name); if (n > 15) n = 15;
    memcpy(dir[idx].name, name, n);
    dir[idx].start = start;
    dir[idx].size = size;
    dir[idx].flags = 0;
    fs_sync_dir();
    return 0;
}

static int alloc_fd(void) {
    for (int i = 0; i < OPEN_MAX; i++) if (!ofiles[i].used) return i;
    return -1;
}

static open_file_t *get_ofile(int fd) {
    int idx = fd - 3;
    if (idx < 0 || idx >= OPEN_MAX) return 0;
    if (!ofiles[idx].used) return 0;
    return &ofiles[idx];
}

static void ofile_flush(open_file_t *f) {
    if (!f->sector_valid || !f->sector_dirty) return;
    ata_write_sector(f->cur_lba, f->sector);
    f->sector_dirty = 0;
}

static void ofile_load_sector(open_file_t *f, u32 lba) {
    if (f->sector_valid && f->cur_lba == lba) return;
    ofile_flush(f);
    f->cur_lba = lba;
    f->sector_valid = 1;
    f->sector_dirty = 0;
    /* If reading past current size or new file, zero-fill.
       Use division to avoid lba*512 overflowing u32. */
    if (f->mode == 1) {
        u32 file_sectors = f->size / 512 + (f->size % 512 ? 1 : 0);
        if (lba >= file_sectors) {
            memset(f->sector, 0, 512);
        } else {
            ata_read_sector(lba, f->sector);
        }
    } else {
        ata_read_sector(lba, f->sector);
    }
}

struct idt_entry { u16 off1; u16 sel; u8 zero; u8 type; u16 off2; } __attribute__((packed));
static struct idt_entry idt[256];

static void idt_set_gate(int n, u32 handler, u16 sel, u8 type) {
    idt[n].off1 = handler & 0xFFFF;
    idt[n].sel = sel;
    idt[n].zero = 0;
    idt[n].type = type;
    idt[n].off2 = (handler >> 16) & 0xFFFF;
}

struct tss_entry {
    u32 prev;
    u32 esp0;
    u32 ss0;
    u32 esp1; u32 ss1; u32 esp2; u32 ss2;
    u32 cr3; u32 eip; u32 eflags;
    u32 eax, ecx, edx, ebx;
    u32 esp, ebp, esi, edi;
    u32 es, cs, ss, ds, fs, gs;
    u32 ldt; u16 trap; u16 iomap;
} __attribute__((packed));

static struct tss_entry tss;

static void gdt_set_tss(u32 base, u32 limit) {
    u64 desc = 0;
    desc |= (limit & 0xFFFFULL);
    desc |= ((u64)(base & 0xFFFFFF) << 16);
    desc |= (u64)0x89 << 40;
    desc |= ((u64)((limit >> 16) & 0xF) << 48);
    desc |= ((u64)((base >> 24) & 0xFF) << 56);
    gdt[5] = desc;
}

static void tss_init(void) {
    memset(&tss, 0, sizeof(tss));
    tss.ss0 = KERNEL_DS;
    tss.esp0 = (u32)&kernel_stack_top;
    gdt_set_tss((u32)&tss, sizeof(tss) - 1);
    ltr(TSS_SEL);
}

struct regs {
    u32 gs, fs, es, ds;
    u32 edi, esi, ebp, esp, ebx, edx, ecx, eax;
};

struct fault_frame {
    u32 gs, fs, es, ds;
    u32 edi, esi, ebp, esp, ebx, edx, ecx, eax;
    u32 int_no;
    u32 err;
    u32 eip;
    u32 cs;
    u32 eflags;
    u32 useresp;
    u32 ss;
};

#define MAX_THREADS 8
#define USER_TRAMP_OFF (USER_FB_OFFSET - 0x1000)

typedef struct {
    int used;
    struct fault_frame ctx;
    u32 sleep_until;
} thread_t;

static thread_t threads[MAX_THREADS];
static int current_tid = 0;
static int thread_count = 0;
int thread_exit_pending = 0;

/* ================================================================
 * Network: PCI + RTL8139 + ARP + IP/UDP
 * ================================================================ */

/* PCI config space */
static u32 pci_read32(u8 bus, u8 dev, u8 fn, u8 reg) {
    outl(0xCF8, 0x80000000u|((u32)bus<<16)|((u32)dev<<11)|((u32)fn<<8)|(reg&0xFC));
    return inl(0xCFC);
}
static void pci_write32(u8 bus, u8 dev, u8 fn, u8 reg, u32 val) {
    outl(0xCF8, 0x80000000u|((u32)bus<<16)|((u32)dev<<11)|((u32)fn<<8)|(reg&0xFC));
    outl(0xCFC, val);
}

/* RTL8139 buffers + state */
#define RTL_RX_SIZE 8192
static u8  rtl_rx_buf[RTL_RX_SIZE + 1536] __attribute__((aligned(4)));
static u8  rtl_tx_buf[4][1536]             __attribute__((aligned(4)));
static u16 rtl_iobase  = 0;
static u8  rtl_mac[6];
static u32 rtl_cur_rx  = 0;
static int rtl_tx_idx  = 0;
static int rtl_ready   = 0;

/* QEMU user-mode network (always 10.0.2.15/24, gw 10.0.2.2) */
#define NET_MY_IP  ((10u<<24)|(0u<<16)|(2u<<8)|15u)
#define NET_GW_IP  ((10u<<24)|(0u<<16)|(2u<<8)|2u)
#define NET_MASK   0xFFFFFF00u

/* ARP cache */
#define ARP_CACHE_MAX 8
typedef struct { u32 ip; u8 mac[6]; } arp_ent_t;
static arp_ent_t arp_cache[ARP_CACHE_MAX];
static int       arp_count = 0;

/* UDP receive queue */
#define UDP_Q_MAX  8
#define UDP_PL_MAX 1472
typedef struct { u32 src_ip; u16 src_port; u16 dst_port; u16 len; u8 data[UDP_PL_MAX]; } udp_pkt_t;
static udp_pkt_t udp_q[UDP_Q_MAX];
static int udp_q_head = 0, udp_q_tail = 0;

/* Big-endian packet helpers (no alignment assumptions) */
static u16 net_r16(u8 *p) { return ((u16)p[0]<<8)|p[1]; }
static u32 net_r32(u8 *p) { return ((u32)p[0]<<24)|((u32)p[1]<<16)|((u32)p[2]<<8)|p[3]; }
static void net_w16(u8 *p, u16 v) { p[0]=v>>8; p[1]=v&0xFF; }
static void net_w32(u8 *p, u32 v) { p[0]=(v>>24)&0xFF; p[1]=(v>>16)&0xFF; p[2]=(v>>8)&0xFF; p[3]=v&0xFF; }

/* IP header checksum */
static u16 ip_cksum(u8 *d, int len) {
    u32 s = 0;
    while (len > 1) { s += ((u32)d[0]<<8)|d[1]; d += 2; len -= 2; }
    if (len) s += (u32)d[0]<<8;
    while (s >> 16) s = (s & 0xFFFF) + (s >> 16);
    return (u16)(~s);
}

/* Send raw Ethernet frame */
static void rtl_send_raw(u8 *buf, u32 len) {
    if (!rtl_ready || len < 14 || len > 1514) return;
    int idx = rtl_tx_idx & 3;
    memcpy(rtl_tx_buf[idx], buf, len);
    if (len < 60) { memset(rtl_tx_buf[idx]+len, 0, 60-len); len = 60; }
    outl(rtl_iobase + 0x20 + idx*4, (u32)rtl_tx_buf[idx]);
    outl(rtl_iobase + 0x10 + idx*4, len & 0x1FFF);
    int t = 100000;
    while (!(inl(rtl_iobase + 0x10 + idx*4) & 0x8000) && t-- > 0) {}
    rtl_tx_idx = (rtl_tx_idx + 1) & 3;
}

/* ARP helpers */
static int arp_find(u32 ip, u8 *mac_out) {
    for (int i = 0; i < arp_count; i++)
        if (arp_cache[i].ip == ip) { memcpy(mac_out, arp_cache[i].mac, 6); return 1; }
    return 0;
}
static void arp_store(u32 ip, u8 *mac) {
    for (int i = 0; i < arp_count; i++)
        if (arp_cache[i].ip == ip) { memcpy(arp_cache[i].mac, mac, 6); return; }
    if (arp_count < ARP_CACHE_MAX) {
        arp_cache[arp_count].ip = ip;
        memcpy(arp_cache[arp_count].mac, mac, 6);
        arp_count++;
    }
}
static void arp_send_request(u32 tgt_ip) {
    u8 f[42]; memset(f, 0, 42);
    memset(f, 0xFF, 6); memcpy(f+6, rtl_mac, 6); f[12]=0x08; f[13]=0x06;
    u8 *a = f+14;
    a[0]=0; a[1]=1; a[2]=0x08; a[3]=0; a[4]=6; a[5]=4; a[6]=0; a[7]=1;
    memcpy(a+8, rtl_mac, 6); net_w32(a+14, NET_MY_IP);
    memset(a+18, 0, 6);       net_w32(a+24, tgt_ip);
    rtl_send_raw(f, 42);
}

/* Process one received Ethernet frame */
static void net_rx_frame(u8 *f, int len) {
    if (len < 14) return;
    u16 et = net_r16(f+12);
    if (et == 0x0806 && len >= 42) {              /* ARP */
        u8 *a = f+14;
        u16 op = net_r16(a+6);
        u32 sip = net_r32(a+14);
        arp_store(sip, a+8);
        if (op == 1 && net_r32(a+24) == NET_MY_IP) { /* request for us -> reply */
            u8 r[42]; memset(r, 0, 42);
            memcpy(r, f+6, 6); memcpy(r+6, rtl_mac, 6); r[12]=0x08; r[13]=0x06;
            u8 *ra = r+14;
            ra[0]=0; ra[1]=1; ra[2]=0x08; ra[3]=0; ra[4]=6; ra[5]=4; ra[6]=0; ra[7]=2;
            memcpy(ra+8, rtl_mac, 6); net_w32(ra+14, NET_MY_IP);
            memcpy(ra+18, f+6, 6);    net_w32(ra+24, sip);
            rtl_send_raw(r, 42);
        }
    } else if (et == 0x0800 && len >= 34) {       /* IPv4 */
        u8 *ip = f+14;
        if ((ip[0]&0xF0) != 0x40 || ip[9] != 17) return;
        u32 dip = net_r32(ip+16);
        if (dip != NET_MY_IP && dip != 0xFFFFFFFFu) return;
        int ihl = (ip[0]&0x0F)*4;
        if (len < 14+ihl+8) return;
        u8 *u = f+14+ihl;
        u16 sp = net_r16(u+0), dp = net_r16(u+2), ul = net_r16(u+4);
        int pl = (int)ul - 8;
        if (pl < 0 || pl > UDP_PL_MAX) return;
        int nxt = (udp_q_tail+1) & (UDP_Q_MAX-1);
        if (nxt != udp_q_head) {
            udp_pkt_t *pkt = &udp_q[udp_q_tail];
            pkt->src_ip = net_r32(ip+12); pkt->src_port = sp;
            pkt->dst_port = dp; pkt->len = (u16)pl;
            memcpy(pkt->data, u+8, pl);
            udp_q_tail = nxt;
        }
    }
}

/* Poll RTL8139 RX ring buffer */
static void rtl_poll(void) {
    if (!rtl_ready) return;
    while (!(inb(rtl_iobase + 0x37) & 0x01)) {
        u32 off  = rtl_cur_rx & (RTL_RX_SIZE - 1);
        u8 *hdr  = rtl_rx_buf + off;
        u16 stat = (u16)(hdr[0] | ((u16)hdr[1]<<8));
        u16 rlen = (u16)(hdr[2] | ((u16)hdr[3]<<8)); /* includes 4-byte CRC */
        if (!(stat & 1) || rlen < 8 || rlen > 1518) {
            rtl_cur_rx = 0;
            outw(rtl_iobase + 0x38, (u16)(0 - 0x10));
            break;
        }
        int flen = (int)rlen - 4;
        u8 tmp[1514];
        u32 doff = (rtl_cur_rx + 4) & (RTL_RX_SIZE - 1);
        if (doff + flen <= RTL_RX_SIZE) {
            memcpy(tmp, rtl_rx_buf + doff, flen);
        } else {
            u32 p1 = RTL_RX_SIZE - doff;
            memcpy(tmp, rtl_rx_buf + doff, p1);
            memcpy(tmp + p1, rtl_rx_buf, flen - p1);
        }
        net_rx_frame(tmp, flen);
        rtl_cur_rx = (rtl_cur_rx + 4 + rlen + 3) & ~3u;
        outw(rtl_iobase + 0x38, (u16)(rtl_cur_rx - 0x10));
    }
}

/* Send a UDP packet; src_port=0 auto-assigns 49152+ */
static u16 udp_src_counter = 49152;
static int net_udp_send(u32 dst_ip, u16 dst_port, u16 src_port, u8 *data, int dlen) {
    if (!rtl_ready || dlen < 0 || dlen > UDP_PL_MAX) return -1;
    if (src_port == 0) { src_port = udp_src_counter++; if (udp_src_counter == 0) udp_src_counter = 49152; }
    u32 route = ((dst_ip & NET_MASK) == (NET_MY_IP & NET_MASK)) ? dst_ip : NET_GW_IP;
    u8 dmac[6];
    int found = arp_find(route, dmac);
    if (!found) {
        arp_send_request(route);
        __asm__ volatile("sti");
        u32 ts = timer_ticks;
        while (!found && (timer_ticks - ts) < 10) {
            rtl_poll();
            found = arp_find(route, dmac);
            if (!found) __asm__ volatile("hlt");
        }
        __asm__ volatile("cli");
        if (!found) return -1;
    }
    int ip_len = 20 + 8 + dlen;
    u8 frame[14 + 20 + 8 + UDP_PL_MAX];
    memcpy(frame,   dmac,    6);
    memcpy(frame+6, rtl_mac, 6);
    frame[12]=0x08; frame[13]=0x00;
    u8 *ip = frame+14;
    ip[0]=0x45; ip[1]=0; net_w16(ip+2,(u16)ip_len);
    net_w16(ip+4,0); ip[6]=0x40; ip[7]=0; ip[8]=64; ip[9]=17;
    net_w16(ip+10,0); net_w32(ip+12,NET_MY_IP); net_w32(ip+16,dst_ip);
    net_w16(ip+10, ip_cksum(ip, 20));
    u8 *udp = frame+34;
    net_w16(udp+0, src_port); net_w16(udp+2, dst_port);
    net_w16(udp+4, (u16)(8+dlen)); net_w16(udp+6, 0);
    memcpy(frame+42, data, dlen);
    rtl_send_raw(frame, 14+ip_len);
    return dlen;
}

/* Initialize RTL8139 (PCI scan bus 0) */
static void net_init(void) {
    for (u8 d = 0; d < 32; d++) {
        u32 id = pci_read32(0, d, 0, 0);
        if ((id & 0xFFFF) == 0x10EC && (id>>16) == 0x8139) {
            pci_write32(0, d, 0, 4, pci_read32(0,d,0,4) | 0x7);
            rtl_iobase = (u16)(pci_read32(0,d,0,0x10) & 0xFFFC);
            break;
        }
    }
    if (!rtl_iobase) return;
    outb(rtl_iobase+0x52, 0);                /* power on */
    outb(rtl_iobase+0x37, 0x10);             /* software reset */
    int i = 0; while ((inb(rtl_iobase+0x37)&0x10) && i++<1000000) {}
    memset(rtl_rx_buf, 0, sizeof(rtl_rx_buf));
    outl(rtl_iobase+0x30, (u32)rtl_rx_buf);  /* RX buffer */
    outw(rtl_iobase+0x3C, 0);                /* disable interrupts */
    outl(rtl_iobase+0x44, 0x0000E78Fu);      /* RCR: AB|APM|WRAP|MXDMA=unlim|RXFTH=none */
    outl(rtl_iobase+0x40, 0x03000700u);      /* TCR: MXDMA=unlim, IFG=std */
    outb(rtl_iobase+0x37, 0x0C);             /* enable RX+TX */
    for (int j = 0; j < 6; j++) rtl_mac[j] = inb(rtl_iobase+j);
    rtl_ready = 1;
}

/* ================================================================ */

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

static void thread_clear_all(void) {
    for (int i = 0; i < MAX_THREADS; i++) threads[i].used = 0;
    thread_count = 0;
    current_tid = 0;
}

static void thread_init_main(u32 entry, u32 stack) {
    thread_clear_all();
    threads[0].used = 1;
    thread_count = 1;
    current_tid = 0;
    memset(&threads[0].ctx, 0, sizeof(threads[0].ctx));
    threads[0].sleep_until = 0;
    threads[0].ctx.ds = USER_DS;
    threads[0].ctx.es = USER_DS;
    threads[0].ctx.fs = USER_DS;
    threads[0].ctx.gs = USER_DS;
    threads[0].ctx.eip = entry;
    threads[0].ctx.cs = USER_CS;
    threads[0].ctx.eflags = 0x202;
    threads[0].ctx.useresp = stack;
    threads[0].ctx.ss = USER_DS;
}

static int thread_create(u32 entry, u32 stack) {
    if (thread_count >= MAX_THREADS) return -1;
    int tid = -1;
    for (int i = 0; i < MAX_THREADS; i++) {
        if (!threads[i].used) { tid = i; break; }
    }
    if (tid < 0) return -1;
    threads[tid].used = 1;
    thread_count++;
    memset(&threads[tid].ctx, 0, sizeof(threads[tid].ctx));
    threads[tid].sleep_until = 0;
    threads[tid].ctx.ds = USER_DS;
    threads[tid].ctx.es = USER_DS;
    threads[tid].ctx.fs = USER_DS;
    threads[tid].ctx.gs = USER_DS;
    threads[tid].ctx.eip = entry;
    threads[tid].ctx.cs = USER_CS;
    threads[tid].ctx.eflags = 0x202;
    threads[tid].ctx.useresp = stack;
    threads[tid].ctx.ss = USER_DS;
    return tid;
}

static void thread_kill(int tid) {
    if (tid < 0 || tid >= MAX_THREADS) return;
    if (!threads[tid].used) return;
    threads[tid].used = 0;
    threads[tid].sleep_until = 0;
    thread_count--;
    if (thread_count < 0) thread_count = 0;
}

static int thread_is_runnable(int tid) {
    if (tid < 0 || tid >= MAX_THREADS) return 0;
    if (!threads[tid].used) return 0;
    u32 w = threads[tid].sleep_until;
    if (w == 0) return 1;
    if ((int)(timer_ticks - w) >= 0) {
        threads[tid].sleep_until = 0;
        return 1;
    }
    return 0;
}

static void thread_schedule(struct fault_frame *f) {
    if (thread_count == 0) return;
    if (f->cs != USER_CS) return;
    if (current_tid < 0 || current_tid >= MAX_THREADS) return;
    int cur_used = threads[current_tid].used;
    if (cur_used) {
        threads[current_tid].ctx = *f;
    }
    int next = current_tid;
    for (int i = 0; i < MAX_THREADS; i++) {
        next = (next + 1) % MAX_THREADS;
        if (thread_is_runnable(next)) break;
    }
    if (!thread_is_runnable(next)) return;
    if (next == current_tid && cur_used && thread_is_runnable(current_tid)) return;
    current_tid = next;
    *f = threads[current_tid].ctx;
}

int thread_exit_switch(struct regs *r, u32 *frame) {
    thread_exit_pending = 0;
    thread_kill(current_tid);
    if (thread_count <= 0) {
        exit_requested = 1;
        return -1;
    }
    int next = current_tid;
    for (int i = 0; i < MAX_THREADS; i++) {
        next = (next + 1) % MAX_THREADS;
        if (thread_is_runnable(next)) break;
    }
    if (!thread_is_runnable(next)) {
        exit_requested = 1;
        return -1;
    }
    current_tid = next;
    struct fault_frame *c = &threads[current_tid].ctx;
    r->gs = c->gs;
    r->fs = c->fs;
    r->es = c->es;
    r->ds = c->ds;
    r->edi = c->edi;
    r->esi = c->esi;
    r->ebp = c->ebp;
    r->esp = c->esp;
    r->ebx = c->ebx;
    r->edx = c->edx;
    r->ecx = c->ecx;
    r->eax = c->eax;
    frame[0] = c->eip;
    frame[1] = c->cs;
    frame[2] = c->eflags;
    frame[3] = c->useresp;
    frame[4] = c->ss;
    return 0;
}

static void write_user_trampoline(void) {
    u8 *p = (u8*)(USER_BASE + USER_TRAMP_OFF);
    /* mov eax, SYS_EXIT; xor ebx, ebx; int 0x80; hlt */
    p[0] = 0xB8; p[1] = 6; p[2] = 0; p[3] = 0; p[4] = 0;
    p[5] = 0x31; p[6] = 0xDB;
    p[7] = 0xCD; p[8] = 0x80;
    p[9] = 0xF4;
}

static int exec_user(const char *name);
u32 syscall_dispatch(struct regs *r) {
    switch (r->eax) {
    case SYS_WRITE: {
        u32 fd = r->ebx;
        u32 off = r->ecx;
        u32 len = r->edx;
        if (!user_range_ok(off, len)) return (u32)-1;
        const char *buf = (const char*)(USER_BASE + off);
        if (fd == 1 || fd == 2) {
            for (u32 i = 0; i < len; i++) vga_putc(buf[i]);
            return len;
        }
        return (u32)-1;   /* invalid fd: signal error instead of silent drop */
    }
    case SYS_READ: {
        u32 fd = r->ebx;
        u32 off = r->ecx;
        u32 len = r->edx;
        if (!user_range_ok(off, len)) return (u32)-1;
        char *buf = (char*)(USER_BASE + off);
        if (len == 0) return 0;
        if (fd != 0) return (u32)-1;  /* invalid fd: signal error instead of silent EOF */
        return readline(buf, len);
    }
    case SYS_LIST: {
        u32 off = r->ebx;
        u32 max = r->ecx;
        if (!user_range_ok(off, max)) return (u32)-1;
        u8 *buf = (u8*)(USER_BASE + off);
        u32 count = 0;
        for (u32 i = 0; i < (u32)(DIR_SECTORS * 512 / sizeof(dirent_t)); i++) {
            if (dir[i].name[0] == 0) continue;
            if ((count + 1) * 20 > max) break;
            memcpy(buf + count * 20, dir[i].name, 16);
            *(u32*)(buf + count * 20 + 16) = dir[i].size;
            count++;
        }
        return count;
    }
    case SYS_LOAD: {
        u32 name_off = r->ebx;
        u32 buf_off = r->ecx;
        u32 max = r->edx;
        if (!user_range_ok(buf_off, max)) return (u32)-1;
        char uname[32];
        if (!user_copy_str(name_off, uname, sizeof(uname))) return (u32)-1;
        char kname[16]; normalize_name(uname, kname);
        void *buf = (void*)(USER_BASE + buf_off);
        return (u32)fs_read(kname, buf, max);
    }
    case SYS_SAVE: {
        u32 name_off = r->ebx;
        u32 buf_off = r->ecx;
        u32 size = r->edx;
        if (!user_range_ok(buf_off, size)) return (u32)-1;
        char uname[32];
        if (!user_copy_str(name_off, uname, sizeof(uname))) return (u32)-1;
        char kname[16]; normalize_name(uname, kname);
        const void *buf = (const void*)(USER_BASE + buf_off);
        return (u32)fs_write(kname, buf, size);
    }
    case SYS_EXEC: {
        u32 name_off = r->ebx;
        char uname[32];
        if (!user_copy_str(name_off, uname, sizeof(uname))) return (u32)-1;
        normalize_name(uname, next_prog);
        next_prog_set = 1;
        exit_requested = 1;
        return 0;
    }
    case SYS_EXIT:
        exit_code = r->ebx;
        exit_requested = 1;
        return 0;
    case SYS_SBRK: {
        u32 inc = r->ebx;
        u32 prev = user_brk_off;
        /* Check without overflow: inc > (GUARD - prev) catches both the
           normal over-limit case and any u32 wraparound from large inc. */
        if (inc > USER_STACK_GUARD - prev) return (u32)-1;
        user_brk_off = prev + inc;
        return prev;
    }
    case SYS_RENAME: {
        u32 old_off = r->ebx;
        u32 new_off = r->ecx;
        char oldu[32], newu[32];
        if (!user_copy_str(old_off, oldu, sizeof(oldu))) return (u32)-1;
        if (!user_copy_str(new_off, newu, sizeof(newu))) return (u32)-1;
        char kold[16], knew[16];
        normalize_name(oldu, kold);
        normalize_name(newu, knew);
        int idx = fs_find(kold);
        if (idx < 0) return (u32)-1;
        if (fs_find(knew) >= 0) return (u32)-1;
        memset(dir[idx].name, 0, sizeof(dir[idx].name));
        u32 n = strlen(knew); if (n > 15) n = 15;
        memcpy(dir[idx].name, knew, n);
        fs_sync_dir();
        return 0;
    }
    case SYS_DELETE: {
        u32 name_off = r->ebx;
        char uname[32];
        if (!user_copy_str(name_off, uname, sizeof(uname))) return (u32)-1;
        char kname[16];
        normalize_name(uname, kname);
        int idx = fs_find(kname);
        if (idx < 0) return (u32)-1;
        memset(&dir[idx], 0, sizeof(dirent_t));
        fs_sync_dir();
        return 0;
    }
    case SYS_GETKEY: {
        int k = 0;
        while (k == 0) k = kbd_getkey();
        return (u32)k;
    }
    case SYS_GETKEY_NB: {
        return (u32)kbd_getkey_nb();
    }
    case SYS_KEYSTATE: {
        u32 k = r->ebx;
        if (k < 128) return key_down[k];
        if (k == 0x100) return ext_key_down[0x48]; /* KEY_UP */
        if (k == 0x101) return ext_key_down[0x50]; /* KEY_DOWN */
        if (k == 0x102) return ext_key_down[0x4B]; /* KEY_LEFT */
        if (k == 0x103) return ext_key_down[0x4D]; /* KEY_RIGHT */
        if (k == 0x104) return ext_key_down[0x53]; /* KEY_DEL */
        return 0;
    }
    case SYS_UDP_SEND: {
        /* ebx=dst_ip ecx=dst_port edx=buf_off esi=len */
        u32 dst_ip   = r->ebx;
        u16 dst_port = (u16)r->ecx;
        u32 buf_off  = r->edx;
        u32 dlen     = r->esi;
        if (!user_range_ok(buf_off, dlen)) return (u32)-1;
        if (dlen > UDP_PL_MAX) return (u32)-1;
        return (u32)net_udp_send(dst_ip, dst_port, 0, (u8*)(USER_BASE+buf_off), (int)dlen);
    }
    case SYS_UDP_RECV: {
        /* ebx=my_port ecx=buf_off edx=maxlen esi=src_ip_out(user ptr or 0) */
        u16 my_port  = (u16)r->ebx;
        u32 buf_off  = r->ecx;
        u32 maxlen   = r->edx;
        u32 sip_off  = r->esi;
        if (!user_range_ok(buf_off, maxlen)) return (u32)-1;
        __asm__ volatile("sti");
        u32 ts = timer_ticks;
        while ((timer_ticks - ts) < 100) { /* 1 second timeout */
            rtl_poll();
            /* scan queue without dropping non-matching packets */
            int qi = udp_q_head;
            while (qi != udp_q_tail) {
                udp_pkt_t *pkt = &udp_q[qi];
                if (my_port == 0 || pkt->dst_port == my_port) {
                    u32 n = pkt->len; if (n > maxlen) n = maxlen;
                    memcpy((void*)(USER_BASE+buf_off), pkt->data, n);
                    if (sip_off && user_range_ok(sip_off, 4))
                        *(u32*)(USER_BASE+sip_off) = pkt->src_ip;
                    /* remove: swap with head and advance */
                    if (qi != (int)udp_q_head) {
                        udp_pkt_t tmp  = udp_q[qi];
                        udp_q[qi]      = udp_q[udp_q_head];
                        udp_q[udp_q_head] = tmp;
                    }
                    udp_q_head = (udp_q_head + 1) & (UDP_Q_MAX - 1);
                    __asm__ volatile("cli");
                    return n;
                }
                qi = (qi + 1) & (UDP_Q_MAX - 1);
            }
            __asm__ volatile("hlt");
        }
        __asm__ volatile("cli");
        return (u32)-1;
    }
    case SYS_NET_IP: {
        return NET_MY_IP;
    }
    case SYS_UDP_RECV_NB: {
        /* non-blocking: poll once, return packet or -1 immediately */
        /* ebx=my_port ecx=buf_off edx=maxlen esi=src_ip_out(or 0) */
        u16 my_port = (u16)r->ebx;
        u32 buf_off = r->ecx;
        u32 maxlen  = r->edx;
        u32 sip_off = r->esi;
        if (!user_range_ok(buf_off, maxlen)) return (u32)-1;
        rtl_poll();
        /* scan queue for matching port without blocking */
        int qi = udp_q_head;
        while (qi != udp_q_tail) {
            udp_pkt_t *pkt = &udp_q[qi];
            if (my_port == 0 || pkt->dst_port == my_port) {
                u32 n = pkt->len; if (n > maxlen) n = maxlen;
                memcpy((void*)(USER_BASE+buf_off), pkt->data, n);
                if (sip_off && user_range_ok(sip_off, 4))
                    *(u32*)(USER_BASE+sip_off) = pkt->src_ip;
                /* remove: swap with head and advance */
                if (qi != udp_q_head) {
                    udp_pkt_t tmp = udp_q[qi];
                    udp_q[qi]      = udp_q[udp_q_head];
                    udp_q[udp_q_head] = tmp;
                }
                udp_q_head = (udp_q_head + 1) & (UDP_Q_MAX - 1);
                return n;
            }
            qi = (qi + 1) & (UDP_Q_MAX - 1);
        }
        return (u32)-1;
    }
    case SYS_TICKS: {
        return (u32)timer_ticks;
    }
    case SYS_TCREATE: {
        u32 entry = r->ebx;
        u32 stack = r->ecx;
        if (!user_range_ok(entry, 1)) return (u32)-1;
        if (!user_range_ok(stack, 1)) return (u32)-1;
        return (u32)thread_create(entry, stack);
    }
    case SYS_TEXIT: {
        thread_exit_pending = 1;
        return 0;
    }
    case SYS_SLEEP: {
        u32 ms = r->ebx;
        if (ms == 0) return 0;
        u32 ticks = (ms + 9) / 10; /* 100Hz -> ~10ms per tick */
        u32 target = timer_ticks + ticks;
        while ((int)(timer_ticks - target) < 0) {
            __asm__ volatile("sti; hlt");
        }
        return 0;
    }
    case SYS_SLEEPF: {
        u32 ms = r->ebx;
        if (ms == 0) return 0;
        u32 ticks = (ms + 9) / 10;
        u32 target = timer_ticks + ticks;
        if (current_tid < 0 || current_tid >= MAX_THREADS) return 0;
        threads[current_tid].sleep_until = target;
        /* if no other runnable threads, fall back to process sleep */
        int other = 0;
        for (int i = 0; i < MAX_THREADS; i++) {
            if (i == current_tid) continue;
            if (thread_is_runnable(i)) { other = 1; break; }
        }
        if (!other) {
            while ((int)(timer_ticks - target) < 0) {
                __asm__ volatile("sti; hlt");
            }
            threads[current_tid].sleep_until = 0;
        }
        return 0;
    }
    case SYS_CLS:
        vga_cls();
        return 0;
    case SYS_SETCURSOR:
        vga_setcursor(r->ebx, r->ecx);
        return 0;
    case SYS_VMODE:
        if (r->ebx == 0) {
            if (fb_enabled) {
                /* VBE framebuffer is the active display: keep it active and
                   just clear it so the framebuffer text console takes over.
                   Switching VGA hardware registers alone cannot disable VBE. */
                vga_cls();
            } else {
                /* VGA hardware mode (e.g. mode 13): restore via registers */
                vga_set_text();
                if (text_clear_pending) {
                    vga_cls();
                    text_clear_pending = 0;
                }
            }
            text_restore_pending = 0;
            return 0;
        }
        if (r->ebx == 13) {
            fb_enabled = 0;
            vga_set_mode13();
            text_clear_pending = 1;
            text_restore_pending = 1;
            return 0;
        }
        if (gfx_mode_id != 0 && fb_enabled && r->ebx == gfx_mode_id) {
            return 0;
        }
        if (set_fb_mode((u32)r->ebx) < 0) return (u32)-1;
        return 0;
    case SYS_BLIT: {
        u32 off = r->ebx;
        if (gfx_bpp != 8 && gfx_bpp != 32) return (u32)-1;
        u8 *dst = (u8*)gfx_lfb;
        u32 w = gfx_w;
        u32 h = gfx_h;
        u32 pitch = gfx_pitch;
        u32 bytes = (gfx_bpp == 32) ? 4 : 1;
        u32 row_bytes = w * bytes;
        u32 total = h * row_bytes;
        if (!user_range_ok(off, total)) return (u32)-1;
        const u8 *buf = (const u8*)(USER_BASE + off);
        for (u32 y = 0; y < h; y++) {
            memcpy(dst + y * pitch, buf + y * row_bytes, row_bytes);
        }
        return w * h * bytes;
    }
    case SYS_PALETTE: {
        if (gfx_bpp != 8) return (u32)-1;
        u8 idx = (u8)(r->ebx & 0xFF);
        u32 rgb = r->ecx;
        u8 r8 = (u8)((rgb >> 16) & 0xFF);
        u8 g8 = (u8)((rgb >> 8) & 0xFF);
        u8 b8 = (u8)(rgb & 0xFF);
        vga_set_palette(idx, r8, g8, b8);
        return 0;
    }
    case SYS_GFXINFO: {
        gfxinfo_t gi;
        gi.w = gfx_w;
        gi.h = gfx_h;
        gi.pitch = gfx_pitch;
        gi.bpp = gfx_bpp;
        if (r->ecx < (u32)sizeof(gi)) return (u32)-1;
        if (!user_range_ok(r->ebx, sizeof(gi))) return (u32)-1;
        memcpy((void*)(USER_BASE + r->ebx), &gi, sizeof(gi));
        return (u32)sizeof(gi);
    }
    case SYS_VBEMODES: {
        int max = (int)r->ecx;
        if (max < (int)sizeof(vbe_mode_t)) return (u32)-1;
        int maxn = max / (int)sizeof(vbe_mode_t);
        if (maxn > 128) maxn = 128;
        int n = 0;
        if (vbe_list && vbe_list_count > 0) {
            n = vbe_list_count;
            if (n > maxn) n = maxn;
            for (int i = 0; i < n; i++) {
                vbe_mode_buf[i].mode = vbe_list[i].mode;
                vbe_mode_buf[i].w = vbe_list[i].w;
                vbe_mode_buf[i].h = vbe_list[i].h;
                vbe_mode_buf[i].bpp = vbe_list[i].bpp;
                vbe_mode_buf[i].pitch = vbe_list[i].pitch;
            }
        } else {
            n = 0;
        }
        if (n <= 0) return (u32)-1;
        u32 total = (u32)(n * (int)sizeof(vbe_mode_t));
        if (!user_range_ok(r->ebx, total)) return (u32)-1;
        memcpy((void*)(USER_BASE + r->ebx), vbe_mode_buf, total);
        return (u32)n;
    }
    case SYS_GFX_FBINFO: {
        u32 off = r->ebx;
        u32 max = r->ecx;
        if (max < (u32)sizeof(gfxfbinfo_t)) return (u32)-1;
        refresh_user_framebuffer_mapping();
        if (!gfx_fb_user_mapped) return (u32)-1;
        if (!user_range_ok(off, sizeof(gfxfbinfo_t))) return (u32)-1;
        gfxfbinfo_t info;
        info.w = gfx_w;
        info.h = gfx_h;
        info.pitch = gfx_pitch;
        info.bpp = gfx_bpp;
        info.size = gfx_fb_size;
        info.user_addr = gfx_fb_user_addr - USER_BASE;
        memcpy((void*)(USER_BASE + off), &info, sizeof(info));
        return (u32)sizeof(info);
    }
    case SYS_OPEN: {
        u32 name_off = r->ebx;
        int mode = (int)r->ecx;
        char uname[32];
        if (!user_copy_str(name_off, uname, sizeof(uname))) return (u32)-1;
        char kname[16];
        normalize_name(uname, kname);
        int fd = alloc_fd();
        if (fd < 0) return (u32)-1;
        open_file_t *f = &ofiles[fd];
        memset(f, 0, sizeof(*f));
        int idx = fs_find(kname);
        if (mode == 0) {
            if (idx < 0) return (u32)-1;
            f->used = 1;
            f->mode = 0;
            f->dir_index = idx;
            f->start = dir[idx].start;
            f->size = dir[idx].size;
            f->pos = 0;
            return (u32)(fd + 3);
        } else if (mode == 1) {
            int existing_idx = idx;   /* >= 0 when file already exists */
            if (idx < 0) {
                for (u32 i = 0; i < (u32)(DIR_SECTORS * 512 / sizeof(dirent_t)); i++) {
                    if (dir[i].name[0] == 0) { idx = (int)i; break; }
                }
            }
            if (idx < 0) return (u32)-1;
            /* For existing files, use fs_next_free_excl so the old sectors are
               treated as free and the new allocation never overlaps other files. */
            u32 start = (existing_idx >= 0) ? fs_next_free_excl(existing_idx) : fs_next_free();
            memset(dir[idx].name, 0, sizeof(dir[idx].name));
            u32 n = strlen(kname); if (n > 15) n = 15;
            memcpy(dir[idx].name, kname, n);
            dir[idx].start = start;
            dir[idx].size = 0;
            dir[idx].flags = 0;
            fs_sync_dir();
            f->used = 1;
            f->mode = 1;
            f->dir_index = idx;
            f->start = start;
            f->size = 0;
            f->pos = 0;
            f->sector_valid = 0;
            f->sector_dirty = 0;
            return (u32)(fd + 3);
        }
        return (u32)-1;
    }
    case SYS_FREAD: {
        int fd = (int)r->ebx;
        u32 off = r->ecx;
        u32 len = r->edx;
        if (!user_range_ok(off, len)) return (u32)-1;
        char *buf = (char*)(USER_BASE + off);
        open_file_t *f = get_ofile(fd);
        if (!f || f->mode != 0) return (u32)-1;
        if (len == 0) return 0;
        if (f->pos >= f->size) return 0;
        u32 remaining = f->size - f->pos;
        if (len > remaining) len = remaining;
        u32 done = 0;
        while (done < len) {
            u32 lba = f->start + (f->pos / 512);
            u32 sec_off = f->pos % 512;   /* offset within the 512-byte sector */
            ofile_load_sector(f, lba);
            u32 chunk = 512 - sec_off;
            if (chunk > (len - done)) chunk = len - done;
            memcpy(buf + done, f->sector + sec_off, chunk);
            f->pos += chunk;
            done += chunk;
        }
        return done;
    }
    case SYS_FWRITE: {
        int fd = (int)r->ebx;
        u32 off = r->ecx;
        u32 len = r->edx;
        if (!user_range_ok(off, len)) return (u32)-1;
        const char *buf = (const char*)(USER_BASE + off);
        open_file_t *f = get_ofile(fd);
        if (!f || f->mode != 1) return (u32)-1;
        if (len == 0) return 0;
        u32 done = 0;
        while (done < len) {
            u32 lba = f->start + (f->pos / 512);
            u32 sec_off = f->pos % 512;   /* offset within the 512-byte sector */
            ofile_load_sector(f, lba);
            u32 chunk = 512 - sec_off;
            if (chunk > (len - done)) chunk = len - done;
            memcpy(f->sector + sec_off, buf + done, chunk);
            f->sector_dirty = 1;
            f->pos += chunk;
            done += chunk;
            if (f->pos > f->size) f->size = f->pos;
            if (sec_off + chunk == 512) {
                ofile_flush(f);
                f->sector_valid = 0;
            }
        }
        return done;
    }
    case SYS_CLOSE: {
        int fd = (int)r->ebx;
        open_file_t *f = get_ofile(fd);
        if (!f) return (u32)-1;
        if (f->mode == 1) {
            ofile_flush(f);
            dir[f->dir_index].start = f->start;
            dir[f->dir_index].size = f->size;
            dir[f->dir_index].flags = 0;
            fs_sync_dir();
        }
        memset(f, 0, sizeof(*f));
        return 0;
    }
    case SYS_SEEK: {
        int fd = (int)r->ebx;
        int pos = (int)r->ecx;
        open_file_t *f = get_ofile(fd);
        if (!f) return (u32)-1;
        if (f->mode == 1) ofile_flush(f);
        if (pos < 0) pos = 0;
        if ((u32)pos > f->size) pos = (int)f->size;
        f->pos = (u32)pos;
        f->sector_valid = 0;
        f->sector_dirty = 0;
        return f->pos;
    }
    default:
        return 0;
    }
}


void after_user(void) {
    __asm__ volatile("sti");
    if (text_restore_pending) {
        vga_set_text();
        if (text_clear_pending) vga_cls();
        text_restore_pending = 0;
        text_clear_pending = 0;
    }
    exit_requested = 0;
    asm volatile(
        "mov %0, %%esp\n"
        "jmp *%1\n"
        :
        : "r"(sched_esp), "r"(sched_eip)
    );
}

static int exec_user(const char *name) {
    user_brk_off = 0;
    int size = fs_read(name, file_buf, FILE_BUF_MAX);
    if (size <= 0) return -1;
    if (size < 52) return -1;
    u8 *p = file_buf;
    if (p[0] != 0x7F || p[1] != 'E' || p[2] != 'L' || p[3] != 'F') return -1;
    u32 e_phoff = *(u32*)(p + 28);
    u16 e_phentsize = *(u16*)(p + 42);
    u16 e_phnum = *(u16*)(p + 44);
    u32 e_entry = *(u32*)(p + 24);
    /* reset user address space (never touch the FB region at USER_FB_BASE+) */
    paging_switch(proc_pd);
    u32 guard_off = USER_STACK_GUARD - USER_BASE; /* segment offset of guard page */
    memset((void*)USER_BASE, 0, guard_off);
    u32 tail_off = guard_off + PAGE_SIZE;
    if (tail_off < USER_FB_OFFSET) {
        memset((void*)(USER_BASE + tail_off), 0, USER_FB_OFFSET - tail_off);
    }
    for (u32 i = 0; i < e_phnum; i++) {
        u8 *ph = p + e_phoff + i * e_phentsize;
        u32 p_type = *(u32*)(ph + 0);
        if (p_type != 1) continue;
        u32 p_offset = *(u32*)(ph + 4);
        u32 p_vaddr = *(u32*)(ph + 8);
        u32 p_filesz = *(u32*)(ph + 16);
        u32 p_memsz = *(u32*)(ph + 20);
        memcpy((void*)(USER_BASE + p_vaddr), p + p_offset, p_filesz);
        if (p_memsz > p_filesz)
            memset((void*)(USER_BASE + p_vaddr + p_filesz), 0, p_memsz - p_filesz);
        if (p_vaddr + p_memsz > user_brk_off)
            user_brk_off = p_vaddr + p_memsz;
    }
    kill_program = 0;
    write_user_trampoline();
    thread_init_main(e_entry, USER_FB_OFFSET);
    enter_user(e_entry, USER_FB_OFFSET);
    return (int)exit_code;
}

void fault_handler(struct fault_frame *f) {
    if (f->cs == USER_CS) {
        vga_puts("USER EXC ");
        vga_hex(f->int_no);
        vga_puts(" ERR ");
        vga_hex(f->err);
        if (f->int_no == 14) {
            u32 cr2;
            __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
            vga_puts(" CR2 ");
            vga_hex(cr2);
        }
        vga_puts("\n");
        f->eip = USER_TRAMP_OFF;
        return;
    }
    vga_puts("EXC ");
    vga_hex(f->int_no);
    vga_puts(" ERR ");
    vga_hex(f->err);
    vga_puts(" EIP ");
    vga_hex(f->eip);
    vga_puts(" CS ");
    vga_hex(f->cs);
    vga_puts("\n");
    for (;;) { __asm__ volatile("cli; hlt"); }
}

void irq_handler(struct fault_frame *f) {
    if (f->int_no == 32) {
        timer_ticks++;
        if (kill_program && f->cs == USER_CS) {
            f->eip = USER_TRAMP_OFF;
            kill_program = 0;
        } else {
            thread_schedule(f);
        }
    } else if (f->int_no == 33) {
        u8 sc = inb(0x60);
        if (kbd_handle_scancode(sc)) {
            if (f->cs == USER_CS) {
                f->eip = USER_TRAMP_OFF;
                kill_program = 0;
            } else {
                exit_requested = 1;
            }
        }
    }
    /* send EOI */
    if (f->int_no >= 40) outb(0xA0, 0x20);
    outb(0x20, 0x20);
}

void kmain(void) {
    bootinfo_init();
    serial_init();
    paging_init();
    if (!fb_enabled) vga_set_text();
    vga_puts("mini-os32\n");
    if (!fb_enabled) vga_font_save();
    fs_load_dir();
    for (int i = 0; i < 32; i++) {
        idt_set_gate(i, isr_stub_table[i], KERNEL_CS, 0x8E);
    }
    for (int i = 0; i < 16; i++) {
        idt_set_gate(0x20 + i, irq_stub_table[i], KERNEL_CS, 0x8E);
    }
    idt_set_gate(0x80, (u32)syscall_entry, KERNEL_CS, 0xEE);
    lidt(idt, sizeof(idt) - 1);
    tss_init();
    pic_remap();
    pic_setmask(0xFC, 0xFF); /* enable IRQ0 and IRQ1 */
    pit_init(100);
    __asm__ volatile("sti");
    net_init();

    for (;;) {
        sched_eip = &&sched_shell_done;
        asm volatile("mov %%esp, %0" : "=r"(sched_esp));
        exec_user("shell");
sched_shell_done:
        if (next_prog_set) {
            next_prog_set = 0;
            sched_eip = &&sched_prog_done;
            asm volatile("mov %%esp, %0" : "=r"(sched_esp));
            exec_user(next_prog);
sched_prog_done:
            ;
        }
    }
}
