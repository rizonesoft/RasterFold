---
name: groom-plan
description: Harden the todo tree for a weaker executor -- sequence check, drift sweep, gap scan, complete-feature pass, backlog triage (promote or leave; never drop) -- without moving rows between phases, ticking boxes, or starting a campaign. Use before a long run, or when the plan feels stale.
---

# Groom Plan

## Evidence before reordering

```bash
python3 scripts/todo-graph.py query sequence
```

It prints the longest dependency chain, which is where delay costs most, and every coupling the review findings recorded: a section that filed a finding to another section ran into work that section owns, which the dependency graph does not carry.

Read what it says about direction before acting on it. **A filing is not proof the order is wrong.** Most filings are work discovered early rather than work needed first, and nothing in the data separates those. Acting on the signal alone can invert a correct order, which is exactly what happened in the plan this system was ported from.

The command proposes and cannot act: it opens no file for writing. Reordering happens here, by hand, with the addresses and cross-references kept consistent as below.

Hardening the tree so a weaker executor can run it. Grooming adds prerequisites and fills gaps; it never moves a row out of its phase, never ticks a box, never deletes a backlog entry without a trace, never changes `todo/budget.json`, and never starts a campaign.

Record the groom's start commit (`git rev-parse HEAD`) and a run id first (`python3 scripts/campaign_guard.py mint-generation`, or the campaign's `run_id` when a campaign runs the groom): every section the groom files of its own finding carries `**Origin:** discovered run=<run id> <YYYY-MM-DD> -- groom-plan <step>` directly under its heading, so the groom record and `python3 scripts/todo-graph.py query growth --since <start>` can show what it added. Nothing caps how many (operator decision 2026-09-27: "the cap is worrying me, because I'm worried features will be left behind."). Sections the operator asks for during a groom are operator-directed: no Origin line.

## Workflow

### 1. Sequence check

For every dependency edge (section-level `Depends On` and file-level `depends_on`), confirm the prerequisite sits no later than its consumer in phase/row order. Where an edge points later:

- Prefer adding or splitting the prerequisite under the consumer's phase (a small new section with a dated note), so the consumer's row can run in order.
- Never move the consumer's row: phase membership is stable, and moving rows rewrites the plan's history.
- Record each fix with `**Corrected YYYY-MM-DD:**` or `**Groomed YYYY-MM-DD:**`.

Run `python3 scripts/todo-graph.py validate` after every structural edit. Cycles are FATAL: break them at authoring time.

### 2. Drift sweep

Walk every open section's concrete claims against today's repository: files, functions, constants, line numbers, command options, test cases, and measured ratios (re-run a quoted ratio when the code under it changed since the date it states). Correct drift in place with dated notes, exactly as `process-todo-section` steps 2-3 do, but tree-wide and without building anything.

```bash
python3 scripts/todo-claims.py              # re-measure what the TODOs claim
python3 scripts/todo-claims.py --coverage   # Current state blocks nothing re-measures
```

Pay special attention to:

- "Nothing exists yet" claims that are no longer true.
- Counts and versions quoted confidently (these age fastest): SDK, package, and target framework versions especially.
- Line numbers in `src/cm.c` and `docs/research.md` section numbers that later edits moved.
- Deferrals whose descriptions drifted while their owners stayed open.
- XREFs whose targets moved or shipped (reciprocity still holds, or the line is corrected).
- **Stop conditions**: every open experiment section names its measurement command and the bar it must clear, and says what ships when it misses (the negative result). A section that only says "improve" is rewritten with a bar. Record each with `**Groomed YYYY-MM-DD:**`.
- **Already tried**: no open section repeats an experiment in the "Already tried" table of `docs/research.md` unchanged; one that does is corrected to say what differs this time, or struck with a pointer to the table.

### 3. Gap scan

