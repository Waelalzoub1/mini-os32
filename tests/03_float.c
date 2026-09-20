// float arithmetic and int<->float conversion (no float printf in this libc)
#include <stdio.h>
int main() {
    float f = 3.5; float g = 2.0; int i = 7;
    float h = f * g + i;
    printf("%d\n", (int)h);
    printf("%d\n", (int)(f / g * 100.0));
    float k = i / 2;
    printf("%d\n", (int)k);
    float m = i; m = m / 2;
    printf("%d\n", (int)(m * 10));
    float neg = 0.0 - f;
    printf("%d\n", (int)neg);
    return 0;
}
