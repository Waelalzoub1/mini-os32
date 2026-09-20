#ifndef GFX_H
#define GFX_H

#define GFX_MODE_TEXT 0
#define GFX_MODE_320x200x256 13
#define GFX_MODE_640x480x256 0x101
#define GFX_MODE_1440x900x32 0x180

typedef struct {
    int w;
    int h;
    int pitch;
    int bpp;
} gfx_info_t;

typedef struct {
    int w;
    int h;
    int pitch;
    int bpp;
    int size;
    int user_addr;
} gfx_fbinfo_t;

int gfx_mode(int mode);
int gfx_blit(char *buf);
void gfx_set_palette(int idx, int r, int g, int b);
int gfx_info(gfx_info_t *out);
int gfx_fbinfo(gfx_fbinfo_t *out);

#endif
