// casts between int/char/unsigned/pointer/float
#include <stdio.h>
int main() {
    int i = 300; unsigned char uc = (unsigned char)i; char c = (char)200;
    unsigned int u = (unsigned int)-1; float f = (float)7; int fi = (int)3.99;
    int *p = (int *)0x1000; int pv = (int)p;
    printf("%d %d %u %d %d %d\n", uc, c, u, (int)f, fi, pv);
    return 0;
}
