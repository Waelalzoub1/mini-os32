/* UEFI loader for mini-os32.
 *
 * Replaces boot.S + stage2.S on machines with no CSM, which is every recent
 * x86 laptop.  It does what the BIOS path used to do, using firmware services
 * instead of real-mode interrupts:
 *
 *   int 10h VBE  ->  Graphics Output Protocol
 *   int 13h      ->  Simple File System Protocol on the volume we booted from
 *   ATA at 0x1F0 ->  nothing; the filesystem is handed over as a RAM disk,
 *                    because the target machine may have no ATA controller
 *
 * Then it leaves long mode and jumps into the 32-bit kernel.  Everything the
 * kernel receives must live below 4GB, so every allocation is capped there.
 *
 * Only the handful of firmware structures actually used are declared here --
 * there is no gnu-efi on this toolchain.  Field order matters: these are
 * firmware-defined layouts, so members must stay exactly as the UEFI spec
 * lists them even where unused.
 */

#include <stdint.h>
#include "../kernel/bootinfo.h"

#define EFIAPI __attribute__((ms_abi))

typedef uint64_t UINTN;
typedef uint64_t EFI_STATUS;
typedef void *EFI_HANDLE;
typedef uint16_t CHAR16;

#define EFI_SUCCESS          0
#define EFI_BUFFER_TOO_SMALL 0x8000000000000005ULL

typedef struct { uint32_t d1; uint16_t d2, d3; uint8_t d4[8]; } EFI_GUID;

static const EFI_GUID GUID_LOADED_IMAGE =
    {0x5B1B31A1,0x9562,0x11d2,{0x8E,0x3F,0x00,0xA0,0xC9,0x69,0x72,0x3B}};
static const EFI_GUID GUID_SIMPLE_FS =
    {0x964E5B22,0x6459,0x11D2,{0x8E,0x39,0x00,0xA0,0xC9,0x69,0x72,0x3B}};
static const EFI_GUID GUID_GOP =
    {0x9042A9DE,0x23DC,0x4A38,{0x96,0xFB,0x7A,0xDE,0xD0,0x80,0x51,0x6A}};
static const EFI_GUID GUID_ACPI20 =
    {0x8868E871,0xE4F1,0x11D3,{0xBC,0x22,0x00,0x80,0xC7,0x3C,0x88,0x81}};
static const EFI_GUID GUID_ACPI10 =
    {0xEB9D2D30,0x2D88,0x11D3,{0x9A,0x16,0x00,0x90,0x27,0x3F,0xC1,0x4D}};

typedef struct { uint64_t sig; uint32_t rev, hdrsz, crc, rsvd; } EFI_TABLE_HEADER;

typedef struct EFI_SIMPLE_TEXT_OUT {
    void *Reset;
    EFI_STATUS (EFIAPI *OutputString)(struct EFI_SIMPLE_TEXT_OUT *, CHAR16 *);
    void *TestString, *QueryMode, *SetMode, *SetAttribute;
    EFI_STATUS (EFIAPI *ClearScreen)(struct EFI_SIMPLE_TEXT_OUT *);
} EFI_SIMPLE_TEXT_OUT;

typedef struct EFI_FILE {
    uint64_t Revision;
    EFI_STATUS (EFIAPI *Open)(struct EFI_FILE *, struct EFI_FILE **, CHAR16 *, uint64_t, uint64_t);
    EFI_STATUS (EFIAPI *Close)(struct EFI_FILE *);
    void *Delete;
    EFI_STATUS (EFIAPI *Read)(struct EFI_FILE *, UINTN *, void *);
    void *Write;
    EFI_STATUS (EFIAPI *GetPosition)(struct EFI_FILE *, uint64_t *);
    EFI_STATUS (EFIAPI *SetPosition)(struct EFI_FILE *, uint64_t);
    void *GetInfo, *SetInfo, *Flush;
} EFI_FILE;

typedef struct EFI_SIMPLE_FS {
    uint64_t Revision;
    EFI_STATUS (EFIAPI *OpenVolume)(struct EFI_SIMPLE_FS *, EFI_FILE **);
} EFI_SIMPLE_FS;

typedef struct {
    uint32_t Revision;
    EFI_HANDLE ParentHandle;
    void *SystemTable;
    EFI_HANDLE DeviceHandle;
} EFI_LOADED_IMAGE;

