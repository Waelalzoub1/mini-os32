// functions: 8 params, recursion, void, early return, prototypes, call-by-value
#include <stdio.h>
int sum8(int a, int b, int c, int d, int e, int f, int g, int h) { return a+b+c+d+e+f+g+h; }
int fib(int n) { if (n < 2) return n; return fib(n - 1) + fib(n - 2); }
void noret(int *p) { *p = 1; return; }
void bump(int v) { v = v + 1; }
int main() {
    int flag = 0; int v = 5;
    noret(&flag); bump(v);
    printf("%d %d %d %d\n", sum8(1,2,3,4,5,6,7,8), fib(15), flag, v);
    return 0;
}
