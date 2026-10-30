/// The shell (views/shell.h owns the contract and the anatomy).
#include "views/shell.h"

#include "imgui/imgui.h"

#include "model/session_rows.h"
#include "session_panel.h"
#include "store/thread_store.h"
#include "tui.h"
#include "tui_api.h"
#include "ui/input.h"
#include "ui/layout.h"
#include "ui/metrics.h"
#include "ui/primitives.h"
#include "ui/text.h"
#include "ui/tokens.h"
#include "views/composer.h"
#include "views/feedback.h"
#include "views/help.h"
#include "views/inspector.h"
#include "views/nowbar.h"
#include "views/palette.h"
#include "views/rail.h"
#include "views/rawlog.h"
#include "views/settings.h"
#include "views/thread.h"
#include "views/topbar.h"

#include "model/settings.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace llmfun::tui::views {
namespace {

/// The session the top bar describes: the snapshot row D marked active.
///
/// The id is authoritative (`ChatTabSessionPanel::activeId`); `SessionEntry::isActive` repeats
/// the same fact inside the row, so a snapshot carrying only one of the two still names the
/// session instead of losing its title. Both empty = no session yet (before the first
/// `tuiSetSessionList`), which the bar renders as "no title" rather than inventing one.
struct ActiveSession {
    std::string id;
    std::string title;
};

ActiveSession activeSession(const TuiState& state) {
    const ChatTabSessionPanel& panel = state.sessionPanel;
    for (const SessionEntry& entry : panel.sessions) {
        const bool active = panel.activeId.empty() ? entry.isActive : entry.id == panel.activeId;
        if (active)
            return ActiveSession{entry.id, entry.title};
    }
    return ActiveSession{};
}

/// The bar's run state (design §4.2), from what the frame owns today.
///
/// Task 39 replaces the body with the phase table (`RuntimeStatus::phase`); until then the
/// four states are exactly the facts the frame has: the backend-unavailable flag is `failed`,
/// a run with a stop already requested is `waiting` (the UI is waiting for it to wind down),
/// and a run without one is `running`.
views::RunState runStateOf(const TuiState& state) {
    if (state.ui.offline)
        return views::RunState::Failed;
    if (state.readyStatus)
        return views::RunState::Idle;
    if (state.cancelRequested)
        return views::RunState::Waiting;
    return views::RunState::Running;
}

/// How long the current run has been going, in milliseconds; 0 while none is.
///
/// `TuiState::startProcesssingTime` is the frame layer's run clock, and it is wall time -
/// so a clock jump can make one
/// frame's number smaller. That is harmless: `elapsedLabel()` clamps, and the pill is a clock,
/// not a timer.
std::int64_t runElapsedMs(const TuiState& state) {
    if (state.readyStatus)
        return 0;
    const auto elapsed = std::chrono::system_clock::now() - state.startProcesssingTime;
    const std::int64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    return ms > 0 ? ms : 0;
}

/// The phase for the *fallback* path: the facts the frame owns, used only while no structured
/// status has arrived (D-PHASE).
///
/// `RuntimeStatus::phase` arrives as an ordinal from D (task 39) and `deriveNowBar()` then stops
/// deriving a phase at all, so this function is what a state with no structured update draws -
/// and what it draws for the whole run if the sender is an older D.
///
/// From the frame's own facts it answers five of the seven phases honestly: the model is
/// unreachable (`Offline`), no run is in flight (`Idle`), a stop has been requested and the UI is
/// waiting for it to wind down (`Waiting`), and otherwise the run is working: `Writing` once the
/// answer's text has started to arrive, `Thinking` before that. Two are deliberately unreachable
/// rather than guessed:
///   * `Tooling` - nothing in the v4 store says that a tool call is the *current* step. The
///     nearest fact is "the newest stored message is a tool call", and a bar that says `tooling`
///     while the model is answering is worse than one that says `writing`.
///   * `Error` - a failed run simply ends (`readyStatus` again); the failure itself is in the
///     thread.
///
/// Once a structured status owns the row this function is dead for the run, and that is the point:
/// `Waiting` here meant "the user asked to stop", which D's vocabulary uses for a pending question.
/// The UI's own stop stays visible without it - the `Esc stop` hint, the composer's `Stopping`
/// latch and the top bar's running pill all read `cancelRequested` directly.
model::Phase phaseOf(const TuiState& state) {
    if (state.ui.offline)
        return model::Phase::Offline;
    if (state.readyStatus)
        return model::Phase::Idle;
    if (state.cancelRequested)
        return model::Phase::Waiting;
    // The answer's text has started to arrive. `hasStreamMsg` is the live-stream flag
    // (`ChatTab`), and the text is what separates a run that is writing from one that is
    // still thinking.
    const bool writing = state.chat.hasStreamMsg && !state.chat.streamMsg.text.empty();
    return writing ? model::Phase::Writing : model::Phase::Thinking;
}

/// The frame's clock, in unix milliseconds (defined below; declared here because the lane, the
/// stream and the feedback derivations in this file read it).
std::int64_t frameClockMs();

/// The status the now bar reads and the chips both it and the composer draw.
///
/// One writer (the frame thread), like every other derivation here, and one precedence
/// (design §4.7: "`RuntimeStatus` when available, else the legacy status sentence"):
///
///   * `ui.status.structured` - task 39's `tuiSetRuntimeStatus` fills the numbers, raises
///     the flag and formats the chips into `ui.chips` (`model::chipsFromStatus`, once per
///     status update); this function then leaves both alone. Formatting them *here* would
///     put four strings and two number formats on the render path of every frame, which is
///     the thing §6.3's "formatted once per value change" forbids;
///   * otherwise the provisional phase above and the chips `tuiSetStatusText` derived when
///     the sentence changed (task 6) - projected into `ui.chips` so a view reads the
///     aggregate, with the comparison keeping a settled set from churning its strings every
///     frame.
///
/// The clock is the frame's while a run is in flight, structured or not: D's `elapsedMs` is
/// sampled when a status is released, and a run with no tick (a slow first token, a tool call)
/// would otherwise freeze the row while the top bar keeps counting. D's value is the settled
/// duration, shown once the run is over.
///
/// The projection is written whether or not the row has a rectangle: compact mode can
/// fold the top bar away but never this row, and the composer's meter reads the same
/// chips.
views::NowBarInput deriveNowBar(TuiState& state, const ui::Metrics& m) {
    model::UiState& ui = state.ui;
    if (!ui.status.structured) {
        ui.status.phase = model::phaseOrdinal(phaseOf(state));
        // No step yet: nothing in today's frame says which tool is running, and the
        // structured status (task 39) is what will.
        ui.status.detail.clear();
        ui.status.elapsedMs = runElapsedMs(state);
        ui.status.tokensPerSecond = 0.0; // the sentence's chip stays the formatter's
        ui.status.contextUsed = state.statusChips.contextUsed;
        ui.status.contextMax = state.statusChips.contextMax;
        ui.status.compactAt = 0;

        // The sentence path's chips, with the two phases the sentence has no word for kept
        // from the phase: D's sentence still says `Ready` until the structured status
        // exists, and a `Ready` chip beside an `offline` row would contradict the row it
        // sits on. Every other phase keeps the sentence's word verbatim (task 6: a fragment
        // the splitter does not recognise is never dropped and never replaced).
        model::StatusChips chips = state.statusChips;
        const model::Phase phase = model::phaseFromOrdinal(ui.status.phase);
        if (phase == model::Phase::Offline || phase == model::Phase::Error)
            chips.state = model::phaseStateWord(phase);
        if (chips != ui.chips)
            ui.chips = chips;
    }

    views::NowBarInput in;
    in.phase = model::phaseFromOrdinal(ui.status.phase);
    in.detail = ui.status.detail;
    // D's `elapsedMs` is a sampled value: it only advances when a status is released, so a run
    // that has not produced a tick yet (a slow first token) or one that is executing a tool would
    // freeze the row's clock while the top bar keeps counting. While the run is in flight the
    // frame's own clock is the live one; D's value is the settled duration, shown when it ends.
    in.elapsedMs = state.readyStatus ? ui.status.elapsedMs : runElapsedMs(state);
    in.chips = ui.chips;
    in.rawLogOpen = ui.rawLogOpen;
    in.running = !state.readyStatus;
    // The `Esc stop` hint is drawn exactly while this press would *stop the run*: the chain's
    // earlier steps can spend it - an overlay closes, and in focus mode the mode is left (task 43)
    // - and a hint that outlives its step advertises a press that now does something else
    // (D-ESC's steps are one per press). The question is the resolver's, not the run's
    // (`model::escapeStopsRun`), so the two can never disagree.
    in.stopAvailable = model::escapeStopsRun({escStepClosesOverlay(state), cancelStepOpen(state),
                                              !state.userQuery.inputBuf.empty(), ui.focusMode});
    // Focus mode's exit is the step the press performs while the mode is on, so the row names it:
    // the terminal's way *in* was the palette's row, which is gone from view once the mode is up.
    in.focusMode = ui.focusMode;
    in.following = ui.follow;
    // The newest feedback item takes the row's flash slot - on the **text grid only**: the pixel
    // profile's presentation is the stack (`views/feedback.h`'s `nowBarFlashItem` is the one rule).
    // Two surfaces showing one item would be a duplicate (a copy in the row *and* a card), and the
    // row's `Ctrl+K commands` hint would be displaced for the item's whole lifetime. The quit
    // question below outranks the flash either way: a question needs an answer, a flash *is* one.
    // The row draws the item's *title* - the detail line is the pixel profile's card, because a
    // one-line row cannot hold two lines of a message.
    if (const model::Toast* toast = views::nowBarFlashItem(ui, m.textGrid)) {
        in.flash = toast->title;
        in.flashKind = toast->kind;
    }
    // The one open question the row can be asked is the quit confirmation (task 11). It
    // takes the hint slot and the row's middle, never the state or its chip: a question is
    // not a reason to stop saying what the run is doing. The short form is the same question
    // for a row that has no room for the framing - the row picks whichever still names the
    // key, because cutting the long one's tail would cut exactly that.
    if (state.quitConfirmArmed) {
        in.prompt = kQuitConfirmHint;
        in.promptShort = kQuitConfirmShort;
    }
    return in;
}

/// The top bar's inputs for this frame (`views/topbar.h` owns the fields).
///
/// The bar reads no aggregate: this is the one place the four run states, the clock and the
/// safety flags are derived, so a later source (the phase in task 39) changes one function
/// instead of a view.
void deriveTopBar(TuiState& state) {
    const ActiveSession active = activeSession(state);
    state.topBar.sessionId = active.id;
    // The snapshot's title, and only that: a committed rename is a request in flight, so it shows
    // up when D's next snapshot carries it - two frames after the commit at the actor's own
    // cadence, which is shorter than a blink. The bar holds no copy of a name D has not confirmed,
    // so it cannot show one that was refused (the alternative - a "pending title" override - needed
    // a reconciliation rule against the snapshot and had no answer for a rename D drops).
    state.topBar.sessionTitle = active.title;
    state.topBar.run = runStateOf(state);
    state.topBar.elapsedMs = runElapsedMs(state);
    state.topBar.rawLogOpen = state.ui.rawLogOpen;
    // The rail's `canQueue` rule: a rename is not queued while a run is active, because D
    // would refuse it.
    state.topBar.canRename = state.readyStatus;
    // Which presses are the bar's. Two reasons a frame's keys are not:
    //   * a surface owns the `Esc` chain's first step while it is up - the palette always, the
    //     inspector panel while it floats (`escStepClosesOverlay()`) - so `Esc` closes that
    //     first and the rename box second (D-ESC: one step per press);
    //   * the frame already latched an `Esc` that a view-local handler took (an ImGui
    //     popup/menu, `tui.cpp`'s `escapeConsumed`), and a press can only be spent once.
    // The bar gates the title's click on the same answer: an overlay must not lose the keyboard
    // to a click on the region underneath it.
    state.topBar.acceptKeys = !escStepClosesOverlay(state) && !state.escapeConsumed;
}

/// Apply what the bar answered this frame, and clear the answers so they cannot be applied
/// twice.
///
/// The rename goes through `queueSessionAction()` - the rail's own call - so both entry points
/// produce one `SessionAction{Rename, id, title}` and D sees one protocol (design §6.2, "one
/// registry entry"). The id was captured when the box opened (`TopBarState::renameId`), so a
/// session switch while the field was up cannot rename the session that replaced it.
void applyTopBarAnswers(TuiState& state) {
    if (state.topBar.escapeConsumed) {
        state.escapeConsumed = true;
        state.topBar.escapeConsumed = false;
    }
    // The bar's `Settings` entry (design §4.2: "gear opens Settings") is one of the sheet's two
    // entry points; the palette's `open settings` row is the other (task 35). It opens the sheet
    // on the frame the click landed, because the overlay layer re-lays out for a surface that
    // opened after the shell painted.
    if (state.topBar.settingsRequested) {
        state.topBar.settingsRequested = false;
        openSettings(state.settingsSheet);
    }
    if (!state.topBar.renameCommitted)
        return;
    const std::string id = state.topBar.renameId;
    const std::string title = state.topBar.renamedTitle;
    state.topBar.renameCommitted = false;
    state.topBar.renamedTitle.clear();
    state.topBar.renameId.clear();
    // The view already refuses an empty title and an empty target; both are checked again
    // here because this is the boundary the action crosses.
    if (id.empty() || title.empty())
        return;
    // One request, nothing more: the bar keeps drawing the snapshot's title, and D's next snapshot
    // is what changes it - or does not, if D refuses (the transcript carries the reason). No local
    // copy of the name survives this call.
    queueSessionAction(state.sessionPanel, SessionActionType::Rename, id, title);
}

/// The rail's inputs for this frame: the snapshot mapped onto `model::SessionRow`
/// (the only place `SessionEntry` and the rail's model meet), the unread set applied
/// to it, and the list grouped and filtered for the view.
///
/// The pending-switch bookkeeping runs here, not in the view, because it must run on
/// every frame - a closed rail (`Ctrl+B`) still has to flush the switch the user
/// queued before collapsing it, before the view's own early returns can skip it.
struct RailFrame {
    std::vector<model::SessionRow> rows;
    model::RailModel list;
};

/// Start a switch: the id that is in flight, and a *fresh* frame budget for it.
///
/// Both assignment sites go through here because the budget has to measure the wait it
/// bounds. `deriveRail()` counts frames per `loadingId`, and a second select while the
/// first is still unacknowledged would otherwise inherit the first wait's used frames -
/// so the mark (and the thread's loading card) for the second switch would expire early,
/// by an amount that depends on the user's last click instead of on the wait.
void beginRailLoad(model::RailState& rail, const std::string& id) {
    rail.loadingId = id;
    rail.loadingFrames = 0;
}

RailFrame deriveRail(TuiState& state, std::int64_t nowMs) {
    ChatTabSessionPanel& panel = state.sessionPanel;
    model::RailState& rail = state.ui.rail;

    // 1. The deferred switch: a row clicked while a run was active becomes an
    //    ordinary Select on the first ready frame (last click wins), which is what
    //    makes `pendingId` a state of *one frame* rather than a queue.
    if (!rail.pendingId.empty() && state.readyStatus) {
        if (rail.pendingId != panel.activeId) {
            queueSessionAction(panel, SessionActionType::Select, rail.pendingId, "");
            // Sent and not acknowledged: the skeleton's trigger (D-RAIL/§4.11).
            beginRailLoad(rail, rail.pendingId);
        }
        rail.pendingId.clear();
    }

    // 2. Acknowledge the skeleton: D's snapshot now names that session as active, or
    //    the session is gone (deleted, or the store rolled back). The wait is bounded
    //    as well, because `activateSession` in D returns *without* sending a snapshot
    //    when the selected file fails to load ("keep the current session unchanged"):
    //    the rail would otherwise show the skeleton - or the pending mark - until some
    //    other action produced a list.
    if (!rail.loadingId.empty()) {
        const bool present =
            std::any_of(panel.sessions.begin(), panel.sessions.end(),
                        [&rail](const SessionEntry& entry) { return entry.id == rail.loadingId; });
        constexpr int kLoadingFrameBudget = 120; // ~2 s at 60 Hz, ~10x D's slowest round trip
        if (panel.activeId == rail.loadingId || !present ||
            ++rail.loadingFrames > kLoadingFrameBudget) {
            rail.loadingId.clear();
            rail.loadingFrames = 0;
        }
    } else {
        rail.loadingFrames = 0;
    }

    // 3. The rows: the snapshot plus the three facts that are not in it - which row
    //    is active (the id is authoritative, `isActive` repeats it), the pin
    //    (a `ui.conf` preference) and unread (this process's own set).
    RailFrame frame;
    frame.rows.reserve(panel.sessions.size());
    for (const SessionEntry& entry : panel.sessions) {
        model::SessionRow row;
        row.id = entry.id;
        row.title = entry.title;
        row.preview = entry.preview;
        row.messageCount = entry.messageCount;
        row.updatedAtMs = entry.updatedAtMs;
        row.active = panel.activeId.empty() ? entry.isActive : entry.id == panel.activeId;
        row.running = (entry.flags & kSessionFlagRunning) != 0;
        row.pinned = state.ui.prefs.pinned.count(entry.id) != 0;
        frame.rows.push_back(std::move(row));
    }

    // 4. Unread (D-UNREAD) before the list is built: `apply` is the one writer, and
    //    a row's dot is read back from it so the rule cannot drift between the
    //    tracker and the row it marks.
    state.ui.unread.apply(frame.rows);
    for (model::SessionRow& row : frame.rows)
        row.unread = state.ui.unread.isUnread(row.id);

    // 5. The pin prune, once and only once (D-UI-CONF, `ShellPrefs::pinned`): a `Pin.<id>` line
    //    for a session the store no longer has would keep a dead id in the preference for ever.
    //    The moment to drop them is the first *complete* snapshot - pruning against a list that
    //    is still loading would delete the pins it has not seen yet - and this is the one place
    //    the frame holds that snapshot.
    if (!state.uiConfPinsPruned && !panel.sessions.empty()) {
        state.uiConfPinsPruned = true;
        for (auto it = state.ui.prefs.pinned.begin(); it != state.ui.prefs.pinned.end();) {
            const bool known =
                std::any_of(panel.sessions.begin(), panel.sessions.end(),
                            [&it](const SessionEntry& entry) { return entry.id == *it; });
            it = known ? std::next(it) : state.ui.prefs.pinned.erase(it);
        }
    }

    // 6. Group, order and filter (model/session_rows.h). Once per frame, not once
    //    per drawn row: the view walks the groups it is given.
    const std::string filter(rail.filterBuf.data());
    frame.list = model::buildRailRows(frame.rows, nowMs, filter);
    return frame;
}

/// Apply what the rail answered this frame, and clear the answers so they cannot be
/// applied twice.
///
/// Every session action goes through `queueSessionAction()` - the registry entry the top
/// bar's rename also uses - so the rail cannot reach D a second way (design §6.2: "one
/// registry entry"). The guards are the v4 panel's, kept: a select is queued when ready and
/// deferred when busy, while new/rename/delete are skipped while a run is active, because D
/// would refuse them.
///
/// `fullRailW` is the frame's own profile width (`Metrics::railW`, the width the rail
/// has where the display can hold it): the one answer that re-tiles the frame - "show
/// me the search" - needs it, and taking it from the caller keeps the profile lookup in
/// the frame layer instead of in this rule.
void applyRailAnswers(TuiState& state, float fullRailW) {
    model::RailState& rail = state.ui.rail;
    if (rail.escapeConsumed) {
        // The press the rail spent (a closed box, a cleared search): the frame's `Esc`
        // chain stands down for it (D-ESC).
        state.escapeConsumed = true;
        rail.escapeConsumed = false;
    }

    const bool canQueue = state.readyStatus;
    if (!rail.selectId.empty()) {
        const std::string id = rail.selectId;
        rail.selectId.clear();
        if (id != state.sessionPanel.activeId) {
            if (canQueue) {
                queueSessionAction(state.sessionPanel, SessionActionType::Select, id, "");
                beginRailLoad(rail, id); // sent, not acknowledged: the skeleton is up
            } else {
                rail.pendingId = id; // busy: the single deferred slot, last wins
            }
        }
        // The user is looking at it: its dot goes now rather than one frame later.
        state.ui.unread.clear(id);
    }

    if (rail.newRequested) {
        rail.newRequested = false;
        if (canQueue)
            queueSessionAction(state.sessionPanel, SessionActionType::New, "", "");
    }

    if (!rail.renameId.empty()) {
        const std::string id = rail.renameId;
        const std::string title = rail.renamedTitle;
        rail.renameId.clear();
        rail.renamedTitle.clear();
        if (canQueue && !id.empty() && !title.empty())
            queueSessionAction(state.sessionPanel, SessionActionType::Rename, id, title);
    }

    if (!rail.deleteId.empty()) {
        const std::string id = rail.deleteId;
        rail.deleteId.clear();
        if (canQueue && !id.empty())
            queueSessionAction(state.sessionPanel, SessionActionType::Delete, id, "");
    }

    if (!rail.pinToggleId.empty()) {
        const std::string id = rail.pinToggleId;
        rail.pinToggleId.clear();
        // Pins are a preference (`ui.conf`, task 22), not session data: the toggle edits
        // the one owner, `ShellPrefs::pinned`. The file itself is the frame layer's business
        // (`views::uiConfFrame()` / `uiConfQuit()`, task 29): a pin reaches the disk with the
        // next debounced write or on quit, and a pin the store no longer has is pruned once, by
        // `deriveRail()`'s first complete snapshot.
        const auto erased = state.ui.prefs.pinned.erase(id);
        if (erased == 0 && !id.empty())
            state.ui.prefs.pinned.insert(id);
    }

    if (rail.searchEnded || (rail.searchWide && state.ui.prefs.railOpen == 0)) {
        // The search is over (the view's answer), or the rail it widened was closed:
        // either way the width request goes back, so a display narrow enough to prefer
        // the collapsed column returns to it instead of staying widened - and
        // `Ctrl+B` twice gets the strip back rather than the widened column.
        closeRailSearch(state.ui);
        rail.searchEnded = false;
    }

    if (rail.focusSearch) {
        // "Focus search" also means "show me the field": a request from the collapsed
        // strip (or the palette) has no field to focus there, and the strip's shape is
        // the width rule, so the answer is a width request - `openRailForSearch()` owns
        // the whole rule (open if closed, widen to the profile's rail).
        openRailForSearch(state.ui, fullRailW);
        rail.filterFocus = true;
        rail.focusSearch = false;
    }
}

/// The frame's clock, in unix milliseconds: the one read the rail's grouping and its
/// time labels share, so a row cannot be grouped by one instant and labelled by
/// another.
std::int64_t frameClockMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

/// The composer's inputs for this frame (`views/composer.h` owns the fields).
///
/// The numbers come from the *projection* (`UiState::chips`) rather than from the
/// sentence: the mini meter and the canonical `ctx` chip are then the same value by
/// construction (`StatusChips::contextUsed/contextMax` are the numbers the chip's string
/// was formatted from - task 6's splitter keeps them for exactly this reason), and the
/// structured status (task 39) reaches this row without a second switchover.
///
/// Nothing here is invented. `mode` is empty because the MVP has no Ask/Plan/Code
/// switch (the composer draws the model alone), `compactAt` is 0 because the sentence
/// carries no compaction threshold before structured status (task 39), and `phase` is
/// the phase *word* (task 28's vocabulary), so the meta row says `writing` where it used
/// to say `Busy` - one state, one word, whichever row shows it.
views::ComposerInput composerInputFor(TuiState& state, std::int64_t nowMs) {
    views::ComposerInput in;
    in.buffer = &state.userQuery.inputBuf;
    in.history = &state.userQuery.inputHistory;
    in.historyPos = &state.userQuery.historyPos;
    in.draft = &state.userQuery.draftBuf;
    in.handoff = &state.userQuery.newInputBufString;
    in.busy = !state.readyStatus;
    in.stopping = state.cancelRequested;
    in.offline = state.offline;
    in.contextUsed = state.ui.chips.contextUsed;
    in.contextMax = state.ui.chips.contextMax;
    in.contextKnown = state.ui.chips.context != model::kMissingChipValue;
    in.model = state.ui.chips.model;
    in.phase = model::phaseWord(model::phaseFromOrdinal(state.ui.status.phase));
    in.speed = state.ui.chips.speed;
    in.sendKey = state.ui.prefs.sendKey;
    in.acceptKeys = !escStepClosesOverlay(state) && !state.escapeConsumed && !railOwnsEscape(state);
    // `overlayOpen` makes the composer's own `Esc` resolve to `CloseOverlay`, which it does not
    // perform: the frame's step does (D-ESC's first step). It has to be the *same* predicate
    // the frame uses, or a floating inspector would let the field clear the draft or cancel the
    // run while the press was meant for the panel. A press the *rail* owns is not an overlay's:
    // the rail closes its own box or clears its own query (railOwnsEscape), so this stays false
    // and the field simply does not read the key.
    in.overlayOpen = escStepClosesOverlay(state);
    // Focus mode (task 43) is not an overlay - the composer keeps the keyboard there - but its
    // `Esc` step is the frame's, so the field has to know the chain will not answer with one of
    // *its* steps. One predicate, read from the same state the frame dispatches from.
    in.focusMode = state.ui.focusMode;
    in.nowMs = nowMs;
    in.state = &state.composer;
    return in;
}

/// Apply what the composer answered this frame, and clear the answers so they cannot be
/// applied twice.
///
/// The send keeps the shipped handshake - the same three steps the v4 composer ran (drop
/// NULs, drop one trailing newline, refuse whitespace-only, push history, clear the
/// draft and the history cursor) - through `views::prepareSubmit()`, so a draft sends
/// exactly what it used to.
void applyComposerAnswers(TuiState& state) {
    views::ComposerState& composer = state.composer;
    if (composer.escapeConsumed) {
        // The press the composer spent (a clear, a blur, the paste question): the
        // frame-level `Esc` chain stands down for it (D-ESC: one step per press).
        state.escapeConsumed = true;
        composer.escapeConsumed = false;
    }
    if (composer.cancelRequested) {
        composer.cancelRequested = false;
        // D-CANCEL: the same request `/stop` makes, which the D loop turns into
        // `stopAgent()`; the partial output is kept.
        tuiRequestCancel(state);
    }
    if (composer.openPalette) {
        // The `/` on an empty line (design §4.6): the palette's command entry. The composer took
        // the character instead of inserting it, so it is handed over as the palette's *seed* -
        // typing `continue` then gives `/continue`, and Enter submits a command line rather than
        // the word behind the slash (plan/key_gate.md §10: the character is the entry point, and
        // it has to survive it). The field keeps its draft; the palette takes the keyboard while
        // it is up.
        composer.openPalette = false;
        views::openPaletteForCommands(state.palette, state, "/");
    }
    if (!composer.submit)
        return;
    composer.submit = false;
    std::string query;
    const bool hasQuery = views::prepareSubmit(state.userQuery.inputBuf, query);
    // A typed `/rawlog` is a command, not a query: it is never submitted and never enters the
    // history, exactly like D's typed `/stop` intercept (`source/llm/tui/package.d`). The
    // intercept lives in the submit path because that is the one place the query text exists -
    // the composer's own answer is "the user pressed Send", and it may not read the draft.
    if (hasQuery && handleRawLogCommand(state, query))
        return;
    if (hasQuery) {
        state.userQuery.submitReady = true;
        state.userQuery.submitQuery = query;
        views::pushHistory(state, query);
    }
    state.userQuery.inputBuf.clear();
    state.userQuery.historyPos = -1;
    composer.paste.dismiss();
    composer.reloadCaret = std::string::npos;
}

/// The inspector's inputs for this frame (`views/inspector.h` owns the fields).
///
/// The frame owns the four derived facts, because they are answers of the layout or of the
/// record stream rather than of the surface that draws them (D-METRICS: one owner for the
/// geometry, one writer for the state):
///
///   * *is the region up* and *is it the floating shape* - `ui::computeShell()`'s rectangles,
///     including the `-1 auto` rule `shellLayoutFor()` applied;
///   * *which tab is showing* - `ShellPrefs::tab`, the persisted preference, is the owner. The
///     surface asks for another one and `applyInspectorAnswers()` writes it, so the drawn tab
///     and the saved tab are never two values;
///   * *how many records arrived unread* - the Activity projection's own count (task 31,
///     D-UNREAD: a record that arrives while the tab is not on screen is unread, the count
///     clears the moment the tab is shown, and it is never persisted).
///
/// The unread count is `UiState::unreadActivity`, incremented by `model::appendActivity()` when
/// a record arrives while the tab is not on screen: a *counter*, not the ring's size, because
/// the ring is bounded and "the list got shorter" cannot tell a new record from an eviction. The
/// frame is still the one writer of the badge the surface draws and the one place the count is
/// read to zero, so the rule and its home stay here.
void deriveInspector(TuiState& state, const ui::ShellLayout& layout) {
    views::InspectorState& inspector = state.inspector;
    // Was the Activity tab on screen at the end of the last frame? The *transition* into it
    // is what owes one scroll to the newest record (quiet by default: nothing else moves the
    // list), so the two old answers are read before this frame's derivation overwrites them.
    const bool wasActivity = inspector.open && inspector.tab == model::InspectorTab::Activity;
    inspector.open = layout.inspector.w > 0.0f && layout.inspector.h > 0.0f;
    inspector.overlay = inspector.open && layout.inspectorOverlay;
    inspector.tab = state.ui.prefs.tab;
    // The drill-down is the thread's mode, so its owner is `UiState::focusedAgent`: the panel
    // only *marks* the row it names. Derived here with the rest of the panel's inputs because
    // the lane rows are drawn from it.
    inspector.focusedLane = state.ui.focusedAgent;

    // An armed `Stop all` question belongs to the lanes that were live when it was asked. The
    // action row (the question and its `No`) is drawn only while some lane is live, so if the
    // run ends by itself - or every lane stops - the latch would outlive the control that can
    // clear it, and the *next* run's action row would be born asking a question nobody pressed:
    // one press would then stop it for real. Dropping it here keeps "two presses" true.
    if (inspector.stopAllArmed && !model::anyLiveAgentLane(state.ui.agents))
        inspector.stopAllArmed = false;

    // A notice about a key nobody can act on any more is noise - but a notice pressed while the
    // panel was closed *is* the answer to that press, so it is not cleared here: the next tab
    // selection, the toggle and the overlay's close clear it (`applyInspectorAnswers()`,
    // `Action::ToggleInspector`, `closeTopOverlay()`).

    const bool activityOnScreen = views::activityVisible(inspector);
    // Showing the tab *is* reading it: the projection's count is cleared the moment the tab is
    // on screen, and one scroll to the newest record is owed by the transition (never by a
    // frame: quiet by default, task 31 / D-FOLLOW).
    if (activityOnScreen) {
        if (!wasActivity)
            inspector.activityScrollToEnd = true;
        model::clearActivityUnread(state.ui);
        inspector.unreadActivity = 0;
    } else {
        inspector.unreadActivity = state.ui.unreadActivity;
    }
}

/// The lane list's once-per-frame derivation (design §5.21, task 30).
///
/// Two facts, both owned by the frame rather than by a view (D-METRICS: one owner, one
/// writer): a lane that is still `running` when no run is in flight has to settle into
/// `failed`/`stopped`, and every live lane's clock has to advance. Both are cheap - no
/// allocation, one pass over at most `MaxAgents` lanes - and both run whether or not the
/// panel is up, because the thread's drill-down draws a lane too.
void deriveAgents(TuiState& state, std::int64_t nowMs) {
    model::settleAgentLanes(state.ui, /*runActive=*/!state.readyStatus,
                            /*cancelRequested=*/state.cancelRequested);
    model::refreshAgentLanes(state.ui, nowMs);
}

/// The live answer's clock (design FR-08, task 38).
///
/// Derived once per frame and by the frame alone, for the same reason the lanes' clocks are:
/// the facts are the store's (`hasStreamMsg`, whether the streamed message already carries
/// prose or reasoning), this frame's reading of the wall clock and the user's stop. Views read
/// `UiState::thread.stream` and never ask for a clock of their own, which is what keeps the
/// live row's `00:12`, the now bar's clock and the caret's blink phase the *same* numbers on
/// screen instead of three approximations of one moment.
///
/// Runs whether or not the thread has a rectangle (compact mode never folds the thread away,
/// but a surface may) and before anything draws, because the now bar's inputs are derived
/// after it.
void deriveStream(TuiState& state, std::int64_t nowMs) {
    model::refreshStreamClock(state.ui.thread.stream, /*hasStream=*/state.chat.hasStreamMsg,
                              /*hasText=*/!state.chat.streamMsg.text.empty(),
                              /*hasThinking=*/!state.chat.streamMsg.thinking.empty(), nowMs,
                              state.cancelRequested);
}

/// Apply what the inspector asked for this frame, and clear the request.
///
/// One place turns "the surface wants this tab" into a preference, so the preference has one
/// writer: a click on a tab, `[`/`]` and `Ctrl+5`/`Ctrl+6` all arrive here as the same request.
/// A tab press also *shows* the panel - selecting a tab nothing displays would be a press that
/// does nothing - and because it is the user's own action it writes `1`, never back to
/// `-1 auto` (the rule `Ctrl+B` follows for the rail).
///
/// A request is dropped here, not at the entry points, while focus mode hides the panel: the mode's
/// contract is that the side regions' preferences are left alone, and a fourth entry point cannot
/// bypass a rule that lives at the writer (`model::regionControlsApply`).
void applyInspectorAnswers(TuiState& state) {
    views::InspectorState& inspector = state.inspector;

    // --- the Agents tab (task 30) ------------------------------------------
    //
    // The drill-down, the lane stop and the run stop are three different things and they are
    // applied in one place, so a click, a menu row and the confirmation all end up as the same
    // kind of answer (D-METRICS: the surface asks, the frame writes).
    if (inspector.backRequested) {
        inspector.backRequested = false;
        model::focusAgentLane(state.ui, "");
    }
    if (!inspector.focusRequest.empty()) {
        model::focusAgentLane(state.ui, inspector.focusRequest);
        inspector.focusRequest.clear();
    }
    if (!inspector.stopLaneId.empty()) {
        model::stopAgentLane(state.ui, inspector.stopLaneId);
        inspector.stopLaneId.clear();
    }
    if (inspector.stopAllRequested) {
        inspector.stopAllRequested = false;
        inspector.stopAllArmed = false;
        // Every lane stops with the run - which is the run's own stop path (D-CANCEL), so a
        // `Stop all` from the Agents tab winds down exactly like `/stop`, the composer's Stop
        // and the `Esc` step: one implementation, and the partial output is preserved the same
        // way. The lanes are settled here as well, because the mirror stops updating them the
        // moment D's frame loop takes the cancel - through the model's own lane stop, so a lane
        // that stops this way freezes exactly like one stopped from its menu.
        for (model::AgentLane& lane : state.ui.agents) {
            if (lane.active)
                model::stopAgentLane(state.ui, lane.id);
        }
        tuiRequestCancel(state);
    }

    if (!inspector.tabRequestPending)
        return;
    inspector.tabRequestPending = false;
    // Focus mode hides the panel, so the request has no control to act on: writing the tab or the
    // open flag would change what the user sees when the mode is left, which no press in the mode
    // asked for (the same rule the toggles follow, `model::regionControlsApply`).
    if (!model::regionControlsApply(state.ui))
        return;
    state.ui.prefs.tab = inspector.tabRequested;
    inspector.reservedPressed = false;
    if (!inspector.open)
        state.ui.prefs.inspectorOpen = 1;
}

/// The settings sheet's inputs for this frame (design §4.10, task 34).
///
/// The facts only the frame knows, because a view may read none of them (D-METRICS): the profile's
/// two file paths (`ui.conf` beside the ini file, `imgui2.conf` derived from its name), the UI log
/// archive's switch, the ABI and format versions, the run's flags - and the display size, which the
/// sheet reports as the "size" of the window it is drawn in. They are derived *only while the sheet
/// is up* - a closed sheet costs nothing - and while it is up they are rebuilt each frame: a
/// handful of short concatenations in a modal surface the user opened, which is not the always-on
/// render path §6.3's "no formatting" rule is about (the status chips' strings are rejected because
/// *every* frame of every session pays for them; here the user is looking at the frame).
void deriveSettings(TuiState& state, ImVec2 display) {
    views::SettingsState& sheet = state.settingsSheet;
    if (!sheet.open)
        return;
    sheet.uiConfPath = model::uiConfPath(state.iniFilename);
    sheet.iniConfPath = settingsPath(state.iniFilename);
    sheet.logArchive = state.isLogActive;
    sheet.versions = "tui api " + std::to_string(TUI_API_VERSION) + ", ui.conf v" +
                     std::to_string(model::kUiConfVersion);
    sheet.flags = "offline=" + std::string(state.offline ? "1" : "0") +
                  " run=" + std::string(state.readyStatus ? "0" : "1") +
                  " stop=" + std::string(state.cancelRequested ? "1" : "0") +
                  " rawlog=" + std::string(state.ui.rawLogOpen ? "1" : "0");
    // The display the layout was computed from, in the profile's units: the same number every
    // region was placed with, so the copy describes the window that is on screen.
    sheet.displayW = static_cast<int>(display.x);
    sheet.displayH = static_cast<int>(display.y);
}

/// Apply what the sheet answered this frame, and clear the answers.
///
/// Two answers travel through the frame because the view cannot perform them: a *close* hands the
/// keyboard back (`returnKeyboardBelow()`), and a committed font scale is written to `imgui2.conf`
/// here and only here: the view knows that the value changed, but the profile name is the frame's.
/// One write per commit, never one per frame (`saveTuiSettings`).
void applySettingsAnswers(TuiState& state) {
    views::SettingsState& sheet = state.settingsSheet;
    if (sheet.fontScaleChanged) {
        sheet.fontScaleChanged = false;
        saveTuiSettings(state.settings, state.iniFilename);
    }
    if (sheet.closeRequested) {
        sheet.closeRequested = false;
        closeSettings(state.settingsSheet);
        returnKeyboardBelow(state);
    }
}

/// The Raw Log's inputs for this frame (design §4.9, D-RAWLOG, task 33).
///
/// Four facts, none of them the surface's own invention:
///
///   * *is it up* - `UiState::rawLogOpen`, the flag every access path writes and the now bar's
///     badge reads. It is never persisted, so every start is closed (D-RAWLOG);
///   * *which shape* - the text grid's full-screen overlay, or the pixel profile's drawer. The
///     profile is read from the metrics the frame already has (one place, D-METRICS), and the
///     shape decides what the surface may claim: only the overlay is modal, only the overlay
///     takes `Esc`;
///   * *the two view modes* - follow (pointer-owned, D-FOLLOW) and pause;
///   * *the level floor* - `ShellPrefs::rawLog` names the level the surface starts at. `off` is
///     the shipped default: the surface is closed on every start and shows what a normal run
///     logs; `debug`/`trace` widen the floor to the developer records the borrowed logger
///     level brings in.
///
/// The ring itself is not touched here: the frame hands the view a borrowed view of the one
/// store (`rawLogRing()`), which is what keeps R-08 true.
void deriveRawLog(TuiState& state, const ui::Metrics& m) {
    views::RawLogState& raw = state.rawLog;
    raw.open = state.ui.rawLogOpen;
    raw.overlayShape = raw.open && m.textGrid;
    raw.follow = state.ui.rawLogFollow;
    raw.paused = state.ui.rawLogPaused;
    raw.minLevel = model::rawLogFloor(state.ui.prefs.rawLog);
}

/// The ring, as the surface reads it: a borrowed view of the one raw store plus its dropped
/// counter. No copy - 5000 records of owned strings per frame would be the second store R-08
/// forbids, not to speak of the cost.
model::RawLogRing rawLogRing(const TuiState& state) {
    // Three facts: the ring itself (borrowed), the dropped counter and the accept counter. The
    // last one is what tells the surface's cached index that a record arrived - at capacity the
    // size does not move, and a burst can stamp several records with the same millisecond.
    return model::RawLogRing{&state.logRecords, state.droppedRecords, state.logRecordSeq};
}

/// Apply what the Raw Log answered this frame, and clear the answers.
///
/// The surface never writes the aggregate (D-METRICS: a view is a geometry taker), so the four
/// answers land here: a close, a pause toggle, a re-armed follower and the pointer clearing it.
/// The answer to a `Copy` or a `Save` travels the same way - the surface performed the write (it is
/// a model call and a clipboard call, neither of which needs the frame), and the *notice* it leaves
/// is the frame's to publish as one feedback item (task 45: the now bar's flash in the grid, a
/// toast card in the pixel profile), which is where the design says the path is reported.
void applyRawLogAnswers(TuiState& state, const ui::Metrics& m) {
    views::RawLogState& raw = state.rawLog;
    if (raw.closeRequested) {
        raw.closeRequested = false;
        closeRawLog(state);
    }
    if (raw.pauseRequested) {
        raw.pauseRequested = false;
        state.ui.rawLogPaused = !state.ui.rawLogPaused;
    }
    if (raw.followRequested) {
        raw.followRequested = false;
        state.ui.rawLogFollow = true;
        // `f` on a paused surface is "show me the newest": resuming is what the user asked for.
        state.ui.rawLogPaused = false;
    }
    if (raw.followCleared) {
        raw.followCleared = false;
        state.ui.rawLogFollow = false;
    }
    if (raw.resizeTo > 0.0f) {
        // The drawer's grip: the height the drag asked for, which the *layout* clamps into the
        // band it can afford (a view never re-tiles by itself, D-METRICS). Per-run only: `ui.conf`
        // has no key for a dragged height, like the rail's width.
        state.ui.rawLogH = raw.resizeTo;
        raw.resizeTo = 0.0f;
    }
    if (!raw.notice.empty()) {
        // The surface's one-line answer (a saved path, a failed save, a copy it made) becomes a
        // feedback item: the *one* place it is published (task 45). Nothing duplicates it - the now
        // bar's flash and the pixel profile's stack both read this queue, so the two presentations
        // cannot say different things about one press.
        notifyRawLog(state, raw.notice, raw.noticeKind, m);
        raw.notice.clear();
        raw.noticeKind = model::ToastKind::Info;
    }
}

} // namespace

