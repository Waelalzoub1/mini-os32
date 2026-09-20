// comparisons, && || ! with short-circuit (side effects must not run)
#include <stdio.h>
int hits = 0;
int touch(int v) { hits = hits + 1; return v; }
int main() {
    int a = 3; int b = 5;
    printf("%d%d%d%d%d%d\n", a == b, a != b, a < b, a <= b, a > b, a >= b);
    if (0 && touch(1)) printf("bad\n");
    if (1 || touch(1)) printf("sc\n");
    printf("%d %d %d\n", hits, !0, !7);
    if (touch(1) && touch(1)) printf("both %d\n", hits);
    unsigned int u = 0xEE6B2800; int s = -1;
    printf("%d\n", u > 1);
    return 0;
}
