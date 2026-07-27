/*
  64-bit unsigned divide/modulo helpers.

  libsmb2 divides 64-bit quantities (file offsets, FILETIME conversion), which the compiler lowers
  to calls into __udivdi3/__umoddi3. Those normally come from libgcc -- but linking the toolchain's
  libgcc.a into an IRX breaks iopfixup with "R_MIPS_LO16 without R_MIPS_HI16", because libgcc is
  built with explicit relocations while IRX modules must be built -mno-explicit-relocs. Verified by
  dropping -lgcc: the relocation error disappears and only these two symbols go unresolved.

  So they are provided here instead, compiled with this module's own flags. Plain restoring binary
  long division -- correctness matters far more than speed, since these sit on path lookups and
  timestamp conversion, not on bulk transfer.
*/

#include <tamtypes.h>

typedef unsigned long long u64_t;

/*
  Variable-distance 64-bit shifts also lower to libgcc calls, so they need the same treatment --
  including the ones inside the division routine below. Both are written against the two 32-bit
  halves so nothing here can recurse back into a helper that does not exist.

  The halves are laid out low-word-first: the IOP is little-endian.
*/
typedef union
{
    u64_t v;
    struct
    {
        unsigned int lo;
        unsigned int hi;
    } h;
} split64_t;

u64_t __ashldi3(u64_t a, int b)
{
    split64_t s;

    if (b == 0)
        return a;

    s.v = a;

    if (b >= 32) {
        s.h.hi = s.h.lo << (b - 32);
        s.h.lo = 0;
    } else {
        s.h.hi = (s.h.hi << b) | (s.h.lo >> (32 - b));
        s.h.lo = s.h.lo << b;
    }

    return s.v;
}

u64_t __lshrdi3(u64_t a, int b)
{
    split64_t s;

    if (b == 0)
        return a;

    s.v = a;

    if (b >= 32) {
        s.h.lo = s.h.hi >> (b - 32);
        s.h.hi = 0;
    } else {
        s.h.lo = (s.h.lo >> b) | (s.h.hi << (32 - b));
        s.h.hi = s.h.hi >> b;
    }

    return s.v;
}

// Single pass computing quotient and remainder together; both entry points share it so the two can
// never disagree.
static void udivmoddi(u64_t num, u64_t den, u64_t *quot, u64_t *rem)
{
    u64_t q = 0, r = 0;
    int i;

    if (den == 0) {
        // Undefined in C. Return zeros rather than trapping: a division by zero here would be a
        // libsmb2 bug, and taking down the IOP mid-browse is a worse outcome than a wrong value.
        if (quot != NULL)
            *quot = 0;
        if (rem != NULL)
            *rem = 0;
        return;
    }

    for (i = 63; i >= 0; i--) {
        r <<= 1;
        r |= (num >> i) & 1ULL;
        if (r >= den) {
            r -= den;
            q |= (1ULL << i);
        }
    }

    if (quot != NULL)
        *quot = q;
    if (rem != NULL)
        *rem = r;
}

u64_t __udivdi3(u64_t num, u64_t den)
{
    u64_t q;

    udivmoddi(num, den, &q, NULL);

    return q;
}

u64_t __umoddi3(u64_t num, u64_t den)
{
    u64_t r;

    udivmoddi(num, den, NULL, &r);

    return r;
}

/* Arithmetic (sign-propagating) 64-bit right shift; same reasoning as __lshrdi3 above, but the
   high word shifts in the sign bit. */
long long __ashrdi3(long long a, int b)
{
    union
    {
        long long v;
        struct
        {
            unsigned int lo;
            int hi;
        } h;
    } s;

    if (b == 0)
        return a;

    s.v = a;

    if (b >= 32) {
        s.h.lo = (unsigned int)(s.h.hi >> (b - 32));
        s.h.hi = s.h.hi >> 31; /* 0 or -1 */
    } else {
        s.h.lo = (s.h.lo >> b) | ((unsigned int)s.h.hi << (32 - b));
        s.h.hi = s.h.hi >> b;
    }

    return s.v;
}

/* 32-bit byte swap. libsmb2 uses it for endian conversion; the IOP is little-endian and SMB2 is
   little-endian, so this is only hit on the few big-endian-tagged fields. */
unsigned int __bswapsi2(unsigned int x)
{
    return ((x & 0x000000FFu) << 24) |
           ((x & 0x0000FF00u) << 8) |
           ((x & 0x00FF0000u) >> 8) |
           ((x & 0xFF000000u) >> 24);
}