void pushHistory(TuiState& state, const std::string& line) {
    // One bound for every submit path (the composer's, the palette's command rows, the ASK row).
    // The v4 composer bounded its recall helper the same way.
    state.userQuery.inputHistory.push_back(line);
    if (state.userQuery.inputHistory.size() > UserQueryState::MAX_HISTORY)
        state.userQuery.inputHistory.erase(state.userQuery.inputHistory.begin());
}

void returnKeyboardBelow(TuiState& state) {
    // A surface that is still up owns the keyboard, and it takes it back itself: the Raw Log's
    // overlay re-focuses on its next frame, and the settings sheet is modal, so handing the
    // composer a focus request behind it would type into a field nobody can see (review finding:
    // `Ctrl+K` above the sheet, then `Esc`, used to do exactly that). A sheet that is up re-arms
    // its own search field instead.
    if (state.palette.open || (state.ui.rawLogOpen && state.rawLog.overlayShape))
        return;
    if (state.settingsSheet.open) {
        state.settingsSheet.searchFocus = true;
        return;
    }
    // The help page (task 42) is a modal surface as well, and the palette's `Show help` row closes
    // the palette *after* opening it - so this call runs with the page already up. The page has no
    // field to re-arm; it asserts its own window focus every frame, and a `focusRequested` written
    // here would be a request to type into a field behind it (and it dirties
    // `userQuery.isSubmitted` for whoever reads that latch next).
    if (state.help.open)
        return;
    state.userQuery.isSubmitted = true;
    state.composer.focusRequested = true;
}

