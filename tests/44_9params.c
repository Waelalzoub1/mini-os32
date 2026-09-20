// function with 9 parameters (cc.c's link_objects and build_elf take 9)
#include <stdio.h>
int sum9(int a, int b, int c, int d, int e, int f, int g, int h, int i) { return a+b+c+d+e+f+g+h+i; }
int main() {
    printf("%d\n", sum9(1,2,3,4,5,6,7,8,9));
    return 0;
}
