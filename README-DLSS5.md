# MPCVR-Plus

A fork of [Aleksoid1978/VideoRenderer](https://github.com/Aleksoid1978/VideoRenderer) with more
features. Every one of them is optional and off by default, and with all of them off the
rendering is bit-identical to upstream.

| | |
|---|---|
| **Scaling** | six upscalers chosen by measurement — ArtCNN C4F16 DS, FSRCNNX 8/16, RAVU-zoom and the anti-ringing versions — with both lists ordered best first |
| **Chroma** | three more methods, and a 4:4:4 pre-pass that hands the hardware video processor a full picture: +7 dB of colour on 10-bit film |
| **Detail** | sharpening after the resize, two methods and five calibrated levels |
| **DLSS 5 NR** | NVIDIA neural reconstruction (NGX feature 18) with a temporal stabilizer made for video |
| **DLSS SR** | DLSS Super Resolution 4.5 for enlarging, and DLAA at native size (experimental) |
| **Presentation** | render ahead, so none of it drifts off the audio clock |
| **Settings** | one page, seven tabs, in the order the picture travels through them |

It was called MPCVR-DLSS5 until 1.7, when DLSS 5 stopped being the whole of it. The branch is
still `dlss5`, so the filter reports itself as `0.10.9.xxxx.dlss5`.

## The two DLSS passes

The fork adds an optional NVIDIA **DLSS 5 Neural Rendering** pass (NGX feature 18) to the
Direct3D 11 video pipeline, with a temporal stabilizer made for video, and an experimental
option to enlarge the picture with **DLSS Super Resolution** (NGX feature 1) instead of the
resize shaders.

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
| Optical Flow | `nvofapi64.dll`, installed with the NVIDIA driver (RTX 20 series and later). Optional: without it nothing is steadied, and the statistics say why |
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

**Visual Studio 2026** builds it as well, with its own v145 tools, and upstream provides for
that. But those tools no longer target Windows 7, and it shows: the Win32 filter they make
imports `GetSystemTimePreciseAsFileTime`, which Windows 7 does not have, so it will not load
there. The x64 one asks Windows for nothing it did not ask before. To build under 2026 what
2022 builds, add three individual components in its installer: *MSVC v143 - VS 2022 C++
x64/x86 build tools (v14.44-17.14)*, and *C++ v14.44 (17.14) MFC* and *ATL for v143 build
tools (x86 & x64)*, which the first does not bring. `Directory.Build.props` then takes the
14.44 compiler by itself and each project says `MSVC 14.44…` as it builds; the filter that
comes out imports exactly what the released 1.9.1 does, 268 entries on x64 and 253 on Win32.
Without the components the same line says 14.5x and the build is v145's.

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
opened a second time on the D3D12 side. The two devices wait for each other on the GPU,
through two shared fences, and the render thread waits for neither. It used to wait for
both on the CPU, 18 ms a picture at 1080p on the RTX 4060, which held DLSS 5 NR with DLSS SR
from 1080p to 4K at "ready in 26.2 ms" with 107 pictures of a 10-second run left out; with
the waits on the GPU, 19.6 ms and none. What keeps that from the device removal an earlier
GPU-side version caused, and from a hung queue freezing the picture, is in
`Source/DLSS/DlssNR.h`.

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
`ScalingRatio 1.0`: `DLSSNR.Depth` is never set, and `DLSSNR.MVec` only while the stabilizer
runs, which gives the network its Optical Flow vectors. Left unset, the DLL's own log confirms
it runs that way (`EvaluateFeature Color=... MVec=0000000000000000 Depth=0000000000000000`).
What the vectors do, and what the stabilizer adds to them, is in the next section.

Other findings worth knowing: the SDK version must be **0x13** (the driver core rejects
anything newer with `FAIL_OutOfDate`), the driver's NGX core does not know feature 18 at all
so the snippet must be loaded directly, and the working set is about **500 MB of video
memory at 1080p**, growing with resolution.

**The runtime is the installed driver's.** `_nvngx.dll` lives in the driver store, and the
store keeps the packages of the drivers that were replaced. DLSS 5 NR used to take the first
one the store listed, which is the oldest: on a machine with three, it ran on the runtime of
a driver two versions back (32.0.15.9186) while DLSS Super Resolution, which asked the
registry, ran on the installed one (32.0.16.1656). Both ask the registry now, and only when it
names none take the newest the store holds (`Source/DLSS/NgxRuntime.h`).

### An evaluation that fails

The renderer switches DLSS 5 NR off rather than fail on every picture, and until now nothing
switched it on again but the toggle key: one evaluation the GPU did not answer within two
seconds, and the rest of the film went without. The session is now started again from
nothing two seconds later, twice at most, and again after a seek.

Made to fail on purpose in MPC-HC, every hundredth evaluation (`MPCVR_DLSSNR_FAIL_AT=100`
in the player's environment): 99 pictures with it, 50 without while the film plays on, back
for 99, the same once more, then off -- until the seek, which brings it back, as the toggle
key does. Starting the session again takes about 0.7 s, during which 16 pictures were not
shown. The statistics say what happened (`DLSS 5 NR : unavailable: ...`), and so does the
trace (see *Diagnostic tools*).

### What it was put through

In MPC-HC, a 1080p film at 25 fps on a television at 25 Hz, with the trace saying for every
picture whether DLSS 5 NR ran on it: the toggle key while playing, while paused, and three
times within a second; seeks short and long, forwards and back, three in half a second;
subtitles next, off and on; another sound track; pause and play; out of fullscreen and back;
the next file and the previous one. DLSS 5 NR was back after each: 2 403 pictures of 2 581,
the rest being the stretches it was switched off for and the two to five pictures a restart
loses.

The same handling, shorter, in each way it can be set up:

| | pictures | DLSS 5 NR ran on |
|---|---|---|
| the shaders converting instead of the video processor | 1 131 | 1 015 |
| after upscaling, at 3840x2160 | 1 014 | 482 |
| without the stabilizer | 1 152 | 1 047 |
| the stabilizer on the shader detector, since removed | 1 151 | 1 047 |
| Optical Flow, vectors not given to the network | 1 151 | 1 047 |
| DLSS Super Resolution after it | 1 137 | 1 033, and DLSS SR on 1 106 |
| a 4K HDR film at 23.976 fps, the network at 3840x1600 | 1 010 | 480 |

No fault in any of them. The network takes 14 to 15 ms of GPU at 1920x1080 on an RTX 4060,
45 ms at 3840x1600 and 62 ms at 3840x2160, which is longer than a frame lasts: that is the
second row and the last, where about half the pictures are left out before they are drawn
and the film stays with its sound (see *A refresh chosen for every picture*).

And with *Use the video processor for resizing* ticked, the processor enlarging what the
network leaves (see *Who resizes while DLSS runs*). The same handling with the toggle key
out of fullscreen as well, where the processor reduces instead:

| | pictures | DLSS 5 NR ran on | of which the processor resized |
|---|---|---|---|
| RTX Video HDR, no Super Resolution (Y410) | 1 504 | 1 345 | 1 345 |
| RTX Video HDR, Super Resolution (P010) | 1 499 | 1 314 | 1 314 |
| the same with the chroma replacement (Y410) | 1 504 | 1 342 | 1 342 |
| SDR, no Super Resolution | 1 485 | 1 320 | 1 320 |
| SDR, Super Resolution | 1 493 | 1 226 | 1 226 |
| an interlaced clip, 1080i, no Super Resolution | 2 137 | 1 883 | 1 883 |
| the same, Super Resolution | 1 506 | 1 251 | 1 251 |
| an HDR10 film, 1920x800, Super Resolution | 1 430 | 1 284 | 1 284 |
| an HDR10 film, 3840x1600: resized in a window only | 1 372 | 816 | 96 |
| DLSS SR ticked as well: it enlarges, not the processor | 1 487 | 1 323 | 0 |
| a Dolby Vision film: the processor is not in the chain | 1 317 | 612 | 0 |

One fault came out of it, and it was an old one: with *Apply after upscaling*, coming back to
fullscreen left the network's textures at a size the picture and the window had in common on
the way (2122x1022 on the way to 3840x2160), every picture was then refused for not fitting
them, and DLSS 5 NR stayed off for the rest of the film with its lines still in the
statistics -- 0 pictures of 178 in the build before, 84 of 164 now, which is every picture
the network has the time for at that size.

### A working size below the picture's

The network costs in proportion to the pixels it is given, and a 4K film gives it more than an
RTX 4060 can carry in the time a picture lasts. *Working size*, on the DLSS 5 NR tab, runs it on
the picture reduced -- 50 to 100 per cent of its size each way, in steps of 5, 100 by default --
and carries what it changed back onto the picture at its own size. Game injectors such as
OptiScaler offer the same trade. The DLL has a parameter whose name promises it,
`DLSSNR.ScalingRatio`, and it does nothing: set to 0.5, `--tnrscale` measured the same pictures
to the hundredth of a decibel and the network in the same time, 60.8 ms at 3840x2160 against
60.7. So the reduction is the renderer's (`Source/DLSS/DlssCompose.cpp`,
`Shaders/d3d11/ps_dlss_compose.hlsl`):

1. the picture is reduced with Lanczos into the network's input;
2. the network and the stabilizer run at that size, as they do at any other;
3. the change between their input and their output is enlarged with Catmull-Rom and added to
   the picture.

The picture keeps its own fine detail and its own grain, and the network's change reaches it
down to the detail the working size holds. Nothing else in the chain moves: the pass still
takes and gives the picture at the source's size, or at the window's after upscaling, and
whatever resizes goes on resizing after it. At 100 nothing is reduced and the pass is what it
was. The statistics give the working size a line under the `DLSS 5 NR` step, `working`, with what
the reduction and the composition cost; the trace's note says it too.

Measured with `dlssnr_harness --tnrscale` on a 4K film frame (`4K1.png`, 3840x2160) with fresh
grain on every picture and the filter's default settings, against the network and the
stabilizer at the full size, frame for frame (PSNR, RGB, dB; still picture, 8 measured
pictures after 12):

| working size | network at | the picture as it is | the network's output enlarged | the change carried back |
|---|---|---|---|---|
| 80 % | 3072x1728 | 42.56 | 47.03 | 53.87 |
| 75 % | 2880x1620 | 42.56 | 46.55 | 53.73 |
| 70 % | 2688x1512 | 42.56 | 45.53 | 51.86 |
| 60 % | 2304x1296 | 42.56 | 44.01 | 49.77 |
| 50 % | 1920x1080 | 42.56 | 42.43 | 48.20 |

The output enlarged loses the grain: its picture-to-picture change falls from 0.0054 to 0.0033
at 80 % and 0.0021 at 50 %, where the network at the full size keeps the input's. The change
carried back keeps it (0.0054 at every size). On a pan of 2 by 1 pixels a picture the same
holds, 0.3 to 1 dB higher (54.84 dB at 80 %, 48.48 at 50 %). The smaller the working size,
the stronger the change: projected on the full-size one it is 100 % of it at 80 %, 105 % at
75 %, 108 % at 70 %, 114 % at 60 % and 115 % at 50 %. *Intensity* tempers it if it shows.

Two other ways of carrying the change back were measured and left in the bench. A joint
bilateral enlargement, guided by the picture's own colours, came within 0.13 dB of Catmull-Rom
either way at every size and setting tried, for the same cost. A guided filter, fitted between
the network's input and output and applied to the picture, took the grain away: with the
regularisation at 0.001 it landed further from the network at the full size than the picture
left alone (39.2 to 41.9 dB), and at 0.0001 it kept half the grain and stayed 3.8 to 7.5 dB
under Catmull-Rom.

What it costs on the RTX 4060, from the same suite: the network as the filter times it, and a
whole picture through the reduction, Optical Flow, the network, the stabilizer and the
composition, waited for:

| working size | 3840x2160: network | whole | 3840x1600: network | whole |
|---|---|---|---|---|
| 100 % | 60.7 ms | 65.0 ms | 43.8 ms | 47.4 ms |
| 90 % | 51.3 | 56.6 | 35.7 | 39.9 |
| 80 % | 37.0 | 41.2 | 28.0 | 32.2 |
| 75 % | 34.1 | 37.8 | 24.4 | 28.5 |
| 70 % | 29.4 | 33.0 | 21.6 | 25.4 |
| 60 % | 21.3 | 25.1 | 14.9 | 18.2 |
| 50 % | 15.4 | 18.9 | 11.7 | 14.0 |

The reduction takes 0.35 to 1.0 ms and the composition 0.4 to 1.1.

And in MPC-HC, 40 s of each film from 10 minutes in, the television at 24 Hz, with the trace
(`pacing_report.py --dlss`). A picture is drawn in the time the network takes and 11 to 16 ms
more -- the decoded 4K HDR picture converted, Optical Flow, the stabilizer, the composition --
and one that does not fit the 41.7 ms a picture lasts is left out undrawn:

| film | working size | drawing (median) | pictures drawn |
|---|---|---|---|
| The Shepherd, 3840x2160 HDR | 100 % | 71.6 ms | 440 of 873 |
| | 75 % | 51.4 ms | 462 of 908 |
| | 60 % | 36.6 ms | 876 of 911 |
| | 55 % | 32.5 ms | 910 of 912 |
| | 50 % | 29.0 ms | 907 of 909 |
| Kill Bill, 3840x1600 HDR | 100 % | 56.6 ms | 432 of 856 |
| | 75 % | 36.1 ms | 881 of 885 |
| | 70 % | 32.7 ms | 884 of 886 |
| | 60 % | 25.7 ms | 890 of 892 |

Where every picture but a few was drawn, those few are the first, while the session starts,
and one more in Kill Bill at 75 %, 9.4 s in. On this card a 3840x2160 film wants 55 % and a
3840x1600 one 75 %; a faster card wants less taken off, and the statistics' *Total* line, against
the frame it names, says how much room is left. A new working size is taken on Apply, and the network starts
again at it: seven pictures went undrawn in the bench while it did
(`playback_test --nrscale 100 --nrscaleto 70`).

The stabilizer's Optical Flow works on a fraction of the working picture, *Optical Flow
resolution*, so its size follows the working size too: at 1/2, on a 3840x2160 film,
1920x1080 at 100 % and 1056x594 at 55 %. At Auto, the default, `FlowFactor` chooses 1, 2 or 4
from the working height to stay near 540 lines, as the stabilizer always did and DLSS Super
Resolution does for its own flow, and the flow's size jumps as the working size moves: 960x540
at 100 %, 672x378 at 70 %, 1248x702 at 65 %, 960x540 again at 50 %. A source under 720 lines
has its flow at its own size there.

`--tnrscale --nrflow N` forces the divider at every working size below the full one. From 270
lines of flow to the working picture itself, 2112x1188 to 3072x1728, on the still picture, the
pan and a 1152x768 object moving over the picture, the result moved by half a decibel at most,
on the pan, where the finest flow came lowest; what the stabilizer drags across the moving
object (`obj dev`, its mean distance from the network without it) went down by 0.0001 at most
with a finer flow, a fortieth of an 8-bit level, where going from 80 to 55 % adds 0.0004.
Optical Flow took 0.6 ms at 480x270, 2.1 ms at 1728x972 and 6.8 ms at 3456x1944, Full at 90 %.
So the divider is left to the viewer, and Auto keeps every film where it always was.

---

## Temporal stabilizer

The network blends each frame with its own previous output (`CC_Control_History_Blend`,
`dlssnr_prev_output`). A game keeps that history aligned with motion vectors; video has
none, and the result is a luminance shimmer: on real pictures, plain DLSS shows 1.14 to 1.33
times the low-frequency flicker of its own input, and a moving object up to twice its own. The temporal suites of the harness (below)
measured what this DLL does with its inputs:

- **Motion vectors steady the network, and it reads their direction.** Their convention could
  be found (pixels, current to previous, scale 1). Given as `DLSSNR.MVec`, they make it keep
  more of its effect and shimmer less than its own input; on a still picture, where the motion
  is zero, any zero vectors will do, but where something moves the same vectors inverted leave
  a moving object shimmering 1.7 to 2.6 times its input's, against 0.7 to 0.8 the right way
  round (`--tstab`, below).
  An earlier note here said correct, inverted and absent vectors measured the same: they do
  not. Depth changes nothing.
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
3. **Motion** comes from **NVIDIA Optical Flow**: forward and backward flow from the driver's
   `nvofapi64.dll` (4×4 grid, medium quality), computed on the working picture divided by
   *Optical Flow resolution* -- 1/4, 1/3, 1/2, Full, or Auto, the default, which takes 1, 2
   or 4 from the working height for about 540 lines as the stabilizer always did: 960x540 for a
   1080p film. The history is moved along the vectors, so moving areas are steadied too. A
   vector is trusted according to its forward/backward consistency, its matching cost, and
   how well the previous input, moved along it, matches the current one; where trust falls,
   the network's output for that picture is used as it is.

   If Optical Flow cannot start (a GPU older than RTX 20, an old driver), nothing is
   steadied: every picture goes through as the network left it, and the statistics and the
   page's status line say why. Until 2026-10-15 a shader detector stood in, and could be
   chosen instead: four passes at a quarter of the resolution, with a noise floor measured
   on the video, a dilation and an 8-picture hold. Without vectors it steadied only what
   stood still (its row below), and it was removed with its setting.
4. **The vectors also go to the network** as `DLSSNR.MVec`, from the same Optical Flow and the
   same picture, cleaned the way DLSS Super Resolution's are (`CDlssStabilizer::ForDlssNR`, and
   *DLSS Super Resolution* below): each block drawn to the picture's global motion, refined on
   the flow frames, unless it moves on its own, and then searched again on the frames. The
   stabilizer itself still warps its history with the raw vectors it was measured with. Until
   2.0 this was a box on the page, off by default, and it sent the raw vectors.

