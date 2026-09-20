// FILES: string.c
// string.h functions from string.c
#include <stdio.h>
#include <string.h>
int main() {
    char buf[16]; char b2[16];
    strcpy(buf, "hello"); strncpy(b2, buf, 3); b2[3] = 0;
    memset(buf + 5, '!', 2); buf[7] = 0;
    printf("%s %s %d %d\n", buf, b2, strlen(buf), strcmp("abc", "abd"));
    memcpy(b2, "xyz", 4); memmove(b2 + 1, b2, 3); b2[4] = 0;
    printf("%s %d %d\n", b2, memcmp("ab", "ab", 2), strncmp("abcd", "abzz", 2));
    return 0;
}
