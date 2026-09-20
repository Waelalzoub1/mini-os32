#include <stdint.h>
#include <stddef.h>
#include "bootinfo.h"
#include "font8x16.h"
#include "nvme.h"

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
#ifndef DIR_SECTORS
#define DIR_SECTORS 32
#endif
#define KERNEL_LBA (1 + STAGE2_SECTORS)
#define FS_DIR_LBA (KERNEL_LBA + KERNEL_SECTORS)
#define FS_DATA_LBA (FS_DIR_LBA + DIR_SECTORS)

#define USER_BASE 0x00400000u
/* user pointers are offsets within the user segment */
#define PAGE_SIZE 4096u
/* User address space, 64MB, in four 16MB bands:
 *
 *   0 .. 24MB   real RAM: program image, heap, and the stack at the top
 *   24 .. 40MB  framebuffer window (a 1920x1200x32 panel needs 8.79MB)
 *   40 .. 56MB  device registers mapped by sys_map_phys
 *   56 .. 72MB  contiguous DMA buffers from sys_dma_alloc
 *
 * 24MB of arena because cc compiling cc.c needs ~14MB of .bss and still
 * wants a stack.
 *
 * Only the first band is backed by the physical arena; the rest are windows
 * onto memory that already exists somewhere else, so widening the space costs
 * no RAM.  The GDT user segment limit in entry.S must match USER_SPACE_SIZE. */
#define USER_SPACE_SIZE (96u * 1024u * 1024u)
#define USER_ARENA_SIZE (24u * 1024u * 1024u)
#define USER_FB_SIZE (16u * 1024u * 1024u)
#define USER_FB_OFFSET (USER_ARENA_SIZE)
#define USER_FB_BASE (USER_BASE + USER_FB_OFFSET)
#define USER_MMIO_OFFSET (USER_FB_OFFSET + USER_FB_SIZE)
#define USER_MMIO_SIZE (16u * 1024u * 1024u)
#define USER_MMIO_BASE (USER_BASE + USER_MMIO_OFFSET)
#define USER_DMA_OFFSET (USER_MMIO_OFFSET + USER_MMIO_SIZE)
#define USER_DMA_SIZE (16u * 1024u * 1024u)
#define USER_DMA_BASE (USER_BASE + USER_DMA_OFFSET)
#define USER_STACK_TOP (USER_FB_BASE)
#define USER_STACK_SIZE (1u * 1024u * 1024u)
#define USER_STACK_BOTTOM (USER_STACK_TOP - USER_STACK_SIZE)
#define USER_STACK_GUARD (USER_STACK_BOTTOM - PAGE_SIZE)
/* USER_STACK_* above are linear addresses (they include USER_BASE); user
 * EIP/ESP and page offsets are segment offsets.  Mixing the two planted the
 * kill-trampoline 4MB high -- inside the framebuffer window, where console
 * repaints overwrote it, so any fault looped forever instead of killing the
 * program -- and left the guard page mapped. */
#define USER_STACK_GUARD_OFF (USER_STACK_GUARD - USER_BASE)
#define FILE_BUF_MAX (1024*1024)
#define KERNEL_IDENTITY_LIMIT (128u * 1024u * 1024u)
#define PTE_P 0x001u
#define PTE_W 0x002u
#define PTE_U 0x004u
#define PTE_PWT 0x008u
#define PTE_PCD 0x010u
#define PTE_PAT 0x080u   /* 4KB PAE PTE: selects PAT entry 4..7 */
/* PWT|PCD with the PAT bit clear selects PAT entry 3, which is strong UC at
 * reset and stays that way -- pat_init only rewrites entries 4..7.  Device
 * registers must be UC: a cached or write-combined doorbell write may never
 * reach the controller, or reach it out of order. */
#define PTE_UC (PTE_PWT | PTE_PCD)

static int pat_wc_ready = 0;   /* set once PAT entry 4 has been made WC */


/* Kernel virtual window for the linear framebuffer.  On a UEFI machine the
 * GPU aperture is often mapped above 4GB, which no 32-bit pointer can name, so
 * the framebuffer is never identity-mapped: PAE puts its 64-bit physical
 * address behind this fixed window and every drawing path keeps using a plain
 * 32-bit pointer.  Sits above KERNEL_IDENTITY_LIMIT so it collides with
 * nothing, and stays inside the first GB so it needs no extra page directory. */
#define FB_VIRT_BASE  0x10000000u
#define FB_VIRT_SIZE  (64u * 1024u * 1024u)

/* PAE: CR3 points at a 4-entry PDPT, each entry a page directory covering 1GB,
 * each directory entry a page table covering 2MB, all entries 64 bits wide. */
#define PAE_PDPT_ENTRIES 4

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

/* 64-byte directory entry: 47-char names + NUL.  Grown from 32 bytes
 * (15-char names) on 2026-08-01 -- a store formatted before that must be
 * re-formatted with tools/mkstore.py. */
