/// The typed thread adapter (thread_store.h owns the contract).
///
/// This file is the projecting half of the thread adapter: the
/// group build and the block model. It draws no thread of its
/// own - the views do the drawing. A view asks `beginGroup()`/
/// `blockAt()` for a row, draws its chrome with `views::renderBlock()` and its body
/// with `renderBlockBody()`, which is what keeps "the content is the store's, the
/// chrome is the view's" a property of the construction rather than of a screenshot
/// comparison.
#include "thread_store.h"

#include "fnv1a.h"
#include "tui.h"
#include "tui_common.h"
#include "tui_widgets.h"

#include "imgui/imgui_internal.h"

#include "ui/icons.h"
#include "ui/layout.h"
#include "ui/markdown.h"
#include "ui/metrics.h"
#include "ui/table.h"
#include "ui/text.h"
#include "ui/tokens.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace llmfun::tui::store {

// ---------------------------------------------------------------------------
// The thread as data: groups, summaries, expansion
// ---------------------------------------------------------------------------

// One message body: model markdown through the wrapper (ui/markdown.h), verbatim
// wrapped text when markdown is not wanted (ToolCall/ToolResponse - a transcript,
// not prose) and for a headless state with no rich context (a harness that never
// called tuiInit keeps its plain output).
//
// Everything rich_md needs lives in the wrapper (D-RAW): `RenderRaw` and never
// `Render` (which resolves `![[...]]` by reading files), the block's stable id
// scope, the token theme and the profile's selection policy. This file only
// decides *whether* a body is markdown.
//
// The table bridge is `ui::bridgeBodyForProfile()` (design §6.8, D-TABLE, task 37),
// applied here because this is the one place a markdown body reaches the renderer: the
// agent streams and the block rows both come through it, so no two surfaces can render
// one table two ways. A verbatim body is *not* bridged - a fence inside a tool
// transcript would be printed literally.
static void renderBodyText(std::string_view text, bool asMarkdown = true,
                           bool showTableSource = false) {
    if (!asMarkdown || !ui::markdownReady()) {
        textUnformattedMultiline(text);
        return;
    }
    // A body without a `|` cannot hold a table (the detection wants a top-level
    // separator), so the ordinary body pays a scan and no copy at all - and a body that *has*
    // a pipe pays one only when the bridge actually rewrites it, because the seam writes into
    // `bridged` and answers whether it did. Nothing here copies a body twice.
    //
    // The profile, measured once: the bridge and the theme below read the same answer.
    const ui::Metrics m = ui::metricsFor();
    std::string bridged;
    if (text.find('|') != std::string_view::npos &&
        ui::bridgeBodyForProfile(m.textGrid, text, showTableSource, bridged)) {
        // `text` now views `bridged`, a local of this function: it outlives the render below and
        // is never read after it, which is the whole of the lifetime contract.
        text = bridged;
    }
    // The caller pushed maxX-1 for the plain path; the wrapper neutralises it for
    // the markdown path only, because rich_md wraps to the region itself and a
    // non-zero outer wrap position would wrap every paragraph twice.
    const ui::MarkdownTheme theme = ui::markdownThemeFromTokens(m.textGrid);
    ui::renderModelText(text, theme, ui::markdownSelectable(m.textGrid));
}

/// A summary collapsed to one line: every run of blanks (and the newlines a
/// multi-line tool call carries) becomes one space, so a row can never be widened by
/// text the header cannot show. The body keeps the original.
std::string oneLine(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    bool pendingSpace = false;
    for (const char c : text) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v') {
            pendingSpace = !out.empty();
            continue;
        }
        if (pendingSpace) {
            out.push_back(' ');
            pendingSpace = false;
        }
        out.push_back(c);
    }
    return out;
}

/// Separator between the role label and the time (and before the preview).
/// The grid stays ASCII: its chrome must not depend on the terminal's locale
/// (under LC_ALL=C ncurses turns U+00B7 into U+FFFD), while the pixel profile
/// can afford the middle dot.
static const char* roleSeparator() { return tuiIsTextGrid() ? " - " : u8" · "; }

ThreadStoreAdapter::ThreadStoreAdapter(TuiState& state)
    : chat_(&state.chat), thread_(&state.ui.thread) {}

void ThreadStoreAdapter::bind(TuiState& state) {
    chat_ = &state.chat;
    thread_ = &state.ui.thread;
}

void ThreadStoreAdapter::bind(ChatTab& chat, model::ThreadState& thread) {
    // The mount rebinds every frame with the pair it was given; a *different* store
    // than the caches were built from (the user moved the focus to another agent) is
    // a full drop: the caches are group ids of one store's lines, so carrying them to
    // another store would answer agent B's ids for agent A's entries. The reset also
    // discards the view state bound next to the caches: a fresh `ThreadState`.
    if (chat_ != &chat)
        *this = ThreadStoreAdapter();
    chat_ = &chat;
    thread_ = &thread;
}

void ThreadStoreAdapter::dropChat(const ChatTab* chat) {
    // The store this mount was built from is being destroyed (an eviction, a
    // pipeline clear): the binding - and with it the caches that name its lines -
    // goes with it. A different store's eviction leaves this mount alone.
    if (chat_ == chat)
        *this = ThreadStoreAdapter();
}

void ThreadStoreAdapter::sync() const { syncTyped(); }

// The per-turn change fingerprint: every field that changes what is drawn,
// folded into one number. A rebuild compares it per turn to bump the turn's
// revision - the `view.revision` a height hint and an anchor key on - so an
// upsert that merged new content into an old turn is a visible change, not a
// silent same.
static std::uint64_t typedTurnFingerprint(const store::TypedThreadStore& typed,
                                          std::int64_t turnId) {
    Fnv1a64 h;
    h.fold(static_cast<std::uint64_t>(typed.blockCount(turnId)));
    const std::size_t blockN = typed.blockCount(turnId); // hoisted: a bound
    // re-queried per iteration is a hash lookup per block (the store is a map).
    for (std::size_t b = 0; b < blockN; ++b) {
        const store::TypedBlock& block = typed.blockAt(turnId, b);
        h.foldBytes(block.id);
        h.fold(static_cast<std::uint64_t>(block.kind));
        h.fold(static_cast<std::uint64_t>(block.status));
        h.fold(static_cast<std::uint64_t>(block.flags));
        h.foldBytes(block.title);
        h.foldBytes(block.meta);
        h.foldBytes(block.body);
        h.fold(static_cast<std::uint64_t>(block.durationMs));
    }
    return h.value();
}

