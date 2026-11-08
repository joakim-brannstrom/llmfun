/// The Raw Log's own model.
///
/// The Raw Log is the *developer* surface. It draws the one raw ring - `TuiState::logRecords`,
/// filled by `tuiAddLogRecord` - through a zero-copy view (`RawLogRing`), and this module owns
/// everything that can be decided without a screen:
///
///   * `RawLogRecord`, the record's value type;
///   * the row text (`time level module.function file:line message`), the filter match and the
///     save path - all pure functions, so the surface's content is testable without a screen.
///
/// This module never writes a colour or a glyph.
///
/// Privacy: the record keeps the developer fields because this surface shows them; the Activity
/// projection has no field for module, file or line. The save path is
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
    std::int64_t timestampMs = 0; ///< Unix-epoch ms (UTC); 0 only for a record that never got one
    int level = kRawLogLevelInfo; ///< one of `kRawLogLevel*`
    int action = 0;               ///< the ABI's action id (0 = none); the Raw Log prints the id
    int line = 0;                 ///< the caller's source line, 0 = unknown
    std::string message;          ///< unformatted, one line
    std::string detail;           ///< optional multi-line context
    std::string module;           ///< `LogEntry.moduleName`
    std::string function;         ///< `LogEntry.funcName`
    std::string file;             ///< `LogEntry.file`
    std::string actionPayload;    ///< what the action would act on (turn/tool id)
    /// The D-side log stamp every record D sends, so a
    /// gap between consecutive stamps means records were lost in transport. `0` = unstamped
    /// (the C++ side synthesized itself).
    int seq = 0;
    /// True for the rows that mark a seq gap rather than report a log line
    bool loss = false;
};

/// The ring, seen by the Raw Log: a borrowed view, never a copy - one sink, one store.
///
/// `records` is the state's own deque, oldest first; a null view is an empty one (a harness or a
/// state whose ring was never filled). `dropped` is the ring's monotonic counter - the number of
/// records the store could not keep - which the surface renders as its own row so nothing is
/// lost silently.
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
    /// Every seq before the oldest survivor (0 when the ring never wrapped or the oldest
    /// survivor is unstamped)
    std::size_t headN = 0;
    /// The seqs missing between consecutive stamped survivors
    std::size_t interiorGapSum = 0;
    /// The sum of the Ns the queue-drop warnings reported, one per accepted warning.
    std::size_t queueDropNs = 0;

    std::size_t size() const { return records != nullptr ? records->size() : 0; }
    bool empty() const { return size() == 0; }
    /// The `index`-th record, oldest first, or nullptr when the index is outside the ring.
    const RawLogRecord* at(std::size_t index) const;
};

/// The C++-side marker in `RawLogRecord::function` for a placeholder row the accept path
/// inserts to mark a seq gap (`-- N records lost here --`).
inline constexpr char kRawLogSeqGapMarker[] = "seq-gap";

/// The C++-side spelling of the function name D stamps on its queue-drop warnings.
inline constexpr char kRawLogQueueDropMarker[] = "queue-drop";

/// The interior placeholder's text: `-- N records lost here --`.
std::string rawLogGapText(std::int64_t n);

/// The folded queue-drop warning's text: `-- N records lost here (D-side queue drop) --`.
std::string rawLogQueueDropText(std::int64_t n);

/// Every seq before the oldest survivor: the head placeholder's N, derived from the stamps
/// rather than remembered, so the ring's current contents always agree with it.
///
/// `dropped > 0` means the ring wrapped; the count is then the oldest survivor's stamp minus
/// one.
std::size_t rawLogHeadGap(const std::deque<RawLogRecord>& records, std::size_t dropped);

/// The seqs missing between consecutive stamped survivors, summed over the ring in order.
/// A pair is skipped when either side is unstamped or when the stamps go backwards
/// or repeat. A loss placeholder
/// carries the seq of the record it precedes.
std::size_t rawLogInteriorGapSum(const std::deque<RawLogRecord>& records);

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
/// it - a record with no metadata prints `12:00:00.000 info  something happened`. The clock is
/// *local* time.
std::string rawLogRowText(const RawLogRecord& record);

/// The detail's continuation lines, prefixed with two spaces and the design's continuation mark.
///
/// The raw text is split on `\n` (a `\r` before it is dropped, so a CRLF producer does not leave
/// a stray glyph).
std::vector<std::string> rawLogDetailLines(const RawLogRecord& record);

/// Does a record of `level` pass the floor?
bool rawLogShowsLevel(int level, int minLevel);

/// Does the record pass `filter`?
///
/// A case-insensitive substring match over the row text *and* the detail: the Raw Log is a
/// developer stream, so a search for a file name, a module or a word of the message has to find
/// the record, and hiding a hit whose detail carries the term would be a search that lies. An
/// empty or blanks-only filter matches everything.
bool rawLogMatches(const RawLogRecord& record, std::string_view filter);

/// The directory `Save` writes to: `$XDG_DATA_HOME/llmfun/logs`, else
/// `$HOME/.local/share/llmfun/logs`.
///
/// Built from the environment, never from the working directory - the workarea rule. An empty
/// answer means neither variable names a directory, which `rawLogSave()` reports as an error
/// instead of guessing one.
std::string rawLogSaveDir();

/// The file name `Save` would use for a clock reading: `ui-YYYYMMDD-HHMMSS.log`, in local time.
///
/// `attempt` 0 is the plain name and 1..99 are the `-N` variants `rawLogSave()` falls back to
/// when the plain one exists - an existing file is never overwritten, so the
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
