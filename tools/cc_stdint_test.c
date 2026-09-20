/* cc conformance sample for stdint.h and const.
 * Copy into fs/, rebuild, then inside the OS:  cc tint.c -> tint ; run tint */
#include <stdio.h>
#include <stdint.h>

const int g_const = 7;

struct pkt {
    uint8_t  kind;
    uint16_t len;
    uint32_t id;
};

/* const in every position C allows it */
static int sum(const int *a, int n) {
    int i = 0;
    int t = 0;
    while (i < n) { t = t + a[i]; i = i + 1; }
    return t;
}

static int count(char const *s) {
    int n = 0;
    while (s[n]) n = n + 1;
    return n;
}

static int deref(int * const p) { return *p; }

int main() {
    printf("sizes: u8=%d u16=%d u32=%d ptr=%d\n",
           sizeof(uint8_t), sizeof(uint16_t), sizeof(uint32_t), sizeof(void *));

    printf("struct pkt=%d (1+2+4 packed to alignment)\n", sizeof(struct pkt));

    uint8_t b = 250;
    b = b + 10;                       /* wraps at 8 bits */
    printf("uint8 250+10 = %d\n", b);

    uint16_t w = 65530;
    w = w + 10;                       /* wraps at 16 bits */
    printf("uint16 65530+10 = %d\n", w);

    int16_t sw = -2;
    printf("int16 -2 = %d\n", sw);

    uint32_t big = 4000000000u;
    printf("uint32 big = %u\n", big);

    printf("cast (uint8_t)300 = %d\n", (uint8_t)300);
    printf("cast (uint16_t)70000 = %d\n", (uint16_t)70000);
    printf("cast (int8_t)200 = %d\n", (int8_t)200);

    struct pkt p;
    p.kind = 3;
    p.len = 1400;
    p.id = 305419896;
    printf("pkt: kind=%d len=%d id=%X\n", p.kind, p.len, p.id);

    const char *msg = "const array";
    int arr[4];
    arr[0] = 1; arr[1] = 2; arr[2] = 3; arr[3] = 4;
    printf("sum=%d count=%d deref=%d gconst=%d\n",
           sum(arr, 4), count(msg), deref(&arr[2]), g_const);
    return 0;
}