// The one chips composer: distinct names in first-occurrence
// order, empty names never chip, the first three drawn then `+N` for the rest.
std::string chipsText(const std::vector<std::string>& toolNames) {
    std::vector<std::string> names;
    for (const std::string& name : toolNames) {
        if (name.empty())
            continue;
        if (std::find(names.begin(), names.end(), name) == names.end())
            names.push_back(name);
    }
    if (names.empty())
        return std::string();
    const std::size_t shown = std::min<std::size_t>(names.size(), 3);
    std::string out;
    for (std::size_t i = 0; i < shown; ++i) {
        if (i != 0)
            out.push_back(' ');
        out.push_back('[');
        out += names[i];
        out.push_back(']');
    }
    if (names.size() > shown) {
        out += " +";
        out += std::to_string(names.size() - shown);
    }
    return out;
}

// The child order inside one batch block: reasoning, then content,
// then the call cards in call order - whatever order the wire sent. Live sends
// content first, replay sends reasoning first; the projection is the one that
// decides. Anything else a response batch carries (a notice riding it, an
// unknown kind) keeps its insertion order after the calls.
enum class ChildRank { Reasoning = 0, Content = 1, Call = 2, Other = 3 };

ChildRank childRank(model::BlockKind kind) {
    switch (kind) {
    case model::BlockKind::Thinking:
        return ChildRank::Reasoning;
    case model::BlockKind::Assistant: // the batch's prose (a Code prose too)
    case model::BlockKind::Code:
        return ChildRank::Content;
    case model::BlockKind::ToolStep:
        return ChildRank::Call;
    default:
        return ChildRank::Other;
    }
}

