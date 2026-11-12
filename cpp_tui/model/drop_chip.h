/// Drag & drop chips: one chip per dropped path, the queues D drains, and the
/// by-id updates D reports back.
///
/// A drop lands as a Pending chip the composer's row shows the same frame, and
/// the raw path queues for D's per-frame poll. D copies or converts the file
/// into the workarea and reports the chip by id, which flips it to Done or
/// FailedRaw with the final workarea-relative path. An X click on a chip
/// removes it at once and queues the id for D's cancel poll.
///
/// Layering: `model` depends on nothing app-specific - no ImGui, no `tui.h`, no
/// backend header - so the module is testable without a renderer
/// (`test_drop_chip` exercises it with no renderer attached).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace llmfun::tui::model {

/// The life of one dropped attachment. The ordinals are the ABI's status
/// vocabulary: D reports a chip through the by-id setter with one of these
/// numbers, so the ABI's status constants mirror this enum by comment rather
/// than by a layout pin.
enum class DropChipState : std::int32_t {
    Pending = 0,   ///< dropped, D has not reported back yet
    Done = 1,      ///< the workarea copy exists, the display is D's capped basename
    FailedRaw = 2, ///< the conversion failed, the raw copy was attached instead, the display is
                   ///< D's capped basename
    Failed = 3,    ///< nothing was attached, the display is the short reason
};

/// One dropped attachment as the chip row and the Log tab read it.
struct DropChip {
    std::int64_t id =
        0; ///< monotonic, nonzero, never reused. 0 is the ABI's "none", never a real chip id.
    DropChipState state = DropChipState::Pending;
    /// The chips user facing display string.
    std::string display;
    /// The original drop path, for the hover and the Log tab
    std::string fullPath;
    /// The workarea path D reports on Done and FailedRaw. Empty otherwise.
    std::string workareaPath;
};

/// One queued drop: the raw path and the chip id its Pending chip took. The
/// pair is the drop getter's answer — D reports the chip by the same id it
/// took, so a path dropped twice reports to the chip its own drop created.
struct PendingDrop {
    std::int64_t chipId = 0; ///< the id appendDroppedPath() handed the path's chip
    std::string path;        ///< the raw drop path, verbatim
};

/// The composer's chip row plus the two queues D drains. One struct because one
/// writer owns all of it: the drop callback and the row's X clicks append on the
/// frame thread, and D's per-frame polls pop the queues through the C ABI.
struct DropChipRow {
    std::vector<DropChip> chips;           ///< the chip row, in creation order
    std::vector<PendingDrop> pendingDrops; ///< path + chip id pairs, FIFO, the drop getter's source
    std::vector<std::int64_t> cancels;     ///< X-clicked chip ids, FIFO, the cancel getter's source
    std::int64_t nextChipId = 1; ///< the id appendDroppedPath() hands out next. Ids are never
                                 ///< reused, so a clear does not reset it.
};

/// Appends one Pending chip for `rawPath` and queues the path for D. The chip
/// takes `nextChipId` (and the counter advances), so a path dropped twice
/// becomes two chips with two ids.
std::int64_t appendDroppedPath(DropChipRow& row, const std::string& rawPath);

/// Removes the chip `id` from the row immediately and queues the id for D's
/// cancel poll. An unknown or already-removed id answers false and queues
/// nothing.
bool removeChip(DropChipRow& row, std::int64_t id);

/// Flips the chip `id` to `state` with the workarea path and the display D
/// reports. `path` is the workarea-relative path on Done and FailedRaw and
/// empty otherwise; `display` is the chip text D authors for any state. An
/// unknown or removed id is a silent no-op that answers false: a late report
/// for a chip the user cancelled must not resurrect it.
bool setChipStatus(DropChipRow& row, std::int64_t id, DropChipState state, const std::string& path,
                   const std::string& display);

/// How many chips the row shows.
size_t chipCount(const DropChipRow& row);

/// The chip at `index`, nullptr past the end. The pointer is into the row's
/// vector, so any append, removal or update invalidates it.
const DropChip* chipAt(const DropChipRow& row, size_t index);

/// Pops the oldest queued drop into `out`. Answers false when the queue is
/// empty and leaves `out` alone. The paired form: the path rides with the chip
/// id its Pending chip took, which is the id D reports back.
bool popDroppedPath(DropChipRow& row, PendingDrop& out);

/// The path-only form: pops the oldest queued drop's path. The pairing is the
/// getter's business, so a caller that does not report by id (a test, a future
/// consumer) keeps a one-argument answer.
bool popDroppedPath(DropChipRow& row, std::string& outPath);

/// Pops the oldest queued cancel id, 0 when the queue is empty (0 is the ABI's
/// "none", never a real chip id).
std::int64_t popChipCancel(DropChipRow& row);

/// Empties the chip row, the chips only. The two queues keep draining
/// independently: a chip dropped between D's snapshot and this clear is a
/// pending chip at Send, cleared without composing while its job keeps running.
void clearChipRow(DropChipRow& row);

} // namespace llmfun::tui::model
