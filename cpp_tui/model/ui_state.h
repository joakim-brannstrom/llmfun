/// The workbench's own state (design §3.8: one writer, the frame thread; views
/// only read), plus the constants the shell and the thread region share.
///
/// `TuiState::ui` is the aggregate a view reads. The v4 members stay where the
/// store keeps them; this struct is the seam that lets the shell be written without
/// them (D-ADAPTER: `views/*` names no v4 type), and it is what tasks 22/25/28/32/33
/// extend.
///
/// Layering: `model` carries plain data and no renderer. It does not depend on
/// `ui/` either - the layout engine's `LayoutPrefs` is *derived* from
/// `ShellPrefs` by the shell (`views::layoutPrefsFor`), so the model header stays
/// includable from a header that has no `cpp_tui` include directory (see
/// `views/palette.h` for why that matters).
#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "activity.h"
#include "block.h"
#include "session_rows.h"
#include "settings.h"
#include "status_chips.h"
#include "stream_state.h"
#include "thread_view.h"
#include "toast.h"

namespace llmfun::tui::model {

/// Groups in the thread's bounded window (D-BOUNDS): the last 40 on the text
/// grid and the last 30 in the pixel profile, where markdown costs more per
/// group. Compact mode narrows the window to 3, or to 1 when the display is at
/// most `kCompactRows` rows tall - both values come from the layout engine
/// (`ShellLayout::compactGroups`), these two are the ordinary case.
inline constexpr int kThreadWindowGroupsGrid = 40;
inline constexpr int kThreadWindowGroupsGui = 30;

/// The session rail's cross-frame state (design §4.3, task 23).
///
/// Plain data, like the rest of `UiState`: the frame layer builds the rail's
/// inputs from the v4 snapshot (`SessionEntry` never reaches a view), the
/// view reads and updates this struct while it draws, and the frame layer
/// applies the *answers* at the bottom in the same frame. Nothing here is
/// persisted: the search text and the row latches belong to the session, and
/// the pinned set lives in `ShellPrefs` (`ui.conf`) where task 22 put it - and the file itself
/// is the frame layer's business (`views::uiConfFrame()` / `uiConfQuit()`, task 29), so a
/// `Ctrl+B` choice and a pin survive a restart from the frame that made them.
///
/// Why the model and not `views/rail.h`: a header that travels inside `tui.h`
/// may not include `ui/` or another directory's header (see `views/topbar.h`),
/// and the rail's answers are applied by the shell - so the state has no
/// renderer-facing type in it at all.
struct RailState {
    // --- the search field ---------------------------------------------------
    /// The filter query. `filterSeq` suffixes the field's ImGui id so a
    /// programmatic clear (a select, `Esc`) makes the widget re-read the buffer
    /// instead of its stale edit state - the shipped panel's rule.
    std::array<char, 128> filterBuf = {};
    int filterSeq = 0;
    /// Ask for the field to take the keyboard on the next frame (`/`, or the
    /// field's own click in a profile that needs it).
    bool filterFocus = false;
    /// The field held the keyboard at the previous frame boundary - or was handed it on
    /// that frame (`SetKeyboardFocusHere()` is one frame ahead of `IsItemActive()` in some
    /// backends, and the search's end-of-mode rule needs the difference). ImGui clears a
    /// text field's active id inside `NewFrame` (an `Esc` reverts the buffer to its
    /// activation snapshot before this view runs), so "the query was real on the frame the
    /// press arrived" has to be read from the frame before.
    bool filterHadFocus = false;
    /// The rendered query was a real one at the end of the last frame; together
    /// with the buffer it lets the `Esc` clear tell "the widget reverted" from
    /// "the user typed nothing".
    bool filterNonEmptyLastFrame = false;

    // --- row latches --------------------------------------------------------
    /// The row under the pointer at the end of the last frame ("" when the
    /// pointer is off the rail): the first target of the row keys.
    std::string hoverRowId;
    /// The row that holds the keyboard-nav focus; dropped on the first frame no
    /// row owns it, so a single click cannot become a permanent target.
    std::string navRowId;
    /// The row whose menu popup is up, and the one-shot `m` request latched
    /// before the rows are drawn (consumed inside that row's own id scope, so a
    /// request for a row that is filtered or scrolled out is dropped).
    std::string menuRowId;
    std::string menuRequestId;
    /// Measured popup height (px): the menu flips above its row using this, so a
    /// popup at the bottom edge cannot cover the row it acts on.
    float menuHeightPx = 0.0f;