void openRailForSearch(model::UiState& ui, float fullW) {
    // The user's own action, so it writes an explicit 1 - never back to `-1 auto`, and never
    // leaving an automatic rail automatic: below the rail's breakpoint `-1 auto` closes the
    // *panel* shape (`shellLayoutFor`), so an auto rail that stayed auto would answer `/` with no
    // field to type in - the collapsed strip has none either.
    if (ui.prefs.railOpen != 1)
        ui.prefs.railOpen = 1;
    // The request this one replaces, kept so the search can hand the rail back the width
    // it had (`closeRailSearch()`): the first `/` wins the "before", a second one while
    // the field is up keeps it.
    if (!ui.rail.searchWide) {
        ui.rail.searchWide = true;
        ui.rail.searchPrevRailW = ui.railW;
    }
    // `railW`'s 0 is "the profile's own width", which the width breakpoint answers with
    // the *collapsed* column below the wide breakpoint - so "no request" and "the full
    // rail" are the same number at two different sizes, and asking for the full width
    // means writing it. A wider request the user made is kept, and a non-finite or zero
    // one writes nothing (`railRequest()` drops those anyway).
    if (ui.railW < fullW)
        ui.railW = fullW;
}

void closeRailSearch(model::UiState& ui) {
    if (!ui.rail.searchWide)
        return;
    ui.rail.searchWide = false;
    ui.railW = ui.rail.searchPrevRailW;
    ui.rail.searchPrevRailW = 0.0f;
}

