/// The typed thread store, behind a narrow adapter.
///
/// The typed payload (`TypedTurn`/`TypedBlock`) lives in `typed_thread_store.h`; this
/// module is the only code that projects it, and `views/` sees `GroupView` and
/// `model::BlockDesc` instead. That is what lets the thread be *mounted* in the workbench
/// layout without a view naming a store type, and what keeps a restyle
/// a view change while the store keeps working.
///
/// What this header may not name: `TypedThreadStore`'s map internals,
/// `TuiState`'s layout. The aggregate is forward-declared and only
/// the implementation sees it, so a view can include this header with no `tui.h`
/// and no `cpp_tui` include directory.
///
/// Group identity: the adapter's group id IS the persisted
/// turn id - never renumbered within a session, reset only by `reset()`
/// (clear / session switch). Expansion is one store, not two:
/// `expanded()/setExpanded()` are the seam a restyled header uses,
/// and they read and write the very pins the store keeps (`ChatTab::groupPins`
/// for a work group's fold, `ChatTab::rowPins` for a row's - absolute state
/// keyed by the turn id), so the block model and the
/// store cannot disagree. A turn is born folded: there is no one-shot default
/// expansion to seed, and `commitSeen()` is the view's once-per-frame shape
/// (a deliberate no-op kept so a frame's render contract does not change with
/// what the fold happens to store).
///
/// Rendering: the workbench draws a group as a list of *blocks* (design
/// §4.5, task 26). `beginGroup(k, cell)` opens one group's draw in the current
/// window's flow - the block id scope, the expansion sample and the top row the
/// height hint is measured from - `blockCount()/blockAt()` describe the rows,
/// `views::renderBlock()` draws each row's chrome, `renderBlockBody()` draws the
/// body underneath it (this is the only code that knows how a block body is
/// rendered), and `endGroup(k, cell)` closes the group, latches the toggle and stores
/// the height it used. `cell` is the box the caller reserved, with `cell.w` as the
/// content width and `cell.h` the height hint from `measureHint()`.
///
/// The hint is a hint: a group taller than its estimate is drawn in full (never
/// clipped to the estimate), and the height it actually used is cached as the next
/// hint - keyed by `(groupId, revision, contentWidth, row, compact)`, so a width or
/// font change drops it (design §4.4, task 25 owns the anchoring that consumes it).
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

// TODO: should never use relative paths that traverse backwards.
// Relative, not `"model/block.h"`: this header travels inside `tui.h`, and several
// test targets include `tui.h` with no `cpp_tui/` on the include path (they rely on a
// header resolving its own neighbours - the same reason `model/ui_state.h` names
// `session_rows.h` without a directory). A `"../"` include is resolved relative to
// *this* file first, so it works with and without that path.
#include "../model/block.h"

#include "typed_thread_store.h"

namespace llmfun::tui::model {
struct ThreadState;
} // namespace llmfun::tui::model

namespace llmfun::tui {
struct TuiState;
struct ChatTab;
} // namespace llmfun::tui

namespace llmfun::tui::ui {
struct Rect;
} // namespace llmfun::tui::ui