Read the plan as the codec's users will meet it, domain by domain, and ask what has no owner: an encoder path without its decoder, a format change without its oracle, spec, and known-answer change, a speed change without its size check, a new method without fuzz seeds, an option nothing reads, an error path nothing tests. File each gap with `add-todo` (evidence, owner, dependency), which applies the admission test and stamps the Origin line: a gap becomes a section (or an item on an open section that can hold it) when it is a defect in shipped or in-flight work, something an acceptance-bar aim requires, or a prerequisite of a planned row. Nice-to-haves go to the backlog, where they are kept. Place any new rows in their phases and sync the plan.

Check the Adjacency declarations while here: `python3 scripts/todo-graph.py query adjacency` advisories that name real gaps become sections; ones that are already covered get their anchors sharpened.

### 4. Complete-feature pass

For every format change in the plan, confirm the change is whole: the encoder, the decoder, the oracle, the spec, the known-answer file, the rejection cases, the fuzz seeds, and the `inspect` output. For every write, confirm the atomic path and the refusal cases (existing destination, unreadable input). For every speed change, confirm a size check proves the bytes did not move, or the size cost is measured and accepted. For every mechanism used twice (the range coder, the counters, the quantizers), confirm one copy is shared. File what is missing through `add-todo` (these usually pass the admission test, because the acceptance bar names lossless round trips, file safety, and measured claims), preferring an item on the owning section to a new section; do not redesign what exists.

### 5. Backlog triage

```bash
python3 scripts/todo-graph.py query backlog
python3 scripts/todo-graph.py query budget
```

The backlog has no cap, so triage is never about room. On every groom, walk it: each entry is either promoted or left, and nothing is dropped on the groom's own judgement (operator decision 2026-09-27, "No drop without operator approval"; `validate` fails an entry that leaves without a trace as `backlog-dropped`):

- **Promote what has come due**: an entry whose `promote when` has come true and that passes the admission test is promoted through `add-todo` (the promoting commit writes the section, with its Origin line and the entry's `source` as its `-> SOURCE:` line, and deletes the entry).
- **Leave the rest**: an entry not yet due stays as it is. Correct a drifted `needs:` ref or summary in place.
- **Merge duplicates**: two entries for one real-world thing become one. Keep the lower id, fold the other's text into the survivor's summary, and keep the absorbed entry's key as a merge marker, ``merged: `<source-key>` (B-NNN, YYYY-MM-DD)``, in the survivor's summary or as its own `merged:` field. The marker stays for as long as the survivor lives; removing it later is `backlog-dropped` unless its key has since been promoted.
- **Stale entries are the operator's call**: an idea that looks overtaken by shipped work, a decision, or a removed feature is listed in the groom record with the reason and put to the operator. Only when the operator approves in words does the entry leave: delete it and append `- B-NNN <source> -- removed YYYY-MM-DD -- operator: "<their words, verbatim>"` under `## Removed with operator approval` at the end of `todo/backlog.md`. Never paraphrase the words, and never delete a removal record.

### 6. Report and commit

Write the groom record: what was sequenced, what drifted and was corrected, what gaps were filed and where they landed (sections versus backlog), what the backlog triage promoted and merged (with each merge marker), which stale entries were put to the operator and what the operator said, the run id and the discovered sections it filed, and the size line the Progress block now shows. Commit as `todo: groom <scope> (<date>)` after a final `validate` plus `plan --sync` plus `plan --check`.

## Guardrails

- Do not move a row out of its phase. Add or split prerequisites instead.
- Do not tick a box. Grooming never ships.
- Do not start a campaign. Grooming prepares one.
- Do not redesign sections. Harden them.
- Do not file a discovered section without its Origin line, and do not change `todo/budget.json`.
- Do not delete a backlog entry, a merge marker, or a removal record except by promotion, by a merge that keeps the key, or with the operator's recorded words.
- Do not leave the tree unvalidated. `validate` plus `plan --check` pass before the commit.
- Do not add a ref to `todo/.design-baseline`. It stays empty while RasterFold has no surface.