// ---------------------------------------------------------------------------
// The Raw Log's three entry points (task 33)
// ---------------------------------------------------------------------------

void notifyRawLog(TuiState& state, std::string text, model::ToastKind kind, const ui::Metrics& m) {
    model::raiseToast(state.ui, model::Toast{kind, std::move(text), {}}, frameClockMs(),
                      m.textGrid);
}

void closeRawLog(TuiState& state) {
    // Closing never touches the ring and never touches the archive: the surface is a view
    // (R-08), and D's level restore is driven by `tuiIsRawLogOpen` on its next frame.
    state.ui.rawLogOpen = false;
    state.rawLog.open = false;
    state.rawLog.overlayShape = false;
    state.rawLog.closeRequested = false;
    state.rawLog.filterFocus = false;
    // The next open is a fresh look at the stream, not a resumed query: the filter is cleared and
    // its widget id moves on, so the field re-reads the empty buffer instead of ImGui's stale edit
    // state (the rail's `filterSeq` rule). The drawer's dragged height goes back to the design's
    // 30 % for the same reason - a mode the user left is not a preference.
    state.rawLog.filter.fill(0);
    ++state.rawLog.filterSeq;
    state.ui.rawLogH = 0.0f;
    // Hand the keyboard back through the one rule every closing surface uses
    // (`returnKeyboardBelow()`): a palette or a settings sheet still open above this surface keeps
    // it, and only when nothing else is up does the composer get the one-shot request.
    returnKeyboardBelow(state);
}

