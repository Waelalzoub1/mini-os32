// unary - + ! ~, nested calls as arguments, parenthesized expressions
#include <stdio.h>
int id(int x) { return x; }
int main() {
    int a = 5;
    printf("%d %d %d %d\n", -a, +a, !a, ~a);
    printf("%d %d\n", id(id(id(3)) + id(4)), -(-a));
    printf("%d\n", (a + 1) * (a - 1) / (2 + 1) % 5);
    return 0;
}
