// This program writes straight into the mapped framebuffer so we don't
// need to allocate another huge buffer.
#include "stdio.h"
#include "gfx.h"

static char *g_buf = 0;
static int g_w = 0;
static int g_h = 0;
static int g_stride = 0;
static int g_bytes = 1;
static int g_size = 0;

void clear_buf(void) {
    if (g_bytes == 4) {
        int *p = (int *)g_buf;
        int n = g_size / 4;
        int i = 0;
        while (i < n) { p[i] = 0; i = i + 1; }
    } else {
        int i = 0;
        while (i < g_size) { g_buf[i] = 0; i = i + 1; }
    }
}

void set_pixel(int x, int y, int color) {
    if (x < 0 || y < 0 || x >= g_w || y >= g_h) return;
    if (g_bytes == 1) {
        g_buf[y * g_stride + x] = (char)color;
    } else {
        int off = y * g_stride + x * 4;
        g_buf[off + 0] = (char)(color & 0xFF);
        g_buf[off + 1] = (char)((color >> 8) & 0xFF);
        g_buf[off + 2] = (char)((color >> 16) & 0xFF);
        g_buf[off + 3] = (char)0x00;
    }
}

void draw_rect(int x, int y, int w, int h, int color) {
    int yy = 0;
    while (yy < h) {
        int xx = 0;
        while (xx < w) {
            set_pixel(x + xx, y + yy, color);
            xx = xx + 1;
        }
        yy = yy + 1;
    }
}

int main() {
    gfx_info_t gi;
    if (gfx_info(&gi) < 0) {
        gfx_mode(GFX_MODE_TEXT);
        puts("gfx_info failed\n");
        return 0;
    }
    if (gi.bpp == 0) {
        if (gfx_mode(GFX_MODE_320x200x256) < 0) {
            gfx_mode(GFX_MODE_TEXT);
            puts("gfx_mode failed\n");
            return 0;
        }
        if (gfx_info(&gi) < 0) {
            gfx_mode(GFX_MODE_TEXT);
            puts("gfx_info failed\n");
            return 0;
        }
    }
    if (gi.bpp != 8 && gi.bpp != 32) {
        gfx_mode(GFX_MODE_TEXT);
        puts("unsupported bpp\n");
        return 0;
    }
    gfx_fbinfo_t fb;
    if (gfx_fbinfo(&fb) < 0) {
        gfx_mode(GFX_MODE_TEXT);
        puts("gfx_fbinfo failed\n");
        return 0;
    }
    g_w = fb.w;
    g_h = fb.h;
    g_bytes = (fb.bpp == 32) ? 4 : 1;
    g_stride = fb.pitch;
    g_size = fb.size;
    g_buf = (char *)fb.user_addr;
    printf("gfx_info: w=%d h=%d pitch=%d bpp=%d\n", g_w, g_h, g_stride, g_bytes * 8);
    printf("gfx_fb buffer: addr=%X size=%d pitch=%d bpp=%d\n", (int)g_buf, g_size, g_stride, g_bytes * 8);
    puts("press any key to start, q to quit\n");
    int start_key = sys_getkey();
    if (start_key == 'q') {
        gfx_mode(GFX_MODE_TEXT);
        return 0;
    }

    gfx_set_palette(0, 0, 0, 0);
    gfx_set_palette(1, 0, 120, 255);
    gfx_set_palette(2, 0, 200, 80);
    gfx_set_palette(3, 255, 80, 80);
    gfx_set_palette(4, 255, 200, 0);
    gfx_set_palette(5, 200, 0, 200);
    gfx_set_palette(6, 0, 200, 200);
    gfx_set_palette(7, 255, 255, 255);

    int x1 = g_w / 2 - 10;
    int y1 = g_h / 2 - 10;
    int w1 = 20;
    int h1 = 20;

    int x2 = g_w / 4;
    int y2 = g_h / 3;
    int w2 = 14;
    int h2 = 14;
    int vx2 = 2;
    int vy2 = 1;

    /* clear screen once before entering the loop */
    clear_buf();

    /* previous rect positions for dirty-rect erase; -1 = not yet drawn */
    int px1 = -1;
    int py1 = -1;
    int px2 = -1;
    int py2 = -1;

    int running = 1;
    while (running) {
        /* erase previous rect positions by painting them black */
        if (px1 >= 0) draw_rect(px1, py1, w1, h1, 0);
        if (px2 >= 0) draw_rect(px2, py2, w2, h2, 0);

        /* draw rects at current positions */
        if (g_bytes == 1) {
            draw_rect(x1, y1, w1, h1, 4);
            draw_rect(x2, y2, w2, h2, 2);
        } else {
            draw_rect(x1, y1, w1, h1, 0x00FFC000);
            draw_rect(x2, y2, w2, h2, 0x0000CC66);
        }

        px1 = x1; py1 = y1;
        px2 = x2; py2 = y2;

        x2 = x2 + vx2;
        y2 = y2 + vy2;

        if (x2 < 0) { x2 = 0; vx2 = 0 - vx2; }
        if (y2 < 0) { y2 = 0; vy2 = 0 - vy2; }
        if (x2 + w2 >= g_w) { x2 = g_w - w2 - 1; vx2 = 0 - vx2; }
        if (y2 + h2 >= g_h) { y2 = g_h - h2 - 1; vy2 = 0 - vy2; }

        if (sys_keystate(SC_Q)) {running = 0; gfx_mode(GFX_MODE_TEXT);clear_buf();}
        if (sys_keystate(SC_W)) y1 = y1 - 3;
        if (sys_keystate(SC_S)) y1 = y1 + 3;
        if (sys_keystate(SC_A)) x1 = x1 - 3;
        if (sys_keystate(SC_D)) x1 = x1 + 3;

        if (x1 < 0) x1 = 0;
        if (y1 < 0) y1 = 0;
        if (x1 + w1 >= g_w) x1 = g_w - w1 - 1;
        if (y1 + h1 >= g_h) y1 = g_h - h1 - 1;

        sys_sleep(1);
    }

    gfx_mode(GFX_MODE_TEXT);
    return 0;
}
