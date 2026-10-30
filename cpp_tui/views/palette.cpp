/// Palette overlay: item table, fuzzy filtering, geometry, keyboard handling and
/// activation (views/palette.h owns the contract and the key evidence).
///
/// Two things this file deliberately does NOT do:
///   * it does not tile - the overlay is its own window, and where it sits comes
///     from `ui::computeShell()` (the `overlay` argument), so the shell's regions
///     never move under it and no view positions itself (task 17);
///   * it does not rank per frame - `visible` is rebuilt when the query changes
///     (or the palette opens), and a frame only walks the visible window. The
///     row *flags* (run active / answer available) are refreshed per frame,
///     because they move without the table changing.
///
/// Layering note (`tools/gate_ratchet.txt`): this file includes `tui.h` - the
/// palette's contract is `renderPalette(PaletteState&, TuiState&, const Rect&)`,
/// because activating a row *acts* on the aggregate - but it names no v4 type.
/// "Copy last answer" walks the *block seam* (`threadAdapter.blockAt()` /
/// `blockText()`), not the message store: a copy target is a block (D-PALETTE: the
/// newest `Answer`, or the live streamed body), which is also why this file never
/// mentions a message id. `views/` still reads no v4 type, so the ledger's v4
/// column for this file is 0. A profile branch, a hand-made `ImVec4` or a
/// display-size read here would still fail `./test_metrics`.
///
/// One table, one dispatcher: `paletteActions()` builds the rows (in section order) and
/// `palettePerformRow()` performs them - `Enter`, a click and `./test_palette`'s registry walk
/// all go through that one function. A row's `model::Action` *is* what it does, so a row cannot
/// claim one action and perform another; the palette-local rows (`Action::None`) are the ones
/// whose command is the palette's own: a session pick, a command line, the free-text fallback.

#include "views/palette.h"

#include "views/blocks.h"
#include "views/composer.h"
#include "views/rawlog.h"
#include "views/settings.h"
#include "views/thread.h"

#include "session_fuzzy.h"
#include "session_panel.h"
#include "store/thread_store.h"
#include "tui.h"
#include "tui_widgets.h"
#include "ui/metrics.h"

#include "imgui/imgui_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace llmfun::tui::views {

/// Groups the thread's bounded window currently hides, by the thread's own arithmetic
/// (`views/thread.cpp` draws with it). Defined below with the other row queries; declared here
/// because `refreshFlags()` re-derives the `Load earlier` row from it every frame.
std::size_t threadHiddenGroups(const TuiState& state);

