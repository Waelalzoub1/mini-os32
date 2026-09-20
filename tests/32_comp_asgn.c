// compound assignment operators
#include <stdio.h>
int main() {
    int i = 1; i += 2; i *= 3; i -= 1; i <<= 1; i |= 1; i ^= 4; i &= 0x1F; i >>= 1; i %= 7; i /= 2;
    unsigned int u = 0x80000000; u >>= 31;
    printf("%d %u\n", i, u);
    return 0;
}
