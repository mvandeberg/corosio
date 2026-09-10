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
| `extract-docstrings.mjs` | Extracts header docstrings into `.docstrings/*.adoc` so Vale can lint them. Doxygen targets (`@ref`/`@p`/`@c`/`@see`) are re-emitted as code spans, and `@par !example` directives are dropped — see below. |
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

**A local Vale run can under-report, so it is not sufficient evidence.** Local Vale
missed a live `Corosio.SimpleTense` finding on `benchmark-report.adoc` that CI caught
on the same commit — a long single-line paragraph that the Ruby asciidoctor in CI
extracts as prose and the local JS build does not. Local reported 131 page findings
against CI's 66 and still missed that one. For the gated rules, a raw `grep` over the
sources is a useful independent check precisely because it has no extraction step:
`grep -rn '\bwill\b|\bhas been\b|\bhave been\b'` found eight sites the rule could not
see at all.

**A Vale gate spec must never carry a leading `^`.** The check name is at the tail, so
`^Corosio\.PartHeadings$` matches nothing and the comparator then reports
`gated: true, gatedNew: 0` at **exit 0** — a gate that announces it is gating while checking
nothing. Measured on this corpus; see the bite-test log below.

**Never hand-edit `baseline.json`.** Reseed via the `workflow_dispatch` steps in the
workflow, never locally: a local run differs from a CI run and would grandfather hundreds of
local-vs-CI drift fingerprints.

> **A false positive that came and went.** An earlier baseline carried
> `benchmark-report.adoc:#1:Google.OxfordComma` on the sentence "…comparable to its
> unidirectional throughput, suggesting serialization between the read and write
> paths." That is not a list needing an Oxford comma — "the read and write paths" is
> a compound noun phrase and the comma opens a participial clause, which slips past the
> rule's own guard against clause-introducers because "suggesting" is not in its
> exemption list. Grandfathering a rule false positive is what the baseline is for;
> rewriting sound prose to appease a heuristic would be worse.
>
> It surfaced only after the vocabulary additions removed a `Vale.Spelling` alert that had
> been masking it at the same position, vanished in the 2026-09-09T18:40Z reseed, and came
> **back** in the 2026-09-09T20:27Z one — all three times with that page untouched. It is
> the position-resolution artifact the `.vale.ini` comment describes, and it flaps. Treat any
> future appearance the same way: it is a false positive on a participial clause, it
> grandfathers, and the prose is left alone.
>
> `baseline.json` is **CI-authored** (`workflow_dispatch`, 2026-09-09T18:38Z) and is the
> reference point the strict gate compares against. Counts at the original local seed and in
> the accepted reseed:
>
> | Check | Local seed | CI baseline |
> |---|---|---|
> | `vale_adoc` | 466 | 66 |
> | `vale_docstrings` | 785 | 55 |
> | `sentence_length` | 204 | 71 (hard 1, advisory 70) |
> | `doc_lint` | 93 | 3 (all D2, the documented carve-out) |
> | `mrdocs_warnings` | 460 | 44 |
>
> Four reseeds were needed. The first was refused because the MrDocs version pin made
> `mrdocs_warnings` report SKIPPED, which would have wiped a 460-fingerprint gated backlog.
> The second was refused for a **real** gated regression a local Vale run could not see. The
> third retired 365 and grandfathered the one false positive above. The fourth, after the
> rebase onto develop, absorbed the `io_uring`->`uring` rename churn and retired the 19 B4
> parameter mismatches; its one gated addition was the same rename churn
> (`io_uring_t::construct` -> `uring_t::construct`) and was fixed rather than grandfathered,
> so that baseline was briefly stale-high by 4 in `mrdocs_warnings`. A fifth reseed retired
> 71 more (the parameter and return-value documentation pass) and grandfathered none, and is
> what is installed now. `doc_lint` and
> `sentence_length` measured **identically** in both environments (3 and 71), which is what
> makes them safe to gate; every other difference above is environment drift.

### Corosio's posture: everything blocks except the reference surface

`selftest.mjs` is **blocking**. It has no baseline and no environment dependence, so a red
run there means a linter regressed.

