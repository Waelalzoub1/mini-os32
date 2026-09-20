#ifndef STDINT_H
#define STDINT_H

/* Fixed-width integer types for mini-os32.
 *
 * This is an ILP32 target: int and pointers are both 32 bits, and `long` is
 * also 32 bits.  cc has no 64-bit integer type, so int64_t/uint64_t are
 * deliberately absent rather than defined as a lie -- code needing 64 bits
 * has to carry a high and low half itself.
 */

typedef signed char        int8_t;
typedef unsigned char      uint8_t;
typedef short              int16_t;
typedef unsigned short     uint16_t;
typedef int                int32_t;
typedef unsigned int       uint32_t;

/* Pointer-sized and size types, 32-bit here. */
typedef int                intptr_t;
typedef unsigned int       uintptr_t;
typedef int                ptrdiff_t;
typedef unsigned int       size_t;

/* "Fast"/"least" variants map to the natural word where that is no smaller. */
typedef signed char        int_least8_t;
typedef unsigned char      uint_least8_t;
typedef short              int_least16_t;
typedef unsigned short     uint_least16_t;
typedef int                int_least32_t;
typedef unsigned int       uint_least32_t;

typedef int                int_fast8_t;
typedef unsigned int       uint_fast8_t;
typedef int                int_fast16_t;
typedef unsigned int       uint_fast16_t;
typedef int                int_fast32_t;
typedef unsigned int       uint_fast32_t;

typedef int                intmax_t;
typedef unsigned int       uintmax_t;

#define INT8_MIN    (-128)
#define INT8_MAX    127
#define UINT8_MAX   255

#define INT16_MIN   (-32768)
#define INT16_MAX   32767
#define UINT16_MAX  65535

#define INT32_MIN   (-2147483647 - 1)
#define INT32_MAX   2147483647
#define UINT32_MAX  4294967295u

#define INTPTR_MIN  INT32_MIN
#define INTPTR_MAX  INT32_MAX
#define UINTPTR_MAX UINT32_MAX
#define SIZE_MAX    UINT32_MAX
#define INTMAX_MIN  INT32_MIN
#define INTMAX_MAX  INT32_MAX
#define UINTMAX_MAX UINT32_MAX

#endif
