/// The top bar (views/topbar.h owns the contract and the anatomy).
///
/// Two renderings, one anatomy: the numbers come from `topBarMeasure()` and both profiles
/// draw from them, so the grid's one cell row and the pixel profile's 48 px row are the
/// same design at two sizes rather than two designs that resemble each other.
///
/// What this file deliberately does not do:
///   * it does not read the aggregate, the display size or the profile flag - the bar is a
///     geometry taker, so `./test_topbar` can draw it into a cell grid with no TuiState;
///   * it does not queue the rename itself: a committed rename is an *answer* the frame
///     layer applies through the rail's own `queueSessionAction()` (§6.2, one registry
///     entry), so the two entry points cannot disagree about the action;
///   * it does not write a glyph or a colour literal: marks come from `ui/icons.h` and
///     colours from `ui::color()`, like every other view.
///
/// Layering note (`tools/gate_ratchet.txt`): this file has no profile branch (`m.textGrid`
/// is data the metrics handed it), no `ImVec4`, no v4 type and no display read, so it needs
/// no ledger row.
#include "views/topbar.h"

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"

#include "ui/icons.h"
#include "ui/layout.h"
#include "ui/metrics.h"
#include "ui/primitives.h"
#include "ui/text.h"
#include "ui/tokens.h"
#include "views/chrome.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <string_view>

