## Task 3: D-side RESULT pair emission — unpack, sort, cap, wire both walks (D4)

Priority: P0. Dependencies: task 2. Additive; goldens unchanged except the
console-parity golden must stay byte-identical (F9) and the TUI goldens still
pass because nothing renders the pairs yet (design §9 group 2, part 2).

### Description
Compute the RESULT pair set on the D side at block emission — once per block,
live and replay alike (N4) — and wire it into the live step-completion walk
and the replay walk. The unpacker is the single owner of the caps (I5/PL-11).

### Changes Made
- Unpacker (new, `llmfun/source/llm/utility.d` next to the summarizers
  :126/:137/:172, or a small new module if utility.d is the wrong home —
  pick and record): a JSON-object tool result -> the sorted, capped pair set
  + fallback, per the verbatim contract below. Pure function; no UI imports.
- Live walk: `llmfun/source/llm/app_agent/package.d:1064,1079-1083,1110` —
  where the tool block's meta is built today (summarizeToolResponse lines),
  also compute the pair set and put it on the emitted block. The existing
  meta/body summarizers and their 1000-char args cap are UNCHANGED (W5).
- Replay walk: `llmfun/source/llm/app_agent/package.d:110-111,134,153-157` —
  the replay path computes the SAME pair set from the stored payload
  (deterministic; replay parity is pinned, not assumed).
- Cancelled tool blocks carry NO pair set and NO resultRaw — there is no
  result (design §5; N3 honesty).
- Unknown-callId results (live): unchanged v5 behavior — recorded under its
  callId, merged when the call lands; the pair set rides the same upsert (§7).
- Console formatter: passes through UNCHANGED (F9) — it renders today's
  dim RESULT line from the payload text as before; v6 adds no console line.
- Recording-sink tests (`llmfun/source/llm/app_agent/tests.d`, patterns at
  :1947+ user-query turn unittest): every result shape + the caps + replay
  parity, per the checklist.

### Verbatim contract — the unpacker (task-3-owned; task 5 restates its render)
- Input: the tool result payload as it exists today (what the meta's
  summarizeToolResponse sees).
- JSON object -> walk ONE level; each pair: key + stringified value
  (nested objects/arrays render as COMPACT JSON text; scalars as text).
- Sort pairs by key BYTE-WISE ascending (deterministic across hosts).
- Duplicate keys after parse: last-wins.
- Value cap: 200 CHARACTERS = codepoints, never splitting a UTF-8
  sequence (W5) — a value at exactly 200 codepoints truncates nothing.
- Pair cap: 100 — pair 101+ contributes to resultMore only.
- resultMore = total pairs - 100 when over, else 0.
- Non-JSON, JSON array, or zero-pair object -> pairs stay empty,
  resultRaw = the ONE-LINE string rendered as-is (today's dim RESULT
  text — the same string the console formatter uses, F9).
- Cancelled block -> no pairs, no resultRaw (caller's rule; the unpacker
  is never called for one).
- The pair set is computed once at emission (N4); the caps live here,
  not in the store or the view (I5/PL-11).

### Verification
- [ ] Recording-sink tests, every shape: JSON object (flat pairs), nested
      object (compact-JSON value), nested array value, array result
      (fallback), non-JSON (fallback), zero-pair object (`RESULT {}`
      shape — empty pairs + fallback), duplicate keys (last-wins),
      byte-wise sort order (pin a case where byte order != alpha order,
      e.g. `Z` < `a`).
- [ ] Cap boundaries: exactly 100 pairs -> resultMore = 0; 101 -> 1;
      value at exactly 200 codepoints truncates nothing; a multi-byte
      UTF-8 character at the cut stays valid (the receiver's strings
      are never invalid UTF-8).
- [ ] Replay parity: the replay walk emits the IDENTICAL pair set for
      the same stored payload (one test, both walks, one sink).
- [ ] Cancelled call: no pairs, no resultRaw (recording-sink test).
- [ ] Unknown-callId result: pair set rides the existing merge path (test).
- [ ] Console parity golden UNCHANGED and byte-identical (F9) — run it,
      do not regolden it.
- [ ] TUI goldens unchanged (pairs are stored, not rendered — task 5).
- [ ] `dub test` green; cpp_tui suite green (no C++ logic change; the
      copy path from task 2 already carries the fields).

### Notes
- PL-8 does NOT apply here — the Thinking-row grouping is task 6; this
  task changes only the tool block's payload.
- The meta line (the body's args summary) and the pair set are separate
  transports on the same block: meta keeps today's content and cap; the
  pair set is additive. Do not merge them.
- Plan-time anchors to re-verify (they move): app_agent/package.d
  110-111/134/153-157/1064/1079-1083/1110; utility.d 126/137/172.

Plan pointers (task-specific only — standing rules live in the overview):
- Decision letters used: PL-11 (defined in the overview).
- Task 2 owns the wire shape (TuiBlock delta); restate only what this
  task's contract needs — the unpacker semantics above are the owner.