namespace llmfun::tui::store {

/// What a group is, in the three shapes the thread draws (design §4.4). Not
/// `llmfun::tui::GroupKind`: that one is the renderer's own enum and stays
/// behind this header, so a rename of the renderer type cannot reach a view.
enum class GroupKind { UserQuery, FinalAnswer, AssistantWork };

/// The turn-level rows' block index: the query row and the answer row
/// live outside the group's row list, so every per-block accessor
/// (`blockAt`, `blockExpanded`, the copy, the body, the `s` toggle) addresses
/// them through these sentinels. A turn without such a block answers a null
/// entry (a descriptor with `group == 0`) / false there.
inline constexpr std::size_t kQueryRow = static_cast<std::size_t>(-1);
inline constexpr std::size_t kAnswerRow = static_cast<std::size_t>(-2);

/// The row vocabulary's batch sentinel: a plain row's `batch` - the query
/// batch's rows and any batch the projection did not bucket. Not an index
/// into anything: only "is this row a response-batch row" is read off it.
inline constexpr std::size_t kNoBatch = static_cast<std::size_t>(-1);

/// One group of the thread, as a view sees it.
///
/// `id` is the group's id - the persisted turn id (never reused). `revision` is
/// the per-turn change counter bumped when a rebuild observes the turn's blocks
/// change, so a consumer can tell "this group changed" from "this group is the
/// same" without comparing text. `oneLineSummary` is what a collapsed group says
/// in one line, and `lineCountEstimate` is a *rough* line count (the height hint
/// before the group was drawn once).
///
/// `typedTurn` marks a group that projects a typed turn: the minimal
/// projection folds its work rows behind the header and draws only the turn's
/// content.
struct GroupView {
    size_t id = 0;
    GroupKind kind = GroupKind::AssistantWork;
    uint64_t revision = 0;
    std::string oneLineSummary;
    int lineCountEstimate = 1;
    bool typedTurn = false;
    /// Whether the turn's Answer row exists (the kAnswerRow sentinel). The
    /// palette's copy-answer cache stamps this: an answer arriving on the
    /// sentinel moves no count, so the shape stamp needs the flag itself.
    bool hasAnswer = false;
};

/// The only store-aware class the shell touches.

class ThreadStoreAdapter {
public:
    /// Unbound: every accessor answers empty until a `bind()`.
    ThreadStoreAdapter() = default;

    /// The main-window binding: `state.chat` + `state.ui.thread`.
    explicit ThreadStoreAdapter(TuiState& state);

    /// Point the adapter at `state`.
    ///
    /// The adapter is a member of `TuiState`, so its pointers are normally that
    /// state's own members - but a state can be *reassigned* (`state =
    /// TuiState()`, which a harness does). A move leaves the adapter unbound
    /// instead of dangling (see below), so the contract is simply: the frame layer
    /// binds once per frame, before anything reads the thread; between a state
    /// assignment and that bind every accessor answers empty.
    void bind(TuiState& state);

    /// Bind a different store/view pair: a mount over an agent's `ChatTab`
    /// store and the drill-down's own `UiState::agentThread`. Same per-frame
    /// contract as the state bind: called before anything reads. A pair *change*
    /// under live caches (the user moved the focus) is a full drop, because a
    /// cache built from one store's group ids is meaningless over another.
    void bind(ChatTab& chat, model::ThreadState& thread);

    /// Unbind when this store is going away (an eviction, `tuiPipelineClear`): the
    /// mount must never carry a dead store's address across a frame. A no-op when
    /// the adapter holds a different store, so one agent's eviction cannot cost
    /// another agent's mount its caches.
    void dropChat(const ChatTab* chat);

    /// A moved-into adapter forgets its state instead of carrying a stale pointer.
    ///
    /// The adapter is a member of `TuiState` and holds that state's address, so a
    /// state assignment (`state = TuiState()`, which a harness does) would carry the
    /// pointer of the object being assigned *from* - a pointer into a temporary that
    /// is about to die. A move is therefore a **drop**, not a transfer: the target
    /// ends up unbound and every accessor answers empty until the frame layer's next
    /// `bind()`, while the source keeps its own binding. Nothing dangles, and the one
    /// window a move creates can no longer be undefined behaviour. (Tests:
    /// `test_thread_window` pins both halves, so "the move does not actually move"
    /// cannot be "fixed" back into the dangling pointer.) Copying is deleted
    /// outright: two adapters caching one thread is never what is wanted.
    ThreadStoreAdapter(const ThreadStoreAdapter&) = delete;
    ThreadStoreAdapter& operator=(const ThreadStoreAdapter&) = delete;
    ThreadStoreAdapter(ThreadStoreAdapter&& other) noexcept { *this = std::move(other); }
    ThreadStoreAdapter& operator=(ThreadStoreAdapter&&) noexcept {
        groups_.clear();
        indexById_.clear();
        typedRevision_.clear();
        typedFingerprints_.clear();
        typedSynced_ = false;
        syncedTypedVersion_ = 0;
        chat_ = nullptr;
        thread_ = nullptr;
        hintWidth_ = 0.0f;
        hintCompact_ = false;
        expandedGroup_ = 0;
        renderCalls_ = 0;
        groupOpen_ = false;
        groupExpandedBefore_ = false;
        groupOpenBefore_ = 0;
        groupTop_ = 0.0f;
        return *this;
    }

