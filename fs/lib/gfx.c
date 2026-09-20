#include <gfx.h>

extern int sys_gfxinfo(void *buf, int max);
extern int sys_gfx_fbinfo(void *buf, int max);

int gfx_mode(int mode) {
    return sys_vmode(mode);
}

int gfx_blit(char *buf) {
    return sys_blit(buf);
}

void gfx_set_palette(int idx, int r, int g, int b) {
    int rgb = r * 65536 + g * 256 + b;
    sys_palette(idx, rgb);
}

int gfx_info(gfx_info_t *out) {
    return sys_gfxinfo(out, sizeof(*out));
}

int gfx_fbinfo(gfx_fbinfo_t *out) {
    return sys_gfx_fbinfo(out, sizeof(*out));
}
