"""A film clip's own motion, frame by frame, for dlssnr_harness --tsrclip.

    python clip_motion.py <folder> [--compare]

The folder holds the clip's frames as ffmpeg writes them (fNNNN.png). Each frame is
registered against the one before -- a translation, a gain and an offset, by
Gauss-Newton on the luma at the source's size -- and <folder>\\motion.txt gets a line a
frame, 'fNNNN.png dx dy': the content's displacement from the previous frame, source
pixels. Read-only on the frames; ffmpeg and ffprobe on PATH, and numpy.

What moves on its own is left out: at every step, the 16 x 16 blocks whose mean squared
difference passes 2.5 times the median block's. That matters more than anything else
here. On Cyrano (2026-10-15), whose wall weaves 0.48 pixel a frame on average behind
actors who walk, phase correlation over the whole frame -- this file's first way --
came out 0.13 pixel from it and left 21 % more difference on the still part; the same
registration with every pixel taken in, 0.34 pixel on the flow frames.

--compare also registers stand-ins of Optical Flow's input frames (halved, blurred,
8 bits) the ways cs_dlss_global_refine.hlsl could, and prints how far each lands from
the motion written. The shader's way came from that table.
"""

import glob
import os
import subprocess
import sys

import numpy as np

PAD = 8          # edge pixels around a frame, for the shifted reads
BLOCK = 16       # pixels a block, for leaving out what moves
ROBUST = 2.5     # a block past this times the median block's difference is left out


def read_frames(folder):
    files = sorted(glob.glob(os.path.join(folder, "f*.png")))
    if not files:
        sys.exit(f"{folder}: no fNNNN.png frames")
    first = int(os.path.basename(files[0])[1:5])
    probe = subprocess.run(["ffprobe", "-v", "error", "-show_entries", "stream=width,height", "-of", "csv=p=0", files[0]],
                           capture_output=True, text=True).stdout.strip().split(",")
    # Whole blocks of the halved frame.
    w, h = int(probe[0]) & ~(2 * BLOCK - 1), int(probe[1]) & ~(2 * BLOCK - 1)
    raw = subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-start_number", str(first),
                          "-i", os.path.join(folder, "f%04d.png"), "-vf", f"crop={w}:{h}:0:0,format=gray16le",
                          "-f", "rawvideo", "-"], capture_output=True).stdout
    frames = np.frombuffer(raw, np.uint16).reshape(-1, h, w)
    return [os.path.basename(f) for f in files[:len(frames)]], frames


def warp(padded, d, h, w):
    """The frame read at x + d, bilinear: a translation is four shifted slices."""
    ix, iy = int(np.floor(d[0])), int(np.floor(d[1]))
    fx, fy = float(d[0] - ix), float(d[1] - iy)
    ix, iy = max(-PAD + 1, min(PAD - 3, ix)), max(-PAD + 1, min(PAD - 3, iy))

    def at(ox, oy):
        return padded[PAD + iy + oy:PAD + iy + oy + h, PAD + ix + ox:PAD + ix + ox + w]
    return (at(0, 0) * (1 - fx) + at(1, 0) * fx) * (1 - fy) + (at(0, 1) * (1 - fx) + at(1, 1) * fx) * fy


