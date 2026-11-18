#   python make_hdr_scenes.py upscale_refs <out.mkv> [repeats]   (ffmpeg and ffprobe on PATH)
#
# An HDR10 film of known peaks, for the measured peak in the real filter: the four scenes of
# dlssnr_harness --ttonemap (tonemap_suite.inl) -- three film references halved from the middle
# to 1920x800, made linear (gamma 2.4), divided by their own 1-in-10 000 peak, below half of it
# kept diffuse up to 100 nits and above it climbing to the scene's peak (Expand), a grain of
# 1 % new each picture -- 1000, 4000, 300 and 1000 nits, 48, 72, 72 and 48 pictures, the whole
# repeated, at 23.976 pictures a second, PQ and BT.2020 as the file says, HEVC 10-bit.
import subprocess, sys
import numpy as np

REFS = sys.argv[1]                 # the upscale_refs folder, 4K1, 4K3 and 4K4 in it
OUT = sys.argv[2]                  # the .mkv to write
REPEATS = int(sys.argv[3]) if len(sys.argv) > 3 else 8
W, H = 1920, 800
SCENES = [(0, 1000.0, 48), (1, 4000.0, 72), (2, 300.0, 72), (0, 1000.0, 48)]
FILES = ['4K1.png', '4K3.png', '4K4.png']   # the three --ttonemap took


def load(path):
    probe = subprocess.run(['ffprobe', '-v', 'error', '-select_streams', 'v:0', '-show_entries',
                            'stream=width,height', '-of', 'csv=p=0', path], capture_output=True, text=True)
    w, h = (int(v) for v in probe.stdout.strip().split(','))
    raw = subprocess.run(['ffmpeg', '-v', 'error', '-i', path, '-f', 'rawvideo', '-pix_fmt', 'rgb48le', '-'],
                         capture_output=True).stdout
    img = np.frombuffer(raw, dtype='<u2').reshape(h, w, 3).astype(np.float64) / 65535.0
    oy, ox = (h - 2 * H) // 2, (w - 2 * W) // 2
    img = img[oy:oy + 2 * H, ox:ox + 2 * W]
    lin = np.clip(img, 0, 1) ** 2.4
    lin = lin.reshape(H, 2, W, 2, 3).mean(axis=(1, 3))
    m = lin.max(axis=2).ravel()
    k = m.size - 1 - m.size // 10000
    peak = max(np.partition(m, k)[k], 1e-3)
    return lin, peak


def expand(x, peak):
    return np.where(x <= 0.5, 200.0 * x, 100.0 + (peak - 100.0) * np.power(np.clip((x - 0.5) / 0.5, 0, 1), 2.4))


def pq(nits):
    m1, m2, c1, c2, c3 = 0.1593017578125, 78.84375, 0.8359375, 18.8515625, 18.6875
    y = np.clip(nits / 10000.0, 0, 1) ** m1
    return ((c1 + c2 * y) / (1 + c3 * y)) ** m2


pictures = [load(REFS + '\\' + f) for f in FILES]
rng = np.random.default_rng(1)
grains = [1.0 + 0.01 * rng.uniform(-1, 1, size=(H, W, 1)) for _ in range(16)]

enc = subprocess.Popen([
    'ffmpeg', '-v', 'error', '-y', '-f', 'rawvideo', '-pix_fmt', 'rgb48le', '-s', '%dx%d' % (W, H),
    '-framerate', '24000/1001', '-i', '-',
    '-vf', 'scale=out_color_matrix=bt2020nc:out_range=tv,format=yuv420p10le',
    '-c:v', 'libx265', '-preset', 'fast', '-crf', '14',
    '-x265-params', 'colorprim=bt2020:transfer=smpte2084:colormatrix=bt2020nc:range=limited:hdr10=1:max-cll=4000,400:'
                    'master-display=G(13250,34500)B(7500,3000)R(34000,16000)WP(15635,16450)L(40000000,50)',
    '-color_primaries', 'bt2020', '-color_trc', 'smpte2084', '-colorspace', 'bt2020nc', '-color_range', 'tv',
    OUT], stdin=subprocess.PIPE)

frame = 0
for r in range(REPEATS):
    for ref, peak, count in SCENES:
        lin, linPeak = pictures[ref]
        base = expand(np.minimum(lin / linPeak, 1.0), peak)
        for k in range(count):
            nits = base * grains[frame % len(grains)]
            code = np.round(pq(nits) * 65535.0).astype('<u2')
            enc.stdin.write(code.tobytes())
            frame += 1
enc.stdin.close()
enc.wait()
print('%d pictures, %d scenes, %.1f s -> %s' % (frame, REPEATS * len(SCENES), frame * 1001 / 24000.0, OUT))
