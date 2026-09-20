// const and volatile qualifiers are accepted
#include <stdio.h>
const int LIMIT = 7;
int main() {
    const char *msg = "const ok"; volatile int v = 3; const int local = 4;
    v = v + LIMIT;
    printf("%s %d %d\n", msg, v, local);
    return 0;
}
