// idioms cc.c relies on: while (*s) with pointer walk, char comparisons, nested ifs, negative int division
#include <stdio.h>
int count(char *s, char ch) { int n = 0; while (*s) { if (*s == ch) n = n + 1; s = s + 1; } return n; }
int main() {
    printf("%d %d\n", count("banana", 'a'), count("", 'x'));
    int n = -7;
    printf("%d %d %d\n", n / 2, n % 2, (n < 0) ? -n : n);
    return 0;
}