    // --- the inline rename box ---------------------------------------------
    std::string renameRowId;
    std::array<char, 256> renameBuf = {}; ///< the shipped 256-byte draft buffer
    bool renameFocus = false;             ///< focus the field on the next frame
    bool renameHadFocus = false;          ///< it held the keyboard last frame
    int renameSeq = 0;                    ///< fresh InputText state per open

    // --- the loading skeleton (design §4.11) --------------------------------
    /// The id of a Select that has been sent to D and not yet acknowledged. While it
    /// is set and the list is still empty, the rail draws its 3-row skeleton (design
    /// §4.11); a list that already has rows keeps drawing them, with the target row in
    /// the pending state, so a switch can never hide the sessions.
    std::string loadingId;
    /// Frames `loadingId` has been waiting. Bounded in `deriveRail()`: D's
    /// `activateSession` returns *without* sending a snapshot when the session file
    /// fails to load, so an unbounded wait would leave the skeleton (or the pending
    /// mark) up until something else produced a list.
    int loadingFrames = 0;
    /// The session a switch is deferred for, because a run was active when the row
    /// was clicked (the single "last click wins" slot, flushed on the first ready
    /// frame): its row draws the pending-switch state.
    std::string pendingId;

    // --- answers, applied and cleared by the frame layer --------------------
    std::string selectId;      ///< queue (or defer) a Select
    bool newRequested = false; ///< queue a New
    std::string renameId;      ///< a committed rename: queue `{Rename, id, renamedTitle}`
    std::string renamedTitle;
    std::string deleteId;        ///< delete this session (the row the user aimed at)
    std::string pinToggleId;     ///< flip this session's pin in `ShellPrefs::pinned`
    bool focusSearch = false;    ///< "focus search": also opens the rail when it is collapsed
    bool escapeConsumed = false; ///< the rail took this frame's `Esc` (D-ESC: one step per press)
    /// The rail is widened *for the search*: `openRailForSearch()` set `UiState::railW`
    /// because the collapsed shape has no field, and `searchPrevRailW` is the request it
    /// replaced. The search is a mode, so the width goes back when the mode ends - the
    /// query is empty and the field has let go of the keyboard, or the rail is closed.
    bool searchWide = false;
    float searchPrevRailW = 0.0f;
    /// The search ended this frame (the view's answer, applied by the frame layer). Kept
    /// apart from `searchWide` so the view can report the end without deciding where the
    /// previous width came from.
    bool searchEnded = false;

    // --- observations, for the harness and the log -------------------------
    /// The rail is the *panel* shape this frame (design §3.3's "single column, both overlays"),
    /// derived from the layout by the frame layer - the same pair the inspector carries as
    /// `open`/`overlay`, and for the same reason: `Esc`'s first step and every `acceptKeys` gate
    /// have to agree on "is a surface over the tiling", and neither may read the layout itself.
    bool overlay = false;
    /// The rail has a rectangle this frame (a column, the collapsed strip or the panel). The
    /// *visible* state, as opposed to the preference: `model::toggleRail()` decides from this, so
    /// the first `Ctrl+B` on a narrow display - where `-1 auto` resolves to a closed panel - opens
    /// the rail instead of writing a second "closed" (the inspector's rule, task 29).
    bool open = false;
    /// Rows the view *drew* this frame. Bounded by the viewport plus one row of
    /// overscan (D-RAIL) - the counter the 2000-session fixture asserts on.
    std::size_t drawnRows = 0;
    /// Lines per row this frame (1 or 2), as `views::railRowLines()` answered.
    int rowLines = 1;
};

/// One group's cached drawn height (design §4.4, "bounded rendering").
///
/// The key is `(groupId, revision, width, row, compact)`: a group id is never
/// reused, `revision` moves when the streaming group's text changes, and the
/// layout numbers describe the box the height was measured in - `row` is the
/// profile's resolved line height, which *is* the font scale (1 cell on the text
/// grid, the scaled line height in the pixel profile). A change in any of them
/// makes the number a statement about a different box, so it is dropped instead
/// of trusted.
///
/// A hint, never a clipper contract (C-03): a wrong hint costs a scroll jump, a
/// missing group costs content. `h` is the height the group actually used.
struct HeightHint {
    std::size_t groupId = 0;
    std::uint64_t revision = 0;
    float width = 0.0f;
    float row = 0.0f;
    bool compact = false;
    float h = 0.0f;
};

/// The thread's cross-frame view state (design §4.4, tasks 18/25).
///
/// Expansion is *not* here. It lives in the v4 sets the store keeps
/// (`ChatTab::outputLineOpen`, `ChatTab::openWorkGroups`) and the adapter exposes
/// them: one store with two readers. A second set keyed by group id would let a
/// click and a read disagree, which is what D-GROUPID exists to prevent (see
/// `UiState` for the same note).
///
/// What *is* here is what the window itself owns: the hint cache (dropped on a
/// width / row / compact / revision change and on a session switch), the scroll
/// anchor of a just-toggled group, and the two observations the harnesses read.
struct ThreadState {
    /// Drawn heights, keyed by group id. Pruned to the live groups on every group
    /// rebuild, so an evicted group's hint cannot outlive it.
    std::map<std::size_t, HeightHint> hints;

