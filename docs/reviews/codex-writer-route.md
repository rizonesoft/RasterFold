## Round 1: spark (`muse-spark-1.3` via `muse`)

- Writer: codex
- Run: 2026-10-27T19:59:19Z
- Effort: high
- Lenses: adversarial, integration
- Lines: 21
- Digest: 1bfa96e6abbb37a5

Patch fully inspected (all 830 lines), plus the changed sources, tests, and governing `AGENTS.md`.

## Lens: `adversarial` — verdict: needs-attention

1. **P2 — Codex Stop adapter is not fail-open.** `.codex/hooks.json:8` shells out to `git rev-parse --show-toplevel` before Python runs, and `scripts/codex-campaign-hook.py:10-14` has no guard around `stdin.read()`, root resolution, or the import. Any failure there (cwd outside a checkout, missing/broken git, closed stdin) exits non-zero before the shared guard's fail-open path in `scripts/campaign_guard.py:988-1035` is reached. Sibling hook `.agents/hooks.json:8` deliberately avoids git (`CLAUDE_PROJECT_DIR or cwd`). Fix: resolve root from the hook event/cwd with git only as fallback, and wrap the adapter so it always exits 0.
2. **P2 — Codex session identity is unverified end to end.** `scripts/campaign_guard.py:1004` keys on `conversationId`/`session_id`, and `scripts/campaign_guard.py:841` returns allow-stop when the session is empty or mismatched. The only proof is a synthetic payload in `scripts/tests/test_codex_campaign_hook.py:72-76`. If the real Codex Stop event names the session differently (or `$CODEX_THREAD_ID` in `.agents/skills/process-plan/SKILL.md:56` is not that field), the hook never blocks the owning Codex session and the campaign runs unguarded while the guard file claims ownership. Fix: capture one real Codex Stop payload as a fixture and assert the mapping.
3. **P2 — Stop decision vocabulary is passed through untranslated.** `scripts/campaign_guard.py:932` returns `{"decision": "block"}`, rewritten to `continue`-style only for `conversationId` events in `scripts/campaign_guard.py:1024-1025`. The Codex path therefore emits raw `block`, and `scripts/tests/test_codex_campaign_hook.py:77-78` bakes that assumption in. There is no in-repo evidence Codex honors `block` on Stop despite the adapter docstring claiming adaptation; if it ignores the verdict, the guard is decorative. Fix: confirm the Codex Stop contract and translate (or fail closed at campaign start).
4. **P3 — Muse review text is all of stdout.** `scripts/review-dispatch.py:323-327` builds a stdout-captured invocation with no `-o` equivalent, and `scripts/review-dispatch.py:542-544` treats the whole stream as the review. Any non-review stdout (progress, warnings) from a real `muse exec` pollutes `blocked()` / `valid_one_pass_footer()` and recorded receipts; the fake in `scripts/tests/test_codex_writer_review.py:706-720` prints only the review, so this is untested. Fix: prefer a dedicated output-file flag when available, or extract the trailing verdict block.
5. **P3 — Muse runs without sandbox confinement.** The Muse argv carries `--disable-write --disable-shell --disable-web-tools` but nothing corresponding to Codex's `--sandbox read-only --ephemeral` in `scripts/review-dispatch.py:328-329`, while the docstring at `scripts/review-dispatch.py:322` still promises a read-only review. Residual risk is low given the flags (and unknown flags fail safe to outage), but the review target is untrusted input. Fix: add a read-only sandbox flag if the Muse CLI offers one.

## Lens: `integration` — verdict: needs-attention

