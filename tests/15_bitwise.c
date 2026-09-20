// bitwise & | ^ ~ << >>, unsigned vs signed shift
#include <stdio.h>
int main() {
    int a = 0xF0; int b = 0x3C; int neg = -16; unsigned int un = 0x80000000;
    printf("%x %x %x %x\n", a & b, a | b, a ^ b, ~a & 0xFF);
    printf("%d %d %d\n", 1 << 10, 1024 >> 3, neg >> 2);
    printf("%u\n", un >> 31);
    return 0;
}
