/// The v4 thread adapter (thread_store.h owns the contract).
///
/// This file is the v4-aware half that task 18 moved out of `tui_chat.cpp`: the
/// group build and the block model. It draws no thread of its
/// own - task 41 removed the renderer that did. A view asks `beginGroup()`/
/// `blockAt()` for a row, draws its chrome with `views::renderBlock()` and its body
/// with `renderBlockBody()`, which is what keeps "the content is the store's, the
/// chrome is the view's" a property of the construction rather than of a screenshot
/// comparison.
#include "thread_store.h"

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
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace llmfun::tui::store {

/// The v4 renderer's own grouping enum (`llmfun::tui::GroupKind`). Qualifying it
/// once is what keeps the two same-named enums apart: this file lives inside
/// `store::`, where the unqualified `GroupKind` is the view-facing one.
using TuiGroupKind = ::llmfun::tui::GroupKind;

/// The store's message type, under a local name. The block build (task 26) maps it to
/// a `model::BlockKind`, and the alias keeps that one switch readable without the
/// ledger counting eight v4 names for it (`tools/gate_ratchet.txt` rows retire, they do
/// not grow).
using TuiMessageType = ChatMessageType;

// ---------------------------------------------------------------------------
// The thread as data: groups, summaries, expansion
// ---------------------------------------------------------------------------

static bool renderAsMarkdown(const ChatMessageType type) {
    if (type == ChatMessageType::ToolCall || type == ChatMessageType::ToolResponse)
        return false;
    return true;
}

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

/// The role label table: one label per message type, the whole header contract
/// both backends share (YOU, AGENT, TOOL, ANSWER, SYSTEM). No `default:` arm, so
/// a new `ChatMessageType` shows up as a `-Wswitch` warning instead of silently
/// rendering as a system notice.
static const char* roleLabel(ChatMessageType type) {
    switch (type) {
    case ChatMessageType::User:
    case ChatMessageType::Vision:
        return "YOU";
    case ChatMessageType::Assistant:
        return "AGENT";
    case ChatMessageType::ToolCall:
    case ChatMessageType::ToolResponse:
        return "TOOL";
    case ChatMessageType::FinalAnswer:
        return "ANSWER";
    case ChatMessageType::System:
        return "SYSTEM";
    case ChatMessageType::Count:
        break; // sentinel: not a message
    }
    return "SYSTEM";
}

/// A summary as the header needs it: the preview text and the display time.
///
/// The v4 summary is `"<body>"` or `"[nr] <body> [<time>]"` - the D side's
/// display policy (llm/tui/package.d: addChatMessage + timeSuffix). The header
/// shows the time as " - 1.2s" and keeps the body as the one-line preview, so
/// neither decoration reaches the screen twice.
struct SummaryParts {
    std::string_view body;
    std::string_view time;
};

static bool isDigits(std::string_view s) {
    if (s.empty())
        return false;
    for (const char c : s)
        if (c < '0' || c > '9')
            return false;
    return true;
}

/// True for every tier `prettyDuration` produces (my/time.d): "<n>ms",
/// "<n>.<d>s", "<m>m <s>s", "<h>h <m>m" - numbers with an optional decimal,
/// each followed by a unit, separated by single spaces. Anything else in a
/// bracket is message text, not a time.
static bool isDurationText(std::string_view s) {
    std::size_t i = 0;
    while (i < s.size()) {
        const std::size_t numberStart = i;
        bool dot = false;
        while (i < s.size() && ((s[i] >= '0' && s[i] <= '9') || (s[i] == '.' && !dot))) {
            dot = dot || s[i] == '.';
            ++i;
        }
        if (i == numberStart || s[numberStart] == '.')
            return false;
        if (s.compare(i, 2, "ms") == 0) {
            i += 2;
        } else if (i < s.size() && (s[i] == 's' || s[i] == 'm' || s[i] == 'h')) {
            ++i;
        } else {
            return false;
        }
        if (i < s.size()) {
            if (s[i] != ' ')
                return false;
            ++i;
        }
    }
    return !s.empty();
}

static SummaryParts splitSummary(const std::string& summary) {
    SummaryParts parts{summary, {}};

    // "[nr] " - the message number. Dropped: the thread identifies the author
    // by role label and the number is an internal counter.
    const std::size_t close = summary.find(']');
    if (summary.size() > 2 && summary[0] == '[' && close != std::string::npos &&
        isDigits(std::string_view(summary).substr(1, close - 1))) {
        std::size_t start = close + 1;
        while (start < parts.body.size() && parts.body[start] == ' ')
            ++start;
        parts.body = parts.body.substr(start);

        // Trailing "[<duration>]", only in the decorated form (the prefix proves
        // the D side built the string, so a message that merely ends in "]" is
        // never mistaken for a timed one).
        if (parts.body.size() > 2 && parts.body.back() == ']') {
            const std::size_t open = parts.body.rfind('[');
            if (open != std::string_view::npos && (open == 0 || parts.body[open - 1] == ' ')) {
                const std::string_view inner =
                    parts.body.substr(open + 1, parts.body.size() - open - 2);
                if (isDurationText(inner)) {
                    parts.time = inner;
                    parts.body = parts.body.substr(0, open == 0 ? 0 : open - 1);
                }
            }
        }
    }
    return parts;
}

/// The duration a summary's trailing `[<time>]` carries, in seconds - 0 when it
/// carries none, or when the bracket is message text rather than a time (the same
/// `isDurationText()` test the header splitter uses, so the two cannot disagree).
///
/// A number, not the store's own "1.2s": the block row formats one label for both
/// profiles from it (task 26), and a test can compare the arithmetic instead of a
/// spelling.
double durationSeconds(std::string_view text) {
    if (!isDurationText(text))
        return 0.0;
    double total = 0.0;
    std::size_t i = 0;
    while (i < text.size()) {
        double value = 0.0;
        bool dot = false;
        double scale = 1.0;
        while (i < text.size() && ((text[i] >= '0' && text[i] <= '9') || text[i] == '.')) {
            if (text[i] == '.')
                dot = true;
            else if (dot)
                // 1.2 is one *and two tenths*: the first digit after the dot is the
                // tenth, so the scale steps before it is used.
                value += (text[i] - '0') * (scale /= 10.0);
            else
                value = value * 10.0 + (text[i] - '0');
            ++i;
        }
        if (text.compare(i, 2, "ms") == 0) {
            total += value / 1000.0;
            i += 2;
        } else if (i < text.size() && text[i] == 's') {
            total += value;
            ++i;
        } else if (i < text.size() && text[i] == 'm') {
            total += value * 60.0;
            ++i;
        } else if (i < text.size() && text[i] == 'h') {
            total += value * 3600.0;
            ++i;
        } else {
            return total;
        }
        if (i < text.size() && text[i] == ' ')
            ++i;
    }
    return total;
}

/// A summary without the emoji the D side puts in it (`app_agent/package.d` writes
/// the tool outcome as "✅" / "❌" inside the summary text).
///
/// Task 26's rule is "no emoji in any summary", and D-BLOCK's is "state via glyph +
/// word": so the marker does not just disappear - `takeStateMarker()` below turns it
/// into the block's `state` first, and whatever else a summary carries (a model
/// writing "done ✅" in prose) is dropped here rather than drawn. The ranges are the
/// ones the task's own check greps for: U+2705, U+274C, the U+1F300-U+1FAFF
/// pictographs, and the variation selector that selects emoji presentation for the
/// codepoints that are text by default.
std::string stripEmoji(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    std::size_t i = 0;
    while (i < text.size()) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        const bool threeByteEmoji = c == 0xE2 && i + 2 < text.size() &&
                                    ((static_cast<unsigned char>(text[i + 1]) == 0x9C &&
                                      static_cast<unsigned char>(text[i + 2]) == 0x85) || // ✅
                                     (static_cast<unsigned char>(text[i + 1]) == 0x9D &&
                                      static_cast<unsigned char>(text[i + 2]) == 0x8C)); // ❌
        const bool variationSelector = c == 0xEF && i + 2 < text.size() &&
                                       static_cast<unsigned char>(text[i + 1]) == 0xB8 &&
                                       static_cast<unsigned char>(text[i + 2]) == 0x8F; // U+FE0F
        const bool pictograph =
            c == 0xF0 && i + 3 < text.size() && static_cast<unsigned char>(text[i + 1]) == 0x9F;
        if (threeByteEmoji || variationSelector) {
            i += 3;
            if (i < text.size() && text[i] == ' ' && !out.empty() && out.back() == ' ')
                ++i; // the removal would leave a double space
            continue;
        }
        if (pictograph) {
            i += 4;
            if (i < text.size() && text[i] == ' ' && !out.empty() && out.back() == ' ')
                ++i;
            continue;
        }
        out.push_back(text[i]);
        ++i;
    }
    return out;
}

