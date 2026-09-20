// sizeof on types and expressions
#include <stdio.h>
struct S { char a; int b; char c; };
int main() {
    int arr[10]; struct S s; int *p; char ch;
    printf("%d %d %d %d\n", sizeof(int), sizeof(char), sizeof(int *), sizeof(float));
    printf("%d %d %d %d\n", sizeof arr, sizeof(arr), sizeof(struct S), sizeof s);
    printf("%d %d %d\n", sizeof p, sizeof(*p), sizeof ch);
    return 0;
}
