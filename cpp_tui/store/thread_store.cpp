/// The typed thread adapter (thread_store.h owns the contract).
///
/// This file is the projecting half that task 18 moved out of `tui_chat.cpp`: the
/// group build and the block model. It draws no thread of its
/// own - task 41 removed the renderer that did. A view asks `beginGroup()`/
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

// The typed-backed rebuild (v5, task 2): one group per stored turn, one row per
// displayed block, through the same Entry/BlockEntry machinery the v4 body
// fills - the GroupView/BlockDesc consumers cannot tell the paths apart. The
// group id IS the persisted turnId (design §4: overrides, expansion flags and
// hints survive upserts, re-emission and rebuilds because nothing is
// renumbered), and the rebuild guard is the store's mutation counter, so an
// unchanged store rebuilds nothing (§4.4).
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
            typedRevision_.emplace(turn.id, 0); // born at 0, like every non-stream v4 group
        else if (known->second != fingerprint)
            typedRevision_[turn.id] += 1;
        typedFingerprints_[turn.id] = fingerprint;

        Entry entry;
        entry.view.id = id;
        entry.view.kind = GroupKind::AssistantWork;
        entry.view.revision = typedRevision_[turn.id];
        entry.view.typedTurn = true;
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
        std::string firstProseId;
        bool sawProseMeta = false;
        for (std::size_t b = 0; b < blockN; ++b) {
            const store::TypedBlock& block = typed.blockAt(turn.id, b);
            if (block.kind != store::kBlockKindProse)
                continue;
            if (firstProseId.empty())
                firstProseId = block.id;
            if (block.meta == "user") {
                queryId = block.id;
                break; // the first user-tagged prose wins
            }
            sawProseMeta = sawProseMeta || !block.meta.empty();
        }
        if (queryId.empty() && !sawProseMeta)
            queryId = firstProseId; // the pre-meta fallback
        for (std::size_t b = 0; b < blockN; ++b) {
            const store::TypedBlock& block = typed.blockAt(turn.id, b);
            if (!blockDisplayable(block))
                continue;
            BlockEntry row;
            row.timingS =
                block.durationMs != 0 ? static_cast<double>(block.durationMs) / 1000.0 : 0.0;
            row.expandable = true;
            // Pre-cutover keying: the row's `key` is the
            // block id's hash - stable per block id, but not injective.
            row.key = std::hash<std::string>{}(block.id);
            row.typedTurnId = turn.id;
            row.typedBlockId = block.id;
            row.typedBatchId = block.batchId;
            row.typedMeta = block.meta;
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
                row.tokensIn = block.tokensIn; // the card's token fields (F5)
                row.tokensOut = block.tokensOut;
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
            // The row's state comes straight off the wire status (D7 is the
            // emitter's: the store merges, it never enforces a transition).
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
                entry.blocks.push_back(std::move(row));
            }
        }
        // The batch's thinking meta (F5): the disclosure names its response's
        // tool count - "Thinking · N tool calls" - counted over the Tool rows
        // that share its batch id. A tool-less batch leaves the meta 0, and
        // the disclosure carries no count.
        for (BlockEntry& row : entry.blocks) {
            if (row.kind != model::BlockKind::Thinking)
                continue;
            for (const BlockEntry& other : entry.blocks)
                if (other.kind == model::BlockKind::ToolStep &&
                    other.typedBatchId == row.typedBatchId)
                    ++row.batchTools;
        }
        // A rough estimate only: one line per row the group draws. The hint is
        // replaced by the drawn height after the first draw, exactly like v4's.
        entry.view.lineCountEstimate = std::max(1, static_cast<int>(entry.blocks.size()));
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
    // group id -> its index: the lookup `entryFor()` needs, a probe instead of a
    // walk of the thread (design §4.4).
    indexById_.clear();
    for (std::size_t i = 0; i < groups_.size(); ++i)
        indexById_.emplace(groups_[i].view.id, i);
    ++groupBuilds_;

    // The hint prune the v4 sync does: a hint for a turn that is gone is memory
    // kept for a group that no longer exists, and ids are never reused so
    // dropping it cannot lose a live hint.
    auto& hints = thread_->hints;
    for (auto it = hints.begin(); it != hints.end();)
        it = indexById_.count(it->first) != 0 ? std::next(it) : hints.erase(it);
    // window counts the stored groups and the view appends the live one after
    // (D-BOUNDS, the same shape the v4 projection gave the stream group) - and
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