    /// False on an adapter that has not been bound since its state moved. Only the
    /// frame layer's `bind()` makes it true again.
    bool bound() const { return chat_ != nullptr && thread_ != nullptr; }

    /// The thread's mode: the tab's own flag
    /// (`ChatTab::threadMinimalMode`, factory default minimal). Unbound, the
    /// adapter answers minimal - every default the view draws is the folded one.
    bool minimalMode() const;

    /// Whether the group is drawn OPEN: the group's
    /// absolute pin first (`ChatTab::groupPins`, keyed by the persisted turn
    /// id) - then the mode's liveness default, open only while the turn is
    /// live in maximal mode. Unbound, closed (every default the view draws is
    /// the folded one). Cheap: one map probe over the drawn window.
    bool groupOpen(size_t groupId) const;

    /// Whether the group draws in the MINIMAL STYLE: the mode is
    /// minimal AND the group is not pinned or live-open. An opened minimal
    /// group draws maximal-style; a folded maximal group never draws the
    /// minimal chips. Unbound, true (the folded shape). Same cost.
    bool groupStyleIsMinimal(size_t groupId) const;

    /// Groups in the thread, streaming message included as the last one. Cheap:
    /// the group list is rebuilt only when the store changed (append, eviction,
    /// clear, stream update), never per call. Unbound, the sync inside answers
    /// an empty list, so the count/entry accessors answer empty without a
    /// dereference - the frame layer still binds before it reads.
    size_t groupCount() const;

    /// The group at `k`, `k` in `[0, groupCount())`; a zeroed view for anything
    /// out of range (never a crash, never a neighbour's data).
    GroupView groupAt(size_t k) const;

    /// Whether the thread ends with the live answer. The stream
    /// group is the *last* one while a stream runs, so this is also "index
    /// `storedGroupCount()` exists".
    bool hasStream() const;
    /// Whether the live answer carries prose / reasoning *right now* - the two
    /// facts the frame layer's clock derives its phases from (`hasText` ends the
    /// reasoning, `hasThinking` starts it). False with no live answer.
    bool streamHasText() const;
    bool streamHasThinking() const;
    /// The groups the bounded window budgets for: `groupCount()` without the live answer.
    ///
    /// The stream is not part of the window (D-BOUNDS): it is drawn last, after the window,
    /// so a live answer can never be paged out by `Load earlier` and the paging arithmetic
    /// counts the *stored* conversation - the number the user is paging through.
    size_t storedGroupCount() const;

    // --- the block seam (design §4.5, task 26) ------------------------------
    //
    // One group is a list of blocks: its own header row, then one row per
    // displayed store block. The list is built where the group list is built
    // (`sync()`), so enumeration stays incremental and a block index is stable
    // for as long as its group is.

    /// Blocks in group `k` (0 for an index the adapter does not hold).
    ///
    /// A work group answers 1 + its displayed blocks (a thinking row of its own
    /// among them); every group's first row is its header.
    std::size_t blockCount(size_t k) const;

    /// Block `b` of group `k`, described in the renderer's own vocabulary
    /// (`model::BlockDesc`); a default-constructed descriptor out of range. The
    /// summary is display text: the store's decoration has already been
    /// resolved into the row's own fields (the state). `b` also names the
    /// turn-level rows through `kQueryRow`/`kAnswerRow` - a turn without
    /// such a block answers a descriptor with `group == 0`.
    model::BlockDesc blockAt(size_t k, std::size_t b) const;

