// FILES: stdlib.c string.c
// malloc/free/calloc/realloc/atoi/strtol
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main() {
    int *a = malloc(4 * sizeof(int)); a[3] = 42;
    int *z = calloc(4, sizeof(int));
    a = realloc(a, 8 * sizeof(int)); a[7] = 8;
    printf("%d %d %d %d\n", a[3], a[7], z[2], atoi("-123"));
    char *end;
    printf("%d %d\n", strtol("ff", 0, 16), strtol("42xyz", &end, 10));
    printf("%s\n", end);
    free(a); free(z);
    return 0;
}
