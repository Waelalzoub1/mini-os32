// FILES: string.c
// fixed-width typedefs as cc.c uses them; a store through uint16_t* must write exactly 2 bytes
#include <stdio.h>
#include <string.h>
typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
int main() {
    uint8_t buf[8]; memset(buf, 0xAA, 8);
    uint8_t *p = buf;
    *(uint16_t*)(p + 2) = 0x0102;
    *(uint32_t*)(p + 4) = 0x11223344;
    printf("%x %x %x %x\n", buf[0], buf[1], buf[2], buf[3]);
    printf("%x %x %x %x\n", buf[4], buf[5], buf[6], buf[7]);
    uint16_t h = 0xFFFF; h = h + 1;
    printf("%u %d\n", h, sizeof(uint16_t));
    return 0;
}