namespace {

// The sections, in confidence order (design §4.12: "sections ordered by confidence"): the
// actions a user came for, the sessions they were in, the command lines the agent understands,
// the developer surface, and the free-text fallback last. This array is the one owner of the
// order - `rankItems()` sorts by it, `cycleGroup()` (`Tab`) walks it, and `groupIndexOf()` maps a
// name to the filter value the title row prints.
constexpr const char* kGroupActions = "ACTIONS";
constexpr const char* kGroupSessions = "SESSIONS";
constexpr const char* kGroupCommands = "COMMANDS";
/// The developer group (task 33): the Raw Log lives here and nowhere else in the normal groups -
/// "not an inspector tab and not in normal palette groups (a `Developer` group with a `dev`
/// badge at most)", as design §4.9 puts it.
constexpr const char* kGroupDeveloper = "DEVELOPER";
constexpr const char* kGroupAsk = "ASK";

constexpr const char* kSections[] = {kGroupActions, kGroupSessions, kGroupCommands, kGroupDeveloper,
                                     kGroupAsk};
constexpr int kSectionCount = 5;

/// `""` is the ALL pseudo-group; then the real sections in cycle order.
constexpr const char* kGroupCycle[kSectionCount + 1] = {
    "", kGroupActions, kGroupSessions, kGroupCommands, kGroupDeveloper, kGroupAsk};

/// A section's `groupIndex` (0 = ALL, also the answer for a name the table does not have). One
/// lookup, so `Tab`'s numbers and the `/` entry point's target cannot drift apart.
int groupIndexOf(const char* group) {
    for (int i = 1; i <= kSectionCount; ++i) {
        if (std::strcmp(kGroupCycle[i], group) == 0)
            return i;
    }
    return 0;
}

/// A section's rank in the list order (the LAST rank for an unknown group, so an unlisted group
/// sinks instead of leading).
int sectionRank(const char* group) {
    for (int i = 0; i < kSectionCount; ++i) {
        if (group != nullptr && std::strcmp(kSections[i], group) == 0)
            return i;
    }
    return kSectionCount;
}

// Stable ids. Never display text: the dispatcher and the tests match on these, so a label may
// change without touching a behaviour or a test.
constexpr const char* kIdNewSession = "actions.newSession";
constexpr const char* kIdFocusComposer = "actions.focusComposer";
constexpr const char* kIdCopyAnswer = "actions.copyLastAnswer";
constexpr const char* kIdStopRun = "actions.stopRun";
constexpr const char* kIdQuit = "actions.quit";
constexpr const char* kIdLoadEarlier = "actions.loadEarlier";
constexpr const char* kIdToggleRail = "actions.toggleRail";
constexpr const char* kIdToggleInspector = "actions.toggleInspector";
/// Focus mode (task 43). The grid's only way in: `F11` is a function key no terminal delivers and
/// `Ctrl+Shift+M` arrives as `Enter` (plan/key_gate.md §3b), while the way out is `Esc` - the one
/// key a mode that hides the rail and the inspector must always keep.
constexpr const char* kIdFocusMode = "actions.focusMode";
/// The inspector's two tabs (task 29). Rows as well as chords, because `Ctrl+5`/`Ctrl+6` never
/// reach the terminal (plan/key_gate.md §3) and the panel is closed when the user needs them.
constexpr const char* kIdInspectorAgents = "actions.inspectorAgents";
constexpr const char* kIdInspectorActivity = "actions.inspectorActivity";
/// The settings sheet (task 34). Its other entry point is the top bar's `Settings` control, and
/// below the width breakpoint the bar folds away - so this row is the *only* way in there
/// (D-PALETTE; the sheet's view says the same).
constexpr const char* kIdOpenSettings = "actions.openSettings";
/// The help page (task 42). Its chord (`Shift+/`) is a *typed character* while the composer holds
/// the keyboard - a `?` in a question is the field's - so the palette is the path that works from
/// the field the user is normally in, and `./test_help` walks it.
constexpr const char* kIdHelp = "actions.help";
/// The developer row (task 33): the grid's path to the Raw Log - the chord is not delivered
/// there (plan/key_gate.md §10), and `/rawlog` is the typed one.
constexpr const char* kIdToggleRawLog = "dev.rawlog";
constexpr const char* kIdFreeText = "ask.freeText";
/// A command row's id is `commands.<name>`: the id is derived from the command line it submits,
/// and the *label* is that same line (see `paletteActions()`), so the row cannot show one
/// command and send another.
constexpr const char* kIdCommandPrefix = "commands.";

/// Result cap per profile: §4.12's scale line - 12 results in the pixel profile,
/// 5 at 80x24 in the text grid. The overlay's height ceiling (60 % of the
/// display in the grid) is the second bound, so a 40x15 terminal shows 3.
constexpr int kGuiRows = 12;
constexpr int kTextRows = 5;

/// The 1-cell (grid) / 1-px (pixel) frame around the overlay. Its own metric,
/// like the composer's: a theme with a zero border size must not remove the
/// palette's edge.
constexpr float kPaletteFrame = 1.0f;

const char* groupName(int groupIndex) {
    if (groupIndex <= 0 || groupIndex > kSectionCount)
        return "ALL";
    return kGroupCycle[groupIndex];
}

bool isAskRow(const PaletteItem& item) { return std::strcmp(item.id, kIdFreeText) == 0; }

bool groupIs(const PaletteItem& item, const char* group) {
    return item.group != nullptr && std::strcmp(item.group, group) == 0;
}

/// The role a block carries when its body turned it into `Code`: `blockAt()` rewrites the kind
/// for a body that is a single fenced code block and keeps the role word, and the words are the
/// same table `views/blocks.cpp` draws from. So the answer test reads the kind *and* the word -
/// a code-only answer is an answer, and a tool result that happens to be code is not.
bool blockIsAnswerRole(const model::BlockDesc& block) {
    if (block.kind == model::BlockKind::Answer || block.kind == model::BlockKind::Assistant)
        return true;
    return block.label == defaultBlockLabel(model::BlockKind::Answer) ||
           block.label == defaultBlockLabel(model::BlockKind::Assistant);
}

/// The newest answer in the thread, as a *block*: (group index, block index).
///
/// The copy row names a block, not a message id (D-PALETTE), so this walks the adapter's groups
/// and blocks from the end. Two blocks qualify: a stored `Answer` (or an `Assistant` message,
/// which is what an interrupted run leaves) and the live streamed body - whose row exists before
/// the store has a message, which is why "copy the partial answer" works mid-run.
///
/// `threadAdapter` answers an empty thread when it is unbound (`state_ == nullptr`), so a harness
/// without a bound adapter gets "no answer" instead of a crash. The walk is short: the answer is
/// in the last group or two of a transcript, and `blockAt()` is a copy out of an already-built
/// block list, not a store walk.
bool lastAnswerBlock(const TuiState& state, std::size_t& group, std::size_t& block) {
    const std::size_t groups = state.threadAdapter.groupCount();
    for (std::size_t k = groups; k > 0; --k) {
        const std::size_t blocks = state.threadAdapter.blockCount(k - 1);
        for (std::size_t b = blocks; b > 0; --b) {
            const model::BlockDesc desc = state.threadAdapter.blockAt(k - 1, b - 1);
            if (!blockIsAnswerRole(desc))
                continue;
            // A row with no body is not copyable: the streamed row is the exception, because
            // its body is the text being written (an empty stream is an answer still arriving).
            if (desc.hasBody || desc.streaming) {
                group = k - 1;
                block = b - 1;
                return true;
            }
        }
    }
    return false;
}

/// Whether there is an answer to copy - the copy row's enabled flag. Cheap on purpose: it walks
/// the block descriptors and never copies a body (the text is copied when the row is activated).
bool hasAnswerToCopy(const TuiState& state) {
    std::size_t group = 0, block = 0;
    return lastAnswerBlock(state, group, block);
}

/// A cheap stamp of the thread's *shape*: the group count, the newest group's id and its block
/// count. `groupAt()` and `blockCount()` are probes (no store walk, no block descriptors), which is
/// what makes this affordable on the per-frame flag refresh.
///
/// The id is in the stamp because an ended stream and its answer can leave both counts unchanged
/// (one group replaces another, one block replaces one), and that transition is exactly when the
/// answer appears.
std::size_t threadShape(const TuiState& state) {
    const std::size_t groups = state.threadAdapter.groupCount();
    std::size_t stamp = groups;
    if (groups > 0) {
        stamp = stamp * 1000003u + state.threadAdapter.groupAt(groups - 1).id;
        stamp = stamp * 1000003u + state.threadAdapter.blockCount(groups - 1);
    }
    return stamp;
}

/// `hasAnswerToCopy()` with a one-slot cache (`PaletteState::answerStamp`/`answerKnown`).
///
/// The walk builds a `BlockDesc` per block - several `std::string` copies each - and a long tool
/// run with no answer yet would turn the per-frame refresh into a whole-thread walk at 60 Hz (the
/// review's M3). The answer can only appear or disappear when the thread's shape moves, so that is
/// the key.
bool answerForCopy(PaletteState& p, const TuiState& state) {
    const std::size_t stamp = threadShape(state);
    if (p.answerStamp != stamp) {
        p.answerStamp = stamp;
        p.answerKnown = hasAnswerToCopy(state);
    }
    return p.answerKnown;
}

bool isUtf8Continuation(char c) { return (static_cast<unsigned char>(c) & 0xC0) == 0x80; }

/// `s` cut to at most `cells` bytes with a trailing "..." when anything was
/// dropped. Byte-wise like the rail's title clipping: ASCII input is exact and a
/// multi-byte label loses a whole glyph instead of a character, because the cut
/// walks back over continuation bytes.
std::string fitText(const std::string& s, int cells) {
    if (cells <= 0)
        return {};
    if (static_cast<int>(s.size()) <= cells)
        return s;
    if (cells <= 3)
        return std::string(static_cast<std::size_t>(cells), '.');
    std::size_t n = static_cast<std::size_t>(cells - 3);
    while (n > 0 && isUtf8Continuation(s[n]))
        --n;
    return s.substr(0, n) + "...";
}

/// Why the free-text row is (not) usable - one string for the build and for the
/// per-frame flag refresh.
const char* askDescription(bool ready) {
    return ready ? "Send this query to the agent" : "A run is active - stop it first";
}

/// Refresh the flags that move *without* the table changing. Whether a run is
/// active and whether an answer exists are properties of the session, not of the
/// query, so a table built on open would go stale while the overlay stays up:
/// open the palette before an answer exists and "Copy last answer" would stay
/// disabled after it arrives, open it during a run and "Stop run" would stay
/// enabled after the run ends. `visible` is untouched - nothing is re-ranked.
void refreshFlags(PaletteState& p, const TuiState& state) {
    const bool runActive = !state.readyStatus;
    const bool hasAnswer = answerForCopy(p, state);
    for (PaletteItem& item : p.items) {
        if (item.action == model::Action::CancelRun) {
            item.enabled = runActive;
        } else if (std::strcmp(item.id, kIdCopyAnswer) == 0) {
            item.enabled = hasAnswer;
        } else if (std::strcmp(item.id, kIdLoadEarlier) == 0) {
            // The thread's window moves without the table changing too (a session switch, a
            // page revealed by the thread's own row, an answer that arrives), and the live
            // answer is *not* one of the groups that window counts (task 38). One arithmetic:
            // the row is enabled exactly while `views::threadWindow()` hides something.
            item.enabled = threadHiddenGroups(state) != 0;
        } else if (isAskRow(item)) {
            item.enabled = !runActive;
            item.description = askDescription(!runActive);
        }
    }
}

/// Rank `items` into `visible` for `query` and the current group filter.
///
/// Called on open and on every query/group change - never per frame (design
/// §4.12: "matching runs over the model, not per draw"). Two rules and nothing else:
///
///   * the *sections* keep their order (ACTIONS before SESSIONS before COMMANDS before
///     DEVELOPER before ASK), so a query narrows the list without rearranging it - the thing a
///     user learns after one look at the palette stays true while they type;
///   * inside a section the best score leads, and the table order breaks a tie (a stable sort
///     over rows that already sit in the intended order).
///
/// The free-text row is the last section by construction, so it is pinned last whatever its
/// score: it is the fallback, not a result.
void rankItems(PaletteState& p, const std::string& query) {
    struct Scored {
        int section = 0;
        int score = 0;
        int index = 0;
    };
    std::vector<Scored> scored;
    scored.reserve(p.items.size());
    // The filter's section, guarded: `groupIndex` is public state, and an index past the table
    // would read `kGroupCycle` out of bounds (the renderer's `groupName()` guards it too).
    const int filtered = (p.groupIndex >= 1 && p.groupIndex <= kSectionCount) ? p.groupIndex : 0;
    for (int i = 0; i < static_cast<int>(p.items.size()); ++i) {
        const PaletteItem& item = p.items[static_cast<std::size_t>(i)];
        if (filtered > 0 &&
            std::strcmp(item.group == nullptr ? "" : item.group, kGroupCycle[filtered]) != 0) {
            continue;
        }
        // One matcher for every row, from session_fuzzy.h - the same matcher the
        // rail's filter uses. A session row is matched on its title *and* its
        // preview (fuzzyScoreFields: the rail's own call, title weighted 2x); an
        // action row only on its label. Descriptions are prose: subsequence
        // matching over a sentence makes "alp" hit "Stop the running agent
        // (partial output is kept)", which is noise, not a result. An empty query
        // matches everything in table order.
        int score = 0; // the empty query matches everything in table order
        if (!query.empty()) {
            score = groupIs(item, kGroupSessions)
                        ? fuzzyScoreFields(query, item.label, item.description)
                        : fuzzyScore(query, item.label);
        }
        if (score < 0)
            continue;
        scored.push_back({sectionRank(item.group), score, i});
    }
    std::stable_sort(scored.begin(), scored.end(), [](const Scored& a, const Scored& b) {
        if (a.section != b.section)
            return a.section < b.section;
        return a.score > b.score;
    });
    p.visible.clear();
    p.visible.reserve(scored.size());
    for (const Scored& entry : scored)
        p.visible.push_back(entry.index);
    const int lastRow = std::max(0, static_cast<int>(p.visible.size()) - 1);
    p.selected = std::clamp(p.selected, 0, lastRow);
    p.firstVisible = std::clamp(p.firstVisible, 0, lastRow);
}

/// Rebuild the table from the model and rank it. `openPalette` uses this for the
/// caller-supplied items, renderPalette for the query-change rebuild.
///
/// A new query is a new list, so the selection goes back to its first row: without that a user who
/// walked to row 18 and then typed would press Enter on the 5th best match instead of the best
/// (the review's M2). `selectGroup()` resets it for the section filter; `rankItems()` itself only
/// clamps, because it also runs on the per-frame flag refresh path.
void rebuild(PaletteState& p, TuiState& state, const std::string& query) {
    p.itemIds.clear();
    p.items = paletteActions(state, query, p.itemIds);
    p.builtQuery = query;
    p.selected = 0;
    p.firstVisible = 0;
    rankItems(p, query);
}

/// Tab / Shift+Tab: the next group with at least one row, wrapping. A group with no rows in this
/// query (ASK while the query is empty, DEVELOPER in a build without the surface) is skipped -
/// cycling into an empty section would look like the palette lost its table.
void cycleGroup(PaletteState& p, int direction) {
    for (int step = 1; step <= kSectionCount; ++step) {
        int next = (p.groupIndex + direction * step) % (kSectionCount + 1);
        if (next < 0)
            next += kSectionCount + 1;
        const char* name = kGroupCycle[next];
        bool hasRows = false;
        for (const PaletteItem& item : p.items) {
            if (name[0] == '\0' ||
                std::strcmp(item.group == nullptr ? "" : item.group, name) == 0) {
                hasRows = true;
                break;
            }
        }
        if (hasRows) {
            selectGroup(p, name);
            return;
        }
    }
}

/// Where the overlay sits and how many result rows it shows. Pure geometry: the
/// shell's layout is never consulted and never written.
struct Geometry {
    ImVec2 pos = ImVec2(0.0f, 0.0f);
    float width = 0.0f;
    float height = 0.0f;
    float frameT = kPaletteFrame;
    float pad = 0.0f;   // the overlay's inner padding (window padding)
    float lineH = 1.0f; // one text line (a title / hint row)
    float gap = 0.0f;   // vertical spacing between blocks
    float rowH = 1.0f;  // one result row
    float queryH = 1.0f;
    int rows = 1; // result rows in the viewport (1 = the "no matches" row)
    float listTop = 0.0f;
    float listH = 0.0f;
};

/// Where the overlay's contents sit inside the box `ui::computeShell()` answered
/// for it (task 17): the box is as large as the palette may become, the sheet
/// shrinks to its own content inside it, and it is centred there. Pure geometry -
/// this file never reads the display size and never writes the shell's layout.
///
/// `textGrid` is the profile, resolved once in renderPalette from
/// `ui::metricsFor()` and passed down as data: this file never asks the backend
/// which renderer is live (D-METRICS).
Geometry paletteGeometry(const PaletteState& p, bool textGrid, const ui::Rect& box) {
    Geometry g;
    g.pad = textGrid ? 0.0f : ImGui::GetStyle().WindowPadding.x;
    g.lineH = textGrid ? 1.0f : ImGui::GetTextLineHeight();
    g.gap = textGrid ? 0.0f : ImGui::GetStyle().ItemSpacing.y;
    g.rowH = textGrid ? 1.0f : ImGui::GetFrameHeight() + g.gap;
    g.queryH = textGrid ? 1.0f : ImGui::GetFrameHeight();
    const int cap = textGrid ? kTextRows : kGuiRows;
    const int matches = static_cast<int>(p.visible.size());
    g.rows = std::max(1, std::min(cap, matches));
    // The box states the width (80 % of the grid, 640 px in the pixel profile, or
    // 70 % of a narrow window); the title row, the query field, the list and the
    // hint row are content-sized, so the grid never shows a dead band.
    g.width = box.w;
    const float chromeH =
        g.lineH + g.gap + g.queryH + g.gap + g.gap + g.lineH + 2.0f * (g.frameT + g.pad);
    float height = chromeH + static_cast<float>(g.rows) * g.rowH;
    if (height > box.h) {
        g.rows = std::max(1, static_cast<int>(std::floor((box.h - chromeH) / g.rowH)));
        height = chromeH + static_cast<float>(g.rows) * g.rowH;
    }
    g.height = std::min(height, box.h);
    g.pos = ImVec2(box.x + std::round((box.w - g.width) * 0.5f),
                   box.y + std::round((box.h - g.height) * 0.5f));
    g.listTop = g.pos.y + g.frameT + g.pad + g.lineH + g.gap + g.queryH + g.gap;
    g.listH = static_cast<float>(g.rows) * g.rowH;
    return g;
}

/// Selection and group keys, handled before the frame is drawn so the drawn
/// frame shows their effect.
///
/// `Esc` is deliberately absent: closing the overlay is the first step of the
/// D-ESC chain, and the frame-level keymap dispatcher performs it (tui.cpp).
/// One place decides a press, so a key that closes the overlay can never also
/// act on the region behind it.
void handleNavKeys(PaletteState& p) {
    const ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsKeyPressed(ImGuiKey_Tab, false))
        cycleGroup(p, io.KeyShift ? -1 : 1);
    // Up/Down are the pixel profile's binding; the text backend delivers them as
    // Esc + "[A"/"[B" (plan/key_gate.md §4b), so the grid's proven path is
    // Ctrl+P/Ctrl+N (§4c). Both work here, in both backends.
    const bool previous = ImGui::IsKeyPressed(ImGuiKey_UpArrow, false) ||
                          (ImGui::IsKeyPressed(ImGuiKey_P, false) && io.KeyCtrl);
    const bool next = ImGui::IsKeyPressed(ImGuiKey_DownArrow, false) ||
                      (ImGui::IsKeyPressed(ImGuiKey_N, false) && io.KeyCtrl);
    const int lastRow = static_cast<int>(p.visible.size()) - 1;
    if (previous && lastRow >= 0)
        p.selected = std::max(0, p.selected - 1);
    if (next && lastRow >= 0)
        p.selected = std::min(lastRow, p.selected + 1);
}

