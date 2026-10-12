# MPCVR-DLSS5

A fork of [Aleksoid1978/VideoRenderer](https://github.com/Aleksoid1978/VideoRenderer),
branched at its 0.10.7 release. It adds NVIDIA DLSS 5 neural reconstruction, a temporal
stabilizer, DLSS Super Resolution, mpv prescalers on luma and chroma, and a 4:4:4 chroma
pre-pass before the hardware video processor. `README-DLSS5.md` is the manual.

It is published as **MPCVR-Plus**: `origin` is HumbleUser33/MPCVR-Plus, the branch is
`dlss5`, and `upstream` is Aleksoid1978/VideoRenderer.

## Working with the maintainer

- **Answer in French.** The maintainer writes in French. Code, comments, commit messages and
  the manual stay in English.

- **Commit locally; push, tag or publish only when asked, each time.** The maintainer tries a
  build in their own player before anything goes out, and makes the tags and the GitHub
  releases themselves. A release is the zip `build_mpcvr.cmd` leaves in `_bin`, renamed
  `MPCVR-Plus-<version>.zip`, with a `changelog-<version>.md` beside it. `git ls-remote
  --tags origin` says which numbers are taken; the local tag list does not.

- **The installed player is theirs.** Never write into its folder or its registry keys. Try a
  build in a copy of the player's folder, with the `.ax` under test put in its `MPCVR\`.

## Before changing anything, know this

- **Bringing in a new upstream version goes through [MERGING-UPSTREAM.md](MERGING-UPSTREAM.md).**
  Start with `python tools\upstream_check.py`, which is read-only and says what the merge
  will cost. Do not merge upstream by improvisation.

- **Resource numbers live in reserved ranges.** Upstream owns everything below 1900, and
  every control below 1200. Ours go in 4000-4019 (dialogs), 1900-1999 (shaders),
  2000-2099 (generated) and 1200-1399 (controls). The head of `Source/resource.h` says so,
  and section 5 of the pre-flight checks it. A number used by both sides compiles without
  a word and loads the wrong resource.

- **Some files are generated; do not edit them by hand.**
  `Shaders/mpv/<shader>/passNN.hlsl`, `Source/Upscale/MpvShaderTables.h` and the marked
  blocks of `Shaders/compile_shaders.cmd` and `Source/res/MpcVideoRenderer.rc2` all come
  from `python Shaders\mpv\mpv_shaders.py renderer`.

- **Settings are saved by name**, and the enum values in `Source/IVideoRenderer.h` are
  append-only. The Upscaling, Downscaling and Chroma upsampling lists are shown in measured
  quality order and carry their value as combo item data, so the display order can change
  without moving anybody's saved setting. The lists that still save the position they sit on
  are `IDC_COMBO4`, `6`, `7`, `8`, `9` and `IDC_SHARPEN`, and those must not be reordered.

- **`IVideoRenderer` and `Settings_t` are append-only, at the end.** The interface keeps
  upstream's identifier, so a caller built against upstream's header finds methods by their
  place in the table and fields by their offset. One of ours inserted in the middle moves
  all of upstream's that follow, silently: `playback_test --file` prints a `settings kept:`
  line for exactly this.

- **A compute pass sets no render target.** If the caller has just drawn into a texture the
  pass reads, Direct3D drops that texture's shader resource view and the pass reads zeros
  (black for luma, green for chroma). Unbind the render targets before any dispatch placed
  after a draw. This has bitten twice.

- **Line endings stay as committed.** Most files are LF, a dozen are CRLF (the solution, the
  two `.props`, `VideoRenderer.cpp`, `stdafx.*`...), `Source/VideoRenderer.h` mixes both, and
  `Source/resource.h` opens with a UTF-8 BOM that a rewrite once dropped. The working
  folder's own git config says `core.autocrlf=false`, where Git for Windows defaults to
  `true`: a fresh clone wants `git clone -c core.autocrlf=false`. Patch such a file in
  place, with nothing that rewrites the whole of it (`sed -i` does), and write a patch script
  to a file rather than through a shell heredoc, which turns the `\n` inside its strings
  into real line breaks.

## Building and measuring

```
build_mpcvr.cmd NoWait                 REM Win32 then x64, always; zero warnings expected
tools\dlssnr_probe\build.cmd           REM the measurement tools
tools\dlssnr_probe\run_all.cmd         REM the whole battery, one report
```

Compare the report with `tools/dlssnr_probe/baseline/`. "0 failures" is necessary and not
sufficient — the numbers are what tell you nothing regressed.

The tools compile the filter's own sources, so they measure the shipped code, not a copy of
it. `--tport`, `--tstabport`, `--tsrport` and `--tmpvport` run the filter's code and the
harness's own on the same picture and require the same result to the last half float.
`--tdownport` does it for the downscaling shaders against the kernels they are written from.

`playback_test` is the exception: it loads the built filter,
`_bin\Filter_x64\MpcVideoRenderer64.ax`. Build the filter before it, and run `build.cmd`
again after changing a source the other tools share.

### What the build needs

- **`build_mpcvr.cmd` is upstream's and reads five words only**: `Debug`, `Sign`, `NoWait`,
  `VS2019`, `VS2022`. Anything else is ignored and both platforms are built. Without
  `NoWait` it waits for a key on an error.

- **From a Claude Code shell, clear `NoDefaultCurrentDirectoryInExePath` first.** It is set
  there, and cmd then no longer runs a script or a program named without its path from the
  current folder, which is how these scripts are called and how `run_all.cmd` calls the tools.

  ```
  PowerShell   $env:NoDefaultCurrentDirectoryInExePath = $null
               cmd.exe /c "set NoDefaultCurrentDirectoryInExePath=& call build_mpcvr.cmd NoWait"
  bash         NoDefaultCurrentDirectoryInExePath= cmd.exe //c "set NoDefaultCurrentDirectoryInExePath=& call build_mpcvr.cmd NoWait" < /dev/null
  ```

- **git on `PATH`.** The pre-build step, `update_revision.cmd`, writes `revision.h` from git
  and leaves it empty when it finds none. **`python` a real Python 3 with numpy**, which the
  shader generator imports; on a fresh Windows, `python` is a Microsoft Store stub that does
  nothing.

- **Releases are built with MSVC 14.44**, the v143 tools of Visual Studio 2022. Visual Studio
  2026 builds with v145 (14.5x), which no longer targets Windows 7: the Win32 filter it makes
  imports `GetSystemTimePreciseAsFileTime` and will not load there. So under 2026
  `Directory.Build.props` takes 14.44 when it is installed with its MFC, and `build.cmd`
  does the same for the tools. That is three individual components in the Visual Studio
  installer: *MSVC v143 - VS 2022 C++ x64/x86 build tools (v14.44-17.14)*, and *C++ v14.44
  (17.14) MFC* and *ATL for v143 build tools (x86 & x64)*; the first does not bring the
  other two. Each project prints `MSVC <version>` as it builds. **If that line says 14.5x
  the components are missing and the build has gone back to v145 without another word: read
  it before packaging anything.** `PlatformToolset=v143` is not the way under 2026 (it is
  MSB8020); the older compiler is taken by its version, which is what the file does.

### Running the battery

- **It needs a display that is awake and nobody at the machine**, for about 35 minutes on an
  RTX 4060. The playback steps present to the screen and watch the desktop. Once Windows has
  put the display to sleep its vertical blanks stop coming (`IDXGIOutput::WaitForVBlank`
  returns at once) and nothing those steps time means anything. Hold the display awake for
  the run (`SetThreadExecutionState` with `ES_DISPLAY_REQUIRED`); that does not wake one
  already asleep, an input does.

- **A step that dies prints no verdict**, so its absence from the summary is not a pass.
  `run_all.cmd` writes the exit code of a step that ends on anything but zero, and
  `compare_report.py` calls the step MISSING and its rows gone: run the comparison every
  time, it is what notices.

- **The numbers are the machine's.** `baseline/` says which card measured it and what moves
  from one card to another with no change in the code. Compare on the machine the baseline
  comes from, or measure a new one there first.

### What is not in the repository

A copy of the working folder carries all of this; a clone carries none of it, and
`.gitignore` names each one. The two submodules, `external/BaseClasses` and
`external/minhook`, come with `git clone --recursive`.

- `nvngx_dlssnr.dll` and `nvngx_dlss.dll`, at the root: NVIDIA's, never committed and never
  put in a release zip. The harness's DLSS suites need them.
- `tools/dlssnr_probe/upscale_refs/`: the 4K film frames every picture-quality suite reads,
  and a film. Not ours to redistribute. `run_all.cmd` measures on `4K1.png` from there.
- `tools/dlssnr_probe/upscalers/`: the mpv shaders the generator translates, with glslang and
  SPIRV-Cross in its `tools` folder.
- `external/onnxruntime/` and `tools/dlssnr_probe/models/`: the harness's neural rows
  (EfRLFN, measured and not integrated). The tools build without them, minus those rows.
- `Optical_Flow_SDK_5.0.7/`: reference only. The two headers the filter compiles are in
  `Source/DLSS/NvOF`.
- `_bin/`: every build product, the release zips and their changelogs.

## In a real player

The manual's *Diagnostic tools* section lists every program and what each switch does. What
it does not say:

- **A player writes its settings as it closes**, a copy of it too. Before driving one with
  `mpchc_drive`, export `HKCU\Software\MPC-HC` and
  `HKCU\Software\MPC-BE Filters\MPC Video Renderer`, and import them back afterwards.
- **A test puts back what it moved**: the display mode if it changed it, the filter's
  registry key if it wrote it.
- **Never send a window of another process a message whose parameter points into our own
  memory.** The pointer means nothing over there.
- **Measuring the renderer is not measuring the screen.** A swap chain can present every
  picture on time into a window the desktop no longer updates, and every number the filter
  prints stays perfect. `playback_test --toggle` watches the desktop itself for this; a
  screen copy made with GDI does not see flip-model content.
- **For anything about pacing, trace the real player.** With `MPCVR_PACING_TRACE=<file>` in
  the player's environment the renderer writes a line a picture, and
  `tools\dlssnr_probe\pacing_report.py` says on which refresh each was shown and how long
  after its time. The bench's window does not behave as a fullscreen player does: a
  `Present` reaches the screen a refresh sooner in it once it has settled, and one called
  3 ms after a refresh was in time or not from picture to picture there, which fullscreen in
  MPC-HC it always was. What holds in one has to be measured in the other.
- **A setting changes what is measured.** The bench takes the filter's settings from the
  registry, the maintainer's own: with *Swap effect: Discard* there is no flip model, DXGI
  gives no refresh times, and none of the scheduled presentation runs. Set what the test is
  about, and put it back.

## House style

Comments and commit messages say *why*, in plain English, in the voice the surrounding code
already uses. Numbers in the manual and in commit messages come from the tools in
`tools/dlssnr_probe`, measured on the machine at hand — never from memory or estimate.
