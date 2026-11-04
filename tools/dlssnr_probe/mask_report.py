"""Takes the machine out of a battery report: the working folder becomes <repo> and the user's
profile %USERPROFILE%, so that a report committed as the reference says nothing of whose disk
it was measured on. run_all.cmd runs it on the report it has just written.

    python mask_report.py run_all_report.txt

The report is rewritten in place, byte for byte apart from those paths: the tools write it in
the console's code page, with CRLF and the odd bare CR, and none of that is touched. Only text
that is not a measurement changes, the same way in every report, so two reports still compare
row for row."""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))


def variants(path):
    # A path as the tools and the drivers print it: backslashes, or forward slashes after the
    # drive, in the ANSI code page or in UTF-8 when it is not plain ASCII.
    out = set()
    for p in (path, path.replace("\\", "/")):
        for enc in ("mbcs", "utf-8"):
            try:
                out.add(p.encode(enc))
            except (UnicodeError, LookupError):
                pass
    return sorted(out, key=len, reverse=True)


def mask(data):
    rules = [(REPO, b"<repo>")]
    profile = os.environ.get("USERPROFILE")
    if profile:
        rules.append((profile, b"%USERPROFILE%"))
    machine = os.environ.get("COMPUTERNAME")
    for path, token in rules:
        for v in variants(path):
            # Either slash may follow the folder, and Windows takes the path in any case.
            data = re.sub(re.escape(v) + rb"(?=[\\/:'\")\s]|$)", token, data, flags=re.I)
    if machine:
        data = re.sub(rb"\b" + re.escape(machine.encode("ascii", "ignore")) + rb"\b", b"<pc>", data, flags=re.I)
    return data


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    path = sys.argv[1]
    raw = open(path, "rb").read()
    masked = mask(raw)
    if masked != raw:
        open(path, "wb").write(masked)
    print(f"{path}: the working folder and the user's profile masked")
    return 0


if __name__ == "__main__":
    sys.exit(main())
