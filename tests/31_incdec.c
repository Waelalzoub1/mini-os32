// ++ and -- (prefix and postfix), as statements and inside expressions.
// (Argument evaluation order is unspecified in C, so nothing here relies on it.)
#include <stdio.h>
int main() {
    int i = 0; int a[3]; int k = 0;
    i++; ++i; i--;
    a[k++] = 10; a[k++] = 20; a[k] = 30;
    int p = ++i; int q = i++; int r = --i; int s = i--;
    printf("%d %d %d %d %d %d %d\n", i, k, a[0] + a[1] + a[2], p, q, r, s);
    return 0;
}