    /// The scroll anchor (task 25): which group's header must stay on the row the
    /// user clicked it on, and the screen row it sat on when the toggle happened.
    /// `anchorPending` is the one-shot "apply it on the next frame" latch; the
    /// frame that consumes it does not run the follow pin, so the anchor cannot
    /// fight the follower.
    std::size_t anchorGroupId = 0;
    float anchorScreenY = 0.0f;
    bool anchorPending = false;

    /// A one-shot "open the next thread at its newest content", in drawn frames.
    ///
    /// A session switch resets the thread, but the scrolling child that holds it
    /// outlives the session and keeps its `Scroll.y`; without this the new session
    /// would open at the offset the old one was left at (a long thread loaded after
    /// a short one opened at the top). The child's `ScrollMax` is one frame behind
    /// the content it measures, so the request has to survive two `renderThread()`
    /// frames: the first measures the new content, the second begins at the max it
    /// measured. Starts at 2: the first thread a fresh state draws also opens at its
    /// newest content (an app start does not go through a session switch), and a
    /// switch sets it back to 2.
    int scrollToBottom = 2;

    /// The window size the thread was drawn with last frame, in groups. `PgUp` /
    /// `Ctrl+U` reveal one page, and the page is this number: the frame layer
    /// consumes the key but does not own the layout (D-METRICS).
    int pageGroups = kThreadWindowGroupsGrid;

    /// Groups the thread actually drew last frame: the bounded-rendering counter a
    /// fixture can assert on, and the reason it can - drawing every group would
    /// show up here. It counts the live answer too while a stream runs (task 38): the live
    /// answer is drawn after the window, not inside it, so `drawn == window + 1` there.
    std::size_t drawnGroups = 0;

    /// The first group whose header row is fully inside the visible band, and that
    /// header's screen row - both recorded while the group was drawn. A *diagnostic*:
    /// only the anchor harness reads them (it clicks that row, which is where the group
    /// really is rather than where a scroll offset says it should be); no production
    /// view consumes them. `firstGroupY` stays in screen coordinates because the click
    /// is; the anchor's own rows are child-relative.
    std::size_t firstGroupId = 0;
    float firstGroupY = 0.0f;

    // --- the block focus (task 26, design §4.5) ------------------------------
    /// The block that owns the thread's keyboard focus, named by its stable
    /// `BlockId` (group, key, kind) - never by a position.
    ///
    /// A position would re-point silently the first time a block list gained a row in
    /// front of the focused one (a thinking disclosure appearing on the next stream
    /// update), and paging a window is exactly the kind of change that renumbers
    /// indexes. The identity is unique for as long as the block exists: group ids are
    /// monotonic and never reused (D-GROUPID), and within one group a key yields at
    /// most one row per kind. `focus.valid()` is false when no block is focused.
    /// Exactly one block is focused at a time.
    BlockId focus;

    /// The thinking disclosures the user opened, keyed by the block's identity
    /// (task 26). A stored thinking row is `{its group, its message id, Thinking}`;
    /// the stream's reasoning is `{the stream group, the reserved stream id,
    /// Thinking}` and is seeded open when the stream group is born.
    ///
    /// The store has no thinking expansion, and ImGui's tree storage is not a place to
    /// keep state (a windowed render cannot read it back). So this is plain data, and
    /// the adapter - the one place that maps a block to its state - reads and writes it,
    /// exactly as it reads the v4 sets for a message's own disclosure.
    std::set<BlockId> thinkingOpen;