Only a new picture moves the history on. A redraw — paused video, the OSD, a change that
brings no new picture — is steadied again against the same history as its picture, so it
shows what changed without taking a second step.

Measured on a photo with grain and 8×8 block noise, 1920×1080 windows moving with known
motion (`--tstab`, default network settings, RTX 4060). Flicker is the frame-to-frame change
of blurred luma, compensated by the true motion, as a ratio to the input's; on a moving object
it is taken inside the object, compensated by the object's own motion. *Effect* is how much of
plain DLSS's change to the picture is left, over the picture and on the object:

| | Still | Slow pan | Pan | Around an object | The object | A fast object | Cut | Effect |
|---|---|---|---|---|---|---|---|---|
| Plain DLSS | 1.16× | 1.19× | 1.19× | 1.33× | 1.83× | 2.18× | 1.14× | 100 % |
| Stabilizer, no vectors to the network (until 2.0) | 1.01× | 1.02× | 1.02× | 1.05× | 1.07× | 1.13× | 1.01× | 98–100 % |
| Stabilizer, true motion, no vectors to the network | 1.01× | 1.01× | 1.01× | 1.02× | 1.06× | 1.10× | 1.01× | 98–100 % |
| Raw vectors to the network + stabilizer (the box, until 2.0) | 0.97× | 0.98× | 0.98× | 0.99× | 0.93× | 0.97× | 0.97× | 114–129 % |
| **Cleaned vectors + stabilizer (now)** | **0.93×** | **0.94×** | **0.93×** | **0.93×** | **0.92×** | **0.97×** | **0.93×** | **114–127 %** |
| The same, stabilizer at 50 | 0.86× | 0.89× | 0.87× | 0.87× | 0.81× | 0.89× | 0.87× | 115–128 % |
| Cleaned vectors, no stabilizer | 0.81× | 0.88× | 0.82× | 0.83× | 0.75× | 0.87× | 0.83× | 116–129 % |
| True motion to the network, no stabilizer | 0.81× | 0.88× | 0.82× | 0.82× | 0.71× | 0.81× | 0.83× | 115–132 % |
| The same, inverted | 0.81× | 0.99× | 1.21× | 0.84× | 1.68× | 2.59× | 0.83× | 99–120 % |

The object moves 4 pixels a frame over the still picture, the fast one 12 over the slow pan.
Without vectors the stabilizer removes nearly all the flicker DLSS adds, moving content
included, and stays within 0.0005 of plain DLSS on average. With the vectors the network
steadies itself: alone, it shimmers less than its own input, a moving object included, and
keeps 14 to 27 % more of its effect. That is what the box used to warn of as rendering
differently around moving objects — more of the effect, nothing broken: behind the object,
where it was in the last eight frames and no longer is, the picture differs from the same run
with no object at all by 3.4 to 3.9 thousandths of luma against 2.9 to 4.0 without vectors
(×1000, at a quarter of the resolution), and exact vectors leave the same, 3.5 to 4.0: the
larger effect building up again on what the object uncovers. The raw vectors the box sent
steadied less, and least on a film that weaves: on the wall of *Cyrano*, DLSS 5 NR and the
stabilizer shimmered 1.21 times the source without vectors, 1.12 with the raw ones, 1.04 with
the cleaned ones (`--tsrclipnr`).

The stabilizer, which steadies the effect and leaves the video's own flicker alone, gives
back part of what the network, given vectors, takes out of the input: 0.93× at 100 against
0.81× without it, 0.86× at 50. That is the slider's to decide, by eye; at 0 nothing runs,
Optical Flow and the vectors included. A variant filtering the whole output, TAA-style, was
measured too and dropped: it steadies more by also smoothing the video itself, its grain and
its edges.

Checked in the harness (`--tstabport`): the filter's own class, with its shaders embedded as
in the filter, measures exactly what the reference passes measure — every sequence, alone and
giving the network its cleaned vectors — and each of its redraws shows, bit for bit, what its
picture showed.

