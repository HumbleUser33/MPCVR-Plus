# The reference numbers

`run_all_report.txt` here is what the whole battery measured on 26 October 2026, on an
RTX 4060 (driver 32.0.16.1742) with the Windows desktop in HDR, all twenty-five steps green.
The filter is `6e6ceb5`, build 2746, and filter and tools are built by MSVC 14.44. It is the thing
a merge from upstream is compared against, at step 8 of
[MERGING-UPSTREAM.md](../../../MERGING-UPSTREAM.md):

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

## What the numbers depend on

The first lines of the report record the date and the commit, and the card is named further
down. The tables move with:

- **the GPU and its driver** — another card gives other times and other numbers, as the
  next section measures, so regenerate the baseline on the machine you compare on;
- **the reference pictures** in `upscale_refs/`, which are not in the repository (they are
  film frames). Comparing two reports means comparing runs over the same folder;
- **the DLSS DLLs** present, for the suites that need them;
- **the film** in `upscale_refs/` and **LAV Filters** where an installed MPC-HC or MPC-BE keeps
  them (`file_graph.inl`), for the two steps that play a real film. Without either they fail,
  and say which.

So the baseline is a local artefact of a known-good state, not a universal truth. When a
merge lands and the numbers are explained, replace this file with the new report — that is
step 10.

## From the RTX 3050 to the RTX 4060

The report this one replaces was measured on 30 September 2026 on an RTX 3050, at `4013957`;
the history keeps it. Of the 2546 rows the two share, 34 moved by more than 0.1, and none of
them because of the code:

- **28 in the two "chroma through the filter" steps.** The hardware video processor's own
  chroma is worse on this card: 52.25 → 46.84 psnr-c on an 8-bit source, 49.60 → 45.96 on a
  10-bit one. The shader rows keep their psnr-c in 8 bits (Catmull-Rom 53.38 → 53.32) and
  lose about a decibel of psnr-rgb (50.30 → 49.31), and about a decibel of both in 10 bits.
  The filter's build is not what moved them: the released 1.9.1 filter, built on the other
  machine, gives the same 32 rows as the one built here, to 0.001. Which of the card, its
  driver and the desktop being in HDR does it was not separated.
- **4 in "the DLSS passes against the harness".** The Optical Flow engine does not answer the
  same on the two cards — on the first "zoom 3 %" frame it finds a global motion of
  (-1.59, -2.44) there and (-1.16, -1.28) here — so everything that follows from its vectors
  is another figure. Three of the rows are the table of GPU times and of blocks keeping
  their own vector. The fourth is worth knowing: on this card's vectors one block value of
  70 983, in a second "zoom 3 %" frame, lands one refinement step (0.125) from the CPU
  reference, where the other card's had none apart. The suite passes it, and it is the same
  block whichever machine built the tools.
- **2 in "the pipeline"**, which prints its times bare.

Seven rows are new: the Settings page prints a third list.

The tools are not what moved anything either. The battery run on this card with the tools as
the other machine had built them gives the same 2474 rows to 0.001, six apart, all six of
them times.

## From 12 to 13 October 2026

The report of the day before, at `658071e`, is in the history. Between the two the schedule
learnt to leave a sample out before it is drawn, DLSS 5 NR was reworked, and the step called
"the DLSS page" took the name of what it does, "DLSS through the filter, five graphs", with
the page's DLSS 5 NR section as a step of its own: so the comparison of the two reports says
`the DLSS page MISSING from the new report`, once, and that is why. Of the 2553 rows they
share, 2 moved, both the pipeline's bare times; none is gone; 46 are new, the two steps'.

The filter's settings are read from the registry of whoever runs the battery, and the
presentation steps follow them: this report was taken with *Swap effect* on Flip, the one
before with Discard, and the steps that present gave the same rows both ways.

## 13 October 2026, later: the processor after DLSS 5 NR

The report of that morning, at `565855d`, is in the history. Since then the video processor
resizes what DLSS 5 NR leaves, and the battery has four more steps:

- "what the Settings page greys", the page's rule with each DLSS box ticked;
- "the processor enlarging after DLSS 5 NR" and "the processor after DLSS 5 NR, PQ picture":
  a real film, paused at the same place under each way of enlarging and read off the desktop.
  They are the only steps that bring the hardware processor in on a decoder's pictures after
  the network -- the bench's own film is RGB and never does;
- "every step ran", which counts the steps in the report against those `run_all.cmd` names.

Of the 2599 rows the two reports share, 7 moved, none of them a measurement of the code: five
are the *On screen* lines' counts of late pictures, two the pipeline's bare times. None is
gone; 16 are new, the two film steps'.

