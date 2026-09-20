// float comparisons: 3.5 vs 2.0 with all six operators (expected C semantics)
#include <stdio.h>
int main() {
    float f = 3.5; float g = 2.0; float e = 3.5;
    printf("%d%d%d%d%d%d\n", f > g, f < g, f >= g, f <= g, f == e, f != g);
    printf("%d%d%d%d\n", g > f, g < f, g >= f, g <= f);
    if (f > g) printf("gt\n"); else printf("not-gt\n");
    return 0;
}
