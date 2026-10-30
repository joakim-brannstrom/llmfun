/// TUI core: the widget render entry point and the public state accessors.
/// Backend lifecycle (init/frames/shutdown) is driven by the C API layer and
/// owned per state (TuiState::backend); this file renders the widgets.
#include "tui.h"
#include "model/keymap.h"
#include "model/thread_view.h"
#include "tui_chrome.h"
#include "tui_common.h"
#include "ui/layout.h"
#include "ui/markdown.h"
#include "ui/metrics.h"
#include "views/shell.h"
#include "views/thread.h"

#include "imgui/imgui_internal.h"

#include "tui_api.h" // String / TuiLogRecord: the ABI's own view of a record (toInbound)

#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>
#include <thread>

namespace llmfun::tui {

namespace {

/// Debug-build tripwire for the frame-thread rule (§3.8). NDEBUG compiles the
/// assertion out (the project builds Release), so the *query* is what the unit
/// tests pin; the assertion is the developer-build aid on top of it.
void debugAssertFrameThread(const TuiState& state) {
    assert(debugIsFrameThread(state) && "TUI state touched off the frame thread");
}

/// The GUI's text scale, applied once per frame before anything is measured.
///
/// `ui::metricsFor()` reads `ImGuiStyle::FontScaleMain`, and `TuiState::settings`
/// is the only source of that value: this is the writer half of the metrics seam.
/// It runs at the top of `tuiRender()`, before the root window is begun (a Begun
/// window has already locked its font size, so a later write would only take
/// effect next frame). Without this line every GUI length would silently fall
/// back to 100 %.
void applyGuiFontScale(const TuiState& state) {
    // The text grid never loads a non-default scale: a cell is a cell, and the
    // terminal owns its size.
    if (tuiIsTextGrid())
        return;
    // loadTuiSettings() already rejects NaN and out-of-range values, so this is
    // the second line of defence; the range matches tui_settings.cpp's
    // Min/MaxFontScaleMain (50-300 %).
    const float scale = state.settings.fontScaleMain;
    ImGui::GetStyle().FontScaleMain = (scale >= 0.5f && scale <= 3.0f) ? scale : 1.0f;
}

/// The region that owns the keyboard this frame, as the keymap sees it.
///
/// `Focus::Composer` means "a text field is being edited", which is all the
/// plain-key rule needs and is true for the rail's search field and its rename box
/// as well. `Focus::Rail` is in the table's vocabulary but has no row yet: the rail's
/// own row keys (`m`, `r`, `d`, `p`, `/`) are *characters*, read and consumed by the
/// rail itself, because a printable key never arrives as a key event in the TUI
/// (plan/key_gate.md F2). Its one chord, `Ctrl+B`, is an anywhere row.
model::Focus currentFocus(const TuiState& state) {
    // The help page (task 42) is the top of the stack, so it is asked first: it is a modal surface
    // with its own region, and its only registry row is quit.
    if (state.help.open)
        return model::Focus::Help;
    if (state.palette.open)
        return model::Focus::Overlay;
    // The settings sheet (task 34) is modal too and sits directly under the palette (D-OVERLAY:
    // palette > settings > Raw Log > inspector), so it is asked second. Its own region keeps
    // `Ctrl+K` meaning "open the palette above me" rather than "close the topmost surface".
    if (state.settingsSheet.open)
        return model::Focus::Settings;
    // The Raw Log's full-screen overlay owns the keyboard too, but under its own region: its own
    // keys are the plain ones (`f`/`p`/`/`/`y`, read from the character queue) plus `End`, which
    // the palette must not receive (its own S9 rule). Both are traps for anywhere rows.
    if (state.rawLog.ownsKeyboard())
        return model::Focus::RawLog;
    if (ImGui::GetIO().WantTextInput)
        return model::Focus::Composer;
    return model::Focus::None;
}

/// Run this frame's keys through the keymap (D-KEYMAP).
///
/// Called once per frame, after the shell and before the overlays draw: a chord
/// that opens an overlay draws it in the same frame, and a chord that closes one
/// stops it from drawing at all. Because it runs after the shell, `ScrollEnd`
/// applies to the next frame's thread - harmless for a boolean, and the ordering
/// task 25's bounded rendering will want to keep in mind ("set follow, then
/// render").
///
/// The two leading `Esc` steps are performed here as well: the overlay is the
/// topmost region, and the cancel step must not depend on the composer having the
/// keyboard - a running agent has to be stoppable from the Log tab or after the
/// field was released. Only `ClearInput`/`Blur` belong to the composer, because
/// the clear has to run after its widget to survive ImGui's cancel_edit.
void dispatchKeys(TuiState& state, Log& log) {
    debugAssertFrameThread(state);
    const model::Keymap& keymap = model::defaultKeymap();
    const model::Binding* binding =
        model::resolvePressedBinding(keymap, currentFocus(state), [](ImGuiKeyChord chord) {
            return ImGui::IsKeyChordPressed(chord);
        });
    if (binding != nullptr && binding->action != model::Action::Quit) {
        // The log names the chord that actually fired, not a hard-coded one: as
        // soon as two chords share an action the literal would lie. Quit is the
        // exception: its outcome (armed vs. quit) is only known after the policy
        // runs, so that case logs below.
        log("keymap: %s -> %s\n", ImGui::GetKeyChordName(binding->chord),
            model::actionName(binding->action));
    }
    switch (binding != nullptr ? binding->action : model::Action::None) {
    case model::Action::None:
    case model::Action::Count:
        break;
    case model::Action::Quit: {
        // "Quit (confirm while running)": the policy is `tuiRequestQuitConfirmed`,
        // shared with the palette's Quit row so it belongs to quitting rather than
        // to this chord. While it is armed the active tab's status row shows the
        // prompt; nothing is ever written into the thread. The log distinguishes
        // the two outcomes, so an operator (or S2) can tell an arm from a quit.
        const bool quit = tuiRequestQuitConfirmed(state);
        log("keymap: %s -> Quit%s\n", ImGui::GetKeyChordName(binding->chord),
            quit ? "" : " (armed)");
        break;
    }
    case model::Action::Palette:
        views::openPaletteForState(state.palette, state, "");
        break;
    case model::Action::CloseOverlay:
        // The topmost surface that can be closed: the palette, else the inspector panel while
        // it floats (views/shell.h owns the order, so the chord and the Esc step agree).
        views::closeTopOverlay(state);
        break;
    case model::Action::ScrollEnd:
        // One press, one catch-up: `ui.follow` is the thread's follower (D-FOLLOW: a
        // catch-up is owed, not a position test) and `autoScroll` the v4 flag the adapter
        // still carries for the v4 store.
        state.autoScroll = true;
        state.ui.follow = true;
        // The Raw Log's follower is the same kind of flag, and `End` is D-FOLLOW's catch-up chord
        // there too (`Focus::RawLog` row; the palette deliberately does not get one). Only while
        // the surface is up: a hidden surface has nothing to catch up.
        if (state.ui.rawLogOpen)
            views::rawLogFollowNewest(state.rawLog);
        // The Activity list has no follower (quiet by default, task 31), so its one-shot latch
        // is the catch-up: the same press owes it the newest record while that tab is the one on
        // screen. `resolveBinding()` keeps a plain key away from a text field, so this cannot
        // steal the Activity search box's caret move.
        views::scrollActivityToNewest(state.inspector);
        break;
    case model::Action::ToggleRail:
        // The user's press is an explicit choice, so it never writes back `-1
        // auto`: the rail *on screen* (`RailState::open`, derived by the shell) becomes
        // closed and anything else becomes open - the preference alone would be
        // ambiguous, because `-1 auto` is a column on a wide display and a closed panel
        // on a narrow one. `ui.conf` carries the choice from this frame on (the frame
        // layer's `uiConfFrame()`/`uiConfQuit()`), so a restart starts from the user's
        // last state. `model::toggleRail()` owns the rule because the palette has a row
        // for the same action (D-PALETTE).
        //
        // Focus mode hides the rail (task 43), so the toggle does nothing there: writing a
        // preference for a region nobody can see would make a panel appear the moment the mode is
        // left - a press that asked for something else (`model::regionControlsApply`).
        if (model::regionControlsApply(state.ui))
            model::toggleRail(state.ui.prefs, state.ui.rail.open);
        break;
    case model::Action::LoadEarlier:
        // One page of older groups (D-BOUNDS): the same page the focusable `Load earlier`
        // row reveals, whether the chord or the row is used.
        views::requestEarlierPage(state.ui);
        break;
    case model::Action::ToggleInspector:
        // The press is an explicit choice, and what it means depends on what is *on screen*,
        // not on the raw preference: `-1 auto` is a docked panel on a wide display and no panel
        // at all on a narrow one, so reading the preference would make the first press a no-op
        // where the panel is auto-closed (`model::toggleInspector()` owns the rule).
        // `state.inspector.open` is this frame's derivation - the shell ran above. Focus mode
        // hides the panel (task 43), so the press does nothing there, like `Ctrl+B`'s.
        if (model::regionControlsApply(state.ui)) {
            model::toggleInspector(state.ui.prefs, state.inspector.open);
            state.inspector.reservedPressed = false;
        }
        break;
    case model::Action::InspectorAgents:
    case model::Action::InspectorActivity:
        // The tab is the preference's (`ShellPrefs::tab`) and the frame layer is its one writer:
        // the action is the same request a tab click makes. The request is written on the *next*
        // frame - `applyInspectorAnswers()` runs at the end of the shell and this dispatch runs
        // after it - and it is dropped there while focus mode hides the panel, so a press in the
        // mode cannot select a tab nobody can see.
        state.inspector.requestTab(binding->action == model::Action::InspectorAgents
                                       ? model::InspectorTab::Agents
                                       : model::InspectorTab::Activity);
        break;
    case model::Action::InspectorReserved:
        // A reserved tab number (`Ctrl+1`/`Ctrl+2`, D-INSPECTOR): nothing is selected and the
        // panel's strip says `not available yet`, which is the whole point - a digit that
        // silently did nothing would read as a broken tab. The rule (including "the notice
        // needs the strip, so the press opens a closed panel") lives in the shell, and focus
        // mode has no strip to answer on, so the press arms nothing there.
        views::pressReservedDigit(state);
        break;
    case model::Action::ToggleRawLog:
        // One press, one surface (task 33). The surface it opens owes a catch-up to the newest
        // record; the layout reserves the drawer on the *next* frame, because this runs after
        // the shell painted - the `Ctrl+B` rule for a preference that re-tiles.
        views::toggleRawLog(state);
        break;
    case model::Action::Help:
        // `?` (`Shift+/`, the pixel profile's row). The page has no tiling to reserve: the overlay
        // layer draws it in this same frame, and `openHelp()` starts it at the top with no query,
        // so a second press while it is up cannot carry a stale search over.
        views::openHelp(state.help);
        break;
    case model::Action::ToggleFocusMode:
        // One rule for the chord and the palette's row (task 43), in the model where the mode
        // lives. The re-tiling is the layout's next frame, like `Ctrl+B`: this runs after the
        // shell painted, and a mode that hides regions costs one frame of the old layout - which
        // is what the screen shows.
        model::toggleFocusMode(state.ui);
        break;
    // Steps of the Esc chain: performed here only for the cancel step (below),
    // and by the composer for clear/blur.
    case model::Action::CancelRun:
    case model::Action::ClearInput:
    case model::Action::Blur:
        break;
    // Focus mode's exit is a chain step, never a table row: `Esc` performs it below
    // (task 43), the same way `CloseOverlay` is performed above.
    case model::Action::ExitFocusMode:
        break;
    // History is the field's: the composer performs it, because only the composer
    // knows whether the caret sits on the draft's first or last line (a `↑` in the
    // middle of a draft is a caret move). Resolving the row here keeps the one table
    // authoritative and logs the press; acting here would recall from the wrong line.
    case model::Action::HistoryPrev:
    case model::Action::HistoryNext:
        break;
    }

    // Esc, steps 1 and 2. The chain's order lives in model::resolveEscape; this
    // site performs the leading steps because both must work whatever has the
    // keyboard: an open overlay closes, and a running agent stops (the composer
    // may be blurred, or the Log tab may be showing and never draw the composer
    // block at all). `tuiRequestCancel` is idempotent, so a press the composer
    // already acted on cannot double-request. A press a view-local handler
    // already took (the rail's rename/filter, or an ImGui popup/menu) is marked
    // `escapeConsumed`; acting here too would break D-ESC's one step per press.
    if (!state.escapeConsumed && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        // Step 1 is "an overlay is up" and not "an overlay owns the keyboard": the inspector's
        // panel closes on `Esc` while it floats without taking the keyboard away from the
        // composer (`views::escStepClosesOverlay()` owns that rule, and the now bar's hint asks
        // the same function).
        const model::Action step =
            model::resolveEscape({views::escStepClosesOverlay(state), cancelStepOpen(state),
                                  !state.userQuery.inputBuf.empty(), state.ui.focusMode});
        if (step == model::Action::CloseOverlay) {
            views::closeTopOverlay(state);
            log("keymap: Esc -> %s\n", model::actionName(model::Action::CloseOverlay));
        } else if (step == model::Action::ExitFocusMode) {
            // Task 43: the mode is the shell's own, so `Esc` leaves it before it can reach the
            // composer's steps. The composer has already undone its widget's cancel_edit and kept
            // the draft (`views/composer.cpp`), which is why nothing else happens here.
            model::exitFocusMode(state.ui);
            log("keymap: Esc -> %s\n", model::actionName(model::Action::ExitFocusMode));
        } else if (step == model::Action::CancelRun) {
            tuiRequestCancel(state);
            log("keymap: Esc -> %s\n", model::actionName(model::Action::CancelRun));
        }
        // ClearInput / Blur stay with the composer (views/composer.cpp).
    }
}

/// The severity word the archive prints. One spelling for the whole app, and the same one
/// `model::activityLevelWord()` uses for the tab's rows: `trace`, `debug`, `info`, `warn`,
/// `error` - the values D's `LogLevel` maps onto, lower-cased so a log file greps cleanly.
const char* logLevelWord(int level) {
    switch (level) {
    case TuiLogLevel_Trace:
        return "trace";
    case TuiLogLevel_Debug:
        return "debug";
    case TuiLogLevel_Warn:
        return "warn";
    case TuiLogLevel_Error:
        return "error";
    default:
        return "info";
    }
}

/// `2026-10-27T20:57:14.123` in UTC for a millisecond clock reading, or `?` for a record
/// that never got a stamp. UTC on purpose: the archive is compared across machines and
/// across DST changes, and a local-time log is ambiguous twice a year.
std::string archiveTimestamp(std::int64_t timestampMs) {
    if (timestampMs <= 0)
        return "?";
    const std::time_t seconds = static_cast<std::time_t>(timestampMs / 1000);
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &seconds);
#else
    gmtime_r(&seconds, &utc);
#endif
    // 64 bytes: a year wider than four digits cannot truncate the stamp (`%04d` grows), and
    // gcc's format-truncation analysis cannot prove the tm fields are small.
    char buffer[64];
    std::snprintf(buffer, sizeof buffer, "%04d-%02d-%02dT%02d:%02d:%02d.%03d", utc.tm_year + 1900,
                  utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec,
                  static_cast<int>(timestampMs % 1000));
    return std::string(buffer);
}

/// The archive failed (a failed open, write or flush): latch it for the rest of this request and
/// drop the stream. A latched-but-open stream is not a retryable stream - `tuiSetLogging(true)`
/// clears the latch, and the next archived line must be a new `fopen`, not another write to the
/// handle that just failed.
void failLogArchive(TuiState& state) {
    state.logArchive.unavailable = true;
    closeLogArchive(state);
}

/// The archive's one handle (`tuiSetLogging` -> `llmfun_ui_log.txt`), opened lazily by the
/// first archived line and never retried within a request: a failed open, write or flush
/// latches it off (a read-only working directory must not cost an `fopen` per record - the
/// `ui.conf` path's rule - and a session that never switches the archive on must not hold a
/// descriptor at all). `tuiSetLogging(true)` is the next attempt.
///
/// One handle for the whole run: the record path needs a handle that survives between frames (a
/// record arrives between two of them), so the state owns it rather than opening one per write
/// (task 32: one archive, one handle).
FILE* archiveHandle(TuiState& state) {
    if (!state.isLogActive || state.logArchive.unavailable)
        return nullptr;
    if (state.logArchive.file == nullptr) {
        state.logArchive.file = std::fopen("llmfun_ui_log.txt", "a");
        if (state.logArchive.file == nullptr)
            failLogArchive(state);
    }
    return state.logArchive.file;
}

/// The record as the C ABI's POD sees it (borrowed views, valid for the call): the legacy
/// two-string path has to hand `model::projectForActivity()` the same input the ABI does, so
/// the projection rules cannot drift between the two entry points.
TuiLogRecord toInbound(const LogRecord& record) {
    const auto view = [](const std::string& s) { return String{s.data(), s.size()}; };
    TuiLogRecord out{};
    out.level = record.level;
    out.action = record.action;
    out.timestampMs = record.timestampMs;
    out.line = record.line;
    out.message = view(record.message);
    out.detail = view(record.detail);
    out.module = view(record.module);
    out.function = view(record.function);
    out.file = view(record.file);
    out.actionPayload = view(record.actionPayload);
    return out;
}

} // namespace

