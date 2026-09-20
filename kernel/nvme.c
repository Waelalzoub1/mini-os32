/* NVMe: PCI discovery, admin queue bring-up, one I/O queue pair, polled.
 *
 * Everything the controller touches by DMA lives in this file's .bss.  That
 * matters: the kernel image is identity-mapped and physically contiguous, so a
 * pointer to a static array is already the physical address the controller
 * needs, with no allocator and no page-table walk to get it wrong.
 */
#include "nvme.h"

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

/* Supplied by kernel.c: map `size` bytes of physical `phys` as uncached MMIO
 * and return the virtual address (0 on failure). */
extern u32 kern_map_uc(u64 phys, u32 size);

/* ---- PCI configuration space ---------------------------------------- */

static inline void outl_(u16 port, u32 v) {
    __asm__ volatile("outl %0, %1" : : "a"(v), "Nd"(port));
}
static inline u32 inl_(u16 port) {
    u32 v;
    __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static u32 pci_read32(u8 bus, u8 dev, u8 fn, u8 off) {
    u32 addr = 0x80000000u | ((u32)bus << 16) | ((u32)dev << 11) |
               ((u32)fn << 8) | (off & 0xFC);
    outl_(0xCF8, addr);
    return inl_(0xCFC);
}

static void pci_write32(u8 bus, u8 dev, u8 fn, u8 off, u32 v) {
    u32 addr = 0x80000000u | ((u32)bus << 16) | ((u32)dev << 11) |
               ((u32)fn << 8) | (off & 0xFC);
    outl_(0xCF8, addr);
    outl_(0xCFC, v);
}

/* ---- controller registers -------------------------------------------- */

#define REG_CAP     0x00
#define REG_CC      0x14
#define REG_CSTS    0x1C
#define REG_AQA     0x24
#define REG_ASQ     0x28
#define REG_ACQ     0x30

#define CC_EN       (1u << 0)
#define CSTS_RDY    (1u << 0)
#define CSTS_CFS    (1u << 1)

#define QDEPTH      64u          /* entries per queue; 64*64B fills one page */

typedef struct {
    u32 cdw0;
    u32 nsid;
    u32 rsvd2, rsvd3;
    u64 mptr;
    u64 prp1;
    u64 prp2;
    u32 cdw10, cdw11, cdw12, cdw13, cdw14, cdw15;
} __attribute__((packed)) sqe_t;

typedef struct {
    u32 dw0, dw1, dw2, dw3;
} __attribute__((packed)) cqe_t;

/* DMA memory.  Page-aligned so each queue and the PRP list start on their own
 * page, which the controller requires. */
static sqe_t admin_sq[QDEPTH] __attribute__((aligned(4096)));
static cqe_t admin_cq[QDEPTH] __attribute__((aligned(4096)));
static sqe_t io_sq[QDEPTH]    __attribute__((aligned(4096)));
static cqe_t io_cq[QDEPTH]    __attribute__((aligned(4096)));
static u8    ident_buf[4096]  __attribute__((aligned(4096)));
static u8    bounce[NVME_CHUNK] __attribute__((aligned(4096)));
static u64   prp_list[NVME_CHUNK / 4096] __attribute__((aligned(4096)));

static volatile u8 *regs = 0;
static u32 doorbell_stride = 0;
static u32 admin_sq_tail = 0, admin_cq_head = 0, admin_phase = 1;
static u32 io_sq_tail = 0, io_cq_head = 0, io_phase = 1;
static u16 next_cid = 1;
static nvme_info_t info;

static inline u32 mmio_r32(u32 off) {
    return *(volatile u32 *)(regs + off);
}
static inline void mmio_w32(u32 off, u32 v) {
    *(volatile u32 *)(regs + off) = v;
}
static inline u64 mmio_r64(u32 off) {
    /* The spec allows 32-bit halves; doing it that way also avoids relying on
     * how a 32-bit compiler splits a 64-bit MMIO access. */
    return (u64)mmio_r32(off) | ((u64)mmio_r32(off + 4) << 32);
}
static inline void mmio_w64(u32 off, u64 v) {
    mmio_w32(off, (u32)v);
    mmio_w32(off + 4, (u32)(v >> 32));
}

/* Doorbells start at 0x1000, two per queue, spaced by the stride the
 * controller reports. */
static inline void ring_sq(u32 qid, u32 value) {
    mmio_w32(0x1000 + (2 * qid) * doorbell_stride, value);
}
static inline void ring_cq(u32 qid, u32 value) {
    mmio_w32(0x1000 + (2 * qid + 1) * doorbell_stride, value);
}

static void nmemset(void *d, int v, u32 n) {
    u8 *p = (u8 *)d;
    while (n--) *p++ = (u8)v;
}
static void nmemcpy(void *d, const void *s, u32 n) {
    u8 *a = (u8 *)d;
    const u8 *b = (const u8 *)s;
    while (n--) *a++ = *b++;
}

/* Submit one command and wait for it.  Returns the 15-bit status field: 0 is
 * success, and -1 marks a timeout so a dead controller cannot wedge the boot. */
static int submit(sqe_t *sq, cqe_t *cq, u32 qid, u32 *tail, u32 *head,
                  u32 *phase, const sqe_t *cmd) {
    u16 cid = next_cid++;
    sqe_t *slot = &sq[*tail];
    nmemcpy(slot, cmd, sizeof(sqe_t));
    slot->cdw0 = (slot->cdw0 & 0x0000FFFFu) | ((u32)cid << 16);

    *tail = (*tail + 1) % QDEPTH;
    ring_sq(qid, *tail);

    /* Polled: the phase bit flips each time the controller wraps the queue,
     * which is how we tell a fresh entry from a stale one. */
    for (u32 spin = 0; spin < 200000000u; spin++) {
        volatile cqe_t *e = &cq[*head];
        u32 dw3 = e->dw3;
        if (((dw3 >> 16) & 1u) == *phase) {
            u32 status = (dw3 >> 17) & 0x7FFFu;
            *head = (*head + 1) % QDEPTH;
            if (*head == 0) *phase ^= 1u;
            ring_cq(qid, *head);
            return (int)status;
        }
        if (mmio_r32(REG_CSTS) & CSTS_CFS) return -1;   /* fatal controller error */
    }
    return -1;
}

static int admin_cmd(const sqe_t *cmd) {
    return submit(admin_sq, admin_cq, 0, &admin_sq_tail, &admin_cq_head,
                  &admin_phase, cmd);
}
static int io_cmd(const sqe_t *cmd) {
    return submit(io_sq, io_cq, 1, &io_sq_tail, &io_cq_head, &io_phase, cmd);
}

static int wait_ready(int want) {
    for (u32 spin = 0; spin < 200000000u; spin++) {
        u32 csts = mmio_r32(REG_CSTS);
        if (csts & CSTS_CFS) return -1;
        if (((csts & CSTS_RDY) ? 1 : 0) == want) return 0;
    }
    return -1;
}

/* Identify strings are space-padded and not NUL-terminated. */
static void copy_str(char *dst, const u8 *src, int n) {
    int i;
    for (i = 0; i < n; i++) dst[i] = (char)src[i];
    dst[n] = 0;
    for (i = n - 1; i >= 0 && (dst[i] == ' ' || dst[i] == 0); i--) dst[i] = 0;
}

static int find_controller(u8 *bus_o, u8 *dev_o, u8 *fn_o) {
    for (u32 bus = 0; bus < 256; bus++) {
        for (u32 dev = 0; dev < 32; dev++) {
            for (u32 fn = 0; fn < 8; fn++) {
                u32 id = pci_read32((u8)bus, (u8)dev, (u8)fn, 0x00);
                if ((id & 0xFFFF) == 0xFFFF) {
                    if (fn == 0) break;     /* no device: skip the other funcs */
                    continue;
                }
                u32 cls = pci_read32((u8)bus, (u8)dev, (u8)fn, 0x08);
                /* class 01 (mass storage), subclass 08 (NVM), prog-if 02 (NVMe) */
                if ((cls >> 8) == 0x010802u) {
                    *bus_o = (u8)bus; *dev_o = (u8)dev; *fn_o = (u8)fn;
                    info.pci_vendor = (u16)(id & 0xFFFF);
                    info.pci_device = (u16)(id >> 16);
                    return 1;
                }
                if (fn == 0) {
                    u32 hdr = pci_read32((u8)bus, (u8)dev, 0, 0x0C);
                    if (!((hdr >> 16) & 0x80)) break;   /* not multifunction */
                }
            }
        }
    }
    return 0;
}

int nvme_init(void) {
    u8 bus, dev, fn;
    nmemset(&info, 0, sizeof(info));

    if (!find_controller(&bus, &dev, &fn)) return -1;
    info.pci_bus = bus; info.pci_dev = dev; info.pci_fn = fn;

    /* BAR0 is a 64-bit memory BAR; the low 4 bits are flags. */
    u32 bar_lo = pci_read32(bus, dev, fn, 0x10);
    u32 bar_hi = ((bar_lo >> 1) & 3u) == 2u ? pci_read32(bus, dev, fn, 0x14) : 0;
    if (bar_lo & 1u) return -1;              /* I/O BAR: not an NVMe controller */
    u64 bar = ((u64)bar_hi << 32) | (bar_lo & ~0xFu);
    if (!bar) return -1;
    info.bar = bar;

    /* Memory space + bus master; firmware may have left the device disabled. */
    u32 cmd = pci_read32(bus, dev, fn, 0x04);
    pci_write32(bus, dev, fn, 0x04, cmd | (1u << 1) | (1u << 2));

    /* Registers plus the doorbell page. */
    regs = (volatile u8 *)kern_map_uc(bar, 0x2000);
    if (!regs) return -1;

    u64 cap = mmio_r64(REG_CAP);
    doorbell_stride = 4u << ((u32)(cap >> 32) & 0xFu);
    u32 mqes = (u32)(cap & 0xFFFFu) + 1u;
    if (mqes < QDEPTH) return -1;            /* controller too small for our queues */
    if (!((cap >> 37) & 1u)) return -1;      /* NVM command set unsupported */

    /* Reset before touching anything else: firmware left it enabled. */
    mmio_w32(REG_CC, 0);
    if (wait_ready(0) != 0) return -1;

    nmemset(admin_sq, 0, sizeof(admin_sq));
    nmemset(admin_cq, 0, sizeof(admin_cq));
    admin_sq_tail = admin_cq_head = 0;
    admin_phase = 1;

    mmio_w32(REG_AQA, ((QDEPTH - 1) << 16) | (QDEPTH - 1));
    mmio_w64(REG_ASQ, (u64)(u32)admin_sq);
    mmio_w64(REG_ACQ, (u64)(u32)admin_cq);

    /* MPS=0 (4KB pages), CSS=0 (NVM), IOSQES=6 (64B), IOCQES=4 (16B). */
    mmio_w32(REG_CC, (6u << 16) | (4u << 20) | CC_EN);
    if (wait_ready(1) != 0) return -1;

    sqe_t c;

    /* Identify Controller. */
    nmemset(&c, 0, sizeof(c));
    c.cdw0 = 0x06;
    c.prp1 = (u64)(u32)ident_buf;
    c.cdw10 = 1;                             /* CNS=1: controller */
    if (admin_cmd(&c) != 0) return -1;
    copy_str(info.serial, ident_buf + 4, 20);
    copy_str(info.model, ident_buf + 24, 40);
    copy_str(info.firmware, ident_buf + 64, 8);

    /* Ask for one I/O queue pair.  Allocation is advisory -- the controller
     * reports what it granted -- but every device gives at least one. */
    nmemset(&c, 0, sizeof(c));
    c.cdw0 = 0x09;                           /* Set Features */
    c.cdw10 = 0x07;                          /* Number of Queues */
    c.cdw11 = 0;                             /* 0-based: one SQ, one CQ */
    admin_cmd(&c);                           /* advisory: ignore failure */

    /* Identify Namespace 1. */
    info.nsid = 1;
    nmemset(&c, 0, sizeof(c));
    c.cdw0 = 0x06;
    c.nsid = 1;
    c.prp1 = (u64)(u32)ident_buf;
    c.cdw10 = 0;                             /* CNS=0: namespace */
    if (admin_cmd(&c) != 0) return -1;

    info.blocks = *(u64 *)(ident_buf + 0);   /* NSZE */
    u32 flbas = ident_buf[26] & 0xF;         /* index of the active LBA format */
    u32 lbaf = *(u32 *)(ident_buf + 128 + flbas * 4);
    u32 lbads = (lbaf >> 16) & 0xFF;         /* block size as a power of two */
    if (lbads < 9 || lbads > 12) return -1;  /* 512B..4KB is all we handle */
    info.block_size = 1u << lbads;
    if (!info.blocks) return -1;

    /* I/O completion queue first: the submission queue references it. */
    nmemset(io_cq, 0, sizeof(io_cq));
    nmemset(io_sq, 0, sizeof(io_sq));
    io_sq_tail = io_cq_head = 0;
    io_phase = 1;

    nmemset(&c, 0, sizeof(c));
    c.cdw0 = 0x05;                           /* Create I/O Completion Queue */
    c.prp1 = (u64)(u32)io_cq;
    c.cdw10 = ((QDEPTH - 1) << 16) | 1u;     /* size-1, qid 1 */
    c.cdw11 = 1u;                            /* physically contiguous, no IRQ */
    if (admin_cmd(&c) != 0) return -1;

    nmemset(&c, 0, sizeof(c));
    c.cdw0 = 0x01;                           /* Create I/O Submission Queue */
    c.prp1 = (u64)(u32)io_sq;
    c.cdw10 = ((QDEPTH - 1) << 16) | 1u;
    c.cdw11 = (1u << 16) | 1u;               /* CQID 1, physically contiguous */
    if (admin_cmd(&c) != 0) return -1;

    info.present = 1;
    return 0;
}

/* One command, at most NVME_CHUNK bytes, through the bounce buffer.  PRP1 is
 * the first page; anything beyond that is described by a list of page
 * addresses, which is what PRP2 points at for transfers over two pages. */
static int rw_chunk(int write, u64 lba, u32 count) {
    u32 bytes = count * info.block_size;
    u32 pages = (bytes + 4095u) / 4096u;
    sqe_t c;

    nmemset(&c, 0, sizeof(c));
    c.cdw0 = write ? 0x01u : 0x02u;
    c.nsid = info.nsid;
    c.prp1 = (u64)(u32)bounce;
    if (pages == 2) {
        c.prp2 = (u64)(u32)(bounce + 4096);
    } else if (pages > 2) {
        for (u32 i = 1; i < pages; i++)
            prp_list[i - 1] = (u64)(u32)(bounce + i * 4096u);
        c.prp2 = (u64)(u32)prp_list;
    }
    c.cdw10 = (u32)lba;
    c.cdw11 = (u32)(lba >> 32);
    c.cdw12 = count - 1;                     /* NLB is 0-based */
    return io_cmd(&c) == 0 ? 0 : -1;
}

static int rw(int write, u64 lba, u32 count, void *buf) {
    if (!info.present || !count) return -1;
    if (lba + count > info.blocks) return -1;

    u32 per_chunk = NVME_CHUNK / info.block_size;
    u8 *p = (u8 *)buf;
    while (count) {
        u32 n = count < per_chunk ? count : per_chunk;
        u32 bytes = n * info.block_size;
        if (write) nmemcpy(bounce, p, bytes);
        if (rw_chunk(write, lba, n) != 0) return -1;
        if (!write) nmemcpy(p, bounce, bytes);
        p += bytes;
        lba += n;
        count -= n;
    }
    return 0;
}

int nvme_read(u64 lba, u32 count, void *buf) { return rw(0, lba, count, buf); }
int nvme_write(u64 lba, u32 count, const void *buf) { return rw(1, lba, count, (void *)buf); }

const nvme_info_t *nvme_get_info(void) { return &info; }
