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
- **Never edit any `CLAUDE.md` directly.** If a CLAUDE.md change seems warranted, append a one-line proposal to `Docs/_pending-claude-md.md` for human review.
- ADRs are immutable, numbered, never reused — supersede, don't rewrite.
- If you create a new doc, link it from that unit's `Docs/INDEX.md`.
- Subagents/workflows: the unit you operated on still owns the record — apply the same routing.
- Don't capture what the code or git history already records. Capture the non-obvious *why*.
