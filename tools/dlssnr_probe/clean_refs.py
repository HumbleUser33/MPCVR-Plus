r"""Real encodes for the artifact removal study, and FFmpeg's own filters on them.

    python clean_refs.py [--refs N]       make clean_refs\ (FFmpeg on the path)
    python clean_refs.py --cambi          score what the harness wrote with CAMBI

The harness's --tclean degrades the truth itself, with a model of an MPEG-2 intra
block. This makes the same truth go through the real encoders instead -- MPEG-2,
MPEG-4 part 2 as XviD writes it, x264, x265 at 8 and 10 bits, SVT-AV1 -- at the
strengths a starved source has, decodes them back, and runs FFmpeg's deblocking and
debanding filters on every decoded picture: spp, pp7, fspp, deblock, deband,
gradfun, and libplacebo's deband, the one cs_deband.hlsl is a translation of. These
are the anchors the shaders are weighed against (dlssnr_harness --tclean --study
--files clean_refs).

clean_refs\manifest.txt, one line each, tab-separated:
    case    <name> <W> <H> <bits> <truth.yuv> <decoded.yuv>
    anchor  <name> <label> <file.yuv>
Pictures are raw yuv420p (8 bits) or yuv420p10le (10 bits), BT.709 limited range.

--cambi reads clean_refs\out\cambi.txt, which the harness writes (<case> <label>
<file> <bits> per line), and gives each file Netflix's banding score, CAMBI (libvmaf,
no reference): 0 is no visible banding, 5 is a lot.
"""
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REFS = os.path.join(HERE, "upscale_refs")
OUT = os.path.join(HERE, "clean_refs")

ENCODES = [
    # name, codec arguments, container, bits
    ("mpeg2_q4", ["-c:v", "mpeg2video", "-qscale:v", "4", "-g", "1"], "m2v", 8),
    ("mpeg2_q8", ["-c:v", "mpeg2video", "-qscale:v", "8", "-g", "1"], "m2v", 8),
    ("mpeg2_q12", ["-c:v", "mpeg2video", "-qscale:v", "12", "-g", "1"], "m2v", 8),
    ("xvid_q4", ["-c:v", "mpeg4", "-vtag", "XVID", "-qscale:v", "4", "-g", "1"], "avi", 8),
    ("xvid_q8", ["-c:v", "mpeg4", "-vtag", "XVID", "-qscale:v", "8", "-g", "1"], "avi", 8),
    ("xvid_q12", ["-c:v", "mpeg4", "-vtag", "XVID", "-qscale:v", "12", "-g", "1"], "avi", 8),
    ("x264_crf32", ["-c:v", "libx264", "-preset", "medium", "-crf", "32"], "mkv", 8),
    ("x264_crf38", ["-c:v", "libx264", "-preset", "medium", "-crf", "38"], "mkv", 8),
    ("x264_crf44", ["-c:v", "libx264", "-preset", "medium", "-crf", "44"], "mkv", 8),
    ("x265_crf34", ["-c:v", "libx265", "-preset", "medium", "-crf", "34", "-x265-params", "log-level=error"], "mkv", 8),
    ("x265_crf40", ["-c:v", "libx265", "-preset", "medium", "-crf", "40", "-x265-params", "log-level=error"], "mkv", 8),
    ("x265_10b_crf36", ["-c:v", "libx265", "-preset", "medium", "-crf", "36", "-pix_fmt", "yuv420p10le",
                        "-x265-params", "log-level=error"], "mkv", 10),
    ("av1_crf50", ["-c:v", "libsvtav1", "-preset", "8", "-crf", "50"], "mkv", 8),
    ("av1_crf60", ["-c:v", "libsvtav1", "-preset", "8", "-crf", "60"], "mkv", 8),
]

# FFmpeg's filters on the 8-bit pictures. qp is the quantizer they assume, as ours.
# The deblocking ones on the block codecs without a loop filter, the debanding ones on
# the codecs that smooth their blocks themselves and leave bands instead.
QPS = [2, 4, 8]
DEBLOCK_ANCHORS = (
    [("spp3_%s_qp%d" % (m, q), "spp=quality=3:qp=%d:mode=%s" % (q, m)) for m in ("hard", "soft") for q in QPS]
    + [("spp6_%s_qp%d" % (m, q), "spp=quality=6:qp=%d:mode=%s" % (q, m)) for m in ("hard", "soft") for q in QPS]
    + [("pp7_qp%d" % q, "pp7=qp=%d" % q) for q in QPS]
    + [("fspp_qp%d" % q, "fspp=quality=4:qp=%d" % q) for q in QPS]
    + [("deblock_weak", "deblock=filter=weak:block=8"), ("deblock_strong", "deblock=filter=strong:block=8")]
)
DEBAND_ANCHORS = (
    [("deband", "deband"), ("deband_strong", "deband=1thr=0.04:2thr=0.04:3thr=0.04:range=24")]
    + [("gradfun", "gradfun"), ("gradfun_strong", "gradfun=strength=3.5:radius=24")]
)
# libplacebo's own deband, at its defaults and at the five levels of
# Source/Clean/CleanParams.h (iterations, threshold, radius, grain).
DEBAND_LEVELS = [(1, 2, 16, 2), (1, 3, 16, 4), (2, 4, 16, 4), (2, 6, 20, 6), (3, 8, 24, 8)]


def run(args, cwd=None):
    r = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, cwd=cwd)
    if r.returncode:
        sys.stderr.write(r.stderr.decode("utf-8", "replace")[-2000:])
        raise SystemExit("failed: " + " ".join(args))
    return r