    /// How many response batches the turn projects (0 out of range) - a
    /// read-only count over the turn's bucketed batches, so the tests can pin
    /// the bucketing without reaching into the adapter. The query's batch is
    /// not a response batch and is never counted.
    std::size_t batchCount(size_t k) const;

    /// Whether block `b` draws at all, and whether it shows its body. Both read
    /// the derived predicate: the turn's liveness, the
    /// batch's position and the user's absolute pins (`ChatTab::rowPins` /
    /// `groupPins`) - never a mode+kind fold. A click and a read cannot
    /// disagree; a thinking disclosure is the view's own and lives
    /// in `UiState`.
    bool blockExpanded(size_t k, std::size_t b) const;
    /// Flip a block's disclosure; answers the state it now has (false out of range,
    /// and false for a block that does not open at all).
    bool setBlockExpanded(size_t k, std::size_t b, bool open);

    /// The block's body as it would be copied: the store block's text verbatim.
    /// Only the copy path asks for it - a per-frame copy of
    /// every body would pay for a feature nobody pressed.
    std::string blockText(size_t k, std::size_t b) const;

    /// Whether the block's copy strips the markdown fences: a `Code`
    /// row copies its code, and a batch owner copies its batch's content - the
    /// fences strip when THAT content is code (the copy goes through
    /// the owner). The copy path's only fence question, answered here so the
    /// view never re-derives it (B1).
    bool blockCopyIsCode(size_t k, std::size_t b) const;

    /// Whether `s` has anything to show on block `b` of group `k`: the row is drawn as markdown
    /// *and* `ui::bodyHasTable()` says its body holds a GFM table this profile draws as a grid.
    ///
    /// The detector is the answer - it is the same span list `bridgeTables()` rewrites - so the
    /// toggle and the renderer can never disagree about what a table is, and the predicate pays
    /// a scan rather than building the bridged copy. A row that is drawn verbatim (a tool
    /// transcript) answers false instead of setting a flag no renderer reads. Called on a key
    /// press only, never per frame.
    bool blockHasTable(size_t k, std::size_t b) const;

    /// Draw block `b`'s body under its header, in the current flow, in `cell`
    /// (`cell.w` the content width). Prose goes through the markdown wrapper,
    /// a tool body verbatim, a thinking body dim - the one renderer, because this is
    /// the only code that knows how a body is drawn.
    void renderBlockBody(size_t k, std::size_t b, const ui::Rect& cell);

    /// Open group `k`'s draw: sync, unit check, the block id scope, the expansion
    /// sample `endGroup()` compares against, and the top row the hint is measured
    /// from. False when there is nothing to draw (out of range, wrong units, no
    /// state) - the caller then draws no blocks at all.
    bool beginGroup(size_t k, const ui::Rect& cell);
    /// Close it: latch the toggle (task 25's anchor reads it through
    /// `takeExpandedGroup()`) and store the height the group used as the next hint.
    void endGroup(size_t k, const ui::Rect& cell);

    /// Whether the group with this id is drawn open: the group's absolute pin
    /// (`ChatTab::groupPins`, keyed by the turn id) first, then the liveness
    /// default - the same state the store keeps, which is why the two cannot
    /// drift.
    bool expanded(size_t id) const;
    void setExpanded(size_t id, bool open);

    /// Forget every pin: the fold's escape hatch. The pins are
    /// absolute, so a user who pinned twenty rows needs one action that clears
    /// them all at once - and it *clears*, never writes: writing `false` into
    /// every live row's pin would pin against the default and defeat the fold
    /// the user asked to restore. The next frame draws every group on its mode
    /// default. Unbound, a no-op.
    void collapseAll();

    /// The height `id` is expected to need, in the active profile's units
    /// (cells / px): the cached drawn height once the group has been rendered in
    /// *this* box, otherwise `lineCountEstimate` rows. A hint for paging and
    /// anchoring - never a clipper.
    ///
    /// The cache lives in `UiState::thread.hints` (design §5.1), keyed by
    /// `(groupId, revision, width, row, compact)`. `setHintContext()` is how the
    /// view states the box it is about to draw into; a context that differs from
    /// the cached one makes the entry a guess about a *different* box, so it is
    /// ignored rather than trusted.
    float measureHint(size_t id) const;