/// A surface that is still up *under* the palette: the settings sheet (the palette opens above it)
/// or the Raw Log's full-screen overlay. Closing the palette hands the keyboard to whichever of
/// them is up (`views::returnKeyboardBelow()`), so a row that promises the composer the keyboard
/// cannot keep that promise while one of them is open.
bool surfaceUnderPalette(const TuiState& state) {
    return state.settingsSheet.open || (state.ui.rawLogOpen && state.rawLog.overlayShape);
}

/// Submit `text` exactly as the composer would (design §6.2's submit handshake, one
/// implementation): drop one trailing newline, refuse whitespace-only, let the UI's own
/// `/rawlog` intercept take the line, cap the history the way every other submit path does
/// (`views::pushHistory`), then hand the query to the frame. The palette's query is a typed line,
/// so a command row and a typed `/continue` reach D's registry through the same door rather than a
/// second one - and the composer's own buffer is not part of the transaction (see below).
bool submitLine(TuiState& state, const std::string& text) {
    std::string query;
    if (!prepareSubmit(text, query))
        return false;
    if (handleRawLogCommand(state, query))
        return true; // a UI command: never a query, never in the history
    state.userQuery.submitReady = true;
    state.userQuery.submitQuery = query;
    // The draft belongs to the composer, which is the only place that clears a buffer *it*
    // submitted: a palette line is not necessarily that draft, and clearing it unconditionally
    // swapped an unsent message for `/compact` (the review's I1). Leaving it is also the safer
    // half of the "the two are the same text" case - the field re-asserts its own copy when it
    // takes the keyboard back anyway, so a clear here would be a fight the palette loses.
    pushHistory(state, query);
    return true;
}