typedef struct {
    uint32_t Version;
    uint32_t HorizontalResolution;
    uint32_t VerticalResolution;
    uint32_t PixelFormat;
    uint32_t PixelInformation[4];
    uint32_t PixelsPerScanLine;
} EFI_GOP_MODE_INFO;

typedef struct {
    uint32_t MaxMode, Mode;
    EFI_GOP_MODE_INFO *Info;
    UINTN SizeOfInfo;
    uint64_t FrameBufferBase;
    UINTN FrameBufferSize;
} EFI_GOP_MODE;

typedef struct EFI_GOP {
    void *QueryMode, *SetMode, *Blt;
    EFI_GOP_MODE *Mode;
} EFI_GOP;

typedef struct {
    EFI_TABLE_HEADER Hdr;
    void *RaiseTPL, *RestoreTPL;
    EFI_STATUS (EFIAPI *AllocatePages)(uint32_t, uint32_t, UINTN, uint64_t *);
    EFI_STATUS (EFIAPI *FreePages)(uint64_t, UINTN);
    EFI_STATUS (EFIAPI *GetMemoryMap)(UINTN *, void *, UINTN *, UINTN *, uint32_t *);
    EFI_STATUS (EFIAPI *AllocatePool)(uint32_t, UINTN, void **);
    EFI_STATUS (EFIAPI *FreePool)(void *);
    void *CreateEvent, *SetTimer, *WaitForEvent, *SignalEvent, *CloseEvent, *CheckEvent;
    void *InstallProtocolInterface, *ReinstallProtocolInterface, *UninstallProtocolInterface;
    EFI_STATUS (EFIAPI *HandleProtocol)(EFI_HANDLE, const EFI_GUID *, void **);
    void *Reserved, *RegisterProtocolNotify, *LocateHandle, *LocateDevicePath;
    void *InstallConfigurationTable;
    void *LoadImage, *StartImage, *Exit, *UnloadImage;
    EFI_STATUS (EFIAPI *ExitBootServices)(EFI_HANDLE, UINTN);
    void *GetNextMonotonicCount, *Stall, *SetWatchdogTimer;
    void *ConnectController, *DisconnectController;
    void *OpenProtocol, *CloseProtocol, *OpenProtocolInformation;
    void *ProtocolsPerHandle;
    EFI_STATUS (EFIAPI *LocateHandleBuffer)(uint32_t, const EFI_GUID *, void *,
                                            UINTN *, EFI_HANDLE **);
    EFI_STATUS (EFIAPI *LocateProtocol)(const EFI_GUID *, void *, void **);
} EFI_BOOT_SERVICES;

typedef struct { EFI_GUID VendorGuid; void *VendorTable; } EFI_CONFIG_TABLE;

typedef struct {
    EFI_TABLE_HEADER Hdr;
    CHAR16 *FirmwareVendor;
    uint32_t FirmwareRevision;
    EFI_HANDLE ConsoleInHandle;
    void *ConIn;
    EFI_HANDLE ConsoleOutHandle;
    EFI_SIMPLE_TEXT_OUT *ConOut;
    EFI_HANDLE StandardErrorHandle;
    EFI_SIMPLE_TEXT_OUT *StdErr;
    void *RuntimeServices;
    EFI_BOOT_SERVICES *BootServices;
    UINTN NumberOfTableEntries;
    EFI_CONFIG_TABLE *ConfigurationTable;
} EFI_SYSTEM_TABLE;

/* One entry of the UEFI memory map.  Only the leading fields are fixed by
 * spec; entries are DescriptorSize apart, which may exceed sizeof(this). */
typedef struct {
    uint32_t Type;
    uint32_t Pad;
    uint64_t PhysicalStart;
    uint64_t VirtualStart;
    uint64_t NumberOfPages;
    uint64_t Attribute;
} EFI_MEMORY_DESCRIPTOR;

#define EfiLoaderCode          1
#define EfiLoaderData          2
#define EfiBootServicesCode    3
#define EfiBootServicesData    4
#define EfiConventionalMemory  7

/* AllocatePages types / memory types we use */
#define ALLOCATE_MAX_ADDRESS 1
#define EFI_LOADER_DATA      2

