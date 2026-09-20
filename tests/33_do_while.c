// do { } while
#include <stdio.h>
int main() {
    int i = 0; int n = 0;
    do { i = i + 1; } while (i < 5);
    do { n = n + 1; } while (0);
    printf("%d %d\n", i, n);
    return 0;
}