bool ThreadStoreAdapter::groupMinimal(size_t groupId) const {
    if (!bound())
        return true;
    // The override first (design §6, task 8): a pinned group draws against the
    // mode - a maximal batch the user folded stays folded, a minimal batch the
    // user opened draws maximal-style - and the unpinned groups follow the mode.
    const auto pinned = chat_->groupOverrides.find(static_cast<long long>(groupId));
    if (pinned != chat_->groupOverrides.end())
        return pinned->second == store::GroupOverride::ForceClosed;
    return chat_->threadMinimalMode;
}

bool ThreadStoreAdapter::expanded(size_t id) const {
    const Entry* entry = entryFor(id);
    if (entry == nullptr || !bound())
        return false;
    // The per-group override first (task 8): the same answer the group's own
    // header row draws, so the endGroup latch cannot miss a toggle the draw
    // performed.
    const auto pinned = chat_->groupOverrides.find(static_cast<long long>(id));
    if (pinned != chat_->groupOverrides.end())
        return pinned->second == GroupOverride::ForceOpen;
    // The typed live turn is a work group like any other - its fold is the
    // minimal mode's chips line, never a body.
    return chat_->openWorkGroups.count(id) != 0;
}

void ThreadStoreAdapter::setExpanded(size_t id, bool open) {
    sync();
    if (!bound())
        return;
    const auto found = indexById_.find(id);
    if (found == indexById_.end())
        return;
    // The group-level seam writes the same state the block seam writes (task
    // 8): the per-group override in maximal - a fold a maximal batch cannot
    // express through the mode, which draws a batch open on its own (F5) - and
    // `openWorkGroups` in minimal (task 6's mechanism, unchanged).
    if (chat_->threadMinimalMode) {
        chat_->groupOverrides.erase(static_cast<long long>(id));
        if (open)
            chat_->openWorkGroups.insert(id);
        else
            chat_->openWorkGroups.erase(id);
        return;
    }
    const long long key = static_cast<long long>(id);
    if (open)
        chat_->groupOverrides.erase(key);
    else
        chat_->groupOverrides[key] = GroupOverride::ForceClosed;
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
    out.batchTools = block->batchTools;
    out.tokensIn = block->tokensIn;
    out.tokensOut = block->tokensOut;
    out.expandable = block->expandable;
    out.expanded = blockExpanded(k, b);

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
        out.kind = model::BlockKind::Code; // classified at build, like v4
    // Every row previews something: a tool card its tool name (set in the
    // switch), every other row its body's first line.
    if (block->kind != model::BlockKind::ToolStep)
        out.summary = oneLine(block->typedBody);
    out.hasBody = !block->typedBody.empty();
    return out;
}

bool ThreadStoreAdapter::blockExpanded(size_t k, std::size_t b) const {
    const BlockEntry* block = blockEntryAt(k, b);
    if (block == nullptr || !bound())
        return false;
    const Entry& entry = groups_[k];
    if (block->group) {
        // The per-group override first (design §6, task 8): a pinned group
        // draws against the mode - a maximal batch the user folded stays
        // folded, a minimal batch the user opened (its pin lives in
        // `openWorkGroups`, task 6) draws maximal-style.
        const auto pinned = chat_->groupOverrides.find(static_cast<long long>(entry.view.id));
        if (pinned != chat_->groupOverrides.end())
            return pinned->second == GroupOverride::ForceOpen;
        return minimalMode() ? chat_->openWorkGroups.count(entry.view.id) != 0
                             : true; // F5: a maximal batch draws open, always
    }
    // An open group renders maximal-style (design §6): every row it owns
    // draws open, and the per-row set only matters while the group is folded.
    if (chat_->openWorkGroups.count(entry.view.id) != 0)
        return true;
    const auto it = typedRowOpen_.find(static_cast<std::int64_t>(entry.view.id));
    const bool pinned = it != typedRowOpen_.end() && it->second.count(block->typedBlockId) != 0;
    // The row's default (F5): maximal mode draws every content row open -
    // the query, the answer, the tool cards - with only the batch's thinking
    // disclosure collapsed behind its meta. Minimal mode folds the work rows
    // behind the group header instead. The set pins AGAINST the default: a
    // click stores the opposite of what the row would draw on its own.
    const bool defaultOpen =
        !groupMinimal(entry.view.id) && block->kind != model::BlockKind::Thinking;
    return pinned ? !defaultOpen : defaultOpen;
}

