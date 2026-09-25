# The reference numbers

`run_all_report.txt` here is what the whole battery measured on 25 September 2026, on an
RTX 3050, at commit `e324bf5`. It is the thing a merge from upstream is compared against, at
step 8 of [MERGING-UPSTREAM.md](../../../MERGING-UPSTREAM.md):

```
cd tools\dlssnr_probe
run_all.cmd
python compare_report.py baseline\run_all_report.txt run_all_report.txt
```

Why a stored report rather than pass/fail alone: every check reading zero is necessary and
not sufficient. A merge can pass all of them and still cost half a decibel of detail, or
quietly stop exercising a path. The comparison catches both — it ignores the timings, which
say more about the machine's mood than about the code, and it flags a row that has **stopped
being measured** as loudly as one that moved.

## One step of this baseline is not green

**`pictures from a decoder's device` fails, 3 to 4 checks of 14.** The battery found it on its
first run, and it is recorded here as measured rather than hidden, so that the next comparison
shows it unchanged instead of springing it on somebody.

What it is: with the pictures arriving as D3D11 textures the way a hardware decoder delivers
them — which also means a 10-bit P010 source, since that is what `d3d11_source.inl` sends —
ticking **Replace VP chroma upsampling** while the film plays leaves the screen frozen. The
renderer goes on drawing at 24 fps and every call returns at once; only the desktop stops
being updated. Unticking the box brings it back. The same toggles on the memory path, in
NV12, all pass, so it is the decoder device, the 10-bit source, or the two together — the
battery says *that* it happens, not yet *why*.

It is not a regression of the work that built this battery: the 4:4:4 pre-pass it involves
predates it. Until it is understood, this step's failure is the known state; anything else
failing is new.

## What the numbers depend on

The first line of the report records the machine and the commit. The tables move with:

- **the GPU and its driver** — these were measured on an RTX 3050; another card gives other
  times and slightly other numbers, so regenerate the baseline on the machine you compare on;
- **the reference pictures** in `upscale_refs/`, which are not in the repository (they are
  film frames). Comparing two reports means comparing runs over the same folder;
- **the DLSS DLLs** present, for the suites that need them.

So the baseline is a local artefact of a known-good state, not a universal truth. When a
merge lands and the numbers are explained, replace this file with the new report — that is
step 10.

## When something moved

A drift beyond about 0.1 dB wants a reason, and there usually is one: upstream changed a
conversion, a shader was regenerated, a reference picture changed. Find it before you accept
it. The four `port` suites are the ones to trust most — they run the filter's own code and
the harness's on the same picture and demand the same result to the last half float, so they
cannot drift for an innocent reason.