// The typed-backed rebuild: one group per stored turn, one row per
// displayed block. The group id IS the persisted turnId, and the rebuild
// guard is the store's mutation counter: an unchanged store rebuilds nothing.
// Overrides, expansion flags and hints survive upserts, re-emission and
// rebuilds because nothing is renumbered.
void ThreadStoreAdapter::syncTyped() const {
    if (!bound()) {
        groups_.clear();
        indexById_.clear();
        typedRevision_.clear();
        typedFingerprints_.clear();
        typedSynced_ = false;
        return;
    }
    ChatTab& chat = *chat_;
    const store::TypedThreadStore& typed = chat.typedTurns;
    if (typedSynced_ && syncedTypedVersion_ == typed.version())
        return;
    typedSynced_ = true;
    syncedTypedVersion_ = typed.version();

    std::vector<Entry> groups;
    groups.reserve(typed.turnCount());
    for (std::size_t k = 0; k < typed.turnCount(); ++k) {
        const store::TypedTurn& turn = typed.turnAt(k);
        const size_t id = static_cast<size_t>(turn.id);
        const std::uint64_t fingerprint = typedTurnFingerprint(typed, turn.id);
        const auto known = typedFingerprints_.find(turn.id);
        if (known == typedFingerprints_.end())
            typedRevision_.emplace(turn.id, 0); // born at 0, like every non-stream group
        else if (known->second != fingerprint)
            typedRevision_[turn.id] += 1;
        typedFingerprints_[turn.id] = fingerprint;

        Entry entry;
        entry.view.id = id;
        entry.view.kind = GroupKind::AssistantWork;
        entry.view.revision = typedRevision_[turn.id];
        entry.view.typedTurn = true;
        // The turn's live state off the wire: Running from the upsert that
        // opens the turn until the done/stop upsert closes it.
        entry.turnLive = turn.state == store::kTurnStateRunning;
        const int steps = stepCount(typed, turn.id);
        entry.view.oneLineSummary = "WORK";
        entry.view.oneLineSummary.append(roleSeparator());
        entry.view.oneLineSummary.append(std::to_string(steps));
        entry.view.oneLineSummary.append(steps == 1 ? " step" : " steps");

        // The blocks: the group's own header row, then one row per displayed
        // block, in insertion order. The wire's block vocabulary maps onto
        // model/block.h's extended one; a wire kind this build does not know
        // draws as a system notice, never as a neighbour's row.
        BlockEntry header;
        header.kind = model::BlockKind::WorkGroup;
        header.key = id;
        header.group = true;
        header.steps = steps;
        header.expandable = true;
        entry.blocks.push_back(header);

        // One hash lookup for the bound, not one per iteration (blockCount is a
        // map probe; the fingerprint loop hoists the same way).
        const std::size_t blockN = typed.blockCount(turn.id);
        // the turn's meta-"user" prose. A turn without one falls back to its first prose,
        // the pre-meta shape. The pipeline's streamed content carries the
        // agent role as meta instead, so it renders as assistant work inside
        // the group, never as the turn's query.
        std::string queryId;
        std::string queryBatchId; // the query row's batch - not a response batch
        std::string firstProseId;
        std::string firstProseBatchId;
        bool sawProseMeta = false;
        for (std::size_t b = 0; b < blockN; ++b) {
            const store::TypedBlock& block = typed.blockAt(turn.id, b);
            if (block.kind != store::kBlockKindProse)
                continue;
            if (firstProseId.empty()) {
                firstProseId = block.id;
                firstProseBatchId = block.batchId;
            }
            // TODO: meta should not be a string but an enum, if possible, to
            // avoid coupling strings in D and C++ with each other.
            if (block.meta == "user") {
                queryId = block.id;
                queryBatchId = block.batchId;
                break; // the first user-tagged prose wins
            }
            sawProseMeta = sawProseMeta || !block.meta.empty();
        }
        if (queryId.empty() && !sawProseMeta) {
            queryId = firstProseId; // the pre-meta fallback
            queryBatchId = firstProseBatchId;
        }
        // The rows in wire order first: the bucketing below reads them whole,
        // and the emission walks them by wire index (the owner row's anchor).
        std::vector<BlockEntry> rows;
        rows.reserve(blockN);
        for (std::size_t b = 0; b < blockN; ++b) {
            const store::TypedBlock& block = typed.blockAt(turn.id, b);
            if (!blockDisplayable(block))
                continue;
            BlockEntry row;
            row.timingS =
                block.durationMs != 0 ? static_cast<double>(block.durationMs) / 1000.0 : 0.0;
            // Keying: the row's `key` is the
            // block id's hash - stable per block id, but not injective.
            row.key = std::hash<std::string>{}(block.id);
            row.typedTurnId = turn.id;
            row.typedBlockId = block.id;
            row.typedBatchId = block.batchId;
            row.typedTitle = block.title;
            row.typedBody = block.body;
            switch (block.kind) {
            case store::kBlockKindProse: // the query row vs the streamed assistant content
                if (block.id == queryId) {
                    row.typedQuery = true;
                    row.kind = model::BlockKind::User;
                } else {
                    row.kind = model::BlockKind::Assistant;
                }
                break;
            case store::kBlockKindReasoning:
                row.kind = model::BlockKind::Thinking;
                row.thinking = true;
                break;
            case store::kBlockKindTool: // the merged card (call + result, one row)
                row.kind = model::BlockKind::ToolStep;
                row.resultPairs = block.resultPairs; // the card's RESULT rows
                row.resultRaw = block.resultRaw;
                row.resultMore = block.resultMore;
                break;
            case store::kBlockKindAnswer:
                row.kind = model::BlockKind::Answer;
                break;
            case store::kBlockKindNotice:
                row.kind = model::BlockKind::System;
                break;
            default:
                row.kind = model::BlockKind::System; // an unknown wire kind: a notice
                break;
            }
            // A row's disclosure is a property of the row itself: only a
            // row that HAS a body to fold offers one. The owner row is
            // constructed expandable at its emission (its initialiser,
            // below); every other row's offer comes from its kind. A body
            // segment or a query/answer sentinel keeps false, so the
            // setBlockExpanded guard answers false for them.
            row.expandable = row.kind == model::BlockKind::WorkGroup ||
                             row.kind == model::BlockKind::Thinking ||
                             row.kind == model::BlockKind::ToolStep;
            // The row's state comes straight off the wire status; the emitter
            // owns transitions - the store merges, it never enforces one.
            switch (block.status) {
            case store::kBlockStatusPending:
                row.state = model::BlockState::Pending;
                break;
            case store::kBlockStatusRunning:
                row.state = model::BlockState::Running;
                break;
            case store::kBlockStatusOk:
                row.state = model::BlockState::Ok;
                break;
            case store::kBlockStatusFailed:
                row.state = model::BlockState::Failed;
                break;
            case store::kBlockStatusCancelled:
                row.state = model::BlockState::Cancelled;
                break;
            default:
                break;
            }
            row.code =
                row.kind != model::BlockKind::ToolStep && !ui::fencedCodeOf(block.body).empty();
            // The query and the answer project as turn-level rows outside
            // the group - the query draws above the header, the answer after it;
            // the group's row list carries only work rows. The store is untouched - this is
            // projection only.
            if (row.kind == model::BlockKind::User) {
                entry.queryRow = std::move(row);
                entry.hasQuery = true;
            } else if (row.kind == model::BlockKind::Answer) {
                entry.answerRow = std::move(row);
                entry.hasAnswer = true;
            } else {
                rows.push_back(std::move(row));
            }
        }
        // The bucketing: the rows group by batch id - every batch but the
        // query's is a response batch (ids are opaque, nothing parses them).
        // The rows keep their wire order; each records the bucket it joined,
        // and the batch accumulates the facts its owner row and the tests
        // read - all of them, before any drop: the emission drops rows, never facts.
        std::unordered_map<std::string, std::size_t> bucketOf;
        std::vector<std::vector<std::string>> batchCalls; // per batch: call names, in call order
        for (std::size_t i = 0; i < rows.size(); ++i) {
            BlockEntry& row = rows[i];
            if (entry.hasQuery && row.typedBatchId == queryBatchId)
                continue; // the query batch's rows stay plain - its notice rides along
            std::size_t slot;
            const auto found = bucketOf.find(row.typedBatchId);
            if (found == bucketOf.end()) {
                slot = entry.batches.size();
                bucketOf.emplace(row.typedBatchId, slot);
                BatchEntry batch;
                batch.batchId = row.typedBatchId;
                batch.firstRow = i;
                entry.batches.push_back(std::move(batch));
                batchCalls.emplace_back();
            } else {
                slot = found->second;
            }
            BatchEntry& batch = entry.batches[slot];
            row.batch = slot;
            row.batchOwned = true;
            batch.hasReasoning = batch.hasReasoning || row.kind == model::BlockKind::Thinking;
            batch.hasContent = batch.hasContent || row.kind == model::BlockKind::Assistant;
            if (row.kind == model::BlockKind::ToolStep) {
                batch.hasAnyCall = true;
                batchCalls[slot].push_back(row.typedTitle);
                if (row.state == model::BlockState::Failed) {
                    batch.failedCalls += 1;
                    batch.anyFailed = true;
                }
                if (row.state != model::BlockState::Ok && row.state != model::BlockState::Failed &&
                    row.state != model::BlockState::Cancelled)
                    batch.allSettled = false;
            }
            // The batch's duration is the content block's, read before
            // any drop - a live content block says 0s until it ticks.
            if (row.kind == model::BlockKind::Assistant && row.timingS > 0.0 &&
                batch.durationS == 0.0)
                batch.durationS = row.timingS;
        }
        // The turn's last bucket is the newest.
        if (!entry.batches.empty())
            entry.batches.back().isNewest = true;
        // Buckets are created at their first row's visit, so the vector's
        // order IS the first-row order - a bucket born later always sits later.
        for (std::size_t i = 1; i < entry.batches.size(); ++i)
            assert(entry.batches[i - 1].firstRow < entry.batches[i].firstRow);
        // The batch's chips line, composed by the one chips composer.
        for (std::size_t i = 0; i < entry.batches.size(); ++i)
            entry.batches[i].chips = chipsText(batchCalls[i]);

        // The Answer's own batch suppresses its content child, so the answer
        // text draws once, on the turn-level ANSWER row. The Answer block names
        // its batch (the row's typedBatchId), so the projection reads it with
        // no id parsing; the batch's facts were all read in the bucket pass, so
        // the drop moves no count. A final response whose preamble differs from its answer loses
        // that preamble.
        const std::string answerBatch =
            entry.hasAnswer ? entry.answerRow.typedBatchId : std::string();

        // The emission: the wire order decides only WHERE each batch's block
        // starts - at its first row's index. The block itself is canonical: one
        // owner row when a child survived the drops, then the children in rank
        // order, contiguous; a plain row that lay between the batch's rows is
        // emitted after the block, at the first wire position the walk reaches after it.
        std::vector<char> batchDrawn(entry.batches.size(), 0);
        for (std::size_t i = 0; i < rows.size(); ++i) {
            if (rows[i].batch != kNoBatch && batchDrawn[rows[i].batch])
                continue; // consumed by the block emitted at the batch's first row
            BlockEntry row = std::move(rows[i]);
            if (row.batch == kNoBatch) {
                // A plain row: the query batch's rows ride along, and so does
                // anything the projection did not bucket.
                entry.blocks.push_back(std::move(row));
                continue;
            }
            batchDrawn[row.batch] = 1;
            const std::size_t slot = row.batch;
            // A content/reasoning row whose body never arrived would draw
            // a bare AGENT header above nothing, so it goes - the batch's
            // duration was already read in the bucket pass.
            std::vector<BlockEntry> children;
            children.push_back(std::move(row));
            for (std::size_t j = i + 1; j < rows.size(); ++j)
                if (rows[j].batch == slot)
                    children.push_back(std::move(rows[j]));
            std::vector<BlockEntry> kept;
            kept.reserve(children.size());
            for (BlockEntry& child : children) {
                const bool bodyLess =
                    child.typedBody.empty() && (child.kind == model::BlockKind::Assistant ||
                                                child.kind == model::BlockKind::Thinking);
                // The answer batch's CONTENT child never draws - the ANSWER
                // row carries the text. The batch's reasoning and calls are
                // not the answer; they stay.
                // Guarded on hasAnswer: without an Answer row no batch is
                // suppressed, answerBatch being empty must not mean "batch
                // empty-string".
                const bool answerOwned = entry.hasAnswer && answerBatch == child.typedBatchId &&
                                         child.kind == model::BlockKind::Assistant;
                if (!bodyLess && !answerOwned)
                    kept.push_back(std::move(child));
            }
            // Rank order, stable: each rank keeps its own wire order, so the
            // calls stay in call order and a rank's ties never reorder.
            std::stable_sort(kept.begin(), kept.end(),
                             [](const BlockEntry& a, const BlockEntry& b) {
                                 return childRank(a.kind) < childRank(b.kind);
                             });
            // The batch's surviving content body: the first kept
            // Assistant child's typed body, the owner row's copy source. A
            // calls-only batch keeps it empty - its owner copies nothing.
            BatchEntry& batch = entry.batches[slot];
            for (const BlockEntry& child : kept)
                if (child.kind == model::BlockKind::Assistant && batch.contentBody.empty()) {
                    batch.contentBody = child.typedBody;
                    // The copy's fence question rides with the body: the
                    // owner's copy answers it from the child the body came from.
                    batch.contentIsCode = child.code;
                }
            if (!kept.empty()) { // the owner exists iff a child survived
                BlockEntry owner;
                owner.kind = model::BlockKind::Assistant; // the AGENT row (blockAt labels it)
                owner.timingS = batch.durationS;
                owner.expandable = true;
                // Keying, same as the wire rows: the owner's
                // namespaced id's hash - stable, but not injective.
                owner.key = std::hash<std::string>{}("batch#" + batch.batchId);
                owner.typedTurnId = turn.id;
                owner.typedBlockId = "batch#" + batch.batchId;
                owner.typedBatchId = batch.batchId;
                owner.batch = slot;
                owner.batchOwned = true;
                owner.ownsBatch = true;
                // The state stays None: no wire status is written by the
                // C++ side - the FAIL word and the chips are the batch's own
                // facts, and the tasks that draw them read `Entry::batches`.
                entry.blocks.push_back(std::move(owner));
            }
            for (BlockEntry& child : kept)
                entry.blocks.push_back(std::move(child));
        }
        // A turn's row ids never repeat - the owner namespace (`batch#...`)
        // colliding with a wire block id would silently merge two rows.
#ifndef NDEBUG
        {
            std::set<std::string> ids;
            for (const BlockEntry& row : entry.blocks) {
                const auto inserted = ids.insert(row.typedBlockId);
                assert(inserted.second && "a turn's row ids never repeat");
            }
        }
#endif
        // A rough estimate only: the rows a *folded* turn draws - the
        // header, the plain rows and the owner rows. A batch child (thinking,
        // content, a card) draws behind its owner's fold, so it weighs nothing.
        // Mode- and pin-independent on purpose: the hint is computed here while
        // visibility resolves per frame; the drawn height replaces it after the first draw.
        int plainRows = 0;
        int ownerRows = 0;
        for (const BlockEntry& row : entry.blocks) {
            if (row.group)
                continue; // the header: the +1 below counts it once
            if (row.ownsBatch)
                ++ownerRows;
            else if (!row.batchOwned)
                ++plainRows;
        }
        entry.view.lineCountEstimate = std::max(1, 1 + plainRows + ownerRows);
        entry.view.hasAnswer = entry.hasAnswer; // the copy-row stamp reads this
        groups.push_back(std::move(entry));
    }

    // Turns that are gone take their revision and fingerprint with them: ids
    // are never reused, so a stale entry could only be re-armed by a collision.
    for (auto it = typedFingerprints_.begin(); it != typedFingerprints_.end();) {
        if (typed.turnById(it->first) == nullptr) {
            typedRevision_.erase(it->first);
            it = typedFingerprints_.erase(it);
        } else {
            ++it;
        }
    }

    groups_ = std::move(groups);
    // group id -> its index: the lookup `entryFor()` needs, a probe instead of
    // a walk of the thread.
    indexById_.clear();
    for (std::size_t i = 0; i < groups_.size(); ++i)
        indexById_.emplace(groups_[i].view.id, i);
    ++groupBuilds_;

    // The hint prune: a hint for a turn that is gone is memory kept for a
    // group that no longer exists, and ids are never reused so dropping it
    // cannot lose a live hint.
    auto& hints = thread_->hints;
    for (auto it = hints.begin(); it != hints.end();)
        it = indexById_.count(it->first) != 0 ? std::next(it) : hints.erase(it);
    // window counts the stored groups and the view appends the live one after
    // them - and
    // the frame layer's clock reads `hasStream()` off this mark, so a turn that
    // has closed stops the clock by the same flag.
    if (!groups_.empty() && typed.turnAt(typed.turnCount() - 1).state == store::kTurnStateRunning)
        groups_.back().stream = true;
}