namespace llmfun::tui::views {
namespace {

using ui::ColorToken;
using ui::Icon;

/// The identity, and the one string the bar draws without a mark: the appendix spells the
/// wordmark as the word itself (an accent square to its left in the GUI, the word alone on
/// a one-cell bar).
constexpr const char* kWordmark = "llmfun";

/// The palette hint: the discoverability layer, and the first thing to drop when the bar is
/// narrow (`Ctrl+K` still works - the hint is an advertisement, not the control). The design
/// spells it per profile (§4.2: `Ctrl+K commands` on the one-row bar) and §5.1
/// (`Ctrl+K Search` in the window), so it is a string the metrics' profile picks.
constexpr const char* kHintGrid = "Ctrl+K commands";
constexpr const char* kHintGui = "Ctrl+K Search";

/// The settings entry. Text, not a gear glyph: `ui/icons.h` carries state marks, not
/// shapes, and a gear would be a shape with no word beside it (NFR-16) - and a second name
/// for the same control. It is the sheet's visible entry point (design §4.2 "gear opens
/// Settings", task 34), so the label *is* the control and needs no tooltip to explain itself.
constexpr const char* kSettingsLabel = "Settings";
constexpr const char* kBusyRenameHint = "a rename cannot be queued while a run is active";

/// The hint this profile draws, so the measure and the drawing cannot pick different
/// strings.
const char* hintText(const ui::Metrics& m) { return m.textGrid ? kHintGrid : kHintGui; }

/// `text`'s width in the profile's own unit: cells on the grid (a wide glyph counts two,
/// the renderer's own table) and pixels in the pixel profile (the atlas).
///
/// `ui/primitives.cpp` has the same measure for its own labels; it is restated here because
/// a bar of *plain* text needs it - every `ui::measure*()` answers a box (text plus
/// padding), and the title's box is the room that is left rather than a measured string.
float textUnits(const ui::Metrics& m, std::string_view text) {
    if (text.empty())
        return 0.0f;
    if (m.textGrid)
        return static_cast<float>(ui::displayWidth(text));
    // A context without a font (a measure before the first frame) contributes nothing
    // rather than crashing in `CalcTextSize`.
    if (ImGui::GetCurrentContext() == nullptr || ImGui::GetFont() == nullptr)
        return 0.0f;
    return ImGui::CalcTextSize(text.data(), text.data() + text.size()).x;
}

/// One line of text inside `r`, vertically centred and clipped to it - the two renderings
/// `ui::primitives`' labels use: the grid cuts in cells with the profile's own ellipsis (a
/// cell cannot be half drawn), the pixel profile draws the whole line and the clip cuts it.
void line(const ui::Metrics& m, const ui::Rect& r, std::string_view text, ColorToken ink) {
    if (r.w <= 0.0f || r.h <= 0.0f || text.empty())
        return;
    const float lineH = ImGui::GetCurrentContext() != nullptr ? ImGui::GetTextLineHeight() : 1.0f;
    const ImVec2 pen(r.x, r.y + std::floor((r.h - lineH) * 0.5f));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 col = ImGui::GetColorU32(ui::color(ink));
    dl->PushClipRect(ImVec2(r.x, r.y), ImVec2(r.right(), r.bottom()), true);
    if (m.textGrid) {
        const std::string fitted = ui::truncate(text, static_cast<int>(r.w));
        dl->AddText(pen, col, fitted.c_str(), fitted.c_str() + fitted.size());
    } else {
        dl->AddText(pen, col, text.data(), text.data() + text.size());
    }
    dl->PopClipRect();
}

/// A measured part as the rectangle the primitives take: the *unit* comes from the metrics the
/// bar was measured with, stated once here instead of repeated on every part (`TopBarPart`).
ui::Rect rectOf(const TopBarPart& part, const ui::Metrics& m) {
    return ui::Rect{part.x, part.y, part.w, part.h, m.textGrid};
}

/// A title that is only whitespace is not a title: D rejects it and the rail already
/// rejects it before sending, so the bar refuses it at the source instead of queueing an
/// action that cannot succeed.
bool whitespaceOnly(const std::string& text) {
    for (const char c : text)
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
            return false;
    return true;
}

/// The run pill's ink and mark: one glyph, one word, one colour per state (NFR-16).
///
/// The pairs are measured against the `raised` chip fill - idle 7.9:1, running 7.0:1,
/// waiting 8.0:1, failed 5.7:1 - so the word stays readable at every colour depth;
/// `./test_topbar` re-measures them rather than trusting this comment.
struct PillLook {
    Icon mark;
    ColorToken ink;
};

PillLook pillLook(RunState state) {
    switch (state) {
    case RunState::Idle:
        return PillLook{Icon::Ok, ColorToken::textMid};
    case RunState::Running:
        return PillLook{Icon::Running, ColorToken::accent};
    case RunState::Waiting:
        return PillLook{Icon::Pending, ColorToken::stateWarn};
    case RunState::Failed:
        return PillLook{Icon::Error, ColorToken::stateDanger};
    case RunState::RawLogOpen:
        break;
    }
    // `RawLogOpen` is the bar's mode, not a run state: a caller that reaches here asked the
    // pill to report a mode, and the answer is the ordinary run's.
    return PillLook{Icon::Running, ColorToken::accent};
}

} // namespace

const char* runStateName(RunState state) {
    switch (state) {
    case RunState::Idle:
        return "idle";
    case RunState::Running:
        return "running";
    case RunState::Waiting:
        return "waiting";
    case RunState::Failed:
        return "failed";
    case RunState::RawLogOpen:
        return "rawLogOpen";
    }
    return "idle";
}

RunState barState(const TopBarState& state) {
    return state.rawLogOpen ? RunState::RawLogOpen : state.run;
}

std::string elapsedLabel(long long ms) {
    if (ms < 0)
        ms = 0;
    const long long totalSeconds = ms / 1000;
    const long long hours = totalSeconds / 3600;
    const long long minutes = (totalSeconds / 60) % 60;
    const long long seconds = totalSeconds % 60;
    char buffer[32];
    if (hours > 0)
        std::snprintf(buffer, sizeof buffer, "%lld:%02lld:%02lld", hours, minutes, seconds);
    else
        std::snprintf(buffer, sizeof buffer, "%02lld:%02lld", minutes, seconds);
    return buffer;
}

std::string pillLabel(RunState state, long long elapsedMs) {
    switch (state) {
    case RunState::Idle:
        return "Ready";
    case RunState::Running:
        return "running " + elapsedLabel(elapsedMs);
    case RunState::Waiting:
        return "waiting " + elapsedLabel(elapsedMs);
    case RunState::Failed:
        return "failed";
    case RunState::RawLogOpen:
        break;
    }
    return "Ready";
}

TopBarParts topBarMeasure(const TopBarState& state, const ui::Rect& bar, const ui::Metrics& m) {
    TopBarParts parts;
    // Fail closed: a folded region (compact mode folds the top bar: `topBar.h == 0`), a zero
    // area or a rectangle in the other profile's unit paints nothing rather than a plausible
    // bar in the wrong coordinate space.
    if (bar.w <= 0.0f || bar.h <= 0.0f || !ui::rectMatchesProfile(bar, m))
        return parts;

    // The bar draws its plain text at the label step, so it has to *measure* at it too (see
    // `renderTopBar`): the scope is pushed here as well, which is what makes a measure taken
    // outside a draw - by a harness, by a caller that only wants the title's budget - answer the
    // numbers the drawn bar uses. A no-op on the text grid, where a cell has one size.
    const ui::TypeScope labelType(ui::TypeStep::label, m.textGrid);

    const float pad = m.pad1;
    // The content row: one row of chrome, vertically centred in the bar. The grid's bar *is*
    // one row, so that is the bar itself there.
    const float rowY = bar.y + std::floor((bar.h - m.row) * 0.5f);

    // The fixed widths, measured with the primitives that draw them, so a measure and its
    // render cannot drift apart.
    const float markW = m.gu;
    const float wordmarkW = textUnits(m, kWordmark);
    const float ruleW = std::max(0.0f, m.border);
    const ui::ButtonSpec gearSpec{kSettingsLabel, /*enabled=*/false};
    const float gearW = ui::measureButton(m, gearSpec).w;
    const float badgeW = state.rawLogOpen ? ui::measureChip(m, kRawBadge).w : 0.0f;
    const float pillW = ui::measurePill(m, pillLabel(state.run, state.elapsedMs)).w;
    const float hintW = textUnits(m, hintText(m));

    // Build the bar with a given set of optional parts; the title takes what is left.
    //
    // The order on screen is mark, wordmark, rule, title, hint, pill, `RAW`, settings, so the
    // right cluster is placed from the right edge inwards and the hint lands directly left of
    // the pill.
    const auto build = [&](bool withHint, bool withWordmark, bool withGear) {
        TopBarParts out;
        float x = bar.x + pad;
        out.mark = TopBarPart{x, rowY + std::floor((m.row - markW) * 0.5f), markW, markW};
        x = out.mark.right() + pad;
        if (withWordmark) {
            out.wordmark = TopBarPart{x, rowY, wordmarkW, m.row};
            x = out.wordmark.right() + pad;
            out.rule = TopBarPart{x, rowY, ruleW, m.row};
            x = out.rule.right() + pad;
        }
        const float leftEnd = x;

        float right = bar.right() - pad;
        if (withGear) {
            out.gear = TopBarPart{right - gearW, rowY, gearW, m.row};
            right = out.gear.x - pad;
        }
        if (badgeW > 0.0f) {
            out.badge = TopBarPart{right - badgeW, rowY, badgeW, m.row};
            right = out.badge.x - pad;
        }
        out.pill = TopBarPart{right - pillW, rowY, pillW, m.row};
        right = out.pill.x - pad;

        float titleRight = right;
        if (withHint && right - hintW > leftEnd) {
            out.hint = TopBarPart{right - hintW, rowY, hintW, m.row};
            titleRight = out.hint.x - pad;
        }
        out.title = TopBarPart{leftEnd, rowY, std::max(0.0f, titleRight - leftEnd), m.row};
        return out;
    };

    // Priority (design §4.2): the mark, the run pill and the `RAW` badge never drop; the title
    // takes what is left; the hint and the wordmark drop before either of them, and the settings
    // entry only after the wordmark. Four attempts, so a degenerate bar still terminates.
    parts = build(/*withHint=*/true, /*withWordmark=*/true, /*withGear=*/true);
    if (parts.title.empty())
        parts = build(false, true, true);
    if (parts.title.empty())
        parts = build(false, false, true);
    if (parts.title.empty())
        parts = build(false, false, false);

    // The title, cut to its box. The grid cuts in cells and marks the cut with the profile's
    // ellipsis; the pixel profile keeps the string whole (its clip rect cuts) but still reports
    // that it does not fit, because the tooltip and the golden read this.
    if (!parts.title.empty()) {
        if (m.textGrid) {
            const std::string fitted =
                ui::truncate(state.sessionTitle, static_cast<int>(parts.title.w));
            parts.titleTruncated = fitted != state.sessionTitle;
            parts.titleText = fitted;
        } else {
            parts.titleTruncated = textUnits(m, state.sessionTitle) > parts.title.w;
            parts.titleText = state.sessionTitle;
        }
    } else {
        parts.titleTruncated = !state.sessionTitle.empty();
    }
    return parts;
}

void renderTopBar(TopBarState& state, const ui::Rect& bar, const ui::Metrics& m) {
    if (bar.w <= 0.0f || bar.h <= 0.0f || !ui::rectMatchesProfile(bar, m))
        return;

    // The bar's own type step, pushed *before* the measure: in the pixel profile a label is
    // 13 px against the body's 15 px, so a bar measured at the body size and drawn at the label
    // size would reserve ~15 % too much per string - a gap opening before the pill, and a
    // `titleTruncated` that the drawn text does not have. `topBarMeasure()` pushes the same
    // scope itself, so a standalone measure answers the size this view draws at.
    const ui::TypeScope labelType(ui::TypeStep::label, m.textGrid);
    const TopBarParts parts = topBarMeasure(state, bar, m);

    // Region chrome. The design's surface and bottom hairline are pixel-profile chrome: a
    // one-cell bar *is* one cell, and a one-cell hairline would be the whole bar (design §4.2,
    // "TUI: 1 row"). The grid keeps the terminal's own background, like every other region of
    // the grid does. `ui::rule()` is the unrounded fill - a region is square, and the token
    // radius belongs to panels and cards.
    if (!m.textGrid) {
        ui::rule(m, bar, ColorToken::surface);
        ui::Rect edge = bar;
        edge.y = bar.bottom() - m.border;
        edge.h = m.border;
        ui::rule(m, edge, ColorToken::lineSoft);
    }

    // The identity: the accent square, the wordmark and the rule between it and the title. The
    // square is one horizontal unit (1 cell on the grid, 8 px in the GUI); the appendix draws it
    // at 6 px, and the metrics' own `gu` is the token for "one column" - a view may not invent a
    // length the token table does not have (D-THEME).
    ui::rule(m, rectOf(parts.mark, m), ColorToken::accent);
    line(m, rectOf(parts.wordmark, m), kWordmark, ColorToken::textHi);
    if (!parts.rule.empty())
        ui::rule(m, rectOf(parts.rule, m), ColorToken::lineStrong);

    // The title: the inline field while it is open, the read-only line (with its hit box)
    // otherwise.
    const ui::Rect titleRect = rectOf(parts.title, m);
    if (state.renaming) {
        ImGui::SetCursorScreenPos(ImVec2(titleRect.x, titleRect.y));
        if (state.renameFocus)
            ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(std::max(1.0f, titleRect.w));
        // The id changes on every open (renameSeq) so the field re-reads the re-initialised
        // buffer instead of its stale edit state for the same id.
        const std::string fieldId = "##topbar_rename_" + std::to_string(state.renameSeq);
        const bool enter =
            ImGui::InputText(fieldId.c_str(), state.renameBuf.data(), state.renameBuf.size(),
                             ImGuiInputTextFlags_EnterReturnsTrue);
        const bool active = ImGui::IsItemActive();
        if (state.renameFocus && active)
            state.renameFocus = false; // the focus request has been answered
        // The caret marks the focus on the grid; ImGui's nav ring would be a full-cell rectangle
        // that outlives the frame in a terminal (the composer's and the palette's rule).
        if (m.textGrid) {
            ImGuiContext& ctx = *ImGui::GetCurrentContext();
            const ImGuiID fieldItemId = ImGui::GetID(fieldId.c_str());
            if (ctx.ActiveId == fieldItemId || ctx.NavId == fieldItemId)
                ctx.NavCursorVisible = false;
        }
        // `InputText` clears its own ActiveId on Escape, so the press is read from the key rather
        // than from the item state. `acceptKeys` is the frame's answer to "may this press be the
        // bar's": false while an overlay owns the keyboard, and false on a frame whose press a
        // view-local handler (an ImGui popup/menu) already claimed - D-ESC's one step per press.
        // The press this box takes is marked consumed so the frame-level cancel step stands down.
        const bool esc = state.acceptKeys && ImGui::IsKeyPressed(ImGuiKey_Escape, false);
        if (esc) {
            state.renaming = false;
            state.renameFocus = false;
            state.renameHadFocus = false;
            state.escapeConsumed = true;
        } else if (enter) {
            const std::string title(state.renameBuf.data());
            if (whitespaceOnly(title) || state.renameId.empty() || !state.canRename) {
                // Rejected in place: an empty title is never sent to D, and the box stays open
                // so a stray Enter does not throw the draft away.
            } else {
                state.renameCommitted = true;
                state.renamedTitle = title;
                state.renaming = false;
                state.renameFocus = false;
                state.renameHadFocus = false;
            }
        } else if (state.renameHadFocus && !active) {
            // The field had the keyboard and lost it (a click or a nav move landed elsewhere):
            // terminate like the rail's box does, *without* committing - an unfocused edit field
            // parked over the session title is not a state anyone asked for, and it is what would
            // let the box outlive the overlay that took the keyboard.
            state.renaming = false;
            state.renameFocus = false;
        }
        state.renameHadFocus = active;
    } else {
        line(m, titleRect, parts.titleText, ColorToken::textMid);
        if (titleRect.w > 0.0f && titleRect.h > 0.0f) {
            ImGui::SetCursorScreenPos(ImVec2(titleRect.x, titleRect.y));
            ImGui::InvisibleButton("##topbar_title", ImVec2(titleRect.w, titleRect.h));
            // A press opens the box for the session it belongs to, never for "whatever is active
            // later": the id is captured with the buffer. Gated on the same frame answer the Esc
            // is (`acceptKeys`): an overlay that owns the keyboard must not lose it to a click on
            // the bar underneath.
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && state.acceptKeys &&
                state.canRename && !state.sessionId.empty()) {
                state.renaming = true;
                state.renameId = state.sessionId;
                ++state.renameSeq;
                state.renameFocus = true;
                state.renameHadFocus = false;
                // A title that does not fit the buffer opens empty - no silent truncation,
                // exactly like the rail's rename box.
                const bool fits = state.sessionTitle.size() < state.renameBuf.size();
                std::snprintf(state.renameBuf.data(), state.renameBuf.size(), "%s",
                              fits ? state.sessionTitle.c_str() : "");
            }
            // The tooltip explains the two cases a user cannot see: a title too long for its
            // box, and a rename this run would refuse.
            if (!state.canRename)
                ui::tooltip(kBusyRenameHint);
            else if (parts.titleTruncated)
                ui::tooltip(state.sessionTitle);
        }
    }

