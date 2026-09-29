---
name: add-todo
description: Front door for new work -- search the tree and the backlog for an existing home first, then route to an existing section, a new section, a whole new file via create-todo, or the backlog. Work a campaign discovers on its own also passes the admission test and carries an Origin line for provenance; nothing is capped. Use whenever work needs capturing, and to promote a backlog entry.
---

# Add TODO

New work enters through here. The failure this skill exists to prevent is the near-duplicate TODO in a second domain, which is worse than no entry: two owners, neither complete. In RasterFold the classic shape is one idea filed twice: a model change filed under `01-compression` and again under `02-speed` because it also costs time, or a decoder check filed under `04-robustness` and again under the compression section that adds the format it checks. A change has one home; the other domain gets an XREF.

The second failure is lost work: a feature dropped because a cap had no room for it. Nothing in this plan is capped (the budget in `todo/budget.json` records both caps as `null`), so nothing here counts against a limit. Work a campaign discovers on its own still passes the admission test (it decides section or backlog, never keep or lose) and carries an Origin line so the operator can see which run filed it; a backlog entry is never deleted without a promotion, a merge marker, or the operator's recorded words. The rules are in `todo/README.md` under "The budget and the backlog".

## Workflow

### 1. Search before filing

Search the tree for a home the work already has:

```bash
grep -rn "<keywords>" todo/ --include='TODO-*.md' | head -30
grep -n -i "<keywords>" todo/backlog.md | head -10
python scripts/todo-graph.py query findings | head -20
```

Read the candidates. A home exists when a section's scope already covers the work, even if its checklist does not name it yet. Search every app domain, not only the one the request names: a color picker, an undo stack, or a file dialog asked for in one app is often already owned by another or by the core domain. A matching backlog entry is a home too: extend its text rather than adding a second entry.

### 2. Operator-directed or discovered

Decide first who is asking:

- **Operator-directed**: the operator asked for this work in an attended session. The request is the admission: skip the admission test, and write no Origin line. Step 5 still decides the home (an existing section beats a new one).
- **Discovered**: an agent found the work on its own during a campaign run (a `process-phase` gap audit, a `review-todo-section` research or plan-review finding, a `groom-plan` gap scan, an adjacent defect or prerequisite found while building). It takes steps 3 and 4 below.

### 3. The admission test (discovered work)

Discovered work becomes a section (or a file of sections) only when it is one of these, and the section's context paragraph says which:

1. **A defect in shipped or in-flight work**: something built, or being built, that does not do what its section promised.
2. **Required by an acceptance-bar aim**: name the row of "The acceptance bar" in `todo/implementation-plan.md` it serves.
3. **A prerequisite of an existing planned row**: name the `DNN TNN §N` that cannot ship without it.

Everything else (a technique another codec has, an idea from a paper, a nice-to-have from a review, research, or a gap scan) takes the Backlog outcome below. "It would be good" is not an admission clause.

### 4. The Origin line (discovered work)

A discovered section carries one line directly under its `## N.` heading, the run id being the one `campaign_guard.py` minted for the run (a groom outside a campaign mints its own with `python scripts/campaign_guard.py mint-generation`):

```
**Origin:** discovered run=<run id> <YYYY-MM-DD> -- <what found it>
```

The line is provenance, not a quota: there is no per-run cap (`todo/budget.json`), so a discovery that passes the admission test is never pushed to the backlog for lack of room. `python scripts/todo-graph.py query growth --since <run start commit>` lists what the run has added, discovered sections with their run id, for the run file and the report. Still prefer the smallest honest home:

- **Merge**: add the work as an item (or a Build-order stage) on an existing open section whose scope can hold it.
- **Supersede**: when the new work replaces an open section, rewrite that section in place (its address stays) rather than adding a second one.
- **New section**: otherwise, with its Origin line.

`validate` refuses a malformed Origin line (`origin-malformed`). There are no phase ceilings and no caps: nothing limits how many sections a phase, a run, or the tree holds, or how many entries the backlog holds.

### 5. Route to one of five outcomes

