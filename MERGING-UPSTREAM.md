# Following upstream

This is a fork of [Aleksoid1978/VideoRenderer](https://github.com/Aleksoid1978/VideoRenderer),
branched at its **0.10.7** release. Upstream keeps moving, with its own bug fixes, and this
document is how those come here without losing anything of ours or breaking something in
silence.

## Why it is easier than it looks

Measured on 25 September 2026 against upstream as last fetched here, on 10 September —
`tools\upstream_check.py` recomputes every line of this, so take its report over the table:

| | |
|---|---|
| This fork's commits since 0.10.7 | 151 files, **+34 555 / −138** |
| of which files entirely its own | **113** — nothing can conflict there |
| of which upstream files changed | **38**, +3 246 / −138 |
| Upstream since 0.10.7 (26 commits) | 20 files, +130 / −74 |
| Files changed on **both** sides | **8** |
| Trial merge | **2 conflicts** |

Those 138 deleted lines are the whole story: this fork **adds**, it almost never replaces.
`git merge` therefore does nearly all the work by itself.

**So the danger is not the conflict — it is the silence.** Git will merge, cleanly and
without a word, code whose meaning upstream has changed underneath us. Nothing detects
that but reading. Step 2 below is that reading, and the battery in step 8 is what catches
what the reading missed.

And a thing worth saying plainly, because it is the usual worry: **git cannot forget one
of our changes.** The merge carries the whole branch by construction. What can be lost is
meaning, never code.

## The procedure

### 1. Look before you leap

```
git fetch upstream
python tools\upstream_check.py
```

The report is read-only — no fetch of its own, no merge, no file written. It gives you:
where we stand, the upstream commits that touch our files, a risk table, a **trial merge**
with the conflicts you will actually get, a check for resource numbers used by both sides,
and whether the generated files will need regenerating.

### 2. Read the commits it lists — the step that matters

For each commit in its section 2, open it and ask one question: **does it change the
meaning of something this fork is grafted into?** Not "does it conflict" — git answers
that — but "does our code still mean what it meant".

Take two real examples from the 26 commits waiting today:

- `77fd16e` *"SuperRes works only with 8-bit textures"* and `95502cd` *"Super Resolution
  on Nvidia can work with 10/16-bit textures"* — upstream reworked exactly the lines where
  this fork decides whether to ask for Super Resolution alongside RTX Video HDR and the
  4:4:4 pre-pass. A clean merge here would be a wrong merge.
- `46ed311` *"don't show SuperResolution\* in the statistics when nothing is being
  enlarged"* — our statistics line says the same kind of thing for its own reasons.

Note what you find. You will need it at step 4 and at step 8.

### 3. Branch

```
git checkout -b upstream-<version> dlss5
```

Never merge into `dlss5` directly: the branch is what lets you throw the attempt away.

### 4. Merge

```
git merge upstream/master        # or the release tag: git merge 0.10.9
```

Resolve with what you learned at step 2. `git rerere` is enabled, so each resolution is
remembered and replayed on the next merge — the second time round costs minutes.

If a resolution is not obvious, prefer **their structure with our addition inside it**
rather than the other way round: it keeps us close to upstream and makes the next merge
smaller.

### 5. Regenerate, if the report asked

The mpv prescaler passes, their tables, and marked blocks of `compile_shaders.cmd` and
`MpcVideoRenderer.rc2` are **generated**, not written by hand:

```
python Shaders\mpv\mpv_shaders.py renderer
Shaders\compile_shaders.cmd
```

### 6. Build both

```
build_mpcvr.cmd Build Release x64
build_mpcvr.cmd Build Release Win32
```

Zero warnings. x86 has no DLSS but must still compile: the DLSS pass is compiled out of it.

### 7. Rebuild the tools

```
tools\dlssnr_probe\build.cmd
```

They compile the filter's own sources (`DlssNR.cpp`, `DlssSR.cpp`, `MpvShader.cpp`…), so a
change upstream made to a shared header shows up here first.

### 8. Measure, and compare

```
cd tools\dlssnr_probe
run_all.cmd
python compare_report.py baseline\run_all_report.txt run_all_report.txt
```

One command to measure, one to compare. Every check reading zero is necessary and not
sufficient: a merge can pass everything and still cost half a decibel. `compare_report.py`
lines the two reports up row by row, ignores the timings — which say more about the
machine's mood than about the code — and prints only what moved by more than 0.1, plus
anything that **stopped being measured**, which is the worse failure of the two.

The four `port` suites earn their keep here: they run **the filter's code** and the
harness's own on the same picture and demand the same result to the last half float. If
upstream moved something under us, that is where it rings.

### 9. Play a real film

```
playback_test.exe --file <some.mkv> --seek 60
```

Then in MPC-BE, with `Ctrl+J` open: DLSS, the prescalers and the chroma method must be
announced as before, and the picture must look like it did.

### 10. Land it

Merge the branch into `dlss5`, replace the baseline with the new report (noting the card
and the date), update `README-DLSS5.md` if a feature changed, and package.

---

## What this fork adds, and what proves each of it

The answer to "did we keep everything?". Each line is a feature and the check that would
notice its absence.

| What the fork adds | Where it lives | What proves it |
|---|---|---|
| DLSS 5 neural reconstruction (NGX feature 18) | `Source/DLSS/DlssNR.cpp` | `--frames 300`, `--tport` |
| Temporal stabilizer on Optical Flow | `Source/DLSS/DlssStabilizer.cpp`, `DlssOpticalFlow.cpp` | `--tstab`, `--tstabport` |
| DLSS Super Resolution 4.5, with its own motion | `Source/DLSS/DlssSR.cpp`, `DlssMotionMask.cpp` | `--tsr`, `--tsrport`, `--tsrstill` |
| Render ahead | `Source/DX11VideoProcessor.cpp` | `playback_test --seconds` |
| The settings page, seven tabs in one | `Source/SettingsPage/` | `playback_test --mainpage`, `--frame` |
| mpv prescalers: FSRCNNX 8/16, RAVU-zoom, ArtCNN, the AR entries | `Source/Upscale/`, `Shaders/mpv/` | `--tmpvport`, `--tupscale`, `--scalers` |
| Chroma: Jinc (EWA), RAVU-zoom, FSRCNNX 8 AR | `Source/Shaders.cpp`, `Source/DX11VideoProcessor.cpp` | `--tchroma`, `--chroma`, `--chroma10` |
| 4:4:4 pre-pass ("Replace VP chroma upsampling") | `Source/DX11VideoProcessor.cpp`, `Source/Shaders.cpp` | `shader444_test`, `--chroma10`, `vp444_probe` |
| The flip model kept on a window (no more freeze) | `Source/DX11VideoProcessor.cpp` | `playback_test --toggle`, `--switch` |
| Every setting applied while the film plays | `Source/VideoRenderer.cpp`, `DX11VideoProcessor.cpp` | `--toggle`, `--switch`, `--gpu` |
| Lanczos weights kept finite at integer scale | `Shaders/d3d11/ps_interpolation_lanczos*.hlsl` | `--tupscale` |
| The paused picture kept across a processor rebuild | `Source/D3D11VP.h`, `DX11VideoProcessor.cpp` | `vp_rebuild_test` |
| `Tex2D_t::CheckCreate` minding the texture type | `Source/DX11Helper.h` | it compiles, and `--frames 300` |
| The measurement tools themselves | `tools/dlssnr_probe/` | `build.cmd` |

## Traps we have already fallen into

- **A compute pass sets no render target of its own.** If the caller has just drawn into a
  texture the pass reads, that texture is still bound as a render target and Direct3D
  silently drops its shader resource view: the pass reads zeros. It cost a black picture
  with ArtCNN and a green wash with FSRCNNX on chroma. Any dispatch placed after a draw
  needs `OMSetRenderTargets(0, nullptr, nullptr)` first. Symptom to recognise: a plane that
  reads *exactly* zero — black for luma, green for chroma.
- **Resource numbers.** Ours live in reserved ranges (see the head of `Source/resource.h`)
  precisely so a merge can never give two resources the same number. Section 5 of the
  pre-flight checks it. Never number a new resource inside upstream's range.
- **Generated files.** `Source/Upscale/MpvShaderTables.h` and the marked blocks of
  `compile_shaders.cmd` and `MpcVideoRenderer.rc2` are written by
  `Shaders/mpv/mpv_shaders.py`. Regenerate, never merge them by hand.
- **Settings are saved by name**, never by position: the `OPT_*` keys in
  `Source/VideoRenderer.cpp`. The enum values in `Settings_t` are append-only for the same
  reason, and the two scaling lists carry their value as combo item data so their order can
  change without moving anybody's setting.
- **`IVideoRenderer` and `Settings_t` are upstream's, plus ours at the end.** The interface
  keeps upstream's identifier, so anything built against upstream's header finds each method
  by its place in the table and each field by its offset. A method or a field of ours put in
  the middle moves every one of upstream's that follows it, and nothing says a word: it
  compiles, it links, `QueryInterface` succeeds. Measured, when `GetVideoProcessorUse()` sat
  in the middle of the interface: setting any option on a stock build did nothing at all,
  because the call landed one slot along -- our `SetSettings` reached upstream's
  `SaveSettings`, which wrote its own defaults to the registry. Append, always, and if the
  `settings kept:` line of `playback_test --file` ever disagrees with what was asked for,
  this is why.
- **Two known conflict points**, both already met: the order of `superRes` and `rtxHDR` in
  `InitializeD3D11VP` (upstream moved it; keep their order, keep our condition), and
  `SetDirty()` in `PropPage.h` (upstream added a guard; keep it and keep our
  `OnDeactivate()`).

## If a merge ever goes badly wrong

The fork's work is 15 commits on a linear branch. `git rebase --onto <new upstream>
<old base> dlss5` replays them one at a time, which turns one big merge into fifteen small
ones and says exactly which of our changes upstream broke. It rewrites history, so do it on
a scratch branch and only if merging has failed.
