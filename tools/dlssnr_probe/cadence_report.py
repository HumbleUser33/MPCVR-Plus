"""What the screen showed, image by image: cadence_watch's file, read for the cadence.

    python cadence_report.py run.screen.csv [--fps 23.976] [--list N] [--trace run.csv]

cadence_watch writes a line for every image Windows put on the screen: when it went
there, how far the film's picture in it is from the pictures before (a grid of tiles
away from the statistics), and where the statistics' green marker was, which moves one
pixel for every picture the renderer draws. From that, for every new picture of the
film: on how many refreshes it stayed, against what the film's rate on that screen
wants -- 2 and 3 in turn for 23.976 fps at 60 Hz, 1 at 23.976 Hz.

What counts as a fault:
  short     a picture on the screen for fewer refreshes than the cadence allows (1 at 60 Hz)
  long      a picture on the screen for more (4 or more at 60 Hz, 2 or more at 24 Hz)
  slip      at 60 Hz, two pictures in a row held alike (2,2 or 3,3): the 3:2 cadence
            changing phase, which a screen at 60.000 Hz needs once in 16.7 s
  same      a new image whose film picture is the one before it again
  back      a new image whose film picture is the one before the one before (A B A)
  marker    the marker moved by other than one pixel: pictures drawn and not shown, or
            the marker missed

and a cut is a change of picture far larger than the film's usual one, so that each fault
can be placed against the last cut before it.
"""

import argparse
import io
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))


def read_screen(path):
    qpf, hz, rows, notes = 1e7, 60.0, [], []
    for line in io.open(path, encoding="utf-8", errors="replace"):
        line = line.strip()
        if not line:
            continue
        if line.startswith("#"):
            if "ticks per second" in line:
                parts = line[1:].split(",")
                qpf = float(parts[1].split()[0])
                hz = float(parts[2].split()[-2])
            elif line.startswith("# display now"):
                hz = float(line.split()[3])
                rows = []   # the images before the change belong to another mode
                notes.append(line[1:].strip())
            elif not line.startswith("# qpc"):
                notes.append(line[1:].strip())
            continue
        v = line.split(",")
        if len(v) < 11:
            continue
        rows.append(dict(qpc=int(v[0]), acquired=int(v[1]), acc=int(v[2]), marker=int(v[3]),
                         d=[float(x) for x in v[4:10]], mean=float(v[10])))
    return qpf, hz, rows, notes


def period_of(rows, nominal):
    """The refresh period, from the gaps between images: each is a whole number of refreshes."""
    period = nominal
    for _ in range(3):
        num = den = 0.0
        for a, b in zip(rows, rows[1:]):
            gap = b["qpc"] - a["qpc"]
            k = round(gap / period)
            if 1 <= k <= 8 and abs(gap - k * period) < 0.2 * period:
                num += gap
                den += k
        if den:
            period = num / den
    return period


