"""What a pacing trace says: did every picture get a refresh of its own, and which.

The renderer writes one line a picture when MPCVR_PACING_TRACE names a file (see
CPacingTrace in Source/DLSS/DlssTiming.h). This reads it and answers the questions
the statistics on the picture cannot: which refresh each picture reached the screen
on, how long after its own time, and whether the one before it had left.

    python pacing_report.py trace.csv [--skip 3] [--rows N] [--plot]
    python pacing_report.py trace.csv --dlss

--dlss says instead what the DLSS passes did: for every stretch between two notes,
how many of its pictures DLSS 5 NR and DLSS Super Resolution really ran on.

DXGI numbers every Present call and says, of the last picture shown, which call it
was and on which refresh. Read before and after every Present, that gives the
refresh of nearly every picture; the refresh times themselves come from the pairs
(refresh, performance counter) DXGI samples, fitted to a line -- which also measures
the screen's real period rather than the one it announces.

--skip leaves out the first seconds of each stretch, while the film settles.
"""

import io
import math
import sys

COLUMNS = ("n rtStart rtClockAtCall rtClockAtReturn tickSample tickDrawn tickAfterVBlank tickPresentCall "
           "tickPresentReturn tickTarget renderAhead flags "
           "last0 count0 refresh0 sync0 qpc0 last1 count1 refresh1 sync1 qpc1").split()
# What the trace gained later; a file written before has none of them, and reads as zeros.
MORE = "nrWaitUs nrRecordUs nrGpuUs".split()

HELD, VBLANK, FIELD2, NOT_PRESENTED, VP, RTX_HDR, HDR, EXCLUSIVE, SCHEDULED, REPEATED, DROPPED = (
    1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024)
DLSS_NR, NR_FLOW, NR_DETECTOR, NR_VECTORS, DLSS_SR, SKIPPED = 2048, 4096, 8192, 16384, 32768, 65536


def read(path):
    """Stretches of pictures, each under the notes that came before it."""
    qpf, stretches, notes, rows = 10000000.0, [], [], []
    for line in io.open(path, encoding="utf-8", errors="replace"):
        line = line.strip()
        if not line:
            continue
        if line.startswith("#"):
            text = line[1:].strip()
            if "ticks per second" in text:
                qpf = float(text.split(",")[-1].split()[0])
            elif text.startswith("n,"):
                pass
            else:
                if rows:
                    stretches.append((notes, rows))
                    notes, rows = [], []
                notes.append(text)
            continue
        values = line.split(",")
        if not len(COLUMNS) <= len(values) <= len(COLUMNS) + len(MORE):
            continue
        row = dict.fromkeys(MORE, 0)
        row.update(zip(COLUMNS + MORE, (int(v) for v in values)))
        rows.append(row)
    if rows:
        stretches.append((notes, rows))
    return qpf, stretches


def fit_line(xs, ys):
    """Least squares y = a + b x, robust to nothing: the caller filters."""
    n = len(xs)
    mx, my = sum(xs) / n, sum(ys) / n
    sxx = sum((x - mx) ** 2 for x in xs)
    if sxx == 0:
        return my, 0.0
    b = sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / sxx
    return my - b * mx, b