1. **P2 — Codex heartbeat has no concrete tool mapping.** `.agents/skills/process-plan/SKILL.md:56` maps abstract schedule tools to exact tools for `claude`, `muse`, and `agy`, but Codex gets only "native in-session schedule tools" with no names and no probe. The heartbeat contract it must satisfy ("fires only when idle, runs inside this session with full context", `.agents/skills/process-plan/SKILL.md:60`) is therefore unverified; a detached scheduler breaks `heartbeat-still` accounting and session-scoped blocking. Fix: name the Codex schedule tools plus a list/cancel probe, and gate `acquire` on observed same-session targeting.
2. **P2 — Hook root resolution diverges from the sibling hooks.** `.codex/hooks.json:8` uses `git rev-parse --show-toplevel`, while `.agents/hooks.json:8` uses project dir or cwd; then `scripts/codex-campaign-hook.py:12` pins `parents[1]`, overriding the event `workspacePaths`/`cwd` handling in `scripts/campaign_guard.py:999-1001`. Under worktrees, submodules, or off-root cwds the Codex hook can guard the wrong root. Fix: prefer the hook-provided directory, git toplevel only as fallback.
3. **P3 — Documentation precision drift.** `AGENTS.md:82` says "Both routes use medium for plan and completion" but there are now four routes (two reviewers); and `scripts/todo-graph.py:1168` still says `codex_implementer` "opts new stamps into the independent Claude panel" when `scripts/todo-graph.py:1175-1178` now makes it a legacy GPT-name classifier. Fix: "Both reviewers" / "All routes", and reword the docstring to the fallback-classification role.
4. **P3 — Test coverage gaps on the new paths.** No test for adapter fail-open when `git` fails, no test executing the non-active platform key (`command` vs `commandWindows` in `.codex/hooks.json:8-9`), and no stdout-pollution case for the Muse branch. Fix: add the three cases to `scripts/tests/test_codex_campaign_hook.py` and `scripts/tests/test_codex_writer_review.py`.

Checked and holding: shared-schedule intersection in `scripts/review-dispatch.py:187` keeps the historical three-writer pin loadable; `committed_pin` in `scripts/todo-graph.py:6656-6665` replays historical pins and schedules so old receipts keep their original rules; family separation (`scripts/review-dispatch.py:67-68`, enforced in `load_pin` and `returned_review`) still forbids self-review and fallback on all four routes; outage stays non-approval.

HIGHEST: P2
OVERALL: needs-attention

## Writer note: Round 1

- Candidate: working-tree patch against `0f08af291dd36a33a890b5b1e6444a8a24710136`, SHA-256 `72f83a02a3ad7ff792e3e0c05575effe0eb50b862238fed39cf7d7f6cd354662`.
- Fixed hook startup: locate the owning project without Git, return an empty decision on adapter exceptions, and test a broken Git and a nested checkout.
- Native Codex session identity and heartbeat remain unproven here. The campaign instructions now require a real Stop event, matching session identity, proven continuation, and named same-session scheduling tools before acquiring a guard.
- Codex uses `decision: block` for Stop continuation according to OpenAI's hooks documentation at https://learn.chatgpt.com/docs/hooks; the local adapter fixture exercises this shape, but is not a native-runtime proof.
- Muse now uses its JSON event stream, checks the configured model and provider, extracts only the terminal answer, and refuses malformed output or a missing round verdict. Regression tests exercise noisy events, an invalid stream and a wrong model.
- Muse's installed help says its sandbox is enabled by default; the reviewer invocation also disables shell, workspace writes, web tools and reminders. No separate read-only sandbox flag was offered by `muse exec --help`.
- Updated the route wording and legacy classifier description. Windows hook execution still needs native Windows verification; both OS-specific command fields carry the same loader apart from the Python executable name.

## Round 2: spark (`muse-spark-1.3` via `muse`)

- Writer: codex
- Run: 2026-10-27T20:05:58Z
- Effort: medium
- Lenses: adversarial
- Lines: 13
- Digest: b146d46b713b2dd5

## Lens: `adversarial` — verdict: needs-attention

Round 1 fixes verified: git-before-Python startup is gone, the adapter fails open on `Exception`, Muse output is now parsed from JSONL terminal events, and route wording was fixed. Session identity, Stop vocabulary, and sandbox confinement remain unproven or partial, as the writer notes admit.