| Outcome | When | Do this |
| ------- | ---- | ------- |
| Already covered | A section or a backlog entry owns it | Point at it. File nothing. |
| Fits a section | An open section's scope covers it | Add a checklist item there, or extend the section body. Never touch a `[x]` section's checklist. |
| Needs a section | Operator-directed, or discovered and passing the admission test; has no owner and sits inside an existing file's scope | Append a new `## N.` section in numerical position (with its Origin line when discovered) and its Implementation Order row, place the row in a phase of `todo/implementation-plan.md`, then sync the plan. |
| Needs a file | Operator-directed, or discovered and passing the admission test; a new subject no file owns | Delegate to `create-todo`, or to `plan-new-feature` when the subject is a user-facing feature or a new app, passing on whether it is discovered and the run id. |
| Backlog | Discovered and failing the admission test, or the operator asked to defer it | Append one line to `todo/backlog.md` (above `## Removed with operator approval`) with the next free id: `- [B-NNN] <title> -- app: <app> -- source: <key> -- added: <today> -- summary: <what> -- needs: <refs or ids> -- why deferred: <the admission clause it failed, or the operator's words> -- promote when: <trigger>`. The backlog has no cap: an idea is never dropped for lack of room. |

Shared or app-local: work lands in the core domain only when a second app needs it now. One app's need is filed in that app's domain, with a note naming the day it would move.

**Promoting a backlog entry** is this skill run on the entry: a promotion the operator asks for is operator-directed, and one a campaign makes on its own passes the admission test and carries an Origin line. The promoting commit writes the section (carrying the entry's `source` as its `-> SOURCE:` line) and deletes the entry, or `validate` fails on the duplicate source. The section carrying the key is what lets the entry leave: an entry deleted any other way, with no merge marker on a survivor and no operator removal record, is `backlog-dropped`. A release's backlog review (an item in every release section) runs through this skill: each entry for the release's app or `suite` (every entry for an `isotone-v` release) is promoted, or the operator is asked whether it may wait and the answer is appended to the entry as `-- reviewed: <tag> <YYYY-MM-DD> deferred by operator: "<their words>"`. Never write a deferral the operator did not say. When the review promotes only part of an entry, the rest stays and the entry gains `-- reviewed: <tag> <YYYY-MM-DD> promoted DNN TNN §N`. Committed `reviewed:` fields are never edited or removed.

### 6. New sections are born complete

A new section carries everything the format requires from birth: context paragraph, micro-step checklist with a `Commit:` item, `Test checkpoint:` citing one of the six proofs in `todo/README.md`, a stop condition when it is an experiment, a `**Freeze check:**` when it changes the bitstream or file safety, `Needs:` when it needs a host, `Requires:` when it needs an environment capability, and its Implementation Order row with a real `Depends On`. A section filed as a one-line stub is a plan defect, not a head start.

Give it a dependency edge, not a wish. If it must wait on another section, say so in `Depends On`; if it truly stands alone, `--`.

### 7. Wire and verify

- Point at related sections with `-> XREF:` and point back from each target. One-sided XREFs are FATAL.
- Name a `(item: "...")` on any deferral that hands this work to an owner, so closure is detectable at item granularity.
- A finding filed from evidence carries a `-> SOURCE: <key>` line naming what it was filed from, so a second run of the same scan cannot open a second row for it.
- Place the new section's row in exactly one phase table of `todo/implementation-plan.md`, no earlier than the phase of anything it depends on.
- Then prove the tree still holds:

```bash
python scripts/todo-graph.py validate
python scripts/todo-graph.py plan --sync
python scripts/todo-graph.py plan --check
```

### 8. Commit and report

Commit as `todo: file <what> in <ref>`, or `todo: backlog <what> as B-NNN`. Report the address (`DNN TNN §N` or `B-NNN`), whether it is operator-directed or discovered (and for discovered work the admission clause it passed or failed and the run id on its Origin line), why that home won over the alternatives searched, and the size line the Progress block now shows.

## Guardrails

- Do not file work into a `[x]` section. New granularity on shipped work is a new section.
- Do not write a bare `TNN` reference without a section on any line containing `XREF`, `Depends`, or `|`.
- Do not leave the plan unsynced. A section with no plan row breaks `plan --check`.
- Do not file the same real-world thing twice. Search first (the backlog too), and stamp automated filings with `SOURCE:`.
- Do not create a discovered section that fails the admission test or lacks its Origin line. Merge, supersede, or backlog.
- Do not stamp an Origin line on work the operator asked for, and do not apply the admission test to it.
- Do not delete a backlog entry except by promotion, by a merge that keeps its key as a merge marker, or with the operator's words in a removal record.
- Do not change `todo/budget.json`. Only the operator does, in words the history entry quotes.
- Do not file app-agnostic work into one app's domain when a second app already needs it, and do not file into the core domain what only one app needs.
