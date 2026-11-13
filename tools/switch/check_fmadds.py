#!/usr/bin/env python3
"""Checks LostOdysseyRecomp/cpu/guest_fmadds.h against an exact reference, on an AArch64 host (an Apple silicon Mac).

Builds a small driver with the host's C++ compiler, feeds it fmadds operands and compares each of LoFmadds, LoFmsubs,
LoFnmadds and LoFnmsubs with a * c +- b computed exactly (fractions) and rounded once to single precision, ties to even,
subnormals and overflow included: the Xenon's fmadds. Cases: random floats, near-cancellations, subnormal results,
infinities, zeros and NaNs, and doubles that are not floats. NaN and non-float inputs must give the old double form's
bits exactly, exact zero results its sign. One constructed operand set must show the old form's double rounding
(one ulp off) and the new one exact.

  python3 tools/switch/check_fmadds.py [--cases N] [--cxx c++]
"""
import argparse
import math
import platform
import random
import struct
import subprocess
import sys
import tempfile
from fractions import Fraction
from pathlib import Path

HEADER = Path(__file__).resolve().parents[2] / 'LostOdysseyRecomp/cpu/guest_fmadds.h'
DRIVER = r'''
#include "guest_fmadds.h"
#include <cstdio>
#include <cstring>
static unsigned long long Bits(double x) { unsigned long long u; std::memcpy(&u, &x, 8); return u; }
static double Value(unsigned long long u) { double x; std::memcpy(&x, &u, 8); return x; }
int main()
{
    unsigned long long a, c, b;
    while (std::scanf("%llx %llx %llx", &a, &c, &b) == 3)
    {
        volatile double va = Value(a), vc = Value(c), vb = Value(b); // the exactness test at run time
        std::printf("%016llx %016llx %016llx %016llx %016llx %016llx\n", Bits(LoFmadds(va, vc, vb)),
            Bits(LoFmsubs(va, vc, vb)), Bits(LoFnmadds(va, vc, vb)), Bits(LoFnmsubs(va, vc, vb)),
            Bits(double(float(__builtin_fma(va, vc, vb)))), Bits(double(float(-__builtin_fma(va, vc, -vb)))));
    }
}
'''


def f32(x):
    try:
        return struct.unpack('<f', struct.pack('<f', x))[0]
    except OverflowError:  # beyond the float range (the C conversion's result would be an infinity)
        return math.copysign(math.inf, x)


def from_f32_bits(u):
    return struct.unpack('<f', struct.pack('<I', u))[0]


def bits(x):
    return struct.unpack('<Q', struct.pack('<d', x))[0]


def is_float(x):
    return not math.isnan(x) and (math.isinf(x) or f32(x) == x)


def round_single(q):
    """The nearest float to the exact value q, ties to even (Python float, or +-inf)."""
    if q == 0:
        return 0.0
    sign = -1.0 if q < 0 else 1.0
    q = abs(q)
    e = q.numerator.bit_length() - q.denominator.bit_length()
    while Fraction(2) ** e > q:
        e -= 1
    while Fraction(2) ** (e + 1) <= q:
        e += 1
    ulp = Fraction(2) ** (max(e, -126) - 23)
    n, r = divmod(q, ulp)
    if r * 2 > ulp or (r * 2 == ulp and n % 2):
        n += 1
    v = n * ulp
    return sign * math.inf if v >= Fraction(2) ** 128 else sign * float(v)