    // The palette hint, the run pill, the `RAW` badge and the settings entry.
    line(m, rectOf(parts.hint, m), hintText(m), ColorToken::textLow);
    const PillLook look = pillLook(state.run);
    ui::pill(m, rectOf(parts.pill, m), look.mark, pillLabel(state.run, state.elapsedMs), look.ink,
             ColorToken::raised);
    if (!parts.badge.empty())
        ui::chip(m, rectOf(parts.badge, m), kRawBadge, ColorToken::stateWarn, ColorToken::raised);
    if (!parts.gear.empty()) {
        // The `Settings` entry is a real control now that the sheet exists (design §4.2: "gear
        // opens Settings"). It is drawn *enabled* whatever is on screen - the bar's appearance is a
        // function of its own inputs, so opening a panel cannot restyle the row behind it
        // (`./test_inspector_gui` asserts exactly that) - and the *click* is what `acceptKeys`
        // gates, like the title's: an overlay that owns the keyboard must not lose it to a click on
        // the bar underneath.
        const ui::ButtonSpec gearSpec{kSettingsLabel};
        ui::button(gearSpec, m, rectOf(parts.gear, m));
        if (ui::buttonPressed(gearSpec, m, rectOf(parts.gear, m)) && state.acceptKeys)
            state.settingsRequested = true;
    }
}

} // namespace llmfun::tui::views
