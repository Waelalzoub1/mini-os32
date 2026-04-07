#include "libc.h"

static int rd32(const unsigned char *p) {
    return (int)p[0] + (int)p[1] * 256 + (int)p[2] * 65536 + (int)p[3] * 16777216;
}

int main() {
    unsigned char buf[128 * 20];
    int n = sys_vbemodes(buf, sizeof(buf));
    if (n < 0) {
        puts("vbe probe failed\n");
        return 1;
    }
    puts("VBE LFB modes (bpp > 8):\n");
    int best_i = -1;
    int best_area = 0;
    for (int i = 0; i < n; i++) {
        unsigned char *p = buf + i * 20;
        int mode = rd32(p + 0);
        int w = rd32(p + 4);
        int h = rd32(p + 8);
        int bpp = rd32(p + 12);
        int pitch = rd32(p + 16);
        if (bpp <= 8) continue;
        int area = w * h;
        if (area > best_area) { best_area = area; best_i = i; }
        printf("0x%x  %dx%d  bpp=%d  pitch=%d\n",
               mode, w, h, bpp, pitch);
    }
    if (best_i >= 0) {
        unsigned char *p = buf + best_i * 20;
        int mode = rd32(p + 0);
        int w = rd32(p + 4);
        int h = rd32(p + 8);
        puts("max: ");
        printf("%dx%d (0x%x)\n", w, h, mode);
    }
    return 0;
}