def stats(values):
    if not values:
        return "none"
    v = sorted(values)
    mean = sum(v) / len(v)
    return "min %+7.2f  median %+7.2f  mean %+7.2f  max %+7.2f" % (v[0], v[len(v) // 2], mean, v[-1])


def unwrap32(values):
    """DXGI's counters are 32 bits and a film is short, but be safe."""
    out, base, last = [], 0, None
    for v in values:
        if last is not None and v < last - (1 << 31):
            base += 1 << 32
        last = v
        out.append(v + base)
    return out


def report(qpf, notes, rows, skip_s, max_rows, plot):
    for note in notes:
        print("  " + note)
    ms = 1000.0 / qpf
    presented = [r for r in rows if not r["flags"] & NOT_PRESENTED and r["tickPresentCall"]]
    if len(presented) < 20:
        print("  %d picture(s), too few to say anything\n" % len(presented))
        return
    t0 = presented[0]["tickPresentCall"]
    presented = [r for r in presented if (r["tickPresentCall"] - t0) / qpf >= skip_s]
    left_out = sum(1 for r in rows if r["flags"] & NOT_PRESENTED)
    if len(presented) < 20:
        print("  nothing left after the first %.0f s\n" % skip_s)
        return

    # The screen's refreshes: every (refresh, counter) pair DXGI gave, on a line.
    pairs = {}
    for r in rows:
        for s in ("0", "1"):
            if r["qpc" + s]:
                pairs[r["sync" + s]] = r["qpc" + s]
    if len(pairs) < 10:
        print("  DXGI gave no refresh times: nothing can be said about the screen\n")
        return
    xs = sorted(pairs)
    ys = [pairs[x] for x in xs]
    a, period = fit_line(xs, ys)
    # Throw out what is more than a quarter of a period off the line, and fit again:
    # a mode change leaves a few samples from the old one.
    keep = [(x, y) for x, y in zip(xs, ys) if abs(y - (a + period * x)) < 0.25 * period]
    if len(keep) >= 10:
        a, period = fit_line([k[0] for k in keep], [k[1] for k in keep])
    residual = [abs(y - (a + period * x)) * ms for x, y in keep] or [0.0]
    hz = qpf / period if period else 0.0

    # Which Present call reached the screen on which refresh.
    shown = {}
    for r in rows:
        for s in ("0", "1"):
            if r["qpc" + s] and r["count" + s] not in shown:
                shown[r["count" + s]] = r["refresh" + s]

    def refresh_time(index):
        return a + period * index

    def refresh_at(tick):
        return (tick - a) / period

    # From every picture that came, shown or not: a film that loses one picture in two
    # is not a film at half the rate.
    frame = [(rows[i + 1]["rtStart"] - rows[i]["rtStart"]) / 10000.0 for i in range(len(rows) - 1)]
    frame_ms = sorted(frame)[len(frame) // 2]
    seconds = (presented[-1]["tickPresentCall"] - presented[0]["tickPresentCall"]) / qpf

    # The reference clock against the performance counter, and so against the screen.
    cx = [r["tickPresentCall"] for r in presented if r["rtClockAtCall"]]
    cy = [r["rtClockAtCall"] for r in presented if r["rtClockAtCall"]]
    clock_rate = 1.0
    if len(cx) > 10:
        _, slope = fit_line(cx, cy)
        clock_rate = slope * qpf / 1e7
    film_period_ticks = frame_ms / 1000.0 * qpf / clock_rate if clock_rate else 0.0

    dropped = [r for r in rows if r["flags"] & DROPPED]
    skipped = [r for r in rows if r["flags"] & SKIPPED]
    repeated = [r for r in rows if r["flags"] & REPEATED]
    print("  %d pictures presented in %.1f s, %d left out to catch up; the schedule dropped %d, left %d out undrawn and repeated %d"
          % (len(presented), seconds, left_out, len(dropped), len(skipped), len(repeated)))
    first = rows[0]["tickSample"]
    for what, which in (("left out", [r for r in rows if r["flags"] & NOT_PRESENTED]), ("dropped", dropped),
                        ("left out undrawn", skipped), ("repeated", repeated)):
        if which:
            when = [(r["tickSample"] - first) / qpf for r in which]
            print("     %s at (s from the stretch's first picture): %s%s"
                  % (what, " ".join("%.1f" % w for w in when[:40]), " ..." if len(when) > 40 else ""))
    print("  film %.4f ms a frame (%.4f fps); screen %.4f ms a refresh (%.4f Hz), DXGI's times within %.3f ms of that line"
          % (frame_ms, 1000.0 / frame_ms if frame_ms else 0, period * ms, hz, max(residual)))
    print("  reference clock runs at %.6f of the performance counter (%+.1f ppm)" % (clock_rate, (clock_rate - 1) * 1e6))
    if period and film_period_ticks:
        beat = (film_period_ticks - period) / period   # refreshes gained per frame
        if abs(beat) > 1e-9 and expected_step(frame_ms, period * ms) == 1:
            print("  one frame is %+.4f ms longer than one refresh: the film slides a whole refresh every %.0f s"
                  % ((film_period_ticks - period) * ms, abs(1.0 / beat) * frame_ms / 1000.0))

    flags = presented[len(presented) // 2]["flags"]
    print("  how: %s%s%s%s%s%s" % (
        "video processor" if flags & VP else "shaders",
        ", RTX Video HDR" if flags & RTX_HDR else "",
        ", HDR output" if flags & HDR else ", SDR output",
        ", exclusive" if flags & EXCLUSIVE else "",
        ", wait for vblank" if flags & VBLANK else "",
        ", held by render ahead" if flags & HELD else (", scheduled" if flags & SCHEDULED else "")))

    sync, onscreen, latency, phase, draw, vwait, block, lead, wait = [], [], [], [], [], [], [], [], []
    refreshes, unknown = [], 0
    timeline = []
    for r in presented:
        call = r["tickPresentCall"]
        sync.append((r["rtClockAtReturn"] - r["rtStart"]) / 10000.0)
        draw.append((r["tickDrawn"] - r["tickSample"]) * ms)
        block.append((r["tickPresentReturn"] - call) * ms)
        if r["tickAfterVBlank"]:
            vwait.append((r["tickAfterVBlank"] - r["tickDrawn"]) * ms)
        at = refresh_at(call)
        phase.append((at - math.floor(at)) * period * ms)
        # How long before its own time the sample was taken.
        if r["rtClockAtCall"]:
            clock_at_sample = r["rtClockAtCall"] - (call - r["tickSample"]) / qpf * 1e7 * clock_rate
            lead.append((r["rtStart"] - clock_at_sample) / 10000.0)
        index = shown.get(r["last1"])
        if index is None:
            unknown += 1
            refreshes.append(None)
            timeline.append((r, None, None, None))
            continue
        refreshes.append(index)
        lat = index - math.floor(at)
        latency.append(lat)
        wait.append((refresh_time(index) - call) * ms)
        on = None
        if r["rtClockAtCall"]:
            clock_then = r["rtClockAtCall"] + (refresh_time(index) - call) / qpf * 1e7 * clock_rate
            on = (clock_then - r["rtStart"]) / 10000.0
            onscreen.append(on)
        timeline.append((r, index, lat, on))

    # The cadence: one refresh more than the picture before, or not.
    steps = {}
    repeats = drops = 0
    last = None
    pending = 0   # pictures since the last one whose refresh is known
    events = []
    for (r, index, lat, on) in timeline:
        if index is None:
            pending += 1
            continue
        if last is not None:
            step = index - last
            per = step / float(pending + 1)
            steps[step if pending == 0 else "%d over %d" % (step, pending + 1)] = steps.get(
                step if pending == 0 else "%d over %d" % (step, pending + 1), 0) + 1
            # Only a film with a whole number of refreshes per frame has a step to miss.
            if pending == 0 and expected_step(frame_ms, period * ms) and step != expected_step(frame_ms, period * ms):
                events.append(((r["tickPresentCall"] - presented[0]["tickPresentCall"]) / qpf, step))
        last, pending = index, 0
    one = expected_step(frame_ms, period * ms)
    for when, step in events:
        if step > one:
            repeats += 1
        else:
            drops += 1

    print("  refresh each picture was shown on, against the one before: %s"
          % ", ".join("%s: %d" % (k, v) for k, v in sorted(steps.items(), key=lambda kv: str(kv[0]))))
    if one:
        print("  -> a picture every %d refresh(es) expected: %d shown a refresh too long, %d a refresh too short or not at all, %d unknown"
              % (one, repeats, drops, unknown))
    if events:
        print("     at (s): " + " ".join("%.1f%s" % (w, "+" if s > one else "-") for w, s in events[:40])
              + (" ..." if len(events) > 40 else ""))
    print("  Sync offset (clock when Present returns, less the picture's time), ms : " + stats(sync))
    print("  on the screen (its refresh, less the picture's time), ms             : " + stats(onscreen))
    if latency:
        counts = {}
        for l in latency:
            counts[l] = counts.get(l, 0) + 1
        print("  refreshes between the Present call and the screen                   : "
              + ", ".join("%d: %d" % (k, v) for k, v in sorted(counts.items())))
    print("  from the Present call to the screen, ms                              : " + stats(wait))
    print("  Present called this long after a refresh, ms                         : " + stats(phase))
    print("  sample taken this long before its time, ms                           : " + stats(lead))
    print("  drawing, ms                                                          : " + stats(draw))
    if vwait:
        print("  waiting for the vertical blank, ms                                   : " + stats(vwait))
    print("  inside Present, ms                                                   : " + stats(block))

    if max_rows:
        print("\n      t(s)   sync  screen  refreshes  after-vblank  in-Present  queue  flags")
        stride = max(1, len(timeline) // max_rows)
        for i in range(0, len(timeline), stride):
            r, index, lat, on = timeline[i]
            at = refresh_at(r["tickPresentCall"])
            print("  %8.2f %+6.1f %7s %10s %13.1f %11.1f %6d  %s" % (
                (r["tickPresentCall"] - presented[0]["tickPresentCall"]) / qpf,
                (r["rtClockAtReturn"] - r["rtStart"]) / 10000.0,
                "%+.1f" % on if on is not None else "?",
                "%d" % lat if lat is not None else "?",
                (at - math.floor(at)) * period * ms,
                (r["tickPresentReturn"] - r["tickPresentCall"]) * ms,
                r["last1"] - r["count1"], r["flags"]))
    if plot:
        text_plot(timeline, presented, qpf)
    print()


def expected_step(frame_ms, period_ms):
    """Refreshes a frame lasts, when that is a whole number; 0 when it is not."""
    if not period_ms:
        return 0
    ratio = frame_ms / period_ms
    whole = round(ratio)
    return int(whole) if whole >= 1 and abs(ratio - whole) < 0.02 else 0


def text_plot(timeline, presented, qpf):
    """The on-screen offset over time, a column a second, for a terminal."""
    buckets = {}
    for r, index, lat, on in timeline:
        if on is None:
            continue
        s = int((r["tickPresentCall"] - presented[0]["tickPresentCall"]) / qpf)
        buckets.setdefault(s, []).append(on)
    if not buckets:
        return
    print("\n  on the screen, ms, second by second (lowest .. highest):")
    for s in sorted(buckets):
        v = buckets[s]
        print("  %4d s  %+7.1f .. %+7.1f" % (s, min(v), max(v)))


def dlss_timeline(qpf, stretches):
    """What the DLSS passes did, stretch by stretch: a line for what was said, a line
    for the pictures under it. A session that stops by itself shows as pictures that
    DLSS no longer ran on under a note that still asks for it."""
    first = None
    total = on_nr = on_sr = 0
    for notes, rows in stretches:
        for note in notes:
            print("  # " + note)
        ticks = [r["tickPresentCall"] or r["tickDrawn"] for r in rows]
        if first is None:
            first = ticks[0]
        nr = sum(1 for r in rows if r["flags"] & DLSS_NR)
        flow = sum(1 for r in rows if r["flags"] & NR_FLOW)
        detector = sum(1 for r in rows if r["flags"] & NR_DETECTOR)
        vectors = sum(1 for r in rows if r["flags"] & NR_VECTORS)
        sr = sum(1 for r in rows if r["flags"] & DLSS_SR)
        lost = sum(1 for r in rows if r["flags"] & (NOT_PRESENTED | DROPPED | SKIPPED))
        gaps = [(b - a) * 1000.0 / qpf for a, b in zip(ticks, ticks[1:])]
        total += len(rows)
        on_nr += nr
        on_sr += sr
        print("  %7.1f s to %7.1f s, film at %8.1f s: %5d picture(s), NR on %5d (flow %d, detector %d, vectors %d), "
              "SR on %5d, not shown %d, longest wait %.0f ms" % (
                  (ticks[0] - first) / qpf, (ticks[-1] - first) / qpf, rows[0]["rtStart"] / 1e7,
                  len(rows), nr, flow, detector, vectors, sr, lost, max(gaps) if gaps else 0.0))
        draws = sorted((r["tickDrawn"] - r["tickSample"]) * 1000.0 / qpf for r in rows if not r["flags"] & SKIPPED) or [0.0]
        line = "            drawing %.1f ms (median), %.1f at most" % (draws[len(draws) // 2], draws[-1])
        timed = [r for r in rows if r["flags"] & DLSS_NR and r["nrGpuUs"]]
        if timed:
            def median(key):
                return sorted(r[key] for r in timed)[len(timed) // 2] / 1000.0
            line += "; of which DLSS 5 NR waited %.1f ms for its input, recorded for %.1f and waited %.1f for the GPU" % (
                median("nrWaitUs"), median("nrRecordUs"), median("nrGpuUs"))
        print(line)
    print("%d picture(s) in %d stretch(es): DLSS 5 NR ran on %d, DLSS SR on %d" % (total, len(stretches), on_nr, on_sr))


def main():
    args = sys.argv[1:]
    skip, max_rows, plot = 3.0, 0, False
    if "--dlss" in args:
        args.remove("--dlss")
        if len(args) != 1:
            print(__doc__)
            return 2
        qpf, stretches = read(args[0])
        if not stretches:
            print("no picture in " + args[0])
            return 1
        dlss_timeline(qpf, stretches)
        return 0
    if "--skip" in args:
        at = args.index("--skip")
        skip = float(args[at + 1])
        del args[at:at + 2]
    if "--rows" in args:
        at = args.index("--rows")
        max_rows = int(args[at + 1])
        del args[at:at + 2]
    if "--plot" in args:
        plot = True
        args.remove("--plot")
    if len(args) != 1:
        print(__doc__)
        return 2
    qpf, stretches = read(args[0])
    if not stretches:
        print("no picture in " + args[0])
        return 1
    for i, (notes, rows) in enumerate(stretches):
        print("stretch %d of %d, %d line(s)" % (i + 1, len(stretches), len(rows)))
        report(qpf, notes, rows, skip, max_rows, plot)
    return 0


sys.exit(main())
