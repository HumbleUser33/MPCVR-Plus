"""Do the batch files of this working folder have Windows line endings?

In a batch file with Unix endings cmd.exe looks for a label 512 bytes at a time and
loses one that the edge of a read cuts in two: a CALL that is not made, with one line
on stderr, or a GOTO that ends the script. .gitattributes has git write every *.cmd
and *.bat with Windows endings, but git only does so when it writes the file. A
working folder older than the rule keeps the endings it had, and a tool that rewrites
a batch file can put Unix ones back with nothing in `git status` to show for it.

    python tools\\batch_endings.py [--fix]

It reads the batch files git tracks and ends on 1 when one of them has a line that
does not end the Windows way. With --fix it gives those lines their carriage return,
which is what git would have written, and touches nothing else.
"""

import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def tracked(root=ROOT):
    """The batch files git tracks, from the root of the working folder."""
    try:
        out = subprocess.run(["git", "ls-files", "-z", "--", ":(icase)*.cmd", ":(icase)*.bat"],
                             cwd=root, capture_output=True)
    except OSError as e:
        sys.exit("git did not run: %s" % e)
    if out.returncode != 0:
        sys.exit("git ls-files failed:\n%s" % out.stderr.decode("utf-8", "replace"))
    return [p for p in out.stdout.decode("utf-8").split("\0") if p]


def faults(root=ROOT):
    """(path, lines without a carriage return, lines) of each batch file at fault."""
    found = []
    for path in tracked(root):
        full = os.path.join(root, path)
        if not os.path.isfile(full):
            continue
        data = open(full, "rb").read()
        lines = data.count(b"\n")
        bare = lines - data.count(b"\r\n")
        if bare:
            found.append((path, bare, lines))
    return found


def main():
    args = sys.argv[1:]
    if args not in ([], ["--fix"]):
        print(__doc__)
        return 2

    bad = faults()
    if bad and args:
        for path, bare, lines in bad:
            full = os.path.join(ROOT, path)
            data = open(full, "rb").read()
            open(full, "wb").write(data.replace(b"\r\n", b"\n").replace(b"\n", b"\r\n"))
            print("  %-36s %d of %d lines given their carriage return" % (path, bare, lines))
        bad = faults()

    if not bad:
        print("%d batch files, all with Windows line endings." % len(tracked()))
        return 0
    print("%d batch file(s) with Unix line endings, where cmd.exe can lose a label:" % len(bad))
    for path, bare, lines in bad:
        print("  %-36s %d of %d lines" % (path, bare, lines))
    print("python tools\\batch_endings.py --fix  gives them Windows ones.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
