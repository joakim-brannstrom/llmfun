/// The drop chip model's mechanics, on the data drop_chip.h documents.
///
/// Every function here runs on the frame thread by convention, which is what
/// makes the pop-one queues and the by-id updates race-free without a lock.
#include "drop_chip.h"

#include <utility>

namespace llmfun::tui::model {

std::int64_t appendDroppedPath(DropChipRow& row, const std::string& rawPath) {
    const std::int64_t id = row.nextChipId++;
    DropChip chip;
    chip.id = id;
    chip.state = DropChipState::Pending;
    chip.display = "processing...";
    chip.fullPath = rawPath;
    row.chips.push_back(std::move(chip));
    PendingDrop queued;
    queued.chipId = id;
    queued.path = rawPath;
    row.pendingDrops.push_back(std::move(queued));
    return id;
}

bool removeChip(DropChipRow& row, std::int64_t id) {
    for (auto it = row.chips.begin(); it != row.chips.end(); ++it) {
        if (it->id == id) {
            row.chips.erase(it);
            row.cancels.push_back(id);
            return true;
        }
    }
    return false;
}

bool setChipStatus(DropChipRow& row, std::int64_t id, DropChipState state, const std::string& path,
                   const std::string& display) {
    for (DropChip& chip : row.chips) {
        if (chip.id == id) {
            chip.state = state;
            chip.workareaPath = path;
            chip.display = display;
            return true;
        }
    }
    return false;
}

size_t chipCount(const DropChipRow& row) { return row.chips.size(); }

const DropChip* chipAt(const DropChipRow& row, size_t index) {
    if (index >= row.chips.size())
        return nullptr;
    return &row.chips[index];
}

bool popDroppedPath(DropChipRow& row, PendingDrop& out) {
    if (row.pendingDrops.empty())
        return false;
    out = std::move(row.pendingDrops.front());
    row.pendingDrops.erase(row.pendingDrops.begin());
    return true;
}

bool popDroppedPath(DropChipRow& row, std::string& outPath) {
    PendingDrop popped;
    if (!popDroppedPath(row, popped))
        return false;
    outPath = std::move(popped.path);
    return true;
}

std::int64_t popChipCancel(DropChipRow& row) {
    if (row.cancels.empty())
        return 0;
    const std::int64_t id = row.cancels.front();
    row.cancels.erase(row.cancels.begin());
    return id;
}

void clearChipRow(DropChipRow& row) { row.chips.clear(); }

} // namespace llmfun::tui::model