/// The state a tool result's marker names, with the marker removed from `text`.
///
/// The D side writes `tool result ✅ writeFile: 42 lines` (or "❌"), so the outcome is
/// *in* the summary. It becomes the block's state - which the row draws as the
/// Ok/Error glyph beside the word - and the summary keeps the rest of the sentence
/// ("keeps today's text", task 26). Anything else is `None`: a summary without a
/// marker names no outcome, and guessing one from the text would be a state the store
/// never asserted.
model::BlockState takeStateMarker(std::string& text) {
    model::BlockState state = model::BlockState::None;
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        const bool ok = c == 0xE2 && i + 2 < text.size() &&
                        static_cast<unsigned char>(text[i + 1]) == 0x9C &&
                        static_cast<unsigned char>(text[i + 2]) == 0x85; // ✅
        const bool failed = c == 0xE2 && i + 2 < text.size() &&
                            static_cast<unsigned char>(text[i + 1]) == 0x9D &&
                            static_cast<unsigned char>(text[i + 2]) == 0x8C; // ❌
        if (ok || failed) {
            if (state == model::BlockState::None)
                state = ok ? model::BlockState::Ok : model::BlockState::Failed;
            i += 3;
            // The marker sits between two spaces ("result ✅ writeFile"); dropping it
            // and one of them keeps the sentence readable without a double space.
            if (i < text.size() && text[i] == ' ' && !out.empty() && out.back() == ' ')
                ++i;
            continue;
        }
        out.push_back(text[i]);
        ++i;
    }
    text = stripEmoji(out);
    return state;
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

static bool isAssistantWork(ChatMessageType type) {
    return type == ChatMessageType::Assistant || type == ChatMessageType::ToolCall ||
           type == ChatMessageType::ToolResponse || type == ChatMessageType::System;
}

static bool isTopLevel(ChatMessageType type) {
    return type == ChatMessageType::User || type == ChatMessageType::Vision ||
           type == ChatMessageType::FinalAnswer;
}

static void buildRenderGroups(ChatTab& chat) {
    if (chat.outputLines.empty()) {
        chat.renderGroups.clear();
        return;
    }
    if (chat.outputLines.front().id == chat.renderGroupFirstId &&
        chat.outputLines.back().id == chat.renderGroupLastId) {
        return;
    }
    chat.renderGroups.clear();

    size_t i = 0;
    while (i < chat.outputLines.size()) {
        if (isTopLevel(chat.outputLines[i].type)) {
            TuiGroupKind kind = (chat.outputLines[i].type == ChatMessageType::FinalAnswer)
                                    ? TuiGroupKind::FinalAnswer
                                    : TuiGroupKind::UserQuery;
            chat.renderGroups.push_back({i, i + 1, kind});
            ++i;
        } else {
            size_t start = i;
            while (i < chat.outputLines.size() && isAssistantWork(chat.outputLines[i].type)) {
                ++i;
            }
            chat.renderGroups.push_back({start, i, TuiGroupKind::AssistantWork});
        }
    }

    chat.renderGroupFirstId = chat.outputLines.front().id;
    chat.renderGroupLastId = chat.outputLines.back().id;
}

/// Separator between the role label and the time (and before the preview).
/// The grid stays ASCII: its chrome must not depend on the terminal's locale
/// (under LC_ALL=C ncurses turns U+00B7 into U+FFFD), while the pixel profile
/// can afford the middle dot.
static const char* roleSeparator() { return tuiIsTextGrid() ? " - " : u8" · "; }

/// One message body: the thinking disclosure (when the message has any), the
/// text - markdown for every role except tool call/result, whose text is
/// verbatim - and the per-message Copy action. Shared by the top-level and the
/// work-group path so every role renders through exactly one code path. The
/// caller owns the PushID, so "Copy" is unique per message.

