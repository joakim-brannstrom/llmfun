/// The workbench's own settings file (`ui.conf`): the single persisted
/// settings file.
///
/// `ShellPrefs` is the one owner of the user's choices - the model keeps no
/// second copy, and the views read its fields instead of their own flags. What
/// is *not* here is as deliberate: the Raw Log's open state is never
/// persisted, and the unread set is not persisted either - it belongs on
/// `UiState` next to `rawLogOpen`, never in this struct.
///
/// Load semantics (an unparsable value keeps the current one): `Key=Value`
/// split at the first `=`, keys case-sensitive and untrimmed, unknown keys
/// skipped; values parse tolerantly - enum words case-insensitive, blanks
/// ignored, out-of-range numbers clamped, even past `long`. No migration:
/// a file from an older version loads with the newer keys at their defaults.
///
/// Reading is never a change: `loadUiConf()` settles the debounce state, so an
/// untouched profile is not written back behind the user's back.
///
/// Wiring: `views::uiConfFrame()`
/// offers the debounced flush on every frame, `views::uiConfQuit()` saves on
/// the quit path, and `tuiDestroyState()` runs one more `uiConfQuit()` on
/// close. The views that honour a preference only read the fields; a choice
/// reaches the disk through the debounce.
#pragma once

#include "rawlog_data.h"

#include <string>

namespace llmfun::tui::model {

/// What the composer's Send key does: `Tab` is the shipped default - Tab
/// sends and the button (or `Shift+Enter`) breaks the line. `Enter` is the
/// pixel profile's rule - Enter sends and the modifier breaks the line - and
/// `None` is the terminal's rule, where the only thing that sends is the
/// button. Which one ships is the key gate's answer; this is only
/// how the choice is persisted.
enum class SendKey { None, Enter, Tab };

/// Which inspector tab was last up (D-INSPECTOR: two tabs, `Ctrl+5`/`Ctrl+6`).
enum class InspectorTab { Agents, Activity };

/// The Raw Log display floor (`UiLog`). `info` is the shipped default.
inline constexpr int kRawLogError = kRawLogLevelError;
inline constexpr int kRawLogWarn = kRawLogLevelWarn;
inline constexpr int kRawLogInfo = kRawLogLevelInfo;
inline constexpr int kRawLogTrace = kRawLogLevelTrace;
static_assert(kRawLogError == 4 && kRawLogWarn == 3 && kRawLogInfo == 2 && kRawLogTrace == 0,
              "the UiLog constants are the ABI's severity scale");
// severity order: trace < debug < info < warn < error (debug is a severity, not a `UiLog` choice)

// The settings sheet's catalogue (task 34, design §4.10)

inline constexpr int kUiLogChoiceCount = 4;
struct UiLogWord {
    const char* word;
    int severity;
};
inline constexpr UiLogWord kUiLogLadder[kUiLogChoiceCount] = {
    {"error", 4},
    {"warn", 3},
    {"info", 2},
    {"trace", 0},
};
int uiLogIndexOf(int severity);
const char* uiLogWord(int severity);

/// The word a `SendKey` is stored with (`none`, `enter`, `tab`).
const char* sendKeyWord(SendKey key);

/// The three send keys in the order design §4.10 lists them, and a value's index in that order.
///
/// No row *offers* them: the key gate proves one value (`sendKeyProven()`), so the sheet's
/// send-key row is read-only and this table exists for the tests that pin the gate's answer against
/// the design's list - and for the day the gate proves more than one.
inline constexpr int kSendKeyChoiceCount = 3;
SendKey sendKeyChoice(int index);
int sendKeyIndex(SendKey key);

bool sendKeyProven(SendKey key, bool textGrid);
SendKey sendKeyEffective(SendKey key, bool textGrid);
bool sendKeyOffered(SendKey key, bool textGrid);
inline constexpr bool kImeComposingWired = false;

/// The font scale's range and granularity (design §4.10: `0.5 .. 3.0`, in 5 % steps).
/// `ShellPrefs::fontScaleMain` holds the value; this module holds the
/// rule, so the sheet and the options window cannot disagree about what a legal scale is.
inline constexpr float kFontScaleMin = 0.5f;
inline constexpr float kFontScaleMax = 3.0f;
inline constexpr int kFontScaleStepPercent = 5;

/// The legal persisted window size in px:
/// a hand-edited file clamps into this range, and 0 is the "not persisted" sentinel.
inline constexpr int kMinWindowDimension = 1;
inline constexpr int kMaxWindowDimension = 32767;

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

inline constexpr int kUiConfVersion = 2;

/// The idle window before a change reaches the disk (design §5.3: 2 s, plus on
/// close and on quit). Nothing is written per frame.
inline constexpr double kUiConfIdleSeconds = 2.0;

struct ShellPrefs {
    SendKey sendKey = SendKey::Tab;
    int rawLog = kRawLogInfo;
    /// -1 auto, 0 closed, 1 open.
    int railOpen = -1;
    /// -1 auto, 0 closed, 1 open. An explicit press writes 0/1 and never back to `-1`
    int inspectorOpen = -1;
    InspectorTab tab = InspectorTab::Activity;
    /// Persisted window size in px; 0 = "not persisted"
    int windowWidth = 0;
    int windowHeight = 0;
    float fontScaleMain = 1.0f;
};

inline void toggleRail(ShellPrefs& prefs, bool visible) { prefs.railOpen = visible ? 0 : 1; }

inline void toggleInspector(ShellPrefs& prefs, bool visible) {
    prefs.inspectorOpen = visible ? 0 : 1;
}

/// The `ui.conf` path for a given imgui ini filename.
std::string uiConfPath(const std::string& iniFilename);

/// The file's bytes, one `Key=Value` line per setting and a trailing newline:
/// `Version`, `SendKey`, `UiLog`, `RailOpen`, `InspectorOpen`, `InspectorTab`,
/// `WindowWidth`, `WindowHeight`, `FontScaleMain`.
std::string uiConfText(const ShellPrefs& prefs);

/// Apply the file to `prefs`, and record what the profile holds so that
/// `flushUiConf()` may act on it afterwards. A missing (or unreadable) file and an
/// empty `iniFilename` are no-ops for the struct: the values it has (the defaults)
/// are the profile's truth from then on.
///
/// A file whose `Version` is newer than this build keeps the defaults and is
/// marked as the newer build's: only an explicit save of a real change writes it,
/// so starting and quitting this build leaves its bytes alone.
void loadUiConf(ShellPrefs& prefs, const std::string& iniFilename);

/// Write the file now: `ui.conf.tmp`, fsync, `rename()` - a crash or a full
/// disk leaves the previous file intact instead of a truncated one.
void saveUiConf(const ShellPrefs& prefs, const std::string& iniFilename,
                bool windowResized = false);

/// The per-frame call: write the file once `kUiConfIdleSeconds` have passed since
/// the last change, and never per frame. `nowSeconds` is the frame layer's clock
/// (any monotonic seconds; only differences matter).
void flushUiConf(const ShellPrefs& prefs, const std::string& iniFilename, double nowSeconds);

/// Take the pending "the settings could not be written" line, if there is one:
/// the caller posts it once (one Activity warning, design §5.3's read-only row)
/// and the next failure is a new line instead of a stream of them.
bool takeUiConfWarning(std::string& message);

} // namespace llmfun::tui::model
