"""The HDR curve's tune: its targets, from real HDR masters.

Reads what `playback_test --hdrtunescan` printed for each master (one log or several, each
master's report after a line `==== <name>`), each picture the tune measured: its key
(log-average light), its 10th and 90th percentiles. Prints, per master and for all of them as
one film, what the tune reads from a film (Source/HdrCurveTune.h, Summarise): the depth D,
median of ln(key / p10), how far the shadows sit under the picture's key; the key K, median
of the pictures' keys; the contrast C, median of ln(p90 / p10).

    python hdr_tune_targets.py <log>...

The targets are those of all the masters as one film. The masters are short pieces, a scene
or three each, so one of them alone can be as dark as a dark film: the bands are half the
interquartile range of the masters taken one at a time, not wide enough to take in the
furthest of them. A log whose name holds no master (a film under RTX Video HDR, say) is
printed against the targets.
"""

import math
import re
import statistics
import sys

PICTURE = re.compile(r"\|\s+\d+\s+[\d.]+ s\s+picture at\s+[\d.]+ s.*?: (\w+), black (\d+) %,"
                     r" p10 ([\d.]+), median ([\d.]+), p90 ([\d.]+), key ([\d.]+), peak (\d+)")
FILM = re.compile(r"==== (\S+)")


def read(paths):
    films = {}
    for path in paths:
        name = None
        with open(path, encoding="utf-8", errors="replace") as f:
            for line in f:
                line = line.replace("\x00", "")
                m = FILM.search(line)
                if m:
                    name = m.group(1)
                    films.setdefault(name, [])
                    continue
                m = PICTURE.search(line)
                if m:
                    if name is None:
                        name = path
                        films.setdefault(name, [])
                    p = {"p10": float(m.group(3)), "p50": float(m.group(4)), "p90": float(m.group(5)),
                         "key": float(m.group(6)), "peak": float(m.group(7))}
                    if p["p10"] > 0 and p["key"] > 0:
                        films[name].append(p)
    return films


def summarise(pictures):
    return {
        "n": len(pictures),
        "D": statistics.median(math.log(p["key"] / p["p10"]) for p in pictures),
        "K": statistics.median(p["key"] for p in pictures),
        "C": statistics.median(math.log(p["p90"] / p["p10"]) for p in pictures),
        "peak": sorted(p["peak"] for p in pictures)[-2] if len(pictures) > 1 else 0.0,
    }


def main():
    films = read(sys.argv[1:])
    masters = {n: p for n, p in films.items() if re.match(r"0\d_", n)}
    others = {n: p for n, p in films.items() if n not in masters}
    if not masters:
        print("no masters found")
        return 1
    print(f"{'':42} {'pics':>4} {'depth':>6} {'key':>7} {'contr':>6} {'peak':>6}")
    rows = {}
    for name, pictures in masters.items():
        s = summarise(pictures)
        rows[name] = s
        print(f"{name[:42]:42} {s['n']:4} {s['D']:6.2f} {s['K']:7.2f} {s['C']:6.2f} {s['peak']:6.0f}")
    t = summarise([p for pictures in masters.values() for p in pictures])
    print(f"{'the masters as one film: the targets':42} {t['n']:4} {t['D']:6.2f} {t['K']:7.2f} {t['C']:6.2f}")

    def band(values):
        q = statistics.quantiles(values, n=4)
        return (q[2] - q[0]) / 2
    depths = [r["D"] - t["D"] for r in rows.values()]
    keys = [math.log(r["K"] / t["K"]) for r in rows.values()]
    print(f"\ndepth: target {t['D']:.2f}, the masters {', '.join(f'{x:+.2f}' for x in sorted(depths))}, band {band(depths):.2f}")
    print(f"key:   target {t['K']:.2f} nits, the masters at ln {', '.join(f'{x:+.2f}' for x in sorted(keys))}, band {band(keys):.2f}")
    for name, pictures in others.items():
        s = summarise(pictures)
        print(f"\n{name}: {s['n']} pictures, depth {s['D']:.2f} ({s['D'] - t['D']:+.2f}), key {s['K']:.2f} nits"
              f" (ln {math.log(s['K'] / t['K']):+.2f}), contrast {s['C']:.2f}, peak {s['peak']:.0f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