    /// Tell the adapter which box the next draw happens in: the content width the
    /// view reserved and whether the thread is in compact mode (the width in the
    /// profile's own units). Called by `views::renderThread()` once per frame,
    /// before the first `measureHint()`/`beginGroup()`. The row height comes from the
    /// live metrics inside the adapter, so a font-scale change invalidates the
    /// cache without the caller repeating it.
    void setHintContext(float width, bool compact);

    /// The group whose expansion state changed during the last group draw (0 when
    /// none did). A block row toggles inside the view and `setExpanded()` writes the
    /// same sets from outside it; the adapter is the one place that sees both halves
    /// of any change in the same frame, and the view
    /// reads the answer for its scroll anchor (task 25). Reading it clears it, so a
    /// stale id can never anchor a later frame.
    size_t takeExpandedGroup();

    /// How many times the group list has been rebuilt from the message store, and
    /// how many group entries the adapters' lookups have *walked*. Both are
    /// observations for the incremental-enumeration contract (design §4.4): an
    /// append costs one rebuild, and `groupAt(k)` walks nothing at all.
    size_t groupBuilds() const { return groupBuilds_; }
    size_t entryWalks() const { return entryWalks_; }

    /// The table bridge itself lives in `ui/table.h` (`bridgeBodyForProfile()`); this class only
    /// asks it in `renderBodyText()`, the one place a markdown body is drawn, so every block row
    /// goes through the same function and no two surfaces can disagree about what a table is.
    /// `blockHasTable()` asks `ui::bodyHasTable()` with the same profile flag, so the `s` toggle
    /// offers exactly the bodies the transform would rewrite.
    /// Forget the group list, the ids and the height hints: clear (`Ctrl+L`, a
    /// new session) and session switches come through here, so a group id can
    /// never be reused by the next thread.
    void reset();

    /// Commit the expansion defaults this pass applied. The typed fold has no
    /// defaults left to commit - every turn is born folded and the user's
    /// toggles are their own writes - so the call is a deliberate no-op kept
    /// for the view's once-per-frame shape. Call it once per frame, after the
    /// groups were drawn.
    void commitSeen();

    /// How many group draws this adapter has opened - the double-render tripwire
    /// (`./test_shell`, task 19's harness): one per visible group per frame is the
    /// contract, two means the shell mounted the thread twice.
    size_t renderCalls() const { return renderCalls_; }

private:
    /// One response batch of a turn: the facts the projection buckets
    /// per `batchId`. Built while the turn's rows are bucketed - the query's
    /// batch is not a response batch and gets none. `firstRow` is the
    /// wire index of the batch's first row, kept for the bucket vector's
    /// first-row order; the emission does not read it (the
    /// owner row's placement comes from the sort, not from `firstRow`).
    /// The facts read the batch's rows before any drop: the emission's
    /// drops and the batch's row order shape the row list, never these numbers.
    struct BatchEntry {
        std::string batchId;
        std::size_t firstRow = 0;
        bool isNewest = false; ///< the turn's last bucket
        bool hasReasoning = false;
        bool hasContent = false;
        bool anyFailed = false;
        /// the batch's calls have all settled (Ok/Failed/Cancelled) - the
        /// still-running and pending states clear it
        bool allSettled = true;
        /// the batch holds at least one call (a ToolStep row): a call-less
        /// batch is never "settled" - its streaming prose would otherwise be
        /// vacuously folded (a design-pseudocode bug)
        bool hasAnyCall = false;
        int failedCalls = 0;
        /// the batch's chips line: composed once per rebuild by
        /// `chipsText()` over the batch's tool names, ready to draw
        std::string chips;
        /// the batch's duration: the content block's `durationMs` -
        /// a settled batch says "1.2s" in its header
        double durationS = 0.0;
        /// the batch's surviving content body: the first kept Assistant
        /// child's `typedBody`, the owner row's copy source. Empty when the
        /// batch's content rows all dropped (a calls-only batch copies nothing).
        std::string contentBody;
        /// the content body is a fenced code block (the store's own
        /// `code` fact on the child the body came from): the owner's copy
        /// strips the fences, the same answer the content row's own copy gave
        /// before the fold (the copy goes through the owner).
        bool contentIsCode = false;
    };

