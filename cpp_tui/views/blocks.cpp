/// The block renderer (views/blocks.h owns the contract and the anatomy).
///
/// One row, one hit box, one ink decision. The rules that make it short:
///
///  * the role is a *rule* plus a foreground, never a fill (D-BLOCK), so a block
///    paints no background at all - the body behind it is the region surface;
///  * the state is a glyph *and* a word: `blockStateGlyph()` supplies the mark and
///    the label supplies the meaning, so a 16-colour or ASCII surface loses the
///    mark and keeps the row (NFR-16);
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
#include "views/topbar.h" // elapsedLabel(): the one clock formatter, shared with the top bar

#include "imgui/imgui.h"

#include <algorithm>
#include <cstdio>
#include <string>

namespace llmfun::tui::views {
namespace {

/// The separator between a label and what follows it, per profile. The grid stays
/// ASCII: its chrome must not depend on the terminal's locale (`LC_ALL=C` turns
/// U+00B7 into a replacement character), while the pixel profile can afford the
/// middle dot.
const char* separator(bool textGrid) { return textGrid ? " - " : " \xC2\xB7 "; }

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
    }
    return ui::ColorToken::roleSystem;
}

ui::Icon blockStateGlyph(BlockState state) {
    switch (state) {
    case BlockState::None:
        return ui::Icon::Count; // no mark: the row's word carries the meaning
    case BlockState::Pending:
        return ui::Icon::Pending;
    case BlockState::Running:
        return ui::Icon::Running;
    case BlockState::Ok:
        return ui::Icon::Ok;
    case BlockState::Failed:
        return ui::Icon::Error;
    }
    return ui::Icon::Count;
}

bool blockIsCompactRow(BlockKind kind) {
    return kind == BlockKind::ToolCall || kind == BlockKind::ToolResult;
}

bool blockHasDisclosure(BlockKind kind) {
    return kind == BlockKind::WorkGroup || kind == BlockKind::Thinking;
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

std::string blockHeaderText(const BlockView& block, const BlockDraw& draw, bool textGrid) {
    const char* label = block.label.empty() ? defaultBlockLabel(block.kind) : block.label.c_str();
    std::string text = label;
    const std::string timing = blockTimingLabel(block.timingS);
    if (!timing.empty())
        text += separator(textGrid) + timing;
    // The live answer's header (design FR-08, task 38): the phase word the model's vocabulary
    // gives the row and the run's clock - `AGENT · writing · 00:12`, `AGENT · cancelled · 00:07`.
    // The clock is a reading of the frame's clock, not a duration, so it is the top bar's own
    // formatter (`mm:ss`) instead of `blockTimingLabel`'s ("1.2 s"): two clocks in one window
    // must not spell the same moment two ways. Both parts are the descriptor's (the adapter
    // derived them once per frame), so a row that is not streaming draws exactly what it did.
    if (block.streaming && !block.phase.empty()) {
        text += separator(textGrid);
        text += block.phase;
        if (block.elapsedMs > 0)
            text += separator(textGrid) + elapsedLabel(block.elapsedMs);
    }
    if (block.kind == BlockKind::WorkGroup) {
        // The group says how many steps it holds - the one number that tells a
        // collapsed group from a single message.
        text += separator(textGrid);
        text += std::to_string(block.steps);
        text += block.steps == 1 ? " step" : " steps";
    }
    // A compact row's second line *is* its summary, and the streaming message's body
    // is being written under it: neither repeats the preview in the header.
    if (!draw.expanded && !block.streaming && !blockIsCompactRow(block.kind) &&
        !block.summary.empty()) {
        text += "  ";
        text += block.summary;
    }
    return text;
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

    if (blockHasDisclosure(block.kind)) {
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
    // A state glyph, if the block has a state: the mark, then one unit of air.
    const ui::Icon stateIcon = blockStateGlyph(block.state);
    if (stateIcon != ui::Icon::Count) {
        const ui::Rect slot{penX, pos.y, m.gu, m.row, m.textGrid};
        const ui::ColorToken stateInk =
            block.state == BlockState::Failed
                ? ui::ColorToken::stateDanger
                : (block.state == BlockState::Ok ? ui::ColorToken::stateOk
                                                 : ui::ColorToken::textMid);
        ui::iconMark(m, slot, stateIcon, stateInk);
        penX += m.gu + m.pad1;
    }

    textAt(m, header, penX, pos.y, blockHeaderText(block, draw, m.textGrid), ink);
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
    // `Copy` is disabled for a block with no body (the `WORK` header, a stream row before
    // its first character): the item must not promise text the block does not have, and
    // a disabled item copies nothing. The keyboard half stays `y` on the focused block,
    // so copy never needs the terminal's own `Ctrl+C` (plan/tui_copy_probe.md).
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
            const bool canCopy = block.hasBody;
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
