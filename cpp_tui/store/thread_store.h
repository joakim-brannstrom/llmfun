/// The v4 thread store, behind a narrow adapter (design §4.4, D-ADAPTER).
///
/// The v4 payload (`ChatMessage`, `ChatMessageType`, `RenderGroup`) lives in `tui.h`; this
/// module is the only code that reads it, and `views/` sees `GroupView` and
/// `model::BlockDesc` instead. That is what lets the thread be *mounted* in the workbench
/// layout (task 18) without a view naming a v4 type, and what keeps a restyle (tasks 25/26)
/// a view change while the store keeps working.
///
/// What this header may not name: `ChatMessage`, `ChatMessageType`,
/// `RenderGroup`, `TuiState`'s layout. The aggregate is forward-declared and only
/// the implementation sees it, so a view can include this header with no `tui.h`
/// and no `cpp_tui` include directory.
///
/// Group identity (D-GROUPID, P-Δ4): the adapter assigns its own monotonic id
/// when a group first appears - never `ChatMessage::id`, never reused within a
/// session, reset only by `reset()` (clear / session switch). Expansion is one
/// store, not two: `expanded()/setExpanded()` are the seam a restyled header uses
/// (task 26), and they read and write the very sets the store keeps
/// (`ChatTab::outputLineOpen` for a message, `ChatTab::openWorkGroups` for a work
/// group, both keyed by the group's first message id), so the block model and the
/// store cannot disagree. That first-appearance moment is also where the
/// group's one-shot default expansion is applied (`sync()`), so the default is a
/// property of the group rather than of the frame that drew it - a group revealed
/// by `Load earlier` looks the way it did when it was new. `commitSeen()` is the
/// other half: it is what makes a toggle *stick* on a frame that renders only a
/// window of the thread.
///
/// Rendering: the workbench draws a group as a list of *blocks* (design
/// §4.5, task 26). `beginGroup(k, cell)` opens one group's draw in the current
/// window's flow - the block id scope, the expansion sample and the top row the
/// height hint is measured from - `blockCount()/blockAt()` describe the rows,
/// `views::renderBlock()` draws each row's chrome, `renderBlockBody()` draws the
/// body underneath it (this is the only code that knows how a message body is
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

// Relative, not `"model/block.h"`: this header travels inside `tui.h`, and several
// test targets include `tui.h` with no `cpp_tui/` on the include path (they rely on a
// header resolving its own neighbours - the same reason `model/ui_state.h` names
// `session_rows.h` without a directory). A `"../"` include is resolved relative to
// *this* file first, so it works with and without that path.
#include "../model/block.h"

namespace llmfun::tui {
struct TuiState;
} // namespace llmfun::tui

namespace llmfun::tui::ui {
struct Rect;
} // namespace llmfun::tui::ui

namespace llmfun::tui::store {

/// What a group is, in the three shapes the thread draws (design §4.4). Not
/// `llmfun::tui::GroupKind`: that one is the v4 renderer's own enum and stays
/// behind this header, so a rename of the v4 type cannot reach a view.
enum class GroupKind { UserQuery, FinalAnswer, AssistantWork };

/// One group of the thread, as a view sees it.
///
/// `id` is the adapter's monotonic group id (never reused). `revision` is the
/// stream update counter for the streaming group and 0 for every other group, so
/// a consumer can tell "this group changed" from "this group is the same" without
/// comparing text. `oneLineSummary` is what a collapsed group says in one line,
/// and `lineCountEstimate` is a *rough* line count (the height hint before the
/// group was drawn once).
struct GroupView {
    size_t id = 0;
    GroupKind kind = GroupKind::AssistantWork;
    uint64_t revision = 0;
    std::string oneLineSummary;
    int lineCountEstimate = 1;
};

/// The only v4-aware class the shell touches.
class ThreadStoreAdapter {
public:
    explicit ThreadStoreAdapter(TuiState& state);

    /// Point the adapter at `state`.
    ///
    /// The adapter is a member of `TuiState`, so its pointer is normally that
    /// state's own address - but a state can be *reassigned* (`state =
    /// TuiState()`, which a harness does). A move leaves the adapter unbound
    /// instead of dangling (see below), so the contract is simply: the frame layer
    /// binds once per frame, before anything reads the thread; between a state
    /// assignment and that bind every accessor answers empty.
    void bind(TuiState& state) { state_ = &state; }

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
        idByStart_.clear();
        state_ = nullptr;
        nextId_ = 1;
        streamGroupId_ = 0;
        streamRevision_ = 0;
        streamFingerprint_ = 0;
        synced_ = false;
        syncedCount_ = 0;
        syncedFront_ = 0;
        syncedBack_ = 0;
        syncedStream_ = 0;
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
    bool bound() const { return state_ != nullptr; }

    /// Groups in the thread, streaming message included as the last one. Cheap:
    /// the group list is rebuilt only when the store changed (append, eviction,
    /// clear, stream update), never per call.
    size_t groupCount() const;
    /// The group at `k`, `k` in `[0, groupCount())`; a zeroed view for anything
    /// out of range (never a crash, never a neighbour's data).
    GroupView groupAt(size_t k) const;

