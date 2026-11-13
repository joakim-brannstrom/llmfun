/// The block renderer (views/blocks.h owns the contract and the anatomy).
///
/// One row, one hit box, one ink decision. The rules that make it short:
///
///  * the role is a *rule* plus a foreground, never a fill (D-BLOCK), so a block
///    paints no background at all - the body behind it is the region surface;
///  * the state paints a word, never a glyph: a failed tool card renders
///    the red FAIL on its header, folded or open, and the live states drive the
///    thread view's chips line;
///  * every length comes from the metrics and every colour from a token - the file
///    has no colour literal and never asks which profile is up (`tools/gate_ratchet.txt`
///    holds it to that);
///  * the copy control is drawn only for the focused block and the hit box is
///    narrowed by its width, so the two items never overlap: ImGui gives a point to
///    the first item that claims it, and an overlapping copy chip would be
///    unhoverable (the same reason the rail's rows and their menu do not overlap);
///  * copy has three doors and one action: the focused block's chip and `y` (the
///    keyboard half, read by the thread view) plus the block menu any row opens. The
///    terminal owns `Ctrl+C` (`ISIG` -> `SIGINT`), so a copy path that needed it
///    would not exist at all (task 44).
#include "views/blocks.h"

#include "ui/icons.h"
#include "ui/layout.h"
#include "ui/metrics.h"
#include "ui/primitives.h"
#include "ui/text.h"
#include "ui/tokens.h"