Cost on the RTX 4060 at each *Optical Flow resolution* (`--tstabbench`, GPU time per
picture, a moving picture, the flow's size in brackets): Optical Flow with its passes and the
stabilizer together, and in brackets the same without the network's cleaned vectors:

| Working size | 4 | 3 | 2 | Full |
|---|---|---|---|---|
| 1920×1080 | 1.18 ms (0.88), 480x270 | 1.32 ms (0.97), 640x360 | 1.75 ms (1.19), 960x540 | 3.87 ms (2.49), 1920x1080 |
| 3840×2160 | 3.59 ms (2.66), 960x540 | 3.97 ms (3.03), 1280x720 | 5.58 ms (3.87), 1920x1080 | 13.41 ms (9.00), 3840x2160 |

The cleaned vectors cost 0.3 to 0.6 ms at 1080p and 0.9 to 1.7 ms at 2160p below Full: the
global motion, its refinement and the block passes, the same DLSS SR runs on its own picture.

The CPU side stays under half a millisecond: nothing waits for the GPU. Memory: about 95 MB
at 1080p — the history, two input copies and the result at the working size — and 375 MB at
2160p, plus the Optical Flow engine's own buffers. With the stabilizer at 0, nothing is
allocated and nothing runs.

The detector was validated against a perfect motion map on synthetic sequences (`--tdetect`,
removed with it): no lag on fades and slow pans, no added smear around moving objects, and
repeated pictures (variable frame rates, 25p stored as 50p) handled like the perfect map. It
was still the weaker of the two everywhere anything moved, and the page offered it beside
Optical Flow, which every card DLSS 5 NR runs on has.

---

## DLSS Super Resolution (experimental)

**Use DLSS Super Resolution for upscaling**, in the **NVIDIA DLSS Super Resolution (experimental)**
box on the **Scaling** tab beside the other
enlargers, enlarges the picture with DLSS Super Resolution instead of the **Upscaling** method,
which is then greyed. It is independent of DLSS 5 NR: another DLL, another session, and it works with NR on
or off. When NR runs before upscaling, SR takes its output.

**Also run when no enlarging is needed (DLAA).** DLSS only enlarges, so a film already at the
screen's size -- 4K on a 4K screen -- never reached it at all. With this ticked it runs there
too, at the picture's own size, rebuilding it without changing its scale; `QualityModeFor`
asks NGX for `NGX_PERFQUALITY_DLAA` at a ratio of one, which is the mode games call DLAA. The
statistics' Scaling step says so: `3840x2160 -> 3840x2160 DLSS SR DLAA/M`. Off, the same film
reads `DLSS SR : initialised` under it and no pass runs.

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
history. The vectors now go through these passes before DLSS
(`Shaders/d3d11/cs_dlss_global_motion.hlsl` and `cs_dlss_global_refine.hlsl`, then PASS 4 and
PASS 3 of `ps_dlss_stabilize.hlsl`, `CDlssStabilizer::ForDlssSR`):

- **the picture's global motion**: the median of the blocks that can be trusted — forward and
  backward flow agree, the match is good, the vector stays in the picture — exact to the
  engine's 1/32 of a pixel, measured on the GPU with nothing waiting for it. The median of all
  blocks is not enough: on heavy grain the flat areas, where only the grain changes, give a
  coherent wrong motion that moved it 0.6 to 4 pixels on four film frames out of six. The
  median is then refined on the two flow frames themselves, and taken as it is however small
  (*A film that weaves*, below);
- **each block** keeps a motion of its own only if it lies more than 0.75 to 1.5 pixels from the
  global motion and the trusted blocks around it share it (see *Moving subjects* below);
  otherwise it takes the global motion. A block pointing out of the picture where the global
  motion does not is noise as well.

A still background thus gets the motion of the whole picture — none, or the film's weave —, a
pan the pan, and what moves on its own keeps its vector. `--tsrstill` measures it on six 4K film frames, each halved and grained
anew on every frame, then enlarged twice: 40 still frames, an object crossing them, a slow pan
(0.7 pixel a frame) and a pan (3.8 pixels a frame). *Shimmer* is how much the structures of a
still picture move from one frame to the next, *grain flicker* how much the grain left does,
both ×1000 on luma; PSNR in dB.

Measured on the RTX 4060, averages over seven film frames:

| | Light grain | | | | Old film grain | | | |
|---|---|---|---|---|---|---|---|---|
| Vectors | Shimmer | Grain flicker | Slow pan | Pan | Shimmer | Grain flicker | Slow pan | Pan |
| Raw (up to 1.2) | 0.69 | 1.12 | 48.2 | 48.3 | 1.88 | 2.92 | 42.9 | 42.9 |
| Snapping alone (first version) | 0.76 | 0.95 | 48.3 | 48.6 | 1.68 | 1.97 | 43.2 | 43.5 |
| The median global motion (until 2.0) | 0.76 | 0.91 | 48.6 | 48.7 | 1.62 | 1.96 | 43.6 | 43.8 |
| **Now** | **0.73** | **0.89** | **50.9** | **50.7** | **1.62** | **1.97** | **44.8** | **45.2** |
| Exact (not available in playback) | 0.84 | 0.87 | 52.1 | 52.2 | 1.84 | 1.80 | 45.2 | 46.5 |

The shimmer falls to the level of exact vectors, or under it, and the refined global motion
gains the pans 1.2 to 2.3 dB over the median. The raw vectors' shimmer was low only where they
held still in the middle of the picture: along the edges they were off by 8.6 pixels on light
grain and 18 pixels on old film grain in the outermost 4 pixels, shimmered there 1.7 to 2.2 times as
much and left 23 to 27 % more grain than exact ones; now 0.5 pixel, and 5 % more grain. With exact
vectors DLSS keeps a slight edge effect of its
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
| Raw (up to 1.2) | 1.20 | 44.2 | 1.07 | 2.36 | 41.2 | 2.40 |
| Snapping alone (first version) | 1.20 | 43.7 | 0.91 | 2.52 | 41.2 | 1.59 |
| The median global motion (until 2.0) | 0.84 | 44.3 | 0.83 | 1.95 | 41.6 | 1.45 |
| **Now** | **0.84** | **44.3** | **0.31** | **1.94** | **41.6** | **0.86** |
| Exact (not available in playback) | 0.31 | 43.8 | 0.19 | 0.85 | 41.1 | 0.45 |

*Shimmer* is the same measure taken on what moves, against the frame before at the place it
came from: the subject inside its outline, the pan away from the edge it enters by. The subject
shimmers 30 % less on light grain and 23 % less on old film grain than with the first version,
its PSNR passes that of exact vectors, and its vectors are off by 0.6 and 1.2 pixels; pans,
whose global motion is refined, shimmer 63 % and 41 % less than with the median. Still pictures
lose nothing to any of it: against the snapping alone the grain left and the PSNR are the same
or better. A few false motions still survive on flat areas of a still picture, where any
offset matches. What was tried to take them out — weighing each block's say by its texture,
checking each kept vector against the frames, requiring the same motion two pictures running,
asking a larger share of the window — took moving subjects' motion away as well, or cost the
slow pan under heavy grain, and was left out. Subjects still shimmer more than with exact
motion, chiefly under heavy grain, where a block of low contrast cannot tell its own motion from
the grain's: hence *Experimental* on the option.

**A film that weaves.** On film a background that holds still is not still: the frame moves in
the gate, and a scanned picture moves by a fraction of a pixel from one frame to the next. On
*Cyrano* (2021), the wall behind the actors at 00:32:24, which shimmered under DLSS SR, moves
0.48 source pixel a frame on average, and 62 % of the frames more than 0.4. The median of
Optical Flow's blocks came out short of that motion, and under the 0.4-pixel dead zone PASS 4
then had on 77 % of the frames: DLSS was told the wall had not moved, resampled its history to
the wrong place, and the wall shimmered more than the source itself. Finer flow, at the
source's size and with the median, brought it only to 0.77 of the source's shimmer, for 3 ms
more.

So the global motion is now refined on the two flow frames (`cs_dlss_global_refine.hlsl`): two
Gauss-Newton steps on the brightness difference, from the median, leaving out the 16×16 groups
whose difference passes 2.5 times the median group's. Those are what moves on its own: actors
walking a pixel or two a frame lie within Optical Flow's noise of the weave, and taken in they
left the estimate 0.34 pixel off; left out, 0.02. And the dead zone is gone: a small motion
taken to zero measured worse, on films that hardly move, than the same motion followed.

`--tsrclip` measures DLSS SR on a film's own frames, against the film's own motion, written by
`clip_motion.py`, which registers each frame on the one before at the source's size, with what
moves on its own left out. The figure is the shimmer of what holds still once that motion is
taken out, against the source's own:

| | *Cyrano*, the wall | *Boudu* (1932) | *Casimir* (1950) |
|---|---|---|---|
| The film's weave, pixel a frame | 0.48 | 0.03 | 0.01 |
| The median global motion (until 2.0) | 1.14× | 0.47× | 0.79× |
| **Refined (now)** | **0.38×** | **0.38×** | **0.66×** |
| The film's own motion given everywhere | 0.36× | 0.40× | 0.74× |
| Vectors at zero | 1.24× | 0.48× | 0.77× |
| Preset K, refined | 0.21× | 0.26× | 0.48× |

The refinement costs 0.1 to 0.3 ms a frame at 1080p, timed in turns with the median on the same
frames. Preset K is steadier still, and softer: 8 % less detail on the wall of *Cyrano*. With
DLSS 5 NR before DLSS SR, as the filter chains them, the wall went from 1.51 to 0.43 times the
source's shimmer (`--tsrclipnr`); and DLSS SR's vectors computed on the picture before the
network serve as well as those computed after it, 0.43 either way, which is what one Optical
Flow shared by the two would see. DLSS SR keeps its own, on the picture it enlarges.

**A cut.** DLSS resamples its history along the vectors it is given, and nothing told it that
the picture before a cut belonged to another shot: where a bright shot followed a darker one,
it blended the dark one in, and the bright parts of the new shot came out darker for two or
three pictures — seen in playback, not when stepping picture by picture. A seek resets DLSS; a
cut did not. `--tsrcut` measures it on the cuts seen in playback, *La Firme* at 16:17 and
*Cyrano* at 36:59, and one two seconds before the first: what the enlarged picture, brought
back to the source's size, lacks on the pixels that brightened across the cut, against the
level the new shot settles at, ×1000 on luma, on the first three pictures of the new shot:

| | *La Firme*, 16:15 | *La Firme*, 16:17 | *Cyrano*, 36:59 |
|---|---|---|---|
| 2.1 | 62.9, 7.6, 3.6 | 85.3, 24.8, 9.8 | 114.6, 35.3, 12.3 |
| **Now** | **0.1, 0.3, -0.1** | **0.6, 0.4, -0.3** | **0.5, 0.2, 0.5** |
| DLSS reset on the cut | -0.1, 0.1, -0.4 | 0.2, -0.3, -0.7 | 0.0, -1.1, -0.3 |

DLSS has a reset, but the CPU sets it before the GPU has seen the picture, several pictures
ahead with render ahead, and waiting for the GPU is what render ahead exists not to do. A
vector that leaves the picture makes DLSS drop that pixel's history, though, and the vectors are
made on the GPU: on a cut every one of them now points out of the picture, which leaves DLSS as
clean as the reset. The cut is told on the GPU too (`cs_dlss_scene_cut.hlsl`), from the two
flow frames, the last moved by the global motion: the distance of their luma histograms, the
median 16×16 group's difference, that difference against the eight pictures before, and the
median group's normalized correlation, which a cut takes to nothing and a change of light leaves
alone. A cut is a difference and a histogram that move, and either the light at once — the
histogram past 0.25 and the difference thirty times the pictures before — or the picture's
structure — the correlation under 0.25 and the difference three times.

The light alone missed the cuts out of a shot that already changed a lot on every picture, a
chase or a car under trees; the correlation alone missed the flashes, which DLSS dims with the
pictures before them as it darkens a bright shot after a dark one: a flash of *Dances with
Wolves* came out 41.9 thousandths of luma too dark over three quarters of the picture, and is
right now (0.0). `--tcutscan` runs the detection alone over minutes of a film, beside ffmpeg's
own scene score of each picture. Over 48 minutes of six films — *La Firme*, *Cyrano*, *Fight
Club*, *Indiana Jones and the Last Crusade* in HDR, *Les assassins sont parmi nous* (1946) and
the buffalo hunt of *Dances with Wolves* — it called 571 pictures, every one of the 296 where
ffmpeg's score passes 0.3 among them. Of the other 275, 169 have a correlation of 0.1 at most,
cuts ffmpeg scores lower; 45 are flashes and flickering lights; the rest are the splices of the
1946 print and the dust of the hunt. What came closest without being called is dust and blur
in a chase. A picture called a cut that is
not one costs that picture its history: a little softer and grainier, once; in a battle full
of flashes that is many pictures in a second. The detection costs 0.06 ms a picture at 1080p,
timed frame by frame against 2.1's motion on the same frames, and the filter's passes match the
same computed on the CPU (`--tsrport`).

DLSS 5 NR, which has had the vectors since 2.1, leaves no such trail: within 2.5 thousandths of
luma at the same cuts and at the flash, with the filter's own tuning, after the stabilizer and
at the network's own output, with or without the detection (`--tsrcut --tsrcutnr`). Its vectors
go without it.

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
1.3 ms, and nothing when DLSS 5 NR lent its own; DLSS SR now always makes its own. On the RTX
4060 the vectors take 1.3 ms on a still picture or a pan and 1.8 ms on a zoom (`--tsrport`, a
1912x1072 source), the snapping alone 1.05 ms.

**Quality — read this before turning it on.** `--tsrq` pans windows of six 4K film frames by
half a source pixel per frame, reduces them to the source, grains or compresses them per
frame, and measures every method against the reference it never saw. Averages over the six
frames (PSNR in dB; *grain* is the fine detail left in flat areas, the source keeps about 2.4):

Measured on the RTX 4060, averages over seven film frames, the grain row grained anew on every
frame:

| | Clean, still | Clean, moving | Grained, still | Grained, moving | Grain left | Compressed, still | Compressed, moving |
|---|---|---|---|---|---|---|---|
| Catmull-Rom | **54.3** | **54.3** | 45.1 | 45.1 | 2.28 | **45.9** | 45.8 |
| Lanczos3 | 53.1 | 53.1 | 44.5 | 44.5 | 2.43 | 45.5 | 45.4 |
| **DLSS SR, the filter's vectors** | 50.5 | 52.8 | **48.7** | **50.7** | **0.61** | 45.5 | **47.7** |
| DLSS SR, the median global motion (until 2.0) | 50.5 | 51.5 | 48.7 | 48.7 | 0.61 | 45.5 | 46.7 |
| DLSS SR, raw vectors (up to 1.2) | 50.1 | 51.4 | 48.1 | 48.3 | 1.08 | 45.3 | 46.3 |
| DLSS SR, no vectors | 50.5 | 48.0 | 48.9 | 44.1 | 0.41 | 45.5 | 44.5 |
| DLSS SR, exact motion (not available in playback) | 50.5 | 53.6 | 48.9 | 52.2 | 0.41 | 45.5 | 48.6 |

With the vectors a player can compute, DLSS SR stays **less faithful than Catmull-Rom on
clean film**: 3.8 dB below on still pictures, 1.5 dB below on moving ones, where the refined
global motion took 1.3 dB back. On compressed film it is level with it on still pictures, 0.4
dB below, and **1.9 dB above on moving ones**; on grained film it is **3.6 dB above on still
pictures and 5.7 dB above on moving ones**: where the source carries noise, the reconstruction
is worth more than the fidelity it costs. It also **takes about 73 % of the film grain out**
over time, and the grain it leaves flickers 6.3 times less (0.87 against 5.45 between two still
frames, ×1000). That degraining and denoising is what DLSS 4.5 gives a film beyond the
enlargement, and on a grainy source it is what you will notice first.
The cleaned vectors gain 0.2 to 0.6 dB on still pictures and 1.3 to 2.4 dB on moving ones over
the raw vectors of 1.2, and leave 44 % less grain. Without jitter a still picture gives DLSS
nothing new, and Optical Flow is not precise enough for it to accumulate as much detail on a
pan as exact motion does: finer flow — the source size, a vector per pixel, the slowest search —
measured no better. The option is therefore off by default and marked experimental; your eyes
decide whether its cleaner look is worth it on your films.

---

## Upscaling: FSRCNNX, RAVU-zoom and ArtCNN

The **Upscaling** and **Chroma upsampling** lists are ordered by what they measured,
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
They need Direct3D 11 at feature level 11.0 (the passes are shader model 5, ArtCNN's in
compute); on Direct3D 9, on older hardware, and while DLSS Super Resolution is enlarging the
picture, Catmull-Rom stands in.

**They run at any enlargement, however small.** Each of these shaders carries mpv's `//!WHEN`
clause, which asks whether it is worth starting at the size the window happens to want:
FSRCNNX and ArtCNN want 1.3× before they will, RAVU-zoom wants the picture to be growing at
all. That is mpv's economy and it is not this renderer's. A film at 1920x1038 shown at
2496x1349 is enlarged by exactly 1.3, and every one of them stood aside there while the list
went on naming the network and Catmull-Rom took the whole picture. The question is now put at
the network's own scale -- twice the plane, which is what they produce -- so the method named
in the list is the method that runs (`CMpvShader::MakePlan`). Nothing structural is bypassed:
both conditions these shaders carry are "is it worth it", never "can it".

It costs the same at every ratio, because the work is the same: the network doubles the source
whatever the target is. At 1920x1038 to 2496x1349 on an RTX 3050, per frame — FSRCNNX 8 10.0
ms, ArtCNN 19.5 ms, FSRCNNX 16 22.1 ms, RAVU-zoom 6.6 ms.

**They run only where there is something to enlarge**: no axis shrinking and at least one
growing. A PAL film is 720x576 shown at 768x576, enlarged in width alone, and that is enough.
A picture shown at its own size gets no enlarger at all, and one that grows on one axis and
shrinks on the other goes to the resize shaders, one kernel an axis.

**Use a lighter filter for small enlargements** is for the case that rule makes expensive. A
4K film cropped to 3838 across on a 3840 screen is an enlargement, by a twentieth of one per
cent, and a network doubles its source whatever the target is: on an RTX 3050, at 3836x2160
to 3840x2160, FSRCNNX 16 AR worked at 7672x4320 for 1024 ms a frame and skipped half of them.
Ticked, the list under it is used instead whenever the picture grows by less than 5 % on both
axes -- RAVU-zoom took 21.4 ms there and skipped nothing, Jinc2m nothing worth measuring -- and
the method chosen above is used everywhere else. The statistics name the one really in use.
It goes by the ratio and not by the size, so a 1080p film at 2000x1125 takes the lighter one
too, and at exactly 5 % (2016x1134) the method above is back. Off by default.

**They do not run while the picture is being reduced.** A 2x network asked to reduce has to
double the source first and the picture is then brought down from that, which on a 4K film in
a 1080p window had FSRCNNX 16 working at 7680x4320 for 93.4 ms a frame -- the sync offset ran
out to +5 s with 18 frames late. The option that used to ask for it, *Use the "Upscaling"
method to reduce the frame to 50%*, is gone with it: **the Downscaling method now reduces at
every ratio**, which is the one thing it can be relied on to say.

### Spline36, and what a downscaler is for

The **Downscaling** list reduces whenever the shaders do the resizing, at every ratio. It
gained **Spline36**, the piecewise cubic of Avisynth's `Spline36Resize`, which mpv carries as
its `spline36` kernel: six taps like Lanczos, in the same convolution downscaler as the rest
(`Shaders/resize/convolution_filters.hlsl`, `FILTER=5`). It is appended at the end of the list
rather than placed by quality, because that list saves the position it is sitting on and
anything else would move every saved setting one along.

A downscaler has two jobs that pull against each other. It must keep the detail that still
fits in the smaller picture, and it must throw away the detail that no longer does -- because
whatever it fails to throw away does not disappear, it folds back into the picture as moire,
and it crawls when the picture moves. Nothing else about it matters much.

`dlssnr_harness --tdownscale` measures both on the shaders that ship, with no reference kernel
anywhere: the test pictures are sinusoids whose correct reduction is known in closed form. One
below the output's Nyquist should come back as itself, so what comes out says how much of it
survived; one above it cannot exist at the smaller size at all, so the correct answer is flat
and anything left is aliasing. A step edge gives the overshoot. 4K reduced by two, the
frequencies in cycles per output pixel:

| | 0.125 | 0.250 | 0.375 | 0.60 | 0.75 | 0.90 | halo | error |
|---|---|---|---|---|---|---|---|---|
| | *kept* | *kept* | *kept* | *folded* | *folded* | *folded* | | |
| **Lanczos** | 1.003 | 1.010 | 0.881 | 0.178 | 0.010 | 0.004 | 0.045 | **0.087** |
| **Bicubic sharp** | 1.063 | 1.118 | 0.936 | 0.136 | 0.118 | 0.044 | 0.075 | **0.097** |
| **Spline36** | 1.000 | 0.993 | 0.856 | 0.200 | 0.007 | 0.004 | 0.040 | **0.101** |
| Bicubic | 0.995 | 0.941 | 0.772 | 0.276 | 0.058 | 0.004 | 0.025 | 0.150 |
| Bilinear | 0.962 | 0.853 | 0.691 | 0.346 | 0.146 | 0.024 | 0.000 | 0.208 |
| Hamming *(default)* | 0.968 | 0.880 | 0.748 | 0.467 | 0.303 | 0.204 | 0.000 | 0.268 |
| Box | 0.981 | 0.924 | 0.831 | 0.588 | 0.382 | 0.153 | 0.000 | 0.303 |

`error` is the root mean square of all six, which weighs nothing by hand: a kernel that hands
back more contrast than it was given is as wrong as one that loses some, and says so here
instead of being rewarded for looking sharper. The list is in that order.

**The top three are within fifteen per cent of each other and differ in taste rather than in
worth.** Spline36 is the flattest in band -- 1.000 and 0.993, neither softening nor inventing
-- and rings least of the three, and pays for it with the most folded. Bicubic sharp folds
least and buys that with 1.118 at a quarter of Nyquist, which is contrast the source did not
have, and with the worst halo of all seven. Lanczos sits between them and wins the arithmetic
by a nose. Any of the three is a good answer; the four below are not close.

**Hamming, which is the default, is sixth of seven**, and it is upstream's default rather than
a measured choice. It lets through 0.467 of a signal at 0.60 and 0.204 at 0.90 -- four and
fifty times what Lanczos does -- and that is the aliasing that crawls. The default is left
where upstream put it; changing it is a decision for whoever ships the build.

Nothing in the table is free of the shaders themselves, and that the shipped shaders really
are the kernels they are written from is a separate question. `dlssnr_harness --tdownport` is
the one that asks it: it loads the compiled objects the filter embeds -- not the source
rebuilt for the occasion -- runs them on the GPU through the renderer's own constant block and
point sampler, and compares against the same convolution worked out on the CPU in double, with
the same clamping at the edges and the same rounding to half between the two passes. All seven
agree to about one step of half over 50,000 to 100,000 values at each of three ratios.

Two things it has to be careful about, and both are stated in its output. A tap landing exactly
on a branch point of its kernel is a tie: the shader decides it in float from an interpolated
coordinate and the reference decides it in double from an arithmetic one, and for Box, whose
interval is half-open at 0.5, that moves a whole texel. Those pixels are set aside and counted
rather than judged, and at a ratio of exactly two every tap of every kernel is one, which is
why the ratios tested are beside it and not on it. And what a weighted sum is known to depends
on how much it cancels, so the tolerance scales with sum|w| over |sum w| -- Lanczos and the two
bicubics give their taps both signs.

One thing it found and nobody has explained: at 1.11x, 262 of 112,896 of Lanczos's values sit
further out than rounding accounts for here, by up to 1.3 % of scale, where the taps are
nowhere near a branch point and the sum barely cancels. It is two orders below anything a
mistyped coefficient would do, so the check does not call it a failure -- it prints it, which
is the honest state of it.

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

The statistics show what they cost on the step they serve, a luma prescaler on *Scaling* with
the resize that finishes its work, a chroma one on *Convert* or *Chroma* (an 800x450 picture
in a 1280x720 window, `playback_test`):

    1 Scaling     : 800x450 -> 1280x720 FSRCNNX 16 AR                           15.4

Only what runs is listed. When the hardware video processor handles the source format it
converts the picture itself, chroma included, so the **Chroma upsampling** list has nothing to
do and no chroma step appears; the processor's step says so, and that it resizes too (a 1080p
film in a 1280x720 window):

    1 Processor   : D3D11 VP to R10G10B10A2_UNORM, converts chroma, resizes      0.5

