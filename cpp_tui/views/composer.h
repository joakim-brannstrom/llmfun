/// The composer (`views/composer.cpp`): the only input path.
///
///     ┌──────────────────────────────────────────────────────────────┐
///     │ Ask anything...  / commands                              Plan │  text area
///     │ [Prev][Next]                                       [Send] │  meta row
///     └──────────────────────────────────────────────────────────────┘
///
/// One container (1 cell on the text grid, 6 px radius and a `raised` fill in the
/// pixel profile), a text area that auto-grows 1..8 rows, a meta row carrying the
/// history buttons (`Prev`/`Next`, text grid only - the terminal backend delivers
/// neither arrow) and the one real action (right, `Send`/`Stop`/`Stopping`/
/// `Insert`), and the mode text right-aligned on the text line (the context meter,
/// the model name, the phase and the speed belong to the now bar, the row that owns
/// the run's numbers and identity).
///
/// Contract: a view is a geometry taker. It receives `ShellLayout::composer` and a
/// `Metrics`, reads no display size, no profile flag and no aggregate, so it draws
/// the same in both profiles and a harness can render it into a cell grid. The one
/// thing it *reports upwards* is its own height: auto-grow has to reach the layout
/// engine (`LayoutPrefs::composerH`), or the thread would draw under it. The shell
/// hands that number back on the next frame (`ComposerState::height`).
///
/// What the view owns: the field's keyboard latch, the history cursor, and the
/// paste question. What it does **not** own: the draft (`UserQueryState::inputBuf`),
/// the history list, or the submit handshake - those are the shipped store and the
/// frame layer applies the answers (`ComposerState::submit`), so the field and the
/// store cannot disagree about what a send means.
///
/// Layering: `ui/` + imgui (+ `model/settings.h` for the send-key word), no `tui.h`,
/// no `model/ui_state.h`, no v4 type - which is what lets `./test_composer` draw the
/// composer into a cell grid with no application state. `views/shell.cpp` is where
/// the aggregate is turned into a `ComposerInput`.
///
/// Decisions this file implements: paste is captured, folded and asked
/// about, never submitted; the `Esc` chain's last two steps are clear and blur,
/// and this view is its entry point; with no gate match, `Enter` is a
/// newline and the action button sends; and every colour and distance
/// is a token.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

// TODO: relative paths like this is not OK. Fix
// Relative, not `"model/settings.h"`: this header travels inside `tui.h`, and several
// test targets include `tui.h` with no `cpp_tui/` on the include path (they rely on a
// header resolving its own neighbours - the same reason `../store/thread_store.h`
// names `../model/block.h`). A `"../"` include is resolved relative to *this* file
// first, so it works with and without that path.
#include "../model/settings.h"

// Forward declarations, not includes (the palette header's rule): this header
// travels inside `TuiState`, and a translation unit that reaches `tui.h` must not
// need the `cpp_tui` include root just because a *view* was mentioned there.
namespace llmfun::tui::ui {
struct Metrics;
struct Rect;
} // namespace llmfun::tui::ui

namespace llmfun::tui {
/// Forward declaration: `pushHistory()` takes the frame aggregate by reference, so this header
/// stays reachable without `tui.h` (the same rule the `ui/` types above follow).
struct TuiState;
} // namespace llmfun::tui