#include "imgui/imgui.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace llmfun::tui::views {
namespace {

/// The separator between a label and what follows it, per profile. The grid stays
/// ASCII: its chrome must not depend on the terminal's locale (`LC_ALL=C` turns
/// U+00B7 into a replacement character), while the pixel profile can afford the
/// middle dot.
const char* separator(const ui::Metrics& m) { return m.textGrid ? " - " : " \xC2\xB7 "; }

/// `text`'s width in the profile's own unit: cells on the grid (a wide glyph counts
/// two) and pixels in the pixel profile (the atlas). The same measure every view
/// carries a copy of: the pixel half needs `ImGui::CalcTextSize`, which `ui/text.h`
/// deliberately does not reach for, so the per-view copy is what that layering costs.
float textUnits(const ui::Metrics& m, std::string_view text) {
    if (text.empty())
        return 0.0f;
    if (m.textGrid)
        return static_cast<float>(ui::displayWidth(text));
    // A context without a font (a measure before the first frame) contributes
    // nothing rather than crashing in `CalcTextSize`.
    if (ImGui::GetCurrentContext() == nullptr || ImGui::GetFont() == nullptr)
        return 0.0f;
    return ImGui::CalcTextSize(text.data(), text.data() + text.size()).x;
}

/// The batch's rendered chips: the stored `"[bash] [grep] [read_file] +N"` string
/// split back into the shown tokens and the already-dropped count, so the composer
/// can drop one more chip at a time while it fits. A token that is not a
/// `+<digits>` count stays a shown chip, so a chip may be named `+x` without
/// being eaten as the dropped count. It is our own rendering being
/// re-read, never a wire format: the store is the one place that composes it, and
/// the view is the one place that fits it.
struct Chips {
    std::vector<std::string> shown;
    int dropped = 0;
};

Chips splitChips(const std::string& chips) {
    Chips out;
    std::size_t begin = 0;
    while (begin <= chips.size()) {
        const std::size_t end = chips.find(' ', begin);
        const std::string token =
            chips.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
        if (token.empty()) {
            if (end == std::string::npos)
                break;
            begin = end + 1;
            continue;
        }
        if (token.size() > 1 && token[0] == '+' &&
            std::isdigit(static_cast<unsigned char>(token[1])))
            out.dropped = std::atoi(token.c_str() + 1);
        else
            out.shown.push_back(token);
        if (end == std::string::npos)
            break;
        begin = end + 1;
    }
    return out;
}

/// The copy control's label. Its identity is the label *and* its rect, both inside
/// the block's id scope, so two rows' chips are two items. The block menu's item uses
/// the same word: one action, one spelling, wherever it is offered.
constexpr const char* kCopyLabel = "Copy";

/// The block menu (task 44) - the pointer's way to copy a block.
///
/// The terminal cannot give the UI `Ctrl+C` (`ISIG` turns the byte into `SIGINT`, so it
/// quits), and rich_md's mouse selection is off on the grid for the same reason. Copy
/// therefore has to be an action of the block itself: `y` on the focused block is the
/// keyboard half (`views/thread.cpp`) and this menu is the pointer half. It is a
/// *menu*, not a second chip, because it must be usable on a block the user has not
/// focused.
///
/// *Which* block it acts on is two things, and both are needed:
///
///  * the popup id - this constant hashed in the block's own `PushID(key)`/`PushID(kind)`
///    scope (the identity the row's hit box and the `Copy` chip already use), which is
///    what makes "open on row A, begin from row B" impossible in one frame; and
///  * the *owner* - `UiState::thread.menuBlockId`, latched by the thread view from
///    `BlockResult::menuRequested` and submitted only by the block it names. The id
///    scope alone is not enough: `BlockId`s are recycled by a transcript reset (group
///    ids restart at 1), so a new message could inherit an open popup and be copied
///    under a menu the user opened on the old one.
///
/// The menu does not move the focus, so the keyboard keeps acting on the *focused* block
/// while it is open - `y` copies that block, `s` flips its table source. That is how the
/// rail's row menu behaves too (its `m` still targets the active row), and it is the
/// reason the menu's item carries its own block rather than "the selected one".
constexpr const char* kBlockMenuId = "##block_menu";

/// One text line at an absolute position, clipped to `clip`.
///
/// `AddText` neither wraps nor clips on its own: without the clip an over-long
/// summary would paint over the child's scrollbar column. On the grid the string is
/// cut with the profile's own ellipsis, because a cell renderer has to say "there is
/// more" in cells; in the pixel profile the clip rect does that job.
void textAt(const ui::Metrics& m, const ui::Rect& clip, float x, float y, const std::string& text,
            ui::ColorToken ink) {
    if (text.empty() || y + m.row > clip.bottom())
        return;
    const std::string shown =
        m.textGrid ? ui::truncate(text, static_cast<int>(std::max(0.0f, clip.right() - x))) : text;
    // The cursor is restored afterwards: a block draws its row at an absolute position
    // and the *flow* must continue under it, or the next block would start where this
    // one's text ended (ImGui advances the cursor by every item it submits, including
    // `TextUnformatted`). That is what keeps a group's rows stacked and its height the
    // height the hint is measured from.
    const ImVec2 flow = ImGui::GetCursorScreenPos();
    ImGui::PushClipRect(ImVec2(clip.x, clip.y), ImVec2(clip.right(), clip.bottom()), true);
    ImGui::PushStyleColor(ImGuiCol_Text, ui::color(ink));
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    ImGui::TextUnformatted(shown.c_str(), shown.c_str() + shown.size());
    ImGui::PopStyleColor();
    ImGui::PopClipRect();
    ImGui::SetCursorScreenPos(flow);
}

/// The composer's answer, split where the renderer splits its inks: the
/// chips run is the batch's data, not the role's word, so it draws in the
/// quiet `textMid` beside the role ink. `text` is what `blockHeaderText()`
/// returns; `chipsBegin`/`chipsEnd` are the chips run's byte offsets in it
/// (equal when the row carries no chips; the text outside them is the role's).
struct HeaderRuns {
    std::string text;
    std::size_t chipsBegin = 0;
    std::size_t chipsEnd = 0;
};

HeaderRuns composeBlockHeader(const BlockView& block, const BlockDraw& draw, const ui::Metrics& m,
                              float availUnits) {
    const char* label = block.label.empty() ? defaultBlockLabel(block.kind) : block.label.c_str();
    // Thinking carries neither a duration nor a count of its own: the cost
    // is the owner's, so the disclosure says the bare word. Every other row keeps
    // the store's duration.
    const std::string timing =
        block.kind == BlockKind::Thinking ? std::string() : blockTimingLabel(block.timingS);
    if (block.kind == BlockKind::ToolStep) {
        // The tool card's header: `TOOL <name>` while the card streams,
        // `TOOL <name> · <duration>` once the result lands (exactly when
        // durationMs != 0 - never a placeholder). The header carries no token
        // counts - the wire fields stay, this render consumes none of them. A
        // failure paints its red FAIL word in `renderBlock`, never in the text
        // this returns - the caller draws it with its own ink. Cards hold no
        // chips, so there is nothing here to fit.
        std::string text = label;
        if (!block.summary.empty()) {
            text += "  ";
            text += block.summary;
        }
        if (!timing.empty())
            text += separator(m) + timing;
        return HeaderRuns{text, 0, 0};
    }

    // The composer: the parts join with the profile's
    // separator, and the row is fitted to `availUnits` by dropping from the right
    // - chips one token at a time, then the duration, never the label.
    Chips chips = splitChips(block.chips);
    std::vector<std::string> parts;
    // A folded batch owner drops the role word: the duration and the
    // chips already say what the row is, and the mark + the fold say which batch.
    if (!block.ownsBatch || draw.expanded)
        parts.push_back(label);
    std::size_t timingIndex = 0;
    if (!timing.empty()) {
        timingIndex = parts.size();
        parts.push_back(timing);
    }
    std::size_t chipsIndex = 0;
    bool hasChips = false;
    const auto chipsText = [&chips] {
        std::string text;
        for (const std::string& chip : chips.shown) {
            if (!text.empty())
                text += ' ';
            text += chip;
        }
        if (chips.dropped > 0)
            text += " +" + std::to_string(chips.dropped);
        return text;
    };
    // A run is composed only when the chips say something: a chips string of
    // only whitespace parses to nothing, and pushing an empty part would hang
    // a separator with nothing on either side of it.
    if (!chips.shown.empty() || chips.dropped > 0) {
        chipsIndex = parts.size();
        hasChips = true;
        parts.push_back(chipsText());
    }
    // The live answer's header: the phase word the model's vocabulary gives the
    // row - `AGENT · writing`, `AGENT · cancelled`. The phase is event-driven,
    // so the header changes only when the phase does, and the run's clock lives
    // in the top bar and the now bar. The phase is the descriptor's, so a row
    // that is not streaming draws label and timing only.
    if (block.streaming && !block.phase.empty())
        parts.push_back(block.phase);
    if (block.kind == BlockKind::WorkGroup) {
        // The group says how many steps it holds - the one number that tells a
        // collapsed group from a single message.
        parts.push_back(std::to_string(block.steps) + (block.steps == 1 ? " step" : " steps"));
    }

    const auto join = [&m](const std::vector<std::string>& in) {
        std::string text;
        for (const std::string& part : in) {
            if (!text.empty())
                text += separator(m);
            text += part;
        }
        return text;
    };
    std::string text = join(parts);
    while (textUnits(m, text) > availUnits && chips.shown.size() > 1) {
        chips.shown.pop_back();
        ++chips.dropped;
        parts[chipsIndex] = chipsText();
        text = join(parts);
    }
    if (textUnits(m, text) > availUnits && !timing.empty()) {
        parts.erase(parts.begin() + static_cast<std::ptrdiff_t>(timingIndex));
        text = join(parts);
        // The timing sat before the chips, so the chips part moved down with it.
        if (hasChips)
            --chipsIndex;
    }

    // The chips run's place in the fitted text: the renderer splits its inks
    // at this boundary, so the draw needs the byte offsets, not a re-parse.
    HeaderRuns runs;
    runs.text = text;
    if (hasChips) {
        std::string before;
        for (std::size_t i = 0; i < chipsIndex; ++i) {
            if (!before.empty())
                before += separator(m);
            before += parts[i];
        }
        std::string after;
        for (std::size_t i = chipsIndex + 1; i < parts.size(); ++i) {
            if (!after.empty())
                after += separator(m);
            after += parts[i];
        }
        // `before` joins the parts ahead of the run, so the separator that
        // joins them to the run comes now; and what follows the run keeps
        // the separator's join.
        const std::string pre = before.empty() ? std::string() : std::string(separator(m));
        runs.chipsBegin = before.size() + pre.size();
        runs.chipsEnd = runs.chipsBegin + parts[chipsIndex].size();
        runs.text = before + pre + parts[chipsIndex] +
                    (after.empty() ? std::string() : std::string(separator(m)) + after);
    }

    // The tool card's name
    if (!block.summary.empty() && !draw.expanded && !block.streaming &&
        !blockIsCompactRow(block.kind)) {
        // The closed preview keeps its two-space join and is pinned, not fitted:
        // it is the one thing the row says about a body that is hidden, and a
        // truncating composer would take the row's whole meaning with it.
        runs.text += "  ";
        runs.text += block.summary;
    }
    return runs;
}

} // namespace

