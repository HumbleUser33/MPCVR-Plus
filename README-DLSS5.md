# DLSS 5 Neural Rendering for MPC Video Renderer

A fork of [Aleksoid1978/VideoRenderer](https://github.com/Aleksoid1978/VideoRenderer) that adds
an optional NVIDIA **DLSS 5 Neural Rendering** pass (NGX feature 18) to the Direct3D 11
video pipeline, with a temporal stabilizer made for video, and an experimental option to
enlarge the picture with **DLSS Super Resolution** (NGX feature 1) instead of the resize
shaders.

Neither pass is a denoiser. Feature 18 **reconstructs the picture**: it rebuilds detail and
edges, and what it takes out of the grain and the compression noise is a consequence of that,
not the job it was given. DLSS Super Resolution, in the 4.5 DLLs this was built against,
rebuilds the picture as it enlarges it and comes out **cleaner than the source**: it takes
about 70 % of the film grain out over time, and on compressed film it lands above Catmull-Rom
on moving pictures. Which of the two you want, and whether that cleaner look suits your films,
is your call: both are optional and off by default.

The renderer stays a Direct3D 11 filter. Nothing about its existing pipeline changes when
the feature is off, and the rendering is bit-identical to upstream in that state.

---

## What you need

| | |
|---|---|
| GPU | NVIDIA RTX. Developed and tested on an RTX 3050 (Ampere), driver r596 |
| OS | Windows 10 1703 or later (shared fences), x64 only |
| Player | MPC-BE or any DirectShow player that lets you force a renderer |
| DLL | `nvngx_dlssnr.dll` — **not included, see below** |
| Optical Flow | `nvofapi64.dll`, installed with the NVIDIA driver (RTX 20 series and later). Optional: without it the stabilizer uses its shader detector |
| DLSS Super Resolution | Optional: `nvngx_dlss.dll` 310.5 or later (DLSS 4.5), from the public [NVIDIA/DLSS](https://github.com/NVIDIA/DLSS) repository (`lib/Windows_x86_64/rel`). Developed with 310.9.1 |

### The DLL is not in this repository

`nvngx_dlssnr.dll` is NVIDIA's property and is not redistributable, so it is not here and
will not be. At roughly 159 MB it also exceeds GitHub's per-file limit.

You supply it yourself. The filter looks for it, in order:

1. the path set in the property page, if any
2. next to `MpcVideoRenderer64.ax`
3. `<filter directory>\dlss\`
4. one and two directories above the filter

The **DLSS 5 NR** tab of the settings page has a **DLL** field and a browse button if you keep
it elsewhere.
`nvngx_dlss.dll`, for DLSS Super Resolution, is searched the same way and has its own field;
it must keep its name, because the driver's NGX runtime loads it by name from that folder.

---

## Building

```
git clone --recursive https://github.com/<you>/<repo>.git
```

If you forgot `--recursive`: `git submodule update --init --recursive`.

Requirements: **Visual Studio 2022** with the **C++ MFC for v143 build tools (x86 & x64)**
component. Upstream declares `UseOfMfc`, and MSBuild enforces it even though no source
actually includes MFC.

```
build_mpcvr.cmd NoWait
```

Outputs `_bin\Filter_x64\MpcVideoRenderer64.ax` and, beside it,
`_bin\Filter_x64\dlss\nvngx.dll` — a small shim of ours, described below. Register the
filter with `distrib\Install_MPCVR_64.cmd`.

---

## How it works

Four things had to be solved. All of them were settled by measurement, not documentation —
there is none for this feature.

**The D3D11 backend of the snippet is inert.** It exports a complete D3D11 surface —
`Init_Ext`, `CreateFeature`, `EvaluateFeature`, an `NGXCubinD3D11` class, NVAPI cubin calls —
and every one of them returns `FAIL_FeatureNotSupported`. During nine init attempts with
different argument shapes the snippet issued *zero* NVAPI queries, so it is not a driver
gap: it refuses before asking the driver anything, and before initialising its own logging.
The D3D12 path, on the same device in the same process, works.

So `CDlssNR` builds a **private D3D12 device** on the same adapter (matched by LUID). The
renderer keeps allocating its textures on D3D11; they carry an NT share handle and are
opened a second time on the D3D12 side. Synchronisation is CPU-side in both directions —
see `Source/DLSS/DlssNR.h` for why that is deliberate.

**The snippet refuses unknown callers.** It walks back to the calling module and rejects
anything whose path does not contain `nvngx.dll`, returning `0xBAD00002` and logging
"Not called from NGX runtime". `NgxShim/` builds a small `nvngx.dll` of forwarders that the
guarded entry points are called through. It must not be tail-call optimised, or the return
address is the caller's again and the check fails from inside the shim.

**The architecture check.** The snippet asks NVAPI for the GPU architecture in
`NGXCubinGeneric::SetGPUArch` and refuses anything below its own declared minimum
(`0x1B0`, Blackwell). `CDlssNR` hooks `NvAPI_GPU_GetArchInfo` with MinHook and reports the
minimum the snippet itself declares — nothing is invented. It has to be installed before
the snippet loads, because the value is read once during init and cached. The spoofed answer
goes only to calls made from `nvngx_dlssnr.dll` itself: every NGX snippet asks the same
question, and DLSS Super Resolution told it runs on Blackwell picks kernels an Ampere or Ada
card cannot run, writes nothing and removes the device.

**Video has no motion vectors and no depth.** The network runs colour-only at
`ScalingRatio 1.0`: `DLSSNR.Depth` is never set, and `DLSSNR.MVec` only when the stabilizer is
asked to pass its Optical Flow vectors on. Left unset, the DLL's own log confirms it runs
that way (`EvaluateFeature Color=... MVec=0000000000000000 Depth=0000000000000000`). Vectors
alone do not buy temporal stability — see the next section.

Other findings worth knowing: the SDK version must be **0x13** (the driver core rejects
anything newer with `FAIL_OutOfDate`), the driver's NGX core does not know feature 18 at all
so the snippet must be loaded directly, and the working set is about **500 MB of video
memory at 1080p**, growing with resolution.

---

## Temporal stabilizer

The network blends each frame with its own previous output (`CC_Control_History_Blend`,
`dlssnr_prev_output`). A game keeps that history aligned with motion vectors; video has
none, and the result is a luminance shimmer: on real pictures, plain DLSS shows about 1.5×
the low-frequency flicker of its own input. The temporal suites of the harness (below)
measured what this DLL does with its inputs:

- **Motion vectors do not realign the history.** Their convention could be found (pixels,
  current to previous, scale 1), but once the history is in use, correct, inverted and absent
  vectors measure the same. Given as `DLSSNR.MVec`, Optical Flow vectors cut the flicker only
  part of the way and change the rendering around moving objects. Depth changes nothing.
- **Left alone, the history does nothing measurable**: it measures the same as no history.
- **`DLSSNR.ControlMask` steadies, but it strips the effect.** It weighs the history per
  pixel, and with any mask bound only 10 % (mask at 1) to 36 % (mask at 0, a frozen picture)
  of the network's change to the picture is left (`--teffect`, on a photo). The first version
  of this stabilizer drove that mask from a motion detector; in the player it looked exactly
  like DLSS turned off. DLSS5-Reshade-AIO leaves the parameter unused as well.

So the stabilizer works **after** the network and never touches its inputs
(`Source/DLSS/DlssStabilizer.cpp`, `Shaders/d3d11/ps_dlss_stabilize.hlsl`):

1. **Only the effect is steadied.** The network's change to the picture, E = output − input,
   is filtered over time and added back to the current input. The video itself is never
   delayed, so a cut or fast motion cannot leave a ghost of the picture — at most of the
   enhancement.
2. **The history is clamped** to the range of E in the current 3×3 neighbourhood, ± 0.02, so a
   wrong motion estimate cannot drag old values far.
3. **Motion** comes from one of two sources, chosen on the page:
   - **NVIDIA Optical Flow** (default): forward and backward flow from the driver's
     `nvofapi64.dll`, computed at about 540 lines whatever the video (4×4 grid, medium
     quality). The history is moved along the vectors, so moving areas are steadied too. A
     vector is trusted according to its forward/backward consistency, its matching cost, and
     how well the previous input, moved along it, matches the current one; where trust falls,
     the network's output for that picture is used as it is.
   - **Shader detector**: the four passes of the first version (`ps_dlss_motion.hlsl`) at a
     quarter of the resolution, with a noise floor measured on the video, a dilation and an
     8-picture hold. It has no vectors, so it steadies only what stands still.

   If Optical Flow cannot start (a GPU older than RTX 20, an old driver) or keeps failing,
   the detector takes over; the statistics and the page's status line say which one runs.
4. **Optionally**, the Optical Flow vectors also go to the network as `DLSSNR.MVec`.

Only a new picture moves the history on. A redraw — paused video, the OSD, a change that
brings no new picture — is steadied again against the same history as its picture, so it
shows what changed without taking a second step.

Measured on a photo with grain and 8×8 block noise, 1920×1080 windows moving with known
motion (`--tstab`, default network settings, RTX 3050). Flicker is the frame-to-frame change
of blurred luma, compensated by the true motion, as a ratio to the input's; *effect* is how
much of plain DLSS's change to the picture is left:

| | Still | Slow pan | Pan | Moving object | Cut | Effect |
|---|---|---|---|---|---|---|
| Plain DLSS | 1.44× | 1.59× | 1.56× | 1.49× | 1.47× | 100 % |
| **Stabilizer, Optical Flow** | 1.06× | 1.11× | 1.14× | 1.09× | 1.06× | 100 % |
| Stabilizer, shader detector | 1.02× | 1.55× | 1.56× | 1.12× | 1.13× | 100 % |
| Stabilizer, true motion (the ceiling) | 1.02× | 1.04× | 1.04× | 1.04× | 1.02× | 100 % |
| Vectors to DLSS, no stabilizer | 1.22× | 1.29× | 1.29× | 1.22× | 1.23× | 97–100 % |
| Vectors to DLSS + stabilizer | 1.00× | 1.03× | 1.05× | 1.02× | 1.01× | 96–100 % |

With Optical Flow the stabilizer removes about 80 % of the flicker DLSS adds, moving content
included, and stays within 0.0005 of plain DLSS on average (0.0013 on edges and where the
object moved). Passing the vectors to the network as well steadies a little more, but the
network then renders differently: 0.0032 away from plain DLSS where the object moved, 2.5
times the stabilizer alone — which is why that box is off by default. A variant filtering the
whole output, TAA-style, was measured too and dropped: it steadies more by also smoothing the
video itself, its grain and its edges.

Checked in the harness (`--tstabport`): the filter's own class, with its shaders embedded as
in the filter, measures exactly what the reference passes measure — every sequence, every
motion source, vectors to DLSS included — and each of 285 redraws shows, bit for bit, what
its picture showed.

Cost on an RTX 3050 (`--tstabbench`, GPU time per picture):

| Working size | Optical Flow | of which flow and vectors | Shader detector |
|---|---|---|---|
| 1920×1080 | 2.8 ms | 2.2 ms | 0.8 ms |
| 3840×2160 | 5.5 ms | 3.0 ms | 2.9 ms |

The CPU side stays under half a millisecond: nothing waits for the GPU. Memory: about 95 MB
at 1080p — the history, two input copies and the result at the working size — and 375 MB at
2160p, plus the Optical Flow engine's own buffers. With the stabilizer at 0, nothing is
allocated and nothing runs.

The detector itself was validated against a perfect motion map on synthetic sequences
(`--tdetect`): no lag on fades and slow pans, no added smear around moving objects, and
repeated pictures (variable frame rates, 25p stored as 50p) handled like the perfect map.

---

## DLSS Super Resolution (experimental)

**Use DLSS SR 4.5 for upscaling**, in the **NVIDIA DLSS Super Resolution (experimental)**
box on the **Scaling** tab beside the other
enlargers, enlarges the picture with DLSS Super Resolution instead of the **Upscaling** method,
which is then greyed. It is independent of DLSS 5 NR: another DLL, another session, and it works with NR on
or off. When NR runs before upscaling, SR takes its output.

**Also run when no enlarging is needed (DLAA).** DLSS only enlarges, so a film already at the
screen's size -- 4K on a 4K screen -- never reached it at all. With this ticked it runs there
too, at the picture's own size, rebuilding it without changing its scale; `QualityModeFor`
asks NGX for `NGX_PERFQUALITY_DLAA` at a ratio of one, which is the mode games call DLAA. The
statistics line says so: `DLSS SR : 3840x2160->3840x2160 DLAA/M`. Off, the same film reads
`DLSS SR : initialised` and no pass runs.

It costs a whole DLSS pass at the screen's size, which is the most expensive place to put one.
Measured here on an RTX 3050, with `playback_test --dlaa --size WxH --window WxH`, per frame:

| picture | DLSS SR | optical flow | fits 41.7 ms (24 fps)? |
|---|---|---|---|
| 1280x720 | 12.6 ms | 3.3 ms | yes, sync -7.0 ms, 0 skipped |
| 1920x1080 | 14.0 ms | 2.7 ms | yes, sync -6.3 ms, 0 skipped |
| 3840x2160 | 70.0 ms | 4.5 ms | **no**, sync ran out to +4.3 s |

So on this card it is for 1080p and below, and 4K DLAA is out of reach; a faster card moves
that line. Render ahead cannot rescue it -- it hides latency, not a pass that takes longer
than a frame. The honest check is the statistics box: *skipped* staying at 0 and the sync
offset staying put.

**How it runs.** Unlike the NR snippet, this feature runs on Direct3D 11 through the display
driver's NGX runtime (`_nvngx.dll`), which loads `nvngx_dlss.dll` itself: no second device,
no shared textures, no waits (`Source/DLSS/DlssSR.cpp`). Every call into NGX runs in a
separate device context state, so NGX's compute bindings never reach the renderer's passes;
the harness checks that the renderer's own resize gives the same picture, bit for bit, before
and after an evaluation. Several things were settled by measurement: the runtime's
`NVSDK_NGX_D3D11_Init_Ext` works (its plain `Init` answers `OutOfDate` once a current snippet
is on the path, and `Init_ProjectID` with the documented argument order crashes), and its
`Shutdown1(device)` crashes where `Shutdown()` ends the session cleanly, leaves an NR session
in the same runtime working, and allows a new one.

**Inputs.** Video has no jitter and no depth: the input is the picture at its own size, the
depth a constant plane, and the motion vectors come from NVIDIA Optical Flow at about 540
lines, run on the very picture DLSS enlarges — after DLSS 5 NR when that runs — and cleaned up
as described below. A redraw gets no vectors. The quality mode follows the scale (Quality
below ×1.6, Balanced below ×1.85, Performance below ×2.5, Ultra Performance above) and DLSS
picks the model for it unless a preset is set: with 310.9.1, **M** at ×2, **L** at ×3, **K**
below.
Past ×4 DLSS stops there and the resize shaders do the rest; where the picture does not grow
on both axes, or no feature can be made for a size, the Upscaling method runs as before.

**Still backgrounds and the edges.** DLSS resamples its history along the vectors it is given,
and Optical Flow's raw vectors are wrong on a still, grained picture: most blocks by a fraction
of a pixel in every direction, some by several pixels, and worst along the frame's edges, where
a block has no neighbours on one side. A still background shimmered like heat haze, and the
grain stayed in a band along the edges, where vectors leaving the picture made DLSS drop its
history. The vectors now go through two passes before DLSS
(`Shaders/d3d11/cs_dlss_global_motion.hlsl`, then PASS 4 and PASS 3 of `ps_dlss_stabilize.hlsl`,
`CDlssStabilizer::ForDlssSR`):

- **the picture's global motion**: the median of the blocks that can be trusted — forward and
  backward flow agree, the match is good, the vector stays in the picture — exact to the
  engine's 1/32 of a pixel, measured on the GPU with nothing waiting for it. The median of all
  blocks is not enough: on heavy grain the flat areas, where only the grain changes, give a
  coherent wrong motion that moved it 0.6 to 4 pixels on four film frames out of six. Under 0.4
  pixels the global motion counts as none;
- **each block** keeps a motion of its own only if it lies more than 0.75 to 1.5 pixels from the
  global motion and the trusted blocks around it share it (see *Moving subjects* below);
  otherwise it takes the global motion. A block pointing out of the picture where the global
  motion does not is noise as well.

A still background thus gets exactly no motion, a pan exactly the pan, and what moves on its
own keeps its vector. `--tsrstill` measures it on six 4K film frames, each halved and grained
anew on every frame, then enlarged twice: 40 still frames, an object crossing them, a slow pan
(0.7 pixel a frame) and a pan (3.8 pixels a frame). *Shimmer* is how much the structures of a
still picture move from one frame to the next, *grain flicker* how much the grain left does,
both ×1000 on luma; PSNR in dB.

| | Light grain | | | | Old film grain | | | |
|---|---|---|---|---|---|---|---|---|
| Vectors | Shimmer | Grain flicker | Slow pan | Pan | Shimmer | Grain flicker | Slow pan | Pan |
| Raw (up to 1.2) | 1.44 | 1.61 | 48.4 | 48.3 | 3.05 | 3.51 | 42.6 | 42.7 |
| Snapping alone (first version) | 0.73 | 0.95 | 49.9 | 50.3 | 1.84 | 2.12 | 43.6 | 44.7 |
| **Now** | **0.74** | **0.92** | **50.2** | **50.3** | **1.81** | **2.13** | **43.9** | **44.9** |
| Exact (not available in playback) | 0.96 | 1.00 | 53.3 | 53.6 | 1.85 | 1.80 | 44.5 | 47.1 |

The shimmer falls to the level of exact vectors, or under it, and pans gain 1 to 2 dB. Along
the edges, the raw vectors were off by 8.5 pixels on light grain and 21 pixels on old film grain
in the outermost 4 pixels, and left 18 to 24 % more grain there than exact ones; now 0.3
pixel, and 2 to 4 % more grain. With exact vectors DLSS keeps a slight edge effect of its
own: on one film frame, a 32-pixel guard band — the picture mirrored on every side — took a
further 7 to 10 % off the grain flicker in the outermost band and left the grain as it was; not
worth 9 % more DLSS time, so there is none. The filter's passes match the same computed on the
CPU, on still, panned and zoomed pictures: the global motion exactly, the vectors to their
texture's precision, 0.016 pixel (`--tsrport`).

**Moving subjects.** What moves on its own kept Optical Flow's vector, and its error with it:
on a subject crossing a still background, the vectors DLSS got were off by 1.4 pixels (RMS) on
light grain and 2.2 on old film grain, and under heavy grain the engine also underestimates the
motion itself, by 0.4 to 0.8 pixel of 2.9, the same way on every block. The subject shimmered
as a still background did with the raw vectors. PASS 4 now cleans what keeps its own motion,
one block at a time:

- Optical Flow reads a flow frame boxed a little wider — 4×4 source pixels per flow pixel at
  1080p instead of 2×2 — which takes grain out of what it matches;
- each block climbs from its own vector to the motion the trusted blocks around it share: a
  mean shift over the 7×7 blocks around it, weighted by trust and by closeness to within a
  pixel, so that the background, a pixel or more away, never joins in. It keeps that motion if
  at least four blocks' worth of trust, and a tenth to a third of the window's, share it;
- where the picture has texture, the kept vector is searched again on the two flow frames, in
  steps of a half, a quarter and an eighth of a flow pixel, which undoes the engine's bias;
- then it is blended with the vector found one picture earlier where the content came from,
  when the two agree (0.3 for the new one).

| | Light grain | | | Old film grain | | |
|---|---|---|---|---|---|---|
| Vectors | Subject shimmer | Subject PSNR | Pan shimmer | Subject shimmer | Subject PSNR | Pan shimmer |
| Raw (up to 1.2) | 1.72 | 45.0 | 1.46 | 3.14 | 41.5 | 3.10 |
| Snapping alone (first version) | 1.78 | 44.8 | 0.73 | 3.20 | 41.7 | 1.35 |
| **Now** | **0.75** | **46.2** | **0.66** | **2.30** | **42.3** | **1.19** |
| Exact (not available in playback) | 0.29 | 45.7 | 0.14 | 0.93 | 42.3 | 0.49 |

*Shimmer* is the same measure taken on what moves, against the frame before at the place it
came from: the subject inside its outline, the pan away from the edge it enters by. The subject
shimmers 2.4 times less on light grain and 28 % less on old film grain than with the first
version, its PSNR reaches that of exact vectors, and its vectors are off by 0.9 and 1.8 pixels;
pans shimmer 10 % less and gain up to 0.3 dB. What it costs: on still pictures under old film
grain, DLSS keeps 3 % more grain than with the snapping alone (5 % in the outermost 8 pixels)
and the picture measures 0.3 dB lower; on light grain nothing changes, and the edges come out
slightly cleaner. A few false motions still survive on flat areas of a still picture, where any
offset matches. What was tried to take them out — weighing each block's say by its texture,
checking each kept vector against the frames, requiring the same motion two pictures running,
asking a larger share of the window — took moving subjects' motion away as well, or cost the
slow pan under heavy grain, and was left out. Subjects still shimmer more than with exact
motion, chiefly under heavy grain, where a block of low contrast cannot tell its own motion from
the grain's: hence *Experimental* on the option.

**Cost** on an RTX 3050 6 GB (`--tsr`, GPU time per frame):

| Source → output | Mode, preset | Time |
|---|---|---|
| 1920×1080 → 3840×2160 | Performance, M (default) | 23.3 ms |
| same | preset J / K / L | 10.1 / 11.2 / 31.6 ms |
| 1280×720 → 3840×2160 | Ultra Performance, L (default) | 17.2 ms |
| 1920×800 → 3840×1600 | Performance, M | 16.3 ms |
| 1920×1080 → 2560×1440 | Quality, K | 4.2 ms |

Add the vectors, 3.0 ms at 1080p in playback, and up to about 3.6 ms on a zoom, where every
block moves its own way and is searched again: Optical Flow both ways with the matching cost,
then the passes described above. The snapping alone took 2.2 ms; up to 1.2 the raw vectors took
1.3 ms, and nothing when DLSS 5 NR lent its own; DLSS SR now always makes its own.

**Quality — read this before turning it on.** `--tsrq` pans windows of six 4K film frames by
half a source pixel per frame, reduces them to the source, grains or compresses them per
frame, and measures every method against the reference it never saw. Averages over the six
frames (PSNR in dB; *grain* is the fine detail left in flat areas, the source keeps about 2.4):

| | Clean, still | Clean, moving | Grain left | Compressed, still | Compressed, moving |
|---|---|---|---|---|---|
| Catmull-Rom | **55.9** | **55.9** | 2.40 | **46.9** | **46.8** |
| Lanczos3 | 54.7 | 54.7 | 2.55 | 46.5 | 46.4 |
| **DLSS SR, Optical Flow vectors** | 51.8 | 52.7 | **0.68** | 46.5 | **48.3** |
| DLSS SR, no vectors | 51.8 | 49.2 | 0.45 | 46.5 | 45.3 |
| DLSS SR, exact motion (not available in playback) | 51.8 | 54.3 | 0.45 | 46.5 | 49.7 |

With the vectors a player can compute, DLSS SR stays **less faithful than Catmull-Rom on
clean film**, 3 to 4 dB below. On compressed film it is level with it on still pictures, 0.4 dB
below, and **1.5 dB above on moving ones**: where the source carries compression noise, the
reconstruction is worth more than the fidelity it costs. It also **takes about 70 % of the film
grain out** over time, and the grain it leaves flickers 3.5 times less (0.68 against 2.40).
That degraining and denoising is what DLSS 4.5 gives a film beyond the enlargement, and on a
grainy source it is what you will notice first.
The cleaned vectors gain 0.3 to 1.2 dB on still pictures and 2.7 to 3.0 dB on moving ones over
the raw vectors of 1.2 (50.6, 49.7, 0.73, 46.2 and 45.5 on the row above), and remove a little
more grain. Without jitter a still picture gives DLSS
nothing new, and Optical Flow is not precise enough for it to accumulate as much detail on a
pan as exact motion does: finer flow — the source size, a vector per pixel, the slowest search —
measured no better. The option is therefore off by default and marked experimental; your eyes
decide whether its cleaner look is worth it on your films.

---

## Upscaling: FSRCNNX, RAVU-zoom and ArtCNN

The main page's **Upscaling** and **Chroma upsampling** lists are ordered by what they measured,
best first, on 1080p film brought to 4K. Six of the entries enlarge one plane with a small
network instead of a filter kernel, the way mpv's prescalers do:

| Entry | What it is | 1080p→4K luma, RTX 3050 |
|---|---|---|
| **ArtCNN C4F16 DS** | ArtCNN C4F16 DS: four convolution layers in compute passes, doubles. Cleans grain and compression as it enlarges | 14.5 ms, 63 MB |
| **RAVU-zoom** | RAVU-Zoom-AR r3: trained edge-directed weights, to any size at once, anti-ringing inside the kernel | 3.9 ms, no extra texture |
| **FSRCNNX 16 AR** | FSRCNNX_x2_16-0-4-1, 16 feature maps, doubles, with anti-ringing | 18.0 ms, 190 MB |
| **FSRCNNX 8 AR** | FSRCNNX_x2_8-0-4-1, the same with 8, with anti-ringing | 5.9 ms, 95 MB |
| **FSRCNNX 16**, **FSRCNNX 8** | the same two without anti-ringing | the same |

The colour comes from Catmull-Rom, which enlarges the picture as usual; the network's luma
then replaces its own, by adding the difference to R, G and B alike, so chroma is untouched.
Where the picture grows by more than the doubling (720p on a 4K screen) the resize shaders
finish the job, and where it grows by less they reduce what the network doubled, as mpv does.
Below 1.3× FSRCNNX and ArtCNN do not run at all, and RAVU-zoom needs the picture to grow on
both axes; the statistics then name Catmull-Rom, which is what runs. They need Direct3D 11 at
feature level 11.0 (the passes are shader model 5, ArtCNN's in compute); on Direct3D 9, on
older hardware, and while DLSS Super Resolution is enlarging the picture, Catmull-Rom stands
in.

### Anti-ringing

A network asked to double a picture invents an edge steeper than the one it was given, and
overshoots past it — the bright fringe along a dark line. **AR** takes that back the way
libplacebo does inside its own kernels: the value is pulled towards the range the four source
samples around that point really cover, four fifths of the way. RAVU-zoom carries this in its
own kernel already, which is what the AR of its upstream name means; FSRCNNX and ArtCNN do not,
so the list offers the two FSRCNNX entries both ways and you can hear the difference in the
`halo` column below. It costs nothing measurable in time, about a seventh of a decibel of
detail on clean film, and two hundredths of the sharpness.

### What each one is worth: luma

Ten 4K film frames reduced by 2 or by 3, degraded the way a source is, then enlarged again
(`dlssnr_harness --tupscale`), plus the doom9 line-art test. PSNR on the most detailed quarter
of the picture, against Catmull-Rom; `halo` is how far the output overshoots the range the
reference really covers along its edges, so lower is better and 0 is a kernel that cannot ring.

| Method | 1080p→4K | grain | compressed | 720p→4K | line art | halo | time |
|---|---|---|---|---|---|---|---|
| **ArtCNN C4F16 DS** | −1.36 dB | **+1.37 dB** | **+0.38 dB** | +0.20 dB | **+2.92 dB** | 0.00028 | 14.5 ms |
| **RAVU-zoom** | +1.56 dB | +0.58 dB | +0.20 dB | +1.58 dB | +0.65 dB | **0.00002** | 3.9 ms |
| **FSRCNNX 16 AR** | +2.02 dB | +0.31 dB | −0.02 dB | +2.27 dB | +2.43 dB | 0.00005 | 18.0 ms |
| **FSRCNNX 8 AR** | +1.58 dB | +0.15 dB | −0.11 dB | +1.96 dB | +1.98 dB | 0.00005 | 5.9 ms |
| **FSRCNNX 16** | **+2.18 dB** | +0.23 dB | −0.12 dB | **+2.65 dB** | +1.76 dB | 0.00023 | 18.0 ms |
| **FSRCNNX 8** | +1.72 dB | +0.03 dB | −0.22 dB | +2.28 dB | +1.01 dB | 0.00020 | 5.9 ms |
| Catmull-Rom | 0 | 0 | 0 | 0 | 0 | 0.00000 | 0.9 ms |
| Lanczos2 | +0.04 dB | −0.02 dB | −0.01 dB | +0.05 dB | +0.01 dB | 0.00000 | 0.9 ms |
| Mitchell-Netravali | −1.98 dB | −0.16 dB | −0.15 dB | −1.50 dB | −0.40 dB | 0.00000 | 0.8 ms |
| Lanczos3 | −1.23 dB | −0.66 dB | −0.43 dB | −0.42 dB | −0.04 dB | 0.00005 | 1.2 ms |
| Jinc2m | −3.48 dB | −2.65 dB | −1.75 dB | −1.02 dB | +1.61 dB | 0.00039 | 2.6 ms |

How to read it. **A clean 1080p master** is where FSRCNNX 16 wins and ArtCNN DS loses, because
ArtCNN is a denoiser as much as an upscaler and a clean reference has nothing to clean: it
takes real detail with the noise. **A real film** — grain, and compression on top — is the
other way round: ArtCNN DS gains a decibel on everyone else, because it is the only one that
does not amplify what the encoder left behind (it keeps grain at 0.43 of the reference's, the
others push it to 2.2). **Line art** wants a network, any network, and Catmull-Rom is the floor.
**Jinc2m**, the filter's old default, is last on film everywhere and rings four times harder
than anything else; it earns its place only on drawn lines.

So: **ArtCNN C4F16 DS** if your films are grainy or heavily compressed and you have the 14 ms,
**RAVU-zoom** if you want most of the gain for a quarter of the time and no ringing at all,
**FSRCNNX 8 AR** if you want the sharpest edges per millisecond, **FSRCNNX 16 AR** if the time
does not matter. If you would rather keep the grain than have it cleaned, stay away from
ArtCNN DS and take FSRCNNX or RAVU-zoom.

### What each one is worth: chroma

**Chroma upsampling** has three entries beyond the kernels. **Jinc (EWA)** is the polar kernel
madVR and mpv call Jinc: jinc(d) windowed by a jinc, cut at 3.2383 — the third zero, mpv's
`ewa_lanczos`. It is not separable, so it reads a disc of about 32 texels around each point
rather than a row and a column; chroma sits at a fixed place among the luma pixels, so the
weights of the four possible positions are worked out when the conversion shader is generated
and the shader only adds texels up. **RAVU-zoom** and **FSRCNNX 8 AR** instead put Cb and Cr
through an mpv prescaler, each plane on its own and placed where the video's chroma siting puts
it (MPEG-2, co-sited or centred). All three want Direct3D 11 and 4:2:0 in planes; elsewhere —
4:2:2, the hardware video processor without the box below, Direct3D 9 — Catmull-Rom does it.

Measured through the filter itself, on six 1080p film frames played as 4:2:0 with the chroma
sited where video puts it and compared with the same frame in 4:4:4 (`playback_test --chroma`),
plus the doom9 line-art test. PSNR of Cb and Cr over the picture, then along the luma's edges,
where bleeding shows and where the eye looks:

| Method | Cb/Cr | at luma edges | whole picture | line art, at edges | what it costs |
|---|---|---|---|---|---|
| **Jinc (EWA)** | **+0.22 dB** | **+0.61 dB** | **+0.20 dB** | +0.25 dB | about 32 texels a pixel, in the conversion shader |
| **RAVU-zoom** | −0.05 dB | +0.47 dB | −0.08 dB | +1.89 dB | a pass of its own, 2.4 ms at 1080p |
| Catmull-Rom | 0 | 0 | 0 | 0 | 16 texels a pixel |
| **FSRCNNX 8 AR** | −0.32 dB | −0.21 dB | −0.35 dB | **+2.28 dB** | a pass of its own, 4.0 ms at 1080p |
| Bilinear | −0.29 dB | −0.73 dB | −0.26 dB | −1.55 dB | one bilinear fetch |
| Nearest-neighbor | −1.75 dB | −2.94 dB | −1.68 dB | −3.30 dB | one fetch |

Film and drawn lines want opposite things here, and the list is ordered for film, as asked.
**Jinc** is the one to take: it is the only method above Catmull-Rom on both the colour itself
and the colour along edges, it shifts nothing, and it stays inside the conversion shader, which
reads about thirty-two texels a pixel for it instead of Catmull-Rom's sixteen — it never cost a frame
in any of the tests here. **RAVU-zoom**
gains almost as much on edges but half a tenth of a decibel of overall colour, and leaves the
one-tenth-of-a-level offset every luma-trained network leaves on a chroma plane. **FSRCNNX 8
AR** is below Catmull-Rom on film — a network trained on luma has no business guessing colour
from a photograph — and the best of all of them on drawn lines by a wide margin, which is why
it is in the list at all: it is the entry for animation.

Two methods the study measured and the filter does **not** offer: **KrigBilateral** collapses
on line art (−6.7 dB) and is under Catmull-Rom on film; **CfL Prediction** wins only the
synthetic case where chroma is a linear function of luma, which no real film is. **NGU**, the
one that beats everything on the doom9 test, is closed.

The **defaults are unchanged**: Upscaling is Jinc2m and Chroma upsampling Catmull-Rom for a
fresh installation, and settings already saved in the registry are left alone — each entry
carries its own number, so reordering the lists moved nobody's setting.

The statistics show what they cost, next to the DLSS lines:

    Prescale (ms) : FSRCNNX 16 8.4, RAVU-zoom chroma 1.9

Only what runs is listed. When the hardware video processor handles the source format it
converts the picture itself, chroma included, so the **Chroma upsampling** list has nothing to
do and no chroma line appears; the processor line says so:

    VideoProcessor: D3D11 VP, output to R10G10B10A2_UNORM, converts chroma

The settings page greys a list that is out of service for that reason: **Chroma upsampling**
while the video processor converts, **Upscaling** and **Downscaling** while it also resizes
(*Use for resizing*, which DLSS 5 NR and DLSS SR suspend on their own), and **Upscaling**
while DLSS SR enlarges.

Every option carries a tooltip, and a greyed one carries it too -- that is the one worth
reading, since it says why the option is greyed.

It reads those from the settings and from nothing else, with one exception given below. The
renderer can also say what it is doing with the picture at hand, and the page used to listen;
that read well and worked badly. A 4K film on a 4K screen leaves the processor nothing to
resize, so the page moved under you as the film changed -- and it put *Request Super
Resolution* out of reach, because it greyed whatever the pre-pass box said, and that box is
the one it is supposed to answer. These are settings for every film there will ever be, so the
film playing now does not get a vote.

### The exception: Dolby Vision

Dolby Vision is not a matter of degree. The picture carries Dolby Vision's own colour and its
own reshaping, which a fixed-function video processor cannot convert, so the renderer hands
the whole chain to the shaders and the processor is not in it at all
(`DX11VideoProcessor.cpp`, `disableD3D11VP`). While that is what is playing, the page says so:
*Use the video processor for resizing* greys, with a line under it explaining why, and
**Request Super Resolution** greys with it -- that extension lives inside the processor.
**Chroma upsampling**, **Upscaling** and **Downscaling** stay live, because on such a picture
the shaders really are doing all three.

There is no way round it from here. The driver exposes Super Resolution as an extension of the
Direct3D 11 video processor and nowhere else, and a Dolby Vision picture cannot go through
that processor. What there is, for a source that carries an HDR10 base layer under the Dolby
Vision one -- profile 8.1 and the AV1 profile 10.1, which most releases use -- is a choice:
leave **Prefer Dolby Vision over PQ and HLG** off and the renderer ignores the Dolby Vision
layer, plays the HDR10 base layer, keeps the video processor, and Super Resolution works. Turn
it on and you get Dolby Vision's dynamic metadata and lose the processor with everything in
it. Profile 5 carries no base layer, so there the Dolby Vision path is the only one and the
choice does not arise.

*Use for resizing* decides the resizing and nothing else -- the chroma is the business of the
format boxes and of *Replace VP chroma upsampling*. What it also decides is whether
**Request Super Resolution** can do anything at all: that extension lives inside the video
processor and only acts while the processor is the one enlarging the picture, so it greys
without it, greys while a DLSS pass has taken the enlarging back, and greys while the chroma
pre-pass is on, because the processor is then handed a 4:4:4 picture the driver will not
touch. The driver is
still asked for it exactly as upstream asks -- nothing is taken away from it here -- but a
control that cannot change the picture should say so rather than look available, and the
statistics read `D3D11 (No Super Resolution when chroma by shaders)`.

**Replace VP chroma upsampling**, under the list, moves that one job to the shaders without
taking the picture away from the processor. A compute shader reads the planes of a progressive
4:2:0 or 4:2:2 frame, rebuilds Cb and Cr with the method above, and writes the result back in
4:4:4 — AYUV on an 8-bit source, Y410 on a 10-bit one — which the processor then converts and
resizes as it always did. It is the shader video processor's own conversion code, generated as
a compute shader instead of a pixel shader, so both paths stay the same picture.

Measured on a 4K film frame halved, played as 4:2:0 and as 4:4:4 through the same conversion
(`playback_test --chroma` and `--chroma10`): PSNR of Cb and Cr over the picture, then next to
the luma's edges where bleeding shows, and the constant shift of Cb and Cr in 8-bit levels.

| Source | Chroma | Cb/Cr | At luma edges | Colour shift |
|---|---|---|---|---|
| 8-bit (NV12) | the video processor | 52.25 | 48.69 | 0.07 level |
| | **Catmull-Rom before it** | **52.86** | **49.60** | **0.07 level** |
| | **RAVU-zoom before it** | **53.30** | **50.18** | **0.10 level** |
| | **Jinc before it** | **53.66** | **50.72** | **0.07 level** |
| | Catmull-Rom, shaders alone | 53.38 | 50.05 | 0.02 level |
| 10-bit (P010) | the video processor | 49.60 | 47.76 | 0.68 level |
| | **Catmull-Rom before it** | **55.24** | **51.68** | **0.05 level** |
| | **RAVU-zoom before it** | **55.96** | **52.75** | **0.08 level** |
| | **Jinc before it** | **56.61** | **53.51** | **0.06 level** |
| | Catmull-Rom, shaders alone | 55.71 | 51.85 | 0.01 level |

The processor's own chroma comes out between Nearest and Bilinear: 5.6 to 7.0 dB under the
shaders on a 10-bit film, 3.9 to 5.8 dB along its edges. On a 10-bit source it also
shifts the colour by about seven tenths of a level: its driver reads the studio range as
16/255 to 235/255 whatever the depth, while a 10-bit signal runs from 64/1023 to 940/1023. The
shaders read it at its own scale and write the 4:4:4 picture back on the scale the driver
expects, so that shift goes away as well — worth more here than the chroma itself. What is
left between the pre-pass and the shaders alone is the 8- or 10-bit picture handed over.

The processor keeps the picture, so what only it can do keeps applying, with one change each
way (`tools/dlssnr_probe` `vp444_probe`, which measures how much enabling an extension changes
the picture, in 8-bit levels):

- **RTX Video HDR** goes on working, exactly as it does without the pre-pass. The driver tone
  maps whatever the processor is given -- 37.1 on an 8-bit picture, 36.7 on a 10-bit 4:2:0 one,
  37.7 on a 4:4:4 one. An earlier version of this manual said the driver left a 10-bit 4:2:0
  frame alone; that was measured with the processor already told its input and output were PQ
  BT.2020, the one arrangement in which the extension does nothing (0.000), and the depth was
  never the reason. The renderer does not use that arrangement on a 10-bit source, and must
  not: see *Following upstream* for what happens when it does;
- **Super Resolution** does nothing to a 4:4:4 picture (0.000, against 4.0 on 4:2:0, with or
  without RTX Video HDR), so this box costs it. The driver is still asked for it exactly as
  upstream asks -- nothing is taken away from it here -- but *Request Super Resolution* greys
  while this pre-pass is in service, and the scaling line reads `D3D11 (No Super Resolution
  when chroma by shaders)` instead of starring work nobody did;
- **deinterlacing** works on NV12 only on this driver, so interlaced video keeps the
  processor's own chroma; 4:4:4 and RGB sources have no chroma to rebuild and are left alone;
- **HDR passthrough** is unaffected: the processor answers that it converts a PQ picture in
  AYUV, Y410 or Y416 to PQ RGB just as it does one in NV12 or P010 (`vp444_probe --hdrconv`).

The statistics say which is happening, and why when the option does not apply:

    VideoProcessor: D3D11 VP, output to R10G10B10A2_UNORM, chroma by shaders: Catmull-Rom
    VideoProcessor: D3D11 VP, output to B8G8R8A8_UNORM, converts chroma (interlaced)

The box needs Direct3D 11 and costs the chroma pass, a few tenths of a millisecond at 1080p
(RAVU-zoom, which enlarges Cb and Cr through its own shader first, costs about 3 ms).

Render ahead (below) covers them as well: with FSRCNNX 16 or ArtCNN a 4K picture takes more than the 8 ms
the renderer allows itself, and without it the picture would reach the screen late.

The shaders come from mpv's user-shader collections and are translated to HLSL once, offline:
`Shaders/mpv/mpv_shaders.py` runs each pass through glslang and SPIRV-Cross — the route
libplacebo itself takes on Direct3D 11 — and writes `Shaders/mpv/<shader>/passNN.hlsl`, the
lookup tables as half floats, the table `Source/Upscale/MpvShaderTables.h` and the generated
blocks of `compile_shaders.cmd` and `MpcVideoRenderer.rc2`. `Source/Upscale/MpvShader.cpp` runs
them the way libplacebo does: each pass renders — or, for ArtCNN, dispatches — into a texture
of the size its WIDTH and HEIGHT give, reading the plane, what earlier passes saved and the
tables. A compute pass writes through unordered access slot 0, the one slot feature level 11.0
always gives a compute shader. `--tmpvport` checks that this gives the harness's pictures
exactly, to the last half float, and that the chroma siting is applied.

---

## Sharpening

A sharpener restores nothing: it exaggerates. The whole question is *where it is allowed to*,
and the two offered here answer it differently. The pass runs after the resize and before the
dither, so it works whatever enlarged the picture -- the hardware processor, a prescaler, or
nothing at all on a film already at the screen's size, which is the case nothing else in this
filter can reach. Subtitles are drawn after it and are never sharpened.

**Adaptive-Sharpen** is bacondither's mpv shader (version 2021-10-17, BSD 2-clause), ported to
HLSL. It weighs each of 25 neighbours by how active its own area is, sharpens the blurred edges
most and the flat ones least, and limits the result with a tanh against the nearer local
extreme.

**Unsharp + Clamp** is a plain unsharp mask pulled back into the range its own 3x3 neighbourhood
really covers -- the principle of AviSynth's LimitedSharpenFaster, and the same clamp the
prescalers already use against the networks. At an edge that range spans the whole step, so the
step still gets steeper; what it cannot do any more is shoot past it, which is the halo.

### What each one is worth

Ten 4K film frames halved, degraded the way a source is, enlarged again, then sharpened and
scored against the reference none of them saw (`dlssnr_harness --tsharpen`). Every method is
read **at the strength that brings it to the same edge gradient**, because comparing each at
its own best compares six different amounts of sharpening and says nothing about the
sharpeners. `halo` is the mean overshoot past the range the reference really covers along its
edges, x1000; `grain` is the fine detail left in flat areas, output over reference.

| Method | psnr-d | halo | grain | grain source | compressed source | time, 4K |
|---|---|---|---|---|---|---|
| *no sharpening* | 43.93 | 0.048 | 3.698 | 39.53 | 37.97 | — |
| **Adaptive-Sharpen** | 44.93 | 0.076 | **3.597** | **39.37** | **37.92** | 3.05 ms |
| **Unsharp + Clamp** | 44.91 | **0.051** | 3.926 | 39.15 | 37.81 | 0.57 ms |
| unsharp, no clamp | 44.94 | 0.098 | 4.705 | 39.10 | 37.79 | 0.57 ms |
| CAS | 44.86 | 0.055 | 3.842 | 38.64 | 37.62 | 0.64 ms |
| CAS + the same clamp | 44.75 | 0.052 | 3.824 | 38.77 | 37.64 | 0.83 ms |
| RCAS (FSR 1) | 44.73 | 0.055 | 3.901 | 38.63 | 37.62 | 0.51 ms |

The two that ship are the two ends of one trade. **Adaptive-Sharpen is the only one that does
not amplify the grain** -- on a clean master it leaves *less* fine detail in flat areas than
doing nothing at all -- and on the two sources that matter, grain and compression, it is the
only one that costs almost nothing: 0.16 dB and 0.05 dB, where the next best costs 0.38 and
0.16. It is also five times the cost and rings half again as much on a clean picture.
**Unsharp + Clamp** has the lowest ringing of all, at the level of the unsharpened picture
itself, and is second everywhere for a fifth of the time.

Sharpening the colour as well as the luma was measured and dropped: it moves the result by less
than 0.3 dB and the sign turns with the source -- it helps a little on a clean master and hurts
on a compressed one. Both passes move the luma only, by adding one number to R, G and B alike,
which leaves B - Y and R - Y exactly as they were.

### The five levels

A level means the same amount of sharpening whichever method is chosen. The two are not on the
same scale -- and neither is on the scale the comparison above was made on, since a picture
halved and enlarged again is far softer than anything this renderer produces, and
Adaptive-Sharpen holds back where the edges are already steep. So the levels are calibrated
through the filter itself, on a 1080p film enlarged to 3200x1334, as the gain in mean gradient
each one really buys:

| Level | gain in detail | Adaptive-Sharpen | Unsharp + Clamp |
|---|---|---|---|
| 1 | +4 % | 0.22 | 0.45 |
| 2 | +8 % | 0.33 | 0.62 |
| **3** (default) | **+13 %** | **0.46** | **0.83** |
| 4 | +20 % | 0.68 | 1.17 |
| 5 | +28 % | 0.99 | 1.49 |

Measured back through the filter, level 1 gives +3.7 % and +2.8 %, level 3 +13.2 % and +13.0 %,
level 5 +28.3 % and +27.9 %. Level 3 is the +13 the quality measurements settle on; 4 and 5 are
past it, for a taste rather than for fidelity.

It adds to RTX Video Super Resolution and to DLSS, which sharpen in their own way; nothing here
prevents that, and the tooltip says so.

---

## Render ahead

The renderer wakes up 8 ms before a picture's time and only then processes it, so whatever
the DLSS passes take beyond those 8 ms used to reach the screen late. The renderer's own *Sync
offset* on an RTX 4060: −7 ms without DLSS, +11 to +17 ms with DLSS 5 NR, +11 to +19 ms with
DLSS 5 NR and DLSS SR — and DLSS SR's GPU time comes on top, since the GPU runs it after
Present has returned. The delay also varied by several ms from picture to picture, enough to
move some pictures to the next refresh of a 60 Hz screen.

**Render ahead to keep audio sync** (Settings page, on by default) measures each picture from
the start of its processing to the GPU being done with it, and starts the next pictures earlier
by the slowest of the last 32 plus 3 ms. Each picture then waits, while the GPU finishes it,
for the moment the renderer presents at without DLSS: half a refresh before its time, on the
reference clock. Presentation stays on the audio clock. It never waits for the GPU itself: a
picture not finished at its present time is presented anyway, reaches the screen once the GPU is
done, as it would have without render ahead, and moves the start earlier. It does nothing while
neither DLSS nor a luma prescaler runs, and a picture cannot start before the previous one has
been presented: about a frame earlier at most, never more than 60 ms.

Measured in a DirectShow graph with the filter itself (`playback_test.exe`): an 800×450 film in
a 1280×720 window on an RTX 3050 and a 60 Hz screen, with DLSS 5 NR and DLSS SR (NR 23 ms,
stabilizer 1.6 ms, SR 3.2 ms), 20 s per run. Sync offset, mean (5th…95th percentile), and
skipped pictures:

| Film | Render ahead off | Render ahead on |
|---|---|---|
| 23.976 fps | +15.1 ms (+14…+16), 0 skipped | **−6.4 ms (−7…−6)**, 0 skipped |
| 29.97 fps | +17.1 ms (+16…+19), 0 skipped | **−6.5 ms (−7…−6)**, 0 skipped |
| 59.94 fps, more than this GPU can do | +20.2 ms, 707 skipped | +19.9 ms, 707 skipped |

Without DLSS the same film measures −7.2 ms (−8…−6). A first version waited for the GPU before
presenting; at 29.97 fps that took the time from the next picture and skipped 25 of them in
20 s. At 29.97 fps most pictures are counted *late* below: the GPU finishes DLSS SR a moment
after the present time, because the source cannot hand over a picture earlier than a frame
minus its own time. Render ahead does not make the chain faster than the video: at 50 or 60 fps
with DLSS 5 NR pictures are still skipped, as they were.

The statistics show where the time goes and what render ahead does:

    DLSS (ms)     : NR 23.0, stabilizer 1.7, SR 3.3
    Render ahead  : 34 ms (ready in 29.8, max 30.6), late 10

*ready in* is the whole picture, from the start of processing to the GPU being done: the mean
and the slowest of the last 32. *late* counts pictures the GPU had not finished at their present
time. A few at start-up are normal; a count that keeps growing means the chain barely fits the
frame rate, and a lighter DLSS SR preset gives it room. DLSS 5 NR is timed where the renderer
waits for it, the other stages with GPU timestamps while the statistics are shown.
*Times(ms): Present* includes the wait.

### When the graph's clock stops

Every picture is scheduled against the reference clock the graph gets from one of its filters,
which is normally the audio renderer. The statistics name it:

    Frames        :   101, skipped: 0/0, failed: 0, turns late: 27 (turn +254 ms, clock +636 ms, base +0 ms)
    Clock         : MPC Audio Renderer

A clock that stops while the sound goes on playing — seen with one third-party audio renderer
after a very fast pause and play on a 24 Hz display — makes every picture look one frame
further into the future than the one before it, and the picture slows to a crawl while
everything else carries on. The renderer catches that and says so:

    Clock         : SaneAR Audio Renderer, stopped -- pictures paced by the stream

From then on it paces the pictures itself, one frame duration apart, until a picture's turn
arrives again, which is a clock that runs. *turns late* counts the pictures taken that way,
with how far away the turn still was, how far the clock moved since the last one, and how far
the filter's own time base moved with it; *turns lost* counts the rarer case of a turn
cancelled after it was given. All three stay at zero on a clock that keeps time, and the first
thing to try when they do not is another audio renderer.

---

## Settings

Everything lives on the **DLSS** page of the renderer's properties (x64 builds only) and is
stored under `HKCU\Software\MPC-BE Filters\MPC Video Renderer`. Render ahead is the exception:
it serves the luma prescalers as much as DLSS, so it sits on the **Settings** page, where the
32-bit builds reach it too. Its registry key, `DlssRenderAhead`, did not change.

| Setting | Default | Notes |
|---|---|---|
| Enable | off | Forces 16-bit float internal textures |
| Apply after upscaling | off | Runs at display resolution instead of source. Much heavier at 4K |
| Toggle key | F12 | Toggles during playback. The filter swallows this key |
| DLL | empty | Empty means search the locations listed above |
| Style | Default | Default / Natural / Cinematic. Applies live |
| Preset | 0 | **No effect with the DLL builds seen so far** — they ship a single network and every preset falls back to it |
| Auto mask | on | |
| Intensity | 1.50 | |
| Local tone | 0.30 | Low on purpose: the local terms amplify frame-to-frame variation |
| Local struct. | 0.50 | |
| Skin struct. | 0.90 | |
| Stabilizer | 100 | 0–100. Where the history is trusted, the current picture weighs from 1 down to 0.25 at 100, the measured setting. 0 runs nothing. Applies live |
| Motion | NVIDIA Optical Flow | Or *Shader detector (still areas)*. Applies on the next picture |
| Send the motion vectors to DLSS | off | Optical Flow only. Steadier, but the network renders differently around moving objects |
| Disable temporal history | off | Forces `DLSSNR.Reset` every frame. The stabilizer is not affected |
| Use DLSS SR 4.5 for upscaling | off | See above: not perfect yet, which is why the box it sits in says so. Greys the Upscaling list while it is on |
| Also run when no enlarging is needed (DLAA) | off | DLSS runs on a picture already at the screen's size, rebuilding it without enlarging it. A whole pass at the output size: 70 ms a frame at 4K on an RTX 3050, 14 ms at 1080p. Needs *Use DLSS SR* |
| Preset (DLSS SR) | Automatic | Or J, K, L, M. Applies on the next picture |
| DLL (DLSS SR) | empty | `nvngx_dlss.dll` or its folder. Empty means search next to the filter and up |

On the **Settings** page, bottom right:

| Setting | Default | Notes |
|---|---|---|
| Render ahead to keep audio sync | on | See above. Used while DLSS 5 NR, DLSS SR or a luma prescaler runs. Needs Direct3D 11 |

In the **Shader video processor** box, the two lists are ordered best first (see *Upscaling*
above) and each entry keeps its own number, so a setting already saved still means what it did:

| Setting | Default | What it does |
|---|---|---|
| Upscaling | Jinc2m | ArtCNN C4F16 DS, RAVU-zoom and the four FSRCNNX entries enlarge the luma with a network; AR holds what it invented to the range the source covers. Needs Direct3D 11; Catmull-Rom stands in elsewhere and while DLSS SR is enlarging |
| Chroma upsampling | Catmull-Rom | Jinc (EWA) is the best of them on film and stays inside the conversion shader; RAVU-zoom and FSRCNNX 8 AR put Cb and Cr through an mpv prescaler, FSRCNNX being the one for drawn lines. All three need Direct3D 11 and 4:2:0 in planes |

And under the Chroma upsampling list:

| Setting | Default | Notes |
|---|---|---|
| Sharpening | Disabled | *Adaptive-Sharpen* or *Unsharp + Clamp*, run after the resize, so it also reaches a film already at the screen's size. See above for what each is worth. Needs Direct3D 11 |
| Intensity | 3 | Five levels, and a level is the same amount of sharpening whichever method is chosen -- +4, +8, +13, +20 and +28 per cent of mean gradient, calibrated through the filter. 3 is where the quality measurements settle. Greyed while Sharpening is Disabled |
| Replace VP chroma upsampling | off | See above. The shaders rebuild the chroma of a progressive YUV 4:2:0/4:2:2 picture with the Chroma upsampling method and hand the video processor a 4:4:4 one, so the processor stays in the chain and RTX Video HDR goes on working. Interlaced video keeps the processor's chroma. It costs RTX Video Super Resolution, which the driver does not apply to a 4:4:4 picture: *Request Super Resolution* greys while this is ticked, and the statistics say `D3D11 (No Super Resolution when chroma by shaders)`. Needs Direct3D 11 |

**Default** on the **DLSS 5 NR** tab resets the tuning, from Style to Disable temporal history, motion
settings and the DLSS SR preset; it leaves Enable, Use DLSS SR, the key, both DLL paths and
render ahead, which belongs to the Settings page, alone. Applying the page sends only what was
changed on it, so it never undoes the toggle key or the main page, and the main page leaves
these settings alone.

The feature is also reachable programmatically through `IExFilterConfig`:
`Flt_SetBool("dlssNR", true/false)` and `Flt_GetBool("dlssNR", &b)`. `Flt_GetString("statsText")`
returns the statistics as they are drawn on the picture, for tools that log them.

---

## Known limitations

**Some shimmer remains.** With Optical Flow about 1.1× the input's flicker is left on moving
content, and the shader detector leaves moving areas as the network rendered them. Lowering
**Local tone** and **Local struct.** still helps, which is why they default low.

**Where Optical Flow is unsure** — large flat or dark areas, occlusions, transparency — trust
falls and those pixels get the network's output for that picture alone. The clamp keeps any
mistake within the local range of the effect.

**Motion the detector misses.** In detector mode, motion finer than the noise over nearly
flat areas can go undetected, and the steadied effect then lags behind it. Use Optical Flow,
or lower the stabilizer.

**Memory.** ~500 MB of VRAM at 1080p for DLSS, more at higher resolutions, plus a 159 MB
module resident while the session is up. The stabilizer adds about 95 MB at 1080p and 375 MB
at 2160p, plus what the Optical Flow engine allocates.

**Cost.** Two CPU stalls per frame for the cross-device synchronisation, plus the network
itself and the stabilizer. Fine for video framerates; this is not a low-latency design. Render
ahead keeps pictures on time as long as a picture takes less than about a frame.

**Settings change while the film plays.** They all take effect on the picture that follows,
with no restart: the formats the processor may take, RTX Video HDR, Super Resolution, the
chroma of the list and the box that moves it. One case used to freeze the picture with the
sound still running -- anything that stopped HDR output, RTX Video HDR unticked first among
them -- and it was not what it looked like: HDR output needs the modern presentation model,
*Swap effect: Discard* asks for the older one, and Windows never takes a window back to it.
The swap chain then presented every picture, in time and without error, while the screen kept
the last one. A window that has carried the flip model now keeps it, which shows everything
the old model showed. `playback_test --toggle` watches the desktop itself for exactly this.

---

## Diagnostic tools

`tools/dlssnr_probe/` holds four programs; `build.cmd` builds them.

**`dlssnr_probe.exe`** — talks to the snippet directly and reports what it says about
itself: caller validation, the parameter vtable layout, `GetFeatureRequirements`, which
NVAPI entry points it resolves and which the driver refuses, and a full D3D12
create/evaluate cycle. Useful on a new GPU, a new driver or a different DLL build.

```
dlssnr_probe.exe                 report only, nothing altered
dlssnr_probe.exe --spoof-auto    report the architecture the DLL asks for
dlssnr_probe.exe --spoof 1B0     report a specific architecture id
```

**`dlssnr_harness.exe`** — compiles `Source/DLSS/DlssNR.cpp` and the renderer's own
`Tex2D_t` against a real D3D11 device and runs 300 frames with toggles, resolution changes
and preset changes, then a teardown with work in flight. Not a mock: it is the filter's
code. Run it before putting a build in a player.

```
dlssnr_harness.exe --frames 300
```

It should end with `0 check(s) failed`.

Its temporal suites measure the network on sequences whose true motion is known, and write
their tables next to the program. Run them from `tools/dlssnr_probe`: the reference passes
compile the shaders from `Shaders/d3d11`, and `build.cmd` also embeds them the way the filter
does, so the filter's own classes (`DlssMotionMask.cpp`, `DlssOpticalFlow.cpp`,
`DlssStabilizer.cpp`) run unchanged.

```
dlssnr_harness.exe --temporal     motion vector conventions, depth, history
dlssnr_harness.exe --tmask        ControlMask values on still and moving content
dlssnr_harness.exe --toracle      a perfect motion map as the mask
dlssnr_harness.exe --tdetect      the shader detector against that map
dlssnr_harness.exe --tport        the filter's detector class against the harness detector
dlssnr_harness.exe --tbench       what the detector costs at 1080p and 2160p
dlssnr_harness.exe --teffect      how much of the effect each ControlMask value leaves
dlssnr_harness.exe --tflow        NVIDIA Optical Flow: direction, units, accuracy, timings
dlssnr_harness.exe --tstab        the post-DLSS stabilizer variants on real pictures
dlssnr_harness.exe --tstabport    the filter's stabilizer class against those passes, redraws included
dlssnr_harness.exe --tstabbench   what the stabilizer costs at 1080p and 2160p
dlssnr_harness.exe --tpipeline    the whole DLSS chain as the renderer runs it: CPU and GPU time per stage
dlssnr_harness.exe --tsr          DLSS Super Resolution: bring-up, presets, scales, costs, NR next to it
dlssnr_harness.exe --tsrq         DLSS SR against the resize shaders on moving film frames
dlssnr_harness.exe --tsrstill     DLSS SR on still, grained pictures: shimmer, grain along the edges, moving subjects
dlssnr_harness.exe --tsrport      DLSS SR's vectors from the filter against the same made on the CPU, and their GPU time
dlssnr_harness.exe --tupscale     the resize shaders and the mpv prescalers on film frames
dlssnr_harness.exe --tupscalecost what EfRLFN costs at film sizes
dlssnr_harness.exe --tchroma      the chroma upsamplers on 4:2:0 made from film frames
dlssnr_harness.exe --tmpvport     the filter's prescaler runner against the harness's
                                  (ArtCNN included: it is the one made of compute passes)
```

`--tframes N` sets the frames per run; `--tstrong` uses the strongest network settings;
`--timage <file>` picks the photo for `--teffect`, `--tflow` and `--tstab` (a Windows 11
wallpaper by default). `--nonr` skips the DLSS 5 NR session for the suites that do not need
it, `--srdll <path>` points at `nvngx_dlss.dll`, and `--srrefs N` limits `--tsrq`,
`--tsrstill` and `--tsrport` to the first N references. `--tupscale`, `--tchroma`,
`--tmpvport` and the three DLSS SR suites read 4K film frames from
`tools/dlssnr_probe/upscale_refs/`. `--tupscale` also runs the mpv shaders it finds
translated under `tools/dlssnr_probe/upscalers/hlsl/` (`Shaders/mpv/mpv_shaders.py` puts them
there); the three the filter embeds need nothing but the build.

A neural upscaler, EfRLFN, was measured too and not integrated: 922 ms per 1080p frame on the
RTX 3050 with DirectML (`--tupscalecost`), and no better than Catmull-Rom or Lanczos on clean
film frames (`--tupscale`).

**`vp_rebuild_test.exe`** — checks that rebuilding the hardware video processor keeps the
picture, which is what used to show a green frame when DLSS was toggled while paused.

**`shader444_test.exe`** — compiles the chroma pass the filter generates for *Replace VP chroma
upsampling*, for each source format and each chroma method, since a compute shader has rules a
pixel shader has not. One that does not compile leaves the picture to the video processor
without a word, so nothing on screen says it happened.

**`vp444_probe.exe`** — asks the driver, through the renderer's own `CD3D11VP`, what the
video processor really does with each input format: which it takes, how its conversion
compares, and how much enabling Super Resolution, RTX Video HDR or deinterlacing changes the
picture, for 4:2:0 and 4:4:4 alike. `--film` runs it on film frames, `--pack` checks the
packing into AYUV and Y410 is exact, `--views` which bind flags still make an input view, and
`--hdrconv` which conversions the processor declares. The numbers quoted in the chroma section
above come from it.

**`freeze_dump.exe`** — run against a player that hangs (by name, by process id, or with no
argument at all), it asks Windows what every thread is waiting for, names any cycle as the
deadlock it is, and walks each stack with the filter's own names, from the pdb beside its .ax.
It reads and changes nothing, and writes `freeze_dump.txt` beside itself.

**`playback_test.exe`** — plays a synthetic film through the built x64 filter
(`_bin\Filter_x64\MpcVideoRenderer64.ax`) in a DirectShow graph, in a window, on the system
clock: DLSS off, DLSS SR, and DLSS 5 NR with SR, render ahead off and on. It reads the filter's
statistics ten times a second and reports the Sync offset, skipped and late pictures, and how
long pause, run and stop take; it fails when a state change takes more than a second. The
settings go to the filter for the run only, nothing is saved.

With `--scalers` it plays a still NV12 picture through the shader video processor with each
Upscaling and Chroma upsampling method in turn, reports what the statistics say they did and
cost, saves each displayed picture as `scalers_<n>.bmp` and compares them with the Catmull-Rom
one — a wrong pass shows up as a large difference. It also saves the same picture with the
statistics drawn over it, as `scalers_<n>_stats.bmp`, which is how the overlay's box is checked.
`--vp` leaves the hardware video processor the formats it is set for, to see it convert.

`--mainpage` shows the settings page of the built filter for a few seconds and saves it as
`proppage.bmp`, without a player; `--section N` opens one of the seven tabs, and with no
`--section` the page says which tab it came back to on its own; `--click <id>` then clicks one control and saves
the page again as `proppage_clicked.bmp`, which is how the greying is checked. `--mainpage`
also prints the *Chroma upsampling* and *Upscaling* lists as the page really built them, each
entry with the number it carries and which one is selected: a picture says nothing about that,
and the order of those lists is the point of them. `--chroma` plays one picture, a 4K film
frame halved, as 4:2:0 and as 4:4:4 at its own size, and measures what each chroma upsampler
rebuilds against the 4:4:4 one — every method the list offers, and each of them again through
the video processor with *Replace VP chroma upsampling* on, which is where a chroma pass that
leaves a render target behind shows up as a colour cast. The hardware processor's own chroma
comes out between Nearest and Bilinear, 1.7 dB under Catmull-Rom along luma edges.

`--toggle` changes one setting at a time while the film plays, from the thread that owns the
window as the player does, and checks three things: that the call comes back, that the
renderer goes on drawing, and that **the screen itself keeps moving**. The last one is not the
same as the others: a swap chain can present picture after picture, in time and without error,
into a window the desktop no longer updates, and nothing inside the filter can see it. The
suite watches the desktop through the duplication API, which shows what Windows really
composes. A call that does not come back in time prints every thread's stack and what each is
waiting for. `--gpu` sends the pictures as D3D11 textures, the way a hardware decoder does,
which makes the renderer run on the decoder's device; `--hdr` asks for HDR passthrough, which
only means something while the desktop is already in HDR, and never switches the display
itself; `--switch` walks the video processor's own extras -- RTX Video HDR, Super Resolution --
as the chroma moves to the shaders and back; `--file <path> [--seek <seconds>]` plays a real
film through LAV Splitter and LAV Video, the ones the installed player carries, with the sound
rendered silently so the graph is clocked as the player clocks it. The program
carries a Windows 10 manifest, without which the version helpers answer 6.2 and the pages grey
what the player would not; it turns HDR passthrough off for its runs in exchange, since the
filter may then switch the display's own HDR state, which is not what is being measured.

```
playback_test.exe [--seconds 20] [--size 800x450] [--window 1280x720] [--fps 23.976] [--only N]
playback_test.exe --scalers       each Upscaling and Chroma upsampling method (--vp: hardware)
playback_test.exe --greying       what the page greys, for every state the renderer can be in
playback_test.exe --dlaa --size 1920x1080 --window 1920x1080   DLSS SR with nothing to enlarge
playback_test.exe --mainpage 10 --hover 1042   the tip that comes up on a control, greyed or not
playback_test.exe --mainpage 10   the settings page for 10 s, --click <id> clicks one control
playback_test.exe --mainpage 10 --section 4   on one of the seven tabs; --apply presses Apply
playback_test.exe --frame 10      the page in a real property frame, as a player puts it up
playback_test.exe --frame 10 --themed   and with a player painting its controls over it
playback_test.exe --chroma <png>  each chroma upsampler, the hardware one included, against 4:4:4
playback_test.exe --chroma10 <png> the same in 10 bits: P010 against Y410 (--interlaced: who converts)
playback_test.exe --toggle       each setting changed in full playback, as the player does
playback_test.exe --toggle --gpu --hdr --switch   as a hardware decoder feeds it, in HDR
playback_test.exe --toggle --file <mkv> --seek 1740   a real film, split and decoded
playback_test.exe --filter <ax>   any of the above with another build of the x64 filter
```

---

## Corrections

Earlier releases of this fork said things about the video processor that later measurements
disproved. They are listed here rather than quietly removed, because the tooltips and this
manual carried them for two versions and someone may remember them.

| It used to say | What is true |
|---|---|
| *RTX Video HDR (no tone mapping on 10-bit 4:2:0)* in the statistics, and the same claim in the tooltip and here | The driver tone maps a 10-bit 4:2:0 picture as well as any other: **36.7**, against 36.9 on 8-bit and 37.7 on 4:4:4. The 0.000 that started it was measured with the processor already told its input and output were PQ BT.2020, the one arrangement in which the extension does nothing. The statistic is gone and the star is unconditional |
| "tick *Replace VP chroma upsampling* as well and RTX Video HDR starts working on a 10-bit source" | It never needed the pre-pass. What it needed was for the renderer to keep the processor out of PQ passthrough on a 10-bit source, which is what upstream does and what 1.45 had undone |
| 8-bit 52.25 → 30.19, 10-bit 49.60 → 51.61, pre-pass → 30.22, given as the picture moving end to end | Void. They came from a bench whose `--file` never built the film's graph: every "film" run played its own 800x450 RGB32 pattern, which the renderer converts with shaders and which never brings the hardware processor in at all. Measured again on a real 10-bit film, the same configuration now moves 51.61 → **30.38**, the 8-bit signature |
| Super Resolution "is not requested" while the pre-pass is on | It is requested exactly as upstream requests it. The driver answers that it will and then leaves the 4:4:4 picture alone; the page greys the control and the scaling line says so, but nothing is taken away from the driver |

Two structural faults found with them, both this fork's own and both now fixed: a method of
ours sat in the middle of `IVideoRenderer` and a field of ours in the middle of `Settings_t`,
while the interface kept upstream's identifier -- so any caller built against upstream's
header reached `SetSettings` when it asked for `GetSettings`. See *Following upstream*.

---

## Following upstream

This fork branched at upstream's 0.10.7. It adds over a hundred files of its own while
changing fewer than forty of upstream's, and in those it deletes barely a hundred lines: it
adds, it almost never replaces, which is why a new upstream version merges cheaply. The
procedure, the inventory of what the fork adds and the traps already met are in
**[MERGING-UPSTREAM.md](MERGING-UPSTREAM.md)**. Any update starts with

    git fetch upstream
    python tools\upstream_check.py

which is read-only and reports what the merge will cost before anything is touched: the
upstream commits that reach this fork's code, a trial merge with the conflicts you will
really get, resource numbers used by both sides, and whether the generated shader files
will need regenerating.

---

## Licence and credits

MPC Video Renderer is by **Aleksoid1978** and contributors and is licensed **GPLv3**; see
`LICENSE.txt`. This fork is a derivative work and carries the same licence.

The NGX ABI declarations in `Source/DLSS/NGXTypes.h` are hand-written from the publicly
documented shape of the interface. NVIDIA DLSS, NGX and the `nvngx_*` / `_nvngx.dll`
binaries are NVIDIA property under their own licences and are not distributed here;
`nvngx_dlss.dll` comes from NVIDIA's own DLSS repository under its licence.

The prescalers under `Shaders/mpv/` are translated from mpv user shaders and keep their
authors' notices: **FSRCNNX** is Copyright (C) 2017-2021 **igv**
(`github.com/igv/FSRCNN-TensorFlow`) and **RAVU** is by **Bin Jin**
(`github.com/bjin/mpv-prescalers`), both under the **GNU Lesser General Public License 3.0 or
later**, whose text is in `Shaders/mpv/LICENSE.LGPL-3.0.txt`. **ArtCNN** is Copyright (c) 2024
**João Chrisóstomo** (`github.com/Artoriuz/ArtCNN`) under the **MIT** licence, whose text is in
`Shaders/mpv/LICENSE.MIT.txt`. Only the shaders are taken; the translation to HLSL and
everything that runs them is part of this fork and GPLv3 like the rest.

The **Adaptive-Sharpen** pass in `Shaders/d3d11/ps_sharpen.hlsl` is translated from
**bacondither**'s mpv shader, version 2021-10-17 (`github.com/bacondither/Adaptive-sharpen`),
Copyright (c) 2015-2021 bacondither, under the **BSD 2-clause** licence; its notice travels
with the pass. The three-line contrast-adaptive kernel measured beside it comes from AMD's
FidelityFX (MIT) and is not shipped.

`Source/DLSS/NvOF/` holds the two interface headers of the NVIDIA Optical Flow SDK 5.0.7,
copied unchanged. Each carries its own permission notice ("This copyright notice applies to
this header file only"), which allows copying and redistribution. Nothing else from the SDK
is used or included; the Optical Flow engine, `nvofapi64.dll`, comes with the NVIDIA driver.