namespace llmfun::tui::views {

// Constants (the design's own numbers, restated here so a view needs no lookup)

/// Auto-grow bounds: one text row when empty, eight when full.
inline constexpr int kComposerMinRows = 1;
inline constexpr int kComposerMaxRows = 8;

/// The largest paste the composer inserts without asking ("> 5000 chars
/// asks first"). Counted in **characters** (`ui::utf8Count`), not bytes: 5000 CJK
/// characters are 15000 bytes, and a byte rule would ask about a third of the text it
/// promises.
inline constexpr std::size_t kPasteAskAboveChars = 5000;

/// A burst this large in one frame is a paste, not typing.
///
/// The composer senses a paste by counting the frame's queued characters (neither
/// backend reports bracketed paste - see `renderComposer`), and the *capture* path -
/// which folds and inserts the payload itself - must not steal typed input. Typing never
/// produces more than a character or two per frame (a fast typist on a slow frame, an
/// auto-repeat after a long render), while a real paste arrives as one large burst (or
/// starts with one, before the terminal's key trickle spreads its newlines out). Eight
/// leaves room for both and stays far below any payload worth asking about, so a small
/// paste still takes the ordinary insertion path - where the fold is already exact per
/// character (`foldPasteChar`) and `Enter` cannot send.
///
/// A paste delivered *entirely* in reads at or below this size is indistinguishable
/// from typing and is not treated as a paste; the run accumulator in `PasteState`
/// covers the normal delivery, where a terminal read is much larger than this and a
/// multi-read paste would otherwise be judged one read at a time.
inline constexpr std::size_t kPasteCaptureAboveChars = 8;

/// How long after a paste's characters an `Enter` still belongs to that paste.
///
/// The terminal delivers a pasted newline as an `Enter` *key* (imtui maps `0x0A`,
/// which the tty produced from the payload's CR), and ImGui's input trickle applies
/// at most one transition per key per frame - so a three-line paste can present its
/// third `Enter` a few frames after the characters. 250 ms is ~15 frames at 60 Hz,
/// far longer than that trickle and far shorter than a person pressing a key after
/// pasting.
inline constexpr long long kPasteFlightMs = 250;

/// The placeholder. The `…` is East-Asian *Ambiguous*, so the grid -
/// where a CJK terminal may draw it two cells wide - gets the three-dot spelling
/// (the same rule as `model::kEmptySentenceAscii`), and the compact form is what a
/// 40-cell composer shows.
///
/// The leading space is not decoration: on the text grid the caret of a focused *empty*
/// field is a cell, and it sits where the first glyph would be - a placeholder that
/// started with a letter lost it (the shipped composer's placeholders start with a space
/// for the same reason).
inline constexpr const char* kComposerPlaceholder = " Ask anything\u2026  / commands";
inline constexpr const char* kComposerPlaceholderAscii = " Ask anything...  / commands";
inline constexpr const char* kComposerPlaceholderCompact = " Ask\u2026";
inline constexpr const char* kComposerPlaceholderCompactAscii = " Ask...";

/// The text area keeps at least this much room (in `gu`) before the mode block is
/// dropped: a composer whose field is narrower than this is a field the user cannot
/// see their query in - the block is the mode word alone, and the model name lives
/// on the now bar.
inline constexpr int kComposerMinTextGu = 24;

/// The text widths, in `gu`, at which the composer stops paying for the long form:
/// the full placeholder needs 26. `kComposerFullMeterGu` is the now bar's pixel
/// breakpoint: the bar drops its meter below it, and the composer keeps no meter
/// rule of its own.
inline constexpr int kComposerFullMeterGu = 80;
inline constexpr int kComposerFullPlaceholderGu = 44;

/// The context meter's segment counts - the meter's own vocabulary, shared by both
/// bars so they read the same thresholds and the same `contextFraction` and can
/// never disagree: the 10-segment bar in the pixel profile, and the TUI's six-glyph
/// form (`ctx ████░░ 42k`) on the grid.
inline constexpr int kMeterSegmentsGui = 10;
inline constexpr int kMeterSegmentsGrid = 6;

/// `\r\n` and a lone `\r` folded to `\n`; every other byte kept.
///
/// A clipboard payload on Windows and a terminal's paste both carry CRLF, and a
/// composer that inserted the `\r` would show a stray cell (the grid drops it) and
/// submit a query with a carriage return inside.
std::string normalisePaste(std::string_view raw);

/// One character of the same fold, for the character filter ImGui calls per glyph:
/// `afterCr` remembers that the previous call converted a CR, so the LF of a CRLF
/// pair is dropped instead of doubled. Returns true when `out` should be inserted.
///
/// `normalisePaste()` is this rule over a whole payload; the two are pinned against
/// each other in `./test_composer`.
bool foldPasteChar(bool& afterCr, char32_t in, char32_t& out);

/// What to do with a pasted payload: insert it, or ask first (the 5000-character
/// rule, `kPasteAskAboveChars`). `text` is the normalised payload either way and `chars`
/// its size in codepoints, so the question can show that number without re-walking the
/// text (`ui::utf8Count`) and the answer needs no second read.
struct PastePlan {
    bool ask = false;
    std::string text;
    std::size_t chars = 0;
};

/// One contiguous edit: `removed` bytes at `start` in the old text become `inserted`.
///
/// The diff is codepoint-aware: a byte-wise scan is accepted when both cuts land on
/// UTF-8 boundaries (every typed character), and otherwise the prefix and suffix are
/// compared as codepoints, so `start` and `removed` always land on a boundary and
/// `inserted` is never a partial sequence. That is what lets a replacement (`中` ->
/// `丮`) be replayed safely; a purely byte-wise diff would cut both the removed range
/// and the payload inside a codepoint.
struct TextEdit {
    std::size_t start = 0;
    std::size_t removed = 0;
    std::string inserted;
};

TextEdit diffText(std::string_view before, std::string_view after);

/// The payload of `diffText` alone: the text `after` gained over `before`, as one
/// contiguous block (the common prefix and suffix stripped). A deletion or a no-op
/// answers the empty string. The composer uses `diffText` when it also needs the
/// start/removed range; this is the small predicate the tests pin.
std::string insertedText(std::string_view before, std::string_view after);

PastePlan planPaste(std::string_view raw);

/// The paste state: what the field saw this frame, and the question it may be asking.
struct PasteState {
    /// This frame's captured characters, raw. They are appended to `runRaw` and inserted
    /// as one payload when the burst ends, so the fold and the limit apply before
    /// anything reaches the draft.
    std::string captured;
    /// A payload over `kPasteAskAboveChars` is parked here until the user answers.
    std::string pending;
    /// Where `pending` goes when the answer is `Insert`: a byte offset into the draft
    /// (the caret the paste landed on, or the insertion point the draft's own diff
    /// names). `npos` appends - the fallback when no caret was known.
    std::size_t insertAt = std::string::npos;
    /// The bytes `Insert` removes at `insertAt` first: a paste that replaced a
    /// selection replays the whole edit, not just its new text. 0 for an insertion.
    std::size_t removeLen = 0;
    /// A paste that is still arriving: its characters are accumulated here (raw, so
    /// a CRLF split across two terminal reads still folds once) and inserted as one
    /// payload when a frame with no paste-sized burst ends the run. The limit, the
    /// fold and the question then apply to the whole paste, not to one frame's chunk.
    std::string runRaw;
    /// Where the run goes (the selection start, or the caret) and how many bytes it
    /// replaces. `npos` appends - the fallback when no caret was known.
    std::size_t runAt = std::string::npos;
    std::size_t runRemoved = 0;
    /// The question is up: `Insert` sits beside it, `Esc` discards. The action keeps its
    /// own label - the `Stop` control must not be lost while a question is up.
    bool ask = false;
    /// The question's one line (`Paste 6241 characters?`), formatted once when the
    /// question is raised - the view draws a value, it does not format per frame.
    std::string question;
    /// Until this instant, an `Enter` belongs to the paste that is still arriving.
    long long inFlightUntilMs = 0;

