// = assignment, chained assignment, assignment as expression value
#include <stdio.h>
int main() {
    int a; int b; int c;
    a = b = c = 9;
    int d = (a = 4) + 1;
    printf("%d %d %d %d\n", a, b, c, d);
    return 0;
}