The settings page greys a list that is out of service for that reason: **Chroma upsampling**
while the video processor converts, **Upscaling** and **Downscaling** while it also resizes
(*Use for resizing*, with or without DLSS 5 NR, which leaves the resizing to whoever had it),
and **Upscaling** while DLSS SR enlarges, which takes the resizing for itself.

Every option carries a tooltip, and a greyed one carries it too -- that is the one worth
reading, since it says why the option is greyed.

It reads those from the settings and from nothing else, with one exception given below. The
renderer can also say what it is doing with the picture at hand, and the page used to listen;
that read well and worked badly. A 4K film on a 4K screen leaves the processor nothing to
resize, so the page moved under you as the film changed -- and it put *Video Super
Resolution* out of reach, because it greyed whatever the pre-pass box said, and that box is
the one it is supposed to answer. These are settings for every film there will ever be, so the
film playing now does not get a vote.

### The exception: Dolby Vision

Dolby Vision is not a matter of degree. The picture carries Dolby Vision's own colour and its
own reshaping, which a fixed-function video processor cannot convert, so the renderer hands
the whole chain to the shaders and the processor is not in it at all
(`DX11VideoProcessor.cpp`, `disableD3D11VP`). While that is what is playing, the page says so:
*Use the video processor for resizing* greys, with a line under it explaining why, and
**Video Super Resolution** greys with it -- that extension lives inside the processor.
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
format boxes and of the chroma replacement. What it also decides is whether
**Video Super Resolution** can do anything at all: that extension lives inside the video
processor and is part of its resizing, so it greys without the box, greys while DLSS SR
enlarges in the processor's place, and greys while the chroma pre-pass is on, because the
processor is then handed a 4:4:4 picture the driver will not touch. The driver is still asked
for it exactly as upstream asks -- nothing is taken away from it here -- but a control that
cannot change the picture should say so rather than look available, and the statistics read
`D3D11 (No Super Resolution when chroma by shaders)`.

### Who resizes while DLSS runs

One thing resizes the picture, and the boxes say which:

| Ticked | The picture is resized by |
|---|---|
| *Use DLSS Super Resolution for upscaling* | DLSS Super Resolution, whatever else is ticked. The shaders finish what it does not cover, and reduce |
| *Use the video processor for resizing* | the video processor, with the driver's Super Resolution when it is requested. **Upscaling**, **Downscaling** and *Use a lighter filter for small enlargements* grey |
| neither | the shaders: the **Upscaling** and **Downscaling** lists. *Video Super Resolution* greys |

DLSS 5 NR is not in that table. It is a step of its own, at the source's size, and it leaves
the resizing to whoever had it:

    video processor    converts to R'G'B', RTX Video HDR     at the source's size
    DLSS 5 NR          the network, then its stabilizer      at the source's size
    resizing           one of the three above                to the size shown

It did not use to. The processor comes before the network and was held at the source's size
for it, and nothing handed it the picture afterwards: the shaders enlarged whatever the box
said, the page brought the lists back to say so, and the driver's Super Resolution, still
asked for, ran before the network on a picture it had nothing to enlarge -- **2.29** levels of
change on a 960x540 picture, against 4.02 when it doubles it (`vp444_probe`), and **16 ms** a
picture at 1080p on an RTX 4060. With FSRCNNX 16 after the network that was enough to take a
25 fps film from 25 pictures a second to 18.

So the processor is given a second turn (`CDX11VideoProcessor::VPScalePass`). A shader writes
the network's R'G'B' back as the Y'CbCr a video processor takes, in the space the first turn
left it in -- PQ BT.2020 when RTX Video HDR or an HDR10 film made it so, BT.709 otherwise --
and a second processor enlarges it. Nothing is converted on the way but the matrix, there and
back. In which format is decided by what Super Resolution needs, since the driver acts on
4:2:0 and on nothing else (it changes a picture written this way by 3.835 in P010 and by
0.000 in Y410):

| The second turn takes | When | What the trip costs a film frame | GPU, 1080p to 4K, RTX 4060 |
|---|---|---|---|
| Y410, 4:4:4 | no Super Resolution, or the chroma replacement on | under 0.5 dB, and levels within 0.16 of 255 in SDR, 0.29 in PQ | 0.9 ms |
| P010, 4:2:0 | Super Resolution requested, and the driver takes it on a picture that size | 2.2 to 3.1 dB, and 2.3 to 4.0 dB of chroma where the luma has an edge | 13.7 ms, Super Resolution included |

The tooltip of *Video Super Resolution* says so, since that is where the choice is made.

The P010 row is 4:2:0 met twice: the film's own, and the one Super Resolution asks for. It is
measured on film frames halved from 4K, against the same frame through the processor once
(`vp444_probe --after`). That is the price of the driver's sharpening after the network, and
it is on the control's side of the choice: with it set to *Disable* the picture goes round in
4:4:4 and keeps what it had.

In MPC-HC, a 1080p film at 25 fps on a television at 25 Hz, RTX Video HDR on: with DLSS 5 NR
and the processor enlarging, a picture is ready 26 ms after it arrives, and 36 to 39 ms with
Super Resolution as well (the *Render ahead* line) -- of the 40 ms a frame lasts. That is the
edge, on an RTX 4060: over stretches of a hundred pictures one run left none out and another
up to 7. The statistics give the second turn its figure on the Scaling step, which is what it is -- in
the bench, a 1920x800 film in a 2880x1200 window, DLSS 5 NR at 55 %:

    3 Scaling     : 1920x800 -> 2880x1200 D3D11 SuperResolution*                10.7

Four cases stand apart:

- **Apply after upscaling.** The order is the other way round and the processor has one turn
  only: it converts and enlarges, Super Resolution with it, and the network works on the
  picture at the size shown.
- **Nothing to resize.** A picture already the size it is shown at is not sent round, and
  with DLSS on Super Resolution is then not asked for at all. Without DLSS the question is
  upstream's, and upstream asks at that size too.
- **The network fails** (see *An evaluation that fails*). The second turn goes on resizing
  the pictures the network is not there for, so the picture does not change hands twice in
  two seconds: made to fail every hundredth evaluation, 50 pictures went without the network
  between two stretches of 99, and the processor enlarged every one of them.
- **The second turn cannot be made**, or the processor refuses a picture. It is given up
  until a setting changes, the shaders enlarge with the method the **Upscaling** list holds,
  and the *Scaling* line names that method.

