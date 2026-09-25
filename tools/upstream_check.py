"""What a merge from upstream will cost, before anything is touched.

This fork sits on top of Aleksoid1978/VideoRenderer. It adds a great deal and
replaces almost nothing, so `git merge upstream/master` does most of the work by
itself; what it cannot do is tell you where upstream changed the *meaning* of code
this fork is grafted into. That is what this report is for.

    python tools\\upstream_check.py [--base <commit>] [--theirs upstream/master]

It reads the repository and writes nothing -- no fetch, no merge, no file. Run
`git fetch upstream` first; the report says how old the last fetch is.

Read it top to bottom:

  1. where we stand
  2. the upstream commits that touch a file this fork modified -- read these, one
     by one. Nothing automates this step, and it is the one that matters.
  3. the risk table: where their work and ours meet
  4. a trial merge: the conflicts you will get, without creating any
  5. resource identifiers used by both sides, which would compile in silence and
     load the wrong resource
  6. whether the generated files will need regenerating afterwards

The procedure around it is in MERGING-UPSTREAM.md.
"""

import os
import re
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Upstream writes its commit messages in Russian, which a console left on the Windows
# code page cannot encode: print what it can and never fall over a subject line.
for stream in (sys.stdout, sys.stderr):
    try:
        stream.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, OSError):
        pass

# Written by Shaders/mpv/mpv_shaders.py, between markers, so upstream touching them
# means regenerating rather than merging by hand.
GENERATED_HOSTS = ["Shaders/compile_shaders.cmd", "Source/res/MpcVideoRenderer.rc2"]
GENERATED_FILES = ["Source/Upscale/MpvShaderTables.h"]


def git(*args, **kwargs):
    out = subprocess.run(["git"] + list(args), cwd=ROOT, capture_output=True,
                         text=True, encoding="utf-8", errors="replace")
    if out.returncode != 0 and not kwargs.get("allow_fail"):
        sys.exit("git %s failed:\n%s" % (" ".join(args), out.stderr))
    return out.stdout


def rule(title):
    print("\n%s\n%s" % (title, "-" * len(title)))


def fetch_age():
    """How long ago upstream was last fetched, in days, or None."""
    for path in (".git/refs/remotes/upstream/master", ".git/FETCH_HEAD"):
        full = os.path.join(ROOT, path)
        if os.path.exists(full):
            return (time.time() - os.path.getmtime(full)) / 86400.0
    return None


def resource_ids(ref):
    """number -> symbol, from a revision's Source/resource.h."""
    text = git("show", "%s:Source/resource.h" % ref, allow_fail=True)
    ids = {}
    for m in re.finditer(r"^#define\s+(ID[A-Z]_\w+)\s+(\d+)", text, re.M):
        ids.setdefault(int(m.group(2)), m.group(1))
    return ids


