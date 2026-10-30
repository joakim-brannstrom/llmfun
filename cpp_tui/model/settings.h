/// The workbench's own settings file (`ui.conf`): the persisted shell
/// preferences, kept apart from `imgui2.conf` so a `TuiSettings` save cannot
/// clobber them (design §5.3, D-UI-CONF / D-03).
///
/// `ShellPrefs` is the one owner of the user's choices - the model keeps no
/// second copy, and the views read its fields instead of their own flags. What
/// is *not* here is as deliberate: window size and `FontScaleMain` stay in
/// `imgui2.conf` through `TuiSettings` (a rollback has to find them where the
/// older build left them), the Raw Log's open state is
/// never persisted (D-RAWLOG), and the unread set is not persisted either (D-19) -
/// it belongs on `UiState` next to `rawLogOpen`, never in this struct.
///
/// Load semantics are `tui_settings`' (I-09: malformed -> keep current): a line
/// is `Key=Value` split at the first `=`, keys are matched case-sensitively and
/// are not trimmed, lines without `=` and unknown keys are skipped, and a value
/// that does not parse leaves the current value alone. Values are read
/// tolerantly, where a word has no case to get wrong: an enum word is matched
/// case-insensitively, surrounding blanks are ignored, and a number outside its
/// range clamps instead of being dropped - including one too large for a `long`.
///
/// Reading is never a change: `loadUiConf()` settles the debounce state, so a
/// pruned pin or an untouched profile is not written back behind the user's back.
///
/// Wiring (task 29): the frame layer owns the three calls, in one place (D-UI-CONF: the
/// frame is the only writer of this file) -
/// `views::uiConfFrame()` reads the profile on the first frame and offers the debounced flush
/// on every one after it, and `views::uiConfQuit()` saves on the quit path. The views that
/// honour a preference (22a/23/27/29/33/34) only read the fields, so a choice reaches the
/// disk through the debounce the next time the user stops changing things.
#pragma once

#include <set>
#include <string>