    /// One block of a group: the group's own header row, a
    /// message's body row, or a message's thinking disclosure.
    ///
    /// Built with the group list (`sync()`), so `blockAt()` is a copy out of an
    /// already-walked vector rather than a walk of the store per drawn row.
    struct BlockEntry {
        model::BlockKind kind = model::BlockKind::System;
        model::BlockState state = model::BlockState::None;
        /// The block's stable identity: the typed block id's hash, or the turn id
        /// for a group header. Never a position, which a paging window would remap.
        size_t key = 0;
        double timingS = 0.0;
        int steps = 0;
        bool group = false;    ///< the group's own header row
        bool thinking = false; ///< the message's thinking disclosure
        bool code = false;     ///< the body is a single fenced code block
        bool expandable = false;
        std::int64_t typedTurnId = 0;
        std::string typedBlockId;
        /// The store block's title (the tool name on a Tool row): the typed
        /// tool card's summary previews it.
        std::string typedTitle;
        std::string typedBody;
        bool typedQuery = false;
        /// The typed block's batch id: the response's blocks share it, which
        /// is how the projection buckets a row into its batch.
        std::string typedBatchId;
        /// The batch bookkeeping the projection fills: `batch` is the
        /// row's slot in `Entry::batches` (`kNoBatch` for a plain row - the
        /// query batch's rows and anything not bucketed); `batchOwned` says the
        /// row joined a response batch (a bucket child); `ownsBatch` marks the
        /// synthetic owner row itself - the wire never sends it.
        std::size_t batch = kNoBatch;
        bool batchOwned = false;
        bool ownsBatch = false;
        /// The merged card's RESULT pair set
        /// typed block - the renderer draws the store's strings, it never
        /// re-parses. `resultRaw` is the packed one-line fallback for
        /// an array, a non-JSON payload or a zero-pair object. `resultMore` is
        /// the pair count past the sender's cap (the "... K more" tail).
        std::vector<std::pair<std::string, std::string>> resultPairs;
        std::string resultRaw;
        std::uint32_t resultMore = 0;
    };

    struct Entry {
        GroupView view;
        bool stream = false;
        std::vector<BlockEntry> blocks;
        /// The turn-level rows: the query block projects as the turn's
        /// opening row - drawn above the group header - and the answer as the
        /// turn-level row after the group; both draw in both modes, the fold
        /// keeps them. `hasQuery`/`hasAnswer` say the turn carries one.
        BlockEntry queryRow;
        bool hasQuery = false;
        BlockEntry answerRow;
        bool hasAnswer = false;
        /// The turn's live state off the wire (`TypedTurn::state`): Running from
        /// the upsert that opens the turn until the done/stop upsert closes it.
        bool turnLive = false;
        /// The turn's response batches, in first-row order: one per
        /// batch id that is not the query's. The owner rows in `blocks`
        /// project these.
        std::vector<BatchEntry> batches;
    };

    /// Rebuild `groups_` if the store changed since the last call.
    void sync() const;
    const Entry* entryFor(size_t id) const;
    /// The block list of one group (`nullptr` out of range). A probe, not a walk.
    const BlockEntry* blockEntryAt(size_t k, std::size_t b) const;

    /// The bound pair: the store the groups are built from and the
    /// view state the caches read. Null together (unbound) or set together
    ChatTab* chat_ = nullptr;
    model::ThreadState* thread_ = nullptr;

    // The group cache (mutable: `sync()` runs from the const accessors).
    mutable std::vector<Entry> groups_;