#define FS_NAME_LEN 47
typedef struct {
    char name[FS_NAME_LEN + 1];
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
 
#define VBE_RM_BUF   ((void*)0x9200) /* low memory buffer for BIOS VBE calls */

/* Set by entry.S: BOOTINFO_ADDR on the legacy path, %ebx on the UEFI path. */
extern u32 boot_info_ptr;

/* Storage backend.  Booted from BIOS we talk to an ATA disk.  Booted from UEFI
 * there may be no ATA controller at all -- this laptop is NVMe-only -- so the
 * loader hands the filesystem over in RAM and we serve sectors out of that.
 * The image starts at FS_DIR_LBA, which is the origin for the offset.  Writes
 * land in RAM and do not survive a reboot. */
static u8 *rd_base = 0;
static u32 rd_sectors = 0;

/* ACPI S5, resolved by the UEFI loader or scanned on the BIOS path. */
static u32 acpi_pm1a = 0, acpi_pm1b = 0, acpi_slp_a = 0, acpi_slp_b = 0;
static u32 ram_total_kb = 0;   /* usable RAM reported by the loader */

/* Timekeeping.  The 8254 PIT is absent or dead on some recent machines, and a
 * sleep that waits on timer_ticks would then never return -- which freezes any
 * program that paces itself, even though the shell is fine because it only
 * needs the keyboard IRQ.  If the PIT does not tick we fall back to the TSC,
 * calibrated against the ACPI power-management timer. */
static int timer_alive = 0;      /* PIT is delivering IRQ0 */
static u32 pm_tmr_port = 0;      /* ACPI PM timer I/O port, 0 if absent */
static u32 pm_tmr_32bit = 0;
static u32 tsc_khz = 0;          /* 0 if the TSC could not be calibrated */

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
static u32 gfx_lfb = 0xA0000;      /* virtual: what the drawing code writes to */
static u64 gfx_lfb_phys = 0xA0000; /* physical: may sit above 4GB under UEFI */
static int paging_on = 0;
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
static char next_prog[48];
static u32 next_prog_set = 0;
static void *sched_eip = 0;
static u32 sched_esp = 0;
static volatile u32 timer_ticks = 0;
static volatile int kill_program = 0;
static u8 *phys_free = 0;
static u64 *kernel_pdpt = 0;
static u64 *proc_pdpt = 0;
static u32 *user_pts[4];
/* State for user-space device drivers; see the syscalls near SYS_IO_IN. */
static volatile u32 irq_counts[16];
static u32 user_mmio_used = 0;    /* bytes handed out of the MMIO band */
static u32 user_dma_used = 0;     /* bytes handed out of the DMA arena */
static u32 dma_arena_base = 0;    /* physical, set up in paging_init */

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

/* Reported by SYS_MEMINFO.  Kilobytes throughout so a 32-bit field still
 * covers machines with more than 4GB. */
typedef struct {
    u32 total_kb;      /* usable RAM per firmware; 0 if unknown */
    u32 kernel_kb;     /* kernel image + page tables */
    u32 user_kb;       /* physical arena reserved for the running program */
    u32 ramdisk_kb;    /* filesystem image held in RAM */
    u32 used_kb;       /* the three above */
    u32 free_kb;       /* total - used, 0 when total is unknown */
} meminfo_t;

/* Reported by SYS_STORAGE.  64-bit values arrive as lo/hi pairs because the
 * in-OS compiler has no 64-bit integer type to receive them with. */
typedef struct {
    u32 nvme_present;
    u32 block_size;
    u32 blocks_lo, blocks_hi;    /* namespace size in logical blocks */
    u32 pci_vendor, pci_device;
    u32 pci_bdf;                 /* bus<<16 | dev<<8 | fn */
    char model[41];
    char serial[21];
    char firmware[9];
    u32 store_status;            /* STORE_* */
    u32 store_ready;             /* writes armed */
    u32 store_first_lo, store_first_hi;
    u32 store_last_lo, store_last_hi;
    u32 store_fs_sectors;        /* image sectors the header declares */
    u32 rd_sectors;              /* RAM disk size in 512B sectors */
    u32 fs_data_lba;             /* first data sector of the filesystem */
} storageinfo_t;

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

/* All four page directories are allocated up front.  A PDPT entry may only
 * carry the present bit -- bits 1 and 2 are reserved in PAE and fault on CR3
 * load -- and the CPU caches the four entries when CR3 is written, so never
 * creating one lazily also removes any need to reload CR3 to publish it. */
static void pdpt_init(u64 *pdpt) {
    for (u32 i = 0; i < PAE_PDPT_ENTRIES; i++)
        pdpt[i] = (u64)alloc_page_phys() | PTE_P;
}

static u64 *pae_pd(u64 *pdpt, u32 virt) {
    return (u64*)(u32)(pdpt[virt >> 30] & ~0xFFFULL);
}

/* `phys` is 64-bit; page tables themselves always live in kernel memory below
 * 4GB, so their own addresses still fit a pointer. */
static void map_page(u64 *pdpt, u32 virt, u64 phys, u32 flags) {
    u64 *pd = pae_pd(pdpt, virt);
    u32 pd_idx = (virt >> 21) & 0x1FF;
    u32 pt_idx = (virt >> 12) & 0x1FF;
    u64 *pt;
    if (pd[pd_idx] & PTE_P) {
        pt = (u64*)(u32)(pd[pd_idx] & ~0xFFFULL);
    } else {
        pt = (u64*)alloc_page_phys();
        u64 pde_flags = PTE_P | PTE_W;
        if (flags & PTE_U) pde_flags |= PTE_U;
        pd[pd_idx] = (u64)(u32)pt | pde_flags;
    }
    pt[pt_idx] = (phys & ~0xFFFULL) | (u64)(flags | PTE_P);
}

static void map_identity(u64 *pdpt, u32 base, u32 size, u32 flags) {
    u32 start = base & ~0xFFFu;
    u32 end = (base + size + 0xFFFu) & ~0xFFFu;
    for (u32 addr = start; addr < end; addr += PAGE_SIZE) {
        map_page(pdpt, addr, addr, flags);
    }
}

/* Map `size` bytes of physical `phys` at virtual `virt`, preserving the offset
 * of `phys` within its page. */
static void map_phys_at(u64 *pdpt, u32 virt, u64 phys, u32 size, u32 flags) {
    u32 page_off = (u32)(phys & 0xFFFu);
    u64 p = phys & ~0xFFFULL;
    u32 total = page_off + size;
    for (u32 done = 0; done < total; done += PAGE_SIZE) {
        map_page(pdpt, virt + done, p + done, flags);
    }
}

/* Map device registers into the kernel's MMIO window and return a virtual
 * address for them.  A PCI BAR can sit anywhere in the 64-bit physical space --
 * this laptop's NVMe controller is well above what a 32-bit pointer can name --
 * so, like the framebuffer, it gets a fixed window rather than an identity
 * mapping.  Strong UC: see PTE_UC. */
#define MMIO_VIRT_BASE 0x14000000u
#define MMIO_VIRT_SIZE (1u * 1024u * 1024u)
static u32 mmio_virt_next = MMIO_VIRT_BASE;

u32 kern_map_uc(u64 phys, u32 size) {
    if (!kernel_pdpt || !size) return 0;
    u32 page_off = (u32)(phys & 0xFFFu);
    u32 span = (page_off + size + 0xFFFu) & ~0xFFFu;
    if (span > MMIO_VIRT_SIZE ||
        mmio_virt_next - MMIO_VIRT_BASE > MMIO_VIRT_SIZE - span) return 0;

    u32 virt = mmio_virt_next;
    mmio_virt_next += span;
    map_phys_at(kernel_pdpt, virt, phys, size, PTE_W | PTE_UC);
    if (proc_pdpt) map_phys_at(proc_pdpt, virt, phys, size, PTE_W | PTE_UC);
    if (paging_on) {
        u32 cr3;
        __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
        __asm__ volatile("mov %0, %%cr3" : : "r"(cr3));
    }
    return virt + page_off;
}

static void reload_cr3(void) {
    u32 cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile("mov %0, %%cr3" : : "r"(cr3));
}

static void map_user_framebuffer(u64 *pdpt) {
    gfx_fb_user_mapped = 0;
    gfx_fb_size = 0;
    if (!gfx_lfb_phys) return;
    u32 bytes = gfx_pitch * gfx_h;
    if (bytes == 0 || bytes > USER_FB_SIZE) return;
    gfx_fb_size = bytes;
    gfx_fb_user_addr = USER_FB_BASE;
    gfx_fb_user_mapped = 1;
    u32 start = gfx_fb_user_addr & ~0xFFFu;
    u32 end = (gfx_fb_user_addr + bytes + 0xFFFu) & ~0xFFFu;
    u64 phys = gfx_lfb_phys - (gfx_fb_user_addr - start);
    for (u32 addr = start; addr < end; addr += PAGE_SIZE) {
        map_page(pdpt, addr, phys + (addr - start),
                 PTE_W | PTE_U | (pat_wc_ready ? PTE_PAT : 0));
    }
}

static void remap_user_framebuffer(void) {
    if (!proc_pdpt) return;
    map_user_framebuffer(proc_pdpt);
    if (!gfx_fb_user_mapped) return;
    u32 cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile("mov %0, %%cr3" : : "r"(cr3));
}

/* Mark the framebuffer write-combining.
 *
 * An MMIO aperture defaults to UC, so every pixel store becomes its own bus
 * transaction -- that is what makes the console repaint visibly, row by row,
 * like an old CRT.  WC lets the CPU coalesce stores into full cache-line
 * bursts, which is roughly two orders of magnitude faster for the streaming
 * writes a framebuffer sees.
 *
 * PAT entry 4 is retargeted to WC and selected per page by PTE bit 7.  The
 * other seven entries keep their reset values, so nothing else changes
 * behaviour.  WC still applies when the MTRRs call the region UC -- that is
 * precisely the combination the PAT exists to override. */
static void pat_init(void) {
    u32 eax, ebx, ecx, edx;
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1));
    if (!(edx & (1u << 16))) return;         /* no PAT support */

    /* reset value is 0x00070406_00070406; put WC (0x01) in entry 4 */
    u32 lo = 0x00070406u, hi = 0x00070401u;
    __asm__ volatile("wrmsr" : : "c"(0x277u), "a"(lo), "d"(hi));
    pat_wc_ready = 1;
}

/* Point the kernel's framebuffer window at `phys` and publish the virtual
 * address the drawing code uses.  Both address spaces get the mapping so a
 * mode change while a program is running stays visible in either. */
static void gfx_map_lfb(u64 phys, u32 bytes) {
    gfx_lfb_phys = phys;
    if (!phys || !bytes || bytes > FB_VIRT_SIZE) { gfx_lfb = 0; return; }
    u32 fb_flags = PTE_W | (pat_wc_ready ? PTE_PAT : 0);
    if (kernel_pdpt) map_phys_at(kernel_pdpt, FB_VIRT_BASE, phys, bytes, fb_flags);
    if (proc_pdpt)   map_phys_at(proc_pdpt,   FB_VIRT_BASE, phys, bytes, fb_flags);
    gfx_lfb = FB_VIRT_BASE + (u32)(phys & 0xFFFu);
    if (paging_on) {
        u32 cr3;
        __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
        __asm__ volatile("mov %0, %%cr3" : : "r"(cr3));
    }
}

static int set_fb_mode(u32 mode) {
    if (vbe_set_mode((u16)mode) < 0) return -1;
    fb_enabled = 1;
    gfx_mode_id = mode;
    remap_user_framebuffer();
    return gfx_fb_user_mapped ? 0 : -1;
}

static void refresh_user_framebuffer_mapping(void) {
    if (!proc_pdpt) return;
    map_user_framebuffer(proc_pdpt);
    if (!gfx_fb_user_mapped) return;
    u32 cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile("mov %0, %%cr3" : : "r"(cr3));
}

/* CR4.PAE has to be set while paging is off, which is how both loaders leave
 * us: the UEFI trampoline clears PG, LME and PAE on its way out of long mode,
 * and the legacy path never turns paging on at all. */
