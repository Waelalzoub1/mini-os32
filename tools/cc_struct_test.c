/* structs by value.  cc tstru.c -> tstru */
#include <stdio.h>

struct pt { int x; int y; };
struct box { struct pt a; struct pt b; int tag; };

struct pt g_src = {3, 4};
struct pt g_dst;

/* struct passed by value: the callee gets its own copy */
static int dist2(struct pt p) { return p.x * p.x + p.y * p.y; }

static int boxtag(struct box b) { return b.tag + b.a.x + b.b.y; }

/* two struct params, plus a scalar between them */
static int mix(struct pt a, int k, struct pt b) {
    return a.x + a.y + k + b.x + b.y;
}

/* mutating the copy must not touch the caller's struct */
static void bump(struct pt p) { p.x = 999; p.y = 999; }

int main() {
    struct pt a = {1, 2};
    struct pt b;

    b = a;                       /* local struct assignment */
    printf("b = %d,%d\n", b.x, b.y);

    b.x = 50;
    printf("after b.x=50: a = %d,%d  b = %d,%d\n", a.x, a.y, b.x, b.y);

    g_dst = g_src;               /* global struct assignment */
    printf("g_dst = %d,%d\n", g_dst.x, g_dst.y);

    printf("dist2({3,4}) = %d\n", dist2(g_src));

    struct box bx = { {1, 2}, {3, 4}, 100 };
    printf("boxtag = %d\n", boxtag(bx));

    printf("mix = %d\n", mix(a, 10, b));

    bump(a);
    printf("after bump: a = %d,%d (unchanged)\n", a.x, a.y);

    /* nested struct member assignment */
    struct box c;
    c.a = a;
    c.b = b;
    c.tag = 7;
    printf("c = (%d,%d) (%d,%d) tag %d\n", c.a.x, c.a.y, c.b.x, c.b.y, c.tag);

    /* struct in an array */
    struct pt arr[2];
    arr[0] = a;
    arr[1] = b;
    printf("arr = (%d,%d) (%d,%d)\n", arr[0].x, arr[0].y, arr[1].x, arr[1].y);
    return 0;
}
