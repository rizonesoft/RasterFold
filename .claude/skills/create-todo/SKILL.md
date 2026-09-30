---
name: create-todo
description: Author a whole new TODO file -- frontmatter, Goal, Current state, Inputs, Outcome with Adjacency, Implementation Order, sections, Verification -- then wire indexes, XREFs, and the plan. Use when add-todo routes here.
---

# Create TODO

A new file is warranted when a subject no existing file owns needs a durable home. Most work does not need one: `add-todo` decides, and reaching here directly tends to produce a second TODO over an existing one.

## Workflow

### 1. Confirm the home

Name the domain and the next free `TODO-NN` number in it. Confirm no existing file covers the subject by searching as `add-todo` does. A file that overlaps an existing file's scope is a defect at birth.

A file is operator-directed by default: the operator asked for it, so its sections carry no Origin line and nothing caps how many it holds or how many land in a phase. When a campaign invokes this skill for work it discovered on its own, every section passes `add-todo`'s admission test, and carries `**Origin:** discovered run=<run id> <YYYY-MM-DD>` directly under its heading, for provenance (`python3 scripts/todo-graph.py query growth --since <run start>` reports it by run; nothing caps it); sections that fail the test become backlog entries in `todo/backlog.md` instead.

A new domain is allowed when no domain owns the subject: create `todo/NN-kebab-name/` with its `INDEX.md` (copy the shape of an existing domain index), append it to the Domain order table in `todo/TODO-00-INDEX.md`, and take the next free number. The tooling reads domains from the tree, so nothing else registers it.

### 2. Author from the template

Copy `.claude/skills/create-todo/todo-template.md` to `todo/<domain>/TODO-NN-<short-name>.md` and fill every part:

- **Frontmatter:** `schema_version: 1`, a globally unique kebab-case `id`, `domain` matching the directory, `status: draft`, `title`. `depends_on` only for TODOs that must fully ship first; prefer section-level edges.
- **Goal:** one paragraph, plain terms, true when the file is done.
- **Current state:** what exists RIGHT NOW with real paths, each measured figure carried as a `<!-- claim: ... -->` line `scripts/todo-claims.py` re-measures. Without this the implementer greps the repo to find the starting line.
- **Inputs:** specs, measurements, `docs/research.md` items, papers or reference codecs, existing files with what each section consumes, and `-> XREF:` lines to related work.
- **Outcome:** observable end states, plus the one `**Adjacency:**` line and its rationale paragraph.
- **Implementation Order:** one row per section with real `Depends On` edges (`--` only when truly standalone).
- **Sections:** context, micro-step checklist with `Commit:`, `Test checkpoint:` citing one of the six proofs in `todo/README.md`, a stop condition on every experiment (the bar a measurement must clear, and what ships when it does not), `**Freeze check:**` on every section that changes the bitstream or file safety (and `frozen: true` in frontmatter), `Needs:` on host-bound sections, `Requires:` on environment-gated sections.
- **Verification:** the file-level checks `process-todo-file` will run.

Size sections by what holds together (max 30 items; a file caps at 55 sections). Every section must be implementable with zero conversation context: no "as discussed".

### 3. Wire it in

- List the file in the domain `INDEX.md` (filename must appear verbatim) and under Active TODOs in `todo/TODO-00-INDEX.md` when it is active work.
- Reciprocate every `-> XREF:`: each target file must point back, or `validate` is FATAL.
- Place every section in exactly one phase table of `todo/implementation-plan.md` (a `### Phase <N> -- <Title>` heading with a `| ✔ | Section | Deliverable | Items |` table), no earlier than the phase of anything it depends on, then:

```bash
python3 scripts/todo-graph.py validate
python3 scripts/todo-graph.py plan --sync
python3 scripts/todo-graph.py plan --check
```

### 4. Commit and report

Commit as `todo: author <id> (<n> sections)`. Report the file, its phase placement, its dependency edges, and the new plan totals.

## Guardrails

- Do not author a file whose subject an existing file owns. Search first.
- Do not file-only stub sections. Every section is born buildable.
- Do not skip the Adjacency line. Silence is not a decision.
- Do not author a compression or speed section without its measurement command and its stop condition. A section whose success cannot be measured cannot be stamped.
- Do not leave rows unsequenced. `plan --check` must pass before the commit.