/* Must be EFIAPI: the trampoline reads its arguments from rcx/rdx/r8, so the
 * call has to use the Microsoft ABI rather than gcc's default System V. */
extern void EFIAPI tramp_boot(void *kernel_src, uint32_t size, uint32_t bootinfo);

static EFI_SYSTEM_TABLE *ST;
static EFI_BOOT_SERVICES *BS;

/* freestanding gcc still emits calls to these */
void *memset(void *d, int c, unsigned long n) {
    unsigned char *p = d;
    while (n--) *p++ = (unsigned char)c;
    return d;
}
void *memcpy(void *d, const void *s, unsigned long n) {
    unsigned char *a = d; const unsigned char *b = s;
    while (n--) *a++ = *b++;
    return d;
}

static void print(const CHAR16 *s) { ST->ConOut->OutputString(ST->ConOut, (CHAR16 *)s); }

static void print_hex(uint64_t v) {
    CHAR16 buf[19];
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 0; i < 16; i++) {
        int d = (int)((v >> ((15 - i) * 4)) & 0xF);
        buf[2 + i] = (CHAR16)(d < 10 ? '0' + d : 'a' + d - 10);
    }
    buf[18] = 0;
    print(buf);
}

static void print_dec(uint64_t v) {
    CHAR16 buf[21];
    int i = 20;
    buf[i] = 0;
    if (!v) buf[--i] = '0';
    while (v && i > 0) { buf[--i] = (CHAR16)('0' + (v % 10)); v /= 10; }
    print(&buf[i]);
}

static void die(const CHAR16 *msg, EFI_STATUS st) {
    print(u"\r\n[minios] FATAL: ");
    print(msg);
    print(u"  status=");
    print_hex(st);
    print(u"\r\nHalted.\r\n");
    for (;;) __asm__ volatile("cli; hlt");
}

/* Anything the kernel still needs after it turns paging on must land inside
 * its identity map, which covers only the first 128MB -- firmware left to
 * itself allocates near the top of RAM, far outside it.  It must also stay
 * clear of the arena the kernel carves out just above its own image (roughly
 * 3-11MB) for user space, or the kernel would allocate straight over it.
 * Requesting the highest block below 128MB satisfies both. */
#define KERNEL_MAP_LIMIT 0x08000000u   /* kernel's KERNEL_IDENTITY_LIMIT */
#define KERNEL_ARENA_END 0x01000000u   /* 16MB, safely past the kernel's arena */

#define ANYWHERE_BELOW_4G 0xFFFFFFFFu

/* Allocate page-aligned memory ending at or below `max`. */
static uint64_t alloc_low(UINTN bytes, uint64_t max) {
    uint64_t addr = max;
    UINTN pages = (bytes + 0xFFF) / 0x1000;
    EFI_STATUS st = BS->AllocatePages(ALLOCATE_MAX_ADDRESS, EFI_LOADER_DATA, pages, &addr);
    if (st != EFI_SUCCESS) die(u"AllocatePages failed", st);
    return addr;
}

/* For buffers the kernel reads once paging is live. */
static uint64_t alloc_mapped(UINTN bytes) {
    uint64_t addr = alloc_low(bytes, KERNEL_MAP_LIMIT - 1);
    if (addr < KERNEL_ARENA_END)
        die(u"no free memory between the kernel arena and 128MB", addr);
    return addr;
}

/* The RAM disk needs slack beyond the image: the filesystem allocates new
 * files past the last one in use, and a buffer sized exactly to fs.bin would
 * silently drop every write that lands past the end. */
#define RAMDISK_BYTES (8u * 1024u * 1024u)

/* Read a whole file from the volume we were loaded from.  `min_bytes` pads the
 * allocation, with the tail zeroed, for buffers that are written to later. */