void toggleRawLog(TuiState& state) {
    if (state.ui.rawLogOpen) {
        closeRawLog(state);
        return;
    }
    // Opening is an explicit request and is never persisted (D-RAWLOG: every start is closed).
    // It owes one catch-up to the newest record - the surface opens on the live end of the
    // stream, which is what a developer surface is for; the pin still waits for content to sit
    // below the fold (D-FOLLOW).
    state.ui.rawLogOpen = true;
    state.rawLog.open = true;
    state.rawLog.nextPinAt = 0.0;
    // A stale answer from the previous session would claim a file that is no longer the last
    // thing this surface did.
    state.rawLog.savePath.clear();
    state.rawLog.saveError.clear();
}

bool handleRawLogCommand(TuiState& state, std::string_view query) {
    if (!views::isRawLogCommand(query))
        return false;
    // One implementation for every entry point (the chord, the palette row, `--debug-ui`).
    toggleRawLog(state);
    // The commands' half of the submit handshake: the draft is spent (it was a command, not a
    // question), the history cursor drops, the paste question is dismissed and the caret is not
    // restored to a buffer that no longer holds what the user typed. A command never joins the
    // history - recalling `/rawlog` as a question would be the one way to submit it.
    state.userQuery.inputBuf.clear();
    state.userQuery.historyPos = -1;
    state.composer.paste.dismiss();
    state.composer.reloadCaret = std::string::npos;
    return true;
}

/// Draw the Raw Log's full-screen overlay (the text grid's shape, task 33).
///
/// The frame opens the window, not the view: a view is a geometry taker and never positions
/// itself (D-METRICS). It has to be a *window* and not a draw-list fill inside the root, because
/// an overlay's opaque surface must erase the cells underneath it - a fill inside an existing
/// window recolours them, and a cell renderer would still show the thread's glyphs through the
/// developer stream. The palette solves the same problem the same way, and D-OVERLAY's z-order is
/// what `SetWindowFocus()` on the appearing frame reproduces. Only the topmost surface may take
/// the focus: a palette above this overlay keeps the keyboard (`ownsKeyboard` is false).
void drawRawLogOverlay(TuiState& state, const ui::Rect& rect, const ui::Metrics& m,
                       const model::RawLogRing& ring, bool ownsKeyboard) {
    if (rect.w <= 0.0f || rect.h <= 0.0f)
        return;
    ImGui::SetNextWindowPos(ImVec2(rect.x, rect.y), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(rect.w, rect.h), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::GetStyleColorVec4(ImGuiCol_ChildBg));
    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoSavedSettings;
    ImGui::Begin("##rawlog_overlay", nullptr, flags);
    // The overlay is modal, so the keyboard is its own - and a composer that still holds ImGui's
    // active id would keep *eating* the plain keys (`f`/`p`/`y`/`/`) before the surface reads them
    // from the character queue. Stealing the focus on the frame it appears is not enough: the
    // surface can be re-opened while an earlier overlay left the field active. Focusing the window
    // whenever the focus is elsewhere keeps the modal promise every frame, and a field inside this
    // window (the filter) owns the keyboard as usual - `FocusWindow` does not clear an active id
    // that belongs to it. A surface above it (the palette) keeps the focus: only the topmost
    // overlay may assert it, or the lower one would clear the top's ActiveId first.
    if (ownsKeyboard &&
        (ImGui::IsWindowAppearing() || !ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows)))
        ImGui::SetWindowFocus();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    renderRawLog(state.rawLog, rect, m, ring, ownsKeyboard);
    ImGui::End();
}

