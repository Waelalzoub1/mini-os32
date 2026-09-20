/* Freestanding 32-bit loader: maps 24MB at BASE, loads "cc2h" (an OS ELF
 * relinked at BASE), then TRACEME + SIGSTOP and jumps in.  The supervising
 * runner (64-bit) emulates the OS syscalls from then on. */
typedef unsigned int u32;

static int sc(int n, int a, int b, int c) {
    int r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "b"(a), "c"(b), "d"(c) : "memory");
    return r;
}

struct mmap_arg { u32 addr, len, prot, flags, fd, off; };

#define BASE 0x20000000u
#define SIZE (24u * 1024 * 1024)

static char hdr[4096];

void _start(void) {
    struct mmap_arg ma = { BASE, SIZE, 7, 0x32, (u32)-1, 0 }; /* RWX, PRIV|ANON|FIXED */
    int r = sc(90, (int)&ma, 0, 0);                 /* old_mmap */
    if (r != (int)BASE) sc(1, 10, 0, 0);
    int fd = sc(5, (int)"cc2h", 0, 0);              /* open */
    if (fd < 0) sc(1, 11, 0, 0);
    int n = sc(3, fd, (int)hdr, 4096);              /* read */
    if (n < 96) sc(1, 12, 0, 0);
    u32 entry = *(u32*)(hdr + 24);
    u32 phoff = *(u32*)(hdr + 28);
    char *ph = hdr + phoff;
    u32 vaddr  = *(u32*)(ph + 8);
    u32 filesz = *(u32*)(ph + 16);
    char *dst = (char*)vaddr;
    for (int i = 0; i < n; i++) dst[i] = hdr[i];
    u32 got = (u32)n;
    while (got < filesz) {
        int k = sc(3, fd, (int)(dst + got), (int)(filesz - got));
        if (k <= 0) sc(1, 13, 0, 0);
        got += k;
    }
    sc(6, fd, 0, 0);                                /* close */
    sc(26, 0, 0, 0);                                /* PTRACE_TRACEME */
    int pid = sc(20, 0, 0, 0);                      /* getpid */
    sc(37, pid, 19, 0);                             /* kill(self, SIGSTOP) */
    __asm__ volatile("mov %0, %%esp\n\tjmp *%1" :: "r"(BASE + SIZE), "r"(entry));
    for (;;) {}
}