size_t ThreadStoreAdapter::groupCount() const {
    sync();
    return groups_.size();
}

GroupView ThreadStoreAdapter::groupAt(size_t k) const {
    sync();
    if (k >= groups_.size())
        return GroupView{};
    return groups_[k].view;
}

bool ThreadStoreAdapter::hasStream() const {
    sync();
    return !groups_.empty() && groups_.back().stream;
}

size_t ThreadStoreAdapter::storedGroupCount() const {
    sync();
    const size_t total = groups_.size();
    return !groups_.empty() && groups_.back().stream ? total - 1 : total;
}

bool ThreadStoreAdapter::streamHasText() const {
    sync();
    if (groups_.empty() || !groups_.back().stream)
        return false;
    // The streamed answer's text: the content block the ticks merge in place
    // (the turn's second Prose row, an Assistant row) - or the Answer row the
    // step's completion upserts. Either means the reasoning has ended.
    for (const BlockEntry& b : groups_.back().blocks)
        if ((b.kind == model::BlockKind::Answer || b.kind == model::BlockKind::Assistant) &&
            !b.typedBody.empty())
            return true;
    return false;
}

bool ThreadStoreAdapter::streamHasThinking() const {
    sync();
    if (groups_.empty() || !groups_.back().stream)
        return false;
    for (const BlockEntry& b : groups_.back().blocks)
        if (b.kind == model::BlockKind::Thinking && !b.typedBody.empty())
            return true;
    return false;
}

