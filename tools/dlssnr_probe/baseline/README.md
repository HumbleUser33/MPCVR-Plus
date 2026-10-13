# The reference numbers

`run_all_report.txt` here is what the whole battery measured on 13 October 2026, on an
RTX 4060 (driver 32.0.16.1656) with the Windows desktop in HDR, all twenty-three steps green.
The filter is `569600f`, build 2686, and filter and tools are built by MSVC 14.44. It is the thing
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