The last of the four steps is there because of what the first run of the afternoon did. It
passed every step it ran, and it had not run "chroma through the filter, 8-bit": `run_all.cmd`
had Unix line endings, and with the three steps added cmd.exe no longer found the label of
one `CALL`: it looks for a label 512 bytes at a time in such a file, and misses one that the
edge of a read cuts in two. One line on stderr, which the script that ran the battery that
day was filtering out, and no verdict. The comparison is what said so -- the step `MISSING
from the new report`, sixteen rows gone -- and that report was not kept. The file has
Windows line endings since (`569600f`), and the report in this folder is from after.

The same evening the swap chain of an SDR picture went from two buffers to three (`c6b2a87`),
which every step that plays through the filter goes through, and the reference was taken
again at `c9da620`. Of the 2615 rows it shares with the one at `569600f`, 10 moved: five
counts of late pictures in the *On screen* lines (all of 4 or less, both ways), the three rows
of GPU times in "the DLSS passes against the harness", two of the pipeline's bare times. None
is gone, none new.

That report is the second of two. The first had six *On screen* lines of "the upscalers
through the filter" counting 11 to 14 late pictures, one configuration after another, where
this one counts none and one of 2. It was not the third buffer: the same step run alone three
times counted between none and 6 with two buffers and with three alike. Another session was
running its own tests on the machine during that minute. A battery wants the machine to
itself, and a run that did not have it is not a reference.

## What the first run here found

The sharpening suite died part of the way through, of `bad_alloc`, and the summary said
nothing: a program that dies leaves no verdict, and `run_all.cmd` only looked for exit codes
above zero. Direct3D destroys a released object at the next `Flush`, the suites make their
textures afresh for every picture, and on this card nothing was flushing them — 24 GB of
memory and 7 GB of the card's when it gave up. `ReadRgbaHalf`, which every suite reads back
through, flushes now (4.3 GB and 370 MB at the peak), and `run_all.cmd` writes any exit code
that is not zero. The report in this folder is from **after** both.

## When something moved

A drift beyond about 0.1 dB wants a reason, and there usually is one: upstream changed a
conversion, a shader was regenerated, a reference picture changed. Find it before you accept
it. The four `port` suites are the ones to trust most — they run the filter's own code and
the harness's on the same picture and demand the same result to the last half float, so they
cannot drift for an innocent reason.

## The stabilizer's port check, from 15 October 2026

The step "the DLSS passes against the harness" named three suites, `--tport --tstabport
--tsrport`, and the harness runs one a call: the first its chain finds, DLSS SR's. So this
report, and every one before it, has DLSS SR's port check in that step and nothing of the
stabilizer's or of the shader detector's. The detector has since left the filter, and
`run_all.cmd` gives the stabilizer a step of its own, "the stabilizer against the harness"
(`--tstabport` on the film frame: the wallpapers it used by default are not on this machine).
The harness now refuses two suites in one call, with a failure in the report.

Compared with this report, a battery run with that step shows its 31 rows as new, not as a
failure, and the "every step ran" row with one step more; "the DLSS passes against the
harness" keeps its 51 rows, as they were. Measured on the two steps alone in the working copy,
both at 0 failures: the stabilizer's class against the reference passes on five sequences,
every redraw identical. The battery itself has not been run with it yet; the report that
replaces this one will hold the step.

## 17 October 2026: the statistics laid out as the pipeline

The report of 13 October, at `c9da620`, is in the history. Since then DLSS SR's global motion
is refined and its history dropped at a cut, the stabilizer's port check has its own step
(above), the settings page and the manual say what each option does, and the statistics are
laid out as the pipeline, each step with its time. `playback_test` reads those steps by name
and puts its report's old rows back together from them.

It was taken with the filter's settings of the day: Upscaling FSRCNNX 16 AR, Adaptive-Sharpen
at 4, the DLSS 5 NR style cinematic. The presentation steps' times follow them -- "DLSS off"
in the two playback steps reads ready in 17.6 and 19.0 ms where the report before read 6.3
and 6.2 -- and those rows are times, which the comparison leaves out.

Of the 2615 rows the two reports share, 12 moved by more than 0.1, none of them a measurement
of the code:

- three *On screen* lines' counts of late pictures, as in the comparisons above;
- three rows of "the DLSS passes against the harness", its table of GPU times, printed bare,
  and of blocks keeping their own vector, one point fewer since the refined global motion
  (`f9ed450`): 1.23 ms and 11 % to 1.12 ms and 10 % for "pan 3.5 x 1.5". The run of 16 October,
  at `f9ed450`, already had the 10 %;
- two of "the pipeline", its times printed bare. The battery's run of the step had DLSS SR at
  12.5 ms of GPU, the step run alone twice afterwards at 8.8, against 9.3 on 16 October;
- four `processor` rows of the two chroma steps, whose shape changed: the conversion is now
  read from the statistics' steps, and the chroma replacement's step says
  "4:2:0 -> 4:4:4", whose digits count as numbers. The same wording makes eight rows of those
  steps new; the measured rows beside them are the same to 0.01.