def main():
    args = sys.argv[1:]
    theirs = "upstream/master"
    base = None
    for flag in ("--base", "--theirs"):
        if flag in args:
            i = args.index(flag)
            value = args[i + 1]
            if flag == "--base":
                base = value
            else:
                theirs = value
            del args[i:i + 2]

    ours = git("rev-parse", "--abbrev-ref", "HEAD").strip()
    if not git("rev-parse", "--verify", "--quiet", theirs, allow_fail=True).strip():
        sys.exit("%s is unknown here. Add the remote and fetch:\n"
                 "    git remote add upstream https://github.com/Aleksoid1978/VideoRenderer.git\n"
                 "    git fetch upstream" % theirs)
    base = base or git("merge-base", theirs, "HEAD").strip()

    # ---------------------------------------------------------------- 1 ------
    rule("1. Where we stand")
    behind, ahead = git("rev-list", "--left-right", "--count",
                        "%s...HEAD" % theirs).split()
    print("  branch          %s" % ours)
    print("  common point    %s" % git("log", "-1", "--format=%h %s", base).strip())
    print("  upstream is     %s commits ahead of that point (%s)" % (behind, theirs))
    print("  this fork is    %s commits ahead of it" % ahead)
    age = fetch_age()
    if age is None:
        print("  last fetch      unknown")
    elif age > 1:
        print("  last fetch      %.0f days ago -- run `git fetch upstream` first" % age)
    else:
        print("  last fetch      %.1f hours ago" % (age * 24))

    ourfiles = set()
    added = 0
    for line in git("diff", "--name-status", "%s..HEAD" % base).splitlines():
        parts = line.split("\t")
        if parts[0].startswith("M"):
            ourfiles.add(parts[-1])
        elif parts[0].startswith("A"):
            added += 1
    print("  this fork has   %d files of its own, and changes %d of upstream's"
          % (added, len(ourfiles)))

    # ---------------------------------------------------------------- 2 ------
    rule("2. Upstream commits to read (they touch a file this fork changed)")
    theirlog = git("log", "--reverse", "--format=%h\t%s", "%s..%s" % (base, theirs)).splitlines()
    must_read, skim = [], []
    for line in theirlog:
        sha, _, subject = line.partition("\t")
        touched = set(git("show", "--name-only", "--format=", sha).split())
        (must_read if touched & ourfiles else skim).append((sha, subject, touched & ourfiles))
    if not must_read:
        print("  none -- upstream has not been near this fork's code.")
    for sha, subject, touched in must_read:
        print("  %s  %s" % (sha, subject[:76]))
        print("             %s" % ", ".join(sorted(touched)))
    print("\n  (%d more commit(s) nowhere near this fork's files; skim them.)" % len(skim))

    # ---------------------------------------------------------------- 3 ------
    rule("3. Where their work meets ours")
    rows = []
    for f in sorted(ourfiles):
        stat = git("diff", "--numstat", "%s..%s" % (base, theirs), "--", f).split()
        theirlines = int(stat[0]) + int(stat[1]) if len(stat) >= 2 and stat[0].isdigit() else 0
        if not theirlines:
            continue
        hunks = git("diff", "-U0", "%s..HEAD" % base, "--", f).count("\n@@")
        rows.append((theirlines, hunks, f))
    if not rows:
        print("  no file is changed on both sides.")
    else:
        print("  %-42s %14s %12s" % ("file", "upstream lines", "our hunks"))
        for theirlines, hunks, f in sorted(rows, reverse=True):
            print("  %-42s %14d %12d" % (f[:42], theirlines, hunks))
        print("\n  The top of this table is where to look hardest: many lines from them")
        print("  crossing many contact points of ours is how a clean merge goes wrong.")

    # ---------------------------------------------------------------- 4 ------
    rule("4. Trial merge (nothing is written)")
    trial = git("merge-tree", base, "HEAD", theirs, allow_fail=True)
    if not trial.strip():
        print("  merge-tree said nothing -- check git's version.")
    else:
        conflicts, current = {}, None
        for line in trial.splitlines():
            if line.startswith("changed in both") or line.startswith("added in both"):
                current = None
            m = re.match(r"^\s+(?:our|their|base)\s+\d+ \w+ (\S+)$", line)
            if m:
                current = m.group(1)
            if line.startswith("+<<<<<<<") and current:
                conflicts[current] = conflicts.get(current, 0) + 1
        if not conflicts:
            print("  no conflict: the merge would apply on its own.")
        else:
            print("  %d conflict(s) to resolve by hand:" % sum(conflicts.values()))
            for f, n in sorted(conflicts.items()):
                print("    %-46s %d" % (f, n))
            print("\n  git rerere is %s; with it on, a conflict resolved once comes back"
                  % (git("config", "--get", "rerere.enabled", allow_fail=True).strip() or "off"))
            print("  resolved on its own next time.")

    # ---------------------------------------------------------------- 5 ------
    rule("5. Resource identifiers used by both sides")
    ourids, theirids = resource_ids("HEAD"), resource_ids(theirs)
    clashes = [(n, ourids[n], theirids[n]) for n in sorted(set(ourids) & set(theirids))
               if ourids[n] != theirids[n]]
    if not clashes:
        print("  none. This fork's identifiers live in its reserved ranges")
        print("  (see the head of Source/resource.h), out of upstream's reach.")
    else:
        print("  %d number(s) mean two different things. This compiles without a word" % len(clashes))
        print("  and loads the wrong resource at runtime -- renumber ours into the")
        print("  reserved ranges at the head of Source/resource.h:")
        for n, a, b in clashes:
            print("    %-6d ours %-32s theirs %s" % (n, a, b))

    # ---------------------------------------------------------------- 6 ------
    rule("6. Generated files")
    touched = set(git("diff", "--name-only", "%s..%s" % (base, theirs)).split())
    hit = [f for f in GENERATED_HOSTS + GENERATED_FILES if f in touched]
    if hit:
        print("  upstream changed %s." % ", ".join(hit))
        print("  After the merge, regenerate rather than merge by hand:")
        print("      python Shaders/mpv/mpv_shaders.py renderer")
        print("      Shaders\\compile_shaders.cmd")
    else:
        print("  upstream has not touched the files the shader generator writes into.")

    print("\nNext: MERGING-UPSTREAM.md, step 2.\n")


main()
