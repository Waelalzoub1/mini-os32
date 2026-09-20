/* %f, %g, exponent literals, inf/nan -- via the in-OS cc + fs/stdio.c.
 * cc: source "cc_float_test.c", output "ft".  Its output must match
 * tools/libc_float_test.c, which exercises the same paths in user/libc.c. */
#include <stdio.h>

int main() {
    printf("pi %f  neg %f  third %f\n", 3.14159, -2.5, 1.0 / 3.0);
    printf("prec .2 %.2f  prec .0 %.0f  rounding %.2f %.2f\n",
           3.14159, 3.14159, 0.999, 2.6666);
    printf("g small %g  g tiny %g  g plain %g  g trail %g\n",
           0.0001, 0.00001, 1.5, 2.0);
    printf("g big %g  g mid %g  f big %f\n", 1234567.0, 123456.0, 2500000000.0);
    printf("mixed %d %f txt %g\n", 42, 1.25, 0.5);

    printf("lit %g %g %g %g\n", 1.5e3, 1e-5, 2E+3, 6.02e10);

    float z = 0.0;
    float one = 1.0;
    float inf = one / z;
    printf("inf %f  -inf %f  ginf %g  nan %f\n", inf, 0.0 - inf, inf, z / z);

    float huge = 0.0;
    printf("scan 1e40? ");
    scanf("%f", &huge);
    printf("got %f / %g\n", huge, huge);
    printf("negzero %f\n", (0.0 - one) * z);   /* -1.0 * 0.0 == -0.0 */
    printf("wide %.30f  wideg %.12g\n", 1.5, 12345678900.0);
    return 0;
}