The gate runs as two steps:

| Step | Checks | Posture |
|---|---|---|
| `Lint: gate (BLOCKING)` | `doc_lint` (A1/A6/B2/D2/ANCHOR), `sentence_length` (C2), and the C4/C9/C10/A7 wording rules on both corpora | **`--strict`** |
| `Lint: gate (reference surface, reporting)` | `mrdocs_warnings` | reports, does not fail |

`baseline.json` is now authored by the Documentation job itself, so a strict comparison is
CI-against-CI and carries no environment drift. The wording rules additionally have an
**empty gated subset** — zero baselined C4/C9/C10/A7 fingerprints on either corpus — so
any match at all is a real regression.

`mrdocs_warnings` stays reporting on purpose. Its `.*` spec gates all 352 warnings, and
MrDocs is a rolling `develop-release` build whose output demonstrably moves: the same asset
reported **460** warnings under `0.8.0` and **352** under `2026.9.5`, days apart, on an
unchanged tree. Gating `.*` against a tool that rewrites its own output would fail the job
for upstream reasons unrelated to Corosio's documentation — the same mistake as the version
pin that skipped this check on the first reseed. Promote it only alongside a pinned MrDocs.

> **A local `--strict` run of the full gate will fail, and that is expected.** The baseline
> is CI-authored; a developer machine produces different `vale_*` and `mrdocs_warnings`
> fingerprints (measured: `vale_adoc` 132 locally against 66 in CI, from the Ruby-vs-JS
> asciidoctor Vale shells out to; `mrdocs_warnings` 460 against 352). None of that drift
> touches a gated rule, so the **gated** slice does pass locally:
>
> ```sh
> node lint/check-no-new-violations.mjs --strict \
>   --gate 'doc_lint:^(A1|A6|B2|D2|ANCHOR):' --gate 'sentence_length:^C2:' \
>   --gate 'vale_adoc:Corosio\.PartHeadings$' \
>   --gate 'vale_adoc:(Corosio\.SimpleTense|Corosio\.NoFluff|Corosio\.Terminology)$' \
>   --gate 'vale_docstrings:(Corosio\.SimpleTense|Corosio\.NoFluff|Corosio\.Terminology)$'
> ```

## Linting Doxygen prose

Vale is a plain-text speller; Doxygen prose is not plain text. Three extractor
behaviours exist because of that, and each one removed a class of finding that could
never have been fixed in a header:

* **`@ref X`, `@p X`, `@c X` re-emit as `` `X` ``, not as bare `X`.** All three render
  as a link or as monospace in the real reference, so bare text was a lie about the
  source *and* a guaranteed `Vale.Spelling` hit. Backticking them in the header
  instead would have broken the link or the parameter binding.
* **`@see A, B, C` backticks each identifier-shaped item.** Doxygen auto-links a
  `@see` list; `@see epoll_t, select_t, kqueue_t, iocp_t` alone accounted for 21
  findings.
* **`@par !example <id>` is dropped.** The id names a compiled source under
  `test/doc/reference` for the reference-snippets extension. It is a machine
  directive and never reaches a reader, so ids like `connect_and_read` were
  permanent unfixable findings.

Together with backticking 93 genuinely bare identifiers in the published headers,
this took the docstring corpus from **420** findings to **62**, and `Vale.Spelling`
from **389** to **24**. Trailing punctuation stays outside the span: `@ref io_stream,`
becomes `` `io_stream` ``, not `` `io_stream,` ``.

`detail/` headers are deliberately untouched by the B1 pass. `extract-docstrings.mjs`
excludes them (and strips `namespace detail` blocks) because `mrdocs.yml` marks them
implementation-defined, so no rule governs their prose and B1 is a reference rule.

**Residual, 24 findings and non-gated.** Eight are `backend's`: C.1 lists bare
"backend" as an Avoid term in favour of **I/O backend**, so that one is a real
terminology item rather than noise, and silencing it in the vocabulary would hide the
signal. The rest is a thin tail of single occurrences (`await_suspend`, `key_type`,
`worker_base`, a few `native_*` names) sitting in `@param`/`@return` bodies.

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