    /// Forget the question, the pending edit and any run still in flight (a session
    /// switch, a send, a discard).
    void dismiss();
};

/// Is a paste still in flight at `nowMs`? While it is, no `Enter` may submit:
/// the payload's own newline must not be read as the user's send.
bool pasteInFlight(const PasteState& paste, long long nowMs);

// The rest of the input policy

/// The lines a draft holds: `count('\n') + 1`, never clamped - "the caret is on the
/// last line" needs the real count, while the *area* the composer draws is
/// `composerRows()`.
int draftLines(std::string_view draft);

/// The text rows a draft needs: `draftLines()` clamped to 1..8.
int composerRows(std::string_view draft);

/// The caret's line inside `text` for a byte offset, clamped into the text.
///
/// The composer only needs this to answer "is the caret on the first/last line?",
/// which is the gate the history recall keeps: `PgUp`/`PgDn` recall
/// only from the lines the gate names.
int caretLine(std::string_view text, int caret);

/// Is the line the caret sits on empty?
///
/// This is the `/` gate: a `/` typed where its line holds nothing opens the
/// palette filtered to commands, and a `/` anywhere else is just a slash in the query.
bool lineEmptyAt(std::string_view text, int caret);

enum class SendTrigger { None, Action, Enter, Tab };

/// The one send rule: the action button, or a key when its capability proves it. Never
/// while a paste is in flight (its own newline is not the user's send), and never while
/// an IME composition is up (`composing`): a composition's Enter accepts the candidate,
/// it does not send the query. `enterProven` is the per-profile capability,
/// supplied by the shell from the model - never derived in the composer. Shift+Enter is
/// the hardcoded newline rule: it never sends.
SendTrigger sendTrigger(bool actionPressed, bool enterPressed, bool tabPressed,
                        bool shiftEnterPressed, model::SendKey setting, bool pasteInFlightNow,
                        bool composing, bool enterProven);
/// The shipped submit handshake, as one pure function: drop trailing NULs (the
/// resize callback keeps the widget's terminator inside the buffer), drop *one*
/// trailing newline (Enter in a multiline field may have inserted it), and refuse a
/// whitespace-only draft.
///
/// False means "nothing to send"; `out` is the query otherwise. The v4 composer ran the
/// same three steps, and this is that behaviour copied once rather than re-derived.
bool prepareSubmit(std::string_view draft, std::string& out);

/// Append one submitted line to the composer's history, with the one bound every path uses
/// (`UserQueryState::MAX_HISTORY`). Declared here - and defined in the frame layer - so the
/// composer's own submit, the palette's command rows and the ASK row cannot disagree about the
/// cap; the v4 composer bounded its recall helper the same way.
///
/// `TuiState` is only forward-declared on purpose (`views/palette.h`'s rule): the caller already
/// has the aggregate.
void pushHistory(llmfun::tui::TuiState& state, const std::string& line);

/// History recall: `step` is -1 for older and +1 for newer, and the
/// recalled text goes to `out`.
///
/// The gate is the caret: an older entry is only reachable from the draft's first
/// line and a newer one only from its last line - the line gate the recall keeps,
/// whatever key names the action. `caretLine` is the line the caret sat on *before* the
/// widget processed the press; a negative value means the field was not being edited
/// (nothing is recalled then - history is the field's, not the region's).
///
/// `pos == -1` is "not navigating": the first recall stashes the draft so that
/// stepping past the newest entry hands it back. The store is the shipped one
/// (`UserQueryState::inputHistory`, `historyPos`, `draftBuf`) rather than a copy this
/// view keeps, so a query sent from here is the one the next recall finds.
bool historyRecall(const std::vector<std::string>& entries, int& pos, std::string& draft,
                   std::string_view current, int step, int caretLineIndex, int lineCount,
                   std::string& out);

/// The context meter's severity: `accent` below 70 %, `warn` at
/// 70-89 %, `danger` at 90 % and above. The *colour* is the view's (`ColorToken` is
/// a `ui/` type, and `model` may not name it); this is the threshold rule, testable
/// on its own.
enum class MeterLevel { Normal, Warn, Danger };

MeterLevel meterLevel(long long used, long long max);

/// The used/max fraction clamped into [0, 1]; 0 when `max` is not positive (an
/// unknown capacity is not a full one).
float contextFraction(long long used, long long max);

// The view

/// The composer's cross-frame state, plus the answers it hands back.
///
/// One writer (the frame thread). Everything here survives a frame; the answers at
/// the bottom are consumed by the frame layer in the same frame they are set (the
/// `TopBarState` pattern).
struct ComposerState {
    // geometry, reported upwards
    /// The height the composer drew with, in the profile's own units (cells on the
    /// text grid, pixels in the GUI). `views::layoutPrefsFor()` feeds it back into
    /// `LayoutPrefs::composerH`, so the thread reserves exactly what the composer
    /// took - the layout clamps it, and a composer that is squeezed draws fewer rows
    /// (`rows` says how many).
    float height = 0.0f;
    /// Text rows drawn this frame (1..8): the auto-grow counter the harness reads.
    int rows = kComposerMinRows;

