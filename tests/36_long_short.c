// long and short are accepted but treated as 32-bit int (README says unsupported)
#include <stdio.h>
int main() {
    long l = 100000; short s = 7; unsigned short us = 65535; long long ll = 5;
    printf("%d %d %d %d\n", (int)l, (int)s, (int)us, (int)ll);
    printf("%d %d %d\n", sizeof(long), sizeof(short), sizeof(long long));
    return 0;
}
