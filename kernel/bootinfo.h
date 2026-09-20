/* Contract between a bootloader and the kernel.
 *
 * Two loaders produce this:
 *   - boot/stage2.S   legacy BIOS.  Writes the struct at BOOTINFO_ADDR, gets
 *                     the framebuffer from VBE and the font from int 10h, and
 *                     leaves rd_base zero -- the filesystem lives on ATA.
 *   - boot/uefi.c     UEFI.  Allocates the struct anywhere below 4GB and passes
 *                     its address in %ebx, gets the framebuffer from GOP, and
 *                     supplies the filesystem as a RAM disk because there is no
 *                     BIOS disk service and the machine may well have no ATA
 *                     controller at all.
 *
 * Everything is 32-bit: the kernel runs in 32-bit protected mode, so the UEFI
 * loader must place every buffer it hands over below 4GB.
 */
#pragma once

#define BOOTINFO_ADDR  0x5000            /* where the legacy loader puts it */
#define BOOTINFO_MAGIC 0x4F533332u       /* "OS32" */

typedef struct {
    unsigned int magic;

    /* linear framebuffer */
    unsigned int lfb;
    unsigned int width;
    unsigned int height;
    unsigned int pitch;                  /* bytes per scanline */
    unsigned int bpp;                    /* 8 or 32 */

    /* font bitmap; zero means "use the kernel's built-in font8x16" */
    unsigned int font;
    unsigned int font_h;                 /* low 16: height, high 16: stride */

    unsigned int mode;                   /* VBE mode id, 0 under UEFI */
    unsigned int vbe_list;               /* VBE mode table, 0 under UEFI */
    unsigned int vbe_count;

    /* RAM disk holding the filesystem image, starting at LBA FS_DIR_LBA.
     * Zero means "no RAM disk" and the kernel falls back to the ATA driver. */
    unsigned int rd_base;
    unsigned int rd_size;                /* bytes */

    /* Upper 32 bits of the framebuffer physical address.  Appended rather than
     * placed next to `lfb` because entry.S and uefi_tramp.S reach into this
     * struct with hand-written byte offsets, and keeping the earlier fields
     * where they are means only the new field needs an offset.
     *
     * Nonzero means the framebuffer sits above 4GB: the kernel can still reach
     * it through PAE, but any code running before paging is on -- the debug
     * bars in the two entry paths -- must not try to touch it. */
    unsigned int lfb_high;       /* byte offset 52 */

    /* Everything needed to enter ACPI S5 (power off), already resolved.
     * The tables themselves sit near the top of RAM, outside the kernel's
     * identity map, so the loader walks them while firmware still has
     * everything mapped and passes down only the result.  Zero on the BIOS
     * path, where the kernel falls back to scanning for them itself. */
    unsigned int pm1a_cnt;
    unsigned int pm1b_cnt;
    unsigned int slp_typa;
    unsigned int slp_typb;

    /* ACPI power-management timer: a fixed 3.579545MHz counter in I/O space.
     * Independent of the 8254, so it can calibrate the TSC on machines where
     * the legacy PIT is missing or dead.  Zero if unavailable. */
    unsigned int pm_tmr_blk;
    unsigned int pm_tmr_32bit;   /* 1 if the counter is 32-bit, else 24-bit */

    /* Usable RAM as reported by firmware, in KB so 4GB+ machines still fit a
     * 32-bit field.  Zero when the loader could not determine it (BIOS path). */
    unsigned int ram_total_kb;
} __attribute__((packed)) bootinfo_t;

/* Where the UEFI loader parks the kernel image, and the offset of the 32-bit
 * entry point within it.  The legacy loader enters at offset 0 in real mode;
 * entry.S reserves the first 16 bytes so the protected-mode entry sits at a
 * fixed, known offset that the UEFI loader can jump to without a symbol table. */
#define KERNEL_LOAD_ADDR 0x200000
#define KERNEL_ENTRY32   0x10
