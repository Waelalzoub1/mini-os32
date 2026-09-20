// static globals, static functions, static locals that persist across calls
#include <stdio.h>
static int counter = 100;
static int bump(void) { static int calls = 0; calls = calls + 1; counter = counter + 1; return calls; }
int main() {
    bump(); bump();
    int third = bump();
    printf("%d %d\n", third, counter);
    return 0;
}
