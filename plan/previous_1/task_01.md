## Task 1: v5 wire types + ABI entry points (additive)

Priority: P0. Dependencies: none.

### Description
Declare the v5 turn/block wire types and the five v5 entry points in
`cpp_tui/tui_api.h`, bump `TUI_API_VERSION`, and pin the layout the way this repo
pins contracts (C++ contract-test asserts + D-side static asserts — never asserts
inside tui_api.h itself, user convention). Nothing carries v5 on the wire yet; both
sides build.

### Changes Made
- llmfun/cpp_tui/tui_api.h (edit at 41-44 version, new section near the v4 chat
  decls at 642-699): `#define TUI_API_VERSION 12`; add the types + entry points
  verbatim below; header comments stay semantic (ownership/0-means-unknown), no
  byte-count layout comments.
- llmfun/cpp_tui/test_tui_api_v4.cpp (edit: new case in the contract test, pattern
  of the offset-pin block at 900-1022): pin `sizeof(TuiTurn)`, `sizeof(TuiBlock)`
  and every member offset for the 64-bit ABI; assert the enum values.
- llmfun/source/llm/tui/package.d (edit: new static asserts near the existing
  patterns at 249-262 and 579): D-side `static assert(TuiTurn.sizeof == ...)`,
  `static assert(TuiBlock.sizeof == ...)` on the imported C types.

```c
/* Verbatim contract — v5 thread types (design §4) */
typedef enum TuiTurnState {
    TuiTurnState_Running = 0, TuiTurnState_Done = 1, TuiTurnState_Interrupted = 2
} TuiTurnState;
typedef enum TuiBlockKind {
    TuiBlockKind_Prose = 0, TuiBlockKind_Reasoning = 1, TuiBlockKind_Tool = 2,
    TuiBlockKind_Answer = 3, TuiBlockKind_Notice = 4
} TuiBlockKind;
typedef enum TuiBlockStatus {
    TuiBlockStatus_None = 0, TuiBlockStatus_Pending = 1, TuiBlockStatus_Running = 2,
    TuiBlockStatus_Ok = 3, TuiBlockStatus_Failed = 4, TuiBlockStatus_Cancelled = 5
} TuiBlockStatus;
typedef enum TuiBlockFlags {
    TuiBlockFlag_Collapsible = 1, TuiBlockFlag_DefaultOpen = 2, TuiBlockFlag_Selectable = 4
} TuiBlockFlags;

/* id = persisted turnId (0 = unstamped); ordinal = 1-based display position
 * among displayed turns, D-assigned; messageCount informational. */
typedef struct TuiTurn {
    int64_t id;
    int32_t ordinal;
    int32_t state;
    int32_t messageCount;
    int32_t reserved;
} TuiTurn;

/* One block per (turnId, id). id = tool callId | "answer" | batch-scoped id.
 * batchId = "<turnId>#<batchIndex>", recomputed deterministically on replay,
 * stable within the turn's lifetime in one process run, not persisted.
 * Tool-less batches (thinking+content only) carry a batchId too.
 * Strings are inbound/non-owning (copied by the UI), same as ChatMessageParam.
 * durationMs 0 = unknown (not rendered); tokensIn/tokensOut 0 = not rendered. */
typedef struct TuiBlock {
    int64_t turnId;
    String id;
    String batchId;
    String title;
    String meta;
    String body;
    int64_t durationMs;
    int64_t tokensIn;
    int64_t tokensOut;
    int32_t kind;
    int32_t status;
    int32_t flags;
    int32_t reserved;
} TuiBlock;

/* Bulk snapshot (replay, session switch): clears the store, then loads the
 * turns and their blocks. Turns in array order. */
void tuiSetTurns(TuiState* state, const TuiTurn* turns, size_t turnCount,
                 const TuiBlock* blocks, size_t blockCount);
/* Upsert the only mutating paths: a first upsert creates the slot, later
 * upserts merge in place (keyed by (turnId, id) for blocks). */
void tuiTurnUpsert(TuiState* state, const TuiTurn* turn);
void tuiBlockUpsert(TuiState* state, const TuiBlock* block);
/* Removes the turn AND cascades to the turn's blocks (no per-block remove). */
void tuiTurnRemove(TuiState* state, int64_t turnId);
/* Replaces clear-chat: empties the typed store. */
void tuiThreadClear(TuiState* state);
```

### Verification
- [ ] `cmake --build cpp_tui/build` green; `dub build` green (D sees the new
      types via `import llmfun_tui`)
- [ ] New contract-test case passes: pinned sizes/offsets/enums
- [ ] D static asserts compile (they are the proof the layouts agree)
- [ ] No v5 symbol is called anywhere yet (grep: only header, test, asserts)
- [ ] Regression: `dub test` and the cpp_tui test binaries pass (known
      pre-existing reds excluded — see overview)

### Notes
- D1: `tuiSetTurns` carries turns AND blocks (design §5 "emit tuiSetTurns with
  already-merged blocks" wins over the older 3-arg sketch in
  doc/todo_modern_ui.md §8.2).
- D9: pins live in the contract test + D-side asserts, never in tui_api.h
  (user decision abi_seam_no_pins; TUI_API_VERSION bump is kept).
- `String` is the existing ABI string POD (tui_api.h ~55-70 ownership notes);
  D-side `TuiTurn`/`TuiBlock` literals are constructed from it exactly like
  `ChatMessageParam` today (source/llm/tui/package.d:378 pattern).
- Do NOT implement the store in this task — the entry points are declared, not
  defined. Definitions land in task 2 (they need the store). To keep both sides
  building without a store, give the five functions a temporary no-op body in
  tui_api.cpp (marked `// P0-1: unwired; task 2 replaces this`), or declare-only
  in the header and define in task 2 — pick declare-only if the contract test
  does not need to call them.

Plan pointers (task-specific only — standing rules live in the overview):
- Anchors above were verified at plan time; re-verify before each edit.
- Decision letters used: D1, D9 (defined in the overview).
