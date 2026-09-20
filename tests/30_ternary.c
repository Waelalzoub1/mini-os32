// ?: ternary — README lists this as unsupported
#include <stdio.h>
int main() {
    int a = 3; int b = 7;
    int m = a > b ? a : b;
    printf("%d %d %s\n", m, a < b ? 1 : 0, a ? "yes" : "no");
    return 0;
}
