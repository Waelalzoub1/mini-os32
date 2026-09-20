// FILES: 12_extern_b.c
// extern across two translation units, function defined in the other file
#include <stdio.h>
extern int shared;
int twice(int x);
int main() {
    shared = shared + 1;
    printf("%d %d\n", shared, twice(21));
    return 0;
}