def cases(count, rng):
    def rf(lo=-30, hi=30):
        while True:
            u = rng.getrandbits(32)
            if rng.random() < 0.9:
                u = (u & 0x807FFFFF) | ((127 + rng.randint(lo, hi)) << 23)
            x = from_f32_bits(u)
            if not math.isnan(x) and not math.isinf(x):
                return x
    # Double rounding: a * c = 2^-24 (1 - 2^-46) just under half an ulp of b = 1 + 2^-23 (odd); in double the sum
    # rounds to the halfway point, which then rounds to even, up. The exact sum rounds down.
    yield 2.0 ** -12 * (1 + 2.0 ** -23), 2.0 ** -12 * (1 - 2.0 ** -23), 1 + 2.0 ** -23
    for _ in range(count):
        yield rf(), rf(), rf()
    for _ in range(count // 4):
        x, y = rf(-5, 5), rf(-5, 5)
        if abs(x * y) < 3e38:
            yield x, y, -f32(x * y)
    for _ in range(count // 10):
        sub = from_f32_bits(rng.getrandbits(23) | (rng.getrandbits(1) << 31))
        yield rf(-70, -60), rf(-70, -60), sub if rng.random() < 0.5 else 0.0
    special = [0.0, -0.0, math.inf, -math.inf, math.nan, from_f32_bits(0x7FC00001), from_f32_bits(0xFFC12345),
               from_f32_bits(1), from_f32_bits(0x80800000), 1.0, -1.0, 3.4e38]
    for x in special:
        for y in special:
            for z in special:
                yield x, y, z
    for _ in range(count // 10):
        yield rng.uniform(-10, 10), rf(), rf()
        yield rf(), rf(), rng.uniform(-1, 1)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--cases', type=int, default=200000)
    ap.add_argument('--cxx', default='c++')
    args = ap.parse_args()
    if platform.machine() not in ('arm64', 'aarch64'):
        sys.exit('check_fmadds: needs an AArch64 host (the header computes the double form elsewhere)')
    with tempfile.TemporaryDirectory() as tmp:
        src = Path(tmp) / 'driver.cpp'
        src.write_text(DRIVER)
        exe = Path(tmp) / 'driver'
        subprocess.run([args.cxx, '-std=c++20', '-O2', '-ffp-contract=off', f'-I{HEADER.parent}', str(src), '-o', str(exe)],
                       check=True)
        inputs = list(cases(args.cases, random.Random(1)))
        text = '\n'.join('%016x %016x %016x' % (bits(a), bits(c), bits(b)) for a, c, b in inputs) + '\n'
        out = subprocess.run([str(exe)], input=text, capture_output=True, text=True, check=True).stdout.split('\n')
    bad = rounded_once = exact_checked = old_form = 0
    for (a, c, b), line in zip(inputs, out):
        madd, msub, nmadd, nmsub, old_madd, old_nmsub = (int(t, 16) for t in line.split())
        if not all(is_float(v) for v in (a, c, b)) or any(math.isinf(v) for v in (a, c, b)):
            old_form += 1
            if (madd, nmsub) != (old_madd, old_nmsub):
                bad += 1
                print(f'differs from the double form: {a!r} {c!r} {b!r}')
            continue
        exact_checked += 1
        product = Fraction(a) * Fraction(c)
        for got, exact, sign in ((madd, product + Fraction(b), 1), (msub, product - Fraction(b), 1),
                                 (nmadd, product + Fraction(b), -1), (nmsub, product - Fraction(b), -1)):
            if exact == 0:
                continue  # sign checked below
            want = bits(sign * round_single(exact))
            if got != want:
                bad += 1
                if bad <= 10:
                    print(f'not the single rounding: {a!r} {c!r} {b!r} sign {sign}: {got:016x}, want {want:016x}')
        if (product + Fraction(b) == 0 and madd != old_madd) or (product - Fraction(b) == 0 and nmsub != old_nmsub):
            bad += 1
            print(f'zero sign differs from the double form: {a!r} {c!r} {b!r}')
        if madd != old_madd:
            rounded_once += 1
    first = out[0].split()
    if first[0] == first[4]:
        bad += 1
        print('the constructed double-rounding case did not differ from the double form')
    print(f'check_fmadds: {len(inputs)} operand sets, {exact_checked} checked against the exact single rounding, '
          f'{old_form} against the double form; {rounded_once} where the double form rounds differently; {bad} problems')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
