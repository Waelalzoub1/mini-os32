// typedef of struct, pointer, array, primitive
#include <stdio.h>
typedef unsigned int u32;
typedef struct { int x; int y; } Point;
typedef int *IntPtr;
typedef char Name[16];
int main() {
    u32 a = 0xEE6B2800; Point p; IntPtr ip; Name n;
    p.x = 3; p.y = 4; ip = &p.x; *ip = 30;
    n[0] = 'o'; n[1] = 'k'; n[2] = 0;
    printf("%u %d %d %s %d\n", a, p.x, p.y, n, sizeof(Name));
    return 0;
}