1. **P2 — Hostile reviewer JSON crashes the dispatcher.** `scripts/review-dispatch.py:509` parses every stdout line with `json.loads`, but `scripts/review-dispatch.py:524` catches only `OSError, UnicodeDecodeError, ValueError`. A deeply nested array or object raises `RecursionError`, which escapes `review()`, bypasses the `exit 1` no-review path, and produces a traceback with no JSON summary or record. A compromised `muse` binary on `PATH` can trigger this within the 2 MB output cap.
2. **P2 — Stop vocabulary flips on key presence.** `scripts/campaign_guard.py:1004` resolves the session from `conversationId or session_id`, but `scripts/campaign_guard.py:1024` rewrites to `continue` when `"conversationId" in event`, regardless of value. A payload like `{"session_id": "owner", "conversationId": null}` blocks under Codex semantics yet is emitted as `continue`. If Codex honors only `block`, still unproven from docs alone, the owning session is never held.
3. **P2 — Codex session identity is still synthetic.** Tests use only hand-built `{"session_id": ...}` payloads at `scripts/tests/test_codex_campaign_hook.py:31`, `scripts/tests/test_codex_campaign_hook.py:52`, and `scripts/tests/test_codex_campaign_hook.py:71`. There is no captured native Stop fixture proving the real field equals `$CODEX_THREAD_ID`. A real event naming the session differently never matches the guard, so the campaign runs unguarded. Do not infer campaign readiness; the campaign remains paused.
4. **P2 — Hook root is hijackable by planted markers.** `.codex/hooks.json:8` walks up from `cwd` and takes the nearest ancestor containing both marker files, and `scripts/codex-campaign-hook.py:10` re-derives that root. Code with write access can plant `subdir/.codex/hooks.json` plus `subdir/scripts/codex-campaign-hook.py`, then fire a Stop from below to divert the guard lookup to an empty root that returns `{}`. Sibling `.agents/hooks.json:8` uses project dir or `cwd` with no walk. No test covers planted markers, only nested git at `scripts/tests/test_codex_campaign_hook.py:58`.
5. **P3 — Abort path violates the always-exits-0 contract.** `scripts/campaign_guard.py:988` promises exit 0 always, but `scripts/campaign_guard.py:1027` and `scripts/codex-campaign-hook.py:14` catch `Exception` only. `KeyboardInterrupt`, `SystemExit`, or cancellation escapes non-zero with no defined Codex meaning. `scripts/codex-campaign-hook.py:11` also reads `stdin` unbounded before any budget applies, so a flooded Stop payload can exhaust memory.
6. **P3 — Muse runs without OS-level sandbox confinement.** `scripts/review-dispatch.py:324` disables write, shell, web tools, and reminders, with `cwd` at the repository root, but carries no read-only sandbox flag. Safety rests on the Muse default sandbox and flag names staying correct. A new tool family or renamed flag silently widens a reviewer that reads untrusted candidate diffs.

HIGHEST: P2
OVERALL: needs-attention

## Writer note: Round 2

- Candidate: working-tree patch against `0f08af291dd36a33a890b5b1e6444a8a24710136`, SHA-256 `c417eeef64222f4a81fd37983e2bebe02d7c10d3ee95cb4497a66ec768a6be9b`.
- Fixed the Muse event parser to return no review for recursion-depth failures; a nested JSON fixture reproduces the prior traceback.
- Fixed the shared guard to use Antigravity's continuation vocabulary only for a nonempty `conversationId`; a Codex payload with a null field now keeps `decision: block`.
- A native Codex Stop identity proof is still unavailable. The preflight explicitly refuses campaign acquisition without that proof and a same-session heartbeat; no campaign is running or claimed ready.
- The hook loader now selects the outermost matching project marker so a nested planted marker and script cannot take over the guard; the regression fixture exercises that case.
- The adapter bounds incoming payloads, records an oversized payload as a hook error, and fails open on an import exit or interruption. Fixtures exercise both failures.
- Muse's installed default sandbox and disabled shell and write tools are used; its CLI does not expose a separate read-only sandbox selector. The invocation explicitly pins the Meta provider and checks the configured model event.
- Native Windows hook fixtures and Muse review-route fixtures passed through `scripts/windows.py`; a real Windows Muse dispatch also returned the scheduled high-effort verdict. These are separate from the unproven native Codex Stop continuation.

## Round 3: spark (`muse-spark-1.3` via `muse`)

- Writer: codex
- Run: 2026-10-27T20:10:41Z
- Effort: low
- Lenses: adversarial
- Lines: 16
- Digest: 6c45779318e29c7a

## Lens: `adversarial` — verdict: needs-attention

