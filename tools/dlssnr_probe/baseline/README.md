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

## The step that was not green is fixed

The battery found, on its first run, that ticking **Replace VP chroma upsampling** while the
film played left the screen frozen on the decoder-device path. It was the HDR retry in
`InitializeD3D11VP` calling back into itself without a bound: the processor and the swap chain
could not agree on whether the picture was HDR, and each pass asked for another. It is bounded
now -- once is all it ever legitimately takes -- and `playback_test --gpu --toggle` runs clean
three times over.

The report in this folder is from **after** that fix.

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