void tuiAddOutputLine(TuiState& state, const ChatMessage& msg) {
    state.chat.outputLines.push_back(msg);
    if (state.chat.outputLines.size() > state.chat.MaxChatMessages) {
        // Expansion state is keyed by message id, so the evicted message's entry
        // has to go with it: the id is never reused, and keeping one entry per
        // message ever expanded would grow for the whole session (review M2).
        // A work group whose first message is evicted loses its group key here.
        const size_t evicted = state.chat.outputLines.front().id;
        state.chat.outputLineOpen.erase(evicted);
        state.chat.openWorkGroups.erase(evicted);
        state.chat.outputLines.pop_front();
    }
    tuiStreamChatMessageClear(state);
}

void tuiAddLogRecord(TuiState& state, const LogRecord& record) {
    state.logRecords.push_back(record);
    // The monotonic arrival counter (see `TuiState::logRecordSeq`): the ring is bounded, so
    // this is the only fact that tells a consumer a record arrived.
    ++state.logRecordSeq;
    // Both bounds are enforced FIFO, oldest first: the byte bound is what keeps a session of
    // huge stack traces from turning the ring into an unbounded allocation. A record larger
    // than the whole budget cannot be kept - it evicts everything (itself included) and is
    // counted as dropped like any other eviction, which is what NFR-17 asks for: never a
    // silent loss, always a number the Raw Log can print.
    const auto bytesOf = [](const LogRecord& r) {
        return r.message.size() + r.detail.size() + r.module.size() + r.function.size() +
               r.file.size() + r.actionPayload.size();
    };
    state.logRecordBytes += bytesOf(record);
    // One dropped *error* is a fact the user has to be able to see: the Raw Log's dropped row is
    // developer detail, and Activity is where a user looks for "something failed". The eviction is
    // the only place that knows, so it raises one Activity error for the whole eviction (a burst
    // evicts thousands; one row is the news, a thousand are noise) - task 33's promotion rule.
    bool droppedError = false;
    while (!state.logRecords.empty() && (state.logRecords.size() > state.MaxLogRecords ||
                                         state.logRecordBytes > state.MaxLogRecordBytes)) {
        if (state.logRecords.front().level >= TuiLogLevel_Error)
            droppedError = true;
        state.logRecordBytes -= bytesOf(state.logRecords.front());
        state.logRecords.pop_front();
        ++state.droppedRecords;
    }
    if (droppedError) {
        model::LogRecord notice;
        notice.timestampMs = nowMs();
        notice.level = model::kActivityLevelError;
        notice.message =
            "log ring dropped an error record (open the Raw Log and Save before the ring wraps)";
        model::appendActivity(state.ui, notice, model::kActivityLevelError,
                              isActivityVisible(state));
    }

    // The archive line, formatted from the record (design §6.4: the archive keeps its
    // historical format; the format itself lives in `logArchiveLine()`).
    //
    // The archive is the only formatted copy: the Raw Log and the Activity tab read the
    // record's own strings, so a run without `tuiSetLogging` formats nothing per arrival
    // (task-31 review I3: a second formatted copy of every record used to be built for a
    // tab nobody draws).
    if (state.isLogActive) {
        const std::string line = logArchiveLine(record);
        if (FILE* archive = archiveHandle(state); archive != nullptr) {
            // A failed write or flush latches the archive off exactly like a failed open: the
            // audit trail must not lose its tail silently (a full disk is the case this exists
            // for), and one attempt per request is enough. `&&` short-circuits, so a failed
            // write is not followed by a newline for a line that never landed. `ferror` also
            // catches an error the previous writer left on the shared stream, which a fresh
            // `fflush` on an empty buffer can no longer report.
            const bool stored = std::fputs(line.c_str(), archive) >= 0 &&
                                std::fputc('\n', archive) != EOF && std::fflush(archive) == 0 &&
                                std::ferror(archive) == 0;
            if (!stored)
                failLogArchive(state);
        }
    }
}