    // the field
    /// Take the keyboard on the next drawn frame. Set at startup and after a history
    /// recall or a paste (both rewrite the buffer behind the widget's back, so the
    /// field has to re-read it).
    bool focusRequested = true;
    /// The composer wrote the draft itself and wants the caret here when the field
    /// re-reads it. `npos` = no request; the compare path then moves to the end (a
    /// history recall, a cleared draft). A composer-authored insertion sets it so the
    /// next keystroke lands after the inserted block, not at the end of the draft.
    std::size_t reloadCaret = std::string::npos;
    /// The field held the keyboard at the previous frame boundary. ImGui clears a
    /// text field's active id *inside* NewFrame (an `Esc` reverts the buffer to its
    /// activation snapshot before this view runs), so "the composer owns this press"
    /// has to be read from the frame before - the rail's `renameHadFocus` rule.
    bool focusedLastFrame = false;
    /// The field owns the keyboard this frame (diagnostic, and the gate the Esc chain
    /// and history keys use).
    bool focused = false;

    // paste
    PasteState paste;

    // what the composer answers, consumed by the frame layer
    /// The action button was pressed (`Send`): the frame runs the submit handshake.
    bool submit = false;
    /// `Stop` or the Esc cancel step: the frame calls `tuiRequestCancel()` - the one
    /// path `/stop` uses - and keeps the partial output.
    bool cancelRequested = false;
    /// The composer spent this frame's `Esc` (clear or blur): the frame-level steps
    /// stand down for that press (one step per press).
    ///
    /// Only a *focused* composer spends the press. With the field blurred the chain
    /// answers `ClearInput`, and nothing performs it: `dispatchKeys()` performs the
    /// overlay and cancel steps only, and a clear has to run after the widget that owns
    /// the buffer - which is not drawn while the field is blurred. That is the chain's
    /// end state, and the next `Esc` on the focused field clears it.
    bool escapeConsumed = false;
    /// A `/` on an empty line: open the palette filtered to commands.
    bool openPalette = false;

