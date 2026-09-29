# Section gates

The gate card for `process-todo-section`, `review-todo-section`, and `process-todo-file`. When `docs/research.md` section 2 (the measurement rules) and this card disagree, fix whichever is wrong in the same commit; neither silently wins.

Quote the exit code, the passed, failed, and skipped counts, and the last lines of output. Leave the full log under `build/` (gitignored), never in the context.

## Every section: the one-command sweep

```powershell
pwsh scripts/check-all.ps1
```

It builds `Bin/rasterfold.exe` with `build.ps1` (Clang, `-Wall -Wextra`), runs the correctness suite (`tests/test_correctness.py`) and a short decoder fuzz (`tests/fuzz_decode.py 200`), and runs the TODO gates (`todo-graph.py self-test`, `validate`, `plan --check`, `todo-claims.py`, the findings and run-record checks) and the self-tests of the review and campaign tools. Every gate runs even after an earlier one fails, each gate's log goes to `build/logs/<gate>.log`, and it prints a table of gate results; green means exit 0 with every gate `PASS`. A `SKIP` row (no Python, a script absent) is not green: quote it and fix the cause. `-SkipBuild` reuses the last build for a re-run of the tests and gates only, while iterating, never for the commit-time run. `-Quick` skips the fuzz gate, also for iteration only.

## Every code section: the whole correctness suite

```powershell
python tests\test_correctness.py
```

Green means zero failed and every case run (`33 passed, 0 failed` today, more when a section adds cases). A case that cannot run is a failure, not a skip. While iterating, narrow to the touched cases:

```powershell
python tests\test_correctness.py -k known-answer -k corrupt
```

The filtered run is iteration only; it prints `N skipped by -k`, and a commit-time quote never carries that phrase. The commit-time quote comes from the unfiltered run at the commit being created.

## Decoder sections: fuzz

```powershell
python tests\fuzz_decode.py 2000 <seed>
```

Green means `no crash, hang or mismatch`. A section that changes what the decoder accepts runs at least 2000 mutations; quote the seed so the run can be repeated.

## Format sections: the four places

A section that changes what the encoder writes or the decoder accepts shows, in one commit: the C change, the oracle change in `tests/rfi_ref.py`, the spec change in `spec/FOLD.md`, and the regenerated `KAT_FOLD` (method byte, offset 16, still `1` or the new method's number). `python tests\test_correctness.py` green proves the four agree; the freeze check proves the old version or method still decodes or is refused, never misread.

## Compression sections: the corpus

```powershell
python research\corpus.py                               # once per clone; downloads about 360 MB into build/research
python research\ab.py size build\research\try.c photo  # then tp and synth, and the same three on src\cm.c
```

Quote `total`, `geomean`, and `worst` for each set, before and after. The bar is the section's stop condition, by default `docs/research.md` section 2.3: at least 0.1% on the photo total with the worst image no more than 0.1% worse, confirmed on `tp`.

## Speed sections: alternating runs

```powershell
python research\ab.py ab src\cm.c build\research\try.c -n5 -t1 measure kodim01
python research\ab.py ab src\cm.c build\research\try.c -n5 -t3 measure kodim01
python research\vsjxl.py -n5
```

Quote both minimum-time ratios. Below 0.97 is a real gain, 0.97 to 1.03 is noise, above 1.03 is a loss. Name the machine load; a timing taken while the machine is busy is repeated until two runs agree.

## Release-facing numbers: the reference comparison

```powershell
python tests\measure_final.py
python tests\benchmark.py -n 3
```

`measure_final.py` must exit 0 (PASS). `benchmark.py` rewrites `benchmarks/results.md`; a section that changes size or speed commits the new file, and the README's measurement table is updated in the same commit.

The file-level full suite, the full benchmark, and the long fuzz run belong to `process-todo-file`.
