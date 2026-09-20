// EXPECT: compile-error
// compound literals are not supported
#include <stdio.h>
struct Fix { int pos; int id; };
struct Fix fixes[4];
int main() {
    fixes[0] = (struct Fix){ 5, 6 };
    printf("%d %d\n", fixes[0].pos, fixes[0].id);
    return 0;
}