The battery holds it to that. `playback_test --afternr` plays a real film through DLSS 5 NR,
pauses it at the same place under each way of enlarging, and reads the still off the desktop:
against the shaders, the processor's picture has to be the same colours, block by block.
On a 10-bit film, 1920x800 into 2880x1200, as an SDR picture and as a PQ one:

| Enlarged by | Each channel's shift, 8-bit levels | 16x16 blocks, mean difference |
|---|---|---|
| the processor, Y410 | 0.14 at most, 0.17 in PQ | 0.12, and 0.11 in PQ |
| the processor, P010 and Super Resolution | 0.38 at most, 0.67 in PQ | 0.61, and 0.68 in PQ |

The second row is the driver's sharpening, which is the point of it. A matrix or a range
read wrongly would be several levels in both columns.

**Use this method instead of the video processor's chroma upsampling**, under the list, the
chroma replacement, moves that one job to the shaders without
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
  upstream asks -- nothing is taken away from it here -- but *Video Super Resolution* greys
  while this pre-pass is in service, and the scaling line reads `D3D11 (No Super Resolution
  when chroma by shaders)` instead of starring work nobody did;
- **deinterlacing** works on NV12 only on this driver, so interlaced video keeps the
  processor's own chroma; 4:4:4 and RGB sources have no chroma to rebuild and are left alone;
- **HDR passthrough** is unaffected: the processor answers that it converts a PQ picture in
  AYUV, Y410 or Y416 to PQ RGB just as it does one in NV12 or P010 (`vp444_probe --hdrconv`).

The statistics say which is happening: the chroma pass is a step of its own, before the
processor, with what it costs (a 1920x800 film in a 2880x1200 window, Jinc):

    1 Chroma      : 4:2:0 -> 4:4:4 Jinc (EWA), for the processor                 2.8
    2 Processor   : D3D11 VP to R10G10B10A2_UNORM, resizes                       0.6

