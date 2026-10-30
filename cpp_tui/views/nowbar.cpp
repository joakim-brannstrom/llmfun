/// The now bar (views/nowbar.h owns the contract and the anatomy).
///
/// What this file deliberately does not do:
///   * it does not read the aggregate, the display size or the profile flag - the row
///     is a geometry taker, so `./test_nowbar` can draw it into a cell grid with no
///     application state;
///   * it does not format a chip: the four values arrive as strings (`model::StatusChips`)
///     and are drawn verbatim, which is what keeps this row and the status sentence
///     from ever showing two different numbers for one status (§6.3);
///   * it does not write a glyph or a colour literal: marks come from `ui/icons.h` and
///     colours from `ui::color()`, like every other view (D-THEME).
///
/// Layering note (`tools/gate_ratchet.txt`): no profile branch of its own (`m.textGrid`
/// is data the metrics handed it), no hand-made colour, no v4 type and no display size,
/// so the file needs no ledger row.
#include "views/nowbar.h"

#include "imgui/imgui.h"

#include "ui/icons.h"
#include "ui/layout.h"
#include "ui/metrics.h"
#include "ui/primitives.h"
#include "ui/text.h"
#include "ui/tokens.h"
#include "views/chrome.h"   // kRawBadge
#include "views/feedback.h" // feedbackLook() - the flash's mark and ink
#include "views/topbar.h"   // elapsedLabel() - the one clock formatter

#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>
#include <vector>

