// integer literal suffixes u/U/l/L are not lexed
#include <stdio.h>
int main() {
    unsigned int u = 4000000000u; int l = 5L;
    printf("%u %d\n", u, l);
    return 0;
}
