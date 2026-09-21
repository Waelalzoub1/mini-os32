// EXPECT: compile-error
// returning a struct by value is not supported
#include <stdio.h>
struct P { int x; int y; };
struct P mk(int x) { struct P p; p.x = x; p.y = x * 2; return p; }
int main() {
    struct P p = mk(3);
    printf("%d\n", p.x + p.y);
    return 0;
}
