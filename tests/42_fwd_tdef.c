// forward struct typedef before the struct body (cc.c line 101 uses this)
#include <stdio.h>
typedef struct Node Node;
struct Node { int v; Node *next; };
int main() {
    Node a; Node b; a.v = 1; b.v = 2; a.next = &b; b.next = 0;
    printf("%d %d\n", a.v, a.next->v);
    return 0;
}