std::int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

void tuiAddLogMessage(TuiState& state, const LogMessage& msg) {
    // The shipped two-string call, kept for the compatibility path (design §6.4: "the
    // two-string wrapper only for legacy sites with no metadata"). It maps to a record with
    // no metadata and the informational severity, so one store sees every arrival.
    LogRecord record;
    record.timestampMs = nowMs();
    record.level = TuiLogLevel_Info;
    record.message = msg.summary;
    record.detail = msg.text;
    tuiAddLogRecord(state, record);
    // Activity is a projection of the same arrival. There is no metadata to drop here, so the
    // projection is the record itself (the rules `projectForActivity()` and
    // `activityShowsLevel()` apply are reused, not re-implemented).
    model::appendActivity(state.ui, model::projectForActivity(toInbound(record)), record.level,
                          isActivityVisible(state));
}

std::string logArchiveLine(const LogRecord& record) {
    // `2026-10-27T20:57:14.123 - info: message [func:line]` - D's historical
    // `"%s - %s: %s [%s:%d]"`, formatted from the record's own fields.
    std::string out = archiveTimestamp(record.timestampMs);
    out += " - ";
    out += logLevelWord(record.level);
    out += ": ";
    out += record.message;
    if (!record.function.empty() || record.line > 0) {
        out += " [";
        out += record.function.empty() ? std::string("?") : record.function;
        out += ':';
        out += std::to_string(record.line);
        out += ']';
    }
    if (!record.detail.empty()) {
        // Multi-line context stays multi-line: the archive is a text file, and folding it
        // would make a stack trace one unreadable line.
        out += '\n';
        out += record.detail;
    }
    return out;
}