static void paging_enable(u64 *pdpt) {
    u32 cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1u << 5);
    __asm__ volatile("mov %0, %%cr4" : : "r"(cr4));
    __asm__ volatile("mov %0, %%cr3" : : "r"(pdpt));
    u32 cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= 0x80000000u;
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0));
    paging_on = 1;
}

static void paging_switch(u64 *pdpt) {
    __asm__ volatile("mov %0, %%cr3" : : "r"(pdpt));
}

static void map_user_space(u64 *pdpt) {
    /* Drop the identity mapping inherited from the kernel across the user
     * window.  With 2MB per directory entry the whole 16MB span sits in the
     * first page directory. */
    u64 *pd = pae_pd(pdpt, USER_BASE);
    u32 pd_start = (USER_BASE >> 21) & 0x1FF;
    u32 pd_count = USER_SPACE_SIZE >> 21;
    if ((USER_SPACE_SIZE & 0x1FFFFF) != 0) pd_count++;
    for (u32 i = 0; i < pd_count; i++) {
        pd[pd_start + i] = 0;
    }
    u32 phys = user_phys_base;
    for (u32 off = 0; off < USER_FB_OFFSET; off += PAGE_SIZE) {
        if (off == USER_STACK_GUARD_OFF) {
            phys += PAGE_SIZE;
            continue;
        }
        map_page(pdpt, USER_BASE + off, phys, PTE_W | PTE_U);
        phys += PAGE_SIZE;
    }
}

static void paging_init(void) {
    phys_free = (u8*)align_up((u32)&_kernel_end, PAGE_SIZE);
    kernel_pdpt = (u64*)alloc_page_phys();
    pdpt_init(kernel_pdpt);
    /* identity map low memory for kernel */
    map_identity(kernel_pdpt, 0, KERNEL_IDENTITY_LIMIT, PTE_W);

    proc_pdpt = (u64*)alloc_page_phys();
    /* Give the process its own page directories but let them share the
     * kernel's page tables; map_user_space then replaces just the user span. */
    for (u32 i = 0; i < PAE_PDPT_ENTRIES; i++) {
        u64 *pd = (u64*)alloc_page_phys();
        memcpy(pd, (void*)(u32)(kernel_pdpt[i] & ~0xFFFULL), PAGE_SIZE);
        proc_pdpt[i] = (u64)(u32)pd | PTE_P;
    }

    /* map the LFB into its window in both address spaces */
    if (fb_enabled && gfx_lfb_phys) {
        gfx_map_lfb(gfx_lfb_phys, gfx_pitch * gfx_h);
        if (!gfx_lfb) fb_enabled = 0;   /* too large to window: fall back to text */
    }

    user_phys_base = align_up((u32)phys_free, PAGE_SIZE);
    user_phys_limit = user_phys_base + USER_ARENA_SIZE;
    if (user_phys_limit < user_phys_base) user_phys_limit = user_phys_base;
    phys_free = (u8*)user_phys_limit;

    /* Physical pages a driver can hand to a device.  Carved out here rather
     * than allocated on demand so it is contiguous and stays below the
     * identity-map limit, which is what makes phys == the address the device
     * is programmed with. */
    dma_arena_base = align_up((u32)phys_free, PAGE_SIZE);
    if (dma_arena_base + USER_DMA_SIZE <= KERNEL_IDENTITY_LIMIT) {
        phys_free = (u8*)(dma_arena_base + USER_DMA_SIZE);
    } else {
        dma_arena_base = 0;          /* no room: sys_dma_alloc will refuse */
    }
    /* Keep later allocations reachable.  map_user_space punches the identity
     * map out of the user window, so a page table allocated at a physical
     * address inside that window would be zeroed and filled through the
     * process's translation instead of its own -- corrupting user memory and
     * leaving a garbage table behind.  Everything above the window still maps
     * one-to-one in both address spaces. */
    if ((u32)phys_free < USER_BASE + USER_SPACE_SIZE)
        phys_free = (u8*)(USER_BASE + USER_SPACE_SIZE);
    map_user_space(proc_pdpt);
    map_user_framebuffer(proc_pdpt);
    paging_enable(proc_pdpt);
}

/* Character grid mirroring the framebuffer console.
 *
 * Scrolling used to memcpy the framebuffer onto itself.  Reading back from an
 * MMIO aperture is punishingly slow -- around 9MB of uncached reads per
 * scrolled line on a 1920x1200 panel -- which is what made the display crawl
 * down the screen.  Keeping the text in RAM means a scroll is a small memmove
 * plus a write-only repaint, and the framebuffer is never read. */
#define FB_CELL_COLS 320
#define FB_CELL_ROWS 100
static u8 fb_cells[FB_CELL_ROWS * FB_CELL_COLS];

/* Scrollback.  Lines pushed off the top of the console land in this ring,
 * and Shift+PgUp / Shift+PgDn move a read-only view over them.  Any new
 * output (or clearing the screen) snaps the view back to live. */
#define HIST_ROWS 512
static u8 hist_cells[HIST_ROWS * FB_CELL_COLS];
static u32 hist_next = 0;    /* ring slot the next evicted line goes into */
static u32 hist_count = 0;   /* lines stored; saturates at HIST_ROWS */
static u32 view_off = 0;     /* lines scrolled back; 0 = live */

static void fb_cell_set(u32 row, u32 col, u8 c) {
    if (row < FB_CELL_ROWS && col < FB_CELL_COLS) fb_cells[row * FB_CELL_COLS + col] = c;
}

static u8 fb_cell_get(u32 row, u32 col) {
    if (row < FB_CELL_ROWS && col < FB_CELL_COLS) return fb_cells[row * FB_CELL_COLS + col];
    return ' ';
}

/* Paint one character cell.  Write-only: nothing here reads the framebuffer. */
static void fb_draw_glyph(u32 col, u32 row, u8 c) {
    if (!gfx_lfb) return;
    u32 px = col * 8 * fb_scale;
    u32 py = row * fb_font_h * fb_scale;
    u32 h = fb_font_h * fb_scale;
    u32 w = 8 * fb_scale;
    if (px + w > gfx_w || py + h > gfx_h) return;
    u8 *dst = (u8*)gfx_lfb;

    if (!fb_font || c == 0 || c == ' ') {
        for (u32 y = 0; y < h; y++) {
            u8 *r = dst + (py + y) * gfx_pitch + px * fb_bytes;
            if (fb_bytes == 1) memset(r, fb_bg, w);
            else for (u32 x = 0; x < w; x++) ((u32*)r)[x] = fb_bg32;
        }
        return;
    }

    const u8 *glyph = fb_font + (u32)c * fb_font_stride;
    u32 row_step = fb_font_h ? (fb_font_stride / fb_font_h) : 1;
    if (row_step == 0) row_step = 1;
    for (u32 y = 0; y < fb_font_h; y++) {
        u8 bits = glyph[y * row_step];
        for (u32 sy = 0; sy < fb_scale; sy++) {
            u8 *r = dst + (py + y * fb_scale + sy) * gfx_pitch;
            for (u32 x = 0; x < 8; x++) {
                int on = (bits & (0x80 >> x)) != 0;
                for (u32 sx = 0; sx < fb_scale; sx++) {
                    u32 off = (px + x * fb_scale + sx) * fb_bytes;
                    if (fb_bytes == 1) r[off] = on ? fb_fg : fb_bg;
                    else *(u32*)(r + off) = on ? fb_fg32 : fb_bg32;
                }
            }
        }
    }
}

static void fb_repaint(void) {
    for (u32 r = 0; r < fb_rows; r++)
        for (u32 c = 0; c < fb_cols; c++)
            fb_draw_glyph(c, r, fb_cell_get(r, c));
}

/* Repaint with the view shifted `view_off` lines into history.  The visible
 * window is anchored `view_off` lines above the live bottom: screen row r
 * shows stream line (hist_count + r - view_off), where the stream is all
 * history lines (oldest first) followed by the live rows. */
static void fb_repaint_view(void) {
    for (u32 r = 0; r < fb_rows; r++) {
        u32 idx = hist_count + r - view_off;   /* view_off <= hist_count */
        if (idx < hist_count) {
            u32 slot = (hist_next + HIST_ROWS - hist_count + idx) % HIST_ROWS;
            for (u32 c = 0; c < fb_cols; c++)
                fb_draw_glyph(c, r, hist_cells[slot * FB_CELL_COLS + c]);
        } else {
            u32 live = idx - hist_count;
            for (u32 c = 0; c < fb_cols; c++)
                fb_draw_glyph(c, r, fb_cell_get(live, c));
        }
    }
}

