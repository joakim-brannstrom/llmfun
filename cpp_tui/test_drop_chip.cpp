// test_drop_chip.cpp
//
// Headless unit tests for the drop chip model: the id assignment, the pending
// label, the two FIFO queues D drains, the by-id setter and the clear.
//
// What is pinned here, case by case:
//   - ids are monotonic, nonzero and never reused, across appends and clears
//     alike (0 is the ABI's "none" and can never name a chip);
//   - dropping the same path twice produces two chips, and every drop queues
//     its raw path FIFO;
//   - the pending label is "processing <basename>..." with the basename taken
//     verbatim as the text after the last '/' or '\' (no sanitizing here);
//   - the by-id setter flips state, label and workarea path, and answers false
//     without touching anything for an unknown or removed id;
//   - removeChip removes immediately, queues the cancel id FIFO, and an
//     unknown id queues nothing;
//   - both queues pop one item per call with their empty sentinels (false /
//     id 0, an outPath left alone);
//   - the paired pop carries the chip id the path's Pending chip took, so the
//     same path dropped twice pops as two pairings with two ids;
//   - clearChipRow empties the chips only: the queues keep draining and the
//     id counter keeps counting.
//
// Layering check, by construction: the model module (model/drop_chip.{h,cpp})
// has no ImGui, no tui.h and no backend header, so the model scenarios need no
// renderer. The drop-intake scenario links the GUI backend's named handler and
// drives it with a fake path array, no window required.
//
// Exit codes: 0 = all scenarios pass; 1 = first assertion failure.
// Run from the build dir: ./test_drop_chip

#include "model/drop_chip.h"
#include "tui_backend.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace model = llmfun::tui::model;

// The GUI backend's named drop intake (tui_backend_gui.cpp). White-box on
// purpose: the handler is the seam a test can drive without a window, and the
// published chip row carries the chips out.
namespace llmfun::tui {
void handleDropEvent(int count, const char** paths);
}

