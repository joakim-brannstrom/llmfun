## Task 5: Minimal-mode live edge + tool card shape (D2 + D4 render)

Priority: P1. Dependencies: task 4. Additive rendering; the live-edge mechanics
land mode-generally here, maximal's fold DEFAULTS land at task 6 (design §9 group 4).

### Description
The live turn must always show what the agent is doing (F2), and the tool card
must stop being a jumble of markers (F4/D4). Land the live-edge element
selection (chips / thinking row / nothing), the live step count, and the
rebuilt card (disclosure arrow, no glyph, FAIL red, no tokens, args one pair
per line, dim RESULT pair lines + "... K more" tail). No Pending chip —
v5's dim Pending chip dies (a stated change, W4a).

### Changes Made — live edge (both modes' mechanics; maximal defaults = task 6)
- Live-edge element selection, drawn at the group's live edge (minimal:
  directly under the header, where no other rows render; maximal: at the
  open group's bottom — design §6):
  - while at least one call is Running: ONE chips line — one chip per
    running call, tool NAME only, orange, not clickable; folds the moment
    the call goes dormant (result lands, mid-batch included);
  - while the model streams thinking: a folded, count-only Thinking row
    instead — "Thinking" bare when the response carries no calls yet,
    "Thinking - N tool calls" once calls exist (I3; the streaming
    response's pending calls count, per the v5 store state);
  - while the model streams prose with no tool running: nothing extra;
  - Pending calls draw NO chip (W4a — the v5 dim Pending chip dies);
  - at most one live element (I2); the group's step count updates live
    while tools run; when the turn finishes the live element folds away
    and the group keeps its count (the task_done chip rule is unchanged).
- `llmfun/cpp_tui/views/thread.cpp` (:455 region — the group render) + the
  adapter's live-edge query: implement the selection over the store state
  (no new D-side events — C4/C5; D emits everything mode-independently).

### Changes Made — tool card shape (D4)
- `llmfun/cpp_tui/views/blocks.cpp`:
  - blockHasDisclosure (:172-174): add ToolStep (the card is a foldable
    like the Thinking rows; same disclosure shape, same click behavior).
  - blockStateGlyph (:152-166): the status glyph DIES from the header —
    success stays silent; a failure renders textual FAIL (red) on the
    header, folded or open. (The icons.cpp ascii fallback was the "+";
    the glyph's removal needs no icon edit.)
  - blockHeaderText (:197-255): the token header counts DIE (:217-226);
    name (:211-214) and timing (:215-216) stay.
- `llmfun/cpp_tui/store/thread_store.cpp`:
  - tokens copy (:284-285, :569-570): the renderer stops consuming them
    (the fields stay on the wire, I4; `llmfun/cpp_tui/model/block.h:129-130`
    render-side consumption dies).
  - renderBlockBody RESULT row (:741-749, today `"RESULT  " +
    oneLine(block->typedMeta)`): replace with the pair render — one dim
    `RESULT <key> = <value>` line per pair (values verbatim as sent),
    nested values arrive as compact JSON text, the resultRaw fallback
    line for array/non-JSON/zero-pair, `RESULT {}` for a zero-pair
    object, and one dim "... K more" tail when resultMore > 0. The body
    is rendered from the store's strings — no re-parsing (N4/D4).
- D-side body payload (PL-7): `llmfun/source/llm/app_agent/ui.d` —
  summarizedCallBody (:652-666) and the streaming call body (:763) drop
  the "name(" ... ")" wrapper — the body becomes arguments only, one
  `key=value` pair per line, no repeated name prefix, no "[N]". The
  summarizer + its 1000-char per-value cap are UNCHANGED (W5/PL-11).
  The C++ body render stays verbatim.
- Cancelled card: header-only, no marker, no FAIL, no RESULT row
  (PL-9/W7 recorded decision; N3 — golden-visible via the interrupted-turn
  replay frame).

### Verbatim contract — the live edge + card (task-5-owned; task 6 restates)
- One live element max (I2), selection precedence: running tools ->
  chips line; else streaming thinking -> folded count-only Thinking row;
  else nothing. Fold on dormancy, incl. mid-batch. Finished tools keep
  today's colors; live tools are orange.
- Card header: disclosure arrow + "TOOL <name>" while streaming /
  "TOOL <name> - <duration>" once the result lands; no glyph; FAIL
  (red) only on failure; no token fields.
- Card open body: arguments only, one `key=value` pair per line, then
  the RESULT lines (dim, one per pair) + "... K more" when more > 0.
- Click behavior identical to the Thinking rows (F3).

### Verification
- [x] Golden frames (regolden via --update + eyeball): minimal idle; a
      live turn with mixed states (chips + live count + streamed-thinking
      row); post-completion fold; card shapes — success folded/open,
      failure folded/open (FAIL red), Cancelled header-only; every
      RESULT shape from task 3's tests rendered incl. caps (the
      "... K more" tail and the 200-char cut visible in a frame).
- [x] No Pending chip renders anywhere (grep + the live golden).
- [x] No token header field renders (the wire fields stay — grep
      confirms only the render consumption died).
- [x] No "name(" body prefix renders (D-side unittest pins the body
      text: args only, one pair per line, both completed and streaming).
- [x] Click behavior: the card opens/folds exactly like a Thinking row
      (view test).
- [x] The live edge draws in BOTH modes with these defaults (W4c —
      the mechanics are mode-general; task 6 flips maximal's defaults).
- [x] `dub test` green; cpp_tui suite green (incl. xvfb GUI binaries
      for the new goldens).

### Notes
- PL-7 is the subtle one: today's body text is built D-side with the
  "name(" wrapper baked in — the C++ cannot un-prefix it; the fix is a
  payload change. Verify the streaming path too (:763), not just the
  completed path.
- The group's own disclosure state is unchanged (v6 moves the thinking
  and card defaults, not the group's — design §6).
- Plan-time anchors to re-verify (they move): blocks.cpp 152-174/197-255,
  thread_store.cpp 284-285/569-570/697-751, ui.d 652-666/763,
  thread.cpp 455, block.h 129-130.

- The streaming-live golden's folded Thinking row (the scene streams prose and
  thinking together): design §6's live-edge bullets order the precedence — the
  thinking row while the model streams thinking first, "nothing while the model
  streams prose with no tool running" after it. A scene that streams both draws
  the row per the earlier bullet; the prose-only scene (no reasoning upserted)
  has no Thinking-kind row to select and draws nothing, which is the later
  bullet's case. The store cannot tell which block is streaming, and the
  streamed prose block's streaming flag could suppress the row in the
  both-stream scene — but that reorders the design's own bullet precedence,
  so the overlap is a design-wording pass, not a projection fix; the golden
  shape stands as the bullets read.

Plan pointers (task-specific only — standing rules live in the overview):
- Decision letters used: PL-7, PL-9, PL-11 (defined in the overview).
- Task 6 owns the maximal fold DEFAULTS and the per-batch Thinking rows.
