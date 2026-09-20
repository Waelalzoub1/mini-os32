// struct, member access . and ->, nested, sizeof(struct)
#include <stdio.h>
struct Inner { int a; char b; };
struct Outer { int id; struct Inner in; int arr[3]; struct Inner *ptr; };
struct Outer go;
int sum(struct Outer *o) { return o->id + o->in.a + o->arr[2] + o->ptr->a; }
int main() {
    struct Outer o;
    o.id = 1; o.in.a = 20; o.in.b = 'x'; o.arr[2] = 300; o.ptr = &o.in;
    printf("%d %c\n", sum(&o), o.in.b);
    go.id = 5; go.in.a = 6; go.arr[2] = 7; go.ptr = &go.in;
    printf("%d\n", sum(&go));
    printf("%d %d\n", sizeof(struct Inner), sizeof(struct Outer));
    return 0;
}
