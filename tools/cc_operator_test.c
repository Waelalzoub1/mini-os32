/* ++, -- and compound assignment.  cc top.c -> top */
#include <stdio.h>

int main() {
    /* Sequenced deliberately: passing both i++ and i to one printf would
     * depend on argument evaluation order, which C leaves unspecified (cc
     * evaluates right to left). */
    int i = 5;
    int r;
    r = i++; printf("i++ yields %d, i now %d\n", r, i);
    r = ++i; printf("++i yields %d, i now %d\n", r, i);
    r = i--; printf("i-- yields %d, i now %d\n", r, i);
    r = --i; printf("--i yields %d, i now %d\n", r, i);

    int a = 10;
    a += 5;  printf("a += 5  -> %d\n", a);
    a -= 3;  printf("a -= 3  -> %d\n", a);
    a *= 4;  printf("a *= 4  -> %d\n", a);
    a /= 6;  printf("a /= 6  -> %d\n", a);
    a %= 5;  printf("a %%= 5  -> %d\n", a);
    a |= 8;  printf("a |= 8  -> %d\n", a);
    a &= 12; printf("a &= 12 -> %d\n", a);
    a ^= 5;  printf("a ^= 5  -> %d\n", a);
    a <<= 3; printf("a <<= 3 -> %d\n", a);
    a >>= 2; printf("a >>= 2 -> %d\n", a);

    /* the idiomatic loop that previously had to be written i = i + 1 */
    int sum = 0;
    for (int k = 0; k < 5; k++) sum += k;
    printf("for-loop sum 0..4 = %d\n", sum);

    /* pointer stepping: postfix must yield the old pointer */
    char buf[] = "abcdef";
    char *p = buf;
    char first = *p++;
    char second = *p;
    printf("*p++ gave '%c', p now at '%c'\n", first, second);

    /* walking a string the way real C does */
    int n = 0;
    p = buf;
    while (*p++) n++;
    printf("length via *p++ = %d\n", n);

    /* compound assignment through a pointer and an index */
    int arr[3];
    arr[0] = 1; arr[1] = 2; arr[2] = 3;
    arr[1] += 10;
    arr[2]++;
    printf("arr = %d %d %d\n", arr[0], arr[1], arr[2]);
    return 0;
}