    // the typed-backed projection
    mutable bool typedSynced_ = false;
    mutable std::uint64_t syncedTypedVersion_ = 0;
    /// Per-turn change counter for `view.revision` (0 = unchanged since birth),
    /// keyed by the persisted turnId; bumped when a rebuild observes the turn's
    /// block fingerprint change, dropped when the turn is gone.
    mutable std::map<std::int64_t, std::uint64_t> typedRevision_;
    /// The per-turn block fingerprint the revision compares (last rebuild's).
    mutable std::map<std::int64_t, std::uint64_t> typedFingerprints_;

    /// The derived predicate: "does this row draw" and "is its
    /// body open", both from the turn's liveness, the batch's position and the
    /// user's absolute pins. `blockAt()` fills `visible`/`expanded` from them;
    /// `blockExpanded()` is the open half. No mode+kind fold survives.
    bool resolveVisible(const Entry& entry, const BlockEntry& block) const;
    bool resolveOpen(const Entry& entry, const BlockEntry& block) const;
    /// The batch's live flag: the turn is live, the batch is the turn's
    /// newest and it is unsettled or still holds the turn's content. The ONE
    /// liveness default - the owner row and every child read it, so a batch
    /// whose response is still streaming (no call has landed yet) draws its
    /// thinking open. False for a plain row.
    bool batchLiveFor(const Entry& entry, const BlockEntry& block) const;

    /// The typed-backed rebuild (group id = the persisted turnId).
    void syncTyped() const;

    /// Height hints live in `UiState::thread.hints` (the
    /// cache is view state, and it must be dropped by a session switch that this
    /// class does not observe). What stays is the box the cache was keyed by.
    ///
    /// The context the last `setHintContext()` named: content width, and whether
    /// the thread was compact. `row` is read live from the metrics inside
    /// `measureHint()`, so a font change needs no caller.
    float hintWidth_ = 0.0f;
    bool hintCompact_ = false;

    /// group id -> index in `groups_`. The lookup `expanded()`, `setExpanded()` and
    /// `measureHint()` need, as a map rather than a scan: a scan per call would be
    /// O(groups) per visible group per frame, i.e. exactly the per-frame walk of
    /// the store that incremental enumeration exists to avoid.
    mutable std::map<size_t, size_t> indexById_;
    /// Group entries walked by `entryFor()` since the adapter was created (the
    /// incremental-enumeration counter; a map lookup walks nothing).
    mutable size_t entryWalks_ = 0;
    /// Full group-list rebuilds (an append, an eviction, a stream change, a clear).
    mutable size_t groupBuilds_ = 0;

    /// The group whose expansion changed during the last group draw (0 = none).
    size_t expandedGroup_ = 0;

    size_t renderCalls_ = 0;

    // --- the open group being drawn (task 26) --------------------------------
    // `beginGroup()` samples and `endGroup()` compares, so the pair must not be
    // interleaved with another group's draw. The shell draws the thread once, in one
    // loop, so that is a property of the frame, not a new contract: a group that is
    // opened and never closed is closed by the next `beginGroup()`.
    bool groupOpen_ = false;
    bool groupExpandedBefore_ = false;
    size_t groupOpenBefore_ = 0;
    float groupTop_ = 0.0f;
};

/// The one chips composer: the running tool calls' names as the
/// live-edge chips line - `"[bash] [grep]"`, first three distinct names then
/// `+N` (`"[bash] [grep] [read_file] +1"`), empty for no calls. The projection
/// composes a batch's settled chips with it, and the view composes the live
/// edge's with the same helper, so the two notations can never drift apart.
std::string chipsText(const std::vector<std::string>& toolNames);

// The palette's "copy last answer" used to ask this file for the newest answer's text
// (`lastAnswerText()`). It asks the adapter for a *block* now -
// `ThreadStoreAdapter::blockAt()`/`blockText()`, the same seam the thread draws and copies
// through - because "the last answer" is a block, the live streamed body included, and a
// message-store walk could neither see the stream nor stay in step with what is on screen.

} // namespace llmfun::tui::store
