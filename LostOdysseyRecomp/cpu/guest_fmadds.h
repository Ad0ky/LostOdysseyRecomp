// The single-precision fused multiply-adds (fmadds, fmsubs, fnmadds, fnmsubs) as the Xenon computes them: a * c + b
// exact, rounded once to single precision. Shared by the recompiled code (tools/switch/ppc_fmadds.py rewrites
// XenonRecomp's statements into these calls; the Switch build force-includes this header into LostOdysseyRecompLib) and
// the native versions of game functions under cpu/, so both compute the same bits.
//
// XenonRecomp writes fmadds as `double(float(a * c + b))`. GCC fuses that in double (one fmadd d) and then rounds to
// single: two roundings. When the double result lands exactly halfway between two floats and the exact one does not,
// that is one ulp off the Xenon's (rare: for random inputs about once in a few hundred million operations). It also
// keeps every chain of single-precision operations that contains an fmadds in double registers, with an fcvt before
// and after each one.
//
// Here, when a, c and b are exact floats (values from lfs and from single-precision operations, which is nearly all of
// them), it is one single-precision fused multiply-add: `fmadd s` on the AArch64, the Xenon's result exactly. GCC folds
// the test away where it sees where an input came from (an lfs, an fmuls, another fmadds), so those chains stay in
// float registers. Any other input (a double in the register, or a NaN whose bits do not survive the round trip
// through float) takes the double form, as before.
// Negating (fnmadds, fnmsubs) comes after the rounding, as on the Xenon; in round-to-nearest that is the same value as
// negating before it.
// NaN: a NaN operand that is an exact float takes the single form; AArch64 picks the same operand for fmadd s as for
// fmadd d, and converting that NaN between formats keeps its bits, so the result is the double form's.
//
// LO_FMADDS_DOUBLE_ROUNDING (CMake LO_SWITCH_FMADDS_DOUBLE=ON): the double form everywhere, for A/B runs.
// Not on AArch64: the double form (other compilers and CPUs keep the code they had).
#pragma once

#include <bit>
#include <cmath>
#include <cstdint>

#if defined(__aarch64__) && defined(__GNUC__)
#define LO_FMADDS_INLINE [[gnu::always_inline]] inline
#if !defined(LO_FMADDS_DOUBLE_ROUNDING)
#define LO_FMADDS_SINGLE 1
#endif
// The double form: one fused double operation (GCC's contraction of XenonRecomp's expression, written out so a file
// compiled with -ffp-contract=off gets the same), rounded to single.
#define LO_FMADDS_DOUBLE_FORM(a, c, b) double(float(__builtin_fma((a), (c), (b))))
#else
#define LO_FMADDS_INLINE inline
#define LO_FMADDS_DOUBLE_FORM(a, c, b) double(float((a) * (c) + (b)))
#endif
#if !defined(LO_FMADDS_SINGLE)
#define LO_FMADDS_SINGLE 0
#endif

// Whether x is a float widened to double. Compared as bits: GCC folds bit_cast(double(float(double(f)))) ==
// bit_cast(double(f)) to true, where `double(float(x)) == x` would leave an ordered compare for NaNs.
LO_FMADDS_INLINE bool LoIsExactSingle(double x)
{
    return std::bit_cast<uint64_t>(double(float(x))) == std::bit_cast<uint64_t>(x);
}

// fmadds d,a,c,b: a * c + b. fmsubs: a * c - b. Operands in the guest's order.
LO_FMADDS_INLINE double LoFmadds(double a, double c, double b)
{
#if LO_FMADDS_SINGLE
    if (__builtin_expect(LoIsExactSingle(a) & LoIsExactSingle(c) & LoIsExactSingle(b), 1))
        return double(__builtin_fmaf(float(a), float(c), float(b)));
#endif
    return LO_FMADDS_DOUBLE_FORM(a, c, b);
}

LO_FMADDS_INLINE double LoFmsubs(double a, double c, double b)
{
#if LO_FMADDS_SINGLE
    if (__builtin_expect(LoIsExactSingle(a) & LoIsExactSingle(c) & LoIsExactSingle(b), 1))
        return double(__builtin_fmaf(float(a), float(c), -float(b)));
#endif
    return LO_FMADDS_DOUBLE_FORM(a, c, -b);
}

// fnmadds: -(a * c + b). fnmsubs: -(a * c - b). The negation follows the rounding.
LO_FMADDS_INLINE double LoFnmadds(double a, double c, double b)
{
    return -LoFmadds(a, c, b);
}

LO_FMADDS_INLINE double LoFnmsubs(double a, double c, double b)
{
    return -LoFmsubs(a, c, b);
}