namespace {

int g_scenario = 0;
const char* g_phase = "startup";

void fail(const std::string& what) {
    std::fprintf(stderr, "FAIL [scenario %d — %s]: %s\n", g_scenario, g_phase, what.c_str());
    std::exit(1);
}

void expect(bool condition, const std::string& what) {
    if (!condition)
        fail(what);
}

void phase(const char* what) { g_phase = what; }

std::string quoted(const std::string& value) { return "'" + value + "'"; }

/// The chip `id`, or fails. The one lookup the scenarios share.
const model::DropChip& chip(const model::DropChipRow& row, std::int64_t id, const char* where) {
    for (const model::DropChip& candidate : row.chips)
        if (candidate.id == id)
            return candidate;
    fail(std::string(where) + ": chip " + std::to_string(id) + " is gone from the row");
    std::exit(1); // not reached, fail() exits
}

/// The pending chip appendDroppedPath() produces: the label, the full path and
/// nothing in the workarea yet.
void expectPending(const model::DropChip& dropped, const std::string& label,
                   const std::string& fullPath, const char* where) {
    const std::string what = std::string(where) + " (id " + std::to_string(dropped.id) + ")";
    if (dropped.state != model::DropChipState::Pending)
        fail(what + ": state is not Pending");
    if (dropped.label != label)
        fail(what + ": label " + quoted(dropped.label) + " != " + quoted(label));
    if (dropped.fullPath != fullPath)
        fail(what + ": fullPath " + quoted(dropped.fullPath) + " != " + quoted(fullPath));
    if (!dropped.workareaPath.empty())
        fail(what + ": workareaPath " + quoted(dropped.workareaPath) + " is set before D reported");
}

// Ids start at 1, grow by one per drop and never repeat, not even after the
// row is cleared: 0 is the ABI's "none" and a recycled id would let a stale
// report land on a new chip.
void scenario1_ids() {
    g_scenario = 1;
    phase("id assignment");

    model::DropChipRow row;
    const std::int64_t first = model::appendDroppedPath(row, "/tmp/a.txt");
    const std::int64_t second = model::appendDroppedPath(row, "/tmp/b.txt");
    const std::int64_t third = model::appendDroppedPath(row, "/tmp/c.txt");
    expect(first != 0, "the first id is 0, the ABI's none");
    expect(first == 1 && second == 2 && third == 3, "ids are not the monotonic run 1, 2, 3");
    expect(model::chipCount(row) == 3, "three drops did not leave three chips");

    model::clearChipRow(row);
    const std::int64_t afterClear = model::appendDroppedPath(row, "/tmp/d.txt");
    expect(afterClear > third, "an id was reused after the row was cleared");
    expect(model::chipCount(row) == 1, "the cleared row did not restart empty");
}

// A path dropped twice is two chips (path-as-id is wrong by design) and each
// drop queues its own raw path.
void scenario2_doubleDrop() {
    g_scenario = 2;
    phase("same path twice");

    model::DropChipRow row;
    const std::int64_t first = model::appendDroppedPath(row, "/tmp/report.md");
    const std::int64_t second = model::appendDroppedPath(row, "/tmp/report.md");
    expect(first != second, "the same path produced one chip twice");
    expect(model::chipCount(row) == 2, "the double drop did not leave two chips");

    std::string path;
    expect(model::popDroppedPath(row, path) && path == "/tmp/report.md",
           "the first queued path is not the first drop");
    expect(model::popDroppedPath(row, path) && path == "/tmp/report.md",
           "the double drop did not queue the path twice");
    expect(!model::popDroppedPath(row, path), "an empty queue answered true");
}

// The pending label names the drop's basename, taken verbatim after the last
// separator of either flavour. C++ never sanitizes it: D does, before anything
// is composed into a message.
void scenario3_pendingLabel() {
    g_scenario = 3;
    phase("pending label");

    model::DropChipRow row;
    const std::int64_t unixy = model::appendDroppedPath(row, "/home/user/reports/q3.md");
    expectPending(chip(row, unixy, "unix path"), "processing q3.md...", "/home/user/reports/q3.md",
                  "unix path");

    const std::int64_t windows = model::appendDroppedPath(row, "C:\\data\\notes.txt");
    expectPending(chip(row, windows, "windows path"), "processing notes.txt...",
                  "C:\\data\\notes.txt", "windows path");

    const std::int64_t bare = model::appendDroppedPath(row, "readme");
    expectPending(chip(row, bare, "bare name"), "processing readme...", "readme", "bare name");

    const std::int64_t mixed = model::appendDroppedPath(row, "/data\\mixed.name");
    expectPending(chip(row, mixed, "mixed separators"), "processing mixed.name...",
                  "/data\\mixed.name", "mixed separators");

    const std::int64_t trailing = model::appendDroppedPath(row, "/data/dir/");
    expectPending(chip(row, trailing, "trailing separator"), "processing ...", "/data/dir/",
                  "trailing separator");

    expect(model::chipCount(row) == 5, "the label scenarios did not leave five chips");
}

// D's report flips one chip by id, and an unknown or already-removed id is a
// silent no-op: a late report for a cancelled chip must not resurrect it.
void scenario4_setStatus() {
    g_scenario = 4;
    phase("by-id setter");

    model::DropChipRow row;
    const std::int64_t id = model::appendDroppedPath(row, "/tmp/a.md");
    model::appendDroppedPath(row, "/tmp/b.md");

    expect(
        model::setChipStatus(row, id, model::DropChipState::Done, "dropped/a.md", "dropped/a.md"),
        "the setter refused a live id");
    const model::DropChip& done = chip(row, id, "after Done");
    expect(done.state == model::DropChipState::Done, "the state did not flip to Done");
    expect(done.label == "dropped/a.md", "the Done label did not land");
    expect(done.workareaPath == "dropped/a.md", "the workarea path did not land");
    expect(model::chipCount(row) == 2, "the setter changed the row's size");

    expect(!model::setChipStatus(row, 4242, model::DropChipState::Failed, "nope", ""),
           "the setter accepted an unknown id");
    expect(!model::setChipStatus(row, 0, model::DropChipState::Failed, "nope", ""),
           "the setter accepted the ABI's none id");

    model::removeChip(row, id);
    expect(!model::setChipStatus(row, id, model::DropChipState::FailedRaw, "nope", ""),
           "the setter accepted a removed id");
    expect(model::chipCount(row) == 1, "a no-op report resurrected a removed chip");
    for (const model::DropChip& leftover : row.chips)
        expect(leftover.id != id, "a no-op report left the removed chip in the row");
}

// The drop queue pops one path per call, oldest first, and an empty queue
// answers false with the outPath left alone.
void scenario5_popDrops() {
    g_scenario = 5;
    phase("drop queue");

    model::DropChipRow row;
    std::string path = "untouched";
    expect(!model::popDroppedPath(row, path), "an empty drop queue answered true");
    expect(path == "untouched", "the empty pop wrote the outPath");

    model::appendDroppedPath(row, "/tmp/first");
    model::appendDroppedPath(row, "/tmp/second");
    expect(model::popDroppedPath(row, path) && path == "/tmp/first", "the drop queue is not FIFO");
    expect(model::popDroppedPath(row, path) && path == "/tmp/second",
           "the drop queue lost an item");
    expect(!model::popDroppedPath(row, path), "the drained queue answered true");
    expect(path == "/tmp/second", "the drained pop wrote the outPath");

    // The pop is a take, not a peek: what D took never comes back, and a
    // later clear cannot resurrect it.
    model::clearChipRow(row);
    expect(!model::popDroppedPath(row, path), "the clear restored a popped drop");
}

// The cancel queue pops one id per call, oldest first, 0 = empty. Only a real
// removal queues an id.
void scenario6_cancels() {
    g_scenario = 6;
    phase("cancel queue");

    model::DropChipRow row;
    expect(model::popChipCancel(row) == 0, "an empty cancel queue answered nonzero");

    const std::int64_t a = model::appendDroppedPath(row, "/tmp/a");
    const std::int64_t b = model::appendDroppedPath(row, "/tmp/b");
    expect(!model::removeChip(row, 4242), "removing an unknown id answered true");
    expect(model::popChipCancel(row) == 0, "a failed removal queued a cancel");

    expect(model::removeChip(row, b), "removing a live id answered false");
    expect(model::removeChip(row, a), "removing a second live id answered false");
    expect(model::popChipCancel(row) == b, "the cancel queue is not FIFO");
    expect(model::popChipCancel(row) == a, "the cancel queue lost an id");
    expect(model::popChipCancel(row) == 0, "the drained cancel queue answered nonzero");
    expect(!model::removeChip(row, a), "removing an already-removed id answered true");
}

// The clear empties the chips only: both queues keep draining and the id
// counter keeps counting, so a chip dropped between D's snapshot and the
// clear still delivers its path and its removal still delivers its id.
void scenario7_clear() {
    g_scenario = 7;
    phase("chips-only clear");

    model::DropChipRow row;
    const std::int64_t a = model::appendDroppedPath(row, "/tmp/a");
    const std::int64_t b = model::appendDroppedPath(row, "/tmp/b");
    model::setChipStatus(row, a, model::DropChipState::Done, "dropped/a", "dropped/a");
    model::removeChip(row, b);

    model::clearChipRow(row);
    expect(model::chipCount(row) == 0, "the clear left a chip behind");
    expect(model::chipAt(row, 0) == nullptr, "chipAt answered a chip past the end");

    std::string path;
    expect(model::popDroppedPath(row, path) && path == "/tmp/a",
           "the clear drained the drop queue");
    expect(model::popDroppedPath(row, path) && path == "/tmp/b",
           "the clear drained one drop of two");
    expect(model::popChipCancel(row) == b, "the clear drained the cancel queue");
    expect(model::popChipCancel(row) == 0, "the clear drained a cancel of one");

    const std::int64_t after = model::appendDroppedPath(row, "/tmp/c");
    expect(after > b, "the clear reset the id counter");
}

// The read side: the row order is creation order and chipAt answers nullptr
// past the end, never a neighbour's chip.
void scenario8_reads() {
    g_scenario = 8;
    phase("reads");

    model::DropChipRow row;
    expect(model::chipAt(row, 0) == nullptr, "an empty row answered a chip");
    expect(model::chipCount(row) == 0, "an empty row counted a chip");

    const std::int64_t a = model::appendDroppedPath(row, "/tmp/a");
    const std::int64_t b = model::appendDroppedPath(row, "/tmp/b");
    const model::DropChip* first = model::chipAt(row, 0);
    const model::DropChip* second = model::chipAt(row, 1);
    expect(first != nullptr && second != nullptr, "a filled row answered nullptr");
    expect(first->id == a && second->id == b, "the row is not in creation order");
    expect(model::chipAt(row, 2) == nullptr, "chipAt answered a chip past the end");
    expect(model::chipCount(row) == 2, "chipCount disagreed with the row");
}

// The GLFW drop intake, driven through the named handler the way a real
// callback would fire: every path arrives inside one event, GLFW's array is
// only valid for the call, and one Pending chip per path lands in the same
// frame, in arrival order. A duplicate path is two chips (scenario 2's rule at
// the intake), a null entry is skipped, and a drop with no published row is
// dropped on the floor.
void scenario9_dropIntake() {
    g_scenario = 9;
    phase("drop intake");

    model::DropChipRow row;
    llmfun::tui::setDropRowTarget(&row);

    const char* dropped[3] = {"/tmp/a.md", "/tmp/a.md", nullptr};
    llmfun::tui::handleDropEvent(3, dropped);
    expect(model::chipCount(row) == 2, "the drop did not produce one chip per path");
    const model::DropChip* first = model::chipAt(row, 0);
    const model::DropChip* second = model::chipAt(row, 1);
    expect(first != nullptr && second != nullptr, "the two chips are not both in the row");
    expect(first->id == 1 && second->id == first->id + 1, "the ids are not in arrival order");
    expect(first->state == model::DropChipState::Pending &&
               second->state == model::DropChipState::Pending,
           "an intake chip did not start Pending");
    expect(first->label == "processing a.md..." && second->label == "processing a.md...",
           "the intake labels are not the pending basename form");
    expect(first->fullPath == "/tmp/a.md" && second->fullPath == "/tmp/a.md",
           "the intake did not keep the raw paths");

    std::string path;
    expect(model::popDroppedPath(row, path) && path == "/tmp/a.md",
           "the intake did not queue the paths in arrival order");
    expect(model::popDroppedPath(row, path) && path == "/tmp/a.md",
           "the duplicate path was not queued a second time");
    expect(!model::popDroppedPath(row, path), "a null drop entry queued a path");

    // Nothing in, nothing appended: a null or empty event and an event with no
    // published row are all the same no-op.
    llmfun::tui::handleDropEvent(0, dropped);
    llmfun::tui::handleDropEvent(1, nullptr);
    llmfun::tui::setDropRowTarget(nullptr);
    llmfun::tui::handleDropEvent(1, dropped);
    expect(model::chipCount(row) == 2, "an event without a published row appended a chip");
}

// The paired pop: the path rides with the chip id its Pending chip took, so D
// reports a drop by the id the drop created. The rule a path-only queue cannot
// keep is the duplicate path's: two chips, two ids, one path.
void scenario10_pairedPops() {
    g_scenario = 10;
    phase("paired pops");

    model::DropChipRow row;
    model::PendingDrop popped;
    expect(!model::popDroppedPath(row, popped), "an empty drop queue answered true");
    expect(popped.chipId == 0 && popped.path.empty(), "the empty pop wrote the pairing");

    const std::int64_t first = model::appendDroppedPath(row, "/tmp/same.md");
    const std::int64_t second = model::appendDroppedPath(row, "/tmp/same.md");
    expect(first != second, "the duplicate drop did not take a second id");

    expect(model::popDroppedPath(row, popped) && popped.chipId == first &&
               popped.path == "/tmp/same.md",
           "the first pop is not the first drop's pairing");
    expect(model::popDroppedPath(row, popped) && popped.chipId == second &&
               popped.path == "/tmp/same.md",
           "the second pop is not the second drop's pairing");
    expect(!model::popDroppedPath(row, popped), "the drained queue answered true");
    expect(popped.chipId == second, "the drained pop kept a pairing");

    // The path-only form answers the same FIFO order and sentinel, so a
    // consumer that never reports by id keeps its one-field pop.
    model::appendDroppedPath(row, "/tmp/again.md");
    std::string path;
    expect(model::popDroppedPath(row, path) && path == "/tmp/again.md",
           "the path-only pop is not the same queue");
    expect(!model::popDroppedPath(row, path), "the path-only pop invented a drop");
}

} // namespace

int main() {
    scenario1_ids();
    scenario2_doubleDrop();
    scenario3_pendingLabel();
    scenario4_setStatus();
    scenario5_popDrops();
    scenario6_cancels();
    scenario7_clear();
    scenario8_reads();
    scenario9_dropIntake();
    scenario10_pairedPops();
    std::printf("OK: %d scenarios\n", g_scenario);
    return 0;
}