Round 2 fixes verified in current sources: `scripts/review-dispatch.py:524` now catches `RecursionError` in `muse_response`; `scripts/campaign_guard.py:1024` now rewrites only on truthy `event.get("conversationId")`, so a null field keeps `block`; `.codex/hooks.json:8` now walks outermost so a nested planted marker no longer diverts; `scripts/codex-campaign-hook.py:9-25` bounds stdin at 2M chars and catches `BaseException`. Session identity, Stop-vocabulary proof, and OS-level Muse confinement remain unproven, as the writer notes admit.

1. **P2 — Hostile Muse binary bypasses the output cap through the leaked prompt path.** `scripts/review-dispatch.py:324-328` passes `--prompt-file <tmp>/prompt.md` in argv to the untrusted reviewer, but `scripts/review-dispatch.py:558-563` watches only `[stdout_path, message_path]`. A compromised `muse` binary knows the prompt path and can overwrite it with unbounded bytes after `run()` starts the size watch. The 2 MB `MAX_OUTPUT_BYTES` gate never fires, filling disk/tmp quota while the dispatcher reports a normal round. Fix: add `muse_prompt` to `watched`, or pass the prompt on stdin like the Codex route.

2. **P2 — Outermost-marker walk is hijackable from any ancestor.** `.codex/hooks.json:8` selects `next(... reversed((cwd, *cwd.parents)) ...)` — the outermost ancestor containing both markers. Any ancestor directory the invoking user can write (`/tmp`, `$HOME`, a parent checkout) holding a planted `.codex/hooks.json` plus `scripts/codex-campaign-hook.py` now wins over the real project root for every nested `cwd`. The Round 2 fix inverted the direction (inner plant → outer plant) without adding a trust anchor. Sibling hook design uses project dir or `cwd` with no walk. Fix: prefer the hook-supplied directory/`cwd` with marker existence as sanity check only, never an upward search.

3. **P2 — Session-field collision mistranslates the Stop verdict across harnesses.** `scripts/campaign_guard.py:1004` reads `conversationId or session_id`, while `scripts/campaign_guard.py:1024` treats any truthy `conversationId` as Antigravity continuation vocabulary (`continue`). A Codex Stop event carrying a truthy `conversationId` is therefore emitted as `continue`; if Codex honors only `block` (still proven only by docs/fixture, no native capture), the owning Codex session is never held and the campaign runs unguarded. Key presence/value cannot discriminate harnesses. Fix: discriminate by harness/event shape, not one shared field, and fail closed at campaign start without a native Codex fixture.

4. **P2 — Abort/exhaustion during Muse parsing escapes the JSON-summary contract.** `scripts/review-dispatch.py:506-525` catches only `OSError, UnicodeDecodeError, ValueError, RecursionError`; `MemoryError`, `KeyboardInterrupt`, and `SystemExit` from `read_text` or the list comprehension propagate. `scripts/review-dispatch.py:959-977` wraps `review()` in `except (Refused, Unrecorded, OSError)` only, so a flooded-but-under-cap 2 MB stream that exhausts memory, or a Ctrl-C/cancellation during parse, exits with a traceback and no JSON summary or record. Automation polling stdout JSON sees an outage with no receipt. Fix: catch `MemoryError` (return no-review) and `BaseException` abort at `main` with a JSON no-review summary.

5. **P3 — Muse reviewer runs unsandboxed on the repository root.** `scripts/review-dispatch.py:471` spawns with `cwd=ROOT` and `scripts/review-dispatch.py:324-328` relies on `--disable-write --disable-shell --disable-web-tools` self-restraint, with no `--sandbox read-only` equivalent. A compromised or flag-renamed `muse` binary reads the full candidate diff (supplied via `--prompt-file`) and has unrestricted filesystem and network access. Low probability given flags, but the review target is untrusted input. Fix: add OS-level read-only confinement when the CLI offers one, else document the residual.

HIGHEST: P2
OVERALL: needs-attention

## Writer note: Round 3