ui::LayoutPrefs layoutPrefsFor(const model::UiState& ui, bool paletteOpen, float composerH,
                               bool settingsOpen, bool helpOpen) {
    ui::LayoutPrefs prefs;
    // `-1 auto` is the layout's own answer: the rail is open wherever the display can hold a
    // column (the width breakpoints then pick the column or the collapsed strip), and the two
    // auto rules in `shellLayoutFor()` close a region where its shape would be a *panel* - a
    // panel nobody asked for covers the thread and the composer, which is not what "open" means
    // on a narrow display. Only an explicit 0 is "closed" here.
    prefs.railOpen = ui.prefs.railOpen != 0;
    prefs.railW = ui.railW;
    prefs.inspectorOpen = ui.prefs.inspectorOpen != 0;
    prefs.focusMode = ui.focusMode;
    // The one overlay that changes the tiling (the GUI's Raw Log drawer) and the
    // one the frame knows about so far: an overlay that does not change the
    // tiling only decides whether its rectangle exists.
    prefs.rawLogOpen = ui.rawLogOpen;
    // The drawer's dragged height (0 = the design's 30 %); the layout clamps it into the band it
    // can afford, so a drag can never squeeze the composer or the thread's three rows.
    prefs.rawLogH = ui.rawLogH;
    prefs.paletteOpen = paletteOpen;
    // The settings sheet (task 34) does not change the tiling either; the flag gives it a
    // rectangle and its place in the overlay stack (palette > settings > Raw Log > inspector).
    prefs.settingsOpen = settingsOpen;
    // The help page (task 42): the same kind of flag, one layer up - it is the top of the stack,
    // so it is drawn last and `Esc` closes it first.
    prefs.helpOpen = helpOpen;
    // The composer's grown height is the view's own answer from the frame before (task 27), and
    // "as tall as it is when empty" is the layout's default for the first frame.
    prefs.composerH = composerH;
    return prefs;
}

ui::ShellLayout shellLayoutFor(const ui::LayoutPrefs& prefs, ImVec2 display, bool inspectorAuto,
                               bool railAuto) {
    const ui::Metrics m = ui::metricsFor();
    ui::LayoutPrefs resolved = prefs;
    ui::ShellLayout layout = ui::computeShell(m, display, resolved);
    // The `-1 auto` rules, and *only* auto: an explicit open keeps the shape the engine
    // answered (the docked column, or the panel a narrow display gives it), because the user
    // asked for the panel and `Esc` is what closes it again.
    if (inspectorAuto && resolved.inspectorOpen && layout.inspectorOverlay) {
        resolved.inspectorOpen = false;
        layout = ui::computeShell(m, display, resolved);
    }
    // The rail's rule, one region over: below its own breakpoint the shape it would get is a
    // panel over the thread *and* the composer (the engine's "single column, both overlays" band),
    // which is not what an automatic "open" can mean. `Ctrl+B`/the palette's row write an explicit
    // 1 and keep the panel; this only refuses a panel nobody asked for.
    if (railAuto && resolved.railOpen && layout.railOverlay) {
        resolved.railOpen = false;
        layout = ui::computeShell(m, display, resolved);
    }
    return layout;
}

ui::ShellLayout overlayLayoutFor(const ShellFrame& frame, bool paletteOpen, bool settingsOpen,
                                 bool helpOpen) {
    // The painted tiling, plus the one kind of flag an overlay may change: not
    // `layoutPrefsFor()` from the live state, which may already carry a re-tiling a
    // chord did after the shell drew (the shell would then paint one tiling and the
    // overlay would land on another).
    ui::LayoutPrefs prefs = frame.prefs;
    prefs.paletteOpen = paletteOpen;
    prefs.settingsOpen = settingsOpen;
    prefs.helpOpen = helpOpen;
    return shellLayoutFor(prefs, frame.display, frame.inspectorAuto, frame.railAuto);
}

