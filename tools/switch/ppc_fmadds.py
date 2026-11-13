#!/usr/bin/env python3
"""Rounds the recompiled single-precision fused multiply-adds once, as the Xenon does (LostOdysseyRecomp/cpu/guest_fmadds.h).

XenonRecomp writes fmadds, fmsubs, fnmadds and fnmsubs as
    f0.f64 = double(float(f13.f64 * f0.f64 + f12.f64));          fmsubs: - f12.f64; fnm*: double(float(-(...)))
which GCC computes as one fused double operation rounded to single: two roundings, and the chain around it in double
registers. This step writes them as
    f0.f64 = LoFmadds(f13.f64, f0.f64, f12.f64);                  LoFmsubs, LoFnmadds, LoFnmsubs
which is one single-precision fused multiply-add when the inputs are floats. Each rewritten statement must follow its
instruction's comment (`// fmadds f0,f13,f0,f12`, possibly with a flush-mode statement in between) with the same
registers, and every such comment must be followed by one; otherwise the step stops without writing anything.
Files are only written when they change, so running it again is a no-op and leaves timestamps (and the build) alone.

The Switch build force-includes cpu/guest_fmadds.h into LostOdysseyRecompLib. LO_SWITCH_FMADDS_DOUBLE=ON keeps the
rewritten files and compiles the old double form instead.

  ppc_fmadds.py --ppc LostOdysseyRecompLib/ppc [--check]
"""
import argparse
import glob
import os
import re
import sys

REG = r'((?:ctx\.)?f\d+)\.f64'
STATEMENT = re.compile(r'^(\t(?:ctx\.)?f\d+\.f64 = )double\(float\((-\()?' + REG + r' \* ' + REG + r' ([+-]) ' + REG
                       + r'(\))?\)\);$')
COMMENT = re.compile(r'^\t// (fn?m(?:add|sub)s) f(\d+),f(\d+),f(\d+),f(\d+)$')
FLUSH = re.compile(r'^\tctx\.fpscr\.(?:disable|enable)FlushMode\w*\(\);$')
FUNCTION = {('', '+'): 'LoFmadds', ('', '-'): 'LoFmsubs', ('-(', '+'): 'LoFnmadds', ('-(', '-'): 'LoFnmsubs'}
OP = {'fmadds': 'LoFmadds', 'fmsubs': 'LoFmsubs', 'fnmadds': 'LoFnmadds', 'fnmsubs': 'LoFnmsubs'}


def regnum(name):
    return name.rsplit('f', 1)[1]


def rewrite(path):
    """Returns (new text or None when unchanged, rewritten count, already-rewritten count, errors)."""
    lines = open(path, encoding='utf-8', newline='').read().split('\n')
    errors = []
    count = done = 0
    pending = None  # (line index, op, registers) of the last fused-op comment not yet matched
    for i, line in enumerate(lines):
        line_body = line.rstrip('\r')
        m = COMMENT.match(line_body)
        if m:
            if pending:
                errors.append(f'{path}:{pending[0] + 1}: no statement after the comment')
            pending = (i, m.group(1), m.group(2, 3, 4, 5))
            continue
        if pending and FLUSH.match(line_body):
            continue
        if pending:
            index, op, (d, a, c, b) = pending
            pending = None
            if line_body.startswith('\t') and f'{OP[op]}(' in line_body:
                done += 1
                continue
            s = STATEMENT.match(line_body)
            if not s:
                errors.append(f'{path}:{i + 1}: {op} statement not in the expected form: {line_body.strip()}')
                continue
            lhs, neg, ra, rc, sign, rb, close = s.groups()
            if bool(neg) != bool(close):
                errors.append(f'{path}:{i + 1}: unbalanced negation: {line_body.strip()}')
                continue
            if FUNCTION[(neg or '', sign)] != OP[op]:
                errors.append(f'{path}:{i + 1}: {op} written as {FUNCTION[(neg or "", sign)]}')
                continue
            dest = re.match(r'^\t((?:ctx\.)?f\d+)\.f64', line_body).group(1)
            if [regnum(dest), regnum(ra), regnum(rc), regnum(rb)] != [d, a, c, b]:
                errors.append(f'{path}:{i + 1}: registers differ from the comment `{op} f{d},f{a},f{c},f{b}`')
                continue
            eol = line[len(line_body):]
            lines[i] = f'{lhs}{OP[op]}({ra}.f64, {rc}.f64, {rb}.f64);{eol}'
            count += 1
            continue
        # A statement in this form without its comment would be left in double; there are none in XenonRecomp's output.
        if STATEMENT.match(line_body) and ' * ' in line_body:
            errors.append(f'{path}:{i + 1}: fused statement without an fmadds-family comment: {line_body.strip()}')
    if pending:
        errors.append(f'{path}:{pending[0] + 1}: no statement after the comment')
    return ('\n'.join(lines) if count else None), count, done, errors


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--ppc', required=True, help='the generated folder (ppc_recomp.*.cpp)')
    ap.add_argument('--check', action='store_true', help='count only, write nothing')
    args = ap.parse_args()
    paths = sorted(glob.glob(os.path.join(args.ppc, 'ppc_recomp.*.cpp')))
    if not paths:
        sys.exit(f'ppc_fmadds: no ppc_recomp.*.cpp in {args.ppc}')
    results = []
    errors = []
    for path in paths:
        new, count, done, errs = rewrite(path)
        results.append((path, new, count, done))
        errors += errs
    if errors:
        for e in errors[:20]:
            print(e, file=sys.stderr)
        sys.exit(f'ppc_fmadds: {len(errors)} problems, nothing written')
    total = sum(r[2] for r in results)
    already = sum(r[3] for r in results)
    files = 0
    for path, new, count, _ in results:
        if new is not None and not args.check:
            with open(path, 'w', encoding='utf-8', newline='') as f:
                f.write(new)
            files += 1
    verb = 'to rewrite' if args.check else 'rewritten'
    print(f'ppc_fmadds: {total} single-precision fused multiply-adds {verb} in {files if not args.check else sum(1 for r in results if r[2])} '
          f'files, {already} already rewritten')
    return 0


if __name__ == '__main__':
    sys.exit(main())
