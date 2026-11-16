## Task 2: ABI additive plumbing — RESULT pair transport on the tool block (v6)

Priority: P0. Dependencies: none. Additive; both sides build; nothing consumes
the pairs yet; existing goldens pass unchanged (design §9 group 2, part 1).

### Description
Append the RESULT pair transport to the layout-pinned tool block on the flat C
wire (design §4), bump the ABI version, and re-pin the layout on both sides.
Fields are plumbed and stored but unwired: D populates nothing yet, the renderer
reads nothing yet (task 3 populates, task 5 renders).

### Changes Made
- `llmfun/cpp_tui/tui_api.h`:
  - :29 — `#define TUI_API_VERSION 12` -> `13` (PL-2; the design's "v6" is a
    design-generation label, the macro is the integer — recorded conflict).
  - :667-685 TuiBlock — append the verbatim fields below after `reserved`,
    in this order (PL-1). Keep the existing comment style semantic, no
    byte-count layout comments.
- `llmfun/cpp_tui/test_tui_api_v4.cpp` (caseThreadTurnBlockFields 1147-1203,
  the offset-pin pattern): update the version pins (1029, 1159, 1657) to 13;
  pin the new sizeof + member offsets for the 64-bit ABI (the struct grows
  past 128 bytes; pin whatever the ABI produces, both sides same numbers);
  extend the vocabulary pins (1190-1203) where they touch the new fields.
- `llmfun/cpp_tui/test_clear_output_state.cpp:141` — the other
  `TUI_API_VERSION == 12` pin -> 13.
- `llmfun/source/llm/tui/package.d`:
  - extend the D wire mirror + UiBlockData (:1242) with the same fields;
  - :46,53 pattern — extend toBlockData/toTuiBlock (:60-64, :68-83) to copy
    the new fields mechanically (null/empty until task 3 populates them);
  - static asserts — extend to the new layout (they are the D-side proof).
- `llmfun/source/llm/tui/package.d:1260-1268` — UiBlockUpsert/UiSetTurns
  payloads carry the new fields (mechanical; still empty).
- `llmfun/cpp_tui/tui_api.cpp` — the store-copy paths (tuiSetTurns/tuiBlockUpsert
  copies, tokens region :851-912): copy the new fields into the C++ store
  (mechanical; null/empty until emission exists).
- `llmfun/cpp_tui/store/typed_thread_store.h:27-38` — TypedBlock gains the
  four-field pair set (resultKeys/resultValues/resultCount/resultMore +
  resultRaw); blockUpsert (:94-95) merges it ATOMICALLY — an upsert carrying
  a pair set replaces all result fields together, never field-by-field
  (design §4 decision; store test below pins it).
- Store unit test (new, in `llmfun/cpp_tui/test_typed_thread_store.cpp`,
  pattern of the wire-shape tests :531-597): feed the store a tool block
  with a pair set -> stored verbatim; upsert again with a different pair
  set -> all four fields replaced together; upsert without a pair set ->
  existing pair set untouched (merge, not clear).

### Verbatim contract — TuiBlock delta (C, appended after `int32_t reserved;`)
```c
/* RESULT pair transport (tool kind only). Parallel vectors, same length,
 * sorted by key byte-wise ascending; duplicate keys collapse last-wins;
 * values pre-truncated at 200 codepoints (UTF-8-safe) by the sender;
 * at most 100 pairs (resultMore = pairs past the cap, 0 when under).
 * resultRaw: packed fallback when the result is not an unpackable JSON
 * object (array, non-JSON, zero-pair object) — pairs stay empty and
 * resultRaw carries the one-line string rendered as-is. Strings are
 * inbound/non-owning, copied by the UI like every other TuiBlock string. */
const String* resultKeys;
const String* resultValues;
String        resultRaw;
uint32_t      resultCount;
uint32_t      resultMore;
uint32_t      resultReserved;
```
- Caps are RESULT-only: the card body's arguments keep the existing D-side
  summarizer and its 1000-char per-value cap (W5/PL-11).
- The D-side unpacker computes the pair set ONCE at emission, never per
  render (N4); C++ renders strings verbatim, never re-parses (D4 rule).

### Verification
- [ ] Version pins all bumped (grep `TUI_API_VERSION` — every `== 12` pin
      updated; the versions-sheet string at `llmfun/cpp_tui/views/shell.cpp:705`
      renders the macro, no edit).
- [ ] Contract test passes: new sizeof + offsets pinned both sides;
      D static asserts compile (the proof the layouts agree).
- [ ] `cmake --build cpp_tui/build` green; `dub build` green.
- [ ] Store unit test green (store + atomic merge, fed directly).
- [ ] Existing goldens pass UNCHANGED (additive commit — nothing renders
      the pairs yet). Any golden delta here = a bug in this task.
- [ ] `dub test` + cpp_tui test binaries green (known pre-existing reds
      excluded — overview).

### Notes
- PL-1: representation = two parallel `const String*` arrays + u32 count +
  u32 more + String resultRaw, appended after `reserved` in this order —
  the flat-C analogue of the tuiSetTurns array pattern; pointers inbound,
  C copies them during the call like every other inbound string.
- PL-2: bump is monotonic 12 -> 13 here, 13 -> 14 at task 4's cutover; the
  design's C1 text ("bump to v6"/"to v7") labels design generations, not
  macro values. Recorded as a design-vs-code conflict for user review.
- resultReserved keeps the tail u32-aligned; the offset test pins the
  padded sizeof.
- Plan-time anchor facts to re-verify (they move): tui_api.h 29/655-694,
  test pins 1029/1159/1657/141, package.d 46/53/60-83/1236/1242/1260-1268,
  tui_api.cpp 851-912, typed_thread_store.h 27-38/94-95.

Plan pointers (task-specific only — standing rules live in the overview):
- Decision letters used: PL-1, PL-2, PL-11 (defined in the overview).
- Task 3 owns the emission contract (what populates these fields);
  task 5 owns the render contract (what draws them).