    /// The blocks whose table source is showing (design §6.8, D-TABLE, task 37): `s` on a
    /// focused block flips its membership, and the transform writes the block's top-level
    /// tables as a `llmfun-table-source` fence instead of a `llmfun-table` one.
    ///
    /// Plain data for the same reason as `thinkingOpen`: the expansion is the *view's*, the
    /// block's identity is the adapter's, and neither may keep ImGui storage. Cleared with the
    /// rest of the thread state on a clear or a session switch (`reset()`), because group ids
    /// restart at 1 and a flag would otherwise land on the next session's block.
    std::set<BlockId> tableSource;

    /// The block whose menu is open (task 44), named by its `BlockId` - the same identity
    /// the focus uses, for the same reason.
    ///
    /// Why an owner at all: the popup's ImGui id is the block's id scope, and `BlockId`s are
    /// recycled by a transcript reset (group ids restart at 1), so an id alone would let a
    /// *new* message inherit an open menu and be copied under it. The latch is the menu's
    /// owner - only the block it names may submit the popup - and it is validated every
    /// drawn frame: a block that is no longer drawn (paged away, a state card in front of
    /// the thread), a menu ImGui has already closed, and a clear/session switch all drop
    /// it. That is the rail's `menuRowId` rule (`views/rail.cpp`), in thread state.
    BlockId menuBlockId;

    /// The live answer's clock, caret and cancel mark (design FR-08, task 38).
    ///
    /// The stream itself is the store's (`ChatTab::hasStreamMsg` + `streamMsg`, one copy the
    /// adapter reads), so this carries only what a clock can tell: how long the answer has
    /// run, how long the model reasoned, whether the user stopped it and which half of the
    /// caret's blink this frame falls in. `refreshStreamClock()` is the one writer, called
    /// by the frame layer once per frame and by a harness with its own clock - and
    /// `resetStreamClock()` is the lifecycle clear, so a new stream never inherits the
    /// previous answer's start time.
    StreamState stream;
};

/// The five words a lane's state is drawn with (design §5.21, D-BLOCK: a state is a
/// glyph *and* a word, never a colour alone). Plain strings, not an enum, because a lane's
/// state is what the row prints: the mirror writes one of these, the view draws it, and a
/// golden reads them back. A view never compares against a literal of its own.
///
/// `queued` is the design's fourth state and today's ABI cannot produce it: a lane is only known
/// to the UI once D sends its first `tuiPipelineAgentUpdate`, and that update *is* the agent
/// starting. It stays in the vocabulary - the row, its glyph and the golden's third lane pin it -
/// because the state is the design's, and a pipeline-start message (post-MVP) would otherwise
/// have to change a view, a test and a golden to reach a state they already draw.
inline constexpr char kAgentStateQueued[] = "queued";
inline constexpr char kAgentStateRunning[] = "running";
inline constexpr char kAgentStateDone[] = "done";
inline constexpr char kAgentStateFailed[] = "failed";
/// The user stopped watching this lane (task 30): the pipeline has no per-agent cancel
/// (`/stop` is the *run's* stop), so stopping one lane is the TUI's own - the lane freezes
/// where it was, and the run keeps going for the agents still on the list.
inline constexpr char kAgentStateStopped[] = "stopped";

/// The lane list's capacity, in lanes: the v4 panel's `ChatTabLeftPanel::MaxAgents`, which is
/// the pipeline's own policy (`plan/system_design.md` §"capacity: existing `MaxAgents = 16`,
/// FIFO (unchanged)"). It is a model constant because `model/` may not include `tui.h`;
/// `tui_api.cpp` pins the two together with a `static_assert`, so a change on either side is a
/// build failure rather than a lane list that disagrees with the panel it mirrors.
inline constexpr std::size_t kAgentLaneMax = 16;

/// One pipeline agent, as the inspector's Agents tab draws it (design §5.21, task 30).
///
/// A POD copy, never a view into the v4 store: `views/` may not name `AgentStream` or any
/// other v4 type (D-ADAPTER), so the C ABI's mirror (`tuiPipelineAgent*`) copies what the
/// lane needs into this struct and the tab reads it. The copy is the price of the layering,
/// and it buys the one property the tab needs: the lane is a value, so a frame that draws it
/// cannot be surprised by the pipeline changing underneath it mid-row.
///
/// Only the fields the row and the transcript need exist - no tool arguments, no diffs (a
/// tool card is P3, not the MVP), no message ids:
///
///   * `label` is the identity the row shows (`planner`, `coder`) and `role` the model role
///     D sent (`assistant`), because the design's row is `role/label`: one may be empty and
///     the row degrades to the other;
///   * `state` is one of the `kAgentState*` words, `active` is the same fact as a bool (the
///     two are written together - `active` is what the action row asks, `state` what it
///     prints);
///   * `elapsedMs` is measured from the lane's first sighting and stops at its last update,
///     so a finished lane keeps the duration it took instead of counting to now;
///   * `tokens` is the context count D reported for this agent (`PipelineChatMessage.contextUsed`,
///     sent with `contextMax`); 0 means there is no number to draw, and the row then omits it
///     rather than printing a zero it does not know;
///   * `currentTool` is the newest tool call of the agent's response
///     (`PipelineChatMessage.currentTool`), empty while the agent has called none;
///   * `content` is the agent's stream text - the transcript the drill-down shows. It is a
///     copy of the message, not a pointer into the store, for the same D-ADAPTER reason.
struct AgentLane {
    std::string id;
    std::string role;
    std::string label;
    std::string state = kAgentStateQueued;
    std::string currentTool;
    std::string content;
    long long elapsedMs = 0;
    long long tokens = 0;
    bool active = false;

