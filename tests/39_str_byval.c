// EXPECT: compile-error
// struct passed and returned by value — README lists as unsupported
#include <stdio.h>
struct P { int x; int y; };
struct P mk(int x) { struct P p; p.x = x; p.y = x * 2; return p; }
int sum(struct P p) { return p.x + p.y; }
int main() {
    printf("%d\n", sum(mk(3)));
    return 0;
}
