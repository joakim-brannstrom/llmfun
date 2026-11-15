/// The typed thread store, behind a narrow adapter.
///
/// The typed payload (`TypedTurn`/`TypedBlock`) lives in `typed_thread_store.h`; this
/// module is the only code that projects it, and `views/` sees `GroupView` and
/// `model::BlockDesc` instead. That is what lets the thread be *mounted* in the workbench
/// layout (task 18) without a view naming a store type, and what keeps a restyle
/// (tasks 25/26) a view change while the store keeps working.
///
/// What this header may not name: `TypedThreadStore`'s map internals,
/// `TuiState`'s layout. The aggregate is forward-declared and only
/// the implementation sees it, so a view can include this header with no `tui.h`
/// and no `cpp_tui` include directory.
///
/// Group identity (D-GROUPID, P-Δ4): the adapter's group id IS the persisted
/// turn id - never renumbered within a session, reset only by `reset()`
/// (clear / session switch). Expansion is one store, not two:
/// `expanded()/setExpanded()` are the seam a restyled header uses (task 26),
/// and they read and write the very sets the store keeps (`ChatTab::openWorkGroups`
/// for a work group's fold, keyed by the turn id), so the block model and the
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

/// One group's session-local override of the minimal/maximal mode (design §6,
/// task 8): a click on one group's header pins THAT group against the mode -
/// `ForceOpen` draws it maximal-style in minimal mode, `ForceClosed` folds it in
/// maximal mode. Keyed by the group's persisted turn id (`GroupView::id`), so
/// an upsert or a rebuild never orphans one; the entry-point layer clears the
/// map on a session switch and a clear, and a turn's entry dies with the turn.
enum class GroupOverride { ForceOpen, ForceClosed };

/// The turn-level rows' block index: the query row and the answer row
/// live outside the group's row list, so every per-block accessor
/// (`blockAt`, `blockExpanded`, the copy, the body, the `s` toggle) addresses
/// them through these sentinels. A turn without such a block answers a null
/// entry (a descriptor with `group == 0`) / false there.
inline constexpr std::size_t kQueryRow = static_cast<std::size_t>(-1);
inline constexpr std::size_t kAnswerRow = static_cast<std::size_t>(-2);

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
        typedRowOpen_.clear();
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

    /// Whether the thread draws in minimal mode (design §6): the tab's own flag
    /// (`ChatTab::threadMinimalMode`, factory default minimal). Unbound, the
    /// adapter answers minimal - every default the view draws is the folded one.
    bool minimalMode() const;

    /// The group's effective mode (design §6, task 8): its session-local
    /// override first - a pinned group draws against the mode (`GroupOverride`,
    /// `ChatTab::groupOverrides`, keyed by the persisted turn id) - then the
    /// mode itself. Unbound, minimal (every default the view draws is the
    /// folded one). Cheap: one map probe over the drawn window.
    bool groupMinimal(size_t groupId) const;

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

    /// Whether block `b` shows its body. Group and row blocks read the very sets
    /// the store keeps (`openWorkGroups` / `typedRowOpen_`), so a click and a read
    /// cannot disagree (D-GROUPID); a thinking disclosure is the view's own and lives
    /// in `UiState`.
    bool blockExpanded(size_t k, std::size_t b) const;
    /// Flip a block's disclosure; answers the state it now has (false out of range,
    /// and false for a block that does not open at all).
    bool setBlockExpanded(size_t k, std::size_t b, bool open);

    /// The block's body as it would be copied: the store block's text verbatim.
    /// Only the copy path asks for it - a per-frame copy of
    /// every body would pay for a feature nobody pressed.
    std::string blockText(size_t k, std::size_t b) const;

    /// Whether `s` has anything to show on block `b` of group `k`: the row is drawn as markdown
    /// *and* `ui::bodyHasTable()` says its body holds a GFM table this profile draws as a grid
    /// (design §6.8, task 37).
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

    /// Whether the group with this id shows its body: the fold is one flag per
    /// group, `openWorkGroups` keyed by the group's id - the same state the
    /// store keeps, which is why the two cannot drift.
    bool expanded(size_t id) const;
    void setExpanded(size_t id, bool open);

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
    /// One block of a group (design §4.5, task 26): the group's own header row, a
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
        /// The typed block's batch id: the response's blocks share it, which is
        /// how a thinking disclosure counts its batch's tool cards (F5).
        std::string typedBatchId;
        /// The batch's tool-card count (F5): filled for a Thinking row by
        /// counting the Tool rows that share its `typedBatchId`.
        int batchTools = 0;
        /// The tool card's token counters (F5), copied from the typed block -
        /// each renders only when non-zero.
        std::int64_t tokensIn = 0;
        std::int64_t tokensOut = 0;
        /// The typed block's meta (the tool result's one-line summary, D4):
        /// the maximal card's embedded Result.
        std::string typedMeta;
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
    mutable std::map<std::int64_t, std::set<std::string>> typedRowOpen_;

    /// The typed-backed rebuild (design §4: group id = the persisted turnId).
    void syncTyped() const;

    /// Height hints used to live here; they are `UiState::thread.hints` now (the
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
    /// the store that incremental enumeration exists to avoid (design §4.4).
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

// The palette's "copy last answer" used to ask this file for the newest answer's text
// (`lastAnswerText()`). It asks the adapter for a *block* now -
// `ThreadStoreAdapter::blockAt()`/`blockText()`, the same seam the thread draws and copies
// through - because "the last answer" is a block, the live streamed body included, and a
// message-store walk could neither see the stream nor stay in step with what is on screen.

} // namespace llmfun::tui::store
