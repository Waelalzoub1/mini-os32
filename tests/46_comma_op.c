// EXPECT: compile-error
// the comma operator is not supported
#include <stdio.h>
int main() {
    int i; int j;
    for (i = 0, j = 10; i < j; i = i + 1, j = j - 1) { }
    printf("%d %d\n", i, j);
    return 0;
}
