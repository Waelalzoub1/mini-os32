/* List PCI devices on buses 0-3 by walking config space from user space.
 * A self-check for sys_pci_read: the NVMe controller printed here must match
 * what `run storage` reports from the kernel driver.
 *
 * Build in-OS:  cc  ->  hw/lspci.c hw/hw.c  ->  lspci
 */
#include "stdio.h"
#include "hw.h"

int main(void) {
    int bus;
    int dev;
    int fn;
    int found = 0;
    puts("bus dev fn  vendor device  class\n");
    for (bus = 0; bus < 4; bus++) {
        for (dev = 0; dev < 32; dev++) {
            for (fn = 0; fn < 8; fn++) {
                int id = pci_read(bus, dev, fn, 0);
                if ((id & 0xFFFF) == 0xFFFF) {
                    if (fn == 0) break;   /* no function 0: no device */
                    continue;
                }
                int cls = pci_read(bus, dev, fn, 8);
                printf("%d   %d   %d   %x   %x   %x",
                       bus, dev, fn,
                       id & 0xFFFF, (id >> 16) & 0xFFFF,
                       (cls >> 8) & 0xFFFFFF);
                if (((cls >> 8) & 0xFFFFFF) == 0x010802) puts("  <- nvme");
                putc('\n');
                found++;
                if (fn == 0) {
                    int ht = pci_read(bus, dev, fn, 12);
                    if (!((ht >> 16) & 0x80)) break;   /* single-function */
                }
            }
        }
    }
    printf("%d devices\n", found);
    return 0;
}