bool isActivityVisible(const TuiState& state) {
    // The last frame's derivation: the panel is up and this tab is the one it draws.
    return state.inspector.open && state.inspector.tab == model::InspectorTab::Activity;
}

void closeLogArchive(TuiState& state) {
    if (state.logArchive.file != nullptr) {
        std::fclose(state.logArchive.file);
        state.logArchive.file = nullptr;
    }
}

void tuiClearOutput(TuiState& state) {
    state.chat.outputLines.clear();
    state.chat.outputLineOpen.clear();
    state.chat.openWorkGroups.clear();
    state.chat.expansionDefaultThroughId = 0;
    tuiStreamChatMessageClear(state);
    // The workbench's thread state describes messages that just went away: the
    // adapter's group ids must not be reused by the next thread (D-GROUPID), and
    // the paging window, the expansion set and the follow flag are the ones a
    // session switch resets too (a clear owes a catch-up, D-FOLLOW).
    state.threadAdapter.reset();
    model::resetForSessionSwitch(state.ui);
    // The run status belongs to the session too (§5.4): its numbers describe a run in a
    // conversation the user just left, so the status is cleared and the row falls back to the
    // sentence splitter, which D re-sends for the new session. The setter's clear path is the
    // one implementation of that, here as well as in the ABI - a second copy could forget the
    // chips or the offline flag.
    tuiSetRuntimeStatus(state, model::RuntimeStatus{});
    // A pending paste question names text the user aimed at the session they just left,
    // and a history cursor without its draft is a stale mode: both belong to the switch.
    // The *draft* survives (design §9: "composer draft is preserved per session in
    // memory"), so nothing here touches `userQuery.inputBuf`.
    state.composer.paste.dismiss();
    state.composer.reloadCaret = std::string::npos;
    state.composer.escapeConsumed = false;
}

namespace {

/// Whether the stream store may accept a snapshot now (task 38): the window has elapsed since
/// the last accepted one, or the clock went backwards (a wall-clock jump must not freeze the
/// live answer until it catches up).
bool streamWindowElapsed(const TuiState& state, std::int64_t nowMs) {
    const std::int64_t last = state.ui.thread.stream.lastAppliedMs;
    if (last == 0 || nowMs < last)
        return true;
    return nowMs - last >= model::kStreamUpdateIntervalMs;
}

/// Store one streamed snapshot and move the clock's window with it: the one place the store,
/// the window and the counters advance together.
void storeStreamSnapshot(TuiState& state, const std::string& text, const std::string& thinking,
                         std::int64_t nowMs) {
    model::StreamState& stream = state.ui.thread.stream;
    stream.lastAppliedMs = nowMs;
    ++stream.applied;
    // The clock, the caret and the cancel mark are derived from the text that is in the store
    // now - not from a snapshot's arrival, which the throttle may have detached from what is on
    // screen. A frame derives them again; this keeps the store self-consistent without one.
    model::refreshStreamClock(stream, /*hasStream=*/true, /*hasText=*/!text.empty(),
                              /*hasThinking=*/!thinking.empty(), nowMs, state.cancelRequested);
}

/// Store the parked snapshot. The caller has already decided that its window has elapsed.
void applyPendingStream(TuiState& state, std::int64_t nowMs) {
    if (!state.chat.pendingStream)
        return;
    state.chat.pendingStream = false;
    state.chat.streamMsg = state.chat.pendingStreamMsg;
    state.chat.hasStreamMsg = true;
    storeStreamSnapshot(state, state.chat.streamMsg.text, state.chat.streamMsg.thinking, nowMs);
}

/// The frame's flush of the throttle: a snapshot parked inside the window is applied by the
/// first frame after it, so it is on screen at most one window late and is never lost.
void flushPendingStream(TuiState& state, std::int64_t nowMs) {
    if (!streamWindowElapsed(state, nowMs))
        return;
    applyPendingStream(state, nowMs);
}

} // namespace