const ThreadStoreAdapter::Entry* ThreadStoreAdapter::entryFor(size_t id) const {
    sync();
    // A map probe, not a walk: this runs for every `expanded()`/`measureHint()` call,
    // i.e. a few times per visible group per frame, and a linear scan there is the
    // per-frame walk of the whole store that incremental enumeration forbids.
    //
    // `entryWalks_` counts the entries *examined*, so it is the honest measurement of
    // that claim: a probe touches one entry on a hit and none on a miss, where the
    // scan this replaced touched between one and `groupCount()` of them. The cost test
    // in `./test_thread_window` is the second half - it sizes the same work against a
    // thread ten times longer.
    const auto found = indexById_.find(id);
    if (found == indexById_.end())
        return nullptr;
    ++entryWalks_; // one entry touched
    return &groups_[found->second];
}

void ThreadStoreAdapter::setHintContext(float width, bool compact) {
    hintWidth_ = width;
    hintCompact_ = compact;
}

size_t ThreadStoreAdapter::takeExpandedGroup() {
    const size_t id = expandedGroup_;
    expandedGroup_ = 0;
    return id;
}
bool ThreadStoreAdapter::minimalMode() const { return !bound() || chat_->threadMinimalMode; }

bool ThreadStoreAdapter::groupOpen(size_t groupId) const {
    if (!bound())
        return false;
    // The absolute pin first: a click stored open/closed and that is the
    // answer, whatever the mode and the turn's liveness say. Then the
    // liveness default: a group draws open only while its turn
    // is live in maximal mode - maximal at rest folds like minimal.
    const auto pinned = chat_->groupPins.find(static_cast<std::int64_t>(groupId));
    if (pinned != chat_->groupPins.end())
        return pinned->second;
    const Entry* entry = entryFor(groupId);
    return entry != nullptr && !chat_->threadMinimalMode && entry->turnLive;
}

bool ThreadStoreAdapter::groupStyleIsMinimal(size_t groupId) const {
    if (!bound())
        return true;
    // The SHAPE, not the fold: the mode is minimal AND the group is
    // not drawn open. A pinned-open minimal group and a live maximal group
    // both draw maximal-style; a folded maximal group never draws the chips.
    return chat_->threadMinimalMode && !groupOpen(groupId);
}

bool ThreadStoreAdapter::expanded(size_t id) const { return groupOpen(id); }

void ThreadStoreAdapter::setExpanded(size_t id, bool open) {
    sync();
    if (!bound())
        return;
    const auto found = indexById_.find(id);
    if (found == indexById_.end())
        return;
    // The writer is absolute: no "drop the entry when it equals the default" -
    // the pin survives a mode flip and a liveness change and answers what was stored.
    chat_->groupPins[static_cast<std::int64_t>(id)] = open;
}

void ThreadStoreAdapter::collapseAll() {
    // The escape hatch: the pins are absolute, so the user needs
    // one action that forgets them all. It writes no status and stores no
    // default - it only clears. `expandedGroup_` is the last-toggle latch, and
    // a fold that just emptied has no anchor to offer the next frame.
    if (!bound())
        return;
    chat_->rowPins.clear();
    chat_->groupPins.clear();
    expandedGroup_ = 0;
}

float ThreadStoreAdapter::measureHint(size_t id) const {
    const Entry* entry = entryFor(id);
    if (entry == nullptr || !bound())
        return 0.0f;
    const ui::Metrics m = ui::metricsFor();
    // The cache is `UiState::thread.hints` (design §5.1), keyed by the box the view
    // announced: a font-scale change moves `row`, a resize moves `width`, compact
    // mode changes how much of a group is drawn - and any of the three makes the
    // cached number a statement about a different box.
    const auto hint = thread_->hints.find(id);
    if (hint != thread_->hints.end()) {
        const model::HeightHint& cached = hint->second;
        if (cached.revision == entry->view.revision && cached.width == hintWidth_ &&
            cached.row == m.row && cached.compact == hintCompact_)
            return cached.h;
    }
    const int lines = std::max(1, entry->view.lineCountEstimate);
    return m.row * static_cast<float>(lines);
}

bool ThreadStoreAdapter::blockHasTable(size_t k, std::size_t b) const {
    const BlockEntry* block = blockEntryAt(k, b);
    if (block == nullptr || !bound() || block->kind == model::BlockKind::WorkGroup)
        return false;
    // A typed tool card is a transcript, drawn verbatim (renderBlockBody's one
    // non-markdown kind), so a `|` inside it must not offer a table toggle (`s`
    // would set a tableSource bit nothing reads - views/thread.cpp: a verbatim
    // row never sets it).
    if (block->kind == model::BlockKind::ToolStep)
        return false;
    const std::string body = blockText(k, b);
    // The renderer's own cheap guards, then the bridge's answer: a body with no pipe cannot hold
    // a table, and a body drawn without a rich context is plain text (nothing to show).
    return !body.empty() && body.find('|') != std::string::npos && ui::markdownReady() &&
           ui::bodyHasTable(ui::metricsFor().textGrid, body);
}

const ThreadStoreAdapter::BlockEntry* ThreadStoreAdapter::blockEntryAt(size_t k,
                                                                       std::size_t b) const {
    sync();
    if (k >= groups_.size())
        return nullptr;
    if (b == kQueryRow)
        return groups_[k].hasQuery ? &groups_[k].queryRow : nullptr;
    if (b == kAnswerRow)
        return groups_[k].hasAnswer ? &groups_[k].answerRow : nullptr;
    if (b >= groups_[k].blocks.size())
        return nullptr;
    return &groups_[k].blocks[b];
}

std::size_t ThreadStoreAdapter::blockCount(size_t k) const {
    sync();
    if (k >= groups_.size())
        return 0;
    return groups_[k].blocks.size();
}

std::size_t ThreadStoreAdapter::batchCount(size_t k) const {
    sync();
    if (k >= groups_.size())
        return 0;
    return groups_[k].batches.size();
}