static uint64_t load_file(EFI_FILE *root, const CHAR16 *path, uint32_t *size_out,
                          int must_be_mapped, UINTN min_bytes) {
    EFI_FILE *f;
    EFI_STATUS st = root->Open(root, &f, (CHAR16 *)path, 1 /* read */, 0);
    if (st != EFI_SUCCESS) { print(u"\r\n[minios] missing file: "); print(path); die(u"open failed", st); }

    /* size via seek-to-end, avoiding EFI_FILE_INFO and its GUID */
    uint64_t size = 0;
    f->SetPosition(f, 0xFFFFFFFFFFFFFFFFULL);
    f->GetPosition(f, &size);
    f->SetPosition(f, 0);
    if (size == 0 || size > 0x4000000ULL) die(u"implausible file size", size);

    UINTN alloc = (UINTN)size > min_bytes ? (UINTN)size : min_bytes;
    uint64_t buf = must_be_mapped ? alloc_mapped(alloc)
                                  : alloc_low(alloc, ANYWHERE_BELOW_4G);
    memset((void *)buf, 0, alloc);
    UINTN want = (UINTN)size;
    st = f->Read(f, &want, (void *)buf);
    if (st != EFI_SUCCESS || want != size) die(u"file read failed", st);
    f->Close(f);

    *size_out = (uint32_t)size;
    return buf;
}

static int guid_eq(const EFI_GUID *a, const EFI_GUID *b) {
    const uint8_t *x = (const uint8_t *)a, *y = (const uint8_t *)b;
    for (int i = 0; i < 16; i++) if (x[i] != y[i]) return 0;
    return 1;
}

/* The RSDP is not at a fixed place on a UEFI machine -- the legacy scan of
 * 0xE0000-0xFFFFF may find nothing -- but firmware always publishes it in the
 * configuration table.  Prefer ACPI 2.0 and fall back to 1.0. */
static uint32_t find_rsdp(void) {
    void *found = 0;
    for (UINTN i = 0; i < ST->NumberOfTableEntries; i++) {
        EFI_CONFIG_TABLE *t = &ST->ConfigurationTable[i];
        if (guid_eq(&t->VendorGuid, &GUID_ACPI20)) return (uint32_t)(uint64_t)t->VendorTable;
        if (guid_eq(&t->VendorGuid, &GUID_ACPI10)) found = t->VendorTable;
    }
    return (uint32_t)(uint64_t)found;
}

/* Resolve ACPI S5 while firmware still has every table mapped.  The sleep
 * type is not a constant -- it lives in the DSDT as a package named _S5_ --
 * so the table has to be scanned for it. */
static void acpi_resolve_s5(bootinfo_t *bi) {
    uint8_t *r = (uint8_t *)(uint64_t)find_rsdp();
    if (!r) return;

    uint8_t *sdt;
    int esz;
    if (r[15] >= 2 && *(uint64_t *)(r + 24)) {
        sdt = (uint8_t *)*(uint64_t *)(r + 24);      /* XSDT */
        esz = 8;
    } else {
        sdt = (uint8_t *)(uint64_t)*(uint32_t *)(r + 16);   /* RSDT */
        esz = 4;
    }
    if (!sdt) return;
    uint32_t len = *(uint32_t *)(sdt + 4);
    if (len < 36) return;

    uint8_t *fadt = 0;
    for (uint32_t i = 0; i + (uint32_t)esz <= len - 36; i += (uint32_t)esz) {
        uint64_t a = (esz == 4) ? (uint64_t)*(uint32_t *)(sdt + 36 + i)
                                : *(uint64_t *)(sdt + 36 + i);
        uint8_t *t = (uint8_t *)a;
        if (a && t[0]=='F' && t[1]=='A' && t[2]=='C' && t[3]=='P') { fadt = t; break; }
    }
    if (!fadt) return;

    /* PM timer: port at FADT+76, and flags bit 8 says whether it counts 32 bits */
    bi->pm_tmr_blk = *(uint32_t *)(fadt + 76);
    if (!bi->pm_tmr_blk) bi->pm_tmr_blk = (uint32_t)*(uint64_t *)(fadt + 208 + 4);
    bi->pm_tmr_32bit = (*(uint32_t *)(fadt + 112) >> 8) & 1;

    uint64_t dsdt = *(uint32_t *)(fadt + 40);
    uint32_t pm1a = *(uint32_t *)(fadt + 64);
    uint32_t pm1b = *(uint32_t *)(fadt + 68);
    if (!dsdt) dsdt = *(uint64_t *)(fadt + 140);            /* X_DSDT */
    if (!pm1a) pm1a = (uint32_t)*(uint64_t *)(fadt + 176);  /* X_PM1a_CNT_BLK address */
    if (!dsdt || !pm1a) return;

    uint8_t *d = (uint8_t *)dsdt;
    uint32_t dlen = *(uint32_t *)(d + 4);
    if (dlen < 36) return;
    for (uint8_t *p = d + 36; p + 8 < d + dlen; p++) {
        if (!(p[0]=='_' && p[1]=='S' && p[2]=='5' && p[3]=='_')) continue;
        uint8_t *q = p + 4;
        if (*q != 0x12) continue;                 /* PackageOp */
        q++;
        q += ((*q & 0xC0) >> 6) + 1;              /* PkgLength */
        q++;                                      /* NumElements */
        if (*q == 0x0A) q++;                      /* BytePrefix */
        bi->slp_typa = *q++;
        if (*q == 0x0A) q++;
        bi->slp_typb = *q;
        bi->pm1a_cnt = pm1a;
        bi->pm1b_cnt = pm1b;
        return;
    }
}