/// Run one row's command. Returns true when the overlay should close (every row that acts
/// closes it); false when the row cannot act in this state, which leaves the palette open.
///
/// Two halves, and the split is the registry's (model/keymap.h's `ActionReach`):
///   * a row whose `action` is not `None` performs that keymap action here, through the same
///     model call `tui.cpp`'s dispatcher makes - so a chord and its row are one implementation,
///     and `./test_palette` can walk the whole table;
///   * a row with `Action::None` is a palette-local command (a session pick, a command line, the
///     free-text fallback) or one of the four whose other entry point is a visible control (new
///     session, focus composer, copy last answer, open settings).
bool performRow(const PaletteItem& item, PaletteState& p, TuiState& state) {
    switch (item.action) {
    case model::Action::Quit:
        // Same policy as the quit chord (D-KEYMAP's "confirm while running"): a click must not
        // throw away a run the chord would ask about.
        tuiRequestQuitConfirmed(state);
        return true;
    case model::Action::CancelRun:
        // D-CANCEL: the same request `/stop` makes, which the D loop turns into `stopAgent()`;
        // the partial output is kept.
        tuiRequestCancel(state);
        return true;
    case model::Action::ToggleRail:
        // `Ctrl+B`'s rule (`model::toggleRail()`): the rail on screen becomes closed, anything
        // else becomes open, and `-1 auto` is never written back. Focus mode hides the rail
        // (task 43), so the row cannot act there either - the same predicate the chord reads.
        if (!model::regionControlsApply(state.ui))
            return false;
        model::toggleRail(state.ui.prefs, state.ui.rail.open);
        return true;
    case model::Action::LoadEarlier:
        // The same call the thread's own row and `PgUp`/`Ctrl+U` make (D-PALETTE: one action,
        // three entry points).
        requestEarlierPage(state.ui);
        return true;
    case model::Action::ToggleInspector:
        // One rule, one home: the same `model::toggleInspector()` the `Ctrl+I` row performs, with
        // the same *drawn* state as its input (`state.inspector.open`) - so the palette and the
        // chord cannot disagree, and the first press from `-1 auto` on a narrow display opens
        // the panel instead of writing a second "closed". Focus mode hides the panel (task 43), so
        // the row cannot act there either (`model::regionControlsApply`).
        if (!model::regionControlsApply(state.ui))
            return false;
        model::toggleInspector(state.ui.prefs, state.inspector.open);
        state.inspector.reservedPressed = false;
        return true;
    case model::Action::ToggleFocusMode:
        // The chord's own call (task 43): a mode of the shell, not a preference, so there is
        // nothing to persist and nothing to reconcile with `LayoutPrefs` - the next frame's
        // layout reads the flag.
        model::toggleFocusMode(state.ui);
        return true;
    case model::Action::InspectorAgents:
    case model::Action::InspectorActivity:
        // The same request a tab click and `Ctrl+5`/`Ctrl+6` make; `applyInspectorAnswers()`
        // turns it into the preference (and shows a closed panel). The palette draws after that
        // application point, so the answer lands on the next frame - like a session pick. Focus
        // mode hides the panel (task 43), so the row cannot act there either: the writer would
        // drop the request, and closing the palette on a request nobody keeps is not an answer.
        if (!model::regionControlsApply(state.ui))
            return false;
        state.inspector.requestTab(item.action == model::Action::InspectorAgents
                                       ? model::InspectorTab::Agents
                                       : model::InspectorTab::Activity);
        return true;
    case model::Action::ToggleRawLog:
        // The same call `/rawlog`, `Ctrl+Shift+L` and `--debug-ui` make (`views::toggleRawLog()`),
        // so the palette cannot become a second implementation of "open the developer surface".
        toggleRawLog(state);
        return true;
    case model::Action::Help:
        // One call: the page's own open (`views::openHelp()`), which is what the chord and the
        // typed `?` reach too. The palette closes on a performed row, so the page is the only
        // surface left up - and it is above the composer, where the row's user came from.
        openHelp(state.help);
        return true;
    case model::Action::None:
        break; // a palette-local row: handled below
    // The rest are not palette rows at all (`ActionReach` in model/keymap.h): the overlay's own
    // key, a scroll gesture, a field's history keys, the `Esc` chain's steps and the reserved
    // digit. A row can never carry one - the registry test asserts it - so this arm is a
    // dead-entry guard rather than a behaviour.
    case model::Action::Palette:
    case model::Action::CloseOverlay:
    case model::Action::ScrollEnd:
    case model::Action::ClearInput:
    case model::Action::Blur:
    case model::Action::ExitFocusMode:
    case model::Action::HistoryPrev:
    case model::Action::HistoryNext:
    case model::Action::InspectorReserved:
    case model::Action::Count:
        return false;
    }

    if (groupIs(item, kGroupSessions)) {
        // The select path the rail's own rows use: the answer is applied on the next frame by
        // `applyRailAnswers()` - queue when the store is ready, defer to the one slot while a
        // run is active (last click wins), mark the target pending and clear its unread dot -
        // exactly the answer the rail's own rows give, because the palette sits above the rail
        // in the same frame.
        state.ui.rail.selectId = item.id;
        return true;
    }
    if (std::strcmp(item.id, kIdNewSession) == 0) {
        // The row's other entry point is the rail's `+`, so it uses the rail's answer and not a
        // second one: `applyRailAnswers()` queues the `New` action when the store is ready and
        // drops the request while a run is active (the rail's own `+` refuses for the same
        // reason). The row is drawn disabled in that state; this is the belt to that braces.
        if (!state.readyStatus)
            return false;
        state.ui.rail.newRequested = true;
        return true;
    }
    if (std::strcmp(item.id, kIdFocusComposer) == 0) {
        // closePalette() hands the keyboard back through `views::returnKeyboardBelow()`, which
        // gives it to whatever is on top: with a settings sheet or a Raw Log overlay still open
        // *under* this palette, the press would re-arm that surface instead of reaching the
        // composer. The row is drawn disabled exactly then (`paletteActions()`), so this is the
        // belt to that braces; with nothing below, closing is the whole action.
        if (surfaceUnderPalette(state))
            return false;
        return true;
    }
    if (std::strcmp(item.id, kIdCopyAnswer) == 0) {
        // The copy target is a *block*, not a message id (D-PALETTE): the newest answer, or the
        // live streamed body while a run is still writing. `blockText()` is the one copy path, so
        // the palette and a block's own Copy button copy the same string - the body, without the
        // row's header decoration.
        std::size_t group = 0, block = 0;
        if (!lastAnswerBlock(state, group, block))
            return false;
        const std::string text = state.threadAdapter.blockText(group, block);
        if (!text.empty())
            ImGui::SetClipboardText(text.c_str());
        return true;
    }
    if (std::strcmp(item.id, kIdOpenSettings) == 0) {
        // The sheet's second entry point - the top bar's `Settings` control is the first, and
        // below the width breakpoint that bar folds away, which leaves this row as the only way
        // in (D-PALETTE). Closing the palette is part of the activation, and `closePalette()`
        // hands the keyboard to whichever surface is on top: the sheet, never the composer.
        openSettings(state.settingsSheet);
        return true;
    }
    if (groupIs(item, kGroupCommands)) {
        // A command row *is* its command line: the label is the text the row shows and the text
        // it submits (`paletteActions()` builds both from the same literal), so the list cannot
        // advertise a command it does not send. The submit path is the composer's, `/rawlog`
        // intercept included, so a command row and a typed command line are one behaviour.
        return submitLine(state, item.label);
    }
    if (isAskRow(item)) {
        // The free-text fallback (Q-01, decided here: "send the query" is final - the palette is
        // the discoverability layer and a modifier would make it less useful than the composer it
        // sits above). The query is submitted as if the composer had: the same handshake, the same
        // `/rawlog` intercept - so a `/`-seeded query arrives at D as the command line it looks
        // like, not as the word behind the slash.
        //
        // No second query while one is running: the composer's action is `Stop` then and
        // retryLastQuery refuses for the same reason, so the guard lives here too (the row is
        // drawn disabled; this is the belt to that braces).
        if (!state.readyStatus)
            return false;
        const std::string query = p.query.data();
        return query.empty() ? true : submitLine(state, query);
    }
    return false;
}

} // namespace