## The 8 `unsupported HTML tag <tt>` warnings are not ours

`mrdocs_warnings` carries eight `unsupported HTML tag <tt>` findings with **no file
attribution** (`file: null`, fingerprinted `?:#N:`). They are **not fixable in Corosio**,
and the trail is worth recording because it is not obvious:

* `grep -rn '<tt>' include/ doc/ test/` returns **zero**. Capy returns zero too.
* Boost.Asio's headers are full of `<tt>` (381 occurrences), which makes it the obvious
  suspect — and it is wrong. `capy/buffers.hpp` only forward-declares
  `namespace asio`; a preprocessor run (`clang++ -H`) over every public Corosio header
  confirms `boost/asio/buffer.hpp` is **never reached**.
* Preprocessing all 59 public headers yields 547 reachable files. Exactly one contains
  `<tt>`: **libstdc++'s `bits/alloc_traits.h`**, which carries 10 of them in its own
  Doxygen comments (`<tt> pointer_traits<pointer>::rebind<const value_type> </tt>`).

MrDocs emits the warnings while extracting declarations, with no location, because they
come from the standard library implementation it parses. Nothing in this repository can
change them. They are also **environment-dependent**: a different libstdc++ version, or
libc++, produces a different count, which is part of why local and CI `mrdocs_warnings`
totals differ and why the `?:#N:` fingerprints reindex on any change.

`mrdocs.yml` has `use-system-libc` and `use-system-stdlib` commented out. Turning them on
would change which standard library MrDocs parses and might retire these eight, but it
would also change the whole reference build; they are off deliberately and this is not a
reason to flip them.

Treat these the way Part E4 treats generator and theme output: not a defect in authored
content. **Do not spend time on them again.**

**Update, and it settles the point:** the 2026-09-09T17:25 reseed came back with **zero**
`<tt>` findings, where the reseed three hours earlier had eight. Nothing in this repository
changed between them. A rolling MrDocs build or a runner image with a different libstdc++ is
enough to make all eight appear or vanish, which is exactly the environment-dependence
described above. If they reappear, they are still not ours.

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
| strict gate, C4/C9/C10 on pages | "The acceptor will simply spawn a coroutine." | **exit 1**, naming `Corosio.SimpleTense`, `Corosio.NoFluff` and `Corosio.Terminology` |
| strict gate, C4/C9/C10 on docstrings | the same sentence spliced into `tcp_socket`'s brief | **exit 1**, naming all three on `vale_docstrings` |
| strict gate, A7 | a `== Part 9:` heading | **exit 1**, naming `Corosio.PartHeadings` |
| strict gate, B2 | a bare `----` listing holding code, against the strict step's real spec | **exit 1** |
| strict gate, C2 | a 28-word sentence on a hard-slice page | **exit 1** |
| strict gate, clean | the same spec against an unmodified tree | exit 0, before and after both plants |
| MrDocs pin | the CI reseed itself, against a binary reporting `2026.9.5` | **caught by the safety net**: the pin rejected the only candidate, `mrdocs_warnings` reported SKIPPED, and `baseline-diff.mjs` refused the candidate rather than let it wipe a 460-fingerprint gated backlog. Re-tested after the fix: with `MRDOCS_ROOT` set, `MRDOCS_VERSION=9999.1.2` is ignored and the check reports its 460 warnings; with `MRDOCS_ROOT` unset the fallback pin still resolves; with `MRDOCS_ROOT` naming an unrunnable binary the check errors instead of silently reporting zero |
| reseed gate-spec extractor | run against the two-step gate | recovers exactly the 6 live specs. It first recovered **8** — the awk program contains the string it searches for, so it matched its own source line and captured the `grep`/`sed` lines below as specs, one of them the invalid regex `[^`. The pattern is anchored to `^ *- name:` for that reason, and the toggle is `inblock = 0` rather than `exit` so the second gate step is not silently dropped |

`selftest.mjs` automates the subset it can (37 assertions at the port) and is the standing
guard afterwards. It is not a substitute for the table above: it passed *before* several of
these were confirmed, which is exactly F4's point.
