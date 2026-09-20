// EXPECT: compile-error
// double — README lists as unsupported
#include <stdio.h>
int main() {
    double d = 1.5;
    printf("%d\n", (int)(d * 2));
    return 0;
}
