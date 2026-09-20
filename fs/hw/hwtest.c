/* hwtest -- exercise the user-space driver syscalls against the NVMe
 * controller the kernel is already driving, so every answer can be checked
 * against a known-good source.   cc hwtest.c hw.c -> hwtest
 *
 * Covers: sys_io_in/out (widths 1/2/4), sys_map_phys, sys_dma_alloc,
 * sys_irq_wait.  sys_pci_read/write were already verified by lspci.
 */
#include <stdio.h>
#include <stdint.h>
#include "hw.h"

static int failures;

static void report(char *name, int ok) {
    printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) failures++;
}

/* Must match kernel/kernel.c exactly: sys_storage refuses a smaller buffer. */
typedef struct {
    uint32_t nvme_present;
    uint32_t block_size;
    uint32_t blocks_lo, blocks_hi;
    uint32_t pci_vendor, pci_device;
    uint32_t pci_bdf;
    char model[41];
    char serial[21];
    char firmware[9];
    uint32_t store_status;
    uint32_t store_ready;
    uint32_t store_first_lo, store_first_hi;
    uint32_t store_last_lo, store_last_hi;
    uint32_t store_fs_sectors;
    uint32_t rd_sectors;
    uint32_t fs_data_lba;
} storageinfo_t;

int main(void) {
    storageinfo_t s;
    if (sys_storage(&s, sizeof(s)) < 0 || !s.nvme_present) {
        printf("no NVMe controller -- nothing to test against\n");
        return 1;
    }
    int bus = (s.pci_bdf >> 16) & 255;
    int dev = (s.pci_bdf >> 8) & 255;
    int fn = s.pci_bdf & 255;
    printf("NVMe at %x:%x.%x  id %x:%x\n\n",
           bus, dev, fn, s.pci_vendor, s.pci_device);

    /* --- sys_io_in / sys_io_out: raw 0xCF8/0xCFC vs sys_pci_read ------- */
    uint32_t id = pci_read(bus, dev, fn, 0);
    outl(0xCF8, 0x80000000u | (bus << 16) | (dev << 11) | (fn << 8));
    uint32_t raw = inl(0xCFC);
    report("io: outl/inl config read matches sys_pci_read", raw == id);
    report("io: inw low word is the vendor id",
           inw(0xCFC) == (int)(id & 0xFFFF));
    report("io: inb low byte is the vendor low byte",
           inb(0xCFC) == (int)(id & 0xFF));
    report("io: width 3 is rejected", sys_io_in(0xCFC, 3) == -1);

    /* --- sys_map_phys: map BAR0, read the NVMe CAP/VS registers -------- */
    uint32_t bar_lo = pci_read(bus, dev, fn, 0x10);
    uint32_t bar_hi = pci_read(bus, dev, fn, 0x14);
    uint32_t base_lo = bar_lo & 0xFFFFFFF0u;
    printf("\nBAR0 %x:%x\n", bar_hi, base_lo);
    int mp = sys_map_phys(base_lo, bar_hi, 0x2000);
    volatile uint32_t *regs = (volatile uint32_t *)mp;
    report("map_phys: mapping BAR0 succeeds", mp != -1);
    if (mp != -1) {
        uint32_t cap_lo = regs[0];
        uint32_t cap_hi = regs[1];
        uint32_t vs = regs[2];
        printf("  CAP %x:%x  VS %x  (NVMe %u.%u)\n",
               cap_hi, cap_lo, vs, vs >> 16, (vs >> 8) & 255);
        report("map_phys: VS says NVMe major version 1", (vs >> 16) == 1);
        report("map_phys: CAP has a nonzero queue size field",
               (cap_lo & 0xFFFF) != 0);
    }
    report("map_phys: zero size is rejected", sys_map_phys(0, 0, 0) == -1);

    /* --- sys_dma_alloc: aligned, zeroed, writable, contiguous ---------- */
    printf("\n");
    int a[2], b[2];
    int r = sys_dma_alloc(4096, a);
    report("dma: allocation succeeds", r == 0);
    if (r == 0) {
        uint32_t *p = (uint32_t *)a[0];
        report("dma: physical address is page-aligned", (a[1] & 0xFFF) == 0);
        report("dma: memory arrives zeroed", p[0] == 0 && p[1023] == 0);
        p[0] = 0x12345678;
        p[1023] = 0xCAFEF00D;
        report("dma: memory is writable and reads back",
               p[0] == 0x12345678 && p[1023] == 0xCAFEF00D);
        if (sys_dma_alloc(4096, b) == 0)
            report("dma: second allocation is contiguous",
                   b[1] == a[1] + 4096);
    }
    report("dma: zero size is rejected", sys_dma_alloc(0, a) == -1);

    /* --- sys_irq_wait: the timer fires, a quiet line times out --------- */
    int t0 = sys_ticks();
    int fired = sys_irq_wait(0, 1000);          /* IRQ0: 100Hz timer */
    int dt = sys_ticks() - t0;
    printf("\nirq0 fired %d time(s) in %d ticks\n", fired, dt);
    report("irq_wait: timer interrupt observed quickly",
           fired >= 1 && dt <= 20);

    t0 = sys_ticks();
    fired = sys_irq_wait(5, 300);               /* IRQ5: nothing there */
    dt = sys_ticks() - t0;
    printf("irq5 fired %d time(s) in %d ticks\n", fired, dt);
    report("irq_wait: quiet line times out on schedule",
           fired == 0 && dt >= 25 && dt <= 40);
    report("irq_wait: line 16 is rejected", sys_irq_wait(16, 10) == -1);

    if (failures) printf("\n%d FAILED\n", failures);
    else printf("\nall tests passed\n");
    return failures != 0;
}