void tuiUpdateStreamChatMessage(TuiState& state, const ChatMessage& msg) {
    const std::int64_t nowMs = model::agentClockMs();
    model::StreamState& stream = state.ui.thread.stream;
    // A new stream is the one case with no window: the first snapshot of an answer is stored the
    // moment it arrives, or the thread would keep showing the *previous* answer's group for a
    // window. `stream.active()` is false for a state that was never cleared, too.
    const bool fresh = !state.chat.hasStreamMsg && !stream.active();
    // What the store shows *or* is about to: comparing against the parked snapshot keeps a
    // resend of it a no-op instead of parking it again.
    const auto& current =
        state.chat.pendingStream ? state.chat.pendingStreamMsg : state.chat.streamMsg;
    if (!fresh && msg.text == current.text && msg.thinking == current.thinking &&
        msg.summary == current.summary && msg.type == current.type) {
        // Unchanged text is not a repaint: D's loop re-sends the accumulated message on every
        // tick, including the ticks that changed nothing at all. (`pendingStream` keeps whatever
        // is parked: it is newer than, and different from, this one.)
        ++stream.unchanged;
        return;
    }
    if (fresh || streamWindowElapsed(state, nowMs)) {
        if (fresh && stream.startedMs == 0)
            stream.startedMs = nowMs != 0 ? nowMs : 1;
        state.chat.streamMsg = msg;
        state.chat.hasStreamMsg = true;
        storeStreamSnapshot(state, msg.text, msg.thinking, nowMs);
        return;
    }
    // Inside the window: park the newest snapshot. The next frame after the window applies it,
    // so a burst costs one repaint per window instead of one per tick, and no text is dropped
    // (each snapshot is the whole message).
    state.chat.pendingStreamMsg = msg;
    state.chat.pendingStream = true;
    ++stream.coalesced;
}

void tuiStreamChatMessageClear(TuiState& state) {
    state.chat.streamMsg = {};
    state.chat.hasStreamMsg = false;
    // A parked snapshot is text D already committed: the clear is the commit's own handshake and
    // the stored message carries the same characters, so nothing is lost by dropping it.
    state.chat.pendingStreamMsg = {};
    state.chat.pendingStream = false;
    // The stream is a block identity too: the next stream gets a new group id, and
    // its reasoning is seeded open when that group is born (task 26, D-GROUPID).
    state.threadAdapter.clearStream();
}

void tuiSetStatusText(TuiState& state, const std::string& text) {
    // D calls this unconditionally on every render (source/llm/tui/package.d:339),
    // so the same sentence arrives up to ~100 times a second. Re-deriving the
    // chips for an unchanged string would spend a sentence split, four strings
    // and two number formats per tick for nothing — D-STATUS's status budget is
    // 20 Hz, and tasks 28/39 build on this seam being cheap.
    if (text == state.statusText)
        return;
    state.statusText = text;
    // One derivation per change, never per frame: the chips are formatted
    // numbers, so they belong to the update. An empty sentence means "nothing
    // sent yet" and yields the default sentence's chips — TuiState seeds exactly
    // those, so the first empty update returns above without work.
    state.statusChips = model::splitStatusText(text.empty() ? model::kDefaultStatusSentence : text);
    // While the numbers are structured they own the chips (§6.3): only the model chip is the
    // sentence's, because the ABI struct carries no model name. Re-formatting all four here would
    // spend a sentence's worth of formatting on every tick - exactly what the 20 Hz budget exists
    // to avoid - so the one chip that cannot come from the numbers is copied instead.
    if (state.ui.status.structured)
        state.ui.chips.model = state.statusChips.model;
}

void tuiSetRuntimeStatus(TuiState& state, const model::RuntimeStatus& status) {
    if (!status.structured) {
        // Back to the sentence: the frame layer's projection (`deriveNowBar`) owns `ui.status`
        // again, and the chips are the sentence's set as it stands. Clearing also drops the one
        // fact only the numbers can express - an unreachable model - because a cleared status
        // never claimed it.
        state.ui.status = model::RuntimeStatus{};
        state.ui.chips = state.statusChips;
        state.offline = false;
        return;
    }
    state.ui.status = status;
    // The chips are formatted *here*, once per update, by the splitter's own formatters: the
    // two paths must produce byte-identical strings for the same numbers (§6.3). The model chip
    // is the sentence's - the struct carries no model name (a model changes when the user
    // changes it, not per status tick).
    state.ui.chips = model::chipsFromStatus(state.ui.status, state.statusChips.model);
    // `offline` is the one phase with a consequence beyond the row: the thread's offline card
    // and the composer's disabled state read `TuiState::offline`, and nothing else writes it.
    state.offline = model::phaseFromOrdinal(status.phase) == model::Phase::Offline;
}

std::string tuiGetInput(const TuiState& state) { return state.userQuery.inputBuf; }

void tuiClearInput(TuiState& state) { state.userQuery.inputBuf.clear(); }

bool tuiIsSubmitReady(const TuiState& state) { return state.userQuery.submitReady; }

void tuiResetSubmit(TuiState& state) {
    state.userQuery.submitReady = false;
    state.userQuery.submitQuery.clear();
}

std::string tuiGetSubmitQuery(const TuiState& state) { return state.userQuery.submitQuery; }