and an interlaced film, which keeps the processor's chroma, reads `converts chroma
(interlaced)` on the Processor step.

The box needs Direct3D 11 and costs the chroma pass, which the Chroma step times: a few tenths
of a millisecond at 1080p for Catmull-Rom and the simpler kernels, more for Jinc (2.8 ms on the
1920x800 film above), about 3 ms for RAVU-zoom, which enlarges Cb and Cr through its own
shader first.

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

**Render ahead to keep audio sync** (Presentation tab, on by default) measures each picture from
the start of its processing to the GPU being done with it, and starts the next pictures earlier
by the slowest of the last 32 plus 3 ms. Each picture then waits, while the GPU finishes it,
for the moment the renderer presents at without DLSS: half a refresh before its time, on the
reference clock. Presentation stays on the audio clock. It never waits for the GPU itself: a
picture not finished at its present time is presented anyway, reaches the screen once the GPU is
done, as it would have without render ahead, and moves the start earlier. A picture cannot start
before the previous one has been presented, so no more than a frame of drawing is ever to be
had.

That was all it did, and only while DLSS or a luma prescaler ran. It now also chooses the
refresh each picture is shown on, for every film: see *A refresh chosen for every picture*
below, which is where the numbers that matter on a television are.

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

The statistics show where the time goes and what render ahead does (see *Reading the
statistics* for the rest):

    2 DLSS 5 NR   : cinematic/P0 i1.50 t0.30 s0.50 k0.90 auto                   16.5
        stabilizer: 100, Optical Flow 960x540, vectors to DLSS                   2.0
    3 Scaling     : 1920x1080 -> 3840x2160 DLSS SR Perf/M                        6.1
        motion    : Optical Flow, global motion                                  1.8
    Render ahead  : asked  42 ms early, ready in  30.6 (max  31.3), 8 not in time

*ready in* is the whole picture, from the start of processing to the GPU being done: the mean
and the slowest of the last 32. *not in time* counts pictures the GPU had not finished at their
present time. A few at start-up are normal; a count that keeps growing means the chain barely fits the
frame rate, and a lighter DLSS SR preset gives it room. DLSS 5 NR is timed where the renderer
waits for it, the other steps with GPU timestamps while the statistics are shown.

### A film at the screen's own rate

The **Sync offset** is taken when `Present` returns, which is when the picture has been
queued and not when it is shown. The statistics now say the other thing too:

    Sync offset   :  -7 ms
    On screen     :  +6 ms, queue 1

`On screen` is the same offset carried forward to the refresh the picture is due on, from what
DXGI reports (`GetFrameStatistics`), and `queue` is how many pictures have been presented and
are still waiting for one.

It matters on a screen that refreshes exactly as often as the film has pictures: 24 fps at 24
Hz, 25 at 25. DXGI lets three pictures wait unless told otherwise and `Present` returns at once
while there is room. At 60 Hz there are two and a half refreshes for every picture and the
room is never used. With one refresh a picture, the first pictures of a film arrive in a burst
and fill it, and nothing ever empties it again -- a pause and play does, which is why that
cured it. Measured with a 30 fps film on a screen at 30 Hz (`playback_test --hz 30 --fps 30
--jolt`): the queue sat at 3 and the picture reached the screen **72 to 92 ms** after its
time, with the Sync offset reading -7 throughout.

The renderer now asks DXGI for one picture waiting and no more
(`IDXGIDevice1::SetMaximumFrameLatency(1)`). Same test: queue 1, on screen +43 ms. At 60 Hz
nothing moves -- sync offset -7.0 to -7.1 ms across the battery, on screen within 6 ms of the
picture's time, nothing skipped.

That bounds the queue and does not bring the film back. With one picture waiting, a picture
is presented once the one before it has been shown and is shown a refresh later, so a film
that starts late stays late by exactly as much for as long as it plays. Measured in a window
on a screen at the film's own rate (`playback_test --hz 24 --fps 24 --jolt`, and 25 at 25,
two runs each): queue 1 and nothing skipped, but the picture on screen **48 to 61 ms** after
its time, the same after 20 s as at the start, with the Sync offset reading +8 to +19.

So the renderer now leaves one picture out when that has lasted: when eight pictures running
have waited more than a refresh and a margin between `Present` and the refresh they are due
on, the next is drawn and not presented, and the one after takes the refresh before. Same
tests: on screen **+1 to +20 ms**, steady, Sync offset -7. The statistics count it:

    On screen     :  +3 ms, queue 1, caught up 1

One or two at the start of a film and one after an option was changed is what the tests show.
It is done once and judged: if the pictures that follow wait as long as before, leaving one out
was no use -- the GPU is not finishing them in time, or the screen does not refresh as it says
-- and nothing more is left out until the next seek; the line then reads `caught up 2 (no use,
stopped)`. Without that judge a television at 25 Hz lost two pictures a second. That case has
not been reproduced on the bench, so the judge is reasoned and not measured.
The measure is the wait and not the lateness, because a 23.976 fps film on 24 Hz slides through
every phase by a millisecond a second: in 75 s of it the offset slid down to -4, jumped
to +36 and slid down again, which is the picture a 24 Hz screen has to repeat, and nothing was
left out beyond the start and the option change. At 60 Hz none of this is reached with a film
(sync offset -7.0 and -6.5 ms, nothing skipped, as before); a 60 fps film on a 59.94 Hz
screen was caught up once in 40 s.

What is left is geometry: a picture can only appear on a refresh, the audio clock and the
screen's scan are not in step, and where a film lands between two refreshes is drawn afresh
every time playback starts. `On screen` is DXGI's account and stops at the swap chain: in a
window the compositor adds its own refresh, which nothing here measures.

Render ahead also no longer holds pictures back for a prescaler that is loaded and has nothing
to do, as on a film shown at its own size.

### A refresh chosen for every picture

Everything above times the call to `Present` and leaves the rest to the queue. Measured where
it matters -- MPC-HC itself, fullscreen on a television at 25 Hz, a 25 fps film through the
video processor with RTX Video HDR, with a line written for every picture (see *Diagnostic
tools*, the pacing trace) -- that was not enough, three times over.

**The picture was on the screen 80 ms after its time.** With *Wait for VBlank before Present*
the call is made on a vertical blank, and the picture appears two refreshes later: **+80.3 to
+81.3 ms**, every picture, while the Sync offset read +1.

**Where the offset sat was drawn afresh at every start.** The reference clock and the screen
are not in step, so the blank the renderer waits for falls anywhere in a frame. Nine starts in
a row, by a change of sound track, of subtitles or a seek: Sync offset −3, +24, 0, −3, +25, −1,
+33, +5, +18 ms, and the picture on the screen **+76 to +112 ms**.

**And in four of those nine a picture was shown twice every twelve**, 20 to 22 times in 10.5 s.
Called on the blank itself, a `Present` is at the one moment where DXGI's account of what is
waiting changes, and the catching up described above read it as a film a refresh late, left a
picture out, read the same again, and went on. That is the stutter after a change of track.

What decides is where in a refresh `Present` is called, which the same trace measured by
calling it a chosen time after a blank:

| `Present` called after a blank | 25 Hz: on the screen after | |
|---|---|---|
| 0 ms | 80.1 ms | the second blank from there |
| 6, 12, 18, 24, 30 ms | 73.9, 67.9, 61.8, 55.8, 49.9 ms | the same one |
| 35 ms | 44.8 ms, and 23 pictures of 283 a refresh later | |
| 38 ms | 81.8 ms | every picture a refresh later |

Anywhere from a blank until some milliseconds before the next, the picture is shown on the
blank after that one; in the last few it slips, and not every time. At 60 Hz the same: 13 ms
after a blank is in time, 15 ms is a refresh late. And the start of a refresh is no safer than
its end: in the bench's window a `Present` called 3 ms after a blank was in time or not from
one picture to the next, and every second or third then waited a whole refresh.

So with **Render ahead** on, the renderer now gives every picture a refresh in advance:

- DXGI gives the time of the refreshes, to a few microseconds from one to the next, and the
  renderer measures the screen's period from them rather than believing the display mode.
- The reference clock gives the moment the picture is due. The refresh nearest to it is the
  picture's.
- `Present` is called a third of a refresh after a blank, clear of both edges, as many
  refreshes before the picture's own as the queue is deep. How deep is measured: DXGI says
  on which refresh each `Present` ended up.
- The sample is asked for early enough for all that: 80 to 100 ms before its time at 25 Hz,
  depending on where the refreshes fall, which is what *Render ahead* in the statistics now
  reads for every film (less the 8 ms every sample is already given).
- A film with a whole number of refreshes per frame keeps that step from picture to picture,
  and is only moved -- a picture given a refresh more, or left out -- when the two clocks have
  drifted three quarters of a refresh apart. The nearest refresh alone would flicker between
  two whenever a picture's time fell half way.
- A redraw the player asks for while the film plays -- its message over the picture after a
  change of track -- is no longer a `Present` of its own: it landed in the same refresh as a
  picture's, which then waited a refresh, and so did every picture after it.

Same player, same film, same settings, six starts in a row by the same changes and a pause:

| | before | now |
|---|---|---|
| On the screen, against the picture's time | +76 to +112 ms | **−8 to +13 ms** |
| Pictures shown twice while playing | 20 to 22 in 10.5 s, four starts in nine | none in 168 s |
| Pictures lost at a start | none, the film ran late instead | 2 to 5, in its first 0.1 s |

The statistics say it in the two lines that used to disagree:

    Frame sync    : a refresh chosen for each picture
    Sync offset   :  -7 ms
    On screen     :  -8 ms, scheduled: repeated 0, dropped 0, shown late 0

*Sync offset* is then the refresh chosen against the picture's time, known before it happens,
and *On screen* what DXGI reported of a picture already shown, which is the proof of it.
*repeated*, *dropped* and *shown late* count what happened while the film played, a start's first
pictures left aside: a picture given a refresh more or left out to keep the step, and one that
reached the screen later than chosen.

What it cannot do:

- **A film loses two to five pictures when it starts**, after a seek and after a pause: they
  are due before they can be on the screen, and showing them late is what made the film late.
- **The two clocks drift.** When they do, a picture is repeated or left out once per refresh
  of drift, and nothing a renderer does can spare it: the sound would have to be resampled.
  How much depends on the display mode. On the machine this was written on, a 25 fps film at
  25 Hz hardly moved: seven minutes with the offset at +11.5 ms, to a tenth. A 23.976 fps
  film in 4K HDR at 23.976 Hz, thirty minutes: 43 086 pictures in a row each on its own
  refresh, none repeated and none left out, and the offset going from −14 to +4 ms, 0.65 ms
  a minute. That is one picture left out every 64 minutes, the first when the offset has
  reached three quarters of a refresh.
- **The compositor can miss a refresh**, which shows as one picture shown twice. Seen twice
  in seven minutes with the video processor doing Super Resolution and RTX Video HDR at 4K,
  20 ms of GPU a picture out of 40. The schedule then calls the following pictures a refresh
  later, or leaves one out when that would put them too far from their time, and is back in
  step within two pictures. It used to stay a refresh late for good.
- **A picture that costs most of a frame to draw** cannot be left out to any purpose: it has
  been drawn by then, and the next one is no sooner for it. With DLSS 5 NR on a 25 fps film
  that turned the rule against itself, twice. At 52 ms a picture, every one came behind its
  time and every one was left out: 744 of 767, a screen that moved once in two seconds. At
  33 ms, on a 25 Hz screen, one picture a refresh late was followed by one left out, whose
  own drawing made the next a refresh late again, and the film ran at 12.5 pictures a second
  for as long as it played, 463 left out of 944. So the schedule asks first whether the next
  picture, drawn after this one, would still be in time for its refresh. When it would not,
  this one is shown where it still can be and it is the next *sample* that goes, before
  anything is spent on it. Same film, same settings: 761 pictures in a row each on its own
  refresh, 3 left out as the film started. And a chain that cannot keep up at all -- 47.5 ms
  a picture -- shows what it can, 18.3 pictures a second with 228 of 843 left out undrawn,
  and stays with the sound: on the screen 12 ms after its time, where it had been 288 ms.
  *dropped* in the statistics counts both kinds.
- **It needs the flip model.** The *Discard* swap effect keeps no account of the refreshes,
  and the statistics then say `not scheduled (Swap effect: Discard)`; pictures are presented
  as before. An HDR picture is presented with the flip model whatever the setting says.
- **After a change of display mode the compositor takes a moment to settle**: a second or
  two in a window, during which it takes fewer than a picture a refresh whatever is done.

**A third buffer for an SDR picture.** Upstream gives the flip model two buffers when the
output is SDR and six when it is HDR. Two are the one on the screen and the one presented, so
the next picture is drawn into the buffer still on the screen, and the GPU holds that draw --
with everything queued behind it on the device -- until the flip. A renderer that waits for
nothing never sees it. One that waits for the GPU does, and waits for the next refresh with
it. In MPC-HC, a 1080p film at 25 fps with DLSS 5 NR and no RTX Video HDR, on an RTX 4060:

| | two buffers | three |
|---|---|---|
| fullscreen at 25 Hz: DLSS 5 NR waits for its input / a picture is drawn in | 21.9 ms / 39.2 ms | 1.6 ms / 18.7 ms |
| exclusive fullscreen at 25 Hz | 22.5 ms / 39.2 ms | 1.6 ms / 18.7 ms |
| in a window, the screen at 60 Hz | 11.9 ms / 28.6 ms | 1.6 ms / 20.2 ms |
| with Super Resolution after the network, 25 Hz: pictures left out, repeated | 47 and 44 of 517 | 2 and 0 of 521 |
| no DLSS, FSRCNNX 16 AR by the shaders: a picture is ready in, counted late | 39.2 ms, 305 times | 17.7 ms, 4 times |

Six buffers measure as three do, and with nothing heavy in the chain two and three play the
same: 457 pictures presented of a film's first 18 seconds either way, 4 left out as it
starts. So an SDR picture gets three, and an HDR one keeps upstream's six.
`MPCVR_SWAPCHAIN_BUFFERS=2` in the player's environment gives the two back, to measure it
again.

With *Render ahead* off nothing of this is used and the renderer times its `Present` as it
did; *Wait for VBlank before Present* and *Adjust the frame presentation time* only apply then.

### A video delay, for a sound that arrives late

All of the above puts the picture where the reference clock says, and the clock is the sound
card's: it says when the sound leaves the computer, not when it is heard. An amplifier that
decodes Dolby Atmos behind the television takes its time over it, and nothing in a player sees
that. The picture is all a renderer can move, so **Video delay** (Presentation tab, *Audio sync*
box) shows every picture that many milliseconds after its time: later for a sound
that arrives after the picture, from −200 to +500 ms. A negative value shows the picture
earlier, for a television that takes longer over the picture than the sound does to arrive.

It is applied where a picture is given its time, so everything else works from there: the
sample is asked for that much later, the refresh nearest the delayed time is the picture's, and
*Sync offset* and *On screen* in the statistics go on reading against the time the picture is
due. A line of its own says the rest while a delay is set:

    Video delay   : +40 ms asked for, the picture +37 ms from its own time

Measured in MPC-HC, a 24 fps film in 4K HDR with its Atmos track bitstreamed, fullscreen at
24 Hz, four starts at each value, the pacing trace reading each picture's refresh against the
time stamped on it:

| Video delay | the picture on the screen, after its own time | pictures lost while playing |
|---|---|---|
| 0 | +1.4, −13.2, −7.8, −11.4 ms | none |
| +40 ms | +21.3, +37.1, +43.1, +35.0 ms | none |
| +80 ms | +57.9, +91.5, +84.6, +75.5 ms | none |

And in the bench's window, the screen at 60 Hz: +40.2 ms for +40, +100.0 for +100, +250.5 for
+250 and −99.9 for −100, medians over ten seconds.

**A picture only moves by whole refreshes.** At 24 Hz that is 41.7 ms, so the value asked for
is met to the nearest refresh, and which one is drawn afresh at every start and every seek:
the spread of the table, about twenty milliseconds either way. The line in the statistics says where
the picture really is. A screen at 120 Hz for a 24 fps film has a refresh every 8.3 ms, and
would hold the value five times closer.

The slider applies as it is dragged, since it is set by ear with a film playing, and the
schedule takes the new time at the next picture rather than keeping its step. The value is
kept out of the presets, like the DLL paths: it belongs to the room.

## When the graph's clock stops

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

## The pointer that stayed on the picture

A fault as old as the renderer, in MPC-HC in fullscreen: change the **HDR output** list (or the
swap effect), close the options, and the mouse pointer stays over the film for good instead of
going away after two seconds.

Those are the settings for which the renderer makes its video window again. Once it has set
the owner, MPC-HC disables whatever the renderer put in its video area, so that the mouse falls
through to its own window: that window is the one that counts two seconds and hides the
pointer. The window made again was enabled, as any new window is. It took the mouse for itself,
the player's window never saw it again, and nothing hid the pointer until the film was closed.

The replacement is now made disabled when the window it replaces was. A player that leaves the
renderer's window enabled sees no difference.

It was found by watching, not by reading. `mpchc_drive.exe --attach 40` follows a running
MPC-HC while a hand does the handling; the trace of the fault reads, in order: a `VRWindow`
made, the old one destroyed, and from then on the pointer `SHOWING` over a `VRWindow`, where
until then the window under it had always been the player's own. `mpchc_drive.exe <player>
<film> --combo 1044 --select -1,0,1` then brings it about by itself with the swap effect: on
the build before, the pointer shows for 4.5 s of 4.5 after each change; on this one it is
hidden after 1.2 s, as it is when nothing was changed.

A first attempt had blamed the thread. MPC-HC opens a film on a worker thread and the options
apply from the interface thread, so the window made again changed threads; it is now made on
the thread of the first (`playback_test --rewindow`, `vrwindow_threads.ps1`). That is tidier
and was not the fault: the pointer stayed exactly as before.

---

## Settings

All the settings are on one page of the renderer's properties, in eight tabs, and are stored
under `HKCU\Software\MPC-BE Filters\MPC Video Renderer`. Each tab opens on a line saying what it
is for, and each control has a tooltip saying what it does, when it applies and what it needs.
The tables below say the same, with the measurements the tooltips leave out. DLSS needs the
64-bit filter: in the 32-bit one the **DLSS 5 NR** tab is not there.

**Default**, at the bottom of the page, puts every tab back to its defaults, the video delay
included, except the DLSS settings, which have their own button on the DLSS 5 NR tab, and the
presets.

### Source

| Setting | Default | What it does |
|---|---|---|
| Use Direct3D 11 (recommended) | on (Windows 8 and later) | Renders with Direct3D 11 instead of Direct3D 9. Everything this fork adds needs it: DLSS, the neural upscalers, the chroma replacement, sharpening, HDR output and render ahead. On Windows 7 the video processor is not available with it |
| Bit depth | Auto (8 or 10-bit) | The textures the renderer works in. Auto takes 10-bit for a source above 8 bits and with RTX Video HDR, 8-bit otherwise. DLSS 5 NR works in 16-bit float whatever this says; RTX Video HDR does not run in 8-bit |
| GPU video processor: NV12, P010/P016, YUY2, Other supported formats | all ticked | The formats the hardware video processor converts. Hardware deinterlacing, RTX Video Super Resolution and RTX Video HDR work on these only. A format left unticked is converted and resized by the shaders, with the methods of the Chroma and Scaling tabs |
| Deinterlacing: Hardware | On | The video processor's deinterlacing, for an interlaced source in a format it converts. *On, with future frames* is a workaround for drivers on which interlaced video judders |
| Double the frame rate when deinterlacing | on | One picture a field: 50i plays as 50p, with twice the pictures to process. Needs hardware deinterlacing |
| Blend fields when the shaders convert (4:2:0) | off | An interlaced 4:2:0 source the shaders convert (its format unticked, or Dolby Vision) has its two fields blended rather than combed. Progressive video is untouched |

### Chroma

| Setting | Default | What it does |
|---|---|---|
| Chroma upsampling: Method | Catmull-Rom | Rebuilds the colour of 4:2:0 and 4:2:2 video at full resolution when the shaders convert the picture, or for the replacement below; greyed while the video processor converts every format. Ordered best first, as they measured (see *Upscaling* above), and each entry keeps its own number, so a setting already saved still means what it did. Jinc (EWA) is the best of them on film and stays inside the conversion shader; RAVU-zoom and FSRCNNX 8 AR put Cb and Cr through an mpv prescaler, FSRCNNX being the one for drawn lines. Those two need Direct3D 11 and 4:2:0 in planes; Catmull-Rom stands in elsewhere |
| Use this method instead of the video processor's chroma upsampling | off | The chroma replacement (see *Who resizes while DLSS runs*). The shaders rebuild the chroma of a progressive 4:2:0 or 4:2:2 picture with the method above and hand the video processor a 4:4:4 one, so the processor stays in the chain and RTX Video HDR goes on working. Interlaced video keeps the processor's chroma. It costs RTX Video Super Resolution, which the driver does not apply to a 4:4:4 picture: *Video Super Resolution* greys while this is ticked, and the statistics say `D3D11 (No Super Resolution when chroma by shaders)`. Needs Direct3D 11 |

### Scaling

One thing resizes the picture: DLSS Super Resolution if it is ticked, the video processor if
*Use the video processor for resizing* is, the Upscaling and Downscaling lists otherwise (see
*Who resizes while DLSS runs*). The lists grey when they have nothing to do.

| Setting | Default | What it does |
|---|---|---|
| Use the video processor for resizing | on | The video processor resizes instead of the shaders: fast, and below the Upscaling and Downscaling methods, which grey. With DLSS 5 NR on it enlarges what the network leaves. Greyed while a Dolby Vision film plays, which the shaders process whole |
| Video Super Resolution | Off | The driver's Super Resolution (NVIDIA RTX, or Intel UHD 610 and later) while the video processor enlarges, for sources up to the size chosen. Needs the processor resizing, every format ticked on the Source tab, Windows 10, Direct3D 11 and the 64-bit filter. Greyed while DLSS SR or the chroma replacement is on. With DLSS 5 NR it runs after the network on a 4:2:0 picture, halving the colour resolution again |
| Upscaling | Jinc2m | Enlarges when the shaders resize. ArtCNN C4F16 DS, RAVU-zoom and the four FSRCNNX entries enlarge the luma with a network; AR holds what it invented to the range the source covers. Ordered best first, each entry keeping its own number. The networks need Direct3D 11; Catmull-Rom stands in elsewhere and while DLSS SR is enlarging |
| Use a lighter filter for small enlargements | off, Jinc2m | Under 5 % on both sides, a slightly cropped 4K film on a 4K screen for instance, the method in the list beside it is used instead: a network costs as much there as for a large enlargement, for nothing visible. The statistics name the method really in use |
| Downscaling | Hamming | Reduces when the shaders resize. Ordered best first (`--tdownscale`, see *Spline36, and what a downscaler is for*): Lanczos, Bicubic sharp and Spline36 are close |
| Use DLSS Super Resolution for upscaling | off | Enlarges with DLSS Super Resolution, in place of the video processor and of the Upscaling method, which greys; it cleans grain and compression noise too. Up to 4 times; the Upscaling method finishes a larger enlargement. Works with or without DLSS 5 NR. Experimental (see *DLSS Super Resolution*): moving subjects can still shimmer on grainy film |
| Also run when no enlarging is needed (DLAA) | off | DLSS runs on a picture already at the screen's size, rebuilding it without enlarging it. A whole pass at the output size: 70 ms a frame at 4K on an RTX 3050, 14 ms at 1080p. Needs *Use DLSS Super Resolution* |
| Preset | Automatic | The DLSS model: J and K are DLSS 4's, L and M DLSS 4.5's; Automatic lets DLSS choose for the ratio. Applies on the next picture |
| DLL | empty | `nvngx_dlss.dll` or its folder. Empty means search next to the filter and up |

### Detail

| Setting | Default | What it does |
|---|---|---|
| Sharpening: Method | Off | *Adaptive-Sharpen* or *Unsharp + Clamp*, run after the resize, so it also reaches a film already at the screen's size. See *Sharpening* for what each is worth. Needs Direct3D 11 |
| Intensity | 3 | Five levels, and a level is the same amount of sharpening whichever method is chosen -- +4, +8, +13, +20 and +28 per cent of mean gradient, calibrated through the filter. 3 is where the quality measurements settle. Greyed while Sharpening is Off |
| Use dithering | on | A very fine noise before output, so that skies and fades to black show no banding |

### DLSS 5 NR

| Setting | Default | What it does |
|---|---|---|
| Enable DLSS 5 NR | off | The network on every picture, at the source's size; the resizing is unchanged and comes after it. Forces 16-bit float internal textures. Needs an NVIDIA RTX card, Direct3D 11 and `nvngx_dlssnr.dll` |
| Apply after upscaling | off | Runs at display resolution instead of the source's. Much heavier at 4K: four times the pixels of a 1080p film |
| Toggle key | F12 | Switches DLSS 5 NR on and off during playback. The filter swallows this key; *None* gives no shortcut |
| DLL | empty | Empty means search the locations listed above |
| Temporal stabilizer: Strength | 100 | 0–100. Where the history is trusted, the current picture weighs from 1 down to 0.25 at 100, the measured setting. Its Optical Flow also gives the network its motion vectors, cleaned as DLSS SR's are, whatever the strength above 0. 0 runs nothing, vectors included. Applies live |
| Optical Flow resolution | Auto | 1/4, 1/3, 1/2, Full or Auto: the fraction of the working picture Optical Flow measures its motion on. Auto takes 1, 2 or 4 from the working height for about 540 lines, as before the slider existed. See *A working size below the picture's* for what it changes. Applies on the next picture |
| Style | Default | Default / Natural / Cinematic. Applies live |
| Preset | 0 | **No effect with the DLL builds seen so far** — they ship a single network and every preset falls back to it |
| Auto mask | on | The network finds skin (faces, hands) by itself and gives it *Skin detail* rather than *Detail*. Off, the whole picture is treated alike and *Skin detail* does nothing: on a 1950 film, the output without it measured the same with Skin detail at 0, 0.50, 0.90 and 2.00, and with it the network changed a face by 19 thousandths of luma more at the defaults (`--tautomask`) |
| Intensity | 1.50 | 0 to 2.00. 1.00 is the network's own result; above it, its changes are amplified |
| Local tone | 0.30 | Low on purpose: the local terms amplify frame-to-frame variation |
| Detail | 0.50 | Fine detail added by the network |
| Skin detail | 0.90 | −1.00 to 2.00. On the skin Auto mask finds; nothing while it is off |
| Working size | 100 % | 50–100 %, in steps of 5. See *A working size below the picture's*: the network runs on the picture reduced, and its change is carried back onto the picture. Taken on Apply, which starts the network again. Registry `DlssNRWorkingScale` |

**Default** on this tab resets the tuning: Style, Preset, Auto mask, the four sliders under them,
the stabilizer and its Optical Flow resolution, *Apply after upscaling*, and on the Scaling tab
the DLSS SR preset and DLAA. It leaves alone whether DLSS 5 NR and DLSS SR are on, the toggle
key, both DLL paths, and the working size, which is what the card can carry.

### HDR

| Setting | Default | What it does |
|---|---|---|
| Prefer Dolby Vision over PQ and HLG | off | For a file that carries Dolby Vision and an HDR10 or HLG layer: the Dolby Vision layer is played, adapted to the output. Dolby Vision is processed whole by the shaders, without the video processor (see *The exception: Dolby Vision*). Off, the HDR10 or HLG layer is played |
| HDR output | Passthrough (Windows 10 and later) | *Passthrough* sends HDR video unchanged and the display tone maps it. *Tone map* adapts it to the peak in *Nits* and outputs HDR; BT.2390 uses Dolby Vision's dynamic metadata when the film has it. *Off (SDR)* outputs SDR. Needs Windows 10, Direct3D 11 and an HDR display with Windows HDR on, or allowed to turn on below. RTX Video HDR needs Passthrough |
| Nits | 1000 | The display's real peak, 100 to 10000, for the tone mapping methods |
| Windows HDR | Don't change | Whether the filter may turn Windows HDR on by itself when HDR video, or RTX Video HDR, starts: in fullscreen only, or always. *On/off* turns it back off afterwards |
| Subtitle and OSD brightness | the leftmost | How bright subtitles, the player's messages and the statistics are drawn over an HDR picture |
| RTX Video HDR | off | The NVIDIA driver turns SDR video into HDR. Needs HDR output on Passthrough, an HDR display with Windows HDR on, an RTX card, the 64-bit filter, a bit depth other than 8-bit and a format the video processor converts. The statistics star it (`RTX Video HDR*`) while it runs |
| Convert to SDR | on | HDR video not output in HDR is converted to SDR, so that it does not look washed out |
| SDR brightness | 125 nits | The target of that conversion, 25 to 400. Applies as the slider is dragged |

### Presentation

| Setting | Default | What it does |
|---|---|---|
| Swap effect | Flip (recommended) | How pictures are handed to Windows. Render ahead needs Flip to give each picture a refresh. *Discard* is the older mode, for drivers that misbehave with Flip. HDR output always uses Flip |
| Use exclusive fullscreen | off | In fullscreen the filter takes the display directly instead of going through the desktop: less latency, but menus drawn over the film may not show and Alt+Tab is slower |
| Wait for VBlank before Present | off | Waits for the next refresh before each picture. Not used while render ahead schedules the pictures |
| Adjust the frame presentation time | on | Holds each picture until its time on the audio clock. Not used while render ahead schedules the pictures |
| Reinitialize device when changing display | off | Creates the Direct3D device again when the window moves to another screen |
| Render ahead to keep audio sync | on | See *Render ahead*. Gives every picture a refresh chosen in advance, and starts each early enough for it. Needs Direct3D 11, and the flip model for the first of the two. Registry `DlssRenderAhead` |
| Show statistics | off | The statistics over the picture; Ctrl+J in MPC-HC |
| Size | Fixed size | Whether the statistics keep their size or grow with the window |
| Video delay | 0 ms | See *A video delay, for a sound that arrives late*. Every picture shown that long after its time, −200 to +500 ms, to the nearest refresh. Applies as the slider is dragged; kept out of the presets |

### Presets

The **Presets** tab holds ten sets of settings. Each line has a name, a key, *Save* and
*Load*:

- *Save* keeps everything the page shows at that moment, applied or not, and writes it at
  once, without waiting for Apply.
- *Load* brings a preset back into the page and lights Apply; nothing is in force until
  Apply is pressed, so what was loaded can be read first. *Load* is greyed on a line nothing
  was saved in.
- The **key** recalls the preset while a film plays, without opening the options: the
  preset is applied, becomes the current settings, and its name stays on the picture for two
  seconds (Direct3D 11), larger than the statistics and on a dark box, paused or playing. The filter swallows the key, as it does the DLSS 5 NR toggle key,
  and a key already given to another preset or to that toggle is refused. A name or a key
  changed without *Save* is written on Apply.
- The key is two lists: what is **held** (nothing, Ctrl, Shift, Alt or any of them together)
  and the key itself (F1 to F12, the digits, the letters, the numeric pad, Home, End, Insert,
  Delete, Page Up, Page Down, Pause, Scroll Lock). Ctrl+1 and Shift+1 are two different keys,
  and neither is 1: the filter swallows the exact combination and leaves the rest to the
  player. While a text box is being typed in, it swallows nothing.

A preset is every setting there is, with four exceptions that belong to the installation or
to the moment and not to a taste: the two DLL paths, the DLSS 5 NR toggle key, the video
delay, and whether the statistics are showing -- a preset saved while they were up used to bring them up with it. Recalling one that changes
Direct3D 11, the swap effect, exclusive fullscreen or the texture format restarts the
rendering, the same short break those options cause from the page; between two presets that
differ only in the picture, the change is immediate. Measured with `playback_test --presets`,
which saves a Catmull-Rom preset on F6 in the first slot and a Lanczos3 one with
Adaptive-Sharpen on Ctrl+F7 in the last, and presses F7, Ctrl+F7, Ctrl+F6 and F6 during
playback: F7 alone and Ctrl+F6 change nothing, the Scaling line follows the other two, and no frame is skipped.

They are stored beside the settings, in `HKCU\Software\MPC-BE Filters\MPC Video
Renderer\Presets\1` to `\10`, under the same value names as the settings themselves plus
`Name` and `Key` (the virtual key, plus 0x100 for Shift, 0x200 for Ctrl, 0x400 for Alt), written and read by the same code: a setting added later is in the presets
without anyone thinking of it.

The feature is also reachable programmatically through `IExFilterConfig`:
`Flt_SetBool("dlssNR", true/false)` and `Flt_GetBool("dlssNR", &b)`. `Flt_GetString("statsText")`
returns the statistics as they are drawn on the picture, for tools that log them;
`playback_test` reads the steps by their names, whatever number they were given.

---

## Reading the statistics

The statistics (*Show statistics*, Ctrl+J in MPC-HC) follow a picture through the renderer:
what comes in, each step it goes through in the order the renderer takes them with what that
step costs, then how it reaches the screen. A step that does not run has no line. The
bench's account of a 1080p film at 25 fps in a 4K window, DLSS 5 NR and DLSS SR on, on the
RTX 4060 (`playback_test --file <film> --window 3840x2160 --only 4`):

    -- Source ----------------------------------------------------------------------
    Input format  : NV12 1920x1080
      Range: 16-235, Matrix: BT.709
      Primaries: BT.709, Function: BT.709
      ChromaLocation: Left(MPEG-2)
    Frame rate    :  25.000p in,  25.003 drawn
    -- Pipeline, in R16G16B16A16_FLOAT -------------------------------------- GPU ms
    1 Processor   : D3D11 VP to R10G10B10A2_UNORM, converts chroma               0.3
    2 DLSS 5 NR   : cinematic/P0 i1.50 t0.30 s0.50 k0.90 auto                   16.5
        stabilizer: 100, Optical Flow 960x540, vectors to DLSS                   2.0
    3 Scaling     : 1920x1080 -> 3840x2160 DLSS SR Perf/M                        6.1
        motion    : Optical Flow, global motion                                  1.8
    4 Finish      : Adaptive-Sharpen 4, dither                                   1.3
      Total       : for a frame of 40.0 ms                                      28.1
    -- Timing ----------------------------------------------------------------------
    Clock         : Audio renderer
    Render ahead  : asked  42 ms early, ready in  30.6 (max  31.3), 8 not in time
    CPU (ms)      : copy   0.2, draw  19.8, wait  25.9, Present   0.1
    Swap chain    : Flip discard, R10G10B10A2_UNORM, 3 buffers
    Frame sync    : a refresh chosen for each picture
    Sync offset   :   +4.9 ms
    On screen     :   -5.1 ms, scheduled: repeated 0, dropped 0, shown late 1
    Frames        :   362 received, skipped 0 late + 0 fields, failed 0

**The pipeline**, in the order a picture goes through it. The heading names the format the
steps work in.

| Step | What it says |
|---|---|
| Chroma | The chroma replacement: the shaders rebuild 4:2:0 or 4:2:2 colour with the Chroma upsampling method and hand the processor a 4:4:4 picture, a prescaler's time included |
| Processor | The video processor converts the picture, its chroma too unless the step above took it. When it is the one resizing it does that in the same call, the step says `resizes`, and its time is the resizing's too: the Scaling step then has none of its own. RTX Video Super Resolution and RTX Video HDR run inside it and are in its time. The processor works on an engine of its own, which the timestamps around it once missed altogether -- 0.0 ms with Super Resolution and RTX Video HDR at 4K, of 21.6 for the picture; it is now timed from its call to its output being ready, 17.4 ms of 20.8 there |
| Convert | The shaders convert the picture instead, with the chroma method named, a chroma prescaler's time included |
| DLSS 5 NR | The network's tuning, or why it is not running. *working* is the reduction and the composition at a working size below the picture's; *stabilizer* its Optical Flow and the stabilizer. The network runs on a Direct3D 12 device of its own and is timed there, by timestamps on its queue |
| Scaling | From what size to what size, and what does it, with all it costs: the processor's second turn after DLSS 5 NR, DLSS SR and its *motion*, or an mpv prescaler, each with the resize shader that finishes the job. `SuperResolution*` when the driver's Super Resolution runs, and `(No Super Resolution when chroma by shaders)` when it was asked for on a 4:4:4 picture. When the processor resized in its own step, a time here is only the resize shader copying the picture to its place -- a film with black bars, or one in a window -- and the line says `placed by the shaders`: 0.5 ms for a 2.39:1 film on a 4K screen, which is not Super Resolution's (that is in the processor's time). With *DLSS SR* ticked and not enlarging, a sub-line says why |
| Finish | What runs at the size the picture is shown at: a correction, the tone mapping, the sharpening and its level, the player's shaders, the dither |

Each time is the mean of the last 32 pictures, measured on the GPU with timestamps read back
later, never waited for, and only while the statistics are shown. A step whose text does not
fit before its time goes on under it, so that the column stays where it is. **Total** adds them up
against how long a picture lasts: the first thing to look at when pictures come late. A chain
whose total comes near the frame has no room left.

Under the total, unnumbered, **HDR** says what becomes of HDR: passthrough, tone mapping or
conversion to SDR, and RTX Video HDR. It is not a pass of its own and has no time: passthrough
computes nothing, RTX Video HDR is in the processor's time, and a tone mapping or a conversion
to SDR is in Finish's.

**The timing**, in the order it happens to a picture. Its figures change with every picture,
so each keeps its width -- the offsets to a tenth of a millisecond -- and a line does not move
as they change:

- *Clock*: whose clock every picture is scheduled against, normally the audio renderer's.
- *Render ahead* (see *Render ahead*): how early a sample is asked for, how long a picture then
  takes from its sample to the GPU being done with it (the mean and the slowest of the last 32,
  CPU and waits included, so above the total), and how many were not done at their present
  time.
- *CPU (ms)*: the last picture on the render thread. *copy* hands the sample to the GPU,
  *draw* issues every step, DLSS 5 NR's included, which it no longer waits for (it did
  until the two devices waited for each other on the GPU), *wait* is the wait for the moment to
  present -- a refresh chosen in advance, a vertical blank, the picture's time -- and
  *Present* the call itself. The wait was once counted as Present.
- *Swap chain* and *Frame sync*: the swap effect, its format and how many buffers it has, and
  what decides when each picture is presented.
- *Sync offset* and *On screen*: see *A film at the screen's own rate* and *A refresh chosen
  for every picture*. *Video delay* follows them when one is set.
- *Frames*: the samples received, those the renderer left undrawn for coming too late, the
  second fields of a deinterlaced picture left out, and the pictures that failed; *turns lost*
  and *turns late* when a picture's turn in the renderer was taken away or came late, the
  last late one's details on the line under.

The sync offset graph, bottom right, draws the *Sync offset* of the last pictures.

---

## Known limitations

**Some shimmer remains.** With Optical Flow about 1.1× the input's flicker is left on moving
content. Lowering **Local tone** and **Detail** still helps, which is why they default
low.

**Where Optical Flow is unsure** — large flat or dark areas, occlusions, transparency — trust
falls and those pixels get the network's output for that picture alone. The clamp keeps any
mistake within the local range of the effect.

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
does, so the filter's own classes (`DlssOpticalFlow.cpp`, `DlssStabilizer.cpp`,
`DlssCompose.cpp`) run unchanged.

```
dlssnr_harness.exe --temporal     motion vector conventions, depth, history
dlssnr_harness.exe --tmask        ControlMask values on still and moving content
dlssnr_harness.exe --toracle      a perfect motion map as the mask
dlssnr_harness.exe --teffect      how much of the effect each ControlMask value leaves
dlssnr_harness.exe --tflow        NVIDIA Optical Flow: direction, units, accuracy, timings
dlssnr_harness.exe --tstab        the post-DLSS stabilizer variants on real pictures, and the vectors the network
                                  is given: the shimmer, a moving object itself and the trail it leaves