bool ThreadStoreAdapter::setBlockExpanded(size_t k, std::size_t b, bool open) {
    const BlockEntry* block = blockEntryAt(k, b);
    if (block == nullptr || !bound() || !block->expandable)
        return false;
    const Entry& entry = groups_[k];
    if (block->group) {
        // The per-group override (design §6, task 8): the mode's own default
        // needs no entry - maximal draws a batch open on its own (F5) and
        // minimal keeps its open groups in `openWorkGroups` (task 6's
        // mechanism, unchanged) - so the map holds only the pin a click
        // cannot express through the mode: a maximal batch the user folded.
        // A click back to the mode's default removes the entry, so the map
        // never grows a stale row.
        if (chat_->threadMinimalMode) {
            chat_->groupOverrides.erase(static_cast<long long>(entry.view.id));
            if (open)
                chat_->openWorkGroups.insert(entry.view.id);
            else
                chat_->openWorkGroups.erase(entry.view.id);
        } else {
            const long long key = static_cast<long long>(entry.view.id);
            if (open)
                chat_->groupOverrides.erase(key);
            else
                chat_->groupOverrides[key] = GroupOverride::ForceClosed;
        }
    } else {
        // The per-row pin (F5): the row's own default - maximal draws every
        // content row open and the thinking disclosure collapsed - is what a
        // click stores against, so a set that agrees with the default is
        // erased rather than carried.
        const bool defaultOpen =
            !groupMinimal(entry.view.id) && block->kind != model::BlockKind::Thinking;
        std::set<std::string>& target = typedRowOpen_[entry.view.id];
        if (open != defaultOpen)
            target.insert(block->typedBlockId);
        else
            target.erase(block->typedBlockId);
    }
    expandedGroup_ = entry.view.id;
    return open;
}

std::string ThreadStoreAdapter::blockText(size_t k, std::size_t b) const {
    const BlockEntry* block = blockEntryAt(k, b);
    if (block == nullptr || !bound() || block->kind == model::BlockKind::WorkGroup)
        return {};
    return block->typedBody;
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
    // A card's Result can exist without arguments (F5): an empty body does not
    // end the draw - only a body-less, result-less row paints nothing.
    const bool hasResult = block->kind == model::BlockKind::ToolStep && !block->typedMeta.empty();
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
    if (block->kind == model::BlockKind::ToolStep && !block->typedMeta.empty()) {
        // The embedded Result (F5): the merged card carries the call's outcome
        // under its arguments, dim, open by default - a body row draws only
        // when the card is open, so closing the card closes the result with it.
        // The status is the header's glyph, never repeated here.
        ImGui::PushStyleColor(ImGuiCol_Text, ui::color(ui::ColorToken::textMid));
        renderBodyText("RESULT  " + oneLine(block->typedMeta), false, false);
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
    // A row inside an agent group toggles `typedRowOpen_` per block, which
    // `expanded(groupId)` cannot see (it reads the group's own bit). Any toggle of the
    // shared sets changes the membership of exactly one of them by one, so their
    // combined size is the O(1) signal that covers them. (A thinking disclosure lives
    // in `UiState`, so that one is the caller's own report.)
    //
    // This is only a toggle signal while the draw does not write the sets itself: the
    // one-shot defaults are seeded in `sync()` at build time, so the writes between
    // these two samples are the user's own activations.
    std::size_t rowOpen = 0;
    for (const auto& [id, rows] : typedRowOpen_)
        rowOpen += rows.size();
    groupOpenBefore_ = rowOpen + chat_->openWorkGroups.size();
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
    std::size_t rowOpen = 0;
    for (const auto& [id, rows] : typedRowOpen_)
        rowOpen += rows.size();
    const std::size_t openAfter = rowOpen + chat_->openWorkGroups.size();
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
    // The thinking disclosures are thread-scoped like the hints: a clear or a session
    // switch replaces the thread, and a disclosure left open would be applied to the
    // next session's block with the same identity (ids restart at 1). The table source
    // flags (task 37) are the same kind of state - the next session's block can be
    // the one the flag would name - so they go with them.
    if (bound()) {
        thread_->thinkingOpen.clear();
        thread_->tableSource.clear();
        // The block menu's owner too (task 44): the ids restart below, so a latch that
        // survived would name a block of the *next* thread.
        thread_->menuBlockId = {};
    }
    typedRevision_.clear();
    typedFingerprints_.clear();
    typedRowOpen_.clear();
    typedSynced_ = false;
    syncedTypedVersion_ = 0;
}

} // namespace llmfun::tui::store
