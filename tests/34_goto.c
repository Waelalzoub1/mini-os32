// goto with labels, forward and backward
#include <stdio.h>
int main() {
    int i = 0;
again:
    i = i + 1;
    if (i < 3) goto again;
    goto done;
    i = 100;
done:
    printf("%d\n", i);
    return 0;
}