static void console_view(int delta) {
    if (!fb_enabled || !gfx_lfb) return;
    u32 nv = view_off;
    if (delta > 0) {
        nv += (u32)delta;
        if (nv > hist_count) nv = hist_count;
    } else {
        u32 d = (u32)(-delta);
        nv = d >= nv ? 0 : nv - d;
    }
    if (nv == view_off) return;
    view_off = nv;
    fb_repaint_view();
}

static void console_view_live(void) {
    if (!view_off) return;
    view_off = 0;
    fb_repaint();
}

static void vga_scroll(void) {
    if (fb_enabled) {
        if (cur_y < fb_rows) return;
        if (!gfx_lfb) return;   /* framebuffer not yet mapped — avoid null deref */
        /* the line about to die scrolls into history */
        memcpy(&hist_cells[hist_next * FB_CELL_COLS], &fb_cells[0], FB_CELL_COLS);
        hist_next = (hist_next + 1) % HIST_ROWS;
        if (hist_count < HIST_ROWS) hist_count++;
        u32 last = fb_rows < FB_CELL_ROWS ? fb_rows : FB_CELL_ROWS;
        for (u32 r = 1; r < last; r++)
            memcpy(&fb_cells[(r - 1) * FB_CELL_COLS], &fb_cells[r * FB_CELL_COLS], FB_CELL_COLS);
        if (last > 0) memset(&fb_cells[(last - 1) * FB_CELL_COLS], ' ', FB_CELL_COLS);
        fb_repaint();
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
        if (view_off) console_view_live();   /* new output snaps back to live */
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
            fb_cell_set(cur_y, cur_x, ' ');
            fb_draw_glyph(cur_x, cur_y, ' ');
            return;
        }
        if (!fb_font) return;
        fb_cell_set(cur_y, cur_x, (u8)c);
        fb_draw_glyph(cur_x, cur_y, (u8)c);
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
    gfx_lfb_phys = 0xA0000;
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
    gfx_lfb_phys = (u32)vga;
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
    if (!boot_info_ptr) return;
    bootinfo_t *bi = (bootinfo_t*)boot_info_ptr;
    if (bi->magic != BOOTINFO_MAGIC) return;

    /* UEFI hands the filesystem over in RAM; BIOS leaves this zero and we
     * keep using the ATA disk. */
    ram_total_kb = bi->ram_total_kb;
    pm_tmr_port = bi->pm_tmr_blk;
    pm_tmr_32bit = bi->pm_tmr_32bit;
    acpi_pm1a = bi->pm1a_cnt;
    acpi_pm1b = bi->pm1b_cnt;
    acpi_slp_a = bi->slp_typa;
    acpi_slp_b = bi->slp_typb;
    if (bi->rd_base && bi->rd_size >= DIR_SECTORS * 512) {
        rd_base = (u8*)bi->rd_base;
        rd_sectors = bi->rd_size / 512;
    }

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
    gfx_lfb_phys = ((u64)bi->lfb_high << 32) | bi->lfb;
    /* Paging is still off here, so the framebuffer is only reachable if it is
     * addressable as-is.  When it sits above 4GB there is nothing to point at
     * until paging_init maps the window, and every draw below is skipped. */
    gfx_lfb = bi->lfb_high ? 0 : bi->lfb;
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
    /* BIOS-copied font (stride may be stored in the high 16 bits).  UEFI has
     * no int 10h font service, so fall back to the built-in one. */
    if (bi->font) {
        fb_font = (u8*)bi->font;
        u32 fh = bi->font_h;
        fb_font_h = (fh & 0xFFFF) ? (fh & 0xFFFF) : 16;
        fb_font_stride = (fh >> 16);
        if (fb_font_stride == 0) fb_font_stride = fb_font_h;
    } else {
        fb_font = (u8*)font8x16;
        fb_font_h = 16;
        fb_font_stride = 16;
    }
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
    if (!gfx_lfb) return;
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
    /* remap only once the pitch is settled, so the window covers the mode */
    gfx_map_lfb(vbe_info.physBasePtr, gfx_pitch * gfx_h);
    if (!gfx_lfb) return -1;
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
        view_off = 0;   /* history survives a cls, but the view goes live */
        /* the grid must be cleared too, or the next scroll repaints stale text */
        memset(fb_cells, ' ', sizeof(fb_cells));
        if (!gfx_lfb) { cur_x = 0; cur_y = 0; return; }   /* not mapped yet */
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
        case 0x49: if (kbd_shift) console_view((int)fb_rows - 2); return 0;   /* Shift+PgUp */
        case 0x51: if (kbd_shift) console_view(-((int)fb_rows - 2)); return 0; /* Shift+PgDn */
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

/* Drain one scancode from the controller, if it has one.
 *
 * The status check and the data read must be atomic against IRQ1.  If the
 * interrupt lands between them, the handler takes the byte and the read here
 * comes back with the *previous* one -- an empty i8042 output buffer re-presents
 * its last value rather than returning nothing -- so the keystroke gets handled
 * twice and every character arrives doubled.  Returns 1 if a code was consumed,
 * -1 if it was the kill combination, 0 if there was nothing to read. */
static int kbd_drain_port(void) {
    u32 flags;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(flags));
    int r = 0;
    if (inb(0x64) & 1) {
        u8 sc = inb(0x60);
        r = kbd_handle_scancode(sc) ? -1 : 1;
    }
    if (flags & 0x200) __asm__ volatile("sti");
    return r;
}

static int kbd_getkey(void) {
    for (;;) {
        int k = kbd_pop();
        if (k) return k;
        int r = kbd_drain_port();
        if (r < 0) { exit_requested = 1; return 0; }
        if (r > 0) continue;
        __asm__ volatile("sti; hlt");
    }
}

