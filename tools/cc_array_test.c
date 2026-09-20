/* array-size inference and string-literal globals.  cc tarr.c -> tarr */
#include <stdio.h>

/* global char array, size inferred from the literal */
char g_arr[] = "inferred global";

/* global pointer to a literal: needs a relocation into .data */
const char *g_ptr = "global pointer";

/* explicit size, zero-padded past the text */
char g_fixed[8] = "abc";

static int slen(const char *s) { int n = 0; while (s[n]) n = n + 1; return n; }

int main() {
    char l_arr[] = "inferred local";
    char l_fixed[8] = "xy";

    printf("g_arr  = %s (len %d, sizeof %d)\n", g_arr, slen(g_arr), sizeof(g_arr));
    printf("g_ptr  = %s (len %d)\n", g_ptr, slen(g_ptr));
    printf("g_fixed= %s (sizeof %d, tail %d %d)\n",
           g_fixed, sizeof(g_fixed), g_fixed[3], g_fixed[7]);
    printf("l_arr  = %s (len %d, sizeof %d)\n", l_arr, slen(l_arr), sizeof(l_arr));
    printf("l_fixed= %s (sizeof %d, tail %d)\n", l_fixed, sizeof(l_fixed), l_fixed[7]);

    l_arr[0] = 'I';
    printf("writable: %s\n", l_arr);
    return 0;
}
