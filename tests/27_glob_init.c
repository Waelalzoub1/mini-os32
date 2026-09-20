// global initializers: scalars, pointers to strings, global struct
#include <stdio.h>
int g = 5;
char *name = "global";
struct S { int a; int b; };
struct S gs;
unsigned int big = 0xFFFFFFFF;
int main() {
    gs.a = g * 2;
    printf("%d %s %d %u\n", g, name, gs.a, big);
    return 0;
}