    // observations, for the harness and the log
    /// The paste question was answered "insert" this frame, and "discard".
    bool pasteInserted = false;
    bool pasteDiscarded = false;
};

/// What the frame tells the composer, once per frame.
///
/// The first group is the shipped input store (`UserQueryState`) seen as plain data:
/// pointers, not copies, because the store has exactly one owner and a second copy
/// of a draft is a second truth. The middle group is the run and the numbers, the
/// last group is the keyboard permission.
struct ComposerInput {
    // the shipped input store (UserQueryState)
    std::string* buffer = nullptr;
    std::vector<std::string>* history = nullptr;
    int* historyPos = nullptr;
    std::string* draft = nullptr;
    /// The one-shot hand-off (`UserQueryState::newInputBufString`): a starter action or
    /// the empty state writes a prompt here and the *next* composer frame applies it.
    /// The composer has to consume it - it is the only field the thread's buttons can
    /// reach - and the hand-off exists because the thread draws before the composer, so
    /// a direct write would land in the buffer the active field is about to overwrite
    /// with its own edit state.
    std::string* handoff = nullptr;

    // the run
    /// A run is active: the action reads `Stop`.
    bool busy = false;
    /// The cancel was already requested: the action reads `Stopping` and is disabled,
    /// so a second click cannot queue a second stop (the shipped latch).
    bool stopping = false;
    /// The backend is unreachable: nothing can be sent, so every control is disabled
    /// rather than removed, and the draft stays visible.
    bool offline = false;

    // the name
    /// `Plan`, `Ask`, `Code`...; empty while the app has no mode, and the view draws
    /// nothing then (nothing is invented for a mode that does not exist). The model
    /// name, the context numbers, the phase and the speed are the now bar's and the
    /// status sentence's business, not the composer's input.
    std::string mode;
    /// The per-profile send-key capability: true where the active profile proves
    /// `enter`, false on the terminal. Supplied by the shell from the model
    /// (`model::sendKeyProven`), never derived in the composer.
    bool enterProven = false;
    model::SendKey sendKey = model::SendKey::Tab;

    // the keyboard
    /// No overlay owns the keyboard, and no view-local handler spent this frame's
    /// `Esc` (one step per press).
    bool acceptKeys = true;
    /// An overlay is up: the Esc chain's first step (it closes the overlay) belongs to
    /// the frame, not to the field.
    bool overlayOpen = false;
    /// Focus mode is on: the chain's step after the overlay's is "leave focus mode", and
    /// it is the frame's too - so this field must not treat the same press as "clear the draft" or
    /// "release the keyboard". It still has to undo its widget's cancel_edit, which is why it
    /// reads the flag rather than ignoring the key.
    bool focusMode = false;
    /// The frame's clock, in unix milliseconds - the paste flight window's time base.
    long long nowMs = 0;

    // the view's own state
    ComposerState* state = nullptr;
};

/// Draw the composer inside `region` and drive its field.
///
/// A zero or mixed-unit rectangle paints nothing (the layout folds the composer away
/// below the floor, and a view that guessed a geometry in the wrong unit would paint
/// a plausible composer in the wrong place). `state.height` is published on every
/// drawn frame, including a squeezed one.
void renderComposer(const ComposerInput& input, const ui::Rect& region, const ui::Metrics& m);

} // namespace llmfun::tui::views
