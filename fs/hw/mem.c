/* mem -- report RAM size and usage.   cc mem.c -> mem
 *
 * The filesystem lives in RAM on a UEFI boot, so "used" here really is used:
 * kernel image and page tables, the physical arena reserved for the running
 * program, and the RAM disk holding the files.
 *
 * Arithmetic stays in 32 bits (cc has no 64-bit type) and printf here has no
 * width specifiers, so columns are padded by hand.
 */
#include <stdio.h>
#include <stdint.h>

typedef struct {
    uint32_t total_kb;
    uint32_t kernel_kb;
    uint32_t user_kb;
    uint32_t ramdisk_kb;
    uint32_t used_kb;
    uint32_t free_kb;
} meminfo_t;

static void pad_to(int printed, int width) {
    while (printed++ < width) putc(' ');
}

static int digits(uint32_t v) {
    int n = 1;
    while (v >= 10) { v /= 10; n++; }
    return n;
}

static void print_label(const char *s) {
    int n = 0;
    while (s[n]) { putc(s[n]); n++; }
    pad_to(n, 9);
}

/* KB -> "N.M MB", no floating point */
static void print_mb(uint32_t kb) {
    uint32_t whole = kb / 1024;
    uint32_t frac = ((kb % 1024) * 10) / 1024;
    printf("%u.%u MB", whole, frac);
}

/* Percent to one decimal.  Both sides are scaled to MB first so the
 * multiply cannot overflow 32 bits on a large machine. */
static uint32_t pct10_of(uint32_t kb, uint32_t total_kb) {
    uint32_t t = total_kb / 1024;
    uint32_t k = kb / 1024;
    if (t == 0) return 0;
    return (k * 1000) / t;
}

static void row(const char *label, uint32_t kb, uint32_t total) {
    printf("  ");
    print_label(label);
    printf("%u KB", kb);
    pad_to(digits(kb) + 3, 12);
    printf("  ");
    print_mb(kb);
    if (total) {
        uint32_t p = pct10_of(kb, total);
        printf("  (%u.%u%%)", p / 10, p % 10);
    }
    printf("\n");
}

static void bar(uint32_t used, uint32_t total) {
    int width = 40;
    int filled = 0;
    uint32_t t = total / 1024;
    uint32_t u = used / 1024;
    if (t) filled = (int)((u * width) / t);
    if (filled > width) filled = width;
    printf("  [");
    for (int i = 0; i < width; i++) putc(i < filled ? '#' : '.');
    printf("]\n");
}

int main() {
    meminfo_t m;
    if (sys_meminfo(&m, sizeof(m)) < 0) {
        printf("meminfo unavailable\n");
        return 1;
    }

    printf("RAM\n");
    if (m.total_kb) {
        row("total", m.total_kb, 0);
    } else {
        printf("  total    unknown (firmware did not report it)\n");
    }

    row("kernel", m.kernel_kb, m.total_kb);
    row("user", m.user_kb, m.total_kb);
    row("ramdisk", m.ramdisk_kb, m.total_kb);
    row("used", m.used_kb, m.total_kb);

    if (m.total_kb) {
        row("free", m.free_kb, m.total_kb);
        bar(m.used_kb, m.total_kb);
    }
    return 0;
}