    /// Bookkeeping for `elapsedMs`: the frame clock (ms) at the lane's first sighting and at
    /// its most recent update. Written by the mirror, read by `refreshAgentLanes()`.
    long long startedMs = 0;
    long long lastUpdateMs = 0;
};

/// The lane clock: unix milliseconds, the same epoch the frame layer's `frameClockMs()`
/// reads, so a lane's `startedMs` and the frame's `now` are comparable numbers.
inline long long agentClockMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

/// Everything the workbench shell and its views read.
///
/// One writer (the frame thread), read-only for views. The members are grouped
/// by owner: preferences (persisted), the per-frame derivation (`view`,
/// `offline`), and the thread's view state (`follow`, `earlierGroups`, `thread`).
///
/// Expansion is deliberately *not* here: it lives in one store, the v4 sets the
/// store keeps (`ChatTab::outputLineOpen` for a message,
/// `ChatTab::openWorkGroups` for a work group), reached through
/// `store::ThreadStoreAdapter::expanded()/setExpanded()`. A second copy here was
/// the "two sources of truth" D-GROUPID exists to avoid - a header click would
/// have had to update both. The restyle (task 26) may move the sets into this
/// struct; until then the adapter is the seam and this file stays free of them.
struct UiState {
    /// The user's preferences - the one preferences owner (settings.h), persisted
    /// in `ui.conf` and read back by every view that honours them. Auto (`-1`) is
    /// resolved where the display is known (`views::layoutPrefsFor` reads it as
    /// open, `views::shellLayoutFor` applies the width rule), so one preference
    /// cannot mean two things in two places.
    ShellPrefs prefs;

    /// The requested rail width, in the profile's own units (cells / px). 0 means
    /// "the profile's own width" - `ui::LayoutPrefs::railW`'s contract. Not a
    /// `ShellPrefs` member: `ui.conf` has no rail-width key (design §5.3), so a
    /// dragged width belongs to the session that dragged it.
    float railW = 0.0f;

    /// Focus mode (task 43, FR-28): the top bar, the rail and the inspector are hidden and the
    /// transcript is drawn in a centred column (`model::toggleFocusMode()` owns the transition).
    /// A mode of this process, never a preference: `ui.conf` has no key for it, and the side
    /// regions' own preferences are left alone so leaving the mode restores what the user had.
    bool focusMode = false;

    /// Which body the thread area shows. Derived once per frame by the frame
    /// layer through `deriveThreadView()`, so the region and the rest of the shell
    /// cannot disagree about what an empty thread, a queued switch or an outage looks
    /// like.
    ThreadView view = ThreadView::Empty;

    /// The model backend is unreachable. The workbench reads this copy only; the
    /// frame layer fills it from the run status once per frame (`TuiState::offline`, which
    /// task 39's `tuiSetRuntimeStatus` writes from phase 6 - the one fact the sentence cannot
    /// express - and which the harnesses write directly).
    bool offline = false;

    /// The Raw Log overlay is up (D-RAWLOG). It starts closed on every start and
    /// is never persisted open; the now bar's badge reads it before task 33
    /// gives the surface its body.
    bool rawLogOpen = false;