model::BlockDesc ThreadStoreAdapter::blockAt(size_t k, std::size_t b) const {
    model::BlockDesc out;
    const BlockEntry* block = blockEntryAt(k, b);
    if (block == nullptr || !bound())
        return out;
    const Entry& entry = groups_[k];

    out.kind = block->kind;
    out.state = block->state;
    out.group = entry.view.id;
    out.key = block->key;
    out.timingS = block->timingS;
    out.steps = block->steps;
    out.expandable = block->expandable;
    // The derived predicate: visibility is computed HERE, before the view
    // sees the row - the view never re-derives it.
    out.visible = b == kQueryRow || b == kAnswerRow ? true // the turn-level rows draw in both modes
                                                    : resolveVisible(entry, *block);
    out.expanded = b == kQueryRow || b == kAnswerRow ? true : resolveOpen(entry, *block);
    out.batchOwned = block->batchOwned;
    // The batch's content child is a body segment: the view draws its
    // body, never a header of its own. Detected on the entry's kind,
    // before the fenced-code rewrite below renames a segment's body to `Code`.
    out.bodySegment =
        block->batchOwned && !block->ownsBatch && block->kind == model::BlockKind::Assistant;
    if (block->ownsBatch) {
        // The owner row is drawn from the batch's facts, not from its
        // (always empty) own body: the chips, the FAIL count and the copy
        // offer all read the batch.
        assert(block->batch != store::kNoBatch && "an owner row always carries its batch slot");
        const BatchEntry& batch = entry.batches[block->batch];
        out.ownsBatch = true;
        out.chips = batch.chips;
        out.failedCalls = batch.failedCalls;
        out.copyable = !batch.contentBody.empty();
    }

    if (block->kind == model::BlockKind::WorkGroup) {
        // The group's own header: "WORK · N steps" over the rows below it. It has no
        // body of its own - the rows are blocks.
        out.hasBody = false;
        out.label = "WORK";
        return out;
    }
    switch (block->kind) {
    case model::BlockKind::Thinking:
        out.label = "Thinking";
        break;
    case model::BlockKind::User:
    case model::BlockKind::Assistant:
        out.label = block->typedQuery ? "YOU" : "AGENT";
        break;
    case model::BlockKind::ToolStep:
        out.label = "TOOL";
        out.summary = oneLine(block->typedTitle);
        break;
    case model::BlockKind::Answer:
        out.label = "ANSWER";
        break;
    default:
        out.label = "SYSTEM";
        break;
    }
    if (block->code)
        out.kind = model::BlockKind::Code; // classified at build
    // Every row previews something: a tool card its tool name (set in the
    // switch), every other row its body's first line. Thinking has no preview:
    // its label is the count only and its open body is the full text.
    // The owner's preview is the batch's own face: the chips when the
    // batch has calls, the content's first line when it has none - an owner
    // has no body of its own to preview, and a batch with calls keeps the
    // summary empty, the chips carrying its face.
    if (block->kind != model::BlockKind::ToolStep && block->kind != model::BlockKind::Thinking) {
        out.summary = block->ownsBatch && !entry.batches[block->batch].hasAnyCall
                          ? oneLine(entry.batches[block->batch].contentBody)
                          : oneLine(block->typedBody);
    }
    // A card's body room is the args text OR its RESULT rows: an
    // empty-args card with a result still opens onto the result lines, so
    // `hasBody` covers both - a body-less, result-less card (a Cancelled one)
    // opens nothing.
    out.hasBody =
        !block->typedBody.empty() || !block->resultPairs.empty() || !block->resultRaw.empty();
    return out;
}

bool ThreadStoreAdapter::blockExpanded(size_t k, std::size_t b) const {
    const BlockEntry* block = blockEntryAt(k, b);
    if (block == nullptr || !bound())
        return false;
    // The turn-level sentinel rows are never expandable state
    // (queryOpen/answerOpen = true), the same answer blockAt() hard-codes into
    // desc.expanded - the two seams must agree.
    if (b == kQueryRow || b == kAnswerRow)
        return true;
    return resolveOpen(groups_[k], *block);
}

bool ThreadStoreAdapter::batchLiveFor(const Entry& entry, const BlockEntry& block) const {
    // The turn is live and the batch is the turn's newest. "Settled" means
    // the batch HELD a call and every call responded: a call-less batch (the
    // streaming thinking/prose tail) is never settled - allSettled defaults true
    // and only ToolStep rows touch it, so the literal predicate would fold the
    // live tail mid-stream. A batch whose calls
    // all responded folds; a plain row holds nothing open.
    if (block.batch == kNoBatch)
        return false;
    const BatchEntry& batch = entry.batches[block.batch];
    return entry.turnLive && batch.isNewest && (!batch.allSettled || !batch.hasAnyCall);
}

bool ThreadStoreAdapter::resolveOpen(const Entry& entry, const BlockEntry& block) const {
    // The header row IS the group: its body-open state is the group's own
    // pin-or-liveness answer, the same one `groupOpen()` draws.
    if (block.group)
        return groupOpen(entry.view.id);
    // A GROUP-LEVEL plain row (a notice, a vision block) is not expandable
    // state: it draws open when the group it rides in draws open.
    if (!block.batchOwned)
        return groupOpen(entry.view.id);
    // The absolute row pin first: what a click stored is the answer,
    // whatever the batch's liveness would default to.
    const auto turn = chat_->rowPins.find(static_cast<std::int64_t>(entry.view.id));
    if (turn != chat_->rowPins.end()) {
        const auto pinned = turn->second.find(block.typedBlockId);
        if (pinned != turn->second.end())
            return pinned->second;
    }
    // The live default: one predicate for the owner row and its children, so a
    // batch whose response is still streaming (no call has landed yet) draws its
    // thinking open with the owner, and the whole batch folds together when the
    // calls settle.
    return batchLiveFor(entry, block);
}

bool ThreadStoreAdapter::resolveVisible(const Entry& entry, const BlockEntry& block) const {
    const bool open = groupOpen(entry.view.id);
    // The header draws folded or open - it IS the fold (its own visibility is
    // not a predicate; `blockAt()` never asks).
    if (block.group)
        return true;
    // ownerVisible = groupOpen: an owner row never draws under a fold,
    // minimal-style or maximal.
    if (block.ownsBatch)
        return open;
    // plainVisible = groupOpen: a group-level plain row (a notice, a
    // vision block, a query batch's rider) draws with the group it rides in.
    if (!block.batchOwned)
        return open;
    const bool styleMinimal = groupStyleIsMinimal(entry.view.id);
    const BatchEntry& batch = entry.batches[block.batch];
    // The batch's content segment is the one child the minimal fold keeps
    // visible on its own: the newest batch of a live turn draws its prose
    // even though the owner above it never drew. Evaluated first: "the owner
    // is not drawn" and "the segment draws" are
    // compatible by construction.
    if (styleMinimal && entry.turnLive && batch.isNewest &&
        block.kind == model::BlockKind::Assistant)
        return true;
    // Every other child draws only under an open owner: the owner's
    // own open state is the pin-or-liveness answer `resolveOpen()` gives
    // the owner row's descriptor.
    const auto turn = chat_->rowPins.find(static_cast<std::int64_t>(entry.view.id));
    bool ownerOpen = batchLiveFor(entry, block);
    if (turn != chat_->rowPins.end()) {
        const auto pinnedOwner = turn->second.find("batch#" + batch.batchId);
        if (pinnedOwner != turn->second.end())
            ownerOpen = pinnedOwner->second;
    }
    return open && ownerOpen;
}