ShellFrame renderShell(TuiState& state, ImVec2 display) {
    // The `?` character (task 42, plan/key_gate.md §10/§11 option (a)): the terminal delivers no
    // key event for a printable character, so the grid's path to the page is the typed `?` - the
    // same route the rail's row keys and the inspector's `[`/`]` take, and like them it is taken
    // *before* any list draws: ImGui's type-to-select claims the queued characters of a nav window
    // that holds a `Selectable`, so a key taken later would never arrive (measured: the char was
    // gone by the end of the shell). `ui::takeCharacter()` refuses while a text field owns the
    // keyboard, so a `?` typed into the composer stays a question mark, and a modal surface keeps
    // its own keys. A press a view-local handler already spent is not spent twice
    // (`escapeConsumed`), the same rule every other frame-level key follows. The pixel profile's
    // chord (`Shift+/`) is the keymap's row and ran before this.
    if (!state.escapeConsumed && !state.help.open && !state.overlayOwnsKeyboard() &&
        ui::takeCharacter('?'))
        openHelp(state.help);

    // The persistent flash's answer (task 45, FR-29, plan/key_gate.md §4i): `x` acknowledges the
    // item the presentations name - `model::dismissShownToast()` is the newest one, which is what
    // the grid's flash and its `x dismiss` hint show and what the pixel profile's stack draws
    // nearest the corner. (A card's own `Dismiss`/cross answers *that* card instead.) A character
    // key read from the queue, the seam `y`/`s`/`?` use: the terminal delivers no key event for a
    // printable character, and `takeCharacter()` refuses while a text field owns the keyboard, so
    // an `x` typed into the composer stays an `x` (and a warning survives while the user types,
    // which is the point
    // - it waits for an answer, not for a keystroke; with a draft in progress the route is `Esc`'s
    // blur step first, D-ESC's order). A modal surface owns every key while it is up, so a press
    // meant for the palette cannot clear a warning behind it, and a press a view-local handler
    // already spent is not spent twice.
    if (!state.escapeConsumed && !state.overlayOwnsKeyboard() && ui::takeCharacter('x'))
        model::dismissShownToast(state.ui);

    // The adapter is a member of the state and is rebound every frame: see
    // `ThreadStoreAdapter::bind` (a moved state leaves it unbound, so the frame
    // layer is what re-attaches it).
    state.threadAdapter.bind(state);

    ShellFrame frame;
    frame.display = display;
    frame.prefs = layoutPrefsFor(state.ui, state.palette.open, state.composer.height,
                                 state.settingsSheet.open, state.help.open);
    // `-1 auto` is the one preference the layout cannot see (its `inspectorOpen` is the
    // resolved bool), so the frame carries it: `shellLayoutFor()` applies the rule and the
    // overlay layer's re-layout has to apply the same one. The rail's `-1 auto` is the same kind
    // of answer and travels the same way.
    frame.inspectorAuto = state.ui.prefs.inspectorOpen < 0;
    frame.railAuto = state.ui.prefs.railOpen < 0;
    frame.layout = shellLayoutFor(frame.prefs, display, frame.inspectorAuto, frame.railAuto);
    const ui::Metrics m = ui::metricsFor();
    const ui::ShellLayout& layout = frame.layout;
    // The overlay stack's keyboard owner: only the topmost surface may take focus or read the
    // plain keys. `layout.top` already includes the palette's live flag, so the Raw Log can be
    // drawn below it without stealing the keyboard back.
    const bool rawLogOwnsKeyboard = layout.top == ui::OverlayLayer::RawLog;

    // The inspector's inputs are derived here, before anything draws or asks `Esc` a question:
    // "is the panel up, does it float, which tab" is an answer of the layout above, and three
    // consumers read it in this frame (the now bar's `Esc stop` hint, the top bar's and the
    // thread's `acceptKeys`, the panel itself). Deriving it after them would hand one frame the
    // previous frame's shape - a saved `InspectorOpen=1` at 80x24, or a resize, is exactly the
    // frame where that shows.
    deriveAgents(state, frameClockMs());
    // The live answer's clock: the same one-per-frame rule as the lanes', and derived before
    // anything draws - the thread's caret and the live row's header read it this frame.
    deriveStream(state, frameClockMs());
    deriveInspector(state, layout);
    // The rail's two derived flags, one region over: `open` is "the region has a rectangle",
    // `overlay` is "it is the panel". Three consumers ask before the rail draws (the now bar's
    // `Esc stop` hint, the top bar's and the thread's `acceptKeys`), so this cannot wait for the
    // draw site: the frame the panel appears or disappears is exactly the one where the previous
    // shape would show.
    const bool railFloats = layout.railOverlay && layout.rail.w > 0.0f && layout.rail.h > 0.0f;
    state.ui.rail.open = layout.rail.w > 0.0f && layout.rail.h > 0.0f;
    state.ui.rail.overlay = railFloats;
    // The Raw Log's four inputs: is it up, which shape, its two view modes and the level floor
    // its `UiLog` preference names (task 33). Derived here with the inspector's, before any
    // region draws, because the `Esc` chain's answer and every region's `acceptKeys` read it.
    const model::RawLogRing ring = rawLogRing(state);
    deriveRawLog(state, m);

    // The six tiled regions, in the layout's own orientation. A zero rectangle is
    // a region the layout folded away (a closed panel, the compact top bar) and
    // paints nothing; the thread is always drawn, because it is the region this
    // frame exists for.
    //
    // Order: the top bar, the thread, then the two panels (the bar is the row above the
    // thread and overlaps nothing). Below the width breakpoint the rail and the inspector
    // *float above* the thread (they are panels, not columns), so drawing them before it
    // would let the transcript paint over them; the design's own z-order puts the inspector
    // above the rail (D-OVERLAY), which is the order they are drawn in here.

    // The top bar (FR-02, views/topbar.h): the frame derives the bar's inputs from the
    // aggregate, the view draws into the rectangle the layout answered, and the answers come
    // back in the same frame. The window-title mirror is the one write that is not a
    // `UiState` field: it is the internal C++ seam of P-Δ2, never the ABI.
    //
    // It runs whether or not the bar has a rectangle (compact mode folds it away): the run
    // state and the OS title belong to the session, not to the row that shows them.
    deriveTopBar(state);
    if (layout.topBar.h > 0.0f)
        renderTopBar(state.topBar, layout.topBar, m);
    // The OS window title mirrors what the bar shows: this frame's derived title, which is the
    // snapshot's title and nothing else. It runs *before* `applyTopBarAnswers` because both strings
    // belong to the frame that drew them: a commit is applied after the frame that drew the old
    // title, so the window and the bar change together on the frame the snapshot arrives - the
    // mirror is never ahead of the row. One call per frame: the seam forwards only a change.
    tuiBackendSetWindowTitle(state.topBar.sessionTitle);
    applyTopBarAnswers(state);
    // The settings sheet's diagnostics (task 34): the paths, the versions, the flags and the
    // display. Derived *after* `applyTopBarAnswers()` because that is where a click on the bar's
    // `Settings` entry opens the sheet - deriving before it would paint one frame with an empty
    // profile path, an empty generation and zeros for the size (`deriveSettings()` is a no-op while
    // the sheet is down), and a `Copy system info` on the opening frame would carry that lie.
    deriveSettings(state, display);
    // The status projection, in the same spirit: the now bar's row and the composer's
    // meter and phase line read the same chips, so it runs before any region draws and
    // whether or not the row has a rectangle (compact mode folds the top bar away, never
    // this row). One derivation per frame, from the structured status when D sends one
    // and from the sentence until then (D-STATUS). The answer is kept on the frame so a
    // harness - and a golden - measures the row that was drawn instead of rebuilding a
    // fixture that could drift from this derivation.
    frame.nowBar = deriveNowBar(state, m);
    frame.nowBar.compact = layout.compact;
    // The top bar's fold is the layout's answer, not the view's guess: in compact mode and in
    // focus mode (task 43) the bar draws no row, so the `RAW` badge lives on this one - a hidden
    // region may never hide a safety signal (design §4.1's degradation rule).
    frame.nowBar.topBarFolded = layout.topBar.h <= 0.0f;
    // A view-local key needs the same permission the rail's row keys get: no surface owns the
    // `Esc` chain's first step (`escStepClosesOverlay()`), and no press was already spent
    // (`Esc` is one step per press). The thread's `y` (copy the focused block) is the second
    // consumer of that answer.
    const bool acceptThreadKeys = !escStepClosesOverlay(state) && !state.escapeConsumed;
    // The transcript is drawn into the readable column, not the region (task 43): focus mode
    // caps and centres it, and with the mode off the two rectangles are the same one.
    renderThread(state.ui, state.threadAdapter, layout.threadColumn, layout.loadEarlier, m,
                 layout.compactGroups, layout.compact, acceptThreadKeys);
    // The rail (task 23, views/rail.h): the frame derives its rows from the snapshot, the view
    // draws the visible window of them into the rectangle the layout answered, and the answers (a
    // select, a rename, a delete, a pin) are applied through `queueSessionAction()` in this same
    // frame.
    //
    // The derivation runs whether or not the rail has a rectangle: the deferred switch, the
    // skeleton's acknowledgement and the unread set belong to the session, not to the column that
    // happens to be showing them. It also runs before the rail draws, because a rail that is
    // collapsed by `Ctrl+B` must still flush the switch the user queued from it.
    const std::int64_t nowMs = frameClockMs();
    const RailFrame railFrame = deriveRail(state, nowMs);
    // Paint order, the rail's half (the inspector's is below and states the rule in full): a
    // *docked* column overlaps nothing and draws here, where its answers are applied next to the
    // rows that produced them; the *panel* shape is an overlay (design §3.3's "single column,
    // both overlays") and is therefore drawn at the end of the frame, above the thread's child
    // window - an overlay that drew before it lost the fill to the child's draw list and the
    // thread's glyphs came through the panel (found in task 40's 60x20 golden).
    //
    // The rect the panel *paints* on: the layout's rectangle down to the composer's top.
    //
    // The layout's overlay rectangle runs to the now bar (one anatomy for both panels, so the
    // z-order and `Esc` need no special case), but a session list is not a modal surface: it stays
    // beside the conversation while the user types - the v4 session panel never covered the
    // composer either, and P-05 is parity. So the panel is opaque over the *thread* and stops at
    // the composer, which keeps its whole field, its meter and its action. The inspector's panel
    // is the other case and keeps covering the composer: a user opens it to *read*, and `Esc`
    // closes it (`escStepClosesOverlay()`).
    ui::Rect railPaint = layout.rail;
    if (railFloats)
        railPaint.h = std::min(railPaint.h, std::max(0.0f, layout.composer.y - layout.rail.y));
    const auto drawRail = [&]() {
        // A *modal* surface owns the keyboard (`overlayOwnsKeyboard()`), and a press a view-local
        // handler already spent must not act twice. `Esc` closing the panel is a different
        // question (D-ESC): the panel's `/`, its row keys and its rename/search `Esc` steps live
        // here, and gating them on `escStepClosesOverlay()` made the panel keyboard-dead - the
        // docked column and the strip never had that problem.
        const bool acceptKeys = !state.overlayOwnsKeyboard() && !state.escapeConsumed;
        renderRail(state.ui.rail, railFrame.list, railPaint, m, railRowLines(m, display.y), nowMs,
                   acceptKeys);
        // A select, a rename, a delete, a pin: queued in the frame that produced them.
        applyRailAnswers(state, m.railW);
    };
    if (layout.rail.w > 0.0f && !railFloats)
        drawRail();
    else if (!railFloats)
        // No column and no panel: the answers still have to be applied, because the palette's
        // session row and `New session` write them with the rail hidden (`Ctrl+B`).
        applyRailAnswers(state, m.railW);
    // The inspector (task 29, views/inspector.h): the frame derived its four inputs above; the
    // view draws the frame, the tab strip and the visible rows of the active tab into the
    // rectangle the layout answered, and the tab it asked for is applied in this same frame.
    //
    // Paint order. A *floating* panel is an overlay, and D-OVERLAY puts overlays above the
    // tiling - the panel's rectangle spans from the top bar to the now bar (that is what keeps
    // its minimum height reachable at the floor), so it covers the composer as well as the
    // thread. Drawing it here, before the composer, let the composer paint over the panel's last
    // rows and its bottom hairline; the floating shape is therefore drawn at the *end* of the
    // frame, after the composer and the now bar. The docked column overlaps nothing and stays
    // here, where its answers are applied next to the rows that produced them.
    const bool inspectorFloats = state.inspector.open && state.inspector.overlay;
    // The panel's character keys (`[`/`]`): a *modal* surface owns every key while it is up
    // (`overlayOwnsKeyboard()` - the palette today, the settings sheet and the Raw Log when they
    // land), and a press a view-local handler already spent is not spent twice. The floating
    // panel is deliberately not gated by `escStepClosesOverlay()`: that answers "does Esc close
    // something", and for the floating shape the answer is the panel itself, which is no reason
    // to refuse its own keys.
    const bool acceptInspectorKeys = !state.overlayOwnsKeyboard() && !state.escapeConsumed;
    if (layout.inspector.w > 0.0f && !inspectorFloats)
        renderInspector(state.inspector, state.ui.agents, layout.inspector, m,
                        /*acceptKeys=*/acceptInspectorKeys, state.ui.activity);
    // The Raw Log's *drawer* (the pixel profile only, task 33): the layout reserves it between
    // the thread and the composer, so it is drawn with the tiling, before the composer - which
    // is also the proof that it cannot cover it (design §4.9). The grid's full-screen overlay is
    // an overlay and is drawn at the end of this function, above the floating inspector.
    if (state.rawLog.open && !state.rawLog.overlayShape)
        renderRawLog(state.rawLog, layout.rawLogOverlay, m, ring, /*ownsKeyboard=*/false);
    // The composer (task 27, views/composer.h): the frame derives its inputs from the
    // store and the chips, the view draws the container, the field, the meter and the
    // action into the rectangle the layout answered - and reports its own grown height,
    // which `layoutPrefsFor()` reserves on the next frame. The answers (a send, a cancel,
    // a spent `Esc`) are applied through the same paths the v4 composer used.
    if (layout.composer.h > 0.0f) {
        views::renderComposer(composerInputFor(state, nowMs), layout.composer, m);
        applyComposerAnswers(state);
    }
    if (layout.nowBar.h > 0.0f) {
        // The now bar (task 28, views/nowbar.h): the frame derived the row's inputs above
        // (and kept them on `frame.nowBar`), the view draws them into the rectangle the
        // layout answered. Compact mode is the layout's answer, not the view's guess: it
        // decides whether the chips collapse and whether the `RAW` badge lives here (the
        // top bar is folded away then).
        views::renderNowBar(frame.nowBar, layout.nowBar, m);
    }
    // The floating rail panel first (it is the bottom of the overlay stack, D-OVERLAY), then the
    // floating inspector, then the Raw Log's full-screen overlay: the same order the layout's
    // `top` uses, so the last thing drawn is the surface `Esc` would close first.
    if (railFloats)
        drawRail();
    // The floating panel, last but for the Raw Log overlay: it is an overlay above the tiling, so
    // it must paint after everything it covers (D-OVERLAY). Its keys are gated by the same
    // predicate as the other views', and a press the frame already spent is not spent twice.
    if (inspectorFloats)
        renderInspector(state.inspector, state.ui.agents, layout.inspector, m,
                        /*acceptKeys=*/acceptInspectorKeys, state.ui.activity);
    // The Raw Log's full-screen overlay last (task 33): D-OVERLAY's order is palette > settings >
    // Raw Log > inspector, and everything above it in that list draws later (`renderOverlays()`
    // paints the palette and the settings sheet). The overlay is where the developer stream is
    // *read*, so it takes the keyboard only while it is the topmost surface - the view's own
    // characters and the `Esc` step both belong to it, and every region below runs with
    // `acceptKeys` false.
    if (state.rawLog.open && state.rawLog.overlayShape)
        drawRawLogOverlay(state, layout.rawLogOverlay, m, ring, rawLogOwnsKeyboard);
    applyInspectorAnswers(state);
    // The Raw Log's answers are applied in the frame that produced them, like the panel's: a
    // close is one press, and a pause/follow change is what the *next* frame's follower reads.
    applyRawLogAnswers(state, m);
    return frame;
}

