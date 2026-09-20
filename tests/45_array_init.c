// array and struct initializer lists
#include <stdio.h>
int table[4] = { 10, 20, 30, 40 };
struct P { int x; int y; } origin = { 3, 4 };
int main() {
    int local[3] = { 1, 2, 3 };
    char s[] = "abc";
    printf("%d %d %d %d %s\n", table[3], local[1], origin.x, origin.y, s);
    return 0;
}
