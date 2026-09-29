# MPCVR-DLSS5

A fork of [Aleksoid1978/VideoRenderer](https://github.com/Aleksoid1978/VideoRenderer),
branched at its 0.10.7 release. It adds NVIDIA DLSS 5 neural reconstruction, a temporal
stabilizer, DLSS Super Resolution, mpv prescalers on luma and chroma, and a 4:4:4 chroma
pre-pass before the hardware video processor. `README-DLSS5.md` is the manual.

## Before changing anything, know this

- **Bringing in a new upstream version goes through [MERGING-UPSTREAM.md](MERGING-UPSTREAM.md).**
  Start with `python tools\upstream_check.py`, which is read-only and says what the merge
  will cost. Do not merge upstream by improvisation.

- **Resource numbers live in reserved ranges.** Upstream owns everything below 1900, and
  every control below 1200. Ours go in 4000-4019 (dialogs), 1900-1999 (shaders),
  2000-2099 (generated) and 1200-1299 (controls). The head of `Source/resource.h` says so,
  and section 5 of the pre-flight checks it. A number used by both sides compiles without
  a word and loads the wrong resource.

- **Some files are generated; do not edit them by hand.**
  `Shaders/mpv/<shader>/passNN.hlsl`, `Source/Upscale/MpvShaderTables.h` and the marked
  blocks of `Shaders/compile_shaders.cmd` and `Source/res/MpcVideoRenderer.rc2` all come
  from `python Shaders\mpv\mpv_shaders.py renderer`.

- **Settings are saved by name**, and the enum values in `Source/IVideoRenderer.h` are
  append-only. The Upscaling and Chroma upsampling lists are shown in measured quality
  order and carry their value as combo item data, so the display order can change without
  moving anybody's saved setting.

- **`IVideoRenderer` and `Settings_t` are append-only, at the end.** The interface keeps
  upstream's identifier, so a caller built against upstream's header finds methods by their
  place in the table and fields by their offset. One of ours inserted in the middle moves
  all of upstream's that follow, silently: `playback_test --file` prints a `settings kept:`
  line for exactly this.

- **A compute pass sets no render target.** If the caller has just drawn into a texture the
  pass reads, Direct3D drops that texture's shader resource view and the pass reads zeros
  (black for luma, green for chroma). Unbind the render targets before any dispatch placed
  after a draw. This has bitten twice.

## Building and measuring

```
build_mpcvr.cmd Build Release x64      REM and Win32; zero warnings expected
tools\dlssnr_probe\build.cmd           REM the measurement tools
tools\dlssnr_probe\run_all.cmd         REM the whole battery, one report
```

Compare the report with `tools/dlssnr_probe/baseline/`. "0 failures" is necessary and not
sufficient — the numbers are what tell you nothing regressed.

The tools compile the filter's own sources, so they measure the shipped code, not a copy of
it. `--tport`, `--tstabport`, `--tsrport` and `--tmpvport` run the filter's code and the
harness's own on the same picture and require the same result to the last half float.

## House style

Comments and commit messages say *why*, in plain English, in the voice the surrounding code
already uses. Numbers in the manual and in commit messages come from the tools in
`tools/dlssnr_probe`, measured on the machine at hand — never from memory or estimate.
