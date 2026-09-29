# Review-run records

schema: 1

Panel wiring: every round runs a slot from `.conclave/panel.toml`, GPT through codex on every slot (`bulk` medium for Full rounds 1-2, `signoff` and `depth` high after), and the writer's family never reviews. Round headings in the per-section review files read `GPT panel`, and a round's `model:` must be a registered model of its heading's family. There are no fallback slots: a failed round waits for the operator.

Terminology, pinned. A run is one section's independent review, however many rounds it took; an engagement is the same thing counted for the report. A round is one reviewer invocation against one candidate. Voided rounds keep their numbers. Two outcomes skip the panel mapping without voiding their findings: stamp, a stamp-review pass over the staged stamp, and independent, a non-panel independent pass inside a panel block; their refs count in yield and coverage, but they meet no panel section. Empty is outcome-based: an engagement is empty when every round came back empty.

One block per stamped section whose review reached an independent round, appended by `review-todo-section` after the stamp. Checked by `scripts/todo-runs.py --check`, which cross-reads the per-section review files: every listed ref must resolve to an independent-marked finding heading, every `(independent)` mark must be listed by exactly one run, every review file must have a run block, `empty` must equal the rounds with outcome `empty`, and `refuted` must equal the listed refs whose disposition is refuted. Panel runs additionally re-read their round verdicts from the review file's panel sections, and every candidate must be a commit that exists.

Each round line carries the model, provider, exact version (`unresolved` when the invocation never pinned one), cost in total tokens (`unresolved` when unrecorded), latency in wall-clock seconds from the reviewer invocation to its returned output (`unresolved` when untimed), opportunity scope, review purpose, provenance, and the refs that round raised. Findings described only in review prose, without a ref, are noted in `#` comments and counted nowhere.

No runs are recorded yet.