void selectGroup(PaletteState& p, const char* group) {
    p.groupIndex = groupIndexOf(group != nullptr ? group : "");
    p.selected = 0;
    p.firstVisible = 0;
    rankItems(p, p.builtQuery);
}

bool palettePerformRow(const PaletteItem& item, PaletteState& p, TuiState& state) {
    // One dispatcher: `Enter`, a click and the registry test all come through here, so a row
    // that cannot act is a `false` in one place and nowhere else.
    return performRow(item, p, state);
}

/// Groups the thread's bounded window currently hides: computed with the same
/// arithmetic `views::renderThread()` pages with, so the palette's row and the
/// thread's own row cannot disagree about whether there is anything older.
///
/// The count is `storedGroupCount()`, not `groupCount()` (task 38): the live answer is drawn
/// *after* the window and is not part of it, so counting it would enable this row one group too
/// early - a `Load earlier` that pages nothing while the thread's own row is already hidden.
std::size_t threadHiddenGroups(const TuiState& state) {
    const std::size_t groups = state.threadAdapter.storedGroupCount();
    return threadWindow(groups, state.ui.thread.pageGroups, state.ui.earlierGroups).hidden;
}

/// The agent's core commands (`source/llm/app_agent/slash_core.d`), most useful first: the `/`
/// entry point exists so a typed `/continue` survives, so that command leads. The aliases live in
/// the description - the row sends the long form, which is what the registry's help prints.
///
/// The table is public through `commandTable()` because the help page lists the same commands
/// (task 42): one table, two readers, so the page cannot describe a command the palette cannot
/// send.
constexpr CommandRow kCommands[] = {
    {"/continue", "Continue processing the last query (/c, /cont)"},
    {"/stop", "Stop the active query (partial output is kept)"},
    {"/compact", "Compress the chat history"},
    {"/help", "Show the available commands"},
    {"/debug", "Toggle verbose debug output"},
    {"/quit", "Exit the agent (/q, /exit)"},
    {"/rawlog", "Show or hide the developer log"},
};

constexpr CommandList kCommandList{kCommands, sizeof(kCommands) / sizeof(kCommands[0])};

const CommandList& commandTable() { return kCommandList; }

