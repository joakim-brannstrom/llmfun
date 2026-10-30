/// The Raw Log's own model (design §4.9 / FR-16, D-RAWLOG, task 33).
///
/// The Raw Log is the *developer* surface. It draws the one raw ring - `TuiState::logRecords`,
/// filled by `tuiAddLogRecord` - through a zero-copy view (`RawLogRing`), and this module owns
/// everything that can be decided without a screen:
///
///   * `RawLogRecord`, the record's value type. It lives here, not in `tui.h`, so
///     `views/rawlog.h` can name it without including `tui.h` (D-ADAPTER: `views/*` names no
///     v4 type and no aggregate). `tui.h` aliases it (`using LogRecord = model::RawLogRecord`)
///     and keeps owning the ring, so there is still one record type, not two.
///   * the row text (`time level module.function file:line message`), the filter match and the
///     save path - all pure functions, so the surface's content is testable without a screen.
///
/// What is *not* here: the ring itself (`TuiState`, task 32), the surface (`views/rawlog.*`),
/// and the level raise/restore rule (D's `TextUserInterface`, because the level is D's
/// `globalLogLevel`). This module never writes a colour or a glyph: it is `model/`.
///
/// Privacy (R-07/NFR-12): the record keeps the developer fields because this surface shows
/// them; Activity's projection has no path to this struct (`model/activity.h` has no field for
/// module, file or line), so the split is structural rather than a filter. The save path is
/// built from the user's data directory and never from the process' working directory - a log
/// written inside the workarea could be indexed by a RAG.
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

namespace llmfun::tui::model {

/// The five severities, as the numbers the C ABI carries (`TuiLogLevel`).
///
/// Spelled out here for the same reason `model/activity.h` spells its three out: this header is
/// reachable from `views/rawlog.h`, which must not drag the C surface in, and a value that
/// crossed the ABI then needs no mapping table on the render path. `tui_api.cpp` pins the two
/// lists together with a `static_assert` (the C enum is the ABI's, so that is the one TU that
/// sees both).
inline constexpr int kRawLogLevelTrace = 0;
inline constexpr int kRawLogLevelDebug = 1;
inline constexpr int kRawLogLevelInfo = 2;
inline constexpr int kRawLogLevelWarn = 3;
inline constexpr int kRawLogLevelError = 4;

/// One raw log record.
///
/// It is the C ABI's `TuiLogRecord` copied at the call boundary (C-02: D may free its buffers
/// the moment `tuiAddLogRecord` returns), which is why every field is an owned string. The
/// field names are the ones the archive and the Raw Log print, so a reader of either can follow
/// one vocabulary.
struct RawLogRecord {
    long long timestampMs = 0;    ///< Unix-epoch ms (UTC); 0 only for a record that never got one
    int level = kRawLogLevelInfo; ///< one of `kRawLogLevel*`
    int action = 0;               ///< the ABI's action id (0 = none); the Raw Log prints the id
    int line = 0;                 ///< the caller's source line, 0 = unknown
    std::string message;          ///< unformatted, one line
    std::string detail;           ///< optional multi-line context
    std::string module;           ///< `LogEntry.moduleName`
    std::string function;         ///< `LogEntry.funcName`
    std::string file;             ///< `LogEntry.file`
    std::string actionPayload;    ///< what the action would act on (turn/tool id)
};

/// The ring, seen by the Raw Log: a borrowed view, never a copy (R-08: one sink, one store).
///
/// `records` is the state's own deque, oldest first; a null view is an empty one (a harness or a
/// state whose ring was never filled). `dropped` is the ring's monotonic counter - the number of
/// records the store could not keep - which the surface renders as its own row so nothing is
/// lost silently (NFR-17).
///
/// `arrivals` is the state's monotonic accept counter (`TuiState::logRecordSeq`). The surface's
/// filtered index is cached, and at capacity the ring's size never changes while its *front*
/// moves - a front timestamp is not enough either, because a burst can stamp many records with
/// the same millisecond and the new front then looks exactly like the evicted one. The accept
/// counter is the one fact that changes on every arrival, so it is part of the cache key.
struct RawLogRing {
    const std::deque<RawLogRecord>* records = nullptr;
    std::size_t dropped = 0;
    std::size_t arrivals = 0;

