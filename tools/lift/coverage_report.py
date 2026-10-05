#!/usr/bin/env python3
"""Merge fallback censuses (SCD_COVERAGE files) into a report and translator seed files (ADR-003 M1).
Usage: coverage_report.py CENSUS.tsv... [--code-dir extracted/code] [--seeds-out DIR] [--check]
Seeds are disc-derived addresses: keep them under extracted/, never commit them."""
import argparse, collections, os, sys

GATE_REASONS = ('not_translated', 'code_mismatch')
SEED_REASONS = ('interpreter', 'not_translated')
MAINRAM = (0xFFE100, 0xFFE3D0)   # SP $7300..$75CF, copied by the Main CPU to $FFE100 (JOURNAL)


def read_census(path):
    rows = []
    with open(path) as f:
        for line in f:
            if not line.strip() or line.startswith('#'):
                continue
            cpu, image, reason, pc, count = line.rstrip('\n').split('\t')
            rows.append((cpu, int(image, 16), reason, int(pc, 16), int(count)))
    return rows


def code_sizes(code_dir):
    def size(name):
        p = os.path.join(code_dir, name)
        return os.path.getsize(p) if os.path.exists(p) else 0
    return {'ip': size('scd_main_ip.bin'), 'sp': size('scd_sub_sp.bin'), 'subcode': size('scd_subcode.bin')}


def program_for(cpu, image, pc, sizes):
    """The translator program (gen_program.py name) that owns pc, or None when no program covers that address."""
    if cpu == 'main':
        if 0xFF0000 <= pc < 0xFF0000 + sizes['ip']:
            return 'main'
        if MAINRAM[0] <= pc < MAINRAM[1]:
            return 'mainram'
        return None
    if image:
        return 'ovl_%08x' % image
    if 0x6000 <= pc < 0x6000 + sizes['sp'] or 0xD400 <= pc < 0xD400 + sizes['subcode']:
        return 'sub'
    return None


def merge(paths):
    total = collections.Counter()
    for p in paths:
        for cpu, image, reason, pc, count in read_census(p):
            total[(cpu, image, reason, pc)] += count
    return total


def write_seeds(total, sizes, out_dir):
    by_prog = collections.defaultdict(set)
    for (cpu, image, reason, pc) in total:
        prog = program_for(cpu, image, pc, sizes) if reason in SEED_REASONS else None
        if prog:
            by_prog[prog].add(pc)
    os.makedirs(out_dir, exist_ok=True)
    for prog, pcs in by_prog.items():
        path = os.path.join(out_dir, prog + '.txt')
        if os.path.exists(path):
            with open(path) as f:
                pcs |= {int(l, 16) for l in f if l.strip() and not l.startswith('#')}
        with open(path, 'w') as f:
            f.write('# executed PCs from coverage censuses (disc-derived, do not commit)\n')
            f.writelines('%06x\n' % pc for pc in sorted(pcs))
    return {prog: len(pcs) for prog, pcs in by_prog.items()}


def report(total, sizes):
    steps, pcs = collections.Counter(), collections.Counter()
    for (cpu, image, reason, pc), n in total.items():
        steps[reason] += n
        pcs[reason] += 1
    lines = ['%-15s %14s %10s' % ('cause', 'steps', 'pcs')]
    lines += ['%-15s %14d %10d' % (r, steps[r], pcs[r]) for r in sorted(steps)]
    gate = collections.Counter()
    for (cpu, image, reason, pc) in total:
        if reason in GATE_REASONS:
            gate[(reason, program_for(cpu, image, pc, sizes) or 'unmapped:' + cpu)] += 1
    if gate:
        lines += ['', 'gate failures (distinct PCs):']
        lines += ['  %-15s %-20s %d' % (r, prog, n) for (r, prog), n in sorted(gate.items())]
        worst = sorted(((n, k) for k, n in total.items() if k[2] in GATE_REASONS), reverse=True)[:20]
        lines += ['', 'top gate PCs (cpu image cause pc steps):']
        lines += ['  %s %08x %s %06x %d' % (cpu, image, reason, pc, n) for n, (cpu, image, reason, pc) in worst]
    return '\n'.join(lines), sum(gate.values())


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('census', nargs='+')
    ap.add_argument('--code-dir', default='extracted/code')
    ap.add_argument('--seeds-out', help='write/extend <dir>/<program>.txt seed files for gen_program.py')
    ap.add_argument('--check', action='store_true', help='exit 1 if any step ran in the interpreter for want of translation')
    a = ap.parse_args(argv)
    sizes = code_sizes(a.code_dir)
    total = merge(a.census)
    text, failures = report(total, sizes)
    print(text)
    if a.seeds_out:
        for prog, n in sorted(write_seeds(total, sizes, a.seeds_out).items()):
            print('seeds %-20s %d' % (prog, n))
    if a.check and failures:
        print('FAIL: %d PCs ran in the interpreter with cause not_translated or code_mismatch' % failures)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
