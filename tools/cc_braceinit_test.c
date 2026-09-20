/* brace initialisers.  cc tinit.c -> tinit */
#include <stdio.h>

struct pt { int x; int y; };
struct box { struct pt a; struct pt b; int tag; };

int g_tab[] = {10, 20, 30, 40};
int g_fixed[6] = {1, 2, 3};              /* rest zero-filled */
struct pt g_pt = {7, 8};
const char *g_names[] = {"alpha", "beta", "gamma"};
struct box g_box = { {1, 2}, {3, 4}, 99 };

int main() {
    printf("g_tab   = %d %d %d %d (n=%d)\n",
           g_tab[0], g_tab[1], g_tab[2], g_tab[3],
           (int)(sizeof(g_tab) / sizeof(g_tab[0])));
    printf("g_fixed = %d %d %d %d %d %d\n",
           g_fixed[0], g_fixed[1], g_fixed[2], g_fixed[3], g_fixed[4], g_fixed[5]);
    printf("g_pt    = %d,%d\n", g_pt.x, g_pt.y);
    printf("g_names = %s %s %s\n", g_names[0], g_names[1], g_names[2]);
    printf("g_box   = (%d,%d) (%d,%d) tag %d\n",
           g_box.a.x, g_box.a.y, g_box.b.x, g_box.b.y, g_box.tag);

    int l_tab[] = {5, 6, 7};
    int l_fixed[5] = {9, 8};
    struct pt l_pt = {11, 12};
    struct box l_box = { {1, 2}, {3, 4}, 42 };

    printf("l_tab   = %d %d %d (n=%d)\n", l_tab[0], l_tab[1], l_tab[2],
           (int)(sizeof(l_tab) / sizeof(l_tab[0])));
    printf("l_fixed = %d %d %d %d %d\n",
           l_fixed[0], l_fixed[1], l_fixed[2], l_fixed[3], l_fixed[4]);
    printf("l_pt    = %d,%d\n", l_pt.x, l_pt.y);
    printf("l_box   = (%d,%d) (%d,%d) tag %d\n",
           l_box.a.x, l_box.a.y, l_box.b.x, l_box.b.y, l_box.tag);

    /* non-constant local initialiser */
    int n = 3;
    int calc[] = {n, n * 2, n * 3};
    printf("calc    = %d %d %d\n", calc[0], calc[1], calc[2]);

    /* the classic lookup table */
    int sum = 0;
    for (int i = 0; i < 4; i++) sum += g_tab[i];
    printf("sum     = %d\n", sum);
    return 0;
}
