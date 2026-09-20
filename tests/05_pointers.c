// pointers: & * pointer arithmetic, pointer to pointer
#include <stdio.h>
int main() {
    int x = 10; int *p = &x; *p = *p + 5;
    int **pp = &p; **pp = **pp * 2;
    printf("%d %d\n", x, *p);
    int arr[4]; arr[0] = 1; arr[1] = 2; arr[2] = 3; arr[3] = 4;
    int *q = arr; q = q + 2;
    printf("%d %d %d\n", *q, *(q - 1), q[1]);
    printf("%d\n", (int)(q - arr));
    char *s = "hello"; s = s + 1;
    printf("%s %c\n", s, *s);
    return 0;
}