const char* defaultBlockLabel(BlockKind kind) {
    switch (kind) {
    case BlockKind::User:
        return "YOU";
    case BlockKind::Answer:
        return "ANSWER";
    case BlockKind::WorkGroup:
        return "WORK";
    case BlockKind::Assistant:
        return "AGENT";
    case BlockKind::ToolCall:
    case BlockKind::ToolResult:
    case BlockKind::ToolStep:
        return "TOOL";
    case BlockKind::Thinking:
        return "Thinking";
    case BlockKind::System:
        return "SYSTEM";
    case BlockKind::Code:
        // A code block is a *body* shape, not a role: the role words stay the
        // message's own ("an agent answered with code"). This is the fallback for a
        // descriptor that carried none.
        return "AGENT";
    }
    return "SYSTEM";
}

ui::ColorToken blockInk(BlockKind kind) {
    switch (kind) {
    case BlockKind::User:
        return ui::ColorToken::roleYou;
    case BlockKind::System:
        // A notice is neutral by design (§4.5: "SYSTEM label, neutral text").
        return ui::ColorToken::roleSystem;
    case BlockKind::ToolCall:
    case BlockKind::ToolResult:
        // The verbatim voice: a tool row is a transcript line, not prose.
        return ui::ColorToken::roleTool;
    case BlockKind::Answer:
    case BlockKind::WorkGroup:
    case BlockKind::Assistant:
    case BlockKind::Thinking:
    case BlockKind::Code:
        return ui::ColorToken::roleAgent;
    case BlockKind::ToolStep:
        // The merged card draws neutral ink: the call and its result are one
        // transcript, and neither side carries a role of its own.
        return ui::ColorToken::roleSystem;
    }
    return ui::ColorToken::roleSystem;
}

