/// The thread block's vocabulary (design §4.5, task 26): what a v4 message is
/// once it is drawn as a *block* rather than as a role row plus a body.
///
/// One home, two readers: the store adapter (`store/`) translates the v4 message
/// store into these descriptors, and the block renderer (`views/blocks.*`) draws
/// them. That is why the vocabulary is here and not in `views/`: a view may include
/// `model/`, and the adapter may include `model/`, but the adapter may not include a
/// view (D-ADAPTER - `views → ui + model + store`, never the reverse). A `using`
/// alias in `views/blocks.h` gives the renderer the `views::BlockKind` spelling the
/// task sketch uses.
///
/// Plain data only: no renderer, no ImGui, no `ui::`, no v4 type. `BlockKind` names
/// the anatomy the design lists, one enumerator per distinct drawing:
///
///   User          role label + rule, markdown body, copy
///   Answer        the run's result: same shape, the ANSWER role
///   WorkGroup     a quiet disclosure header ("WORK · 4 steps") over body rows
///   Assistant     one work-group body row (agent prose)
///   ToolCall      a compact row: state glyph + TOOL label + one-line summary
///   ToolResult    the same row with the outcome the store's marker carried
///   Thinking      a collapsed "Thinking · 3.2 s" disclosure with a dim body
///   System        label SYSTEM, neutral text, optional action (Retry, task 45)
///   Code          a body that is a single fenced code block: drawn by rich_md's
///                 own block, copied as *code* by the block's copy path (S-13)
///
/// `Assistant` is the one name the design's list does not spell out: §4.5 calls the
/// work group's content "body rows" rather than a kind of its own, and a flat block
/// list needs a name for them (the alternative - drawing them with the group's own
/// kind - would let a row claim the disclosure that belongs to the group).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace llmfun::tui::model {

/// The anatomy above. Append-only: a new kind is a new drawing, never a rename.
enum class BlockKind {
    User,
    Answer,
    WorkGroup,
    Assistant,
    ToolCall,
    ToolResult,
    Thinking,
    System,
    Code,
};

/// The state a block paints beside its label - mirrored from the store, never
/// invented by the renderer (D-BLOCK: "state via glyph + word"). `None` means the
/// block carries no state mark at all (a user query has no outcome; a system
/// notice's severity is the text's, not the row's).
///
/// Where the store has no state (a tool *call* has no outcome of its own - its
/// result is the next row) the adapter states what the row is: `Pending`, "the
/// call this result answers".
enum class BlockState {
    None,
    Pending,
    Running,
    Ok,
    Failed,
};

/// The streaming message's reserved key. The stream is not in the message store;
/// its blocks live in the stream group, and this is their key within that group.
inline constexpr std::size_t kStreamMessageId = 0;
/// The stable identity of one block: the group that owns it, the adapter's key
/// within that group, and the kind (a message can yield a thinking row and a body
/// row). Never a position: inserting a row in front of a block or changing the
/// paging window does not re-point it. `group == 0` is invalid (group ids start at
/// 1), so a default-constructed BlockId means "nothing".
struct BlockId {
    std::size_t group = 0;
    std::size_t key = 0;
    BlockKind kind = BlockKind::System;

    bool valid() const { return group != 0; }
    bool operator==(const BlockId& other) const {
        return group == other.group && key == other.key && kind == other.kind;
    }
    bool operator!=(const BlockId& other) const { return !(*this == other); }
    bool operator<(const BlockId& other) const {
        if (group != other.group)
            return group < other.group;
        if (key != other.key)
            return key < other.key;
        return static_cast<int>(kind) < static_cast<int>(other.kind);
    }
};

/// One block, as the renderer needs it.
///
/// `group` names the group that owns the block; `key` is the block's key within
/// that group - the adapter's message id (0 is the reserved streaming message id)
/// or the group id for the group's own header. `group`, `key` and `kind` together
/// form the block's `BlockId`: two frames that draw the same block answer the same
/// identity, and a paging window cannot remap it (D-GROUPID). Group ids start at
/// 1 and are never reused, so a default `BlockId` (`group == 0`) is invalid.
///
/// `summary` is the one-line content the row shows while it is collapsed (and the
/// tool rows' second line). It is *display text*: emoji have already been removed
/// by the adapter (task 26's "no emoji in any summary"), and a duration has already
/// become `timingS`, so nothing in it is a decoration to re-parse.
///
/// `timingS` is the block's duration in seconds, 0 when the store carries none. A
/// number rather than the store's "1.2s" string: the row formats it once, for both
/// profiles, and a test can compare the arithmetic instead of a spelling.
struct BlockDesc {
    BlockKind kind = BlockKind::System;
    BlockState state = BlockState::None;
    /// The group that owns the block; 0 when the descriptor is not from a group
    /// (a pure render test), which makes `id()` invalid.
    std::size_t group = 0;
    std::size_t key = 0;
    std::string label;
    std::string summary;
    double timingS = 0.0;
    /// A work group's step count, 0 for every other kind.
    int steps = 0;
    /// Whether the row offers a disclosure, and whether its body is showing now.
    bool expandable = false;
    bool expanded = true;
    /// Whether there is a body to draw when the row is open. A body-less block
    /// (`hasBody == false`) opens nothing, so the caller must not reserve room.
    bool hasBody = false;
    /// The streaming message: drawn open, and the row says so (no timing, no
    /// preview - its body is being written under it).
    bool streaming = false;

    /// The live answer's phase word, and its clock in milliseconds (design FR-08, task 38):
    /// the streaming row's header reads `AGENT · writing · 00:12`. Empty and 0 for every
    /// stored block - a message that is not being written has a duration (`timingS`), not a
    /// phase - so the row draws the same header it always did for them. `elapsedMs` is a
    /// *clock* (`mm:ss`, the top bar's formatter) where `timingS` is a duration ("1.2 s"):
    /// the live row counts up, a step's row states what it took.
    std::string phase;
    std::int64_t elapsedMs = 0;

    /// The block's identity; `group == 0` means the descriptor did not come from a
    /// group, so the returned id is invalid.
    BlockId id() const { return BlockId{group, key, kind}; }
};

} // namespace llmfun::tui::model