bool ThreadStoreAdapter::setBlockExpanded(size_t k, std::size_t b, bool open) {
    const BlockEntry* block = blockEntryAt(k, b);
    if (block == nullptr || !bound() || !block->expandable)
        return false;
    const Entry& entry = groups_[k];
    if (block->group) {
        // The group's pin: absolute, always stored - no
        // "drop the entry when it equals the default". The pin survives a mode flip and the
        // turn's liveness change and answers what was stored.
        chat_->groupPins[static_cast<std::int64_t>(entry.view.id)] = open;
    } else {
        // The row's pin: absolute, always stored, keyed by the wire block id
        // under the turn it belongs to. The owner rows are
        // pinned under their `batch#<batchId>` key, the same id the
        // visibility predicate reads for the owner's open state.
        chat_->rowPins[static_cast<std::int64_t>(entry.view.id)][block->typedBlockId] = open;
    }
    expandedGroup_ = entry.view.id;
    return open;
}

std::string ThreadStoreAdapter::blockText(size_t k, std::size_t b) const {
    const BlockEntry* block = blockEntryAt(k, b);
    if (block == nullptr || !bound() || block->kind == model::BlockKind::WorkGroup)
        return {};
    // The owner's copy target is its batch's content: the copy belongs
    // to the response, not to the paragraph it happens to live in. A
    // calls-only batch has no content, so its owner copies nothing.
    if (block->ownsBatch) {
        assert(block->batch != store::kNoBatch && "an owner row always carries its batch slot");
        return groups_[k].batches[block->batch].contentBody;
    }
    return block->typedBody;
}

bool ThreadStoreAdapter::blockCopyIsCode(size_t k, std::size_t b) const {
    const BlockEntry* block = blockEntryAt(k, b);
    if (block == nullptr || !bound())
        return false;
    // The owner copies its batch's content: the fence answer is the
    // content child's own `code` fact, captured beside the body it came from.
    if (block->ownsBatch) {
        assert(block->batch != store::kNoBatch && "an owner row always carries its batch slot");
        return groups_[k].batches[block->batch].contentIsCode;
    }
    return block->code;
}

void ThreadStoreAdapter::renderBlockBody(size_t k, std::size_t b, const ui::Rect& cell) {
    const BlockEntry* block = blockEntryAt(k, b);
    if (block == nullptr || !bound())
        return;
    if (!ui::rectMatchesProfile(cell, ui::metricsFor()))
        return;
    const Entry& entry = groups_[k];
    const std::string body = block->typedBody;
    const bool thinking = block->thinking;
    // A tool row is a transcript, not prose: it is drawn verbatim. Everything else goes
    // through the markdown wrapper.
    const bool markdown = block->kind != model::BlockKind::ToolStep;
    // A card's Result can exist without arguments: an empty body does not
    // end the draw - only a body-less, result-less row paints nothing. A
    // Cancelled card carries neither (no pairs, no raw), so it draws
    // header-only.
    const bool hasResult = block->kind == model::BlockKind::ToolStep &&
                           (!block->resultPairs.empty() || !block->resultRaw.empty());
    if (body.empty() && !hasResult)
        return;
    // The drawn body: the body's text verbatim. The query row draws open (the
    // fold keeps the turn's content rows), so the drawn body is the same text
    // the collapsed summary previews - a row draws one of the two, never both.
    const std::string drawn = body;
    // The table bridge is applied by `renderBodyText()` - the one place
    // a markdown body is drawn - and the block's own `s` toggle is what it needs from here: the
    // source flag names the block by its stable id, so a redraw after a paging change or a
    // stream update asks the same question.
    const model::BlockId blockId{entry.view.id, block->key, block->kind};
    const bool showTableSource = thread_->tableSource.find(blockId) != thread_->tableSource.end();

    ImGui::PushTextWrapPos(ImGui::GetContentRegionMax().x - 1);
    if (!body.empty()) {
        if (thinking) {
            // The dim body of a disclosure: `textMid` is the quiet ink that still clears the
            // contrast bar, and the end mark closes the region the header opened.
            ImGui::PushStyleColor(ImGuiCol_Text, ui::color(ui::ColorToken::textMid));
            renderBodyText(drawn, true, showTableSource);
            ImGui::PopStyleColor();
        } else {
            renderBodyText(drawn, markdown, showTableSource);
        }
    }
    if (block->kind == model::BlockKind::ToolStep &&
        (!block->resultPairs.empty() || !block->resultRaw.empty())) {
        // The embedded RESULT: one dim `RESULT <key> = <value>` line per
        // pair, the store's strings verbatim, never re-parsed. The packed
        // fallback line draws for an array, a non-JSON payload or a zero-pair
        // object (the same byte shape today's dim line rendered). The
        // "... K more" dim tail draws when the sender capped the pair set. The
        // card draws this only while it is open, so closing the card closes the
        // result with it. The status is the header's word, never repeated here.
        ImGui::PushStyleColor(ImGuiCol_Text, ui::color(ui::ColorToken::textMid));
        for (const auto& pair : block->resultPairs)
            renderBodyText("RESULT " + pair.first + " = " + pair.second, false, false);
        if (block->resultPairs.empty())
            renderBodyText("RESULT  " + oneLine(block->resultRaw), false, false);
        if (block->resultMore > 0) {
            const std::string more = "... " + std::to_string(block->resultMore) + " more";
            renderBodyText(more, false, false);
        }
        ImGui::PopStyleColor();
    }
    ImGui::PopTextWrapPos();
}

