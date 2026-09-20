// enum with explicit and implicit values, negative
#include <stdio.h>
enum Color { RED, GREEN = 5, BLUE, NEG = -3 };
typedef enum { T_A = 1, T_B = 2 } Tag;
int main() {
    enum Color c = BLUE; Tag t = T_B;
    printf("%d %d %d %d %d %d\n", RED, GREEN, BLUE, NEG, c, t);
    printf("%d\n", sizeof(enum Color));
    return 0;
}