std::vector<PaletteItem> paletteActions(const TuiState& state, const std::string& query,
                                        std::deque<std::string>& idStorage) {
    std::vector<PaletteItem> items;
    const bool runActive = !state.readyStatus;
    const bool hasAnswer = hasAnswerToCopy(state);

    // The ACTIONS group, in confidence order. Every row is also a key binding or a visible
    // control (D-PALETTE), and each description names that other entry point where there is one,
    // so a reader of the list can check the claim: the rail's `+` and the composer field are the
    // two rows whose control is a region rather than a widget, and the settings sheet's control is
    // the top bar's `Settings` entry.
    items.push_back({kIdNewSession, kGroupActions, "New session", "Start a fresh session (rail +)",
                     state.readyStatus, model::Action::None});
    items.push_back({kIdFocusComposer, kGroupActions, "Focus composer",
                     "Move the keyboard to the query field", !surfaceUnderPalette(state),
                     model::Action::None});
    // The side regions' controls are disabled while focus mode hides the regions they act on
    // (task 43): `model::regionControlsApply` is the same predicate the chords read, so a press and
    // a row cannot disagree about a control that is off screen either way. That includes the two
    // tab rows below: a tab press writes the panel's tab and shows the panel, and in the mode there
    // is no panel for it to show.
    items.push_back({kIdToggleRail, kGroupActions, "Toggle rail",
                     "Show or hide the session list (Ctrl+B)", model::regionControlsApply(state.ui),
                     model::Action::ToggleRail});
    // The inspector's toggle (task 29). A real control as well as a chord: `Ctrl+I` reaches
    // the pixel profile and not the terminal (plan/key_gate.md §3b), so this row is the grid's
    // path to the panel - and the panel has no control of its own while it is closed.
    items.push_back({kIdToggleInspector, kGroupActions, "Toggle inspector",
                     "Show or hide the agents and activity panel (Ctrl+I)",
                     model::regionControlsApply(state.ui), model::Action::ToggleInspector});
    // The two tabs (task 29): `Ctrl+5`/`Ctrl+6` are the pixel profile's chords, and the panel's
    // scope is the point of the row - a user who wants the trace stream should not have to open
    // the panel and then find the tab. Focus mode hides that panel (task 43), so the row goes with
    // its siblings: a request the frame would drop is not an enabled row.
    items.push_back({kIdInspectorAgents, kGroupActions, "Agents tab",
                     "Show the inspector's Agents tab (Ctrl+5)",
                     model::regionControlsApply(state.ui), model::Action::InspectorAgents});
    items.push_back({kIdInspectorActivity, kGroupActions, "Activity tab",
                     "Show the inspector's Activity tab (Ctrl+6)",
                     model::regionControlsApply(state.ui), model::Action::InspectorActivity});
    // Focus mode (task 43): `F11` reaches the pixel profile and `Ctrl+Shift+M` arrives as `Enter`
    // in the terminal (plan/key_gate.md §3b), so this row is the grid's way in - and `Esc` is the
    // way out, which is the one thing a mode that hides the chrome must always keep.
    items.push_back({kIdFocusMode, kGroupActions, "Focus mode",
                     "Hide the side panels and give the thread the whole window (F11; Esc leaves)",
                     true, model::Action::ToggleFocusMode});
    // The settings sheet (task 34): the top bar's `Settings` entry is its other entry point, and
    // that bar folds away below the width breakpoint - so here this row is the only way in.
    items.push_back({kIdOpenSettings, kGroupActions, "Open settings",
                     "Send key, log level, text scale and the diagnostics", true,
                     model::Action::None});
    // The help page (task 42). `Shift+/` is the page's key, but with the composer focused that
    // chord is the `?` the user is typing (the keymap's text-field rule), so this row is the path
    // that works from the field the user is normally in.
    items.push_back({kIdHelp, kGroupActions, "Show help",
                     "The key table, the commands and what each region does (?)", true,
                     model::Action::Help});
    items.push_back({kIdCopyAnswer, kGroupActions, "Copy last answer",
                     "Copy the newest answer to the clipboard", hasAnswer, model::Action::None});
    // `Stop run` is `Esc`'s second step and the composer's Stop button as well (D-CANCEL): one
    // request, three entry points.
    items.push_back({kIdStopRun, kGroupActions, "Stop run",
                     "Stop the running agent (partial output is kept)", runActive,
                     model::Action::CancelRun});
    // `Load earlier` is a real control in the thread (the focusable row above the groups) and two
    // chords in the keymap. Enabled only while the bounded window hides something, which
    // `threadHiddenGroups()` answers with the thread's own arithmetic.
    items.push_back({kIdLoadEarlier, kGroupActions, "Load earlier",
                     "Show the groups older than the window (PgUp)", threadHiddenGroups(state) != 0,
                     model::Action::LoadEarlier});
    items.push_back(
        {kIdQuit, kGroupActions, "Quit", "Close the UI (Ctrl+Q)", true, model::Action::Quit});
    // The Raw Log (task 33). Its own group, because the design keeps the developer surface out
    // of the normal actions: a user browsing `ACTIONS` should not stumble into a trace stream.
    items.push_back({kIdToggleRawLog, kGroupDeveloper, "Raw Log",
                     "Show or hide the developer log (/rawlog)", true,
                     model::Action::ToggleRawLog});

    // One row per live session, from the snapshot the panel renders. The ids are
    // runtime strings, so `idStorage` owns them (a deque: stable addresses).
    for (const SessionEntry& entry : state.sessionPanel.sessions) {
        std::string description;
        if (entry.isActive)
            description = "active - ";
        if (!entry.preview.empty()) {
            description += entry.preview;
        } else {
            description += std::to_string(entry.messageCount);
            description += entry.messageCount == 1 ? " message" : " messages";
        }
        idStorage.push_back(entry.id);
        items.push_back({idStorage.back().c_str(), kGroupSessions, entry.title, description, true,
                         model::Action::None});
    }

    // The command lines. They are always in the table (not only for a `/`-seeded query): a user
    // who opened `Ctrl+K` and types `/` should find them by typing, and the section is where the
    // design puts them. The id is derived from the line, so a row and its command cannot drift.
    for (const CommandRow& command : kCommands) {
        idStorage.push_back(std::string(kIdCommandPrefix) + (command.label + 1));
        items.push_back({idStorage.back().c_str(), kGroupCommands, command.label,
                         command.description, true, model::Action::None});
    }

    // The free-text fallback exists only when there is something to send; with
    // an empty query it would read `Send ""` and could not be activated. It is
    // disabled while a run is active - the composer's action is `Stop` then, and
    // a second query would bypass the same guard retryLastQuery applies.
    if (!query.empty()) {
        items.push_back({kIdFreeText, kGroupAsk, "Send \"" + query + "\"",
                         askDescription(state.readyStatus), state.readyStatus,
                         model::Action::None});
    }
    return items;
}

void openPalette(PaletteState& p, std::vector<PaletteItem> items, const char* initialQuery) {
    p.open = true;
    p.items = std::move(items);
    const std::string query = initialQuery != nullptr ? initialQuery : "";
    std::snprintf(p.query.data(), p.query.size(), "%s", query.c_str());
    p.selected = 0;
    p.groupIndex = 0;
    p.firstVisible = 0;
    p.builtQuery = query;
    rankItems(p, query);
}

