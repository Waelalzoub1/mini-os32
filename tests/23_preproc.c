// #define, function-like macro, #ifdef/#ifndef/#if/#elif/#else/#endif, #undef, __LINE__, #include "file"
#include <stdio.h>
#include "23_inc.h"
#define N 4
#define SQ(x) ((x) * (x))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define FLAG
int main() {
    printf("%d %d %d\n", N, SQ(N + 1), MAX(3, 9));
#ifdef FLAG
    printf("flag\n");
#endif
#ifndef NOPE
    printf("nonope\n");
#endif
#if N == 3
    printf("three\n");
#elif N == 4
    printf("four\n");
#else
    printf("other\n");
#endif
#undef N
#ifdef N
    printf("still\n");
#endif
    printf("%d %d\n", INC_VALUE, __LINE__);
    return 0;
}