    std::size_t size() const { return records != nullptr ? records->size() : 0; }
    bool empty() const { return size() == 0; }
    /// The `index`-th record, oldest first, or nullptr when the index is outside the ring.
    const RawLogRecord* at(std::size_t index) const;
};

/// The level's word, in the archive's spelling: `trace`, `debug`, `info`, `warn`, `error`.
///
/// One spelling for the archive (`logArchiveLine`), the Raw Log and Activity (`warn`, not
/// `warning`), so a grep over a saved file finds the same words the screen showed. An unknown
/// number is `info`: the ABI folds it there at the boundary, and a second rule here would let
/// the two disagree.
const char* rawLogLevelWord(int level);

/// The level's word padded to the width of the longest one (`trace`/`debug`/`error`), so the
/// rows of a stream line up in the terminal. The padding is a column, not data: `rawLogSave()`
/// writes the unpadded text, because a saved line should not carry alignment whitespace.
std::string rawLogLevelField(int level);

/// The record's row: `HH:MM:SS.mmm level module.function file:line message[ action N payload]`.
///
/// Every optional part is dropped when the record does not carry it, and the separators go with
/// it - a record with no metadata prints `12:00:00.000 info  something happened`, not a row of
/// stray spaces. The clock is *local* time, the same clock Activity's rows use; a saved file
/// therefore reads in the user's own time, and the goldens pin `TZ=UTC` so a capture in another
/// zone differs only when the code does.
///
/// The action id and its payload are the *last* segment (design §"Projection": "Raw Log = all
/// fields" - the action is what a retryable error is retryable by, and a row that hides it makes
/// two errors look identical). They are part of the row text rather than a separate accessor so
/// the filter and `Save` inherit them without a second implementation.
std::string rawLogRowText(const RawLogRecord& record);

/// The detail's continuation lines, prefixed with two spaces and the design's continuation mark.
///
/// The raw text is split on `\n` (a `\r` before it is dropped, so a CRLF producer does not leave
/// a stray glyph); an empty detail answers no detail lines. A record with an action gets one
/// extra line for the id and payload, so a narrow row that dropped the action suffix still keeps
/// it reachable by expanding the record.
std::vector<std::string> rawLogDetailLines(const RawLogRecord& record);

/// The smallest severity the surface shows, from the `UiLog` preference (D-RAWLOG).
///
/// `UiLog` is the surface's *level*: `off` is the shipped default (closed at every start, and
/// only what a normal run logs), `raw` the same floor with the surface named explicitly,
/// `debug` and `trace` widen it to the developer records the borrowed logger level brings in.
/// D's `std::logger.LogLevel` has no `debug` member at all (all < trace < info < warning <
/// error), so `debug` and `trace` name the same floor today - the mapping is spelled out here so
/// the day a `debug` step exists it changes one line instead of every call site.
int rawLogFloor(int uiLogPref);

/// Does a record of `level` pass the floor?
bool rawLogShowsLevel(int level, int minLevel);

/// Does the record pass `filter`?
///
/// A case-insensitive substring match over the row text *and* the detail: the Raw Log is a
/// developer stream, so a search for a file name, a module or a word of the message has to find
/// the record, and hiding a hit whose detail carries the term would be a search that lies. An
/// empty or blanks-only filter matches everything (the rail's rule).
bool rawLogMatches(const RawLogRecord& record, std::string_view filter);

/// The directory `Save` writes to: `$XDG_DATA_HOME/llmfun/logs`, else
/// `$HOME/.local/share/llmfun/logs` (design §4.9's `~/.local/share/llmfun/logs`).
///
/// Built from the environment, never from the working directory - the workarea rule. An empty
/// answer means neither variable names a directory, which `rawLogSave()` reports as an error
/// instead of guessing one.
std::string rawLogSaveDir();

/// The file name `Save` would use for a clock reading: `ui-YYYYMMDD-HHMMSS.log`, in local time.
///
/// `attempt` 0 is the plain name and 1..99 are the `-N` variants `rawLogSave()` falls back to
/// when the plain one exists - "never overwrite" (design §4.9) is a rule about the file, so the
/// name generator has to be able to answer a free one.
std::string rawLogSaveName(std::int64_t nowMs, int attempt = 0);

/// Write the filtered records to a new file and answer its path.
///
/// `outPath` is the file that was created, `error` a human-readable reason on failure (the
/// surface draws it). The file is created with `O_EXCL`, so an existing file is never
/// truncated: a colliding name moves to the next `-N` variant, and only a directory that
/// already holds a hundred same-second saves fails. The directory is created when it is
/// missing. Every record that passes `filter` is written with its detail (the ring is complete;
/// the surface's visible *window* is what `y` copies), one row per line, in ring order.
bool rawLogSave(const RawLogRing& ring, std::string_view filter, std::int64_t nowMs,
                std::string& outPath, std::string& error);

} // namespace llmfun::tui::model