namespace llmfun::tui::views {
namespace {

using ui::ColorToken;
using ui::Icon;

// ---------------------------------------------------------------------------
// Text helpers (the two every one-row view carries: one measure, one clipped line)
// ---------------------------------------------------------------------------

/// `text`'s width in the profile's own unit: cells on the grid (a wide glyph counts
/// two) and pixels in the pixel profile (the atlas).
///
/// `views/topbar.cpp`, `views/composer.cpp`, `views/rail.cpp` and `ui/primitives.cpp`
/// carry the same four lines: the pixel half needs `ImGui::CalcTextSize`, which
/// `ui/text.h` deliberately does not reach for, so the measure cannot live there and a
/// per-view copy is what that layering costs.
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

/// The icon set this profile draws: the grid's is the terminal's answer, the pixel
/// profile's the atlas' (`ui/icons.h`).
ui::GlyphSet glyphSetFor(const ui::Metrics& m) {
    return m.textGrid ? ui::textGlyphSet() : ui::pixelGlyphSet();
}

/// One line of text inside `box`, vertically centred and clipped to it.
///
/// It does *not* cut: every string this view draws was already cut to its part by
/// `nowBarMeasure()` (or is the state, which is never cut), so the clip only has to stop
/// a degenerate rectangle from painting over its neighbour - and a cell renderer clips
/// on cell boundaries, which is exactly the unit the measure used.
void line(const ui::Metrics& m, const ui::Rect& box, std::string_view text, ColorToken ink) {
    if (box.w <= 0.0f || box.h <= 0.0f || text.empty())
        return;
    const float lineH = ImGui::GetCurrentContext() != nullptr ? ImGui::GetTextLineHeight() : m.row;
    const ImVec2 pen(box.x, box.y + std::floor((box.h - lineH) * 0.5f));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 col = ImGui::GetColorU32(ui::color(ink));
    dl->PushClipRect(ImVec2(box.x, box.y), ImVec2(box.right(), box.bottom()), true);
    dl->AddText(pen, col, text.data(), text.data() + text.size());
    dl->PopClipRect();
}

/// A measured part as the rectangle the primitives take: the *unit* comes from the
/// metrics the row was measured with, stated once here instead of on every part.
ui::Rect rectOf(const NowBarPart& part, const ui::Metrics& m) {
    return ui::Rect{part.x, part.y, part.w, part.h, m.textGrid};
}

// ---------------------------------------------------------------------------
// The row's vocabulary
// ---------------------------------------------------------------------------

/// The join between two already-finished segments: ` | ` on the grid and the design's
/// ` · ` in the pixel profile.
///
/// U+00B7 MIDDLE DOT is East-Asian *Ambiguous* (one cell in imtui, two in a CJK
/// terminal), so the cell renderer gets the ASCII pipe - the status row's own split uses the same
/// punctuation, so the two rows that show the same facts read alike.
const char* separatorText(const ui::Metrics& m) { return m.textGrid ? " | " : " \u00b7 "; }

/// The "there is more below" mark (design §5.11's `↓ latest`).
///
/// Not the design's `↓`: `ui/icons.h` owns every state mark and the down arrow has no
/// row there, so a literal here would be the second home D-THEME forbids - and U+2193 is
/// East-Asian *Ambiguous*, which is why the grid would need an ASCII form of its own
/// anyway. `DisclosureOpen` is the table's "opens downward" mark: `▾` on a surface that
/// draws the glyphs, `v` on one that gets the ASCII column, and the word says the rest.
std::string latestHint(const ui::Metrics& m) {
    return std::string(ui::icon(Icon::DisclosureOpen, glyphSetFor(m))) + " latest";
}

/// One mark and one ink per phase (NFR-16: the word is always beside the mark, so the
/// colour is redundant with two other signals).
///
/// Idle reuses the top bar's idle pill (`Ok`/`textMid`) and a failed run its `failed`
/// pill (`Error`/`stateDanger`); the three working phases draw the live accent the design
/// gives a run (§4.2's running pill), a waiting one the amber the top bar uses for the
/// same fact (§4.8's `Pending` mark), and an unreachable model the danger token the
/// thread's offline card shows for it - the card and this row are never on screen with
/// two severities for one status.
struct Look {
    Icon mark = Icon::Ok;
    ColorToken ink = ColorToken::textMid;
};

Look lookFor(model::Phase phase) {
    switch (phase) {
    case model::Phase::Idle:
        return Look{Icon::Ok, ColorToken::textMid};
    case model::Phase::Thinking:
    case model::Phase::Tooling:
    case model::Phase::Writing:
        return Look{Icon::Running, ColorToken::accent};
    case model::Phase::Waiting:
        return Look{Icon::Pending, ColorToken::stateWarn};
    case model::Phase::Error:
        return Look{Icon::Error, ColorToken::stateDanger};
    case model::Phase::Offline:
        return Look{Icon::Warn, ColorToken::stateDanger};
    }
    return Look{};
}

/// A chip value as the surface can draw it.
///
/// The model's absence value is U+2014, which is East-Asian *Ambiguous*: imtui measures
/// it one cell and a CJK terminal may draw it two, so the grid gets the ASCII floor
/// (`-`). The *rule* is untouched - a missing number is never `0` (design §6.3) - only
/// its glyph is the profile's.
std::string chipValue(const ui::Metrics& m, const std::string& value) {
    if (value != model::kMissingChipValue)
        return value;
    return m.textGrid ? std::string("-") : value;
}

/// The compact context chip: the pair's *used* half (`42k/128k` → `42k`, the design's
/// compact form in §4.7).
///
/// Cut at the separator instead of reformatted from `contextUsed`: the number is already
/// formatted and this view must not own a second rounding of it, so the compact chip can
/// never disagree with the full one. A missing value has no separator and passes through
/// unchanged.
std::string compactContext(const std::string& context) {
    const std::size_t slash = context.find('/');
    return slash == std::string::npos ? context : context.substr(0, slash);
}

/// The flash as the row draws it: the severity's mark, one space, then the item's own words.
///
/// The words are the producer's (a path, a block's label, the reason a write failed) and this
/// row never rebuilds them - it only puts the mark in front, which is what makes the severity
/// readable on a surface that cannot show the ink (NFR-16: a state is a mark *and* a word).
std::string flashTextFor(const NowBarInput& input, const ui::Metrics& m) {
    if (input.flash.empty())
        return {};
    const FeedbackLook look = feedbackLook(input.flashKind);
    return std::string(ui::icon(look.mark, glyphSetFor(m))) + " " + input.flash;
}

/// The contextual hints, most contextual first, as *separate* parts.
///
/// The row draws the longest prefix that fits whole, so the hint that survives a narrow bar
/// is the one that applies now - and never a fragment: a hint cut mid-word ("Esc s…")
/// advertises nothing, and the dangling separator a cut list leaves behind advertises less.
///
/// The "there is more below" mark (§5.11) is a one-part list: the one thing the row must say
/// while the thread is not following is that the answer continued below the fold.
std::vector<std::string> hintPartsFor(const NowBarInput& input, const ui::Metrics& m) {
    std::vector<std::string> parts;
    if (!input.following) {
        parts.push_back(latestHint(m));
        return parts;
    }
    if (input.stopAvailable)
        parts.emplace_back(kNowBarHintStop);
    if (input.focusMode)
        parts.emplace_back(kNowBarHintLeaveFocus);
    parts.emplace_back(kNowBarHintPalette);
    return parts;
}

/// The hint list joined with this profile's separator, stopping before the first part that
/// does not fit whole. `dropped` says whether anything was left out.
std::string packHints(const std::vector<std::string>& parts, const ui::Metrics& m, float room,
                      bool& dropped) {
    std::string text;
    for (const std::string& part : parts) {
        std::string candidate = text;
        if (!candidate.empty())
            candidate += separatorText(m);
        candidate += part;
        if (textUnits(m, candidate) > room) {
            dropped = true;
            break;
        }
        text = candidate;
    }
    return text;
}

/// What the row says after the phase word: the current step and the run's clock.
///
/// `running` decides whether the clock is part of the row at all (idle has none: a time
/// beside `Ready` would claim a run, the top bar's pill rule), `clock` what it reads.
/// `elapsedLabel()` is the top bar's formatter (and is called once per frame, by the
/// caller), so two clocks in one window cannot round or wrap differently.
///
/// `withDetail` is the degradation knob: the detail is the first thing dropped when the
/// row is narrow (the clock is one short number, a tool name is a word of unknown length).
std::string tailFor(const NowBarInput& input, const ui::Metrics& m, const std::string& clock,
                    bool withDetail) {
    std::string text;
    if (withDetail && !input.detail.empty()) {
        text += separatorText(m);
        text += input.detail;
    }
    if (!clock.empty()) {
        text += separatorText(m);
        text += clock;
    }
    return text;
}

/// The chip strip's width for a full four-chip form.
float fullChipsWidth(const ui::Metrics& m, const std::string& ctx, const std::string& speed,
                     const std::string& model, const std::string& state) {
    const float gap = m.gu;
    return ui::measureChip(m, ctx).w + gap + ui::measureChip(m, speed).w + gap +
           ui::measureChip(m, model).w + gap + ui::measureChip(m, state).w;
}

} // namespace

NowBarParts nowBarMeasure(const NowBarInput& input, const ui::Rect& bar, const ui::Metrics& m) {
    NowBarParts parts;
    // Fail closed: a zero area or a rectangle in the other profile's unit paints nothing
    // rather than a plausible row in the wrong coordinate space.
    if (bar.w <= 0.0f || bar.h <= 0.0f || !ui::rectMatchesProfile(bar, m))
        return parts;

    // The row draws at the meta step (design §4.6's type table), so it measures at it too
    // - a measure taken outside a draw answers the numbers the drawn row uses. A no-op on
    // the text grid, where a cell has one size.
    const ui::TypeScope metaType(ui::TypeStep::meta, m.textGrid);

    const float pad = m.pad1;
    const float rowY = bar.y + std::floor((bar.h - m.row) * 0.5f);

    // --- the left side: the rule, the mark, the state -----------------------
    // The rule is one cell on the grid and the design's 2 px in the pixel profile: a cell
    // cannot be divided, and two hairlines is the narrowest pixel rule that reads beside a
    // 24-px row (the composer's focus ring is the same pair).
    const float ruleW = m.textGrid ? m.border : 2.0f * m.border;
    parts.rule = NowBarPart{bar.x, bar.y, ruleW, bar.h};

    float x = bar.x + ruleW + pad;
    const ui::Size markSize = ui::measureIconMark(m);
    parts.mark = NowBarPart{x, rowY, markSize.w, m.row};
    x = parts.mark.right() + pad;

    // The state is reserved first and never cut: it is the row's reason to exist (design
    // §4.7, "truncate hints first, never the state").
    parts.stateText = model::phaseWord(input.phase);
    parts.state = NowBarPart{x, rowY, textUnits(m, parts.stateText), m.row};
    const float leftEnd = parts.state.right();

    // --- the right side: the badge, then the chips --------------------------
    // The badge belongs to a folded top bar: in the full tiling the bar carries it, and a
    // second copy of one signal on one screen is noise. Where the bar is folded away - compact
    // mode, or focus mode (task 43) - this row is the only place it can be.
    float right = bar.right() - pad;
    if ((input.compact || input.topBarFolded) && input.rawLogOpen) {
        const float badgeW = ui::measureChip(m, kRawBadge).w;
        parts.badge = NowBarPart{right - badgeW, rowY, badgeW, m.row};
        right = parts.badge.x - pad;
    }

    // The chip values. All four arrive formatted (task 6, and task 39 for the structured
    // path); everything this file does with them is a join.
    const std::string ctx = std::string(kNowBarContextLabel) + chipValue(m, input.chips.context);
    const std::string ctxCompact =
        std::string(kNowBarContextLabel) + chipValue(m, compactContext(input.chips.context));
    const std::string speed = chipValue(m, input.chips.speed);
    const std::string model = chipValue(m, input.chips.model);
    const std::string state = chipValue(m, input.chips.state);

    const float gap = m.gu;
    const float markW = ui::measureIconMark(m).w;
    const float fullW = fullChipsWidth(m, ctx, speed, model, state);
    const float compactW = ui::measureChip(m, ctxCompact).w + gap + markW;

    // The ladder (design §4.7): compact mode collapses the chips to `ctx 42k` + the state
    // mark, and a row that cannot fit four chips takes the same form - a chip drawn
    // half-length is a number nobody can read. Nothing fits: the state mark alone, which
    // is the chip the design never drops.
    //
    // An open question compresses the chips the same way: the question is the one thing
    // the row is waiting for an answer to, and the state is what must stay beside it
    // (task 11's note) - so the three metric chips yield before the question is cut.
    const float roomForChips = right - leftEnd - pad;
    const bool wantCompact = input.compact || !input.prompt.empty() || fullW > roomForChips;
    parts.compactChips = wantCompact;
    if (!wantCompact) {
        const float stateW = ui::measureChip(m, state).w;
        const float modelW = ui::measureChip(m, model).w;
        const float speedW = ui::measureChip(m, speed).w;
        const float ctxW = ui::measureChip(m, ctx).w;
        float left = right - stateW;
        parts.chipState = NowBarPart{left, rowY, stateW, m.row};
        parts.chipStateText = state;
        left -= gap + modelW;
        parts.chipModel = NowBarPart{left, rowY, modelW, m.row};
        parts.chipModelText = model;
        left -= gap + speedW;
        parts.chipSpeed = NowBarPart{left, rowY, speedW, m.row};
        parts.chipSpeedText = speed;
        left -= gap + ctxW;
        parts.chipCtx = NowBarPart{left, rowY, ctxW, m.row};
        parts.chipCtxText = ctx;
    } else if (compactW <= roomForChips) {
        const float ctxW = ui::measureChip(m, ctxCompact).w;
        parts.chipCtx = NowBarPart{right - ctxW - gap - markW, rowY, ctxW, m.row};
        parts.chipCtxText = ctxCompact;
        parts.chipState = NowBarPart{right - markW, rowY, markW, m.row};
    } else if (markW <= roomForChips) {
        parts.chipState = NowBarPart{right - markW, rowY, markW, m.row};
    }

    // --- the middle: the step and the clock, then the hints -----------------
    float chipsStart = !parts.chipCtx.empty()     ? parts.chipCtx.x
                       : !parts.chipState.empty() ? parts.chipState.x
                                                  : right;

    // The clock is the one number this row formats, and it is formatted once per frame
    // (both forms of the tail read it), so a second `snprintf` per frame cannot creep back
    // in with the degradation step below.
    const std::string clock = input.running ? elapsedLabel(input.elapsedMs) : std::string();

    // The tail takes what it needs from the left; the detail yields first, because a clock
    // is a fixed short number and a tool name is not. An open question owns the middle
    // instead: it gets the room the step and the clock were using rather than being cut to
    // a fragment of itself.
    const float middleRoom = chipsStart - pad - (leftEnd + pad);
    parts.tailText =
        input.prompt.empty() ? tailFor(input, m, clock, /*withDetail=*/true) : std::string();
    if (!parts.tailText.empty() && textUnits(m, parts.tailText) > middleRoom) {
        parts.tailText = tailFor(input, m, clock, /*withDetail=*/false);
        if (textUnits(m, parts.tailText) > middleRoom)
            parts.tailText.clear();
    }
    const float tailW = textUnits(m, parts.tailText);
    if (tailW > 0.0f)
        parts.tail = NowBarPart{leftEnd, rowY, tailW, m.row};

    // The right side's second claim, right-aligned against the chips (the design's right
    // cluster is "hints, then the chips"; the top bar's hint lands directly left of its pill
    // for the same reason, so the row does not open a gap in the middle).
    //
    // It is a question when one is open, and then it is *never* a fragment: the prompt's
    // tail is the instruction ("press Ctrl+Q again"), so cutting it would leave the one
    // string on this row that a user has to act on saying nothing. The ladder is
    //   full prompt -> short form -> the metric chips' room -> (below the floor) a cut.
    const float hintsStart = leftEnd + pad + tailW;
    std::string rightText;
    if (!input.prompt.empty()) {
        float room = chipsStart - pad - hintsStart;
        if (textUnits(m, input.prompt) <= room) {
            rightText = input.prompt;
        } else if (!input.promptShort.empty() && textUnits(m, input.promptShort) <= room) {
            rightText = input.promptShort;
        } else {
            // Even the shortest form needs the metric chips' room. They step aside whole -
            // and the state keeps its word, its mark and its rule, so the row still says
            // what the run is doing while it waits for the answer.
            parts.chipCtx = NowBarPart{};
            parts.chipCtxText.clear();
            parts.chipSpeed = NowBarPart{};
            parts.chipSpeedText.clear();
            parts.chipModel = NowBarPart{};
            parts.chipModelText.clear();
            parts.chipState = NowBarPart{};
            parts.chipStateText.clear();
            parts.compactChips = true;
            chipsStart = right;
            room = chipsStart - pad - hintsStart;
            const std::string& shortest =
                input.promptShort.empty() ? input.prompt : input.promptShort;
            parts.hintsTruncated = textUnits(m, shortest) > room;
            // The grid cuts in cells and marks the cut; the pixel profile draws the whole
            // string and its clip rect cuts it (the top bar's title rule) - `room` is in
            // pixels there, so it cannot be handed to the cell-based `truncate()`.
            rightText = parts.hintsTruncated && m.textGrid
                            ? ui::truncate(shortest, static_cast<int>(std::max(0.0f, room)))
                            : shortest;
        }
    } else {
        // The flash (task 45) owns the hint slot while it is up: it is the answer to a press the
        // user made, and the hints advertise presses. Two forms, longest first - the flash with the
        // key that clears a persistent item, then the flash alone - and both drawn *whole*: a path
        // cut in half is a path nobody can find, and a cut warning is one nobody can act on.
        const float room = chipsStart - pad - hintsStart;
        const std::string flashText = flashTextFor(input, m);
        std::string flashWithKey;
        if (!flashText.empty() && model::toastKindPersists(input.flashKind))
            flashWithKey = flashText + std::string(separatorText(m)) + kNowBarFlashDismiss;
        if (!flashWithKey.empty() && textUnits(m, flashWithKey) <= room) {
            rightText = flashWithKey;
            parts.flashDrawn = true;
        } else if (!flashText.empty() && textUnits(m, flashText) <= room) {
            rightText = flashText;
            parts.flashDrawn = true;
        } else {
            // The hint list: the longest prefix that fits whole, and nothing below
            // `kNowBarMinHintGu` (a fragment of a word advertises nothing).
            const std::vector<std::string> hintParts = hintPartsFor(input, m);
            if (room >= static_cast<float>(kNowBarMinHintGu) * m.gu)
                rightText = packHints(hintParts, m, room, parts.hintsTruncated);
        }
    }

    if (!rightText.empty()) {
        // The box is the text, clamped to the room the row actually has: a clip that had to
        // cut (the pixel profile's last resort) cuts at the right edge the row can afford
        // instead of over its neighbour.
        const float rightW = textUnits(m, rightText);
        const float boxW = std::min(rightW, std::max(0.0f, chipsStart - pad - hintsStart));
        parts.hintsText = rightText;
        parts.hints = NowBarPart{chipsStart - pad - boxW, rowY, boxW, m.row};
    }
    return parts;
}

void renderNowBar(const NowBarInput& input, const ui::Rect& bar, const ui::Metrics& m) {
    if (bar.w <= 0.0f || bar.h <= 0.0f || !ui::rectMatchesProfile(bar, m))
        return;

    const ui::TypeScope metaType(ui::TypeStep::meta, m.textGrid);
    const NowBarParts parts = nowBarMeasure(input, bar, m);
    const Look look = lookFor(input.phase);

    // Region chrome. A one-cell bar *is* one cell, so the grid keeps the terminal's own
    // background, and the pixel profile draws the design's `surface` plus the hairline
    // that separates this row from the composer above it.
    if (!m.textGrid) {
        ui::rule(m, bar, ColorToken::surface);
        ui::Rect edge = bar;
        edge.h = m.border;
        ui::rule(m, edge, ColorToken::lineSoft);
    }

    // The one colour the row spends on a background is the state rule at the far left
    // (design §4.7), drawn in the phase's ink so a run and a failure read before any word
    // does.
    ui::rule(m, rectOf(parts.rule, m), look.ink);
    ui::iconMark(m, rectOf(parts.mark, m), look.mark, look.ink);
    line(m, rectOf(parts.state, m), parts.stateText, look.ink);
    line(m, rectOf(parts.tail, m), parts.tailText, ColorToken::textMid);
    // The hints are the dimmest ink that still clears the text bar - except while the
    // thread is not following (then the one thing the row says is actionable and it wears
    // the accent, design §5.11), and except while a question is up (then the row is waiting
    // for the user and says so in the warning ink). A flash (task 45) is an answer to a press,
    // not a hint: it keeps the severity's ink, so a saved path never reads as a key and a failed
    // save reads like the failure it is.
    ColorToken hintInk = input.following ? ColorToken::textLow : ColorToken::accent;
    if (!input.prompt.empty())
        hintInk = ColorToken::stateWarn;
    else if (parts.flashDrawn)
        hintInk = feedbackLook(input.flashKind).ink;
    line(m, rectOf(parts.hints, m), parts.hintsText, hintInk);

    // The canonical chips: the numbers on a raised surface in the readable ink, the state
    // chip in the phase's own ink (the v4 status row's rule, one step brighter because
    // on this row the numbers are the contract rather than a footnote).
    if (!parts.chipCtx.empty())
        ui::chip(m, rectOf(parts.chipCtx, m), parts.chipCtxText, ColorToken::textMid,
                 ColorToken::raised);
    if (!parts.chipSpeed.empty())
        ui::chip(m, rectOf(parts.chipSpeed, m), parts.chipSpeedText, ColorToken::textMid,
                 ColorToken::raised);
    if (!parts.chipModel.empty())
        ui::chip(m, rectOf(parts.chipModel, m), parts.chipModelText, ColorToken::textMid,
                 ColorToken::raised);
    if (!parts.chipState.empty()) {
        if (parts.chipStateText.empty()) {
            // Compact: the state as its mark, beside the one chip that fits.
            ui::iconMark(m, rectOf(parts.chipState, m), look.mark, look.ink);
        } else {
            ui::chip(m, rectOf(parts.chipState, m), parts.chipStateText, look.ink,
                     ColorToken::raised);
        }
    }

    // The `RAW` badge: the developer log is open, and the mark never drops while it is.
    if (!parts.badge.empty())
        ui::chip(m, rectOf(parts.badge, m), kRawBadge, ColorToken::stateWarn, ColorToken::raised);
}

} // namespace llmfun::tui::views