def expected(fps, hz):
    r = hz / fps
    return r, (int(math.floor(r)), int(math.ceil(r)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("screen")
    ap.add_argument("--fps", type=float, default=24000.0 / 1001.0)
    ap.add_argument("--list", type=int, default=40)
    ap.add_argument("--hz", type=float, default=0.0, help="the screen's rate, when the file does not say it")
    ap.add_argument("--skip", type=float, default=2.0, help="seconds left out at the start")
    ap.add_argument("--cut", type=float, default=6.0, help="a cut is a change this many times the median")
    args = ap.parse_args()

    qpf, hz_nominal, rows, notes = read_screen(args.screen)
    for n in notes:
        print("  " + n)
    if rows and args.skip > 0:
        first = rows[0]["qpc"]
        rows = [r for r in rows if (r["qpc"] - first) / qpf >= args.skip]
    if len(rows) < 50:
        print("  %d images, too few" % len(rows))
        return
    if args.hz:
        hz_nominal = args.hz
    nominal = qpf / hz_nominal
    if hz_nominal in (23.0, 29.0, 59.0):
        nominal = qpf * 1001.0 / ((hz_nominal + 1) * 1000.0)
    period = period_of(rows, nominal)
    hz = qpf / period
    ms = 1000.0 / qpf

    # The refresh each image went up on, counted from the first.
    idx, jitter = [0], []
    for a, b in zip(rows, rows[1:]):
        gap = b["qpc"] - a["qpc"]
        k = round(gap / period)
        jitter.append(abs(gap - k * period) * ms)
        idx.append(idx[-1] + k)
    t0 = rows[0]["qpc"]
    seconds = (rows[-1]["qpc"] - t0) / qpf
    coalesced = sum(max(0, r["acc"] - 1) for r in rows)
    jitter.sort()

    # Which images bring a new film picture. The film's grain moves every tile a little
    # from one picture to the next; an image whose tiles did not move at all is the same
    # picture (a redraw, a repeat). The threshold sits well under the film's least change.
    d1 = [r["d"][0] for r in rows[1:] if r["d"][0] >= 0]
    moving = sorted(x for x in d1 if x > 0)
    median = moving[len(moving) // 2] if moving else 0.0
    same_eps = max(1e-6, 0.02 * median)
    cut_at = args.cut * median

    ratio, (lo, hi) = expected(args.fps, hz)
    whole = abs(ratio - round(ratio)) < 0.02
    print("  %d images in %.1f s; screen %.4f Hz (%.4f ms), image times within %.2f ms of it (p99 %.2f); %d images coalesced by the duplication"
          % (len(rows), seconds, hz, period * ms, jitter[-1] if jitter else 0, jitter[int(0.99 * len(jitter))] if jitter else 0, coalesced))
    print("  film %.3f fps: %.4f refreshes a picture, so %s" % (args.fps, ratio,
          ("%d each" % round(ratio)) if whole else ("%d and %d in turn" % (lo, hi))))

    # New pictures, and how long each stayed.
    pics = []   # (row index, refresh index, new, kind)
    events = []
    last_cut = None
    marker_moves = {}
    for i in range(1, len(rows)):
        r, p = rows[i], rows[i - 1]
        mk = None
        if r["marker"] >= 0 and p["marker"] >= 0:
            mk = p["marker"] - r["marker"]
            if mk < -50:      # wrapped to the right edge of the box
                mk = None
            if mk is not None:
                marker_moves[mk] = marker_moves.get(mk, 0) + 1
        t = (r["qpc"] - t0) / qpf
        d = r["d"]
        if d[0] > cut_at:
            last_cut = t
        if d[0] >= 0 and d[0] <= same_eps:
            events.append((t, "same", "image %d: the film picture of the image before (d %.2g)" % (i, d[0]), last_cut))
        elif d[1] >= 0 and d[1] < 0.35 * d[0] and d[0] > 4 * median:
            events.append((t, "back", "image %d: closer to two images back (d1 %.3g, d2 %.3g)" % (i, d[0], d[1]), last_cut))
        if mk is not None and mk != 1:
            events.append((t, "marker", "image %d: marker moved %d" % (i, mk), last_cut))

    # The cadence, on the images that brought a new picture.
    new_idx = [idx[0]] + [idx[i] for i in range(1, len(rows)) if not (rows[i]["d"][0] >= 0 and rows[i]["d"][0] <= same_eps)]
    new_t = [0.0] + [(rows[i]["qpc"] - t0) / qpf for i in range(1, len(rows)) if not (rows[i]["d"][0] >= 0 and rows[i]["d"][0] <= same_eps)]
    holds = [b - a for a, b in zip(new_idx, new_idx[1:])]
    counts = {}
    for h in holds:
        counts[h] = counts.get(h, 0) + 1
    print("  refreshes each film picture stayed: " + ", ".join("%d: %d" % kv for kv in sorted(counts.items())))
    print("  marker moves between images: " + ", ".join("%d: %d" % kv for kv in sorted(marker_moves.items())))

    # Place each hold among the cuts: cut times on the screen.
    cuts = [(rows[i]["qpc"] - t0) / qpf for i in range(1, len(rows)) if rows[i]["d"][0] > cut_at]

    def since_cut(t):
        before = [c for c in cuts if c <= t + 1e-9]
        return (t - before[-1]) if before else None

    for k, h in enumerate(holds):
        t = new_t[k]   # the picture that stayed h refreshes went up at new_t[k]
        if whole:
            if h < round(ratio):
                events.append((t, "short", "a picture stayed %d refresh(es)" % h, None))
            elif h > round(ratio):
                events.append((t, "long", "a picture stayed %d refreshes" % h, None))
        else:
            if h < lo:
                events.append((t, "short", "a picture stayed %d refresh(es)" % h, None))
            elif h > hi:
                events.append((t, "long", "a picture stayed %d refreshes" % h, None))
            elif k > 0 and h == holds[k - 1]:
                events.append((t, "slip", "two pictures in a row stayed %d" % h, None))

    kinds = {}
    for e in events:
        kinds[e[1]] = kinds.get(e[1], 0) + 1
    print("  cuts seen on the screen: %d (a change over %.1f times the median %.4f)" % (len(cuts), args.cut, median))
    print("  faults: " + (", ".join("%s %d" % kv for kv in sorted(kinds.items())) or "none"))
    near = [e for e in events if e[1] != "slip"]
    if near:
        within = sum(1 for e in near if since_cut(e[0]) is not None and since_cut(e[0]) < 0.5)
        print("  of the faults other than slips, %d of %d came within 0.5 s after a cut" % (within, len(near)))
    events.sort()
    for t, kind, text, _ in events[:args.list]:
        sc = since_cut(t)
        print("   %8.2f s  %-6s %s%s" % (t, kind, text, ("   (%.2f s after a cut)" % sc) if sc is not None and sc < 2.0 else ""))
    if len(events) > args.list:
        print("   ... %d more" % (len(events) - args.list))


if __name__ == "__main__":
    main()
