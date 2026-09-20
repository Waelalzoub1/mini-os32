// EXPECT: compile-error
// bitfields — README lists as unsupported
#include <stdio.h>
struct B { unsigned int a : 3; unsigned int b : 5; };
int main() {
    struct B b; b.a = 5; b.b = 17;
    printf("%d %d\n", b.a, b.b);
    return 0;
}
