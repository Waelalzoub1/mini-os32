// varargs via va_start/va_arg/va_end builtins
#include <stdio.h>
#include <stdarg.h>
int total(int n, ...) {
    va_list ap; va_start(ap, n);
    int s = 0; int i;
    for (i = 0; i < n; i = i + 1) s = s + va_arg(ap, int);
    va_end(ap);
    return s;
}
int main() {
    printf("%d %d\n", total(3, 10, 20, 30), total(0));
    return 0;
}