bool blockIsCompactRow(BlockKind kind) {
    return kind == BlockKind::ToolCall || kind == BlockKind::ToolResult;
}

bool blockHasDisclosure(const BlockView& block) {
    // The disclosure is a property of the row, not of its kind: a batch's
    // owner row is a `BlockKind::Assistant` row and still folds -
    // the AGENT word's row is the batch's own disclosure.
    return block.kind == BlockKind::WorkGroup || block.kind == BlockKind::Thinking ||
           block.kind == BlockKind::ToolStep || block.ownsBatch;
}

std::string blockTimingLabel(double seconds) {
    if (!(seconds > 0.0))
        return {}; // also catches NaN: a duration that is not a number is no duration
    char buffer[48];
    if (seconds < 1.0) {
        std::snprintf(buffer, sizeof buffer, "%d ms", static_cast<int>(seconds * 1000.0 + 0.5));
        return buffer;
    }
    if (seconds < 60.0) {
        std::snprintf(buffer, sizeof buffer, "%.1f s", seconds);
        return buffer;
    }
    const int total = static_cast<int>(seconds + 0.5);
    if (total < 3600) {
        std::snprintf(buffer, sizeof buffer, "%dm %ds", total / 60, total % 60);
        return buffer;
    }
    std::snprintf(buffer, sizeof buffer, "%dh %dm", total / 3600, (total % 3600) / 60);
    return buffer;
}

std::string blockHeaderText(const BlockView& block, const BlockDraw& draw, const ui::Metrics& m,
                            float availUnits) {
    return composeBlockHeader(block, draw, m, availUnits).text;
}

std::string blockRowText(const BlockView& block) {
    return blockIsCompactRow(block.kind) ? block.summary : std::string();
}

float measureBlockHeader(const BlockView& block, const ui::Metrics& m, const BlockDraw& draw) {
    // A compact row is two lines *while it is closed* - its summary is the second one.
    // Open, the body says the same thing once, so the row gives that line back instead
    // of printing it twice. The expansion comes from `draw`, the same state
    // `renderBlock()` is handed: one input, so the measurement and the drawing cannot
    // disagree about the row's height.
    const bool twoLines = blockIsCompactRow(block.kind) && !draw.expanded;
    return m.row * (twoLines ? 2.0f : 1.0f);
}

