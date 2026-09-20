// switch/case/default with fallthrough and break, char cases, negative case
#include <stdio.h>
int classify(int v) {
    switch (v) {
    case 1: case 2: return 12;
    case -3: return -3;
    case 'a': return 97;
    default: return 0;
    }
}
int main() {
    int out = 0;
    switch (2) { case 1: out = out + 1; case 2: out = out + 10; case 3: out = out + 100; break; case 4: out = out + 1000; }
    printf("%d %d %d %d %d %d\n", out, classify(1), classify(2), classify(-3), classify('a'), classify(50));
    return 0;
}
