/* Minimal NVMe driver.
 *
 * This laptop has no ATA or AHCI controller -- NVMe is the only way to reach
 * storage, and UEFI's own block services are gone the moment we call
 * ExitBootServices.  So anything that has to survive a reboot goes through
 * here.
 *
 * Polled, single I/O queue pair, no interrupts.  A filesystem of a few hundred
 * KB does not need more, and not taking an IRQ keeps this usable from any
 * context, including the syscall path with interrupts disabled.
 */
#pragma once
#include <stdint.h>

typedef struct {
    int present;
    char model[41];              /* NUL-terminated, trailing spaces trimmed */
    char serial[21];
    char firmware[9];
    uint32_t nsid;
    uint32_t block_size;         /* bytes per logical block */
    uint64_t blocks;             /* namespace size in logical blocks */
    uint64_t bar;                /* BAR0 physical address, for reporting */
    uint16_t pci_vendor;
    uint16_t pci_device;
    uint8_t  pci_bus, pci_dev, pci_fn;
} nvme_info_t;

/* 0 on success.  Safe to call when no controller is present: it reports the
 * absence and every later call fails cleanly. */
int nvme_init(void);

/* Both return 0 on success.  `count` is in logical blocks. */
int nvme_read(uint64_t lba, uint32_t count, void *buf);
int nvme_write(uint64_t lba, uint32_t count, const void *buf);

const nvme_info_t *nvme_get_info(void);

/* Largest transfer a single call will do internally, in bytes.  Callers may
 * ask for more; it is split. */
#define NVME_CHUNK (128u * 1024u)
