// union: overlapping members, sizeof
#include <stdio.h>
union U { int i; char c[4]; };
int main() {
    union U u; u.i = 0x41424344;
    printf("%c %c %d\n", u.c[0], u.c[3], sizeof(union U));
    return 0;
}
