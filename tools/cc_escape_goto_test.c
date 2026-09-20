/* escapes, do/while and goto.  cc tesc.c -> tesc */
#include <stdio.h>

static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }

int main() {
    /* escapes: \0 must terminate, not become the digit 0 */
    char z[8];
    z[0] = 'a'; z[1] = 'b'; z[2] = '\0'; z[3] = 'c';
    printf("nul-terminated: [%s] len=%d\n", z, slen(z));

    printf("quote[%c] backslash[%c] tab[%d] cr[%d] bell[%d]\n",
           '\"', '\\', '\t', '\r', '\a');
    printf("hex \\x41=%c  octal \\101=%c  \\0 as int=%d\n", '\x41', '\101', '\0');
    printf("literal: [%s]\n", "A\\B\"C\rD");

    /* do/while runs the body once even when false */
    int n = 0;
    do { n++; } while (0);
    printf("do-while(0) ran %d time\n", n);

    int i = 0;
    int sum = 0;
    do {
        sum += i;
        i++;
    } while (i < 5);
    printf("do-while sum 0..4 = %d\n", sum);

    /* break and continue inside do/while */
    i = 0;
    int even = 0;
    do {
        i++;
        if (i % 2) continue;      /* continue -> the condition test */
        if (i > 8) break;
        even += i;
    } while (i < 20);
    printf("do-while even sum = %d (stopped at i=%d)\n", even, i);

    /* goto forward */
    int hits = 0;
    goto skip;
    hits = 999;
skip:
    printf("goto forward: hits=%d\n", hits);

    /* goto backward: a retry loop, the classic use */
    int tries = 0;
retry:
    tries++;
    if (tries < 3) goto retry;
    printf("goto backward: tries=%d\n", tries);

    /* goto out of nested loops, the other classic use */
    int found = -1;
    for (int y = 0; y < 5; y++) {
        for (int x = 0; x < 5; x++) {
            if (y * 5 + x == 13) { found = y * 100 + x; goto done; }
        }
    }
done:
    printf("goto out of nested loops: found=%d\n", found);
    return 0;
}