    /// The Raw Log's two view modes (task 33, D-RAWLOG). Both are per-session facts, not
    /// preferences: `ui.conf` carries the *level* (`ShellPrefs::rawLog`) and never the view
    /// state, so a restart opens at the newest record with the follower running.
    ///
    /// `rawLogFollow` is pointer-owned, like the thread's `follow` (D-FOLLOW): wheel or
    /// scrollbar input clears it, `f`/`End` re-arm it, and the surface pins only while content
    /// sits below the fold. `rawLogPaused` stops the pin without claiming the stream stopped.
    bool rawLogFollow = true;
    bool rawLogPaused = false;

    /// The drawer's height, in the profile's units, after the user dragged its grip (task 33).
    /// 0 is "the design's 30 %". Per-run like the rail's width: `ui.conf` has no key for a
    /// dragged height (design §5.3), and the layout clamps whatever the drag asked for.
    float rawLogH = 0.0f;

    /// The run's status as the now bar reads it (task 28): the phase, the current
    /// step, the clock and the numbers.
    ///
    /// One store, two producers, and `structured` says which one owns it: while it
    /// is false the frame layer fills it from the facts the frame owns (the phase
    /// table in `views/shell.cpp`), and once a structured status arrives (task 39's
    /// `tuiSetRuntimeStatus`, `tui.cpp`) the ABI writes the numbers D sends and the
    /// frame leaves them alone. Views read this member and never the sentence.
    RuntimeStatus status;

    /// The four status chips the row draws (`model::StatusChips`), formatted once per
    /// value change by whoever owns the status: the sentence splitter
    /// (`tuiSetStatusText`, task 6) or the structured status task 39's `tuiSetRuntimeStatus`
    /// formats when the update arrives. The frame layer only *projects* the sentence's set
    /// into this member (once per change, never per frame), so a view reads the aggregate
    /// instead of the v4 state - the values inside it are never reformatted on a render
    /// path (§6.3: the chips are canonical for the numbers).
    StatusChips chips;

    /// The feedback queue (design §5.25, FR-29, task 45): short-lived answers to actions whose
    /// outcome is not in the thread - a copy, a save, a settings write.
    ///
    /// `model::raiseToast()` is the only writer and it stamps the lifetime, so a producer writes
    /// the words and nothing else. The frame layer expires the transients once per frame
    /// (`model::expireToasts`, the injected clock) and the presentations read the newest item
    /// (`model::topToast`): the pixel profile's stack (`views/feedback.cpp`) and the text grid's
    /// one-line flash in the now bar. Depth is the profile's (`kToastMaxGui` / `kToastMaxGrid`),
    /// applied at raise time.
    ///
    /// Not persisted and never part of a session: a toast answers a press of *this* run, and one
    /// that outlived a restart would claim an action nobody made.
    std::deque<Toast> toasts;

    /// Follow the newest content while the user leaves the view at the bottom
    /// (D-FOLLOW). Pointer-owned: `ui::followNewest()` answers what this becomes
    /// next frame, so a wheel notch turns it off and `End` turns it on.
    bool follow = true;

    /// Extra groups the thread's `Load earlier` row has revealed beyond the
    /// bounded window (D-BOUNDS). Reset with the thread.
    int earlierGroups = 0;

    /// The thread's own view state: the height-hint cache, the scroll anchor of a
    /// just-toggled group and the bounded-rendering counters (task 25). The
    /// *store* (messages, group ids, expansion) stays behind the adapter - this is
    /// what survives a frame, not what defines the thread.
    ThreadState thread;

    /// The session rail: its search text, its row latches and its answers
    /// (task 23). The rail's *list* is derived per frame by
    /// `buildRailRows()` from the snapshot; only the state a user can change
    /// lives here.
    RailState rail;

    /// The UI-local unread set (D-UNREAD): `updatedAt` advanced while a session
    /// was not active, cleared on select, never persisted. It lives here rather
    /// than in `ShellPrefs` because it is a fact about this process run, not a
    /// preference - `ui.conf` has no key for it by design (D-19).
    UnreadTracker unread;

    /// The pipeline agents, one lane each, in arrival order (design §5.21, task 30).
    ///
    /// The C ABI's mirror (`tuiPipelineAgentUpdate/Done/Clear`) is the only writer, so the
    /// lanes exist whether or not the inspector is open - the drill-down draws a lane's
    /// transcript in the thread, which is not a panel. The capacity policy is the v4 store's,
    /// copied rather than invented: `kAgentLaneMax` lanes, the oldest `lastUpdateMs` evicted
    /// first (`tui_api.cpp` pins the two constants together), so the tab can never list an
    /// agent the store has already dropped.
    std::vector<AgentLane> agents;

