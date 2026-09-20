/* The same float paths as tools/cc_float_test.c, but through user/libc.c
 * (the gcc-built libc that shell/cc/as link).  Build it the way build.sh
 * builds vbeprobe, drop the ELF in fs/, and the two outputs must agree. */
#include "libc.h"

int main(void) {
    float a = 99.0f;
    int n;

    printf("pi %f  neg %f  third %f\n", 3.14159f, -2.5f, 1.0f / 3.0f);
    printf("prec .2 %.2f  prec .0 %.0f  rounding %.2f %.2f\n",
           3.14159f, 3.14159f, 0.999f, 2.6666f);
    printf("g small %g  g tiny %g  g plain %g  g trail %g\n",
           0.0001f, 0.00001f, 1.5f, 2.0f);
    printf("g big %g  g mid %g  f big %f\n", 1234567.0f, 123456.0f, 2.5e9f);
    printf("mixed %d %f txt %g\n", 42, 1.25f, 0.5f);

    float z = 0.0f, one = 1.0f;
    printf("inf %f  -inf %f  ginf %g  nan %f\n", one / z, -one / z, one / z, z / z);
    printf("negzero %f\n", -0.0f);
    printf("wide %.30f  wideg %.12g\n", 1.5f, 12345678900.0f);

    printf("A? ");  n = scanf("%f", &a);   printf("n=%d a=%f\n", n, a);
    printf("B? ");  n = scanf("%lf", &a);  printf("n=%d a=%g\n", n, a);
    printf("C? ");  n = scanf("%f", &a);   printf("n=%d a=%f\n", n, a);
    return 0;
}