def flow_frame(img):
    """A stand-in for Optical Flow's input at about 540 lines: halved, a [1 2 1] blur, 8 bits."""
    h, w = img.shape
    s = img.reshape(h // 2, 2, w // 2, 2).mean(axis=(1, 3))
    p = np.pad(s, 1, mode="edge")
    s = 0.25 * p[1:-1, :-2] + 0.5 * p[1:-1, 1:-1] + 0.25 * p[1:-1, 2:]
    p = np.pad(s, 1, mode="edge")
    s = 0.25 * p[:-2, 1:-1] + 0.5 * p[1:-1, 1:-1] + 0.25 * p[2:, 1:-1]
    return (np.round(s * 255.0) / 255.0).astype(np.float32)


def register(now, last, steps, brightness=True, robust=True, margin=8):
    """now(x) ~ gain * last(x + d) + offset; d, the content now at x was at x + d before.
    Returns d, gain, offset."""
    h, w = now.shape
    lp = np.pad(last, PAD, mode="edge")
    d = np.zeros(2)
    gain, off = 1.0, 0.0
    inner = np.zeros((h, w), np.float32)
    inner[margin:h - margin, margin:w - margin] = 1
    for _ in range(steps):
        lw = warp(lp, (d[0] - 1, d[1] - 1), h + 2, w + 2)   # a pixel more each side, for the gradient
        L = lw[1:-1, 1:-1]
        gx = (lw[1:-1, 2:] - lw[1:-1, :-2]) * 0.5
        gy = (lw[2:, 1:-1] - lw[:-2, 1:-1]) * 0.5
        e = now - (gain * L + off)
        weight = inner
        if robust:
            r2 = (e * e * inner).reshape(h // BLOCK, BLOCK, w // BLOCK, BLOCK).mean(axis=(1, 3))
            keep = (r2 < ROBUST * np.median(r2[r2 > 0])).astype(np.float32)
            weight = np.repeat(np.repeat(keep, BLOCK, 0), BLOCK, 1) * inner
        cols = [gain * gx, gain * gy] + ([L, np.ones_like(L)] if brightness else [])
        k = len(cols)
        A = np.empty((k, k))
        b = np.empty(k)
        for i in range(k):
            wi = cols[i] * weight
            b[i] = float((wi * e).sum(dtype=np.float64))
            for j in range(i, k):
                A[i, j] = A[j, i] = float((wi * cols[j]).sum(dtype=np.float64))
        step = np.linalg.solve(A, b)
        d += step[:2]
        if brightness:
            gain += step[2]
            off += step[3]
    return d, gain, off


def still_mask(now, last, d, gain, off):
    """The blocks the registration kept: the picture's still part, for judging estimates."""
    h, w = now.shape
    e = now - (gain * warp(np.pad(last, PAD, mode="edge"), d, h, w) + off)
    r2 = (e * e).reshape(h // BLOCK, BLOCK, w // BLOCK, BLOCK).mean(axis=(1, 3))
    return np.repeat(np.repeat(r2 < ROBUST * np.median(r2), BLOCK, 0), BLOCK, 1)


def residual(now, last, d, mask):
    """Mean squared difference left on the mask by d, gain and offset fitted."""
    h, w = now.shape
    L = warp(np.pad(last, PAD, mode="edge"), d, h, w)[mask]
    N = now[mask]
    A = np.stack([L, np.ones_like(L)], 1).astype(np.float64)
    r = N - A @ np.linalg.lstsq(A, N.astype(np.float64), rcond=None)[0]
    return float((r * r).mean())


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    folder = sys.argv[1]
    compare = "--compare" in sys.argv[2:]
    names, frames = read_frames(folder)
    h, w = frames.shape[1:]

    # The flow-frame stand-ins, as the shader could register them: 3 steps from zero
    # (the shader starts from Optical Flow's median, nearer), in source pixels.
    ways = {
        "every pixel": dict(brightness=False, robust=False),
        "robust, 2 steps": dict(brightness=False, robust=True, steps=2),
        "robust, 3 steps": dict(brightness=False, robust=True),
        "robust, gain and offset": dict(brightness=True, robust=True),
    }
    lines, moves, gains = [], [], []
    errors = {k: [] for k in ways}
    residuals = {k: [] for k in ways}
    for i in range(1, len(frames)):
        now = frames[i].astype(np.float32) / 65535.0
        last = frames[i - 1].astype(np.float32) / 65535.0
        d, gain, off = register(now, last, 10)
        lines.append(f"{names[i]} {-d[0]:.4f} {-d[1]:.4f}")
        moves.append(np.hypot(*d))
        gains.append((gain, off))
        if compare:
            mask = still_mask(now, last, d, gain, off)
            base = residual(now, last, d, mask)
            fn, fl = flow_frame(now), flow_frame(last)
            for name, how in ways.items():
                how = dict(how)
                e = register(fn, fl, how.pop("steps", 3), margin=4, **how)[0] * 2
                errors[name].append(np.hypot(*(e - d)))
                residuals[name].append(residual(now, last, e, mask) / base)
        if i % 20 == 0:
            print(f"  {names[i]}: ({-d[0]:+.3f}, {-d[1]:+.3f})", flush=True)

    with open(os.path.join(folder, "motion.txt"), "w") as f:
        f.write("\n".join(lines) + "\n")
    moves = np.array(moves)
    gains = np.array(gains)
    print(f"{folder}: {len(lines)} frames, {w}x{h}: mean motion {moves.mean():.3f} px, median {np.median(moves):.3f}, "
          f"max {moves.max():.3f}, past 0.4: {100 * (moves > 0.4).mean():.0f} %; gain {gains[:, 0].min():.4f} to "
          f"{gains[:, 0].max():.4f}, offset {1000 * gains[:, 1].min():+.2f} to {1000 * gains[:, 1].max():+.2f} /1000")
    if compare:
        print("\nOn the flow-frame stand-ins, against the motion written: source pixels, and the difference")
        print("left on the still part relative to the motion written's.")
        print(f"  {'':26s} {'mean err':>9s} {'p90 err':>8s} {'residual':>9s}")
        for name in ways:
            e = np.array(errors[name])
            print(f"  {name:26s} {e.mean():9.3f} {np.percentile(e, 90):8.3f} {np.mean(residuals[name]):9.4f}")


if __name__ == "__main__":
    main()