    /// The lane whose transcript the thread shows (`""` = the main transcript). One value:
    /// the thread filters on it and the Agents tab marks the row it names (`▸`).
    std::string focusedAgent;

    /// The user-facing operational history the Activity tab draws (task 31, D-ACTIVITY).
    ///
    /// A *projection* of the log records the C ABI delivers - `model::appendActivity()` is
    /// its only writer, and it is the only place that decides what Activity may show. Bounded
    /// FIFO at `kActivityRecordsMax` (500); the raw records themselves live on the state
    /// (`TuiState::logRecords`), so this ring never becomes a second log store (R-08).
    std::deque<LogRecord> activity;

    /// Records that arrived while the Activity tab was not on screen (D-UNREAD's rule for
    /// the record stream). The tab strip's badge draws it, showing the tab clears it, and it
    /// is never persisted: how much of the history the user has read is a fact about this
    /// process run, not a preference.
    int unreadActivity = 0;
};

// ---------------------------------------------------------------------------
// Focus mode (task 43, FR-28)
// ---------------------------------------------------------------------------

/// Show or hide focus mode.
///
/// The chord (`F11`, and `Ctrl+Shift+M` in the pixel profile) and the palette's row both come
/// here, so "what a press means" has one home. The mode itself is `UiState::focusMode`, which the
/// layout turns into `LayoutPrefs::focusMode`: the top bar, the rail and the inspector are hidden
/// and the transcript moves into a centred column. Nothing is persisted and neither side region's
/// preference is touched, so leaving the mode restores the shell as the user had it.
void toggleFocusMode(UiState& ui);

/// Leave focus mode - the `Esc` chain's step (`model::Action::ExitFocusMode`).
///
/// One way on purpose: a chain step that toggled would turn "leave" into "enter" when two presses
/// overtook each other. The order the step sits in is `model::resolveEscape`'s. The caller already
/// knows the state it asked about (the resolver only answers this step while the mode is on), so it
/// answers nothing.
void exitFocusMode(UiState& ui);

/// Do the side regions' controls apply right now - the two toggles (`Ctrl+B`, `Ctrl+I`), the
/// inspector's tab controls (`Ctrl+5`/`Ctrl+6`, `Ctrl+1`/`Ctrl+2`) and their palette rows?
///
/// Focus mode hides the rail and the inspector (task 43), so a control there would write a
/// preference for a region nobody can see: the panel, its tab or its notice would appear the moment
/// the mode is left, and no press asked for that. One rule for every control that addresses a side
/// region, so the entry points cannot disagree about a surface that is off screen either way.
///
/// It answers "does the mode hide the region", not "is the region closed": a tab press on a panel
/// the user closed by hand must still open it.
bool regionControlsApply(const UiState& ui);

/// The lane named `id`, or nullptr - the one lookup the mirror and the frame share.
AgentLane* findAgentLane(UiState& ui, std::string_view id);
const AgentLane* findAgentLane(const UiState& ui, std::string_view id);

/// Forget every lane and the focus (`tuiPipelineClear`: the pipeline ended, or the session
/// was switched out from under it).
void clearAgentLanes(UiState& ui);

/// Stop one lane: the state becomes `stopped`, `active` false, and the focus drops when it
/// named this lane (a transcript of a lane that no longer runs would keep a dead view on
/// screen). Later updates for a stopped lane are ignored by the mirror - that is what makes
/// the stop stick - and the other lanes are untouched.
void stopAgentLane(UiState& ui, std::string_view id);

/// Focus a lane's transcript ("" = the main transcript). An id no lane carries is the main
/// transcript: a stale request cannot leave the thread showing a glyph-named nothing.
void focusAgentLane(UiState& ui, std::string_view id);

/// Settle the lanes against the run's own state, once per frame (the frame layer calls this;
/// a view never does):
///
///   * a lane still `running` while a stop has been requested is `stopped` - the user ended
///     the run, and every agent in it stopped with it;
///   * a lane still `running` when no run is in flight any more is `failed` - the runtime
///     never said it finished, and a lane that keeps claiming `running` beside a `Ready` top
///     bar is a lie the user cannot act on;
///   * a `done`/`failed`/`stopped` lane is never rewritten, so a settled lane keeps its
///     answer when the next run starts.
///
/// A stopped run and a failed one are told apart because the facts are: `cancelRequested` is
/// the user's own stop, and its absence is what makes an abandoned lane a failure.
///
/// The limit of that heuristic, stated so it is not read as a promise: `cancelRequested` is only
/// set by the *UI's* stop paths (`tuiRequestCancel` - the composer's Stop, `Esc`'s cancel step,
/// and the Agents tab's `Stop all`). A typed `/stop` reaches D as a slash command and never sets
/// this flag, so a run ended that way settles as `failed` - the TUI cannot tell it from a run
/// that died. Nothing here can fix that without a status the ABI does not have (task 39's
/// structured status is where it would arrive).
void settleAgentLanes(UiState& ui, bool runActive, bool cancelRequested);

/// Whether any lane is still live - what the action row (and its `Stop all`) exists for. One
/// predicate: the row that draws the control and the frame that drops a stale arming ask the
/// same question.
bool anyLiveAgentLane(const std::vector<AgentLane>& lanes);

/// Recompute `elapsedMs` for the lanes that are still live: `now - startedMs` while a lane
/// is active, the frozen `lastUpdateMs - startedMs` otherwise. Cheap (no allocation, one
/// subtraction per lane) and idempotent, so the frame layer can call it every frame.
void refreshAgentLanes(UiState& ui, std::int64_t nowMs);

// ---------------------------------------------------------------------------
// The mirror's derivations (design §5.21, task 30; fields since task 39a)
// ---------------------------------------------------------------------------
//
// The C ABI's `tuiPipelineAgentUpdate/Done` calls the two functions below - they are here,
// not in `tui_api.cpp`, because they are model answers (what a lane *is*) rather than a
// binding of the C surface, and because a headless test can drive them with no ABI in the way.
//
// Nothing here reads the agent's stream text (task 39a, D-STATUS: numbers, not sentences).
// The count and the tool name are fields on `PipelineChatMessage`, so a change to the way D
// formats its status sentence, or to the `--- Tool ---` layout
// `PipelineStreamMessageUpdater` writes, cannot silently zero or mislabel a row. The run-level
// sentence keeps its reader (the chips' `splitStatusText`); a *pipeline* sentence has none since
// task 41 removed the renderer that showed it.

/// Apply one `tuiPipelineAgentUpdate` to the lane list, creating the lane on first sight.
///
/// A lane the user stopped is left alone: that is what makes stopping one lane stick while
/// the run keeps going for the others (the pipeline has no per-agent cancel - `/stop` is the
/// run's stop - so the TUI's stop is the freeze of its own view of one agent).
///
/// The numbers are D's, not derived here: a `contextUsed` above 0 is the count the row draws,
/// and 0 - whether D means a real zero (`0/8192`) or has no reading at all (`0/0`) - draws no
/// number. The lane needs no presence bit of its own because nothing is latched: an update
/// cannot revive a count D stopped reporting. The tool name is stored as sent.
///
/// `PipelineChatMessage.contextMax` stops at the ABI seam: it is the producer's zero-vs-none
/// bit, and this row draws no number for either zero.
///
/// Cost, stated where a later task will look for it: every update copies the agent's whole
/// accumulated stream into `content` (the drill-down needs it, and a lane can be focused long
/// after its text arrived). The copy is O(n) per tick on top of what the v4 store already
/// copies, so a very long answer is O(n²) across the run - accepted for the MVP because a lane
/// list is at most `kAgentLaneMax` lanes and the text is bounded by one message. The fix, when
/// it is needed, is to keep the text for the focused lane only (or a shared, immutable
/// buffer), not to thin the fields.
void applyAgentUpdate(UiState& ui, std::string_view id, std::string_view role,
                      std::string_view content, std::string_view currentTool,
                      std::int64_t contextUsed, std::int64_t nowMs);

/// Apply one `tuiPipelineAgentDone`: the lane is `done`, it stops counting and keeps the
/// duration it took. A stopped lane stays stopped (the user's answer outranks the runtime's).
void finishAgentLane(UiState& ui, std::string_view id);

/// Reset the thread-scoped half of `ui` for a session switch: the `Load earlier`
/// paging window, the height-hint cache, the scroll anchor and the follow flag
/// belong to the thread that is being replaced, while the shell preferences belong
/// to the user and stay. (Expansion lives in the v4 store, so the adapter's own
/// `reset()` is the other half of a switch - one call each, from the same place.)
void resetForSessionSwitch(UiState& ui);

} // namespace llmfun::tui::model
