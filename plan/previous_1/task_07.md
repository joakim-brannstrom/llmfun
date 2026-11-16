## Task 7: Maximal mode (design P1-6)

Priority: P1. Dependencies: task 5; shares the mode source with task 6. One commit.

### Description
Implement the maximal thread rendering: work groups open as batches with the
full anatomy — thinking disclosure, content block, tool cards with durations,
arguments, embedded Result, token fields — plus the failed-red and
Cancelled-honest states.

### Changes Made
- llmfun/cpp_tui/views/thread.cpp (the same group/step drawing task 6 owns):
  the maximal projection per design §6:
  - Work groups render OPEN as batches, in batch order within the turn:
    1. Thinking disclosure — COLLAPSED by default, meta `N tool calls`
       (the batch's Tool block count; tool-less batches show no count).
    2. Content block (body rendered).
    3. Tool cards, one per Tool block: header `TOOL <name>` while streaming
       (durationMs unknown, W7), `TOOL <name> · <duration>` once the result
       lands; arguments as the body; embedded Result OPEN by default
       (status Ok/Failed/Cancelled shown honestly); token fields render
       ONLY when non-zero (tokensIn/tokensOut).
  - Cards indent under the batch's thinking behind a shared left rule.
  - Failed red INSIDE expanded groups (a collapsed group never shows red).
  - Cancelled renders as its own honest state (neither Ok-green nor
    Failed-red); no fabricated Ok anywhere (stop mid-batch, §8).
  - Status/source constants: statuses come straight from the store's
    blocks (emitter-driven, task 3); the view never derives status.
- Expansion state: maximal defaults everything open; per-group overrides
  (collapsing pins closed) are task 8 — task 7 ships the defaults only.
- The mode branch comes from the same module constant task 6 introduced;
  task 8 replaces it with ShellPrefs.

### Verification
- [ ] Golden frames (§8; the task-6 golden pattern):
      - batch anatomy (thinking disclosure collapsed + content + two cards
        with durations and open Results)
      - a failed step inside an expanded group (red)
      - a live card (`TOOL name`, no duration) vs a landed card
        (`TOOL name · <duration>`)
      - zero/absent token fields render nothing
      - a Cancelled card after a stop renders honestly
- [ ] Expansion toggles work per disclosure/card without touching the store
      (view-local state as today).
- [ ] `dub test` + cpp_tui binaries green (known pre-existing reds excluded).

### Notes
- W7 pin: the duration appears exactly when durationMs != 0; the header
  never shows a placeholder duration.
- The thinking disclosure's collapsed default is per batch; the group-level
  expansion (minimal's header-only vs maximal's open) is the MODE, not the
  disclosure state — the two are independent (a minimal-opened group =
  task 8's override renders maximal-style).
- Decision letters used: D4, D5 (overview).
