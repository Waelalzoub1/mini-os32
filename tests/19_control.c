// if/else, while, for (expr init and declaration init), break, continue, for(;;)
#include <stdio.h>
int main() {
    int i; int s = 0;
    for (i = 0; i < 10; i = i + 1) { if (i % 2) continue; s = s + i; }
    printf("%d\n", s);
    for (int k = 0; k < 3; k = k + 1) printf("%d", k);
    printf("\n");
    int n = 0;
    while (1) { n = n + 1; if (n == 7) break; }
    printf("%d\n", n);
    for (;;) { n = n - 1; if (n < 3) break; }
    if (n == 2) printf("two\n"); else if (n == 3) printf("three\n"); else printf("other\n");
    return 0;
}
