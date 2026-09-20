// '\0' and "\0" — the lexer only translates \n and \t, so '\0' silently becomes '0' (48)
#include <stdio.h>
int main() {
    char s[4]; s[0] = 'a'; s[1] = '\0'; s[2] = 'z'; s[3] = 0;
    printf("%d %s\n", '\0', s);
    return 0;
}
