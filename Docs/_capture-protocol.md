# Capture protocol

When a durable engineering decision, design principle, or architectural fact is settled in this session, record it before moving on. This file is injected into context every session; it is the write-path of the project doc system (structure: `Docs/doc-system.md`).

## Where it goes — nearest unit wins
Record into the `Docs/` of the unit the work belongs to (the unit whose files you are editing). Root unit = `./Docs/`. A plugin = that plugin's own `Docs/`.

## What goes where
- **Decision** — a point-in-time choice with alternatives + rationale → a new immutable ADR `<unit>/Docs/decisions/adr-NNNN-<slug>.md` (copy `Docs/_templates/adr-template.md`). Never edit an accepted ADR; supersede it with a new one. Add it to that unit's `Docs/INDEX.md`.
- **Principle / philosophy** — a durable, evolving rule → append a section to `<unit>/Docs/PHILOSOPHY.md`.
- **Current-state fact** — how the code now works → update the relevant `<unit>/Docs/arch-*.md` or reference doc.
- **New design / plan** → `<unit>/Docs/design-*.md` (or `wip-*.md` if not yet decided).

## Hard rules
- **Never edit any `CLAUDE.md` unilaterally.** When a CLAUDE.md change is warranted, either (a) propose the exact line + location, get the owner's explicit per-edit approval, then edit it directly, or (b) append a one-line proposal to `Docs/_pending-claude-md.md` for later human review. (Amended 2026-06-26 — direct edit after approval is now allowed; the queue is no longer mandatory.)
- ADRs are immutable, numbered, never reused — supersede, don't rewrite.
- If you create a new doc, link it from that unit's `Docs/INDEX.md`.
- Subagents/workflows: the unit you operated on still owns the record — apply the same routing.
- Don't capture what the code or git history already records. Capture the non-obvious *why*.
- **A repo doc must never cite an assistant-memory file as its sole reference** — session memory is invisible to every other reader of the repository. Capture the content into the unit's `Docs/` and cite that; a memory name may appear only as a supplementary pointer. (Added 2026-07-03 — the coherence audit found repo docs depending on out-of-repo memory.)
- **Dispositions need homes, not only decisions.** A DEFERRAL gets a **`Docs/backlog.md` row** (the single ledger — see "Backlog / TODO ledger" below; the roadmap stays the strategic why/when view); a RETIREMENT (of a term or mechanism) gets a one-line banner in the affected living doc; a standing PERMISSION gets an ADR carrying its expiry trigger. These otherwise leak into session memory as their only record. (Added 2026-07-03; deferral-home → backlog 2026-07-10.)

## Backlog / TODO ledger

Each unit's `Docs/backlog.md` is the ONE list of that unit's OPEN future work (create it only when the unit has ≥1 open item). Maintain it as a co-output of work, never as separate bookkeeping:

- **Capture** — a backlog row is a mandatory co-output of recording any DEFERRAL (above): the same moment you defer, add a row (`idea`, or `designed` if a design already exists). Also capture when the owner explicitly voices a future intent ("remember this / someday / later") and read that row back. Do NOT capture idle musings.
- **Link on design** — when a concrete design lands, flip the row to `designed` and link its `design-*`/`wip-*`/ADR.
- **Close on ship** — flip to `done` (or `dropped` + a one-line reason) IN THE SAME commit that records the as-built; the row then leaves Active (its ADR/commit is the permanent record — never grow an unbounded done ledger).

Forward TODO lists elsewhere (the roadmap's open rows, a design doc's deferred list) point to the backlog instead of re-listing; immutable ADR "deferred" clauses stay as point-in-time history. Row shape: `Item (what + why) | State | Design`. States: `idea → designed → done`, terminal alternate `dropped`. No ids — the link is backlog → design, one-way.