bool ThreadStoreAdapter::beginGroup(size_t k, const ui::Rect& cell) {
    sync();
    if (!bound() || k >= groups_.size())
        return false;
    // A rectangle in the wrong unit is a mixed-unit bug: fail closed (paint nothing)
    // rather than draw a plausible thread in the wrong coordinate space.
    // `rectMatchesProfile` answers in every build, asserts in debug.
    if (!ui::rectMatchesProfile(cell, ui::metricsFor()))
        return false;

    const Entry& entry = groups_[k];
    ++renderCalls_;
    // The expansion state *before* the draw. A block row toggles inside the view, a
    // row inside the view; this is the one place that samples both sides of
    // a group draw, so a change the caller did not report is still latched (task 25's
    // anchor reads it).
    groupExpandedBefore_ = expanded(entry.view.id);
    // A row inside an agent group toggles its own pin in `ChatTab::rowPins`,
    // which `expanded(groupId)` cannot see (it reads the group's own pin). Any
    // pin-map mutation shows in `truePins + totalPins`: a value flip of an
    // EXISTING pin (a second click on the same row) changes no membership, so
    // the true count catches it, and a newly stored or erased pin changes the
    // membership count. Either mutation alone would miss the other. (A
    // thinking disclosure lives in `UiState`, so that one is the caller's own
    // report.)
    //
    // This is only a toggle signal while the draw does not write the maps
    // itself: a pin is written once per click in `setBlockExpanded()`, so the
    // writes between these two samples are the user's own activations.
    std::size_t rowOpen = 0;
    std::size_t rowTrue = 0;
    for (const auto& [id, rows] : chat_->rowPins) {
        rowOpen += rows.size();
        for (const auto& [row, open] : rows)
            rowTrue += open ? 1 : 0;
    }
    for (const auto& [id, open] : chat_->groupPins) {
        rowTrue += open ? 1 : 0;
    }
    groupOpenBefore_ = rowOpen + rowTrue + chat_->groupPins.size();
    groupTop_ = ImGui::GetCursorScreenPos().y;
    // One block scope per group, keyed by the adapter's own group id (D-GROUPID:
    // monotonic, never reused, never a frame counter) - rich_md's fragment ids are keyed
    // to it, so paging and a collapsed-then-expanded group cannot remap them.
    ui::beginBlock(entry.view.id);
    groupOpen_ = true;
    return true;
}

void ThreadStoreAdapter::endGroup(size_t k, const ui::Rect& cell) {
    if (!bound() || k >= groups_.size() || !groupOpen_)
        return;
    const Entry& entry = groups_[k];
    ui::endBlock();
    groupOpen_ = false;

    // The expansion answer for this group: only a group that was *drawn* can have been
    // toggled, so this is also the bound on the id's lifetime. The answer is the group
    // id even when the toggle happened on a row inside it: the anchor can only hold a
    // header row the view knows about, so the group's header is what stays put - the
    // row above the content the user just changed.
    // The same combined sample beginGroup took: true pins plus membership, so
    // a value flip of an existing pin and a newly stored or erased pin both
    // move it.
    std::size_t rowOpen = 0;
    std::size_t rowTrue = 0;
    for (const auto& [id, rows] : chat_->rowPins) {
        rowOpen += rows.size();
        for (const auto& [row, open] : rows)
            rowTrue += open ? 1 : 0;
    }
    for (const auto& [id, open] : chat_->groupPins) {
        rowTrue += open ? 1 : 0;
    }
    const std::size_t openAfter = rowOpen + rowTrue + chat_->groupPins.size();
    if (expanded(entry.view.id) != groupExpandedBefore_ || openAfter != groupOpenBefore_)
        expandedGroup_ = entry.view.id;

    // The hint is refreshed *after* the draw, which is the only moment the height is
    // known. It is stored in `UiState::thread.hints` under the box this draw used, so a
    // stream update (new revision), a resize (new width), a font change (new row) or a
    // mode change invalidates it instead of anchoring to a stale number.
    //
    // The draw is also the authority for the *key*: `measureHint()` compares against the
    // announced context, so a caller that hands in a different width than it announced
    // would otherwise store a width nothing ever matches.
    if (cell.w > 0.0f)
        hintWidth_ = cell.w;
    const ui::Metrics m = ui::metricsFor();
    model::HeightHint& hint = thread_->hints[entry.view.id];
    const float drawnH = std::max(m.row, ImGui::GetCursorScreenPos().y - groupTop_);
    // The live answer may grow while it streams and must never *shrink* within one revision
    // (design FR-08: "the group height may grow but never shrink within a revision"). Two
    // frames of the same text can still measure differently - a wrap boundary a hair's
    // breadth away, rich_md re-laying out the last fragment - and a hint that followed them
    // down would make the follower jump back and forth. A taller
    // measurement always wins; the next revision is free to be shorter.
    //
    // "The same revision" includes the **box**: a hint is a statement about the width, the line
    // height and the mode it was measured in (D-METRICS), so a resize starts from scratch -
    // keeping a 100-cell height for a 40-cell box would be the very drift `measureHint()`'s read
    // side refuses (and `hint.width`/`hint.row` below would then describe the new box while `h`
    // described the old one).
    const bool sameBox = hint.groupId == entry.view.id && hint.revision == entry.view.revision &&
                         hint.width == hintWidth_ && hint.row == m.row &&
                         hint.compact == hintCompact_;
    hint.groupId = entry.view.id;
    hint.revision = entry.view.revision;
    hint.width = hintWidth_;
    hint.row = m.row;
    hint.compact = hintCompact_;
    hint.h = sameBox ? std::max(hint.h, drawnH) : drawnH;
}

void ThreadStoreAdapter::commitSeen() {
    // The typed fold has no defaults to commit: every turn is born folded and
    // the user's toggles are their own writes (`setBlockExpanded()` owns the
    // set), so there is no watermark to advance and nothing the render path
    // could re-seed over. The call stays: the view's once-per-frame shape
    // does not change with what the fold happens to store.
}

void ThreadStoreAdapter::reset() {
    groups_.clear();
    indexById_.clear();
    // The hint cache is view state and lives in `UiState`; a reset comes from a
    // clear or a session switch, and `resetForSessionSwitch()` is called from the
    // same place - but clearing here too keeps "ids are reset" and "hints are reset"
    // one call, so an adapter reset in a harness cannot leave a stale height behind.
    if (bound())
        thread_->hints.clear();
    hintWidth_ = 0.0f;
    hintCompact_ = false;
    expandedGroup_ = 0;
    groupOpen_ = false;
    groupExpandedBefore_ = false;
    groupOpenBefore_ = 0;
    groupTop_ = 0.0f;
    // The pins are keyed by the persisted turn id, and a reset comes from
    // a clear or a session switch: the thread's turns are gone, so a pin
    // left behind would be applied to the *next* session's turn with the
    // same id (ids restart at 1). The row pins and the group pins go with the thread.
    if (bound()) {
        chat_->rowPins.clear();
        chat_->groupPins.clear();
    }
    // The table source flags are the same kind of state - the next session's
    // block can be the one the flag would name - so they go with them.
    if (bound()) {
        thread_->tableSource.clear();
        // The block menu's owner too: the ids restart below, so a latch that
        // survived would name a block of the *next* thread.
        thread_->menuBlockId = {};
    }
    typedRevision_.clear();
    typedFingerprints_.clear();
    typedSynced_ = false;
    syncedTypedVersion_ = 0;
}

} // namespace llmfun::tui::store
