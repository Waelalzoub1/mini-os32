// printf formats %d %u %x %X %c %s %%, snprintf, puts (no newline), putc
#include <stdio.h>
int main() {
    char buf[32];
    printf("%d %u %x %X %c %s %%\n", -42, 42, 255, 255, 'Q', "str");
    int n = snprintf(buf, sizeof(buf), "n=%d;%s", 7, "ok");
    puts(buf); putc('|'); printf("%d\n", n);
    return 0;
}