EFIAPI EFI_STATUS efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *systab) {
    ST = systab;
    BS = systab->BootServices;

    if (ST->ConOut->ClearScreen) ST->ConOut->ClearScreen(ST->ConOut);
    print(u"mini-os32 UEFI loader\r\n");

    /* --- framebuffer ------------------------------------------------- */
    /* There can be several GOP instances -- one per output -- and only some
     * may have an aperture a 32-bit kernel can reach.  Modern firmware often
     * maps the GPU BAR above 4GB, so survey them all and report what we find
     * rather than silently taking the first. */
    EFI_GOP *gop = 0;
    UINTN nh = 0;
    EFI_HANDLE *handles = 0;
    EFI_STATUS st = BS->LocateHandleBuffer(2 /* ByProtocol */, &GUID_GOP, 0, &nh, &handles);
    if (st == EFI_SUCCESS && handles) {
        print(u"  GOP handles: ");
        print_dec(nh);
        print(u"\r\n");
        for (UINTN i = 0; i < nh; i++) {
            EFI_GOP *g = 0;
            if (BS->HandleProtocol(handles[i], &GUID_GOP, (void **)&g) != EFI_SUCCESS) continue;
            if (!g || !g->Mode || !g->Mode->Info) continue;
            print(u"    fb ");
            print_hex(g->Mode->FrameBufferBase);
            print(u"  ");
            print_dec(g->Mode->Info->HorizontalResolution);
            print(u"x");
            print_dec(g->Mode->Info->VerticalResolution);
            print(u"  fmt ");
            print_dec(g->Mode->Info->PixelFormat);
            int usable = g->Mode->FrameBufferBase && g->Mode->Info->PixelFormat <= 1;
            int high = g->Mode->FrameBufferBase > 0xFFFFFFFFULL;
            print(!usable ? u"  unsupported format\r\n"
                          : high ? u"  above 4GB (PAE)\r\n" : u"  USABLE\r\n");
            /* A framebuffer above 4GB is fine -- the kernel reaches it through
             * PAE -- but prefer a low one when the firmware offers both, since
             * only a low one can carry the pre-paging handover bars. */
            if (usable && (!gop || (gop->Mode->FrameBufferBase > 0xFFFFFFFFULL && !high)))
                gop = g;
        }
    }
    if (!gop) {
        print(u"\r\n[minios] No GOP reports a usable framebuffer.\r\n");
        die(u"no framebuffer", 0);
    }

    EFI_GOP_MODE_INFO *gi = gop->Mode->Info;
    print(u"  using framebuffer ");
    print_hex(gop->Mode->FrameBufferBase);
    print(u"\r\n");

    /* --- kernel + filesystem ----------------------------------------- */
    EFI_LOADED_IMAGE *li = 0;
    st = BS->HandleProtocol(image, &GUID_LOADED_IMAGE, (void **)&li);
    if (st != EFI_SUCCESS) die(u"HandleProtocol(LoadedImage)", st);

    EFI_SIMPLE_FS *fs = 0;
    st = BS->HandleProtocol(li->DeviceHandle, &GUID_SIMPLE_FS, (void **)&fs);
    if (st != EFI_SUCCESS) die(u"boot volume has no filesystem", st);

    EFI_FILE *root = 0;
    st = fs->OpenVolume(fs, &root);
    if (st != EFI_SUCCESS) die(u"OpenVolume", st);

    uint32_t kernel_size = 0, fs_size = 0;
    uint64_t kernel_buf = load_file(root, u"\\minios\\kernel.bin", &kernel_size, 0, 0);
    uint64_t fs_buf     = load_file(root, u"\\minios\\fs.bin", &fs_size, 1, RAMDISK_BYTES);

    print(u"  kernel ");
    print_hex(kernel_size);
    print(u"  ramdisk ");
    print_hex(fs_size);
    print(u"\r\n");

    /* --- bootinfo ----------------------------------------------------- */
    uint64_t bi_addr = alloc_mapped(sizeof(bootinfo_t));
    bootinfo_t *bi = (bootinfo_t *)bi_addr;
    memset(bi, 0, sizeof(*bi));
    bi->magic  = BOOTINFO_MAGIC;
    bi->lfb      = (uint32_t)gop->Mode->FrameBufferBase;
    bi->lfb_high = (uint32_t)(gop->Mode->FrameBufferBase >> 32);
    bi->width  = gi->HorizontalResolution;
    bi->height = gi->VerticalResolution;
    bi->pitch  = gi->PixelsPerScanLine * 4;
    bi->bpp    = 32;
    bi->font   = 0;              /* no BIOS font; kernel uses its built-in one */
    bi->font_h = 0;
    bi->rd_base = (uint32_t)fs_buf;
    acpi_resolve_s5(bi);
    bi->rd_size = RAMDISK_BYTES;   /* the whole buffer is usable, not just fs.bin */

    /* --- leave firmware ----------------------------------------------- */
    print(u"  exiting boot services\r\n");

    UINTN map_size = 0, map_key = 0, desc_size = 0;
    uint32_t desc_ver = 0;
    void *map = 0;

    /* GetMemoryMap tells us the size, but allocating the buffer can itself
     * change the map, so ask for extra room and retry the exit if the key
     * went stale. */
    BS->GetMemoryMap(&map_size, 0, &map_key, &desc_size, &desc_ver);
    map_size += 8 * desc_size;
    if (BS->AllocatePool(EFI_LOADER_DATA, map_size, &map) != EFI_SUCCESS)
        die(u"AllocatePool for memory map", 0);

    st = ~0ULL;
    for (int attempt = 0; attempt < 4 && st != EFI_SUCCESS; attempt++) {
        UINTN sz = map_size;
        if (BS->GetMemoryMap(&sz, map, &map_key, &desc_size, &desc_ver) != EFI_SUCCESS)
            die(u"GetMemoryMap", 0);

        /* Total up RAM the OS may use.  Boot-services memory counts: it is
         * ours once ExitBootServices returns.  Walk by desc_size rather than
         * sizeof(descriptor) -- firmware may use a larger stride. */
        uint64_t pages = 0;
        for (UINTN off = 0; off + desc_size <= sz; off += desc_size) {
            EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)((uint8_t *)map + off);
            if (d->Type == EfiConventionalMemory ||
                d->Type == EfiBootServicesCode ||
                d->Type == EfiBootServicesData ||
                d->Type == EfiLoaderCode)
                pages += d->NumberOfPages;
        }
        bi->ram_total_kb = (uint32_t)((pages * 4096ull) / 1024ull);

        st = BS->ExitBootServices(image, map_key);
    }
    if (st != EFI_SUCCESS) die(u"ExitBootServices", st);

    /* No firmware calls beyond this point -- not even to print.  The only way
     * left to report progress is to paint the framebuffer, which matters on
     * real hardware where there is no other instrumentation:
     *   blue bar   ExitBootServices survived, about to leave long mode
     *   green bar  the trampoline made it to 32-bit protected mode
     *   neither    we died inside ExitBootServices itself
     * The kernel clears the screen when it starts, so bars that linger mark
     * exactly how far the handover got. */
    volatile uint32_t *fb = (volatile uint32_t *)gop->Mode->FrameBufferBase;
    uint32_t stride = bi->pitch / 4;
    for (uint32_t y = 0; y < 8; y++)
        for (uint32_t x = 0; x < 256; x++)
            fb[y * stride + x] = 0x000000FFu;

    tramp_boot((void *)kernel_buf, kernel_size, (uint32_t)bi_addr);

    for (;;) __asm__ volatile("cli; hlt");
    return EFI_SUCCESS;
}
