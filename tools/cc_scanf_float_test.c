#include <stdio.h>

int main() {
    float a = 99.0;
    int n = 0;

    printf("A? ");
    n = scanf("%f", &a);
    printf("n=%d a=%f\n", n, a);

    printf("B? ");
    n = scanf("%f", &a);
    printf("n=%d a=%f\n", n, a);

    printf("C? ");
    n = scanf("%f", &a);
    printf("n=%d a=%g\n", n, a);

    printf("D? ");
    n = scanf("%f", &a);
    printf("n=%d a=%f\n", n, a);

    printf("E? ");
    n = scanf("%f", &a);
    printf("n=%d a=%f\n", n, a);

    printf("F? ");
    n = scanf("x=%f", &a);
    printf("n=%d a=%f\n", n, a);
    return 0;
}
