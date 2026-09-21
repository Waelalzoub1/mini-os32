// passing a struct by value: the callee gets a copy
#include <stdio.h>
struct P { int x; int y; char tag[4]; };
int sum(struct P p) { p.x = p.x + 100; return p.x + p.y; }
int main() {
    struct P a; a.x = 1; a.y = 2; a.tag[0] = 'k';
    printf("%d %d\n", sum(a), a.x);
    return 0;
}