static int kbd_getkey_nb(void) {
    int k = kbd_pop();
    if (k) return k;
    int r = kbd_drain_port();
    if (r < 0) { exit_requested = 1; return 0; }
    if (r > 0) return kbd_pop();
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

/* ---- persistent store ------------------------------------------------
 *
 * The RAM disk stays the working copy -- every read is a memcpy -- and a
 * partition on the NVMe drive backs it.  Writes go to both, so a power cut
 * loses at most the sector in flight rather than the session.
 *
 * A partition counts as ours only if its first sector carries the header
 * below, put there by tools/mkstore.py.  Discovery deliberately does not go by
 * GPT partition type: the magic is something we wrote ourselves, so a
 * partition this machine has never formatted for mini-os32 cannot be mistaken
 * for the store and written over.  Get the GPT parsing wrong and the result is
 * that no partition matches, not that the wrong one does.
 */
#define STORE_MAGIC0 0x494E494Du   /* "MINI" */
#define STORE_MAGIC1 0x3233534Fu   /* "OS32" */
#define STORE_VERSION 1

typedef struct {
    u32 magic0, magic1;
    u32 version;
    u32 fs_sectors;            /* 512-byte sectors of image following the header */
    u32 reserved[12];
} __attribute__((packed)) store_hdr_t;

static u64 store_first_lba = 0;    /* header sector, absolute on the namespace */
static u64 store_last_lba = 0;     /* inclusive, straight from the GPT entry */
static u32 store_fs_sectors = 0;
static int store_ready = 0;        /* set only when writes are permitted */
static int store_status = 0;       /* for storage.c; see STORE_* below */

#define STORE_NONE       0   /* no NVMe controller */
#define STORE_NO_GPT     1   /* controller present, no usable GPT */
#define STORE_NO_PART    2   /* GPT read, but no partition carries our magic */
#define STORE_BAD_BLOCK  3   /* logical block size is not 512 */
#define STORE_TOO_SMALL  4   /* partition cannot hold the RAM disk */
#define STORE_OK         5

static u8 store_sector[512];

/* Every write lands here first.  Two independent bounds: the offset must fall
 * inside the image the header declares, and the absolute LBA must fall inside
 * the partition the GPT described.  Nothing outside one partition is
 * reachable from this kernel. */
static int store_lba_ok(u64 lba, u32 count) {
    if (!store_ready || !count) return 0;
    if (lba <= store_first_lba) return 0;          /* never overwrite the header */
    if (lba + count - 1 > store_last_lba) return 0;
    return 1;
}

static void store_write_through(u32 lba, u32 count, const u8 *buf) {
    if (!store_ready || lba < FS_DIR_LBA) return;
    u32 off = lba - FS_DIR_LBA;
    if (off + count > store_fs_sectors) return;
    u64 target = store_first_lba + 1 + off;
    if (!store_lba_ok(target, count)) return;
    nvme_write(target, count, buf);
}

static int rd_range_ok(u32 lba, u32 count) {
    if (!rd_base || lba < FS_DIR_LBA) return 0;
    u32 off = lba - FS_DIR_LBA;
    return off + count <= rd_sectors;
}

static void disk_read_sectors(u32 lba, u32 count, u8 *buf) {
    if (rd_base) {
        if (!rd_range_ok(lba, count)) { memset(buf, 0, count * 512); return; }
        memcpy(buf, rd_base + (lba - FS_DIR_LBA) * 512, count * 512);
        return;
    }
    ata_read_sectors(lba, count, buf);
}

static void disk_write_sectors(u32 lba, u32 count, const u8 *buf) {
    if (rd_base) {
        if (!rd_range_ok(lba, count)) return;
        memcpy(rd_base + (lba - FS_DIR_LBA) * 512, buf, count * 512);
        store_write_through(lba, count, buf);
        return;
    }
    ata_write_sectors(lba, count, buf);
}

/* Single-sector forms.  Everything touching the disk must go through these,
 * never through ata_* directly: on a UEFI boot there may be no IDE controller
 * at all, and the ATA status poll would spin forever on a bus that always
 * reads back 0xFF. */
static void disk_read_sector(u32 lba, u8 *buf) { disk_read_sectors(lba, 1, buf); }
static void disk_write_sector(u32 lba, const u8 *buf) { disk_write_sectors(lba, 1, buf); }

/* Walk the GPT looking for a partition whose first sector is a store header.
 * Read-only throughout: nothing here writes, so a machine with no store just
 * ends up with store_ready clear and an unchanged disk. */
static int store_scan(void) {
    const nvme_info_t *ni = nvme_get_info();
    if (!ni->present) { store_status = STORE_NONE; return 0; }
    if (ni->block_size != 512) { store_status = STORE_BAD_BLOCK; return 0; }

    /* LBA 1 is the GPT header; LBA 0 is only the protective MBR. */
    if (nvme_read(1, 1, store_sector) != 0) { store_status = STORE_NO_GPT; return 0; }
    static const char sig[8] = { 'E','F','I',' ','P','A','R','T' };
    for (int i = 0; i < 8; i++)
        if (store_sector[i] != (u8)sig[i]) { store_status = STORE_NO_GPT; return 0; }

    u64 entry_lba = *(u64*)(store_sector + 72);
    u32 entries = *(u32*)(store_sector + 80);
    u32 entry_sz = *(u32*)(store_sector + 84);
    if (!entry_sz || entry_sz > 512 || entries > 512) { store_status = STORE_NO_GPT; return 0; }

    u32 per_sector = 512 / entry_sz;
    u8 tbl[512];
    for (u32 i = 0; i < entries; i++) {
        if (i % per_sector == 0) {
            if (nvme_read(entry_lba + i / per_sector, 1, tbl) != 0) break;
        }
        u8 *e = tbl + (i % per_sector) * entry_sz;

        /* An all-zero type GUID marks an unused slot. */
        int used = 0;
        for (int b = 0; b < 16; b++) if (e[b]) { used = 1; break; }
        if (!used) continue;

        u64 first = *(u64*)(e + 32);
        u64 last  = *(u64*)(e + 40);
        if (last <= first) continue;

        if (nvme_read(first, 1, store_sector) != 0) continue;
        store_hdr_t *h = (store_hdr_t*)store_sector;
        if (h->magic0 != STORE_MAGIC0 || h->magic1 != STORE_MAGIC1) continue;
        if (h->version != STORE_VERSION) continue;

        /* One header sector plus the image has to fit the partition. */
        u64 need = 1 + (u64)h->fs_sectors;
        if (need > last - first + 1) { store_status = STORE_TOO_SMALL; return 0; }

        store_first_lba = first;
        store_last_lba = last;
        store_fs_sectors = h->fs_sectors;
        store_status = STORE_OK;
        return 1;
    }
    store_status = STORE_NO_PART;
    return 0;
}

/* Bring the RAM disk up from the store, replacing the copy the loader took
 * from the ESP.  Only after this succeeds do writes get enabled. */
static void store_init(void) {
    if (!rd_base) return;              /* BIOS path: the ATA disk is already persistent */
    if (nvme_init() != 0) { store_status = STORE_NONE; return; }
    if (!store_scan()) return;

    u32 n = store_fs_sectors;
    if (n > rd_sectors) n = rd_sectors;
    if (nvme_read(store_first_lba + 1, n, rd_base) != 0) {
        store_status = STORE_NO_PART;  /* readable header, unreadable body */
        return;
    }
    store_ready = 1;
}

static void fs_load_dir(void) { disk_read_sectors(FS_DIR_LBA, DIR_SECTORS, (u8*)dir); }
static void fs_sync_dir(void) { disk_write_sectors(FS_DIR_LBA, DIR_SECTORS, (const u8*)dir); }

static void normalize_name(const char *in, char *out) {
    int i = 0;
    for (; i < FS_NAME_LEN && in[i]; i++) {
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
        disk_read_sectors(dir[idx].start, full, (u8*)buf);
    if (rem > 0) {
        /* Read the last partial sector into a stack bounce buffer so we
           copy only 'rem' bytes and never overwrite past the caller's buffer. */
        u8 tmp[512];
        disk_read_sector(dir[idx].start + full, tmp);
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
    disk_write_sectors(start, sectors, (const u8*)buf);
    memset(dir[idx].name, 0, sizeof(dir[idx].name));
    u32 n = strlen(name); if (n > FS_NAME_LEN) n = FS_NAME_LEN;
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
    disk_write_sector(f->cur_lba, f->sector);
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
            disk_read_sector(lba, f->sector);
        }
    } else {
        disk_read_sector(lba, f->sector);
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
/* The page the kernel drops a program onto to make it exit after a fault.
 * It sits below the stack guard page, not at the top of the stack: the stack
 * starts at USER_FB_OFFSET and grows down, so a trampoline one page below
 * that was inside the first stack page and any program with more than 4KB of
 * call depth quietly overwrote it.  A fault then jumped into stack garbage
 * and repeated forever instead of killing the program.  Below the guard page
 * a stack overflow faults first, and the trampoline is still intact to
 * handle it. */
#define USER_TRAMP_OFF (USER_STACK_GUARD_OFF - PAGE_SIZE)

typedef struct {
    int used;
    struct fault_frame ctx;
    u32 sleep_until;
} thread_t;

static thread_t threads[MAX_THREADS];
static int current_tid = 0;
static int thread_count = 0;
int thread_exit_pending = 0;


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
    /* Privileged hardware access, so a device driver can be an ordinary
     * program.  Nothing here is gated: this OS runs one program at a time
     * with no notion of a user, so a permission check would be theatre. */
    SYS_IO_IN       = 41,
    SYS_IO_OUT      = 42,
    SYS_PCI_READ    = 43,
    SYS_PCI_WRITE   = 44,
    SYS_MAP_PHYS    = 45,
    SYS_DMA_ALLOC   = 46,
    SYS_IRQ_WAIT    = 47
};

/* Working directory shared between programs.  The kernel only stores the
 * string; every filesystem syscall still takes a full path, so this changes
 * nothing for callers that ignore it.  The shell publishes its cwd here on
 * `cd` so programs it launches (cc) can resolve relative names the same way. */
static char sys_cwd[64];

/* ================================================================
 * Power off
 *
 * There is no BIOS to ask on a UEFI machine, and UEFI's own ResetSystem is
 * 64-bit code this 32-bit kernel cannot call, so ACPI S5 is what is left.
 * The UEFI loader resolves the ports and sleep types for us -- the tables sit
 * outside our identity map, so we must not walk them ourselves.  On the BIOS
 * path we scan, but only within the mapped region: an ACPI pointer above the
 * identity limit is skipped rather than dereferenced.
 * ================================================================ */

static int acpi_addr_ok(u32 a, u32 len) {
    return a && a < KERNEL_IDENTITY_LIMIT && len <= KERNEL_IDENTITY_LIMIT - a;
}

static u32 acpi_find_rsdp_bios(void) {
    u32 ebda = (u32)(*(volatile u16*)0x40E) << 4;
    for (int pass = 0; pass < 2; pass++) {
        u32 start = pass == 0 ? ebda : 0xE0000;
        u32 end   = pass == 0 ? ebda + 1024 : 0x100000;
        if (pass == 0 && (ebda < 0x400 || ebda > 0xA0000)) continue;
        for (u32 a = start; a + 8 < end; a += 16) {
            const u8 *p = (const u8*)a;
            if (p[0]=='R'&&p[1]=='S'&&p[2]=='D'&&p[3]==' '&&
                p[4]=='P'&&p[5]=='T'&&p[6]=='R'&&p[7]==' ') return a;
        }
    }
    return 0;
}

/* BIOS-path fallback.  Fills the acpi_* globals if it can do so safely. */
static void acpi_scan_bios(void) {
    u32 rsdp = acpi_find_rsdp_bios();
    if (!acpi_addr_ok(rsdp, 36)) return;
    u8 *r = (u8*)rsdp;
    u32 rsdt = *(u32*)(r + 16);
    if (!acpi_addr_ok(rsdt, 36)) return;
    u32 len = *(u32*)(rsdt + 4);
    if (len < 36 || !acpi_addr_ok(rsdt, len)) return;

    u8 *fadt = 0;
    for (u32 i = 0; i + 4 <= len - 36; i += 4) {
        u32 a = *(u32*)(rsdt + 36 + i);
        if (!acpi_addr_ok(a, 132)) continue;
        u8 *t = (u8*)a;
        if (t[0]=='F'&&t[1]=='A'&&t[2]=='C'&&t[3]=='P') { fadt = t; break; }
    }
    if (!fadt) return;

    u32 dsdt = *(u32*)(fadt + 40);
    u32 pm1a = *(u32*)(fadt + 64);
    u32 pm1b = *(u32*)(fadt + 68);
    if (!pm1a || !acpi_addr_ok(dsdt, 36)) return;
    u32 dlen = *(u32*)(dsdt + 4);
    if (dlen < 36 || !acpi_addr_ok(dsdt, dlen)) return;

    for (u8 *p = (u8*)dsdt + 36; p + 8 < (u8*)dsdt + dlen; p++) {
        if (!(p[0]=='_'&&p[1]=='S'&&p[2]=='5'&&p[3]=='_')) continue;
        u8 *q = p + 4;
        if (*q != 0x12) continue;
        q++;
        q += ((*q & 0xC0) >> 6) + 1;
        q++;
        if (*q == 0x0A) q++;
        acpi_slp_a = *q++;
        if (*q == 0x0A) q++;
        acpi_slp_b = *q;
        acpi_pm1a = pm1a;
        acpi_pm1b = pm1b;
        return;
    }
}

/* ---- timekeeping fallback ------------------------------------------- */

/* 64/32 division without libgcc: the kernel links -nodefaultlibs, so a plain
 * 64-bit divide would need __udivdi3. */
static u64 div_u64_u32(u64 n, u32 d) {
    if (!d) return 0;
    u64 q = 0;
    u32 rem = 0;
    for (int i = 63; i >= 0; i--) {
        rem = (rem << 1) | (u32)((n >> i) & 1u);
        if (rem >= d) { rem -= d; q |= (1ull << i); }
    }
    return q;
}

static u64 rdtsc(void) {
    u32 lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((u64)hi << 32) | lo;
}

static u32 pm_tmr_read(void) {
    u32 v = inl((u16)pm_tmr_port);
    return pm_tmr_32bit ? v : (v & 0x00FFFFFFu);
}

/* Calibrate the TSC against the ACPI PM timer, which runs at a fixed
 * 3.579545MHz regardless of CPU frequency scaling. */
static void tsc_calibrate(void) {
    if (!pm_tmr_port) return;
    const u32 mask = pm_tmr_32bit ? 0xFFFFFFFFu : 0x00FFFFFFu;
    const u32 want = 3579545u / 100;       /* ~10ms worth of PM ticks */

    u32 t0 = pm_tmr_read();
    u64 c0 = rdtsc();
    u32 elapsed = 0;
    u32 last = t0;
    /* bounded so a stuck PM timer cannot wedge the boot */
    for (u32 guard = 0; guard < 200000000u && elapsed < want; guard++) {
        u32 now = pm_tmr_read();
        elapsed = (now - t0) & mask;
        if (now != last) last = now;
    }
    u64 c1 = rdtsc();
    if (elapsed < want) return;            /* PM timer never moved */

    u64 dt = c1 - c0;
    /* dt cycles took elapsed/3.579545MHz seconds */
    u64 khz = div_u64_u32(dt * 3579545ull, elapsed * 1000u);
    if (khz > 100000ull && khz < 20000000ull) tsc_khz = (u32)khz;
}

/* Busy-wait, used only when the PIT is not delivering interrupts. */
static void tsc_delay_ms(u32 ms) {
    if (!tsc_khz) return;                  /* no time source: do not hang */
    u64 target = rdtsc() + (u64)ms * tsc_khz;
    while ((int64_t)(rdtsc() - target) < 0) __asm__ volatile("pause");
}

/* Is the tick source actually delivering interrupts?  Bounded by the PM timer
 * where possible: a wall-clock window is both quicker and far more predictable
 * than a spin count, which varies by orders of magnitude between real hardware
 * and emulation. */
static void timer_probe(void) {
    u32 start = timer_ticks;
    timer_alive = 0;
    if (pm_tmr_port) {
        u32 mask = pm_tmr_32bit ? 0xFFFFFFFFu : 0x00FFFFFFu;
        u32 t0 = pm_tmr_read();
        /* 50ms: five ticks at 100Hz, so plenty of margin */
        while (((pm_tmr_read() - t0) & mask) < 3579545u / 20) {
            if (timer_ticks != start) { timer_alive = 1; return; }
        }
        return;
    }
    for (u32 guard = 0; guard < 50000000u; guard++) {
        if (timer_ticks != start) { timer_alive = 1; return; }
        __asm__ volatile("pause");
    }
}

/* ---- local APIC timer ------------------------------------------------
 *
 * Lunar Lake has no working 8254, so the PIT never delivers IRQ0 and nothing
 * advances timer_ticks -- no preemption, and every sleep would have to busy
 * wait.  The local APIC is architectural and always present on such machines,
 * and its timer is per-CPU: it needs no IOAPIC and no 8259 routing.
 *
 * It is calibrated against the ACPI PM timer, the one clock here whose
 * frequency is fixed by spec (3.579545MHz) rather than by the platform. */

#define LAPIC_VECTOR   48          /* matches IRQ 48 in isr.S */
#define LAPIC_SVR      0x0F0
#define LAPIC_EOI      0x0B0
#define LAPIC_LVT_TMR  0x320
#define LAPIC_TMR_INIT 0x380
#define LAPIC_TMR_CUR  0x390
#define LAPIC_TMR_DIV  0x3E0
#define LAPIC_VIRT     0x1F000000u  /* kernel window for the LAPIC page */

extern u32 irq48;
static volatile u32 *lapic = 0;

static void lapic_write(u32 reg, u32 val) { lapic[reg / 4] = val; }
static u32  lapic_read(u32 reg) { return lapic[reg / 4]; }

/* Spin until the PM timer has advanced by `pm_ticks`. */
static void pm_tmr_wait(u32 pm_ticks) {
    u32 mask = pm_tmr_32bit ? 0xFFFFFFFFu : 0x00FFFFFFu;
    u32 t0 = pm_tmr_read();
    for (u32 guard = 0; guard < 400000000u; guard++) {
        if (((pm_tmr_read() - t0) & mask) >= pm_ticks) return;
    }
}

/* Returns 1 if a periodic 100Hz LAPIC tick is running. */
static int lapic_timer_init(void) {
    if (!pm_tmr_port) return 0;             /* no way to calibrate */

    u32 lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0x1Bu));
    u32 base = lo & 0xFFFFF000u;
    if (!base || hi) return 0;              /* must be addressable below 4GB */
    if (!(lo & (1u << 11))) {               /* globally enable the LAPIC */
        lo |= (1u << 11);
        __asm__ volatile("wrmsr" : : "c"(0x1Bu), "a"(lo), "d"(hi));
    }

    /* LAPIC registers must not be cached or write-combined */
    map_phys_at(kernel_pdpt, LAPIC_VIRT, base, PAGE_SIZE, PTE_W);
    map_phys_at(proc_pdpt,   LAPIC_VIRT, base, PAGE_SIZE, PTE_W);
    u32 cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile("mov %0, %%cr3" : : "r"(cr3));
    lapic = (volatile u32*)LAPIC_VIRT;

    lapic_write(LAPIC_SVR, lapic_read(LAPIC_SVR) | 0x100u | 0xFFu);  /* enable + spurious vector */

    /* measure how far the timer counts in 10ms */
    lapic_write(LAPIC_TMR_DIV, 0x3);                 /* divide by 16 */
    lapic_write(LAPIC_LVT_TMR, 0x10000u);            /* masked, one-shot */
    lapic_write(LAPIC_TMR_INIT, 0xFFFFFFFFu);
    pm_tmr_wait(3579545u / 100);                     /* ~10ms */
    u32 elapsed = 0xFFFFFFFFu - lapic_read(LAPIC_TMR_CUR);
    lapic_write(LAPIC_TMR_INIT, 0);
    if (elapsed < 1000) return 0;                    /* implausible: not counting */

    idt_set_gate(LAPIC_VECTOR, (u32)&irq48, KERNEL_CS, 0x8E);
    lapic_write(LAPIC_TMR_DIV, 0x3);
    lapic_write(LAPIC_LVT_TMR, LAPIC_VECTOR | (1u << 17));   /* periodic */
    lapic_write(LAPIC_TMR_INIT, elapsed);                    /* 100Hz */
    return 1;
}

