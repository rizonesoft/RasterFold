---
name: plan-experiment
description: Plan a codec change -- a new model, predictor, coding method, format feature, or speed or memory optimization -- from an idea to a TODO section that a cold session can build and measure, with its baseline, measurement commands, stop condition, and format duties spelled out. Use when an idea for making RasterFold smaller, faster, or leaner needs to become planned work.
---

# Plan Experiment

An idea planned as "try a sixth model" ships as a week of tuning on one image and a number nobody can reproduce. This skill exists to prevent that: it fixes the baseline, the measurement, the bar the change must clear, what ships when it misses, and every place a format change has to touch, before anyone writes code.

It covers three sizes of the same work. A **model or predictor change** alters what method 1 writes (a new format version) and is measured on the corpus. A **new coding method** gets a new method number and its own encoder, decoder, oracle, spec section, and rejection rules. A **speed or memory change** must leave the bytes identical, or measure and justify what it costs in size.

## Workflow

### 1. State the hypothesis

Write one paragraph: what the change is, which images it should help and why (the mechanism, not the hope), and what it costs (decisions per sample, memory, encode passes). Then write the non-goals: the adjacent ideas this experiment does not test, so one experiment tests one thing. Then search `todo/` and `todo/backlog.md` through `add-todo` for an existing home, and the "Already tried" table in `docs/research.md` section 3: an experiment already run unchanged is not planned again; one that differs says exactly what differs.

### 2. Fix the baseline, today

Measure the current code before planning against it, and quote the figures with their commands and the date:

```bash
python research/corpus.py                          # once per clone
python research/ab.py size photo                   # total, geomean, worst
python research/ab.py size tp
python research/ab.py size synth -v                # per image, for class-specific ideas
python research/ab.py bench -r3 -t1 measure kodim01
python research/ab.py bench -r3 -t3 measure kodim01
```

A plan whose baseline came from memory, from `README.md`, or from another day's run is planned against a number that may have moved.

### 3. Survey the prior art

Find where the idea already exists: the published reference codecs (JPEG XL modular mode and libjxl, lossless WebP, FLIF, JPEG-LS and CharLS, CALIC, the paq8 and lpaq image models, GraLIC, or a paper). Write what each does, with its version or section, and the effect it reports or that can be measured here. The survey decides the design details (context quantization, update rates, table sizes) that the plan would otherwise leave to the implementer's guess.

### 4. Define the measurement and the stop condition

Every experiment names, in its section:

- **The commands** that measure it, from `docs/research.md` sections 2.3 and 2.4: `python research/ab.py size build/research/try.c photo` (then `tp` and `synth`) for bytes, `python research/ab.py ab src/cm.c build/research/try.c -n5 -t1` and `-t3` for time.
- **The bar**, written as numbers against the baseline of step 2: by default at least 0.1% on the photo total with the worst image no more than 0.1% worse, confirmed on `tp`; for a speed change, a minimum-time ratio below 0.97 at the thread counts it targets, with the bytes unchanged; for a class-specific change (synthetic images, small images), the class's figure plus no photo regression beyond 0.05%.
- **The negative result**: what ships when the bar is missed (the measurement in the stamp, the row added to the "Already tried" table, the porting items struck with a pointer to the stamp).
- **The guard against tuning**: the parameters to try are listed in the plan before any run, the photo set tunes, and `tp` confirms. No rule may look at a particular image's dimensions, hash, name, or colour statistics.

### 5. Define the format duties

A change to what the encoder writes plans, in the same section, every place it touches: the C code (`src/cm.c`, `src/fold.c`, or a new file), the oracle in `tests/rfi_ref.py` written from the spec text, `spec/FOLD.md`, the regenerated `KAT_FOLD`, the rejection cases a decoder of the new format must refuse, the fuzz seeds that reach the new decoder, the `inspect` output, and the version or method number (`docs/research.md` section 2.2). The file sets `frozen: true` and the section carries a `**Freeze check:**`: older files still decode to the same pixels or are refused as unsupported, never misread. A speed or memory change instead plans the proof that the bytes did not move: `KAT_FOLD` unchanged and every oracle comparison green.

### 6. Split into sections

Measurement and implementation are usually separate sections, because the measurement decides whether the implementation happens: section N prototypes in `build/research/try.c` and measures against the bar; section N+1, depending on it, ports the change with its format duties. When the prototype is cheap and the port is mechanical, one section may hold both, with the port items struck on a miss. Each section is born complete per `create-todo`: context, micro-step checklist with Done-when per item, Test checkpoint that can fail, Commit item, and the `**Needs:**` lines (`ffmpeg with libjxl (benchmarks)` when it compares with JPEG XL, `Network (downloads)` when the corpus is not built, `Idle machine (timing)` when its bar is a speed ratio).

### 7. Author, wire, validate, commit

Author the section through `add-todo` (an item or section on an existing file) or `create-todo` (a new file) in the domain that owns it (`01-compression`, `02-speed`, `03-memory`, or `04-robustness`), with Depends On edges on the experiments whose results it needs and XREFs to the `docs/research.md` item it implements. Place every section in a phase of `todo/implementation-plan.md`.

```bash
python scripts/todo-graph.py validate
python scripts/todo-graph.py plan --sync
python scripts/todo-graph.py plan --check
```

Commit as one `todo:` commit. Report the sections, the baseline figures with their date, the bar, and the prior art each design choice rests on.

## Guardrails

- Do not plan without a baseline measured today and a bar written as numbers.
- Do not plan an experiment the "Already tried" table records unless the plan says what differs.
- Do not plan a format change that skips the oracle, the spec, the known-answer file, or the version or method number.
- Do not plan a tuning sweep without listing its values first, and never on `measure/image.bmp` alone.
- Do not plan a speed change without the proof that the bytes did not move, or the measured size cost it accepts.
- Do not file a "tune it later" section. A parameter the plan leaves open is a parameter the implementer will fit to one image.