void openPaletteForState(PaletteState& p, TuiState& state, const char* initialQuery) {
    // Item ids that are not literals must be owned by this state; clear them
    // before the table that points into them is replaced.
    p.itemIds.clear();
    p.items.clear();
    openPalette(p, paletteActions(state, initialQuery != nullptr ? initialQuery : "", p.itemIds),
                initialQuery);
}

void openPaletteForCommands(PaletteState& p, TuiState& state, const char* seed) {
    const std::string query = seed != nullptr ? seed : "";
    openPaletteForState(p, state, query.c_str());
    // The COMMANDS section, by name: `selectGroup()` is the one lookup, so `Tab`'s numbering and
    // this entry point cannot drift apart.
    selectGroup(p, kGroupCommands);
    // The seed is a prefix the user is about to continue, not a draft to replace: the caret goes
    // after it once the field owns the keyboard (ImGui selects the whole buffer when a field is
    // activated by the keyboard, which would make the first typed character replace the `/` -
    // measured). With an empty seed there is nothing to protect.
    p.seedCaretPending = !query.empty();
}

void closePalette(PaletteState& p, TuiState& state) {
    p.open = false;
    p.items.clear();
    p.visible.clear();
    p.itemIds.clear();
    p.query.fill('\0');
    p.builtQuery.clear();
    p.selected = 0;
    p.groupIndex = 0;
    p.firstVisible = 0;
    // The seed goes with the rest of the query: a stale caret request must not outlive the field
    // it was made for.
    p.seedCaretPending = false;
    // Hand the keyboard back through the one rule every closing overlay uses: a Raw Log overlay or
    // a settings sheet still open under this palette is the next top surface and takes the focus
    // itself; only when nothing else is up does the composer get the one-shot request (the same
    // flag the Chat menu and the starter actions use, consumed before its submit branch, so nothing
    // is sent). The workbench composer has its own latch (`views::ComposerState::focusRequested`),
    // because it owns the field's state rather than reading v4's edit flags.
    views::returnKeyboardBelow(state);
}