- Candidate: working-tree patch against `0f08af291dd36a33a890b5b1e6444a8a24710136`, SHA-256 `e8edaba6dc03c5eb36420968d883d3597e9c13cba02a3727e171f3dbf46411f9`.
- The prompt-file flood requires a compromised reviewer executable. Such an executable can write arbitrary files under the user account, so watching one additional path would not provide the claimed protection. The existing stdout limit still bounds normal reviewer output; the installed CLI is a trusted dependency.
- Reworked hook root selection around the current Git checkout rather than searching ancestor markers. Nested and parent marker fixtures confirm they cannot redirect the project hook. A session rooted in a nested Git repository must satisfy the campaign preflight for that checkout and cannot acquire the RasterFold guard.
- The Codex adapter now removes `conversationId` from native Stop events before passing them to the shared guard, so a collision cannot change the Codex decision vocabulary. A regression fixture exercises a nonempty conflicting field.
- Muse event parsing now treats `MemoryError` as no review. User interruption remains an interruption: swallowing `KeyboardInterrupt` or `SystemExit` in the dispatcher would defeat the operator stop contract.
- The Muse CLI offers no explicit read-only sandbox selector. The configured invocation keeps its default sandbox enabled and disables shell, workspace writes and web tools; this does not claim protection against a compromised executable.

## Round 4: spark (`muse-spark-1.3` via `muse`)

- Writer: codex
- Run: 2026-10-27T20:14:26Z
- Effort: low
- Lenses: consistency
- Lines: 18
- Digest: e0c77fadd05c036b

## Lens: `consistency` — verdict: approve

Round 3 dispositions re-checked against current sources:

- R3-1 (prompt-file flood): declined. `scripts/review-dispatch.py:558-563` still watches only output paths, not `tmp / "prompt.md"`. Under consistency this matches the sibling Codex path (stdin prompt is also unwatched input, not output). No module rule requires watching inputs. Holding as documented residual, not a consistency defect.
- R3-2 (outermost-marker hijack): fixed as claimed. `.codex/hooks.json:8-9` no longer walks ancestor markers; it resolves `git rev-parse --show-toplevel` and requires both `scripts/codex-campaign-hook.py:14` root derivation and marker existence. Nested/parent marker fixtures in `scripts/tests/test_codex_campaign_hook.py:831-871` confirm the anchor change.
- R3-3 (session-field collision): fixed for the tested shape. `scripts/codex-campaign-hook.py:21-25` pops `conversationId` before the shared guard, and `scripts/campaign_guard.py:1024` (truthy check from Round 2) is retained. Fixture at `scripts/tests/test_codex_campaign_hook.py:782-789` covers null and foreign values.
- R3-4 (abort during Muse parse): partially fixed as claimed. `scripts/review-dispatch.py:524` now catches `MemoryError` alongside `OSError, UnicodeDecodeError, ValueError, RecursionError`. `KeyboardInterrupt`/`SystemExit` still propagate, matching the operator-stop contract and the sibling `campaign_guard.py` fail-open-on-`Exception` convention.
- R3-5 (Muse unsandboxed): documented, not fixed. `scripts/review-dispatch.py:322-330` no longer promises a Codex-style sandbox for Muse; it states shell/write/web-tools are disabled. Consistent with the claimed CLI surface.

Whole-patch consistency inspection (candidate r4, base `0f08af291dd36a33a890b5b1e6444a8a24710136`): routing, schedule, receipts, and hook wiring match across `AGENTS.md`, `GEMINI.md`, skills, prompts, `.conclave/reviewers.toml`, and both dispatchers. No P1/P2 normal-operation consistency failure found. Three minor inconsistencies remain:

1. **P3 — Diff-in-prompt instruction is over-broad across routes.** `docs/plan-review-prompt.md:1`, `docs/completion-review-prompt.md:1`, `docs/review-round-prompt.md:3`, and `.agents/skills/independent-review/SKILL.md:39` tell every writer to embed the exact candidate diff for Muse, even on `claude`/`muse`/`agy` routes where the reviewer is Codex with shell and sandbox access. Harmless for the `codex` route, but in normal operation the other three routes carry unneeded diff bloat. Scope the sentence to the Codex-writer/Muse-reviewer route.
2. **P3 — New legacy classifier is case-sensitive where the old delegation may not have been.** `scripts/todo-graph.py:676` uses `re.fullmatch(r'gpt-[a-z0-9][a-z0-9.-]*', ...)` with no `re.I`, while the surrounding `codex_implementer` name match at `scripts/todo-graph.py:665` uses `re.I`. A receipt recording an uppercase model such as `Codex (GPT-4o)` would newly return `False` and change `panel_fallback`. No such receipt is in evidence and pins use lowercase, so this is hypothetical; add `re.I` or an uppercase regression case.
3. **P3 — Harness discrimination uses two different signals.** The adapter gates its `conversationId` strip on `event.get("hook_event_name") == "Stop"` (`scripts/codex-campaign-hook.py:22`), while the shared guard gates continuation vocabulary on truthy `event.get("conversationId")` (`scripts/campaign_guard.py:1024`). A real Codex Stop event that names the session correctly but carries no `hook_event_name` field would skip the strip yet still hit the Antigravity rewrite. No native Codex fixture exists to settle the real shape, and no campaign is started, so this is unproven rather than operational; discriminate once by harness, or strip unconditionally on the Codex adapter path.

