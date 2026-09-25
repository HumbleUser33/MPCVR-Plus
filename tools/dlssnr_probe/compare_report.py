"""What moved between two run_all.cmd reports.

"0 failures" is necessary and not sufficient: a merge can pass every check and still
cost half a decibel somewhere. This lines the two reports up row by row and prints
only what actually moved.

    python compare_report.py baseline\\run_all_report.txt run_all_report.txt [-t 0.1]

A suite prints the same method name once per reference and per case, so rows are
matched by step, name and rank -- the third "Catmull-Rom" of a step against the third
of the other report. Times are ignored: a number followed by "ms" says more about the
machine's mood than about the code. A row that exists on one side only is reported as
well, since a check that stopped running is the worst kind of pass.
"""

import io
import re
import sys

ROW = re.compile(r"^\s{2,}(\S.*?)\s\s+((?:-?\d+\.?\d*\s+)*-?\d+\.?\d*)(\s+\d+\.?\d*\s*ms)?\s*$")
STEP = re.compile(r"^\[STEP\] (.+?)(?: -- exit code \d+)?$")
VERDICT = re.compile(r"(\d+) (?:failure\(s\)|check\(s\) failed)")


def read(path):
    """(step, row name, rank) -> numbers, and the failures counted per step."""
    rows, verdicts, seen, step = {}, {}, {}, "(start)"
    for line in io.open(path, encoding="utf-8", errors="replace"):
        line = line.replace("\x00", "").rstrip()
        m = STEP.match(line.strip())
        if m:
            step = m.group(1)
            continue
        m = VERDICT.search(line)
        if m:
            verdicts[step] = verdicts.get(step, 0) + int(m.group(1))
            continue
        m = ROW.match(line)
        if m:
            numbers = [float(v) for v in m.group(2).split()]
            if not numbers:
                continue
            name = m.group(1).strip()
            rank = seen[(step, name)] = seen.get((step, name), 0) + 1
            rows[(step, name, rank)] = numbers
    return rows, verdicts


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("-")]
    tol = 0.1
    if "-t" in sys.argv:
        tol = float(sys.argv[sys.argv.index("-t") + 1])
    if len(args) != 2:
        print(__doc__)
        return 2

    old, oldv = read(args[0])
    new, newv = read(args[1])

    print("failures, by step")
    bad = False
    for step in sorted(set(oldv) | set(newv)):
        a, b = oldv.get(step), newv.get(step)
        if b is None:
            print("  %-44s MISSING from the new report" % step[:44])
            bad = True
        elif b:
            print("  %-44s %d failure(s)  (was %s)" % (step[:44], b, a))
            bad = True
    if not bad:
        print("  none anywhere, in either report.")

    print("\nrows that moved by more than %.2f" % tol)
    moved = missing = 0
    for key in sorted(old):
        step, name, rank = key
        if key not in new:
            missing += 1
            print("  %-24s %-24s #%-3d gone from the new report" % (step[:24], name[:24], rank))
            continue
        a, b = old[key], new[key]
        if len(a) != len(b):
            print("  %-24s %-24s #%-3d shape changed" % (step[:24], name[:24], rank))
            moved += 1
            continue
        drift = [(i, x, y) for i, (x, y) in enumerate(zip(a, b)) if abs(x - y) > tol]
        if drift:
            moved += 1
            print("  %-24s %-24s #%-3d %s" % (step[:24], name[:24], rank,
                  ", ".join("col%d %.3f -> %.3f" % (i + 1, x, y) for i, x, y in drift[:4])))
    added = len(set(new) - set(old))
    if not moved and not missing:
        print("  none. Every measurement landed where it did before.")
    print("\n%d row(s) compared, %d moved, %d gone, %d new" % (len(old), moved, missing, added))
    if missing:
        print("A row that stopped being measured is worse than one that moved: find out why.")
    return 1 if bad or missing else 0


sys.exit(main())
