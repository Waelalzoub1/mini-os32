// literals: decimal, hex, char, string, the documented escapes \n \t plus \\ \" \'
#include <stdio.h>
int main() {
    printf("%d %d %x %X\n", 255, 0xFF, 255, 0xabc);
    printf("[tab\there]\n");
    printf("%d %d %d\n", '\\', '\'', '"');
    printf("quote\" backslash\\ end\n");
    return 0;
}