void applyGuiChromeMetrics() {
    // Sizes only: the colours stay applyUiTheme()'s (it runs after the backend
    // init and never touches a metric, so the two do not fight).
    //
    // Every number here is a token (D-THEME): spacing steps from `space()`,
    // corners from `radius()`, the hairline from the profile's own border. The
    // values are authored at 100 % scale; `applyDpiScale()` multiplies them once,
    // after this call, so a 2x monitor scales them too. `FontScaleMain` is
    // deliberately *not* folded in: it scales text (and therefore `Metrics::row`),
    // not the chrome around it, which is what keeps a 150 % window's padding from
    // overflowing its own hairlines.
    ImGuiStyle& style = ImGui::GetStyle();
    // The design's ~24 px hit box is the body line plus two pads, so the pad
    // follows the type scale instead of a second, hand-tuned number: at a 24 px row
    // and a 15 px body it is 4.5, rounded up to 5, and a framed widget comes out
    // 25 px (15 + 2 x 5) against a 24 px row - one pixel over the rhythm, which is
    // the rounding the design's "~24" allows.
    //
    // The pad does not follow `FontScaleMain`: a 150 % text scale grows the row to
    // 36 px and the text to 22.5, while the pad keeps the value authored at 100 %
    // (so a framed widget is 32.5 px inside a 36 px row). A framed widget in a
    // row-sized region is task 27's to re-check; the chrome has to stay the size a
    // mouse targets, not the size the text is.
    const ui::Metrics m = ui::metricsFor(false, 1.0f, 1.0f);
    const float hairline = m.border;
    const float framePadY = std::ceil((m.row - ui::typeSize(int(ui::TypeStep::body))) * 0.5f);
    style.WindowPadding =
        ImVec2(ui::space(int(ui::Space::section)), ui::space(int(ui::Space::comfy)));
    style.ItemSpacing = ImVec2(ui::space(int(ui::Space::base)), ui::space(int(ui::Space::compact)));
    style.FramePadding =
        ImVec2(ui::space(int(ui::Space::base)) + ui::space(int(ui::Space::hair)), framePadY);
    // Within a widget (a checkbox's box and its label, an icon and its text): the
    // tight step, stated here rather than left at ImGui's own 4/4 default.
    style.ItemInnerSpacing =
        ImVec2(ui::space(int(ui::Space::tight)), ui::space(int(ui::Space::tight)));
    style.WindowRounding = ui::radius(int(ui::Radius::large));
    style.ChildRounding = ui::radius(int(ui::Radius::large));
    style.PopupRounding = ui::radius(int(ui::Radius::large));
    style.FrameRounding = ui::radius(int(ui::Radius::small));
    style.GrabRounding = ui::radius(int(ui::Radius::small));
    style.ScrollbarRounding = ui::radius(int(ui::Radius::large));
    style.ScrollbarSize = ui::space(int(ui::Space::comfy));
    // Every boundary the profile paints is one hairline (`lineStrong` is the
    // colour, this is the thickness). A framed widget is the exception: its edge
    // is its `inset` fill against the surface, so a border would draw the same
    // boundary twice in two greys.
    style.WindowBorderSize = hairline;
    style.ChildBorderSize = hairline;
    style.PopupBorderSize = hairline;
    style.FrameBorderSize = 0.0f;
}

void applyUiTheme() {
    // The palette and the depth policy live in ui/tokens (D-THEME). The pixel
    // profile is not a terminal: it keeps the design's exact colours whatever
    // TERM says. The text grid asks the terminal, in the documented order
    // (NO_COLOR -> COLORTERM -> TERM=*256color* -> 16).
    const ui::ColorDepth depth =
        tuiIsTextGrid() ? ui::detectColorDepth() : ui::ColorDepth::TrueColor;
    ui::applyTheme(ui::ThemeId::Graphite, depth);

    // One cell is a dot on a tall track; three cells read as a thumb. A metric
    // of the text grid, not of the palette, so it lives here and not in
    // ui::applyTheme(), which knows nothing about profiles. The GUI keeps
    // ImGui's 12 px thumb: a 3 px grab on a 1280-wide window would be lost.
    // audit: gated - harnesses that apply the theme before ImTui_ImplText_Init()
    // (apply-theme-then-init order, e.g. test_tui_maxwidth's harnessInit) run
    // this line while the guard is still false and keep imtui's 1.0 value
    // instead; no harness asserts the grab metric.
    const bool textGrid = tuiIsTextGrid();
    if (textGrid)
        ImGui::GetStyle().GrabMinSize = 3.0f;

    // The markdown style is the other sink of a token colour - `RichMd::Style`,
    // not an `ImGuiCol_*` - so it is re-applied at the depth just chosen. Without
    // this the markdown inks would be the only colours in the app that did not
    // follow the theme (initMarkdownTextMode() runs before the backend, i.e.
    // before the depth is known). A state without a rich context is a no-op.
    ui::applyMarkdownTheme(ui::markdownThemeFromTokens(textGrid));
}

bool tuiIsTextGrid() { return ImTui_TextEncodingActive; }

namespace ui {

Metrics metricsFor() {
    // The profile seam of the geometry (design §3.4). `ui/metrics.*` is pure -
    // it takes the profile and the two scale factors as data and never touches
    // `tui.h` - exactly like `applyUiTheme()` above, because "which renderer is
    // live" is app knowledge. This is the single place where that answer
    // becomes geometry, so a view asks the metrics module and the profiles stay
    // one decision instead of a per-widget branch.
    const bool textGrid = tuiIsTextGrid();
    // No ImGui context yet (a harness between CreateContext and the first frame,
    // or a pure unit test): the scale factors are unknown, so answer 100 %. The
    // text grid ignores both anyway.
    if (ImGui::GetCurrentContext() == nullptr)
        return metricsFor(textGrid, 1.0f, 1.0f);
    // Both are ImGui 1.92's own scale factors: FontScaleMain is what the GUI
    // applies from the font-scale setting, FontScaleDpi what the backend applied
    // from the monitor (tui_backend_gui.cpp).
    const ImGuiStyle& style = ImGui::GetStyle();
    return metricsFor(textGrid, style.FontScaleMain, style.FontScaleDpi);
}

} // namespace ui