namespace llmfun::tui::model {

/// What the composer's Send key does (FR-17): `Enter` sends and needs the
/// modifier to break the line, `CtrlEnter` needs it to send, and `EnterNewline`
/// is the shipped default - Enter breaks the line and only the button (or
/// `Ctrl+Enter`) sends. Which one ships is the key gate's answer (D-KEYGATE);
/// this is only how the choice is persisted.
enum class SendKey { Enter, CtrlEnter, EnterNewline };

/// Which inspector tab was last up (D-INSPECTOR: two tabs, `Ctrl+5`/`Ctrl+6`).
enum class InspectorTab { Agents, Activity };

/// The Raw Log level (`UiLog`). `off` is the shipped default: the surface is closed on every
/// start (nothing but an explicit request opens it) and it shows what a normal run logs. The
/// three open levels are the record's own floor - `raw` = the ordinary floor, `debug`/`trace`
/// widen it to the developer records the borrowed logger level brings in - and, above `raw`, the
/// backend log level the overlay borrows while it is up (D-RAWLOG restores it on close and on
/// quit).
inline constexpr int kRawLogOff = 0;
inline constexpr int kRawLogRaw = 1;
inline constexpr int kRawLogDebug = 2;
inline constexpr int kRawLogTrace = 3;

// ---------------------------------------------------------------------------
// The settings sheet's catalogue (task 34, design §4.10)
// ---------------------------------------------------------------------------
//
// Three settings the MVP actually changes plus the diagnostics paths. The *catalogue* lives
// here, next to the format it feeds, because two properties have to survive the trip from the
// row to the file: the word a row prints is the word `uiConfText()` writes (so a sheet can
// never show a value the file would not hold), and the set of values a row may offer is the
// set this build can honour. Both are model facts, not view copy.

/// The `UiLog` values in the order the sheet cycles them (`off`, `raw`, `debug`, `trace`) and
/// the word for one - the writer's own spelling, read back case-insensitively by `applyLine()`.
/// An out-of-range number clamps to its nearest word, exactly like the writer, so a hand-edited
/// `UiLog=7` shows `trace` in the sheet and is stored as `trace`.
inline constexpr int kUiLogChoiceCount = 4;
const char* uiLogWord(int rawLogPref);

/// The word a `SendKey` is stored with (`enter`, `ctrlenter`, `enterNewline`).
const char* sendKeyWord(SendKey key);

/// The three send keys in the order design §4.10 lists them, and a value's index in that order.
///
/// No row *offers* them: the key gate proves one value (`sendKeyProven()`), so the sheet's
/// send-key row is read-only and this table exists for the tests that pin the gate's answer against
/// the design's list - and for the day the gate proves more than one.
inline constexpr int kSendKeyChoiceCount = 3;
SendKey sendKeyChoice(int index);
int sendKeyIndex(SendKey key);

/// Is this send key the one `plan/key_gate.md` proves - i.e. may the sheet offer it?
///
/// The gate's answer is one value, and it is measured rather than assumed: §3's matrix
/// delivers `Enter` but delivers `Shift+Enter` and `Ctrl+Enter` as *the same byte* in both
/// terminal backends, so "Enter sends, Shift+Enter breaks the line" cannot exist in the grid,
/// and `Ctrl+Enter`'s row says "not bound" outright. D-KEYMAP forbids the GUI-only distinction
/// ("the composer must behave the same in both backends"), so the shipped rule - and the only
/// value this build can honour - is `enterNewline`: Enter breaks the line and the Send button
/// sends (R-01). The composer enforces it independently (`views::kEnterSends` is false).
///
/// The sheet therefore *shows* an unproven value that a hand-edited `SendKey=` produced (that
/// is the effective value, and hiding it would be a lie about the profile) but never offers one
/// as a choice.
bool sendKeyProven(SendKey key);

/// The font scale's range and granularity (design §4.10: `0.5 .. 3.0`, in 5 % steps).
/// `TuiSettings::fontScaleMain` holds the value; this module holds the
/// rule, so the sheet and `tui_settings.cpp` cannot disagree about what a legal scale is.
inline constexpr float kFontScaleMin = 0.5f;
inline constexpr float kFontScaleMax = 3.0f;
inline constexpr int kFontScaleStepPercent = 5;

/// `scale` clamped into the range and quantized to the step (a half step rounds up): the value
/// the UI can actually draw, which is what the sheet prints. Non-finite input answers 1.0 - the
/// shipped default - rather than a length that would round a layout to zero.
float clampFontScale(float scale);

/// `scale` moved by `steps` steps of `kFontScaleStepPercent`, clamped into the range: the
/// sheet's cycle, in one place so a large `steps` cannot walk out of the range.
float stepFontScale(float scale, int steps);

/// `scale` as the percentage the sheet prints (`100` for 1.0), rounded half up; and the
/// inverse, which applies no range check (the caller clamps through `clampFontScale()`).
int fontScalePercent(float scale);
float fontScaleFromPercent(int percent);

/// The format version this build writes, and the newest one it can read. Version
/// 2 exists only for the post-MVP merge into a single settings file, so a file
/// written by the future build is not half-applied here: it keeps the defaults
/// instead, the unattended flush never rewrites it, and an explicit save from
/// this build still carries `Version=1`.
inline constexpr int kUiConfVersion = 1;

/// The idle window before a change reaches the disk (design §5.3: 2 s, plus on
/// close and on quit). Nothing is written per frame.
inline constexpr double kUiConfIdleSeconds = 2.0;

/// The shell preferences as they live on disk.
struct ShellPrefs {
    SendKey sendKey = SendKey::EnterNewline;
    /// `kRawLogOff` .. `kRawLogTrace`. Off is the shipped default: the overlay is
    /// opt-in and closed on every start.
    int rawLog = kRawLogOff;
    /// -1 auto (leave the shape to the layout: `layoutPrefsFor()` reads it as
    /// open and the width breakpoints then pick the docked column, the collapsed
    /// strip or the panel), 0 closed, 1 open. Auto is the default: a fresh profile
    /// keeps the layout's own choice.
    int railOpen = -1;
    /// -1 auto (the shell's auto rule: docked where the display is wide enough for
    /// three columns, closed where the inspector would become a panel - a panel
    /// covers the thread *and* the composer, which is not what "open" means on a
    /// narrow screen), 0 closed, 1 open.
    int inspectorOpen = -1;
    InspectorTab tab = InspectorTab::Activity;
    /// Pinned session ids (design §5.1's `Pin.<sessionId>=1`), pruned on load for a
    /// session the store no longer has. An id that cannot round-trip (empty, or
    /// carrying `=`, CR or LF) is dropped by the parser and never written, so the
    /// set can never hold one the file cannot carry. `std::set` order is the write
    /// order, so the file's bytes are a function of the set and nothing else.
    std::set<std::string> pinned;
};

/// Show or hide the rail, as the user's own press means it (`Ctrl+B` and the palette's
/// `Toggle rail` row, task 35).
///
/// `visible` is the rail that is **on screen** - `RailState::open`, derived from the layout by the
/// frame - and not the raw preference: below the rail's breakpoint `-1 auto` resolves to the
/// *panel* shape, and an automatic panel is closed by `shellLayoutFor()`, so reading the
/// preference would make the first press a no-op on a narrow display (it would write `0`, i.e.
/// "still closed") - the exact defect `toggleInspector()` was fixed for in task 29. The rule is
/// therefore "what is visible becomes closed, what is not becomes open explicitly", and the result
/// is never written back as `-1 auto`: a press is a choice. One function, so the chord and the row
/// cannot disagree.
inline void toggleRail(ShellPrefs& prefs, bool visible) { prefs.railOpen = visible ? 0 : 1; }

/// Show or hide the inspector, as the user's own press means it (`Ctrl+I` and the palette's
/// row, task 29). `visible` is the panel that is **on screen** - `InspectorState::open`, which
/// the frame derived from the layout this frame - and not the raw preference: `-1 auto` means
/// *docked* where the display is wide and *closed* where it is not, so reading the preference
/// would make the first press a no-op on every narrow display (it would write `0`, i.e. "still
/// closed"). The rule is therefore "what is visible becomes closed, what is not becomes open
/// explicitly", and the result is never written back as `-1 auto` - `auto` is the state before
/// the first press and a press is a choice. One function so the chord and the palette row
/// cannot disagree; `toggleRail()` above is the same rule for `Ctrl+B` (the rail has an
/// auto-closed shape too - the narrow band's panel).
inline void toggleInspector(ShellPrefs& prefs, bool visible) {
    prefs.inspectorOpen = visible ? 0 : 1;
}

/// The `ui.conf` path for a given imgui ini filename: the sibling of the file
/// `tui_settings::settingsPath()` derives, i.e. `"imgui2.ini"` and
/// `"a/b/imgui2.ini"` give `"ui.conf"` and `"a/b/ui.conf"`. An empty name has no
/// path and nothing is ever read or written for it.
std::string uiConfPath(const std::string& iniFilename);

/// The file's bytes, one `Key=Value` line per setting and a trailing newline:
/// `Version`, `SendKey`, `UiLog`, `RailOpen`, `InspectorOpen`, `InspectorTab`,
/// then one `Pin.<id>=1` per pinned session in set order.
///
/// This is the format's single owner - the debounce below compares exactly these
/// bytes, so "the file is up to date" has one definition - and the writer never
/// emits text the parser rejects: an out-of-range level or tri-state is clamped
/// to its nearest legal word.
std::string uiConfText(const ShellPrefs& prefs);

/// Apply the file to `prefs`, and record what the profile holds so that
/// `flushUiConf()` may act on it afterwards. A missing (or unreadable) file and an
/// empty `iniFilename` are no-ops for the struct: the values it has (the defaults)
/// are the profile's truth from then on.
///
/// Call it at startup, before anything can change `prefs`: a preference set before
/// the load counts as the profile's baseline, not as a change, so it would never
/// reach the disk.
///
/// A file whose `Version` is newer than this build keeps the defaults and is
/// marked as the newer build's: only an explicit save of a real change writes it,
/// so starting and quitting this build leaves its bytes alone.
///
/// `knownSessionIds` is the session store's id set when the caller has one; pins
/// for an id it does not contain are pruned *in memory* (the load settles the
/// debounce state, so the prune alone never reaches the disk). Pass the complete
/// set: a partial one - a filtered, paged or still-opening store - would drop the
/// pins it does not list, and a later change would then write the pruned set.
/// `nullptr` (the default) skips the pruning, which is what a caller without a
/// store wants.
void loadUiConf(ShellPrefs& prefs, const std::string& iniFilename,
                const std::set<std::string>* knownSessionIds = nullptr);

/// Write the file now: `ui.conf.tmp`, fsync, `rename()` - a crash or a full disk
/// leaves the previous file intact instead of a truncated one. A replaced file
/// keeps its mode; a brand-new one is created 0644 subject to the process umask,
/// so a user whose policy is `umask 077` gets 0600. A temporary left behind by a
/// killed writer is removed.
/// Best effort: a write that fails changes nothing on disk and is reported once
/// through `takeUiConfWarning()`.
///
/// This is the close/quit call and the end of a pending debounce window. It
/// writes only a preference this session actually changed: a quit that changed
/// nothing leaves the file untouched, so a newer build's file keeps its bytes and
/// a hand-edited file keeps its comments, unknown keys and spelling. A first save
/// into a profile with no file yet needs no load - there is nothing to replace -
/// and a save without a load never overwrites a file this session has not read.
/// Because it is the user's own action it also settles the profile: a newer
/// build's file is replaced only by a real choice, and a failed write is not
/// retried by the flush.
void saveUiConf(const ShellPrefs& prefs, const std::string& iniFilename);

/// The per-frame call: write the file once `kUiConfIdleSeconds` have passed since
/// the last change, and never per frame. `nowSeconds` is the frame layer's clock
/// (any monotonic seconds; only differences matter).
///
/// It writes nothing until `loadUiConf()` has run for this profile, and never a
/// file that belongs to a newer build: a session that starts from defaults must
/// not overwrite choices nobody has read yet. Once the profile is loaded, the
/// settled text is the baseline - a session that changes nothing writes nothing,
/// a change writes once after the window, and a value that returns to the settled
/// one writes nothing at all. A failed write also ends the window: a read-only
/// directory cannot turn into a retry loop, and the settings stay in memory.
///
/// Frame thread only, like every other model call that touches this state.
void flushUiConf(const ShellPrefs& prefs, const std::string& iniFilename, double nowSeconds);

/// Take the pending "the settings could not be written" line, if there is one:
/// the caller posts it once (one Activity warning, design §5.3's read-only row)
/// and the next failure is a new line instead of a stream of them.
bool takeUiConfWarning(std::string& message);

} // namespace llmfun::tui::model
