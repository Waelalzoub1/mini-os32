// EXPECT: compile-error
// adjacent string literal concatenation — README lists as unsupported
#include <stdio.h>
int main() {
    printf("abc" "def" "\n");
    return 0;
}
