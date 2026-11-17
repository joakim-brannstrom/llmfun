## Task 13: C++ view - header composition, folded role word, FAIL, disclosure

Priority: P0. Dependencies: Task 9, Task 12.

### Description
One width-aware header composer for both profiles: label -> duration -> chips, chips dropped one
token at a time from the right before the duration, the label never dropped; a folded owner drops
the role word; `FAIL` paints from `failedCalls`; the owner draws a disclosure mark.

### Changes Made
- `cpp_tui/views/blocks.h:143` (edit): the signature gains the metrics and the width.
- `cpp_tui/views/blocks.cpp:39-43` (edit): `separator(const ui::Metrics& m)` reads `m.textGrid`.
- `cpp_tui/views/blocks.cpp:189-239` (rewrite): `blockHeaderText(block, draw, m, availUnits)`.
- `cpp_tui/views/blocks.cpp:304-333` (`renderBlock`, edit): `blockHasDisclosure(block)` (`:308`),
  the header call with the row width (`:327`), FAIL from `failedCalls` (`:329-333`).
- `cpp_tui/test_blocks.cpp:264-266` + `:590` (edit), `cpp_tui/test_streaming.cpp:381-385` +
  `:709-721` (edit): the call sites pass `ui::metricsFor(textGrid, 1.0f, 1.0f)` and a width.

```cpp
// views/blocks.h
/// The row's header text, fitted to `availUnits` in the profile's own unit
/// (`ui::textUnits`). Order: label -> duration -> chips. A folded batch owner drops the
/// role word (FR-8). Chips drop one token at a time from the right before the duration
/// does; the label never drops.
std::string blockHeaderText(const BlockView& block, const BlockDraw& draw, const ui::Metrics& m,
                            float availUnits);
```

```
// The composer (design §7.4, DD-8, DD-18, FR-8, FR-13):
parts = []
if !block.ownsBatch or draw.expanded: parts += label()      # a folded owner has no role word
if block.timingS > 0:                 parts += timing(block.timingS)
if !block.chips.empty():              parts += chips(block.chips)

while textUnits(m, join(parts, sep(m))) > availUnits and chipsRendered > 1:
    drop the rightmost rendered chip; N += 1; rebuild the chips text ("+N" past the last one)
if still too wide and parts has a timing: drop the timing
return join(parts, sep(m))            # sep = " - " on the grid, " · " in the pixel profile
```

```
// Chips fitting, worked (grid, 40 cells):
//   "[bash] [grep] [read_file] +2"  -> too wide
//   "[bash] [grep] +3"              -> still too wide
//   "[bash] +4"                     -> fits            (never fewer than one chip)
// "Thinking" carries neither a count nor a duration (DD-8): label, then chips if any.
```

### Verification
- [ ] `make test_blocks test_streaming > build.log 2>&1` clean.
- [ ] Grid: `YOU - 1.2 s` and pixel `YOU · 1.2 s` (the separator pins stay).
- [ ] Owner, open: `AGENT - 3.2 s - [bash] [grep]`; owner, folded: `3.2 s - [bash] [grep]` (no
      role word, FR-8).
- [ ] 40 units, both profiles: `AGENT - 3.2 s` fits; `AGENT - 3.2 s - [bash] [grep] [read_file]`
      fits to `AGENT - 3.2 s - [bash] [grep] +1`; a 10-unit row drops the timing and keeps the
      label. Assert the exact strings with `ui::metricsFor(true/false, 1.0f, 1.0f)`.
- [ ] `Thinking` header is the bare word (no count, no duration); `blocks-*` scenes where a
      `Thinking` row carried a duration regolden (the duration is on the owner now).
- [ ] `FAIL`: an owner with `failedCalls > 0` paints `FAIL` after its fitted text in
      `stateDanger`; a card with `state == Failed` still paints it; no other state paints.
- [ ] An owner draws the disclosure mark (`v`/`>`), in `textLow` (focused: `accent`), exactly
      where a WorkGroup/Thinking row draws it.
- [ ] Regression: the overview's regression command passes.

### Notes
- The metrics argument is what keeps `-` on the grid and `·` in the GUI: do not reintroduce a
  `bool textGrid` parameter, and do not measure with `displayWidth` (that is a cell count; the
  pixel profile would disagree with the renderer).
- `measureBlockHeader` needs no width: a header is one row tall (a compact row's second line is
  not a header row).
- Ink per design C8: owner label `roleAgent`, chips `textMid`, `FAIL` `stateDanger`, the
  disclosure mark `textLow` / `accent` when focused.

Plan pointers:
- DD-8, DD-18, FR-8, FR-9, FR-13, FR-23, C8, C9a; overview open issues 1 and 3.
- Anchors were verified at plan time; re-verify before each edit.
