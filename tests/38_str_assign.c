// struct assignment by value (whole-struct copy), including nested and array members
#include <stdio.h>
struct P { int x; int y; char tag[4]; };
struct Q { struct P p; int z; };
int main() {
    struct P a; struct P b; a.x = 1; a.y = 2; a.tag[0] = 'k'; a.tag[1] = 0;
    b = a; b.x = 9;
    struct Q qa; struct Q qb; qa.p = a; qa.z = 7; qb = qa;
    printf("%d %d %s %d %d %d\n", a.x, b.x, b.tag, b.y, qb.p.y, qb.z);
    return 0;
}