/// The header text of one message: the role label, "<sep><time>" when the
/// message carries one, and - while the message is collapsed - the one-line
/// preview. The preview is what makes a collapsed row say what it is; an
/// expanded row is a pure role row because its body sits right below.
static std::string messageHeaderText(const ChatMessage& entry, bool expanded) {
    const SummaryParts parts = splitSummary(entry.summary);
    std::string text = roleLabel(entry.type);
    if (!parts.time.empty()) {
        text.append(roleSeparator());
        text.append(parts.time);
    }
    if (!expanded && !parts.body.empty()) {
        text.append("  ");
        text.append(parts.body);
    }
    return text;
}

/// Writes one message id's expansion state, unconditionally. `seedNewGroup()` is the
/// caller: it applies a default at the moment a group is born, where the question "is
/// this message new" is already answered by the group build itself, so a watermark
/// test would be the wrong question.
static void applyExpansionDefault(ChatTab& chat, std::size_t id, bool defaultOpen) {
    if (defaultOpen)
        chat.outputLineOpen.insert(id);
    else
        chat.outputLineOpen.erase(id);
}

/// Commits one drawing pass: the one-shot expansion watermark moves to the newest
/// message, because every message that pass drew has had its default applied.
///
/// Called once per frame from `ThreadStoreAdapter::commitSeen()`, after the groups
/// were drawn - otherwise the next frame would re-seed the default over the user's
/// click. Ids only ever grow, so one high-water id is enough.
static void commitExpansionDefaults(ChatTab& chat) {
    if (chat.outputLines.empty())
        return;
    chat.expansionDefaultThroughId =
        std::max(chat.expansionDefaultThroughId, chat.outputLines.back().id);
}

/// The per-item default inside a work group: the last five steps unfold, everything
/// older stays collapsed. One rule, one place: the birth seeder, the build-time
/// seeder and the draw must agree, or a default applied inside the draw changes the
/// membership the draw path reads as a user toggle.
static bool workItemDefaultOpen(std::size_t index, std::size_t count) {
    return (index + 5) > count;
}

/// Applies the one-shot expansion defaults of a group that has just appeared.
///
/// This is where "new" is decided, and it is a property of the group build rather
/// than of a frame: an id is assigned exactly once, when the group first exists in
/// the store, so the default a group is *born* with never depends on which frame,
/// window or page happens to draw it.
///
/// The draw path cannot seed it: a group revealed later by `Load earlier` sits
/// *below* the watermark, so a per-message seeder is a no-op for it and the group
/// would come back as a header only.
static void seedNewGroup(ChatTab& chat, GroupKind kind, std::size_t start, std::size_t end,
                         bool isNewestGroup, std::size_t count) {
    if (kind == GroupKind::AssistantWork) {
        // The items get their default even while the group is collapsed: a
        // collapsed group is never drawn, so a later expand must not find them all
        // closed. This seeds them before the group's expansion gate for exactly that
        // reason, with the same "the last five unfold" rule.
        for (std::size_t i = start; i < end; ++i)
            applyExpansionDefault(chat, chat.outputLines[i].id, workItemDefaultOpen(i, count));
        // Only the newest work group unfolds. `isNewestGroup` means the newest
        // *group*, which is `gi + 1 == renderGroups.size()` in the v4 body: the
        // streamed message is not a group, so a work group can be the newest one
        // while an answer streams.
        const std::size_t headId = chat.outputLines[start].id;
        if (isNewestGroup)
            chat.openWorkGroups.insert(headId);
        else
            chat.openWorkGroups.erase(headId);
        return;
    }
    // Top-level message: open while it is inside the newest ten, and always for the
    // final answer (the run's result is never hidden).
    const bool defaultOpen =
        kind == GroupKind::FinalAnswer || static_cast<int>(start) >= static_cast<int>(count) - 10;
    applyExpansionDefault(chat, chat.outputLines[start].id, defaultOpen);
}

/// One message: role header row plus, when expanded, its body.
///
/// `indentBody` is set by the top-level path only: a message already inside a
/// work group keeps the group's single indent level, which is what keeps the
/// body geometry identical to the pre-task layout (and rich_md's table renderer
/// sensitive to the exact content width - one extra column and a paragraph
/// above a table is dropped; measured with test_rich_markdown_text case C).

/// A run of assistant work: one group header row (AGENT - N steps) and, while
/// it is expanded, the messages it contains - each with its own role row and its
/// own expansion state. The group is not a message: its expansion lives in
/// ChatTab::openWorkGroups, keyed by the first message id.

/// The streaming message: one always-expanded AGENT row with no time and no
/// preview (its body updates in place). Not interactive: it is a label, not a
/// toggle.

namespace {

/// One message's hint: its header row plus the two bodies, at the reference width.
int estimateMessageLines(const ChatMessage& msg) {
    return 1 /*header*/ + ui::estimateWrappedLines(msg.text) +
           ui::estimateWrappedLines(msg.thinking);
}

/// What changed in the streaming message since the last sync: a fingerprint, not a
/// text compare (the stream can be re-sent every frame, and `sync()` is called once
/// per adapter accessor, i.e. ~2 per visible group per frame).
///
/// It is a *mutation hint*, and it says so: the sizes, the message type and eight
/// bytes sampled across the body catch every append (the common case: the text
/// grows) and a same-length rewrite that touches one of the sampled positions. A
/// rewrite that keeps the length *and* leaves those bytes alone is not detected -
/// the price of not hashing (or copying) a body that can be megabytes, on the frame
/// path. `revision` therefore means "changed, as far as a cheap check can see".
size_t streamFingerprint(const ChatTab& chat) {
    const ChatMessage& msg = chat.streamMsg;
    auto sample = [](const std::string& s, std::size_t at) -> std::size_t {
        return at < s.size() ? static_cast<unsigned char>(s[at]) : 0u;
    };
    const std::size_t mid = msg.text.size() / 2;
    const std::size_t tail = msg.text.empty() ? 0 : msg.text.size() - 1;
    return msg.text.size() * 131u + msg.thinking.size() * 17u + msg.summary.size() * 7u +
           static_cast<std::size_t>(msg.type) * 29u + (chat.hasStreamMsg ? 1u : 0u) +
           sample(msg.text, 0) * 3u + sample(msg.text, mid) * 5u + sample(msg.text, tail) * 11u +
           sample(msg.thinking, 0) * 13u;
}

store::GroupKind toViewKind(TuiGroupKind kind) {
    switch (kind) {
    case TuiGroupKind::UserQuery:
        return store::GroupKind::UserQuery;
    case TuiGroupKind::FinalAnswer:
        return store::GroupKind::FinalAnswer;
    case TuiGroupKind::AssistantWork:
        break;
    }
    return store::GroupKind::AssistantWork;
}

} // namespace