BlockResult renderBlock(const BlockView& block, const ui::Rect& rect, const ui::Metrics& m,
                        const BlockDraw& draw) {
    BlockResult out;
    // Mixed units paint nothing (rectMatchesProfile asserts in a debug build): a
    // block drawn in the wrong coordinate space is a bug, and a plausible-looking
    // row would hide it.
    if (!ui::rectMatchesProfile(rect, m) || rect.w <= 0.0f || rect.h <= 0.0f)
        return out;

    const float headH = std::min(rect.h, measureBlockHeader(block, m, draw));
    const bool compact = blockIsCompactRow(block.kind);

    // The focused block shows its copy control; the row's hit box gives up exactly
    // that width so the two items cannot overlap.
    const ui::ButtonSpec copySpec{kCopyLabel};
    const bool showCopy = draw.focused && block.hasBody;
    const float copyW = showCopy ? std::min(rect.w, ui::measureButton(m, copySpec).w) : 0.0f;
    const float rowW = std::max(0.0f, rect.w - (copyW > 0.0f ? copyW + m.pad1 : 0.0f));

    // The block's own id scope: `key` is the adapter's monotonic identity and the
    // kind separates the two blocks a single message can produce (its thinking
    // disclosure and its body row).
    ImGui::PushID(static_cast<int>(block.key & 0x7fffffff));
    ImGui::PushID(static_cast<int>(block.kind));

    const ImVec2 pos = ImGui::GetCursorScreenPos();
    // A whole-width item: ImGui's ButtonBehavior is what gives the row the mouse and
    // Space/Enter while it holds the nav focus (the v4 role row's own trick).
    //
    // No vertical item spacing: a row *is* one row tall, so the next one must start
    // exactly under it. ImGui's default spacing would put a gutter between two rows and
    // make "the row below the header" a place where nothing is drawn (or clickable).
    const ImVec2 spacing = ImGui::GetStyle().ItemSpacing;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(spacing.x, 0.0f));
    const bool activated = ImGui::InvisibleButton("##block_row", ImVec2(rowW, headH));
    // The block menu's trigger: a right-click on the header. `OpenPopupOnItemClick` is
    // ImGui's own context-menu helper and has to be read while the row is the last item;
    // it opens on the *release*, which is the convention every popup follows. The item,
    // not this read, decides whether there is anything to copy. Nothing else changes on
    // a right-click - the focus and the expansion stay where the left button put them,
    // like the rail's row menu.
    out.menuRequested = ImGui::OpenPopupOnItemClick(kBlockMenuId, ImGuiPopupFlags_MouseButtonRight);
    ImGui::PopStyleVar();
    const ImVec2 afterRow = ImGui::GetCursorScreenPos();
    out.toggled = activated && block.expandable;
    out.focusTaken = activated;

    const ui::Rect header{pos.x, pos.y, rowW, headH, m.textGrid};
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ui::ColorToken roleInk = blockInk(block.kind);
    const ui::ColorToken ink = draw.focused ? ui::ColorToken::accent : roleInk;
    float penX = pos.x;

    if (blockHasDisclosure(block)) {
        // A mark that toggles, then the label: the same anatomy `ui::disclosure`
        // draws, with the ink the focus state needs (the primitive fixes its own
        // two tokens, and a focused block must read as the active row).
        const ui::Rect slot{pos.x, pos.y, m.gu, m.row, m.textGrid};
        ui::iconMark(m, slot, draw.expanded ? ui::Icon::DisclosureOpen : ui::Icon::DisclosureClosed,
                     draw.focused ? ui::ColorToken::accent : ui::ColorToken::textLow);
        penX = pos.x + m.gu + m.pad1;
    } else {
        // The role rule: a filled bar, one cell on the grid and 2 px in the pixel
        // profile, in the role's own foreground (D-BLOCK).
        const float ruleW = std::min(m.textGrid ? 1.0f : 2.0f, rowW);
        if (ruleW > 0.0f)
            ui::rule(m, ui::Rect{pos.x, pos.y, ruleW, headH, m.textGrid}, ink);
        penX = pos.x + ruleW + (m.textGrid ? 0.0f : m.pad1);
    }
    // No state glyph: the status is a word on the row's header - a failed card
    // or a failed batch owner paints red FAIL after its fitted header text,
    // folded or open, and every other state stays silent (success needs no mark).
    const float availUnits = std::max(0.0f, header.w - (penX - pos.x));
    const HeaderRuns runs = composeBlockHeader(block, draw, m, availUnits);
    // The header's inks split at the chips boundary - the role's word and
    // the closed preview in the role's ink, the chips run in the quiet textMid
    // beside it (the chips are the batch's data, not the role's word). Each run
    // starts where the prefixes before it measure to, all off the same text.
    const std::string head = runs.text.substr(0, runs.chipsBegin);
    textAt(m, header, penX, pos.y, head, ink);
    if (runs.chipsBegin < runs.chipsEnd)
        textAt(m, header, penX + textUnits(m, head), pos.y,
               runs.text.substr(runs.chipsBegin, runs.chipsEnd - runs.chipsBegin),
               ui::ColorToken::textMid);
    if (runs.chipsEnd < runs.text.size())
        textAt(m, header, penX + textUnits(m, runs.text.substr(0, runs.chipsEnd)), pos.y,
               runs.text.substr(runs.chipsEnd), ink);
    // The failure is a word, not a state glyph: a failed tool card has always
    // painted red FAIL after its header, folded or open, and a batch owner now
    // does the same for its failed calls - one spelling of failure, wherever
    // the row carries it.
    const bool failed = block.failedCalls > 0 ||
                        (block.kind == BlockKind::ToolStep && block.state == BlockState::Failed);
    if (failed) {
        const float textW = textUnits(m, runs.text);
        textAt(m, header, penX + textW + m.pad1, pos.y, "FAIL", ui::ColorToken::stateDanger);
    }
    if (compact && !draw.expanded) {
        // The compact row's second line: the one-line summary, indented under the label
        // so the glyph column stays a column. It is the row's *rest* state - open, the
        // body carries the same text once, and repeating it above the body would print
        // the summary twice (D-BOUNDS: no content loss, but no duplication either).
        textAt(m, header, penX, pos.y + m.row, blockRowText(block), ui::ColorToken::textMid);
    }

    if (draw.focused && !m.textGrid) {
        // The ring, in the pixel profile: the grid has no cell to draw one in, and
        // there the accent label is the visible focus state (design §4.9's accepted
        // hole for a cell renderer).
        dl->AddRect(
            ImVec2(pos.x + 0.5f, pos.y + 0.5f), ImVec2(pos.x + rowW - 0.5f, pos.y + headH - 0.5f),
            ImGui::GetColorU32(ui::color(ui::ColorToken::focusRing)), m.radius, 0, m.border);
    }

    if (copyW > 0.0f) {
        const ui::Rect chip{pos.x + rowW + m.pad1, pos.y, copyW, m.row, m.textGrid};
        ui::button(copySpec, m, chip);
        // Set, never assigned: the menu below sets the same flag, and an assignment here
        // would silently cancel it the day this block moves.
        if (ui::buttonPressed(copySpec, m, chip))
            out.copyPressed = true;
        // `button()` leaves the cursor advanced to its own item; the block's flow must
        // continue under the *header*, or a compact row's body would start one row too
        // high.
        ImGui::SetCursorScreenPos(afterRow);
    }

    // The block menu (task 44): the pointer's copy path, available on any block - not
    // only the focused one - and with no modifier. Only its owner submits it (see
    // `kBlockMenuId`): a popup the thread state has stopped naming survives one grace
    // frame and is closed at the next `NewFrame` (ImGui restores focus away from the
    // now-inactive popup window); `menuOpen` is what tells the caller to drop the latch.
    //    `Copy` is disabled for a block that copies nothing: the item must not
    //    promise text the block does not have, and a disabled item copies
    //    nothing. A batch owner has no body of its own but IS the copy target
    //    for its batch's content (`copyable`), so the item reads
    //    the copy target, not the body. The keyboard half stays `y` on the
    //    focused block, so copy never needs the terminal's own `Ctrl+C`.
    if (out.menuRequested || draw.menuOwner) {
        // No `SetNextWindowPos`: a mouse-opened popup already latches the pointer as its
        // position (`OpenPopupPos`), and setting one here would be left dangling for the
        // next window whenever the popup is not open.
        if (ImGui::BeginPopup(kBlockMenuId)) {
            out.menuOpen = true;
            // A disabled item's `BeginDisabled()` only scales the *alpha* of its text,
            // and a cell grid decodes a text colour without alpha (imtui's
            // `rgbToAnsi256(..., false)`), so on the grid a disabled item would look
            // exactly like an enabled one - an affordance promising text that is not
            // there. The ink is pushed explicitly, the way the palette dims a row it
            // cannot run and `ui::button` dims a disabled control: the token the theme
            // already put on `ImGuiCol_TextDisabled`. One fact drives both halves -
            // whether the item may act and whether it looks like it may - so "drawn
            // enabled, acts disabled" cannot happen.
            const bool canCopy = block.hasBody || block.copyable;
            if (!canCopy)
                ImGui::PushStyleColor(ImGuiCol_Text,
                                      ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            const bool copy = ImGui::MenuItem(kCopyLabel, nullptr, /*selected=*/false, canCopy);
            if (!canCopy)
                ImGui::PopStyleColor();
            if (copy)
                out.copyPressed = true;
            ImGui::EndPopup();
        }
    }

    // The body is the caller's (the adapter owns markdown): it may draw it now.
    out.drawBody = draw.expanded && block.hasBody;

    ImGui::PopID();
    ImGui::PopID();
    return out;
}

} // namespace llmfun::tui::views