static void power_off(void) {
    if (!acpi_pm1a) acpi_scan_bios();
    __asm__ volatile("cli");
    if (acpi_pm1a) {
        outw((u16)acpi_pm1a, (u16)((acpi_slp_a << 10) | (1 << 13)));  /* SLP_TYP | SLP_EN */
        if (acpi_pm1b) outw((u16)acpi_pm1b, (u16)((acpi_slp_b << 10) | (1 << 13)));
    }
    /* Emulator fallbacks, for when ACPI was unavailable. */
    outw(0x604, 0x2000);      /* QEMU */
    outw(0xB004, 0x2000);     /* Bochs and older QEMU */
    outw(0x4004, 0x3400);     /* VirtualBox */
    vga_puts("\npower off failed; halting.\n");
    for (;;) __asm__ volatile("cli; hlt");
}

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
/* ---- user-space device drivers ---------------------------------------
 *
 * A driver needs four things the C language cannot express: port I/O, a way
 * to reach a PCI BAR, memory whose physical address it knows, and a way to
 * find out an interrupt happened.  These provide exactly that and nothing
 * more -- there is no DMA teardown and no way to hand a device back, because
 * a program that has touched hardware cannot be safely un-run anyway.
 */

/* Let IRQ `n` through the 8259 masks without disturbing the others. */
static void pic_unmask(int n) {
    if (n < 0 || n > 15) return;
    if (n < 8) outb(0x21, inb(0x21) & ~(u8)(1u << n));
    else {
        outb(0xA1, inb(0xA1) & ~(u8)(1u << (n - 8)));
        outb(0x21, inb(0x21) & ~(u8)(1u << 2));   /* cascade */
    }
}

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
    case SYS_MEMINFO: {
        u32 off = r->ebx;
        u32 max = r->ecx;
        if (max < (u32)sizeof(meminfo_t)) return (u32)-1;
        if (!user_range_ok(off, sizeof(meminfo_t))) return (u32)-1;
        meminfo_t mi;
        /* Everything below phys_free has been handed out: the kernel image
         * itself, its page tables, and the user arena carved above it. */
        mi.kernel_kb  = user_phys_base ? (user_phys_base / 1024u) : ((u32)phys_free / 1024u);
        mi.user_kb    = user_phys_limit > user_phys_base
                        ? ((user_phys_limit - user_phys_base) / 1024u) : 0;
        mi.ramdisk_kb = rd_sectors / 2u;          /* 512-byte sectors -> KB */
        mi.total_kb   = ram_total_kb;
        mi.used_kb    = mi.kernel_kb + mi.user_kb + mi.ramdisk_kb;
        mi.free_kb    = (mi.total_kb > mi.used_kb) ? (mi.total_kb - mi.used_kb) : 0;
        memcpy((void*)(USER_BASE + off), &mi, sizeof(mi));
        return (u32)sizeof(mi);
    }
    case SYS_STORAGE: {
        u32 off = r->ebx;
        u32 max = r->ecx;
        if (max < (u32)sizeof(storageinfo_t)) return (u32)-1;
        if (!user_range_ok(off, sizeof(storageinfo_t))) return (u32)-1;
        const nvme_info_t *ni = nvme_get_info();
        storageinfo_t si;
        memset(&si, 0, sizeof(si));
        si.nvme_present = ni->present;
        si.block_size   = ni->block_size;
        si.blocks_lo    = (u32)ni->blocks;
        si.blocks_hi    = (u32)(ni->blocks >> 32);
        si.pci_vendor   = ni->pci_vendor;
        si.pci_device   = ni->pci_device;
        si.pci_bdf      = ((u32)ni->pci_bus << 16) | ((u32)ni->pci_dev << 8) | ni->pci_fn;
        memcpy(si.model, ni->model, sizeof(si.model));
        memcpy(si.serial, ni->serial, sizeof(si.serial));
        memcpy(si.firmware, ni->firmware, sizeof(si.firmware));
        si.store_status   = (u32)store_status;
        si.store_ready    = store_ready;
        si.store_first_lo = (u32)store_first_lba;
        si.store_first_hi = (u32)(store_first_lba >> 32);
        si.store_last_lo  = (u32)store_last_lba;
        si.store_last_hi  = (u32)(store_last_lba >> 32);
        si.store_fs_sectors = store_fs_sectors;
        si.rd_sectors     = rd_sectors;
        si.fs_data_lba    = FS_DATA_LBA;
        memcpy((void*)(USER_BASE + off), &si, sizeof(si));
        return (u32)sizeof(si);
    }
    case SYS_SYNC: {
        /* Writes are already write-through, so this only has to prove the
         * store is reachable -- and rewrite the whole image if it is not. */
        if (!store_ready) return (u32)-1;
        u32 n = store_fs_sectors < rd_sectors ? store_fs_sectors : rd_sectors;
        if (!store_lba_ok(store_first_lba + 1, n)) return (u32)-1;
        return nvme_write(store_first_lba + 1, n, rd_base) == 0 ? 0 : (u32)-1;
    }
    /* ---- hardware access for user-space drivers ---------------------- */
    case SYS_IO_IN: {
        u16 port = (u16)r->ebx;
        switch (r->ecx) {            /* width in bytes */
        case 1: return inb(port);
        case 2: return inw(port);
        case 4: return inl(port);
        }
        return (u32)-1;
    }
    case SYS_IO_OUT: {
        u16 port = (u16)r->ebx;
        switch (r->ecx) {
        case 1: outb(port, (u8)r->edx); return 0;
        case 2: outw(port, (u16)r->edx); return 0;
        case 4: outl(port, r->edx); return 0;
        }
        return (u32)-1;
    }
    case SYS_PCI_READ: {
        /* ebx packs bus<<16 | dev<<8 | fn, matching what lspci prints. */
        u32 addr = 0x80000000u | ((r->ebx & 0xFF0000u) << 0) |
                   ((r->ebx & 0xFF00u) << 3) | ((r->ebx & 0x7u) << 8) |
                   (r->ecx & 0xFCu);
        outl(0xCF8, addr);
        return inl(0xCFC);
    }
    case SYS_PCI_WRITE: {
        u32 addr = 0x80000000u | ((r->ebx & 0xFF0000u) << 0) |
                   ((r->ebx & 0xFF00u) << 3) | ((r->ebx & 0x7u) << 8) |
                   (r->ecx & 0xFCu);
        outl(0xCF8, addr);
        outl(0xCFC, r->edx);
        return 0;
    }
    case SYS_MAP_PHYS: {
        /* Map device registers into the user MMIO band, uncached.  ebx/ecx
         * are the low and high halves of the physical address: a PCI BAR can
         * sit above 4GB, which no single 32-bit argument could name. */
        u64 phys = ((u64)r->ecx << 32) | r->ebx;
        u32 size = r->edx;
        if (!size || size > USER_MMIO_SIZE) return (u32)-1;
        u32 page_off = (u32)(phys & 0xFFFu);
        u32 span = (page_off + size + 0xFFFu) & ~0xFFFu;
        if (span > USER_MMIO_SIZE - user_mmio_used) return (u32)-1;
        u32 virt = USER_MMIO_BASE + user_mmio_used;
        user_mmio_used += span;
        map_phys_at(proc_pdpt, virt, phys, size, PTE_W | PTE_U | PTE_UC);
        reload_cr3();
        return (virt - USER_BASE) + page_off;   /* user pointers are offsets */
    }
    case SYS_DMA_ALLOC: {
        /* Physically contiguous, identity-known memory for descriptor rings.
         * Handed out of a dedicated arena so it can never overlap the pages
         * the program itself is running in. */
        u32 size = (r->ebx + 0xFFFu) & ~0xFFFu;
        u32 out = r->ecx;
        if (!size || size > USER_DMA_SIZE) return (u32)-1;
        if (size > USER_DMA_SIZE - user_dma_used) return (u32)-1;
        if (!user_range_ok(out, 8)) return (u32)-1;
        u32 phys = dma_arena_base + user_dma_used;
        u32 virt = USER_DMA_BASE + user_dma_used;
        user_dma_used += size;
        for (u32 off = 0; off < size; off += PAGE_SIZE)
            map_page(proc_pdpt, virt + off, phys + off, PTE_W | PTE_U);
        reload_cr3();
        memset((void*)phys, 0, size);
        u32 *o = (u32*)(USER_BASE + out);
        o[0] = (virt - USER_BASE);   /* user pointer */
        o[1] = phys;                 /* what the device is programmed with */
        return 0;
    }
    case SYS_IRQ_WAIT: {
        /* Unmask the line, then wait for the count to move.  hlt rather than
         * a spin so the CPU is actually idle between interrupts. */
        int n = (int)r->ebx;
        u32 timeout_ms = r->ecx;
        if (n < 0 || n > 15) return (u32)-1;
        pic_unmask(n);
        u32 start = irq_counts[n];
        u32 t0 = timer_ticks;
        for (;;) {
            if (irq_counts[n] != start) return irq_counts[n] - start;
            if (timeout_ms && (timer_ticks - t0) >= timeout_ms / 10u) return 0;
            __asm__ volatile("sti; hlt");
        }
    }
    case SYS_POWEROFF: {
        power_off();          /* does not return */
        return 0;
    }
    case SYS_SETCWD: {
        char tmp[sizeof(sys_cwd)];
        if (!user_copy_str(r->ebx, tmp, sizeof(tmp))) return (u32)-1;
        memcpy(sys_cwd, tmp, sizeof(tmp));
        return 0;
    }
    case SYS_GETCWD: {
        u32 off = r->ebx;
        u32 max = r->ecx;
        if (max == 0 || !user_range_ok(off, max)) return (u32)-1;
        char *buf = (char*)(USER_BASE + off);
        u32 n = 0;
        while (sys_cwd[n] && n < max - 1) { buf[n] = sys_cwd[n]; n++; }
        buf[n] = 0;
        return n;
    }
    case SYS_LIST: {
        u32 off = r->ebx;
        u32 max = r->ecx;
        if (!user_range_ok(off, max)) return (u32)-1;
        u8 *buf = (u8*)(USER_BASE + off);
        u32 count = 0;
        for (u32 i = 0; i < (u32)(DIR_SECTORS * 512 / sizeof(dirent_t)); i++) {
            if (dir[i].name[0] == 0) continue;
            if ((count + 1) * 52 > max) break;
            memcpy(buf + count * 52, dir[i].name, 48);
            *(u32*)(buf + count * 52 + 48) = dir[i].size;
            count++;
        }
        return count;
    }
    case SYS_LOAD: {
        u32 name_off = r->ebx;
        u32 buf_off = r->ecx;
        u32 max = r->edx;
        if (!user_range_ok(buf_off, max)) return (u32)-1;
        char uname[64];
        if (!user_copy_str(name_off, uname, sizeof(uname))) return (u32)-1;
        char kname[48]; normalize_name(uname, kname);
        void *buf = (void*)(USER_BASE + buf_off);
        return (u32)fs_read(kname, buf, max);
    }
    case SYS_SAVE: {
        u32 name_off = r->ebx;
        u32 buf_off = r->ecx;
        u32 size = r->edx;
        if (!user_range_ok(buf_off, size)) return (u32)-1;
        char uname[64];
        if (!user_copy_str(name_off, uname, sizeof(uname))) return (u32)-1;
        char kname[48]; normalize_name(uname, kname);
        const void *buf = (const void*)(USER_BASE + buf_off);
        return (u32)fs_write(kname, buf, size);
    }
    case SYS_EXEC: {
        u32 name_off = r->ebx;
        char uname[64];
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
        if (inc > USER_STACK_GUARD_OFF - prev) return (u32)-1;
        user_brk_off = prev + inc;
        return prev;
    }
    case SYS_RENAME: {
        u32 old_off = r->ebx;
        u32 new_off = r->ecx;
        char oldu[64], newu[64];
        if (!user_copy_str(old_off, oldu, sizeof(oldu))) return (u32)-1;
        if (!user_copy_str(new_off, newu, sizeof(newu))) return (u32)-1;
        char kold[48], knew[48];
        normalize_name(oldu, kold);
        normalize_name(newu, knew);
        int idx = fs_find(kold);
        if (idx < 0) return (u32)-1;
        if (fs_find(knew) >= 0) return (u32)-1;
        memset(dir[idx].name, 0, sizeof(dir[idx].name));
        u32 n = strlen(knew); if (n > FS_NAME_LEN) n = FS_NAME_LEN;
        memcpy(dir[idx].name, knew, n);
        fs_sync_dir();
        return 0;
    }
    case SYS_DELETE: {
        u32 name_off = r->ebx;
        char uname[64];
        if (!user_copy_str(name_off, uname, sizeof(uname))) return (u32)-1;
        char kname[48];
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
    case SYS_TICKS: {
        /* Synthesise from the TSC when the PIT is dead, so programs that pace
         * themselves off sys_ticks() still see time move. */
        if (!timer_alive && tsc_khz)
            return (u32)div_u64_u32(rdtsc(), tsc_khz * 10u);   /* 10ms units */
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
        if (!timer_alive) { tsc_delay_ms(ms); return 0; }
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
        if (!timer_alive) { tsc_delay_ms(ms); return 0; }
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
        char uname[64];
        if (!user_copy_str(name_off, uname, sizeof(uname))) return (u32)-1;
        char kname[48];
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
    /* Windows opened by the previous program mean nothing to this one. */
    user_mmio_used = 0;
    user_dma_used = 0;
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
    paging_switch(proc_pdpt);
    u32 guard_off = USER_STACK_GUARD_OFF; /* segment offset of guard page */
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
        vga_puts(" EIP ");
        vga_hex(f->eip);
        vga_puts(" ESP ");
        vga_hex(f->useresp);
        vga_puts(" EAX ");
        vga_hex(f->eax);
        vga_puts(" EDX ");
        vga_hex(f->edx);
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
    if (f->int_no == LAPIC_VECTOR) {
        /* Same work as the PIT tick.  Acknowledged only at the LAPIC: sending
         * an EOI to the 8259 here could cut short an in-service keyboard IRQ. */
        timer_ticks++;
        if (kill_program && f->cs == USER_CS) {
            f->eip = USER_TRAMP_OFF;
            kill_program = 0;
        } else {
            thread_schedule(f);
        }
        if (lapic) lapic_write(LAPIC_EOI, 0);
        return;
    }
    if (f->int_no == 32) {
        timer_ticks++;
        if (kill_program && f->cs == USER_CS) {
            f->eip = USER_TRAMP_OFF;
            kill_program = 0;
        } else {
            thread_schedule(f);
        }
    } else if (f->int_no == 33) {
        /* Only read when the controller actually has a byte: a polling reader
         * may have taken it already, and reading an empty output buffer hands
         * back the previous scancode, doubling the keystroke.  Note this must
         * not return early -- the EOI below still has to be sent. */
        if (inb(0x64) & 1) {
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
    }
    /* Every line gets counted, so a user-space driver can wait on one it
     * asked the kernel to unmask.  Counting rather than flagging means a
     * driver can tell "no interrupt yet" from "one arrived and was missed". */
    if (f->int_no >= 32 && f->int_no < 48) irq_counts[f->int_no - 32]++;
    /* send EOI */
    if (f->int_no >= 40) outb(0xA0, 0x20);
    outb(0x20, 0x20);
}

void kmain(void) {
    bootinfo_init();
    serial_init();
    pat_init();      /* before paging_init: the FB mapping needs PAT ready */
    paging_init();
    if (!fb_enabled) vga_set_text();
    vga_puts("mini-os32\n");
    if (!fb_enabled) vga_font_save();
    store_init();    /* before fs_load_dir: it may replace the whole RAM disk */
    if (store_ready) vga_puts("store: NVMe partition attached\n");
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

    /* The PIT is missing on some recent machines; find out before anything
     * tries to sleep on it. */
    timer_probe();
    if (!timer_alive) {
        /* No PIT.  Try the local APIC timer, which restores real ticks and so
         * preemption too; fall back to a calibrated TSC busy-wait if even that
         * cannot be set up. */
        if (lapic_timer_init()) {
            pic_setmask(0xFD, 0xFF);   /* IRQ0 is dead: keep only the keyboard */
            timer_probe();
        }
        if (timer_alive) {
            vga_puts("timer: no PIT, using local APIC\n");
        } else {
            tsc_calibrate();
            vga_puts(tsc_khz ? "timer: no PIT, using TSC\n"
                             : "timer: no PIT and no TSC calibration; sleeps are no-ops\n");
        }
    }

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
