// bool and _Bool
#include <stdio.h>
int main() {
    bool a = 1; _Bool b = 0; bool c = 5;
    printf("%d %d %d\n", a, b, c);
    printf("%d %d\n", sizeof(bool), a && !b);
    return 0;
}
