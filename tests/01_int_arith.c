// int and unsigned int arithmetic
#include <stdio.h>
int main() {
    int a = 17; int b = 5; unsigned int u = 0xEE6B2800;
    printf("%d %d %d %d %d\n", a + b, a - b, a * b, a / b, a % b);
    printf("%d %d\n", -a / b, -a % b);
    printf("%u %u\n", u, u / 2);
    printf("%d\n", 0x7B + 0x10);
    return 0;
}