HIGHEST: P3
OVERALL: approve

## Writer note: Round 4

- Candidate: working-tree patch against `0f08af291dd36a33a890b5b1e6444a8a24710136`, SHA-256 `b0346ab96f010ce00664787c5961d91d7462dc1f1d18590db075887de49db838`.
- Clarified that the candidate-diff instructions apply to the Codex writer route.
- An uppercase model ID is not parsed by `IMPLEMENTER_RE`, which accepts lowercase model IDs only; the classifier does not introduce the proposed change.
- Simplified the Codex-only adapter to discard `conversationId` for every object payload, independent of the event-name field. The regression fixture covers an absent event name and conflicting conversation ID.

## Round 5: spark (`muse-spark-1.3` via `muse`)

- Writer: codex
- Run: 2026-10-27T20:17:20Z
- Effort: low
- Lenses: record
- Lines: 14
- Digest: cb3d57e8a7af50f9

## Lens: `record` — verdict: approve

Round 4 dispositions verified in current sources and the r5 patch:

- Diff-in-prompt scoping is present: `docs/plan-review-prompt.md:371`, `docs/completion-review-prompt.md:266`, `docs/review-round-prompt.md:383`, and `.agents/skills/independent-review/SKILL.md:39` all scope the exact-diff instruction with `For --writer codex`.
- Adapter harness discrimination is present: `scripts/codex-campaign-hook.py:29-31` unconditionally pops `conversationId` from dict payloads, so a Codex Stop event can no longer be rewritten to Antigravity `continue` vocabulary.
- Shared-schedule replay for the new route is present: `scripts/review-dispatch.py:451-460` adds the `muse` CLI, family, and efforts; `scripts/review-dispatch.py:565-566` extracts only the terminal Muse answer; `scripts/review-dispatch.py:578-582` requires a round verdict; `scripts/todo-graph.py:6678-6730` replays route, effort, and lenses from the committed pin, so a `codex -> spark` round counts via `returned_review` and outages still do not advance the schedule.

Whole-patch record inspection (dispatch `record`/`record_completion` at `scripts/review-dispatch.py:802-849`, findings reader at `scripts/review-dispatch.py:655-731`, receipt checks at `scripts/todo-graph.py:6672-6831`): Muse rounds are recorded with the same Lines/Digest envelope, `record_completion` still requires exactly one returned plan/completion pass with candidate binding, and the new-route stamp test at `scripts/tests/test_codex_writer_review.py:1029-1060` exercises the replay. Historical receipts remain loadable via the shared schedule path.

1. **P3 — Legacy GPT classifier is case-sensitive on the model token.** `scripts/todo-graph.py:1178` matches `gpt-[a-z0-9]...` without case-insensitivity while the implementer name match at `scripts/todo-graph.py:1173` uses case-insensitive matching. An uppercase receipt model such as `Codex (GPT-4o)` would return `False` and change `panel_fallback`. No such receipt is in evidence and pins use lowercase, so this is hypothetical with no normal-operation failure demonstrated.

HIGHEST: P3
OVERALL: approve

## Writer note: Round 5

- Candidate: working-tree patch against `0f08af291dd36a33a890b5b1e6444a8a24710136`, SHA-256 `7971c09646169e82dde79e7736abb07a8aadf7cf6e9c0205ea8695abc898f0c1`.
- The repeated uppercase-model note is not a regression: `IMPLEMENTER_RE` in `scripts/todo-graph.py` accepts lowercase model IDs only, so the example is rejected before the classifier both before and after this change.
- Correction to the Round 4 note: the regression fixture uses a null event name, rather than an absent key. The adapter removes the conflicting field for every parsed object.
