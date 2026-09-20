/* storage -- what the machine has to store things on, and whether anything
 * written here will survive a reboot.   cc storage.c -> storage
 *
 * Sizes are computed in 512-byte sectors and scaled late: cc has no 64-bit
 * integer, so a byte count for a 1TB drive would overflow long before it got
 * printed.
 */
#include <stdio.h>
#include <stdint.h>

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

/* Must match the STORE_* list in kernel/kernel.c. */
static char *status_text(uint32_t s) {
    if (s == 0) return "no NVMe controller found";
    if (s == 1) return "no GPT on the drive";
    if (s == 2) return "no partition carries a mini-os32 store header";
    if (s == 3) return "logical block size is not 512 bytes";
    if (s == 4) return "partition too small for the filesystem image";
    if (s == 5) return "attached";
    return "unknown";
}

static void pad(int printed, int width) {
    while (printed++ < width) putc(' ');
}

static int digits(uint32_t v) {
    int n = 1;
    while (v >= 10) { v = v / 10; n++; }
    return n;
}

static void label(char *s) {
    int n = 0;
    while (s[n]) { putc(s[n]); n++; }
    pad(n, 14);
}

/* Sectors -> a size string.  Divides before multiplying so a 2TB drive, which
 * is 4 billion sectors, never has to be held as bytes. */
static void print_size(uint32_t sectors) {
    uint32_t mb = sectors / 2048;
    if (mb >= 10240) {
        uint32_t gb = mb / 1024;
        uint32_t frac = ((mb % 1024) * 10) / 1024;
        printf("%u.%u GB", gb, frac);
    } else if (mb > 0) {
        printf("%u MB", mb);
    } else {
        printf("%u KB", sectors / 2);
    }
}

int main() {
    storageinfo_t s;
    if (sys_storage(&s, sizeof(s)) < 0) {
        printf("storage info unavailable\n");
        return 1;
    }

    printf("NVMe controller\n");
    if (!s.nvme_present) {
        printf("  none found -- this build has no other disk driver\n");
    } else {
        printf("  "); label("model");    printf("%s\n", s.model);
        printf("  "); label("serial");   printf("%s\n", s.serial);
        printf("  "); label("firmware"); printf("%s\n", s.firmware);
        printf("  "); label("pci");
        printf("%x:%x  %x:%x.%x\n", s.pci_vendor, s.pci_device,
               (s.pci_bdf >> 16) & 255, (s.pci_bdf >> 8) & 255, s.pci_bdf & 255);
        printf("  "); label("block size"); printf("%u bytes\n", s.block_size);
        printf("  "); label("capacity");
        if (s.blocks_hi) {
            /* Over 2^32 blocks: shift to sectors-of-2048 before it overflows. */
            printf("more than 2 TB\n");
        } else {
            print_size(s.blocks_lo);
            printf("  (%u blocks)\n", s.blocks_lo);
        }
    }

    printf("\nPersistent store\n");
    printf("  "); label("status"); printf("%s\n", status_text(s.store_status));

    if (s.store_ready) {
        uint32_t first = s.store_first_lo;
        uint32_t last = s.store_last_lo;
        printf("  "); label("partition");
        printf("LBA %u..%u  (", first, last);
        print_size(last - first + 1);
        printf(")\n");
        printf("  "); label("image");
        printf("%u sectors  (", s.store_fs_sectors);
        print_size(s.store_fs_sectors);
        printf(")\n");
        printf("  "); label("writes");
        printf("enabled, bounded to LBA %u..%u\n", first + 1, last);
    } else {
        printf("  "); label("writes");
        printf("disabled -- nothing on this disk will be modified\n");
    }

    printf("\nRAM disk\n");
    printf("  "); label("size");
    printf("%u sectors  (", s.rd_sectors);
    print_size(s.rd_sectors);
    printf(")\n");
    printf("  "); label("backing");
    printf("%s\n", s.store_ready ? "NVMe partition (write-through)"
                                 : "memory only, lost on reboot");
    return 0;
}
