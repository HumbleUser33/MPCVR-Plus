# MPC Video Renderer + DLSS 5 Neural Rendering

A fork of [Aleksoid1978/VideoRenderer](https://github.com/Aleksoid1978/VideoRenderer)
that adds an optional NVIDIA **DLSS 5 Neural Rendering** pass to the Direct3D 11
pipeline. Nothing else changes: with the option off, the renderer behaves exactly
as upstream does.

**→ [What it does, what it needs, how to build it](README-DLSS5.md)**

------------------

To be clear on what's been changed :

DLSS 5 neural reconstruction applied to film, frame by frame. — it rebuilds the picture, somehow.
Comes with a temporal stabilizer driven by NVIDIA Optical Flow, to avoid shimmering.
A RTX 3050 is to slow, but a RTX 4060 can handle 1080p easy (with modded DLSS NR dll)
Option to apply DLSS 5 after upscaling, but very heavy on 4K, you should have RTX 4080 or 4090 (didn't test)

DLSS Super Resolution 4.5 for upscaling (experimental), fed motion vectors made for video. It degrains and denoises nicely as a side effect.

Six new upscalers, chosen by measurement
- ArtCNN C4F16 DS — could be better on real film (grain + compression), best for animes
- FSRCNNX 8 / 16 and RAVU-zoom — sharper edges than any classic filter
- Anti-ringing versions of FSRCNNX, which kill the bright halo networks leave along dark lines (4× less overshoot, and on drawn lines it's better, not just cleaner).
Both lists are now ordered best-first, but the choice is yours wether you prefer sharp or precise, etc. The list is ordered for compressed movies 1080p to 4K.

"Replace VP chroma upsampling" — Does Chroma Upsampling by shader before the GPU, to avoid bilinear chroma upsampling by GPU => hands the GPU's video processor a full 4:4:4 picture. On 10-bit film that's +7 dB of colour, it kills a colour shift the NVIDIA driver introduces, and it makes RTX Video HDR now work on 10-bit sources (it did nothing there before).

Three new methods for Chroma:
- Jinc (EWA): It's the only method better than Catmull-Rom on both the colour itself and the bleeding along edges, and it costs nothing extra: it runs inside a pass that was already there.
- FSRCNNX 8 AR is the opposite trade: slightly worse than Catmull-Rom on a photograph, and by far the best on drawn lines — it's the entry for animation.
- RAVU-zoom: also nice but not sharp at all

Render video ahead, so all of that stays locked to the audio clock — no late frames, no drift.

Nothing is lost: the processor keeps the picture, so deinterlacing, RTX Video Super Resolution and HDR passthrough all carry on. Interlaced video keeps the processor's own chroma, since it alone can deinterlace.

No more freeze when you disable RTX Video HDR, Super Resolution, etc. — now takes effect on the next frame, no restart.

Fixes Lanczos producing invalid pixels at an exact integer scale factor (×2, ×3…)

The default chroma upsampling moves from Bilinear to Catmull-Rom, but best is now Jinc (EWA)

The default luma upscaling is Jinc2m but I prefer less sharp (FSRCNNX 16 AR)

------------------

Two things to know before you start: the feature needs NVIDIA's `nvngx_dlssnr.dll`,
which is **not included here** and which you have to supply yourself, and it is x64
only.

For the unmodified renderer, go to [upstream](https://github.com/Aleksoid1978/VideoRenderer) —
that is where the releases, the issue tracker and the actual development are.

---

## About MPC Video Renderer

MPC Video Renderer is a free and open-source video renderer for DirectShow. The renderer can potentially work with any DirectShow player, but full support is available only in the MPC-BE. Recommended MPC-BE 1.8.9.106 or newer.

## Key features

* Can work with DXVA2 and Direct3D 11 hardware decoder.
* DVXA2 and Direct3D11 Video Processor with hardware de-interlacing for NV12, YUY2, P010 formats.
* Shader video processor for various YUV, RGB and grayscale formats.
* Various frame resizing algorithms, including Super Resolution.
* Subtitle and OSD display.
* Rotation and flip of the video frame.
* Dithering when the final color depth is reduced from 10/16 bits to 8 bits.
* HDR video support (HDR10, HLG and partially Dolby Vision).
* Automatic HDR to SDR conversion.
* Transferring HDR10 data to the display.

## Minimum system requirements

* An SSE2-capable CPU
* Windows 7¹ or newer
* DirectX 9.0c (PS 3.0) video card

¹For Windows 7, you must have D3DCompiler_47.dll file. It can be installed via update KB4019990.

## Recommended system requirements

* An SSE2-capable CPU
* Windows 10 or newer
* DirectX 10/11 video card

## License

MPC Video Renderer's code is licensed under [GPL v3]. This fork is a derivative work
and carries the same licence.

## Download

[Releases](https://github.com/Aleksoid1978/VideoRenderer/releases)

[Nightly builds](https://github.com/Aleksoid1978/VideoRenderer/wiki/Nightly-builds)

## Links

[Topic in MPC-BE forum (Russian)](https://mpc-be.org/forum/index.php?topic=381)

[MPC-BE](https://github.com/Aleksoid1978/MPC-BE)

## Donate

<https://mpc-be.org/forum/index.php?topic=240>