bool renderPalette(PaletteState& p, TuiState& state, const ui::Rect& overlay) {
    if (!p.open)
        return false;

    // Query change -> rebuild and re-rank. Never per frame.
    const std::string query = p.query.data();
    if (query != p.builtQuery)
        rebuild(p, state, query);
    // The row *flags* move without the table changing (a run starts or ends, an
    // answer arrives): refresh those, never the ranking.
    refreshFlags(p, state);

    handleNavKeys(p);

    // The one profile read of this file, and it comes from the metrics module
    // (task 13): everything below takes it as data. The box comes from the layout
    // engine (task 17), so the overlay's geometry has one owner.
    const bool textGrid = ui::metricsFor().textGrid;
    const Geometry g = paletteGeometry(p, textGrid, overlay);
    const float innerW = g.width - 2.0f * (g.frameT + g.pad);

    // Scroll window: follows the selection, and only ever moves when the
    // selection leaves it (so the list does not jump while walking inside).
    const int count = static_cast<int>(p.visible.size());
    int first = p.firstVisible;
    if (p.selected < first)
        first = p.selected;
    if (p.selected >= first + g.rows)
        first = p.selected - g.rows + 1;
    first = std::clamp(first, 0, std::max(0, count - g.rows));
    p.firstVisible = first;

    // A click into another window means the user left the palette; a visible
    // overlay that no longer owns the keyboard would swallow the next lookup.
    // The check runs *before* the window is Begun, so the frame that closes it
    // paints nothing at all - closing after Begin would flash the panel for one
    // frame. Only a window that was up on the *previous* frame can have lost the
    // focus: on the opening frame the name still resolves to the stale window of
    // the last time the palette was open (SetWindowFocus below is what takes it).
    // `NavWindow == nullptr` means "no window owns the keyboard yet", which is
    // not "the user clicked away".
    if (ImGuiWindow* self = ImGui::FindWindowByName("##palette")) {
        ImGuiContext& ctx = *ImGui::GetCurrentContext();
        if (self->LastFrameActive + 1 == ctx.FrameCount) {
            const bool focused =
                ctx.NavWindow == self || ImGui::IsWindowChildOf(ctx.NavWindow, self, false);
            if (ctx.NavWindow != nullptr && !focused) {
                closePalette(p, state);
                return false;
            }
        }
    }

    ImGui::SetNextWindowPos(g.pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(g.width, g.height), ImGuiCond_Always);
    // Square corners and an opaque surface: the overlay is a panel, and the
    // surface colour is what separates it from the thread behind it in a
    // terminal (the canvas and the surface tokens are one step apart on purpose).
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::GetStyleColorVec4(ImGuiCol_ChildBg));
    const ImGuiWindowFlags windowFlags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoSavedSettings;
    ImGui::Begin("##palette", nullptr, windowFlags);
    // The overlay must be drawn last, i.e. be the front-most window: windows
    // render in display order, so a merely-later Begin still ends up under the
    // shell's children, which the root re-focused on its own first frame. Taking
    // the focus once, on the frame the overlay appears, is what moves it to the
    // front - and a later click elsewhere takes the focus away, which is the
    // "focus loss closes the overlay" check above.
    if (ImGui::IsWindowAppearing())
        ImGui::SetWindowFocus();
    // The style variables/colours above are captured by Begin; restore them for
    // the palette's own widgets.
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);

    const ImVec2 windowPos = ImGui::GetWindowPos();
    const float x0 = windowPos.x + g.frameT + g.pad;
    const float y0 = windowPos.y + g.frameT + g.pad;

    // Frame: four filled edges, not an AddRect stroke - a 1 px stroke straddles
    // the boundary and paints two cells in the grid (the composer's lesson).
    // The outline is an essential boundary, so it is the design's `line.strong`
    // token (3.1:1 on the surface it bounds) rather than a stepped-up hairline:
    // a derivation here would be a colour no token owns (D-THEME).
    {
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const ImU32 edge = ImGui::GetColorU32(ui::color(ui::ColorToken::lineStrong));
        const ImVec2 maxPos(windowPos.x + g.width, windowPos.y + g.height);
        drawList->AddRectFilled(windowPos, ImVec2(maxPos.x, windowPos.y + g.frameT), edge);
        drawList->AddRectFilled(ImVec2(windowPos.x, maxPos.y - g.frameT), maxPos, edge);
        drawList->AddRectFilled(windowPos, ImVec2(windowPos.x + g.frameT, maxPos.y), edge);
        drawList->AddRectFilled(ImVec2(maxPos.x - g.frameT, windowPos.y), maxPos, edge);
    }

    // Title row: `Search` on the left, the group filter and the match count on
    // the right. The group name is the only place the active section is shown
    // (sections are ordered in the list, per-row tags carry the rest).
    ImGui::SetCursorScreenPos(ImVec2(x0, y0));
    ImGui::TextUnformatted("Search");
    {
        const std::string section =
            std::string(groupName(p.groupIndex)) + " " + std::to_string(count);
        const float sectionW = ImGui::CalcTextSize(section.c_str()).x;
        ImGui::SetCursorScreenPos(ImVec2(x0 + innerW - sectionW, y0));
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextUnformatted(section.c_str());
        ImGui::PopStyleColor();
    }

    // Query field. Opening the overlay asks for the keyboard once - the same
    // one-shot nav request the composer uses, resolved on the next frame.
    const float queryY = y0 + g.lineH + g.gap;
    ImGui::SetCursorScreenPos(ImVec2(x0, queryY));
    if (ImGui::IsWindowAppearing())
        ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(innerW);
    ImGui::InputText("##palette_query", p.query.data(), p.query.size());
    // ImGui paints the nav cursor around the focused widget; on the 1-cell grid
    // that is a full-cell ring that outlives the frame in the terminal. The
    // caret marks the focus, so hide it in the grid only (the composer's rule).
    if (textGrid) {
        ImGuiContext& ctx = *ImGui::GetCurrentContext();
        const ImGuiID queryId = ImGui::GetID("##palette_query");
        if (ctx.ActiveId == queryId || ctx.NavId == queryId)
            ctx.NavCursorVisible = false;
    }
    // A seeded query is a prefix, not a draft. ImGui selects the whole buffer when a field is
    // activated by the keyboard (`input_requested_by_nav`), so the first typed character would
    // replace the `/` the composer handed over - measured: opening for commands with `/` and
    // typing `continue` gave `continue`. Put the caret after the seed the first frame the field
    // owns the keyboard; a click later in the field is the user's own caret move and must not be
    // overruled, so this runs once and only for the seed.
    if (p.seedCaretPending) {
        if (ImGuiInputTextState* field = ImGui::GetInputTextState(ImGui::GetItemID())) {
            const int seedLength = static_cast<int>(query.size());
            field->SetSelection(seedLength, seedLength);
            p.seedCaretPending = false;
        }
    }

    // List: a child window so the one-row overscan is clipped, and so the drawn
    // window is measurable (its content size is `rows` or `rows + 1` rows, never
    // one row per match).
    int clicked = -1;
    // Reused by every row's highlight: the shared helper takes a caller-owned
    // scratch vector and run list, so drawing a list allocates nothing per row.
    std::vector<std::size_t> matchScratch;
    std::vector<std::pair<std::size_t, std::size_t>> matchRuns;
    ImGui::SetCursorScreenPos(ImVec2(x0, g.listTop));
    const bool listBegun =
        ImGui::BeginChild("##palette_list", ImVec2(innerW, g.listH), 0,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    if (listBegun) {
        if (count == 0) {
            ImGui::SetCursorScreenPos(ImVec2(x0, g.listTop));
            ImGui::TextDisabled("no matches");
        } else {
            const int lastDrawn = std::min(count, first + g.rows + 1);
            for (int row = first; row < lastDrawn; ++row) {
                const int index = p.visible[static_cast<std::size_t>(row)];
                const PaletteItem& item = p.items[static_cast<std::size_t>(index)];
                const int slot = row - first;
                const float rowY = g.listTop + static_cast<float>(slot) * g.rowH;
                // `p.selected` indexes `p.visible`, so it is compared with the
                // *list* row, never with the drawn slot: they differ as soon as
                // the window has scrolled, and comparing the slot puts the
                // highlight on the row after the one Enter would run.
                const bool selected = (row == p.selected);

                ImGui::PushID(index);
                ImGui::SetCursorScreenPos(ImVec2(x0, rowY));
                const ImGuiSelectableFlags flags =
                    item.enabled ? ImGuiSelectableFlags_None : ImGuiSelectableFlags_Disabled;
                if (ImGui::Selectable("##row", selected, flags, ImVec2(innerW, g.rowH)))
                    clicked = index;
                ImGui::PopID();

                // Right-aligned section tag. Drawn first so the label's clip
                // budget is known in the grid.
                const char* tag = item.group != nullptr ? item.group : "";
                const float tagW = ImGui::CalcTextSize(tag).x;
                const float tagX = x0 + innerW - tagW;
                std::string text = item.label;
                if (!item.description.empty()) {
                    text += "  ";
                    text += item.description;
                }
                if (textGrid) {
                    // Cells, not pixels: the tag keeps its column and the label
                    // is cut with an ellipsis instead of running under it.
                    const int budget =
                        static_cast<int>(innerW) - static_cast<int>(std::strlen(tag)) - 1;
                    text = fitText(text, budget);
                }
                const float textY = rowY + (g.rowH - g.lineH) * 0.5f;
                ImGui::SetCursorScreenPos(ImVec2(x0, textY));
                if (!textGrid) {
                    // The pixel profile may hold a long description; clip it one
                    // gap before the tag instead of letting it run underneath.
                    ImGui::PushClipRect(ImVec2(x0, rowY), ImVec2(tagX - 4.0f, rowY + g.rowH), true);
                }
                if (item.enabled)
                    ImGui::TextUnformatted(text.c_str());
                else
                    ImGui::TextDisabled("%s", text.c_str());
                if (!textGrid)
                    ImGui::PopClipRect();

                // Matched words over-drawn in `accent`, the app's "this one" ink (character
                // snapped, word expanded, runs merged by `fuzzyHighlightRuns`), clipped to the
                // label bytes this row actually draws.
                if (!query.empty()) {
                    const std::size_t labelLen = std::min(item.label.size(), text.size());
                    fuzzyHighlightRuns(query, item.label, labelLen, matchScratch, matchRuns);
                    const char* drawn = text.c_str();
                    for (const auto& run : matchRuns) {
                        const float runX = x0 + ImGui::CalcTextSize(drawn, drawn + run.first).x;
                        ImGui::SetCursorScreenPos(ImVec2(runX, textY));
                        ImGui::PushStyleColor(ImGuiCol_Text, ui::color(ui::ColorToken::accent));
                        ImGui::TextUnformatted(drawn + run.first, drawn + run.second);
                        ImGui::PopStyleColor();
                    }
                }

                ImGui::SetCursorScreenPos(ImVec2(tagX, textY));
                ImGui::PushStyleColor(ImGuiCol_Text,
                                      ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                ImGui::TextUnformatted(tag);
                ImGui::PopStyleColor();
            }
        }
    }
    // EndChild is unconditional: ImGui requires the pair even on the frame a
    // fully clipped child made BeginChild return false.
    ImGui::EndChild();

    // Hint row: the keys the overlay answers to, in the backend's own vocabulary.
    ImGui::SetCursorScreenPos(ImVec2(x0, g.listTop + g.listH + g.gap));
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextUnformatted(textGrid ? "Esc close   Enter run   Tab group   ^N/^P move"
                                    : "Esc close   Enter run   Tab group   Ctrl+N/Ctrl+P move");
    ImGui::PopStyleColor();

    ImGui::End();

    // Activation last: a click names its row directly, Enter takes the selection.
    int activation = clicked;
    if (activation < 0 && ImGui::IsKeyPressed(ImGuiKey_Enter, false) && count > 0)
        activation = p.visible[static_cast<std::size_t>(p.selected)];
    if (activation < 0)
        return false;
    const PaletteItem& item = p.items[static_cast<std::size_t>(activation)];
    if (!item.enabled)
        return false;
    if (palettePerformRow(item, p, state)) {
        closePalette(p, state);
        return true;
    }
    return false;
}

} // namespace llmfun::tui::views
