<!--
    Copyright (c) 2026 Michael Vandeberg

    Distributed under the Boost Software License, Version 1.0. (See accompanying
    file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

    Official repository: https://github.com/cppalliance/corosio
-->
# `doc/lint` — the documentation-quality toolkit

These scripts implement the enforcement tiers in `doc/STYLE_GUIDE.md` Part F.0. They run in
the **Documentation** workflow (`.github/workflows/docs.yml`), in the `antora` job, after the
site build. Node built-ins only, no dependencies of their own.

They were ported from Capy's `doc/lint`, which remains the origin for the shared design
rationale. What is Corosio-specific is recorded here.

| Script | What it checks |
|---|---|
| `doc-lint.mjs` | Structural AsciiDoc/nav rules (A1, A6, B2, ANCHOR, SHAPE, D2). JSON on stdout. |
| `extract-docstrings.mjs` | Extracts header docstrings into `.docstrings/*.adoc` so Vale can lint them. |
| `sentence-length.mjs` | **The authority for C2** (no sentence over 25 words), over both corpora. |
| `check-include-tags.mjs` | Every `include::example$…[tag=…]` resolves to a live tag in a compiled source. |
| `mrdocs-warnings.mjs` | Runs MrDocs directly and parses its reference-surface warnings. |
| `selftest.mjs` | Mutates the linters and asserts they notice. Exit 1 on regression. |
| `baseline.mjs` | Runs every check and snapshots their findings to `baseline.json`. |
| `check-no-new-violations.mjs` | **The gate.** Diffs a fresh run against `baseline.json`. |
| `baseline-diff.mjs` | Explains what replacing `baseline.json` with a candidate would change. |

## Running it locally

Vale must run from `doc/`, with `node_modules/.bin` on `PATH`: it shells out to
`asciidoctor` to parse AsciiDoc, and without it Vale exits 2 having printed **nothing**,
which greps identical to a clean run.

```sh
cd doc
export PATH="$PWD/node_modules/.bin:$PATH"
vale --output=JSON modules
node lint/extract-docstrings.mjs && vale --output=JSON lint/.docstrings
node lint/doc-lint.mjs
node lint/sentence-length.mjs
node lint/selftest.mjs

# mrdocs-warnings needs the MrDocs the site build used. build_antora.sh exports
# MRDOCS_ROOT; point it at that install rather than relying on the cache scan.
MRDOCS_ROOT="$PWD/build/mrdocs/MrDocs-0.8.0-Linux" node lint/mrdocs-warnings.mjs
```

`mrdocs-warnings.mjs` applies **no** version check to a binary under `MRDOCS_ROOT`, on
purpose: the `develop-release` asset is a rolling build whose reported version has already
changed scheme once (`0.8.0+<sha>` locally, `2026.9.5` in CI, days apart, same asset). What
the check needs is the MrDocs that produced the rendered reference, which is what
`MRDOCS_ROOT` names. The `PINNED_VERSION` constant is only a tiebreaker for the fallback
cache scan.

A `0` in the output is not evidence of a clean run by itself — it is at least as often
evidence the run never happened. Confirm a non-zero total somewhere before trusting a zero.
Vale does not enforce C2 either way; its authority is `sentence-length.mjs`.

## How the gate works

`baseline.json` is a snapshot of every finding that already existed when it was taken.
`check-no-new-violations.mjs` runs a fresh scan and reports only fingerprints **not** in the
snapshot. Everything in the snapshot is grandfathered.

Which findings *block* is the `--gate <check>:<regex>` spec in the workflow. Each regex is
tested against the **whole** fingerprint.

| Check | Fingerprint | Rule position |
|---|---|---|
| `doc_lint` | `rule:file:#N:message` | **head** |
| `sentence_length` | `C2:file:#N:message`, `advisory-C2:…`, `BACKTICK:…` | **head** |
| `vale_adoc`, `vale_docstrings` | `file:#N:Check.Name` | **tail** |
| `mrdocs_warnings` | `file:#N:message` | — |

`#N` is the Nth occurrence of that (head, tail) pair, **not a line number**, so inserting
text above a finding does not rename it.

**A Vale gate spec must never carry a leading `^`.** The check name is at the tail, so
`^Corosio\.PartHeadings$` matches nothing and the comparator then reports
`gated: true, gatedNew: 0` at **exit 0** — a gate that announces it is gating while checking
nothing. Measured on this corpus; see the bite-test log below.

**Never hand-edit `baseline.json`.** Reseed via the `workflow_dispatch` steps in the
workflow, never locally: a local run differs from a CI run and would grandfather hundreds of
local-vs-CI drift fingerprints.

> **The committed `baseline.json` is a LOCAL seed and must be reseeded in CI before the gate
> is trusted.** It had to be: the gate cannot run at all without a baseline, and the
> bite-tests below needed one to compare against. It was generated on a developer machine, so
> it carries local-vs-CI drift — a different MrDocs develop build hash and a different
> file-processing order at minimum. Run the Documentation workflow via `workflow_dispatch`,
> read the `baseline-diff.txt` report, and commit the candidate. Until that happens, treat a
> clean gate as evidence about the *toolkit*, not about the corpus.
>
> The baseline is also **stale-high**: it was seeded before the remediation phases ran, so it
> grandfathers well over a thousand findings that no longer exist. Nothing can be gated on
> those having stayed fixed until it is reseeded. Counts at the seed, and as measured after
> phase 9, for diffing against the first CI candidate:
>
> | Check | Seeded | Now |
> |---|---|---|
> | `vale_adoc` | 466 | 132 |
> | `vale_docstrings` | 785 | 427 |
> | `sentence_length` | 204 | 71 (hard 1, advisory 70) |
> | `doc_lint` | 93 | 3 (all D2, the documented carve-out) |
> | `mrdocs_warnings` | 460 | 460 |

### Corosio's posture: the gate is split in two

`selftest.mjs` is **blocking**. It has no baseline and no environment dependence, so a red
run there means a linter regressed.

The gate itself runs as two steps, and the split is about baseline trust rather than which
rules matter:

| Step | Checks | Posture |
|---|---|---|
| `Lint: gate (structural + C2, BLOCKING)` | `doc_lint` (A1/A6/B2/D2/ANCHOR), `sentence_length` (C2) | **`--strict`** |
| `Lint: gate (wording + reference, reporting)` | `vale_adoc`, `vale_docstrings`, `mrdocs_warnings` | reports, does not fail |

`doc_lint` and `sentence_length` are pure file parsing with Node built-ins: no external tool,
no version input, identical fingerprints in any environment. They are safe to gate against a
locally-seeded baseline, so they are strict now.

The other three are not. `vale_adoc`/`vale_docstrings` depend on the asciidoctor build Vale
shells out to, and `mrdocs_warnings` on the MrDocs develop build hash. Compared against a
local baseline, environment drift in those reads as a NEW violation and would fail the job for
a reason unrelated to the documentation. **They become strict once the first
`workflow_dispatch` reseed replaces the local baseline with a CI-authored one** — at which
point the change is moving their specs into the strict step.

The cost, stated plainly: until that reseed, a new wording or reference-surface violation is
reported but lands, and stays until a reseed grandfathers it.

## Corosio-specific configuration

Three values differ from Capy's, and each is a judgment rather than a rename.

**`doc-lint.mjs`'s `CONCEPT_DIRS` (D2)** is `3.tutorials`, `4.guide`, `5.testing`.
`2.networking-tutorial` is deliberately absent: it teaches IP, TCP and UDP theory from first
principles and introduces no Corosio type, so there is no type an example could show. It is
background material, the same carve-out Capy's `3a`–`3d` primer carries. Including it would
file roughly 13 findings that no example can fix, and the fix for a D2 finding is never a
decorative `include::example$`.

With that scope, D2 reports **3** findings, all `*.intro.adoc` chapter landing pages
(`3.intro`, `4.intro`, `5.intro`). They introduce no type, so they keep failing D2 for the
same documented reason Capy's landing pages do. That count is an intentional consequence of
D2's scope, not a backlog to chase to zero.

**`sentence-length.mjs`'s `ADVISORY_DIRS` (C2)** is
`modules/ROOT/pages/2.networking-tutorial/`. That chapter is essay-style prose teaching
protocol theory, exactly the material C2 relaxes for, and at the port it held **72 of the 143**
page-level hits — gating it would make the hard slice mostly essays. Docstrings are always
hard, whatever directory they came from. Measured split at the port: `hard: 132`
(71 pages + 61 docstrings), `advisory: 72`.

**There is no accessibility check.** E4 is Review tier, and Capy's `run-a11y.mjs` cannot fail
CI there (`continue-on-error: true`, in no gate spec). A check that cannot fail earns nothing,
so neither it nor `pa11y-ci` was ported. `baseline.mjs` and `baseline-diff.mjs` had their
`a11y` branches removed rather than left dangling.

## F4 bite-test log

Style-guide Part F4: **a check is not adopted until a planted violation has failed it.** A
green run is not evidence. Every check below was verified by planting a violation of that
exact rule and confirming the failure, at the port commit.

| Check | Planted violation | Result |
|---|---|---|
| A1 | `:page-mode: concept` — an invalid *value*, not a missing attribute | fires: `invalid :page-mode: value 'concept'` |
| A6 | measured against the real nav | fires: `quick-start at top-level position 8, must be <= 3` |
| A7 / `Corosio.PartHeadings` | `== Part 3: The Vacuous Rule` | fires. This is the rule that matched **zero** inputs in Capy until `b54fe6c8`, because Vale's heading scope strips the `==` markers; the fixed pattern anchors on heading text and was confirmed here rather than assumed |
| B2, bare listing | code in a bare `----` block | fires |
| B2, long delimiter | code in a `-----` (five-dash) block | fires — a fixed 4-character match would have missed it |
| B2, laundering | `[source,cpp]` + `[role=output]` over real code | fires: `role=output` does **not** exempt a `[source,*]` block, which is B3's load-bearing boundary |
| ANCHOR | `` `[[nodiscard]]` `` in prose | fires |
| C2 routing | a 30-word sentence in `4.guide/`, in `2.networking-tutorial/`, and in a docstring | routes correctly: `C2` hard, `advisory-C2`, `C2` hard |
| C4/C9/C10 | "The reactor will simply spawn the coroutine." | all four fire: `Corosio.SimpleTense`, `Google.Will`, `Corosio.NoFluff`, `Corosio.Terminology` |
| gate, `--strict` | A7 + C4 + C9 + C10 + B2 planted in a live page | **exit 1**, all 5 named as blocking, 2 non-gated new findings reported separately |
| gate, report-only | the same plant, no `--strict` | **exit 0** — confirms the current posture reports without failing, and that the phase-9 flip is the only change needed |
| fail-closed | `extract-docstrings.mjs` made to exit 3 | **exit 1**: `vale_docstrings` and `sentence_length` marked SKIPPED and, being gated, fail the gate. Zero findings did not read as success |
| tail-anchor trap | `--gate 'vale_adoc:^Corosio\.PartHeadings$'` against a planted A7 | **exit 0 while gating nothing** — the trap is real on this corpus. The correct tail-only spec exits 1 on the same input |
| strict gate, B2 | a bare `----` listing holding code, against the strict step's real spec | **exit 1** |
| strict gate, C2 | a 28-word sentence on a hard-slice page | **exit 1** |
| strict gate, clean | the same spec against an unmodified tree | exit 0, before and after both plants |
| MrDocs pin | the CI reseed itself, against a binary reporting `2026.9.5` | **caught by the safety net**: the pin rejected the only candidate, `mrdocs_warnings` reported SKIPPED, and `baseline-diff.mjs` refused the candidate rather than let it wipe a 460-fingerprint gated backlog. Re-tested after the fix: with `MRDOCS_ROOT` set, `MRDOCS_VERSION=9999.1.2` is ignored and the check reports its 460 warnings; with `MRDOCS_ROOT` unset the fallback pin still resolves; with `MRDOCS_ROOT` naming an unrunnable binary the check errors instead of silently reporting zero |
| reseed gate-spec extractor | run against the two-step gate | recovers exactly the 6 live specs. It first recovered **8** — the awk program contains the string it searches for, so it matched its own source line and captured the `grep`/`sed` lines below as specs, one of them the invalid regex `[^`. The pattern is anchored to `^ *- name:` for that reason, and the toggle is `inblock = 0` rather than `exit` so the second gate step is not silently dropped |

`selftest.mjs` automates the subset it can (37 assertions at the port) and is the standing
guard afterwards. It is not a substitute for the table above: it passed *before* several of
these were confirmed, which is exactly F4's point.