dlssnr_harness.exe --tstabport    the filter's stabilizer class against those passes, redraws included
dlssnr_harness.exe --tstabbench   what the stabilizer costs at 1080p and 2160p, at each Optical Flow divider,
                                  with the network's cleaned vectors and without
dlssnr_harness.exe --tnrscale     DLSS 5 NR at a working size: each way of carrying its change back, and the cost
                                  (--nrflow N: the stabilizer's Optical Flow at N working pixels per flow pixel)
dlssnr_harness.exe --tpipeline    the whole DLSS chain as the renderer runs it: CPU and GPU time per stage
dlssnr_harness.exe --tsr          DLSS Super Resolution: bring-up, presets, scales, costs, NR next to it
dlssnr_harness.exe --tsrq         DLSS SR against the resize shaders on moving film frames
dlssnr_harness.exe --tsrstill     DLSS SR on still, grained pictures: shimmer, grain along the edges, moving subjects
dlssnr_harness.exe --tsrport      DLSS SR's vectors from the filter against the same made on the CPU, and their GPU time
dlssnr_harness.exe --tsrclip <dir> DLSS SR on a real film's frames: the shimmer of what holds still once the film's
                                  own motion is taken out, for each way of making the vectors and each preset
                                  (--tsrclipnr: through DLSS 5 NR first; --tsrclipwhich now,rob,...: those rows;
                                  --tsrcrops: a crop of every frame, to look at)
dlssnr_harness.exe --tsrcut <dir> DLSS SR across a film's cuts: the dark trail a bright shot kept of a darker one,
                                  with the cut detection, without it, and against a reset of DLSS on the cut, and
                                  the pictures the detection called a cut against the clip's own
                                  (--tsrcutnr: DLSS 5 NR and its stabilizer instead; --tsrcrops: the crops)
dlssnr_harness.exe --tautomask <dir> DLSS 5 NR with and without its auto mask on a film's frames: where the two
                                  differ, by kind of pixel, with maps of the difference (--skin N: skin detail x100)
dlssnr_harness.exe --tcutscan <film> the cut detection alone over minutes of a film, decoded by ffmpeg: its four
                                  measures on every picture, with ffmpeg's own scene score beside them
                                  (--cutfrom <s> --cutfor <s>: the stretch, 300 s by default)
dlssnr_harness.exe --tupscale     the resize shaders and the mpv prescalers on film frames
dlssnr_harness.exe --tupscalecost what EfRLFN costs at film sizes
dlssnr_harness.exe --tchroma      the chroma upsamplers on 4:2:0 made from film frames
dlssnr_harness.exe --tmpvport     the filter's prescaler runner against the harness's
                                  (ArtCNN included: it is the one made of compute passes)
```

One suite a call: the harness refuses two named together and says so, where it used to run
the first and leave the others out without a word. `--tframes N` sets the frames per run;
`--tstrong` uses the strongest network settings;
`--timage <file>` picks the photo for `--teffect`, `--tflow` and `--tstab` (a Windows 11
wallpaper by default), and the 4K film frame for `--tnrscale` (`upscale_refs\4K1.png`).
`--nonr` skips the DLSS 5 NR session for the suites that do not need it, `--srdll <path>`
points at `nvngx_dlss.dll`, and `--srrefs N` limits `--tsrq`,
`--tsrstill` and `--tsrport` to the first N references. `--tupscale`, `--tchroma`,
`--tmpvport` and the three DLSS SR suites read 4K film frames from
`tools/dlssnr_probe/upscale_refs/`. `--tupscale` also runs the mpv shaders it finds
translated under `tools/dlssnr_probe/upscalers/hlsl/` (`Shaders/mpv/mpv_shaders.py` puts them
there); the three the filter embeds need nothing but the build.

`--tsrclip` takes a folder of a film's frames as ffmpeg extracts them (`fNNNN.png`), and the
film's own motion from frame to frame, `motion.txt`, which `python clip_motion.py <folder>`
writes beside them: neither is in the repository. A film weaves, the whole picture moving by a
fraction of a pixel from one frame to the next, and a still wall can only be judged once that
motion is taken out.

`--tsrcut` takes the same kind of folder, a few seconds around a cut, and finds the cuts in it
itself. `--tcutscan` needs `ffmpeg` and `ffprobe` on the path, and writes `cutscan_<film>.csv`,
a line a picture.

A neural upscaler, EfRLFN, was measured too and not integrated: 922 ms per 1080p frame on the
RTX 3050 with DirectML (`--tupscalecost`), and no better than Catmull-Rom or Lanczos on clean
film frames (`--tupscale`).

**`vp_rebuild_test.exe`** — checks that rebuilding the hardware video processor keeps the
picture, which is what used to show a green frame when DLSS was toggled while paused.

**`shader444_test.exe`** — compiles the chroma pass the filter generates for the chroma
replacement, for each source format and each chroma method, since a compute shader has rules a
pixel shader has not. One that does not compile leaves the picture to the video processor
without a word, so nothing on screen says it happened.

**`vp444_probe.exe`** — asks the driver, through the renderer's own `CD3D11VP`, what the
video processor really does with each input format: which it takes, how its conversion
compares, and how much enabling Super Resolution, RTX Video HDR or deinterlacing changes the
picture, for 4:2:0 and 4:4:4 alike. `--film` runs it on film frames, `--pack` checks the
packing into AYUV and Y410 is exact, `--views` which bind flags still make an input view, and
`--hdrconv` which conversions the processor declares. `--after` is the processor's second
turn, after DLSS 5 NR: a picture written back from R'G'B' by the filter's own shaders, in P010
and in Y410, SDR and PQ -- whether the processor takes it, how exact the trip is, whether
Super Resolution acts on it, and what a second trip costs a film frame. The numbers quoted in
the chroma section above come from it.

**The pacing trace, and `pacing_report.py`** — what became of every picture on its way to the
screen, in any player. Set the environment variable `MPCVR_PACING_TRACE` to a file name before
the player starts and the renderer writes a line a picture there, when the stream is flushed
and when the player closes: when the sample arrived, was drawn and presented, what the
reference clock said, the refresh it was meant for, and DXGI's account of the swap chain before
and after the call. Nothing is written, and nothing is kept, without the variable.

```
set MPCVR_PACING_TRACE=C:\temp\pacing.csv
mpc-hc64.exe film.mkv
python pacing_report.py C:\temp\pacing.csv [--skip 3] [--rows 40] [--plot]
```

The report says, for each stretch between two seeks: the screen's real period and the clock's
rate against the performance counter; on which refresh each picture was shown and how many
were shown a refresh too long or not at all, with the times; how long after its own time each
reached the screen; where in a refresh `Present` was called and how long it took to return.
Every figure in *A refresh chosen for every picture* comes from it.

The same line says whether DLSS 5 NR and DLSS Super Resolution really ran on the picture,
which motion the stabilizer had, and where DLSS 5 NR's time went: the render thread waiting
for the network of the picture before, recording, and the network's own time on the GPU,
the picture before's. A note is written whenever what the two sessions say of themselves
changes. `pacing_report.py trace.csv --dlss` reads only that (playback_test --only 4, 1080p
to 4K):

```
  # dlss: NR on (ready 1920x1080 preset 0; stabilizer: Optical Flow 960x540) SR on (ready 1920x1080 -> 3840x2160, Performance, preset M)
      0.0 s to    18.3 s, film at      0.0 s:   428 picture(s), NR on   425 (flow 425, detector 0, vectors 425), SR on   425, enlarged by the processor     0, not shown 3, longest wait 722 ms
            drawing 1.9 ms (median), 77.6 at most; of which DLSS 5 NR waited 0.0 ms for the picture before, recorded for 0.5, and ran 14.2 on the GPU
```

A trace written before the two devices waited for each other on the GPU holds, in the same
columns, the render thread's waits for the input and for the GPU.

`pacing_report.py trace.csv --cuts` lists the pictures DLSS SR's cut detection called a cut, at
their time in the stream (from the last seek), and the ten that came closest without being
called: the detection's measures are read back from the GPU a few pictures later, never waited
for, and written on the line of the picture they belong to.

A session that stops by itself shows there as pictures DLSS no longer ran on under a note
that still asks for it, and nowhere else: the film goes on playing. To see what the renderer
does about it, `MPCVR_DLSSNR_FAIL_AT=100` in the player's environment makes every hundredth
evaluation of DLSS 5 NR give up (see *An evaluation that fails*).

The first note of a trace is the swap chain: its size, how many buffers it has and whether it
is exclusive. `MPCVR_SWAPCHAIN_BUFFERS=n` forces that count (see *A refresh chosen for every
picture*).

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
the video processor with the chroma replacement on, which is where a chroma pass that
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
playback_test.exe --greying       what the page greys, for every state the renderer can be in and each DLSS box
playback_test.exe --afternr --file <mkv> --seek 1740   who enlarges after DLSS 5 NR, on one still of a real film
playback_test.exe --vpturns --file <film> --rtxhdr 1 --hdrout 1   the shaders, the processor, the processor with Super Resolution: one still each, read off the desktop unclipped (vpturns_N.pfm)
playback_test.exe --hz 30 --fps 30 --jolt   a film at the screen's own rate, and the queue it builds
playback_test.exe --presets     two presets saved, each recalled by its key while it plays
playback_test.exe --rewindow    the video window made again from another thread, and whose it is
mpchc_drive.exe <mpc-hc64.exe> <film>   a real MPC-HC taken through the options; is the pointer hidden after?
mpchc_drive.exe --attach 40     watch a running MPC-HC: windows made or lost, pointer shown or hidden, each timed
dlssnr_harness.exe --tdownscale  what each downscaler keeps and what it lets fold back
dlssnr_harness.exe --tdownport   and that those shaders are the kernels they are written from
playback_test.exe --scalers --size 1920x1038 --window 2496x1349   every upscaler at a small ratio
playback_test.exe --scalers --size 3836x2160 --window 3840x2160 --small 5   the stand-in under 5 %
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
playback_test.exe --nrscale 70 --nrscaleto 100   DLSS 5 NR's working size, and another taken while it plays
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
| *Use the video processor for resizing*, "which DLSS 5 NR and DLSS SR suspend on their own": the scaling lists came back live with DLSS 5 NR ticked, *Request Super Resolution* greyed, and the statistics read `D3D11 SuperResolution*` over a picture the shaders had enlarged | DLSS 5 NR takes the resizing from nobody. The processor was held at the source's size for it and never handed the picture afterwards, so the shaders enlarged whatever the box said -- and the driver's Super Resolution, still asked for, ran before the network at the picture's own size: **2.29** levels of change with nothing to enlarge, and 16 ms a picture at 1080p. The processor now enlarges what the network leaves, Super Resolution with it, and the lists grey as the box says. Only DLSS SR takes the resizing (*Who resizes while DLSS runs*) |

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