The other 31 new rows are the stabilizer's step. None is gone.

## 25 October 2026: after a review of the whole filter

The report of 17 October, at `4a348c2`, is in the history. Since then eight commits
(`24ef5f3` to `6c936d1`) made passes cheaper, had DLSS 5 NR and the renderer wait for each
other on the GPU instead of the CPU, and moved this fork's own functions out of
`DX11VideoProcessor.cpp` into files of their own. The battery has one step more, "the
upscalers through the filter, with bars": the picture kept in its shape between bars, which
the first step after the scaling now reads where it lies instead of copying it there
(`d0dce3e`).

It was taken with the settings of the report before: Upscaling FSRCNNX 16 AR,
Adaptive-Sharpen at 4, the DLSS 5 NR style cinematic, with Jinc chroma upsampling and
*Swap effect* on Flip.

Of the 2654 rows the two reports share, 3 moved by more than 0.1, none of them a measurement
of the code:

- two of "the pipeline", its "frame, upload to done" printed bare as mean / 95th percentile /
  maximum: with DLSS SR alone 13.83 / 24.11 / 25.79 ms then, 9.45 / 10.54 / 17.49 now, and
  with Catmull-Rom alone 0.84 / 5.68 / 10.84 then, 0.39 / 0.42 / 0.48 now. The first run of
  the 25th, on the code of 17 October, already read 9.42 / 10.51 / 16.44 and 0.41 / 0.47 /
  0.57: it was the run of the 17th that was slow. The 95th percentile with DLSS SR alone goes
  from 10.5 to 16.9 and back from one run to the next whatever the code, over the nine runs
  of the 25th;
- one *On screen* count of late pictures, Jinc2m in "the upscalers through the filter": 10
  on the 17th, none in any run of the 25th.

None is gone; the 15 new rows are the new step's.

## 26 October 2026: upstream 0.10.9, RTX Video HDR on a small picture, and a new driver

The report of 25 October, at `6c936d1`, is in the history. Since then upstream's 0.10.9 was
merged (`1cb2a27`), and seven commits followed (`a622873` to `6e6ceb5`): the bench reads an
HDR desktop in half floats; a 10-bit DIB no longer overruns its allocation; Super Resolution
on an 8-bit picture writes eight bits on an HDR desktop; the chroma replacement waits for a
picture really interlaced; the statistics say when the driver leaves RTX Video HDR out, and a
small picture the shaders enlarge is first enlarged past 640x360 for it.

It was taken with the settings of the report before: Upscaling FSRCNNX 16 AR,
Adaptive-Sharpen at 4, the DLSS 5 NR style cinematic, Jinc chroma upsampling and *Swap
effect* on Flip.

**The NVIDIA driver changed under it.** 32.0.16.1742 was installed at 06:32 on the 26th, in
place of 32.0.16.1656, by an update and not by this work. Three batteries were run against the report of the 25th,
and of the 2669 rows each shares with it, none is gone and none is new:

- at `1cb2a27`, the merge, on 1656 the evening before: 3 moved -- the table of GPU times in
  "the DLSS passes against the harness" ("zoom 3 %", 1.87 to 2.00 ms), the pipeline's "frame,
  upload to done" printed bare, and one picture shown late with bars (FSRCNNX 16 AR);
- at `597334f`, on 1742: 2 moved -- the same pipeline row, and ArtCNN with bars dropping one
  picture and showing one late;
- at `6e6ceb5`, this report, on 1742: 1 moved -- the same pipeline row, its 95th percentile
  10.54 to 16.02 ms, the flip between about 10.5 and 16.5 noted on the 25th.

So neither the merge, nor the fixes, nor the driver moved a measurement. The steps the fixes
change -- a picture under 640x360 pixels with RTX Video HDR, Super Resolution with RTX Video
HDR off -- are in no step of the battery; `playback_test --vpturns` is how they were measured.

**What the new driver adds to the report.** Its NGX runtime checks the signature of every
snippet in the folder it is given for DLSS SR, and logs that `nvngx_dlssnr.dll`'s does not
match the file (`nvLoadSignedLibraryW() failed ... missing or corrupted`, `NGXLoadFromPath
failed ... 0xBAD00000`): eleven such lines, absent from every report before, each ending in a
stray carriage return. The filter loads that DLL itself and calls it through its shim, never
through NGX's loader, and DLSS 5 NR ran as before: its rows are unchanged, and the steps that
fail when it does not run passed.

**The screen watch reads half floats now.** `DuplicateOutput1` had answered
`DXGI_ERROR_UNSUPPORTED` in every run before `a622873`, and the stills came from the old
duplication instead. The two steps that compare stills, "the processor enlarging after DLSS 5
NR" and its PQ twin, give the same rows read either way.