bool tuiRender(TuiState& state) {
    // First frame claims the loop for this thread; the frame-thread-only state
    // (cancel flags) may not be touched from anywhere else (debug assert).
    debugMarkFrameThread(state);

    // Per-frame Esc ownership latch (D-ESC's one step per press): the shell's
    // local handlers and the composer set/read it later in this frame, so it
    // must start clean before anything draws.
    state.escapeConsumed = false;

    // The feedback queue's clock (task 45): an info/success item is dropped once its lifetime has
    // run out, and a warn/error item carries no expiry and is therefore untouched. It runs before
    // anything reads the queue (the shell's flash derivation and the stack are both below), so the
    // frame that ends an item's life is the frame it stops being drawn.
    model::expireToasts(state.ui, model::agentClockMs());

    // A stream snapshot the update throttle parked (task 38): the window has had a frame to
    // elapse, so a coalesced update lands here - before anything reads the thread, which is
    // what keeps `revision`, the caret and the store the same age on screen.
    flushPendingStream(state, model::agentClockMs());

    // The metrics seam's input, set for THIS frame and before any window is
    // begun: every GUI length (`ui::metricsFor()`) is derived from the text
    // scale, so a stale value would lay out a whole frame at the wrong size.
    applyGuiFontScale(state);

    // Exit request (the palette's Quit row today, the keymap's quit chord from
    // task 11): the request is a flag, and the frame after it answers "user
    // terminated" the same way a GUI window close does, so the caller's loop
    // ends through its normal handshake.
    if (state.quitRequested) {
        // The quit path's `ui.conf` save (D-UI-CONF, task 29): the debounce window may not have
        // elapsed, and a choice made seconds before quitting still has to survive the restart.
        // It carries a change and nothing else, so a quit that changed no preference leaves the
        // file's bytes alone.
        views::uiConfQuit(state.ui, state.iniFilename);
        return false;
    }

    // The frame's debug lines share the archive's one handle (see
    // `archiveHandle()`): null unless the archive is on - the common case, so this costs no
    // descriptor and no `fopen`.
    FILE* const archive = archiveHandle(state);
    Log log{archive};

    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImVec2 DisplaySize = ImGui::GetIO().DisplaySize;

    // audit: gated - maxWidth is a terminal-column cap (D validates
    // 40..10000); the GUI's DisplaySize is in pixels, so applying the clamp
    // would read a column value as pixels and truncate the window to a
    // sliver. The text backend keeps the exact clamp.
    if (tuiIsTextGrid() && state.maxWidth > 0 && DisplaySize.x > state.maxWidth) {
        DisplaySize.x = static_cast<float>(state.maxWidth);
        ImGui::GetIO().DisplaySize = DisplaySize; // propagate to grid sizing
    }

    static constexpr float MIN_TERMINAL_WIDTH = 40.0f;
    static constexpr float MIN_TERMINAL_HEIGHT = 15.0f;

    // audit: gated - 40x15 is the text grid's minimum cell area; a
    // 40x15-pixel window is not "too small" for the GUI, and blanking the UI
    // to an error page when the user shrinks the window would be wrong.
    //
    // This returns before the shell and dispatchKeys, so below the 40x15
    // backend floor the keymap (Ctrl+Q and the global Esc cancel) is inert.
    // That is the documented floor, not a supported working size: resize the
    // terminal (or use Ctrl+C's SIGINT) to get back above it.
    if (tuiIsTextGrid() &&
        (DisplaySize.x < MIN_TERMINAL_WIDTH || DisplaySize.y < MIN_TERMINAL_HEIGHT)) {
        ImGui::Begin("Error");
        ImGui::Text("Terminal too small! Minimum size: 40x15");
        ImGui::End();
        // No popup survives the error page; clear the next-frame latch so the
        // first Esc after resizing back is not mistaken for a menu press.
        state.popupOpenLastFrame = false;
        return true;
    }

    // An open ImGui popup/menu owns Esc while it is up. ImGui's NavUpdate closes
    // it inside NewFrame, *before* this function runs, so "a popup was up" has
    // to come from the previous frame's end-of-frame latch; without it one Esc
    // would close the menu and cancel the run behind it. The rail's
    // rename/filter latch themselves when they consume the key; this covers
    // every ImGui-owned surface (row menu, menu bar) in both tabs.
    state.escapeConsumed = state.popupOpenLastFrame && ImGui::IsKeyPressed(ImGuiKey_Escape, false);

    // Required: BeginChild calls must be nested inside a Begin/End block.
    // Without a parent window, BeginChild creates an implicit window whose
    // auto-positioning offsets the layout, making the TUI unusable.
    ImGuiWindowFlags parentFlags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoTitleBar |
                                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                                   ImGuiWindowFlags_NoScrollWithMouse |
                                   ImGuiWindowFlags_NoBackground;
    // The workbench draws its own top bar from the layout (it is one of the six
    // regions), so it must not ask for ImGui's menu bar: the flag reserves the
    // bar's height *and* shifts the window's content start down, which would push
    // the top bar's row out of reach and clip it away.
    // audit: pixel profile — the grid intentionally draws no root
    // background (the terminal's own background shows through), while the GUI
    // has a real framebuffer and the backend never clears it (no glClear):
    // without a background, pixels the children do not repaint keep stale
    // frame content (visible after a resize or an early-frame layout change).
    // Let the root window paint the themed background in the GUI only.
    if (!tuiIsTextGrid())
        parentFlags &= ~ImGuiWindowFlags_NoBackground;
    static bool noClose = true;
    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
    ImGui::SetNextWindowSize(DisplaySize, ImGuiCond_Always);
    // The root window is an absolute-positioned layout; it must never scroll.
    // Two ways it otherwise can: the content overshoots the bottom edge (27 px
    // of scrollable range in the pixel profile), and a nav focus request made
    // while a child window is Appearing (the input row at startup) asks for a
    // centered scroll. SetNextWindowScroll() pins the scroll to 0 inside this
    // same Begin — unlike SetScrollY(0), whose target the *next* Begin applies
    // (1.92 semantics), so the startup request used to leave one frame
    // scrolled and the whole layout slid up before settling.
    // audit: accepted for BOTH modes — the absolute SetCursorPos
    // layout depends on the root window never scrolling, and the pixel
    // profile wants the same invariant (a stale scroll offset would slide
    // the whole UI); gating this would risk the drift described above.
    ImGui::SetNextWindowScroll(ImVec2(0.0f, 0.0f));
    // Square corners on the root (a no-op in the grid, which has none anyway):
    // the chrome radius belongs to panels and popups, and a radius on the
    // full-window surface would only cut four clear-colour wedges out of the
    // framebuffer inside a square window frame.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::Begin("##TuiRoot", &noClose, parentFlags);

    // The one composition: the workbench shell. The thread is mounted once per
    // frame (the adapter's renderCalls() counter is the tripwire `./test_shell`
    // reads). The per-frame derivation is the frame layer's job
    // (model/ui_state.h): the shell draws from `state.ui`, it does not decide
    // what the thread area means.
    views::ShellFrame shellFrame;
    {
        // `ui.conf` (D-UI-CONF, task 29): the first frame reads the profile and every
        // frame after it offers the debounced write. It runs *before* the shell so a saved
        // `InspectorOpen=`/`InspectorTab=` shapes this very frame instead of the next one.
        const double nowSeconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
                .count();
        views::uiConfFrame(state.ui, state.uiConfLoaded, state.iniFilename, nowSeconds);
        // Bound before the derivation below reads `groupCount()`: the adapter is a
        // member of the state, and a moved state leaves it unbound rather than
        // dangling (see `ThreadStoreAdapter::bind`).
        state.threadAdapter.bind(state);
        state.ui.offline = state.offline;
        // `groupCount() > 0` is the adapter's answer, so this file asks the seam
        // and not the v4 store which groups exist.
        //
        // A running pipeline agent is not a group: the conversation keeps its own body and the
        // agents live in the inspector's Agents tab (task 30), whose lane list the thread reads
        // *only* while the user has drilled into one (`UiState::focusedAgent` - the drill-down
        // outranks these cards, `views/thread.cpp`). So an agent-only run shows the empty-state
        // card until a lane is clicked, and that is the design's answer rather than a gap: the
        // thread is the conversation, the tab is the run.
        // The thread's "loading" card follows the *store's* load state - a select that
        // has been sent to D and not acknowledged - and not `pendingSelectId`, which is
        // the busy queue: that one is cleared at the top of the next frame, so it only
        // ever meant "switch queued behind a running agent".
        state.ui.view = model::deriveThreadView(
            /*hasMessages=*/state.threadAdapter.groupCount() > 0,
            /*loadingSession=*/!state.ui.rail.loadingId.empty(),
            /*offline=*/state.offline);
        shellFrame = views::renderShell(state, ui::displaySize());
    }

    ImGui::End();
    ImGui::PopStyleVar();

    // Keys (D-KEYMAP): one table, one dispatcher. Runs after the shell so a
    // chord that opens an overlay draws it in this same frame, and before the
    // overlay layer so a chord that closes one stops it from drawing at all.
    dispatchKeys(state, log);

    // Overlay layer, drawn after the shell so it sits on top and never takes
    // part in the shell's tiling (D-OVERLAY). It reuses the rectangles of the
    // layout pass above - and the shell re-lays out when a chord opened an
    // overlay after that pass (the keymap runs between the two calls, so the
    // palette it just opened has no box yet).
    views::renderOverlays(state, shellFrame);

    // End-of-frame latch for the next frame's popup/menu Esc ownership:
    // NavUpdate closes an open popup before the next tuiRender starts, so this
    // is the last point where it can be observed. Note `IsPopupOpen(Any, AnyLevel)`
    // is `OpenPopupStack.Size > 0`, not "submitted this frame": a popup that stops
    // being submitted stays in the stack for one grace frame, so this latch can be
    // one frame stale and spend one `Esc` with nothing on screen (measured, task 44).
    state.popupOpenLastFrame = ImGui::IsPopupOpen(
        static_cast<ImGuiID>(0), ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);

    // Flush what this frame's debug lines wrote. The handle belongs to the state now (closed by
    // `tuiSetLogging(false)` and by the destroy path), but the lines must be on disk when the
    // frame returns - a crash right after a frame must not lose them, the same rule the record
    // path follows record by record.
    //
    // `ferror` is checked as well as the flush result: a debug line that failed mid-frame left
    // the error flag set, and a later `fflush` of an empty buffer reports success again (measured
    // on glibc against `/dev/full`), so the flush result alone would miss the very full-disk case
    // this is here for. This is the last use of `archive` in the frame: latching closes it.
    if (archive != nullptr && (std::fflush(archive) != 0 || std::ferror(archive)))
        failLogArchive(state);

    return true;
}

