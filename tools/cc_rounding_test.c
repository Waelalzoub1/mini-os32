/* FPU rounding mode: casts must chop, everything else must round to
 * nearest.  cc: source "cc_rounding_test.c", output "rt".
 * Expected: cast 2 -2 2 -2 / cast2 0 1 0 / ovf inf  div inf */
#include <stdio.h>

int main() {
    /* Casts must still chop toward zero, in both directions. */
    float a = 2.9;
    float b = 0.0 - 2.9;
    float c = 2.0;
    float d = 0.0 - 2.0;
    printf("cast %d %d %d %d\n", (int)a, (int)b, (int)c, (int)d);
    printf("cast2 %d %d %d\n", (int)0.99, (int)1.99, (int)(0.0 - 0.99));

    /* Overflow must now reach inf rather than saturating at FLT_MAX. */
    float z = 0.0;
    float one = 1.0;
    printf("ovf %g  div %g\n", 1e40, one / z);

    /* Round to nearest, not toward zero: 1/3 keeps its last digit. */
    printf("third %.9g  tenth %.9g\n", one / 3.0, one / 10.0);

    /* An int cast in the middle of an expression must not leave the FPU in
     * chop mode for the arithmetic that follows. */
    int k = (int)(one / 3.0);
    printf("after %d %.9g\n", k, one / 3.0);
    return 0;
}
