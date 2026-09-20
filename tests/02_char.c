// char and unsigned char, escapes
#include <stdio.h>
int main() {
    char c = 'a'; unsigned char uc = 200; char s = -56;
    printf("%c %d %d %d\n", c, c, uc, s);
    printf("%d %d %d\n", '\n', '\t', 'Z' - 'A');
    c = c + 1;
    printf("%c\n", c);
    return 0;
}