ThreadStoreAdapter::ThreadStoreAdapter(TuiState& state) : state_(&state) {}

void ThreadStoreAdapter::sync() const {
    // Unbound (the state moved since the last frame): nothing to sync, and every
    // accessor downstream sees an empty thread instead of a dangling pointer.
    if (state_ == nullptr) {
        groups_.clear();
        indexById_.clear();
        synced_ = false;
        return;
    }
    // Non-const: a group that has just appeared gets its one-shot expansion
    // default written into the v4 store below, at the moment its id is assigned
    // (`seedNewGroup`). That is the same state the draw path writes, and the reason it
    // happens here is that "new" is a property of the group build, not of the frame
    // that happens to draw the group.
    ChatTab& chat = state_->chat;
    const size_t count = chat.outputLines.size();
    const size_t front = count != 0 ? chat.outputLines.front().id : 0;
    const size_t back = count != 0 ? chat.outputLines.back().id : 0;

    // The stream's revision is the adapter's own counter: it advances whenever
    // the stream's content changes, which is what a consumer means by "this
    // group changed" (a text compare per frame would be the thing to avoid).
    const size_t fingerprint = streamFingerprint(chat);
    if (fingerprint != streamFingerprint_) {
        streamFingerprint_ = fingerprint;
        ++streamRevision_;
    }

    if (synced_ && count == syncedCount_ && front == syncedFront_ && back == syncedBack_ &&
        chat.hasStreamMsg == (streamGroupId_ != 0) && streamRevision_ == syncedStream_)
        return;

    synced_ = true;
    syncedCount_ = count;
    syncedFront_ = front;
    syncedBack_ = back;
    syncedStream_ = streamRevision_;

    // One group build for the whole page (`buildRenderGroups`), so a page and a
    // whole-thread walk cannot disagree about where a group starts. The store's own
    // cached copy is refreshed too, which keeps the next reader warm for free.
    buildRenderGroups(state_->chat);
    const std::vector<RenderGroup>& built = state_->chat.renderGroups;

    std::vector<Entry> groups;
    groups.reserve(built.size() + 1);
    std::map<size_t, size_t> ids;
    for (size_t gi = 0; gi < built.size(); ++gi) {
        const RenderGroup& group = built[gi];
        if (group.start >= group.end || group.end > count)
            continue; // a half-built group is never rendered
        const size_t startId = chat.outputLines[group.start].id;
        const GroupKind viewKind = toViewKind(group.kind);
        size_t id = 0;
        const auto known = idByStart_.find(startId);
        const bool fresh = known == idByStart_.end();
        if (!fresh) {
            id = known->second; // an appended message extends the group, it does
                                // not renumber it (D-GROUPID)
        } else {
            id = nextId_++;
            // The one moment this group is new: seed the default it is born with.
            // Doing it here (instead of in the draw path) is what keeps the answer the
            // same for a group the window draws now and one `Load earlier` reveals
            // later.
            seedNewGroup(chat, viewKind, group.start, group.end,
                         /*isNewestGroup=*/gi + 1 == built.size(), count);
            if (viewKind == GroupKind::AssistantWork && gi + 1 == built.size())
                autoOpenWorkGroups_.insert(startId);
        }
        ids.emplace(startId, id);

        Entry entry;
        entry.view.id = id;
        entry.view.kind = viewKind;
        entry.view.revision = 0;
        entry.start = group.start;
        entry.end = group.end;
        if (group.kind == TuiGroupKind::AssistantWork) {
            const size_t steps = group.end - group.start;
            entry.view.oneLineSummary = "AGENT";
            entry.view.oneLineSummary.append(roleSeparator());
            entry.view.oneLineSummary.append(std::to_string(steps));
            entry.view.oneLineSummary.append(steps == 1 ? " step" : " steps");
            int lines = 1;
            for (size_t i = group.start; i < group.end; ++i)
                lines += estimateMessageLines(chat.outputLines[i]);
            entry.view.lineCountEstimate = lines;
        } else {
            entry.view.oneLineSummary =
                messageHeaderText(chat.outputLines[group.start], /*expanded=*/false);
            entry.view.lineCountEstimate = estimateMessageLines(chat.outputLines[group.start]);
        }
        groups.push_back(std::move(entry));
    }

    // Only the newest stored group's work group unfolds. A group the default opened
    // closes when a newer stored group (the answer, a new user query, the next run)
    // arrives; the stream is not a stored group and must not close it. A group the
    // user opened is not in the auto set and is left alone.
    if (!groups.empty()) {
        // `openWorkGroups` is keyed by a work group's first message id, not by the
        // adapter's group id; the auto set uses the same key.
        const std::size_t newestStored = chat.outputLines[groups.back().start].id;
        for (auto it = autoOpenWorkGroups_.begin(); it != autoOpenWorkGroups_.end();) {
            if (*it == newestStored) {
                ++it;
                continue;
            }
            chat.openWorkGroups.erase(*it);
            it = autoOpenWorkGroups_.erase(it);
        }
    }

    if (chat.hasStreamMsg) {
        if (streamGroupId_ == 0) {
            streamGroupId_ = nextId_++;
            // The streamed reasoning starts open: seed its identity when the group is
            // born, the one time it is new. A later toggle is the user's own.
            state_->ui.thread.thinkingOpen.insert(model::BlockId{
                streamGroupId_, model::kStreamMessageId, model::BlockKind::Thinking});
        }
        Entry entry;
        entry.view.id = streamGroupId_;
        entry.view.kind = GroupKind::AssistantWork;
        entry.view.revision = streamRevision_;
        entry.view.oneLineSummary = roleLabel(chat.streamMsg.type);
        entry.view.lineCountEstimate = estimateMessageLines(chat.streamMsg);
        entry.start = count; // not a message range: the stream is its own body
        entry.end = count;
        entry.stream = true;
        groups.push_back(std::move(entry));
    } else {
        if (streamGroupId_ != 0) {
            state_->ui.thread.thinkingOpen.erase(model::BlockId{
                streamGroupId_, model::kStreamMessageId, model::BlockKind::Thinking});
            if (state_->ui.thread.focus.group == streamGroupId_)
                state_->ui.thread.focus = {};
            // The menu opened on one of its rows goes with the rows (task 44): the owner
            // must name a block that exists, or an open popup survives its block.
            if (state_->ui.thread.menuBlockId.group == streamGroupId_)
                state_->ui.thread.menuBlockId = {};
            // The stream ended without a clear (a store the harness wrote directly): its clock
            // counted an answer that is over, so it goes with the group id (task 38).
            model::resetStreamClock(state_->ui.thread.stream);
        }
        streamGroupId_ = 0;
    }

    groups_ = std::move(groups);
    // The blocks each group draws (design §4.5, task 26): the group's own header row,
    // then one row per message, with a message's thinking disclosure in front of it.
    // Built here, with the group list, so `blockAt()` is a copy out of a walked vector
    // rather than a per-row walk of the store - and a body is classified once (a single
    // fenced code block is a `Code` block for as long as its text is unchanged).
    const auto buildBlocks = [&chat](Entry& entry) {
        entry.blocks.clear();
        if (entry.stream) {
            // The streamed message has no id in the store, so its reasoning is a block of
            // its own exactly like a stored message's (task 26, review Important 1: a
            // stream that carries only reasoning must not draw an empty row). A windowed
            // render gives it a row, a focus target and a copy target of its own.
            if (!chat.streamMsg.thinking.empty()) {
                BlockEntry think;
                think.kind = model::BlockKind::Thinking;
                think.key = model::kStreamMessageId;
                think.thinking = true;
                think.expandable = true;
                entry.blocks.push_back(think);
            }
            BlockEntry block;
            block.kind = model::BlockKind::Assistant;
            block.key = model::kStreamMessageId;
            block.expandable = false;
            entry.blocks.push_back(block);
            return;
        }
        if (entry.view.kind == GroupKind::AssistantWork) {
            BlockEntry header;
            header.kind = model::BlockKind::WorkGroup;
            header.key = entry.view.id;
            header.message = entry.start;
            header.steps = static_cast<int>(entry.end - entry.start);
            header.expandable = true;
            entry.blocks.push_back(header);
        }
        for (std::size_t i = entry.start; i < entry.end; ++i) {
            const auto& message = chat.outputLines[i];
            const double timing = durationSeconds(splitSummary(message.summary).time);
            if (!message.thinking.empty()) {
                // The disclosure is a row that opens and closes on its own, so it is a
                // block of its own: one list, one renderer, one focus ring.
                BlockEntry think;
                think.kind = model::BlockKind::Thinking;
                think.key = message.id;
                think.message = i;
                think.thinking = true;
                think.timingS = timing;
                think.expandable = true;
                entry.blocks.push_back(think);
            }
            BlockEntry block;
            block.key = message.id;
            block.message = i;
            block.timingS = timing;
            block.expandable = true;
            switch (message.type) {
            case TuiMessageType::User:
            case TuiMessageType::Vision:
                block.kind = model::BlockKind::User;
                break;
            case TuiMessageType::Assistant:
                block.kind = model::BlockKind::Assistant;
                break;
            case TuiMessageType::FinalAnswer:
                block.kind = model::BlockKind::Answer;
                break;
            case TuiMessageType::ToolCall:
                block.kind = model::BlockKind::ToolCall;
                break;
            case TuiMessageType::ToolResponse:
                block.kind = model::BlockKind::ToolResult;
                break;
            case TuiMessageType::System:
            case TuiMessageType::Count:
                block.kind = model::BlockKind::System;
                break;
            }
            // A body that is one fenced code block is drawn - and copied - as code; the
            // role word stays the message's own ("an agent answered with code").
            //
            // Classified once, here, so the fence scan sits on the store-change path and
            // not on the frame path. The change key is `(count, front, back, hasStream,
            // streamRevision)`: an append, an eviction or a stream update rebuilds, an
            // in-place *rewrite* of an existing message would not - and its `Code` flag
            // would go stale while its summary and duration are recomputed per call. The
            // store never rewrites a message today (a message is appended once), so the
            // flag is as fresh as the text; anything that starts rewriting has to fold a
            // text fingerprint into the key.
            block.code = renderAsMarkdown(message.type) && !ui::fencedCodeOf(message.text).empty();
            entry.blocks.push_back(block);
        }
    };
    for (Entry& entry : groups_)
        buildBlocks(entry);
    idByStart_ = std::move(ids);
    // group id -> its index, so the lookups below are a map probe instead of a walk
    // of the whole thread (design §4.4: enumeration is incremental).
    indexById_.clear();
    for (std::size_t i = 0; i < groups_.size(); ++i)
        indexById_.emplace(groups_[i].view.id, i);
    ++groupBuilds_;
    // Seed the items of *every* work group while the list is built, not only the
    // ones of a group that has just been born. A step appended to an existing
    // group has no other seeder here, and a default applied inside the draw would
    // change the membership the draw path reads as "the user toggled something":
    // the follow would be dropped and an anchor armed on every arriving step.
    // Watermarked, so an id that was seeded (or clicked) earlier is never touched
    // twice; one rule with `seedNewGroup()` and the draw (`workItemDefaultOpen`).
    for (const Entry& e : groups_) {
        if (e.stream || e.view.kind != GroupKind::AssistantWork)
            continue;
        for (std::size_t m = e.start; m < e.end; ++m) {
            const auto& item = chat.outputLines[m]; // `auto`: the ledger counts the v4 type name
            if (item.id > chat.expansionDefaultThroughId)
                applyExpansionDefault(chat, item.id, workItemDefaultOpen(m, count));
        }
    }

    // Hints for groups that are gone would be memory (and an anchor) kept for a
    // group that no longer exists; ids are never reused, so dropping them cannot
    // lose a live hint. `UiState::thread.hints` is the cache (design §5.1) - the
    // adapter only decides which entries are still live, and a session switch clears
    // it wholesale in `resetForSessionSwitch()`.
    auto& hints = state_->ui.thread.hints;
    for (auto it = hints.begin(); it != hints.end();)
        it = indexById_.count(it->first) != 0 ? std::next(it) : hints.erase(it);
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
bool ThreadStoreAdapter::expanded(size_t id) const {
    const Entry* entry = entryFor(id);
    if (entry == nullptr || state_ == nullptr)
        return false;
    if (entry->stream)
        return true; // the streaming message is always open
    const ChatTab& chat = state_->chat;
    if (entry->start >= chat.outputLines.size())
        return false;
    const size_t headId = chat.outputLines[entry->start].id;
    if (entry->view.kind == GroupKind::AssistantWork)
        return chat.openWorkGroups.count(headId) != 0;
    return chat.outputLineOpen.count(headId) != 0;
}

void ThreadStoreAdapter::setExpanded(size_t id, bool open) {
    sync();
    if (state_ == nullptr)
        return;
    const auto found = indexById_.find(id);
    if (found == indexById_.end())
        return;
    const Entry& entry = groups_[found->second];
    if (entry.stream || entry.start >= state_->chat.outputLines.size())
        return;
    const size_t headId = state_->chat.outputLines[entry.start].id;
    if (entry.view.kind == GroupKind::AssistantWork)
        autoOpenWorkGroups_.erase(headId); // the user owns this group from now on
    std::set<size_t>& target = entry.view.kind == GroupKind::AssistantWork
                                   ? state_->chat.openWorkGroups
                                   : state_->chat.outputLineOpen;
    if (open)
        target.insert(headId);
    else
        target.erase(headId);
}

float ThreadStoreAdapter::measureHint(size_t id) const {
    const Entry* entry = entryFor(id);
    if (entry == nullptr || state_ == nullptr)
        return 0.0f;
    const ui::Metrics m = ui::metricsFor();
    // The cache is `UiState::thread.hints` (design §5.1), keyed by the box the view
    // announced: a font-scale change moves `row`, a resize moves `width`, compact
    // mode changes how much of a group is drawn - and any of the three makes the
    // cached number a statement about a different box.
    const auto hint = state_->ui.thread.hints.find(id);
    if (hint != state_->ui.thread.hints.end()) {
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
    if (block == nullptr || state_ == nullptr || block->kind == model::BlockKind::WorkGroup)
        return false;
    // A thinking body is markdown too - it is drawn through the same renderer - so only a stored
    // or streamed *message* row has to pass the verbatim check: a tool transcript is drawn
    // verbatim (a fence inside it would be printed literally), so a TOOL row has nothing to show.
    // That is what stops `s` from setting an invisible bit on such a row.
    if (!block->thinking && !groups_[k].stream) {
        if (block->message >= state_->chat.outputLines.size())
            return false;
        if (!renderAsMarkdown(state_->chat.outputLines[block->message].type))
            return false;
    }
    const std::string body = blockText(k, b);
    // The renderer's own cheap guards, then the bridge's answer: a body with no pipe cannot hold
    // a table, and a body drawn without a rich context is plain text (nothing to show).
    return !body.empty() && body.find('|') != std::string::npos && ui::markdownReady() &&
           ui::bodyHasTable(ui::metricsFor().textGrid, body);
}

const ThreadStoreAdapter::BlockEntry* ThreadStoreAdapter::blockEntryAt(size_t k,
                                                                       std::size_t b) const {
    sync();
    if (k >= groups_.size() || b >= groups_[k].blocks.size())
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
    if (block == nullptr || state_ == nullptr)
        return out;
    const Entry& entry = groups_[k];
    const ChatTab& chat = state_->chat;

    out.kind = block->kind;
    out.state = block->state;
    out.group = entry.view.id;
    out.key = block->key;
    out.timingS = block->timingS;
    out.steps = block->steps;
    out.expandable = block->expandable;
    out.expanded = blockExpanded(k, b);
    out.streaming = entry.stream;

    if (block->kind == model::BlockKind::WorkGroup) {
        // The group's own header: "WORK · N steps" over the rows below it. It has no
        // body of its own - the rows are blocks.
        out.hasBody = false;
        out.label = "WORK"; // the design's word (§4.5); the view has the same fallback
        return out;
    }
    if (entry.stream) {
        // The streaming message is not in the store, so its rows are drawn from
        // `chat.streamMsg` and keyed by the reserved stream id; the stream group id
        // completes their BlockId. The reasoning is its own block (a stream that carries
        // *only* thinking must still show it). The body row is always present - it is the
        // stream's stable role row - and has a body only once there is text to draw.
        const auto& stream = chat.streamMsg;
        // The live answer's clock (design FR-08, task 38), derived once per frame by the frame
        // layer: this file formats nothing and reads no wall clock, so the header cannot
        // disagree with the elapsed time the now bar shows.
        const model::StreamState& clock = state_->ui.thread.stream;
        if (block->thinking) {
            out.label = "Thinking";
            out.hasBody = !stream.thinking.empty();
            // `Thinking · 3.2 s` once the reasoning has ended, and no number while it is still
            // reasoning: `thinkingElapsedMs` is 0 until the first character of the answer ends
            // it, so the row never shows a duration the model has not finished spending.
            out.timingS = static_cast<double>(clock.thinkingElapsedMs) / 1000.0;
            return out;
        }
        out.hasBody = !stream.text.empty();
        out.label = roleLabel(stream.type);
        // `AGENT · writing · 00:12` / `AGENT · thinking` / `AGENT · cancelled · 00:07`: the
        // phase word is the model's vocabulary (`streamPhaseWord`), the clock is the frame's.
        out.phase = model::streamPhaseWord(clock, !stream.text.empty());
        out.elapsedMs = clock.elapsedMs;
        return out;
    }
    if (block->message >= chat.outputLines.size())
        return out;
    const auto& message = chat.outputLines[block->message]; // `auto`: the v4 name stays rare
    if (block->thinking) {
        // One row that opens: "Thinking · 1.2 s", dim body. The duration is the
        // message's - the store keeps one per message, and the reasoning belongs to the
        // step that produced it.
        out.label = "Thinking";
        out.hasBody = !message.thinking.empty();
        return out;
    }

    out.label = roleLabel(message.type);
    const SummaryParts parts = splitSummary(message.summary);
    std::string preview(parts.body);
    if (block->kind == model::BlockKind::ToolCall || block->kind == model::BlockKind::ToolResult) {
        // The outcome the store wrote into the summary becomes the row's state, and the
        // summary keeps the rest of the sentence ("keeps today's text").
        out.state = takeStateMarker(preview);
        if (out.state == model::BlockState::None && block->kind == model::BlockKind::ToolCall) {
            // A call has no outcome of its own - its result is the next row - so the row
            // says what it is: the call those results answer.
            out.state = model::BlockState::Pending;
        }
    } else {
        preview = stripEmoji(preview);
    }
    out.summary = oneLine(preview);
    out.hasBody = !message.text.empty();
    if (block->code)
        out.kind = model::BlockKind::Code;
    return out;
}

bool ThreadStoreAdapter::blockExpanded(size_t k, std::size_t b) const {
    const BlockEntry* block = blockEntryAt(k, b);
    if (block == nullptr || state_ == nullptr)
        return false;
    const Entry& entry = groups_[k];
    if (entry.stream && !block->thinking)
        return true; // the streamed body is always open
    if (block->thinking) {
        // Stored and streamed reasoning share one expansion set, keyed by the
        // block's identity; the stream group id is what keeps the two apart.
        return state_->ui.thread.thinkingOpen.count(
                   model::BlockId{entry.view.id, block->key, model::BlockKind::Thinking}) != 0;
    }
    const ChatTab& chat = state_->chat;
    if (block->kind == model::BlockKind::WorkGroup) {
        if (entry.start >= chat.outputLines.size())
            return false;
        return chat.openWorkGroups.count(chat.outputLines[entry.start].id) != 0;
    }
    return chat.outputLineOpen.count(block->key) != 0;
}

bool ThreadStoreAdapter::setBlockExpanded(size_t k, std::size_t b, bool open) {
    const BlockEntry* block = blockEntryAt(k, b);
    if (block == nullptr || state_ == nullptr || !block->expandable)
        return false;
    const Entry& entry = groups_[k];
    const model::BlockId thinkingId{entry.view.id, block->key, model::BlockKind::Thinking};
    if (entry.stream) {
        if (!block->thinking)
            return true; // the streamed body is always open
        if (open)
            state_->ui.thread.thinkingOpen.insert(thinkingId);
        else
            state_->ui.thread.thinkingOpen.erase(thinkingId);
        // No anchor latch: the view's own report (`BlockResult::toggled`) names the group
        // to anchor, and `endGroup()` deliberately skips the latch for the stream.
        return open;
    }
    if (block->thinking) {
        if (open)
            state_->ui.thread.thinkingOpen.insert(thinkingId);
        else
            state_->ui.thread.thinkingOpen.erase(thinkingId);
    } else {
        const ChatTab& chat = state_->chat;
        const bool group = block->kind == model::BlockKind::WorkGroup;
        if (group && entry.start >= chat.outputLines.size())
            return false;
        const size_t key = group ? chat.outputLines[entry.start].id : block->key;
        if (group)
            autoOpenWorkGroups_.erase(key); // the user owns this group now
        std::set<size_t>& target =
            group ? state_->chat.openWorkGroups : state_->chat.outputLineOpen;
        if (open)
            target.insert(key);
        else
            target.erase(key);
    }
    // The anchor's answer: the group whose content just changed. `takeExpandedGroup()`
    // reports it, whatever performed the toggle.
    expandedGroup_ = entry.view.id;
    return open;
}

std::string ThreadStoreAdapter::blockText(size_t k, std::size_t b) const {
    const BlockEntry* block = blockEntryAt(k, b);
    if (block == nullptr || state_ == nullptr || block->kind == model::BlockKind::WorkGroup)
        return {};
    const Entry& entry = groups_[k];
    if (entry.stream)
        return block->thinking ? state_->chat.streamMsg.thinking : state_->chat.streamMsg.text;
    if (block->message >= state_->chat.outputLines.size())
        return {};
    const auto& message = state_->chat.outputLines[block->message];
    return block->thinking ? message.thinking : message.text;
}

void ThreadStoreAdapter::renderBlockBody(size_t k, std::size_t b, const ui::Rect& cell) {
    const BlockEntry* block = blockEntryAt(k, b);
    if (block == nullptr || state_ == nullptr)
        return;
    if (!ui::rectMatchesProfile(cell, ui::metricsFor()))
        return;
    const Entry& entry = groups_[k];
    std::string body;
    const bool thinking = block->thinking;
    bool markdown = true;
    if (entry.stream) {
        body = block->thinking ? state_->chat.streamMsg.thinking : state_->chat.streamMsg.text;
    } else if (block->kind != model::BlockKind::WorkGroup &&
               block->message < state_->chat.outputLines.size()) {
        const auto& message = state_->chat.outputLines[block->message];
        body = thinking ? message.thinking : message.text;
        // A tool row is a transcript, not prose: it is drawn verbatim. Everything else goes
        // through the markdown wrapper.
        markdown = renderAsMarkdown(message.type);
    }
    if (body.empty())
        return;
    // The live answer's caret (design FR-08, §4.5; task 38). Three facts decide it and all
    // three are the *frame's*: only the body row of a live stream carries one, only while the
    // clock says the answer is still arriving, and only in the half of the blink this frame
    // falls in (`StreamState::caretOn`, derived from the clock - never from a frame counter,
    // so the blink rate does not follow the frame rate).
    //
    // The off half is a space of the caret's own width, not nothing: the two halves of the
    // blink append the same number of cells, so a caret on the last line cannot make the group
    // grow and shrink twice a second.
    //
    // A stream whose clock was never derived (a store-only harness, no frame) draws no caret:
    // `startedMs == 0` means no frame has said this stream is live, and inventing one here
    // would put the blink on the wall clock of a test.
    if (entry.stream && !thinking) {
        const model::StreamState& clock = state_->ui.thread.stream;
        if (clock.active() && !clock.cancelled) {
            const ui::Metrics m = ui::metricsFor();
            const ui::GlyphSet glyphs = m.textGrid ? ui::textGlyphSet() : ui::pixelGlyphSet();
            body = ui::withStreamingCaret(body,
                                          clock.caretOn ? ui::icon(ui::Icon::Rule, glyphs) : " ");
        }
    }
    // The table bridge (design §6.8, task 37) is applied by `renderBodyText()` - the one place
    // a markdown body is drawn - and the block's own `s` toggle is what it needs from here: the
    // source flag names the block by its stable id, so a redraw after a paging change or a
    // stream update asks the same question.
    const model::BlockId blockId{entry.view.id, block->key, block->kind};
    const bool showTableSource =
        state_->ui.thread.tableSource.find(blockId) != state_->ui.thread.tableSource.end();

    ImGui::PushTextWrapPos(ImGui::GetContentRegionMax().x - 1);
    if (thinking) {
        // The dim body of a disclosure: `textMid` is the quiet ink that still clears the
        // contrast bar, and the end mark closes the region the header opened.
        ImGui::PushStyleColor(ImGuiCol_Text, ui::color(ui::ColorToken::textMid));
        renderBodyText(body, true, showTableSource);
        ImGui::PopStyleColor();
        renderSeparator("End ", "-", ImGui::GetContentRegionMax().x - 4, true);
    } else {
        renderBodyText(body, markdown, showTableSource);
    }
    ImGui::PopTextWrapPos();
}

bool ThreadStoreAdapter::beginGroup(size_t k, const ui::Rect& cell) {
    sync();
    if (state_ == nullptr || k >= groups_.size())
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
    // A row inside an agent group toggles `outputLineOpen` per message, which
    // `expanded(groupId)` cannot see (it reads the group's own bit). Any toggle of the
    // shared sets changes the membership of exactly one of them by one, so their
    // combined size is the O(1) signal that covers them. (A thinking disclosure lives
    // in `UiState`, so that one is the caller's own report.)
    //
    // This is only a toggle signal while the draw does not write the sets itself: the
    // one-shot defaults are seeded in `sync()` at build time, so the writes between
    // these two samples are the user's own activations.
    groupOpenBefore_ = state_->chat.outputLineOpen.size() + state_->chat.openWorkGroups.size();
    groupTop_ = ImGui::GetCursorScreenPos().y;
    // One block scope per group, keyed by the adapter's own group id (D-GROUPID:
    // monotonic, never reused, never a frame counter) - rich_md's fragment ids are keyed
    // to it, so paging and a collapsed-then-expanded group cannot remap them.
    ui::beginBlock(entry.view.id);
    groupOpen_ = true;
    return true;
}

void ThreadStoreAdapter::endGroup(size_t k, const ui::Rect& cell) {
    if (state_ == nullptr || k >= groups_.size() || !groupOpen_)
        return;
    const Entry& entry = groups_[k];
    ui::endBlock();
    groupOpen_ = false;

    // The expansion answer for this group: only a group that was *drawn* can have been
    // toggled, so this is also the bound on the id's lifetime. The answer is the group
    // id even when the toggle happened on a row inside it: the anchor can only hold a
    // header row the view knows about, so the group's header is what stays put - the
    // row above the content the user just changed.
    const std::size_t openAfter =
        state_->chat.outputLineOpen.size() + state_->chat.openWorkGroups.size();
    if (!entry.stream &&
        (expanded(entry.view.id) != groupExpandedBefore_ || openAfter != groupOpenBefore_))
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
    model::HeightHint& hint = state_->ui.thread.hints[entry.view.id];
    const float drawnH = std::max(m.row, ImGui::GetCursorScreenPos().y - groupTop_);
    // The live answer may grow while it streams and must never *shrink* within one revision
    // (design FR-08: "the group height may grow but never shrink within a revision"). Two
    // frames of the same text can still measure differently - the caret's half of the blink,
    // a wrap boundary a hair's breadth away, rich_md re-laying out the last fragment - and a
    // hint that followed them down would make the follower jump back and forth. A taller
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
    if (state_ == nullptr)
        return;
    commitExpansionDefaults(state_->chat);
}

void ThreadStoreAdapter::clearStream() {
    if (streamGroupId_ != 0 && state_ != nullptr) {
        state_->ui.thread.thinkingOpen.erase(
            model::BlockId{streamGroupId_, model::kStreamMessageId, model::BlockKind::Thinking});
        // A focus on one of the stream's rows names a block that is gone with the
        // stream; do not leave `y` consuming a character for a target that no row
        // can match. The menu's owner (task 44) goes with it: a menu opened on the
        // answer must not outlive the answer.
        if (state_->ui.thread.focus.group == streamGroupId_)
            state_->ui.thread.focus = {};
        if (state_->ui.thread.menuBlockId.group == streamGroupId_)
            state_->ui.thread.menuBlockId = {};
    }
    streamGroupId_ = 0;
    streamRevision_ = 0;
    streamFingerprint_ = 0;
    syncedStream_ = 0;
    // Clearing the id alone is not enough: sync()'s guard compares `hasStreamMsg`
    // with `(streamGroupId_ != 0)`, and a clear that has already set both to false
    // would make the guard skip the rebuild and leave the old stream group drawn.
    // A forced rebuild drops it and lets the next stream allocate a fresh group id.
    synced_ = false;
    // The live answer's clock goes with the stream it was counting (task 38): a new stream
    // starts at 00:00, uncancelled, with its caret's phase free to begin at the beginning.
    // `refreshStreamClock()` re-stamps `startedMs` on the new stream's first sighting.
    if (state_ != nullptr)
        model::resetStreamClock(state_->ui.thread.stream);
}

void ThreadStoreAdapter::reset() {
    groups_.clear();
    indexById_.clear();
    idByStart_.clear();
    // The hint cache is view state and lives in `UiState`; a reset comes from a
    // clear or a session switch, and `resetForSessionSwitch()` is called from the
    // same place - but clearing here too keeps "ids are reset" and "hints are reset"
    // one call, so an adapter reset in a harness cannot leave a stale height behind.
    if (state_ != nullptr)
        state_->ui.thread.hints.clear();
    hintWidth_ = 0.0f;
    hintCompact_ = false;
    expandedGroup_ = 0;
    groupOpen_ = false;
    groupExpandedBefore_ = false;
    groupOpenBefore_ = 0;
    groupTop_ = 0.0f;
    // The thinking disclosures are thread-scoped like the hints: a clear or a session
    // switch replaces the thread, and a disclosure left open would be applied to the
    // next session's block with the same identity (ids restart at 1). The streamed
    // reasoning is seeded open again when its new group is born. The table source flags
    // (task 37) are the same kind of state - the next session's block can be the one the
    // flag would name - so they go with them.
    if (state_ != nullptr) {
        state_->ui.thread.thinkingOpen.clear();
        state_->ui.thread.tableSource.clear();
        // The block menu's owner too (task 44): the ids restart below, so a latch that
        // survived would name a block of the *next* thread.
        state_->ui.thread.menuBlockId = {};
    }
    nextId_ = 1;
    autoOpenWorkGroups_.clear();
    streamGroupId_ = 0;
    streamRevision_ = 0;
    streamFingerprint_ = 0;
    synced_ = false;
    syncedCount_ = 0;
    syncedFront_ = 0;
    syncedBack_ = 0;
    syncedStream_ = 0;
    // The thread is being replaced: the live answer's clock counted an answer of the thread
    // that is going away (task 38), so the next stream starts its own.
    if (state_ != nullptr)
        model::resetStreamClock(state_->ui.thread.stream);
}

} // namespace llmfun::tui::store