void tuiSetLogging(TuiState& state, bool onOff) {
    state.isLogActive = onOff;
    if (onOff) {
        // A new request is a new attempt: `unavailable` latches one failed open/write for the
        // request that hit it, not for the rest of the session (task-32 review, Minor 7).
        state.logArchive.unavailable = false;
        return;
    }
    // Turning the archive off closes the record handle with it: the next archive write must
    // not land in a file the user just switched off (and a session that never turns it back
    // on must not hold the descriptor either).
    closeLogArchive(state);
}

void tuiSetMaxWidth(TuiState& state, int maxWidth) { state.maxWidth = maxWidth; }

void tuiSetReadyStatus(TuiState& state, bool ready) {
    // The run's own edge is where the run clock starts. D calls this once when a run begins
    // (`uiAgentBusy()` -> `tuiReadyStatus(0)`), and it is the only writer the top bar's run
    // pill can rely on - nothing else stamps the start - so without this line the clock would
    // count from construction. The `state.readyStatus` test makes it an
    // edge, so a repeated "not ready" (a stream tick) cannot restart the clock.
    if (!ready && state.readyStatus)
        state.startProcesssingTime = std::chrono::system_clock::now();
    state.readyStatus = ready;
    if (ready) {
        // The run is over: drop the composer's Stopping latch so the action
        // reads Send again and can request a new cancel. Tying it to the run
        // state (not to the one-frame pending flag) is what makes a second click
        // while the agent is still winding down a no-op. The quit chord's
        // confirmation is armed per run for the same reason: once the run is
        // over, one press must quit again.
        state.cancelRequested = false;
        state.quitConfirmArmed = false;
    }
}

void tuiRequestCancel(TuiState& state) {
    debugAssertFrameThread(state);
    // The stop is the last word on the text: a snapshot the update throttle parked has to reach
    // the store before the clock freezes (task 38), or the partial answer the user keeps would
    // be one window short of what D had already sent.
    applyPendingStream(state, model::agentClockMs());
    // One slot + a latch. The pending flag is the one-shot hand-off to the D
    // loop; the latch keeps the composer in `Stopping` until the run ends, so a
    // second click can never queue a second stop even though the pending flag is
    // consumed within the same frame.
    if (state.cancelRequested) {
        return;
    }
    state.cancelPending = true;
    state.cancelRequested = true;
}

bool tuiTakeCancel(TuiState& state) {
    debugAssertFrameThread(state);
    const bool pending = state.cancelPending;
    state.cancelPending = false;
    return pending;
}

void tuiRequestQuit(TuiState& state) {
    debugAssertFrameThread(state);
    // Idempotent: only the frame-thread-owned flag is written, so a second
    // request before the loop acts on the first changes nothing.
    state.quitRequested = true;
}

bool tuiRequestQuitConfirmed(TuiState& state) {
    debugAssertFrameThread(state);
    // "Confirm while running" (D-KEYMAP): a run may be seconds from finishing, so
    // one press must not throw it away. The first request arms the confirmation
    // (the composer's status row shows kQuitConfirmHint), the next one quits. The
    // arming is per run: tuiSetReadyStatus(true) clears it.
    if (cancelStepOpen(state) && !state.quitConfirmArmed) {
        state.quitConfirmArmed = true;
        return false;
    }
    state.quitConfirmArmed = false;
    tuiRequestQuit(state);
    return true;
}

bool debugIsFrameThread(const TuiState& state) {
    // Before the first frame no thread has claimed this state's loop, so any
    // thread may still become the frame thread (headless unit tests call the
    // accessors before rendering anything).
    return !state.frameThreadClaimed || state.frameThread == std::this_thread::get_id();
}

void debugMarkFrameThread(TuiState& state) {
    if (!state.frameThreadClaimed) {
        state.frameThread = std::this_thread::get_id();
        state.frameThreadClaimed = true;
    }
}

void tuiResetFrameThread(TuiState& state) {
    state.frameThread = std::thread::id{};
    state.frameThreadClaimed = false;
}

void tuiSetIniFilename(TuiState& state, const std::string& filename) {
    if (state.iniFilename.empty())
        state.iniFilename = filename;
}

void saveUiConfOnShutdown(TuiState& state) {
    // A no-op without a profile (D-UI-CONF). Called from `tuiDestroyState()`, which every exit
    // path reaches - the quit chord's `quitRequested` frame saves too, and the second call
    // writes nothing because the bytes already match.
    views::uiConfQuit(state.ui, state.iniFilename);
}

void tuiInitQueryHistory(TuiState& state, const std::vector<std::string>& history) {
    state.userQuery.inputHistory = history;
    state.userQuery.historyPos = -1;
}

bool tuiHasMoreEvents() {
    auto ctx = ImGui::GetCurrentContext();
    return ctx != nullptr && ctx->InputEventsTrail.Size > 0;
}

} // namespace llmfun::tui