def probe_size(path):
    r = run(["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries", "stream=width,height",
             "-of", "json", path])
    s = json.loads(r.stdout)["streams"][0]
    return s["width"], s["height"]


def raw_in(W, H, bits):
    return ["-f", "rawvideo", "-pix_fmt", "yuv420p10le" if bits == 10 else "yuv420p", "-s", "%dx%d" % (W, H),
            "-color_range", "tv", "-colorspace", "bt709", "-color_primaries", "bt709", "-color_trc", "bt709"]


def make():
    nrefs = int(sys.argv[sys.argv.index("--refs") + 1]) if "--refs" in sys.argv else 0
    os.makedirs(OUT, exist_ok=True)
    refs = sorted(f for f in os.listdir(REFS) if f.lower().endswith(".png")
                  and "BrownFox" not in f and "AV1" not in f)
    if nrefs:
        refs = refs[:nrefs]
    manifest = []
    for ref in refs:
        stem = os.path.splitext(ref)[0].replace(" ", "_")
        sw, sh = probe_size(os.path.join(REFS, ref))
        for W in (1920, 960):
            if sw < 2 * W:
                continue
            H = int(round(sh * W / sw / 2.0)) * 2
            name = "%s_%d" % (stem, W)
            truth = os.path.join(OUT, name + ".truth.yuv")
            run(["ffmpeg", "-v", "error", "-y", "-i", os.path.join(REFS, ref), "-vf",
                 "scale=%d:%d:flags=lanczos:out_color_matrix=bt709:out_range=tv,format=yuv420p" % (W, H),
                 "-f", "rawvideo", truth])
            for enc, args, ext, bits in ENCODES:
                packed = os.path.join(OUT, "%s.%s.%s" % (name, enc, ext))
                run(["ffmpeg", "-v", "error", "-y"] + raw_in(W, H, 8) + ["-i", truth] + args + [packed])
                dec = os.path.join(OUT, "%s.%s.yuv" % (name, enc))
                run(["ffmpeg", "-v", "error", "-y", "-i", packed, "-f", "rawvideo", "-pix_fmt",
                     "yuv420p10le" if bits == 10 else "yuv420p", dec])
                os.remove(packed)
                case = "%s.%s" % (name, enc)
                manifest.append("case\t%s\t%d\t%d\t%d\t%s\t%s" % (case, W, H, bits, os.path.basename(truth),
                                                                   os.path.basename(dec)))
                if bits != 8:
                    continue
                if enc.startswith(("mpeg2", "xvid")):
                    filters = list(DEBLOCK_ANCHORS)
                else:
                    filters = list(DEBAND_ANCHORS) + [
                        ("libplacebo_l%d" % (i + 1),
                         "libplacebo=deband=1:deband_iterations=%d:deband_threshold=%g:deband_radius=%g:deband_grain=%g"
                         ":format=yuv420p:colorspace=bt709:range=tv" % lv)
                        for i, lv in enumerate(DEBAND_LEVELS)]
                for label, vf in filters:
                    out = os.path.join(OUT, "%s.%s.%s.yuv" % (name, enc, label))
                    run(["ffmpeg", "-v", "error", "-y"] + raw_in(W, H, 8) + ["-i", dec, "-vf", vf,
                         "-f", "rawvideo", "-pix_fmt", "yuv420p", out])
                    manifest.append("anchor\t%s\t%s\t%s" % (case, label, os.path.basename(out)))
            print("  %s: %d x %d, %d encodes" % (name, W, H, len(ENCODES)))
    with open(os.path.join(OUT, "manifest.txt"), "w") as f:
        f.write("\n".join(manifest) + "\n")
    print("  manifest: %d lines" % len(manifest))


def cambi():
    listing = os.path.join(OUT, "out", "cambi.txt")
    rows = []
    for line in open(listing):
        p = line.rstrip("\n").split("\t")
        if len(p) < 6:
            continue
        case, label, path, W, H, bits = p[0], p[1], p[2], int(p[3]), int(p[4]), int(p[5])
        # Run where the log goes, so that its path needs no drive letter: a colon in a
        # filter's option is an escaping puzzle FFmpeg's graph parser does not need.
        here = os.path.join(OUT, "out")
        src = raw_in(W, H, bits) + ["-i", os.path.join(HERE, path)]
        run(["ffmpeg", "-v", "error", "-y"] + src + src + ["-lavfi",
             "[0:v][1:v]libvmaf=feature=name=cambi:log_fmt=json:log_path=cambi_tmp.json",
             "-f", "null", "-"], cwd=here)
        frames = json.load(open(os.path.join(here, "cambi_tmp.json")))["frames"]
        score = sum(f["metrics"]["cambi"] for f in frames) / len(frames)
        rows.append((case, label, score))
    labels = []
    for _, l, _ in rows:
        if l not in labels:
            labels.append(l)
    encoders = []
    for c, _, _ in rows:
        e = c.split(".", 1)[1]
        if e not in encoders:
            encoders.append(e)
    print("CAMBI (libvmaf, no reference), lower is less banding: each method's mean per encoder,")
    print("and over all of them (%d decoded pictures)." % len({c for c, _, _ in rows}))
    print("  %-24s" % "" + "".join(" %10s" % e[:10] for e in encoders) + " %10s" % "all")
    for l in labels:
        line = "  %-24s" % l
        for e in encoders:
            s = [v for c, ll, v in rows if ll == l and c.split(".", 1)[1] == e]
            line += " %10.3f" % (sum(s) / len(s)) if s else " %10s" % "-"
        s = [v for _, ll, v in rows if ll == l]
        print(line + " %10.3f" % (sum(s) / len(s)))


if __name__ == "__main__":
    if "--cambi" in sys.argv:
        cambi()
    else:
        make()
