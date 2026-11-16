## Task 8: Doc sweep (P3, docs-only)

Priority: P3. Dependencies: task 7. Docs-only: no code, no goldens.

### Description
Bring the docs in line with the shipped v6: the C API boundary, the
composer notes, and any line still describing a v5 shape this design
replaced (design §10 task 8).

### Changes Made
- `llmfun/doc/tui_design.md` (C API boundary section): ordinal gone from
  TuiTurn; the RESULT pair transport on the tool block (sorted parallel
  vectors + resultMore + resultRaw fallback, the caps, one-time
  emission); the busy edge real (uiAgentBusy clears the ready flag;
  entry-point brackets; at-most-one-run assumption).
- `llmfun/doc/todo_modern_ui.md` composer notes: the button flips via
  the real edge from every entry point.
- Sweep for stale v5 lines across `llmfun/doc/` (todo.md, tui_design.md,
  todo_modern_ui.md): the "[N]" ordinals, the empty busy half, the v5
  maximal default (everything open except thinking), the tool card's
  status glyph / token header / "name(" body prefix, the dim Pending
  chip. Update ONLY what v6 changed — the v5 deferral notes that stay
  delivered are not reopened (design §10 Notes).
- Goldens dir README/comments (if any) match the v6 shapes.

### Verification
- [ ] No doc reference to: the "[N]" ordinals, the empty busy half,
      the v5 maximal default, the tool card glyph / token fields /
      name-prefix body, or the Pending chip (grep `doc/` + the
      goldens README; every remaining hit is about the NEW shapes
      or an unrelated feature).
- [ ] tui_design.md's C API section matches the shipped ABI (version
      14 semantics: pairs added, ordinal gone) — the docs say what
      the wire IS, not what the design proposed.
- [ ] No code, no golden, no test changed (git show --stat: docs only).
- [ ] Full suite still green (untouched, but run it once).

### Notes
- The v5 deferral notes (todo_modern_ui.md thread section, todo.md
  live-tool/stats lines) stay delivered; this task only updates
  what v6 changed (design §10 Notes).
- Open questions 6.1 (status glyph) and 6.2 (ordinal feature) are
  CLOSED by the shipped D4/D5 — if the docs carried them as open,
  close them here; 6.4 stays a recorded caveat.

Plan pointers (task-specific only — standing rules live in the overview):
- Decision letters used: none (docs-only).