    /// Whether the thread ends with the live answer (design FR-08, task 38). The stream
    /// group is the *last* one while a stream runs, so this is also "index
    /// `storedGroupCount()` exists".
    bool hasStream() const;
    /// The groups the bounded window budgets for: `groupCount()` without the live answer.
    ///
    /// The stream is not part of the window (D-BOUNDS): it is drawn last, after the window,
    /// so a live answer can never be paged out by `Load earlier` and the paging arithmetic
    /// counts the *stored* conversation - the number the user is paging through.
    size_t storedGroupCount() const;

    // --- the block seam (design §4.5, task 26) ------------------------------
    //
    // One group is a list of blocks: its own header row, then one row per message,
    // with a message's thinking disclosure as a block of its own in front of it. The
    // list is built where the group list is built (`sync()`), so enumeration stays
    // incremental and a block index is stable for as long as its group is.

    /// Blocks in group `k` (0 for an index the adapter does not hold).
    ///
    /// A work group answers 1 + its messages (plus one per thinking disclosure); a
    /// top-level group answers 1 + a thinking disclosure when the message has one.
    std::size_t blockCount(size_t k) const;

    /// Block `b` of group `k`, described in the renderer's own vocabulary
    /// (`model::BlockDesc`); a default-constructed descriptor out of range. The
    /// summary is display text: the store's `"[nr] body [time]"` decoration and any
    /// state emoji have already been resolved away (the emoji becomes `state`).
    model::BlockDesc blockAt(size_t k, std::size_t b) const;

    /// Whether block `b` shows its body. Group and message blocks read the very sets
    /// the store keeps (`openWorkGroups` / `outputLineOpen`), so a click and a read
    /// cannot disagree (D-GROUPID); a thinking disclosure is the view's own and lives
    /// in `UiState` - it has no v4 set to share.
    bool blockExpanded(size_t k, std::size_t b) const;
    /// Flip a block's disclosure; answers the state it now has (false out of range,
    /// and false for a block that does not open at all).
    bool setBlockExpanded(size_t k, std::size_t b, bool open);

    /// The block's body as it would be copied: the message text (the thinking text for
    /// a thinking block) verbatim. Only the copy path asks for it - a per-frame copy of
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

    /// Whether the group with this id shows its body. The streaming group is
    /// always open; a work group follows `openWorkGroups`, a single-message
    /// group follows `outputLineOpen` - the same state the store
    /// write, which is why the two cannot drift.
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

    /// Forget the in-flight stream's identity. `tuiStreamChatMessageClear()` calls
    /// this when the store clears the stream: the next stream must get a fresh group
    /// id (D-GROUPID) and its reasoning starts open again, and the groups are
    /// rebuilt on the next sync so the old stream row cannot linger for a frame.
    void clearStream();

    /// Commit the expansion defaults this pass applied: advance the one-shot
    /// watermark to the newest message, so a frame that drew the thread cannot
    /// have its work undone by the *next* frame's default seeding.
    ///
    /// The defaults themselves are seeded where "new" is decided - `sync()`, at the
    /// moment a group's id is assigned - so a group revealed later by `Load earlier`
    /// keeps the default it was born with and a window or a page cannot change the
    /// answer. This watermark is the other half of the rule: it is what stops the
    /// render path from re-applying that default over a header the user just
    /// toggled, i.e. it is the difference between `setExpanded()` sticking and being
    /// a one-frame no-op. Call it once per frame, after the groups were drawn.
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
        /// The block's stable identity: the message id, or the group id for a group
        /// header. Never a position, which a paging window would remap.
        size_t key = 0;
        /// The message this block belongs to (an index into the v4 store; the
        /// group's own header names its first message).
        size_t message = 0;
        double timingS = 0.0;
        int steps = 0;
        bool group = false;    ///< the group's own header row
        bool thinking = false; ///< the message's thinking disclosure
        bool code = false;     ///< the body is a single fenced code block
        bool expandable = false;
    };

    struct Entry {
        GroupView view;
        size_t start = 0; // first message index in the v4 store
        size_t end = 0;   // exclusive
        bool stream = false;
        std::vector<BlockEntry> blocks;
    };

    /// Rebuild `groups_` if the store changed since the last call.
    void sync() const;
    const Entry* entryFor(size_t id) const;
    /// The block list of one group (`nullptr` out of range). A probe, not a walk.
    const BlockEntry* blockEntryAt(size_t k, std::size_t b) const;

    TuiState* state_;

    // The group cache (mutable: `sync()` runs from the const accessors).
    mutable std::vector<Entry> groups_;
    /// message id of a group's first message -> the group's id, so an appended
    /// message extends a group (and keeps its id) instead of renumbering it.
    mutable std::map<size_t, size_t> idByStart_;
    mutable size_t nextId_ = 1;
    /// `openWorkGroups` keys (a work group's first message id) the default - not the
    /// user - opened because the group was the newest at birth. When a newer stored
    /// group arrives they close again; a user toggle erases the key, so a group the
    /// user opened is never closed here.
    mutable std::set<size_t> autoOpenWorkGroups_;
    mutable size_t streamGroupId_ = 0;
    mutable uint64_t streamRevision_ = 0;
    mutable size_t streamFingerprint_ = 0;
    mutable bool synced_ = false;
    mutable size_t syncedCount_ = 0;
    mutable size_t syncedFront_ = 0;
    mutable size_t syncedBack_ = 0;
    mutable uint64_t syncedStream_ = 0;

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