void renderOverlays(TuiState& state, ShellFrame& frame) {
    // The keymap runs between the shell and this call, so an overlay it just opened
    // has no rectangle: the pass above laid out with the palette closed. Re-lay out
    // for that one frame - `computeShell` is pure, and a closed overlay's rectangle
    // is zero, so this is exactly the "opened since the layout" test. The closing
    // direction needs nothing: `open` is false, so the stale box is never drawn.
    //
    // `overlayLayoutFor` re-lays out from the preferences the shell *painted*, so a
    // chord that changed a tiling preference in between cannot put this overlay on a
    // tiling the screen does not show (views/shell.h).
    if ((state.palette.open && frame.layout.paletteOverlay.w <= 0.0f) ||
        (state.settingsSheet.open && frame.layout.settingsOverlay.w <= 0.0f) ||
        (state.help.open && frame.layout.helpOverlay.w <= 0.0f))
        frame.layout =
            overlayLayoutFor(frame, state.palette.open, state.settingsSheet.open, state.help.open);

    const ui::Metrics m = ui::metricsFor();
    // D-OVERLAY's z-order, bottom first: the settings sheet (task 34) under the palette, and only
    // the topmost surface takes the keyboard. The Raw Log's full-screen overlay and the floating
    // inspector are painted inside `renderShell()` (they are drawn under this layer), so the
    // surfaces that can be *above* them are exactly the ones here.
    if (state.settingsSheet.open) {
        const bool ownsKeyboard = frame.layout.top == ui::OverlayLayer::Settings;
        renderSettings(state.settingsSheet, frame.layout.settingsOverlay, m, state.ui.prefs,
                       state.settings, ownsKeyboard);
        applySettingsAnswers(state);
    }
    if (state.palette.open)
        renderPalette(state.palette, state, frame.layout.paletteOverlay);
    // The help page next, above everything it explains (task 42). Its own `Close` control is the
    // pointer's way out; closing is the same step `Esc` performs, keyboard included.
    if (state.help.open) {
        renderHelp(state.help, frame.layout.helpOverlay, m,
                   /*ownsKeyboard=*/frame.layout.top == ui::OverlayLayer::Help);
        if (state.help.closeRequested) {
            closeHelp(state.help);
            returnKeyboardBelow(state);
        }
    }
    // The feedback stack last, above everything it may explain (task 45): it is chrome, not a
    // surface - it takes no key, closes nothing and never joins the tiling - so it is drawn after
    // every page that could cover it. The area it may use is the display down to the composer's
    // top: the one region it must never cover is the field the user types into, and the row below
    // that is the now bar's own.
    ui::Rect toastArea;
    toastArea.w = frame.display.x;
    toastArea.h = frame.layout.composer.h > 0.0f ? frame.layout.composer.y : frame.display.y;
    toastArea.cells = m.textGrid;
    renderToastStack(state.ui, toastArea, m);
}

bool escStepClosesOverlay(const TuiState& state) {
    // The help page first (task 42): it is the top of the stack, so it is the surface an `Esc`
    // answers before anything under it.
    if (state.help.open)
        return true;
    // The palette next: it is the topmost of the design's own surfaces and the one that owns the
    // keyboard, so while it is up nothing below it can also be the answer.
    if (state.palette.open)
        return true;
    // The settings sheet (task 34): modal, above the Raw Log and the inspector panel and below
    // the palette (D-OVERLAY).
    if (state.settingsSheet.open)
        return true;
    // The Raw Log's full-screen overlay (task 33): modal on the grid, so `Esc` closes it. The
    // pixel profile's *drawer* is one of the tiled regions (D-OVERLAY) and is closed by its
    // button, so it is deliberately not here - `Esc` there belongs to the run and the composer.
    if (state.ui.rawLogOpen && state.rawLog.overlayShape)
        return true;
    // The inspector answers only while it *floats*: the docked column is one of the six
    // regions, and `Esc` beside it belongs to the run and the composer (D-ESC's later steps).
    if (state.inspector.open && state.inspector.overlay)
        return true;
    // ...and the rail panel, the bottom of the stack: the only surface that can cover the
    // composer while it is up, so a press has to reach it (`ui::OverlayLayer::Rail`, task 17's
    // z-order). The docked column and the collapsed strip are part of the tiling and are not.
    return state.ui.rail.overlay;
}

bool railOwnsEscape(const TuiState& state) {
    const model::RailState& rail = state.ui.rail;
    // Focus mode hides the rail (task 43): a view that is not drawn stops refreshing its own
    // latches (`renderRail()` is what clears `filterFocus`/`menuRowId`), so a hidden rail cannot
    // own a press it has no control on screen to spend - the field would go keyboard-dead behind a
    // panel nobody can see.
    if (state.ui.focusMode)
        return false;
    // A rail that is not on screen holds nothing: the strip shape clears its own dialog state
    // every frame (views/rail.cpp), so its `Esc` is the frame's.
    if (!rail.open && !rail.searchWide)
        return false;
    if (!rail.renameRowId.empty() || !rail.menuRowId.empty())
        return true;
    // The search field's own `Esc` clears the query. `filterHadFocus` is the previous frame's
    // latch (the frame that hands the keyboard over cannot end it), so the press that opened
    // the field is not read as the field's.
    return rail.filterFocus || rail.filterHadFocus;
}

void pressReservedDigit(TuiState& state) {
    // Focus mode hides the panel and its strip (task 43), so the digit addresses nothing: arming a
    // notice the user cannot read and opening a panel on the way out would be the mode writing the
    // preferences it promises to leave alone (`model::regionControlsApply`). A no-op there, like
    // the two region toggles.
    if (!model::regionControlsApply(state.ui))
        return;
    state.inspector.reservedPressed = true;
    // The notice lives in the strip, so the press has to show the panel it explains: an
    // explicit 1, never back to `-1 auto`, like every other hand-made choice. Without this a
    // reserved digit pressed with the panel closed would be a key that does nothing at all.
    if (!state.inspector.open)
        state.ui.prefs.inspectorOpen = 1;
}

ui::OverlayLayer closeTopOverlay(TuiState& state) {
    // The help page (task 42), the top of the stack: closing it hands the keyboard to whatever is
    // under it - which is the composer in every state the page can be opened from.
    if (state.help.open) {
        closeHelp(state.help);
        returnKeyboardBelow(state);
        return ui::OverlayLayer::Help;
    }
    if (state.palette.open) {
        views::closePalette(state.palette, state);
        return ui::OverlayLayer::Palette;
    }
    // The settings sheet, below the palette and above the Raw Log (D-OVERLAY). Closing it hands
    // the keyboard to whatever is under it (`returnKeyboardBelow()`), so a closed sheet never
    // leaves the keyboard nowhere.
    if (state.settingsSheet.open) {
        closeSettings(state.settingsSheet);
        returnKeyboardBelow(state);
        return ui::OverlayLayer::Settings;
    }
    // The Raw Log's full-screen overlay, below the palette and above the inspector panel
    // (D-OVERLAY). The drawer shape is not closeable this way: it is part of the tiling.
    if (state.ui.rawLogOpen && state.rawLog.overlayShape) {
        closeRawLog(state);
        return ui::OverlayLayer::RawLog;
    }
    if (state.inspector.open && state.inspector.overlay) {
        // The user's own action, so an explicit 0 and never back to `-1 auto`: a panel that
        // was closed by hand must not re-open on the next resize. The frame re-derives `open`
        // and `overlay` from the layout on its next pass, so clearing them here only makes
        // this frame's answer consistent for whoever reads it.
        state.ui.prefs.inspectorOpen = 0;
        state.inspector.open = false;
        state.inspector.overlay = false;
        state.inspector.reservedPressed = false;
        return ui::OverlayLayer::Inspector;
    }
    if (state.ui.rail.overlay) {
        // The rail panel, the bottom of the stack: the same explicit 0 (`Ctrl+B`'s rule), because
        // a panel the user closed must not come back as an automatic one after a resize.
        state.ui.prefs.railOpen = 0;
        state.ui.rail.open = false;
        state.ui.rail.overlay = false;
        closeRailSearch(state.ui);
        return ui::OverlayLayer::Rail;
    }
    return ui::OverlayLayer::None;
}

void uiConfFrame(model::UiState& ui, bool& loaded, const std::string& iniFilename,
                 double nowSeconds) {
    // No profile name (a harness, a state D never pointed at a file): nothing to read and
    // nothing to write. `uiConfPath("")` is empty, so the module would no-op anyway - the
    // early return just keeps the latch from claiming a load that never happened.
    if (iniFilename.empty())
        return;
    if (!loaded) {
        model::loadUiConf(ui.prefs, iniFilename);
        loaded = true;
    } else {
        model::flushUiConf(ui.prefs, iniFilename, nowSeconds);
    }
    // The read-only-directory rule (design §5.3, task 22): a write that fails is reported once and
    // the settings stay in memory, so the failure can never turn into a retry loop. The module
    // keeps one line per failed window; this is its one consumer, and it posts it as an Activity
    // warning - the tab the user opens to ask "what went wrong" (D-ACTIVITY). It runs *after* the
    // flush, so the warning is published in the frame whose write produced it (and a line left by
    // the quit path's save is published by the next frame).
    std::string warning;
    if (model::takeUiConfWarning(warning)) {
        model::LogRecord record;
        record.timestampMs = frameClockMs();
        record.level = model::kActivityLevelWarn;
        record.message = warning;
        // `visible` is false: the sheet's own write path is not the Activity tab, so a warning it
        // produced counts as unread until the user looks at the tab (D-UNREAD's rule for the
        // record stream).
        model::appendActivity(ui, record, model::kActivityLevelWarn, /*visible=*/false);
    }
}

void uiConfQuit(const model::UiState& ui, const std::string& iniFilename) {
    if (iniFilename.empty())
        return;
    model::saveUiConf(ui.prefs, iniFilename);
}

} // namespace llmfun::tui::views
