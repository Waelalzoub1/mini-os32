// function pointers — README lists as unsupported; this checks what actually happens
#include <stdio.h>
int add(int a, int b) { return a + b; }
int sub(int a, int b) { return a - b; }
int apply(int (*op)(int, int), int x, int y) { return op(x, y); }
int main() {
    int (*fp)(int, int) = add;
    printf("%d %d\n", fp(2, 3), apply(sub, 9, 4));
    return 0;
}
