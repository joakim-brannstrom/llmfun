/// The now bar (views/nowbar.h owns the contract and the anatomy).
///
/// What this file deliberately does not do:
///   * it does not read the aggregate, the display size or the profile flag - the row
///     is a geometry taker, so `./test_nowbar` can draw it into a cell grid with no
///     application state;
///   * it does not format a chip: the values arrive as strings (`model::StatusChips`)
///     and are drawn verbatim, which is what keeps this row and the status sentence
///     from ever showing two different numbers for one status;
///   * it does not write a glyph or a colour literal: marks come from `ui/icons.h` and
///     colours from `ui::color()`, like every other view.
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
#include "views/composer.h" // kMeterSegmentsGrid, meterLevel(), contextFraction() - the
                            // meter's own vocabulary
#include "views/feedback.h" // feedbackLook() - the flash's mark and ink
#include "views/topbar.h"   // elapsedLabel() - the one clock formatter

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace llmfun::tui::views {
namespace {

using ui::ColorToken;
using ui::Icon;

// Text helpers (the two every one-row view carries: one measure, one clipped line)

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

// The row's vocabulary

/// The join between two already-finished segments: ` | ` on the grid and the design's
/// ` · ` in the pixel profile.
///
/// U+00B7 MIDDLE DOT is East-Asian *Ambiguous* (one cell in imtui, two in a CJK
/// terminal), so the cell renderer gets the ASCII pipe - the status row's own split uses the same
/// punctuation, so the two rows that show the same facts read alike.
const char* separatorText(const ui::Metrics& m) { return m.textGrid ? " | " : " \u00b7 "; }

/// The "there is more below" mark.
///
/// Not the design's `↓`: `ui/icons.h` owns every state mark and the down arrow has no
/// row there, so a literal here would be a second home for a mark - and U+2193 is
/// East-Asian *Ambiguous*, which is why the grid would need an ASCII form of its own
/// anyway. `DisclosureOpen` is the table's "opens downward" mark: `▾` on a surface that
/// draws the glyphs, `v` on one that gets the ASCII column, and the word says the rest.
std::string latestHint(const ui::Metrics& m) {
    return std::string(ui::icon(Icon::DisclosureOpen, glyphSetFor(m))) + " latest";
}

/// One mark and one ink per phase: the word is always beside the mark, so the
/// colour is redundant with two other signals.
///
/// Idle draws `Ok`/`textMid` and a failed run `Error`/`stateDanger`; the three working
/// phases draw the live accent of a run in flight, a waiting one the amber the top bar
/// uses for the same fact, and an unreachable model the danger token the
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
/// (`-`). The *rule* is untouched - a missing number is never `0` - only
/// its glyph is the profile's.
std::string chipValue(const ui::Metrics& m, const std::string& value) {
    if (value != model::kMissingChipValue)
        return value;
    return m.textGrid ? std::string("-") : value;
}

/// The compact context chip: the pair's *used* half (`42k/128k` → `42k`, the design's
/// compact form).
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
/// readable on a surface that cannot show the ink (a state is a mark *and* a word).
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
/// The "there is more below" mark is a one-part list: the one thing the row must say
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
/// beside `Ready` would claim a run), `clock` what it reads.
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

/// The context meter's ink: the thresholds are `meterLevel()`'s, the colour is this
/// row's choice, and both profiles draw the same rule.
ColorToken meterInk(std::int64_t used, std::int64_t max) {
    switch (meterLevel(used, max)) {
    case MeterLevel::Normal:
        return ColorToken::accent;
    case MeterLevel::Warn:
        return ColorToken::stateWarn;
    case MeterLevel::Danger:
        return ColorToken::stateDanger;
    }
    return ColorToken::accent;
}

} // namespace

NowBarParts nowBarMeasure(const NowBarInput& input, const ui::Rect& bar, const ui::Metrics& m) {
    NowBarParts parts;
    // Fail closed: a zero area or a rectangle in the other profile's unit paints nothing
    // rather than a plausible row in the wrong coordinate space.
    if (bar.w <= 0.0f || bar.h <= 0.0f || !ui::rectMatchesProfile(bar, m))
        return parts;

    // The row draws at the meta step, so it measures at it too
    // - a measure taken outside a draw answers the numbers the drawn row uses. A no-op on
    // the text grid, where a cell has one size.
    const ui::TypeScope metaType(ui::TypeStep::meta, m.textGrid);

    const float pad = m.pad1;
    const float rowY = bar.y + std::floor((bar.h - m.row) * 0.5f);

    // the left side: the rule, the mark, the state
    // The rule is one cell on the grid and the design's 2 px in the pixel profile: a cell
    // cannot be divided, and two hairlines is the narrowest pixel rule that reads beside a
    // 24-px row (the composer's focus ring is the same pair).
    const float ruleW = m.textGrid ? m.border : 2.0f * m.border;
    parts.rule = NowBarPart{bar.x, bar.y, ruleW, bar.h};

    float x = bar.x + ruleW + pad;
    const ui::Size markSize = ui::measureIconMark(m);
    parts.mark = NowBarPart{x, rowY, markSize.w, m.row};
    x = parts.mark.right() + pad;

    // The state is reserved first and never cut: it is the row's reason to exist
    // ("truncate hints first, never the state").
    parts.stateText = model::phaseWord(input.phase);
    parts.state = NowBarPart{x, rowY, textUnits(m, parts.stateText), m.row};
    const float leftEnd = parts.state.right();

    // the right side: the badge, then the chips
    // The badge belongs to a folded top bar: in the full tiling the bar carries it, and a
    // second copy of one signal on one screen is noise. Where the bar is folded away - compact
    // mode, or focus mode - this row is the only place it can be.
    float right = bar.right() - pad;
    if ((input.compact || input.topBarFolded) && input.rawLogOpen) {
        const float badgeW = ui::measureChip(m, kRawBadge).w;
        parts.badge = NowBarPart{right - badgeW, rowY, badgeW, m.row};
        right = parts.badge.x - pad;
    }

    // The right side's values. They arrive formatted, and everything this file does with
    // them is a join or a cut at the separator.
    const std::string countFull = chipValue(m, input.chips.context);
    const std::string countCompact = chipValue(m, compactContext(input.chips.context));
    const bool modelKnown =
        !input.chips.model.empty() && input.chips.model != model::kMissingChipValue;
    const std::string model = chipValue(m, input.chips.model);
    const bool speedKnown =
        !input.chips.speed.empty() && input.chips.speed != model::kMissingChipValue;
    const std::string speed = chipValue(m, input.chips.speed);

    const float gap = m.gu;
    const float labelW = textUnits(m, kNowBarContextLabel);
    const int segments = m.textGrid ? kMeterSegmentsGrid : kNowBarMeterSegmentsGui;
    const ui::Size barSize = ui::measureMeter(m, segments);
    const float countFullW = textUnits(m, countFull);
    const float countCompactW = textUnits(m, countCompact);
    const float modelW = modelKnown ? ui::measureChip(m, model).w : 0.0f;
    const float modelChunk = modelKnown ? gap + modelW : 0.0f;
    const float speedW = speedKnown ? ui::measureChip(m, speed).w : 0.0f;
    const float speedChunk = speedKnown ? gap + speedW : 0.0f;

    // The clock is the one number this row formats, and it is formatted once per frame
    // (both forms of the tail read it), so a second `snprintf` per frame cannot creep back
    // in with the degradation step below.
    const std::string clock = input.running ? elapsedLabel(input.elapsedMs) : std::string();
    const float clockW = textUnits(m, clock);

    // The ladder: the hints yield first, then the speed chip, then the count
    // compacts (`42k/128k` -> `42k`), then the model chip yields, then the meter's bar -
    // the `ctx` label and the count are the last right-side part standing. When even
    // that does not fit, the row says nothing on the right rather than a fragment of
    // a number. The pixel profile also drops the bar below the full-meter breakpoint
    // (`kComposerFullMeterGu`): a squeezed bar is number-shaped noise.
    const bool fullBar = m.textGrid || bar.w >= static_cast<float>(kComposerFullMeterGu) * m.gu;

    // A bar needs a fraction to fill, and an unknown context has none: its forms draw the
    // count alone, a missing capacity drops the bar and keeps `ctx -`, so the absence
    // glyph lands beside the label instead of after six empty segments.
    const bool contextKnown =
        !input.chips.context.empty() && input.chips.context != model::kMissingChipValue;

    // Compact mode starts one rung down - the count is the compact form from the start
    // (compact mode is asked for below the breakpoint, not earned cell by
    // cell). An open question is not compact mode: it yields the meter rung by rung below.
    const bool startCompact = input.compact;
    struct Form {
        bool bar;
        bool compact;
        bool speed;
        bool model;
        float w;
    };
    auto forms = std::array<Form, 5>{{
        {true, false, true, true,
         labelW + m.pad1 + barSize.w + m.pad1 + countFullW + speedChunk + modelChunk},
        {true, false, false, true, labelW + m.pad1 + barSize.w + m.pad1 + countFullW + modelChunk},
        {true, true, false, true,
         labelW + m.pad1 + barSize.w + m.pad1 + countCompactW + modelChunk},
        {true, true, false, false, labelW + m.pad1 + barSize.w + m.pad1 + countCompactW},
        {false, true, false, false, labelW + m.pad1 + countCompactW},
    }};
    if (!contextKnown) {
        for (Form& form : forms) {
            if (!form.bar)
                continue;
            form.bar = false;
            form.w -= barSize.w + m.pad1;
        }
    }

    // The right cluster is placed from the right edge inwards, so a form's width is the
    // distance from `right` to the `ctx` label - the left edge the middle's tenant (the
    // tail, the hints, the question) measures its room against.
    const auto chipsStartFor = [&](const Form& form) { return right - form.w; };

    // An open question is never a fragment, so the meter yields rung by rung to keep a slot
    // for it: the fullest form that still leaves room for the question's full text, else the
    // fullest form that leaves room for its short form. The walk stops above the bare-count
    // floor: a meter reduced to `ctx 42k` is the narrow-row form, and below that the
    // question owns the row instead (the floor's behaviour at `40 cells`).
    const bool hasPrompt = !input.prompt.empty();
    const std::string& shortPrompt = input.promptShort.empty() ? input.prompt : input.promptShort;
    int promptChoice = 0; // 0 no host, 1 the full prompt, 2 the short form
    auto chosen = forms.end();
    if (hasPrompt) {
        const float hintsStart = leftEnd + pad; // the tail is empty while a question is up
        const float fullW = textUnits(m, input.prompt);
        const float shortW = textUnits(m, shortPrompt);
        const auto last = std::prev(forms.end()); // the bare-count floor never hosts the question
        for (auto it = fullBar ? forms.begin() : last; it != last; ++it) {
            if (startCompact && !it->compact)
                continue; // the full count is not this frame's form
            const float room = chipsStartFor(*it) - pad - hintsStart;
            if (fullW <= room) {
                chosen = it;
                promptChoice = 1;
                break;
            }
            if (shortW <= room) {
                chosen = it;
                promptChoice = 2;
                break;
            }
        }
    } else {
        // The clock keeps its slot against the speed chip: the speed rung is admitted
        // only if the clock still fits beside it, so the rate can only take width the
        // clock is not using. The narrower rungs keep the ladder - the clock yields
        // before the count compacts.
        const float clockChunk = clockW > 0.0f ? clockW + m.pad1 : 0.0f;
        const float roomForMeter = right - leftEnd - pad;
        const auto floor = std::prev(forms.end());
        for (auto it = fullBar ? forms.begin() : floor; it != forms.end(); ++it) {
            if (startCompact && !it->compact)
                continue; // the full count is not this frame's form
            const float w = it->w + (it->speed ? clockChunk : 0.0f);
            if (w <= roomForMeter) {
                chosen = it;
                break;
            }
        }
    }
    parts.compactChips = chosen != forms.begin();
    if (chosen != forms.end()) {
        const Form& form = *chosen;
        const std::string& count = form.compact ? countCompact : countFull;
        const float countW = form.compact ? countCompactW : countFullW;
        // Placed from the right edge inwards: the model chip is the row's rightmost part,
        // then the speed chip, then the meter reads left to right - label, bar, count.
        float x = right;
        if (form.model) {
            parts.chipModel = NowBarPart{x - modelW, rowY, modelW, m.row};
            parts.chipModelText = model;
            x -= modelChunk;
        }
        if (form.speed) {
            parts.chipSpeed = NowBarPart{x - speedW, rowY, speedW, m.row};
            parts.chipSpeedText = speed;
            x -= speedChunk;
        }
        parts.meterCount = NowBarPart{x - countW, rowY, countW, m.row};
        parts.meterCountText = count;
        x -= countW + m.pad1;
        if (form.bar) {
            parts.meterBar = NowBarPart{
                x - barSize.w, rowY + std::floor((m.row - barSize.h) * 0.5f), barSize.w, barSize.h};
            x -= barSize.w + m.pad1;
        }
        parts.meterLabel = NowBarPart{x - labelW, rowY, labelW, m.row};
        parts.meterLabelText = kNowBarContextLabel;
    }

    // the middle: the step and the clock, then the hints
    float chipsStart = !parts.meterLabel.empty() ? parts.meterLabel.x : right;

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
    // cluster is "hints, then the chips"; the top bar's hint lands directly left of its
    // `RAW` badge for the same reason, so the row does not open a gap in the middle).
    //
    // It is a question when one is open, and then it is *never* a fragment: the prompt's
    // tail is the instruction ("press Ctrl+Q again"), so cutting it would leave the one
    // string on this row that a user has to act on saying nothing. The ladder is
    //   full prompt -> short form -> the metric chips' room -> (below the floor) a cut.
    const float hintsStart = leftEnd + pad + tailW;
    std::string rightText;
    if (!input.prompt.empty()) {
        if (promptChoice != 0) {
            // The form walk hosts the question beside the meter, and the form it picked
            // decided which form the question takes (full where it fits, short below that).
            rightText = promptChoice == 1 ? input.prompt : shortPrompt;
        } else {
            // Even the shortest form needs the meter's room. It steps aside whole -
            // and the state keeps its word, its mark and its rule, so the row still says
            // what the run is doing while it waits for the answer.
            const float room = chipsStart - pad - hintsStart;
            parts.hintsTruncated = textUnits(m, shortPrompt) > room;
            // The grid cuts in cells and marks the cut; the pixel profile draws the whole
            // string and its clip rect cuts it (the top bar's title rule) - `room` is in
            // pixels there, so it cannot be handed to the cell-based `truncate()`.
            rightText = parts.hintsTruncated && m.textGrid
                            ? ui::truncate(shortPrompt, static_cast<int>(std::max(0.0f, room)))
                            : shortPrompt;
        }
    } else {
        // The flash owns the hint slot while it is up: it is the answer to a press the
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
            else
                // Below the floor the whole list is dropped - and "the right side is not
                // the full list" is exactly what happened, so the flag says so.
                parts.hintsTruncated = !hintParts.empty();
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

    // The one colour the row spends on a background is the state rule at the far left,
    // drawn in the phase's ink so a run and a failure read before any word
    // does.
    ui::rule(m, rectOf(parts.rule, m), look.ink);
    ui::iconMark(m, rectOf(parts.mark, m), look.mark, look.ink);
    line(m, rectOf(parts.state, m), parts.stateText, look.ink);
    line(m, rectOf(parts.tail, m), parts.tailText, ColorToken::textMid);
    // The hints are the dimmest ink that still clears the text bar - except while the
    // thread is not following (then the one thing the row says is actionable and it wears
    // the accent), and except while a question is up (then the row is waiting
    // for the user and says so in the warning ink). A flash is an answer to a press,
    // not a hint: it keeps the severity's ink, so a saved path never reads as a key and a failed
    // save reads like the failure it is.
    ColorToken hintInk = input.following ? ColorToken::textLow : ColorToken::accent;
    if (!input.prompt.empty())
        hintInk = ColorToken::stateWarn;
    else if (parts.flashDrawn)
        hintInk = feedbackLook(input.flashKind).ink;
    line(m, rectOf(parts.hints, m), parts.hintsText, hintInk);

    // The right side: the context meter (the numbers the row exists to keep visible), the
    // speed chip and the model chip on a raised surface. The meter's bar and count read in
    // the readable ink and the label in the dimmest ink
    // that still clears the text bar; the bar never carries a surface (it is a rule, not
    // a chip).
    if (!parts.meterBar.empty()) {
        const int segments = m.textGrid ? kMeterSegmentsGrid : kNowBarMeterSegmentsGui;
        const ui::Rect bar = rectOf(parts.meterBar, m);
        ui::meter(m, bar, segments,
                  contextFraction(input.chips.contextUsed, input.chips.contextMax),
                  meterInk(input.chips.contextUsed, input.chips.contextMax));
        // The compaction threshold, when the app knows one (`NowBarInput::compactAt`):
        // a one-unit tick across the bar's own row. The pixel profile only: the text
        // grid's atom is the cell, and a threshold between cells cannot be drawn there
        // without painting over the bar's own glyphs - on the grid the fill already
        // carries the fraction.
        if (!m.textGrid && input.compactAt > 0 && input.chips.contextMax > 0) {
            const float tickX =
                bar.x +
                std::floor(contextFraction(input.compactAt, input.chips.contextMax) * bar.w);
            ImDrawList* tickList = ImGui::GetWindowDrawList();
            tickList->AddRectFilled(
                ImVec2(std::max(bar.x, tickX - m.border * 0.5f), bar.y),
                ImVec2(std::max(bar.x, tickX - m.border * 0.5f) + m.border, bar.y + bar.h),
                ImGui::GetColorU32(ui::color(ColorToken::textHi)));
        }
    }
    if (!parts.meterLabel.empty())
        line(m, rectOf(parts.meterLabel, m), parts.meterLabelText, ColorToken::textLow);
    if (!parts.meterCount.empty())
        line(m, rectOf(parts.meterCount, m), parts.meterCountText, ColorToken::textLow);
    if (!parts.chipSpeed.empty())
        ui::chip(m, rectOf(parts.chipSpeed, m), parts.chipSpeedText, ColorToken::textMid,
                 ColorToken::raised);
    if (!parts.chipModel.empty())
        ui::chip(m, rectOf(parts.chipModel, m), parts.chipModelText, ColorToken::textMid,
                 ColorToken::raised);

    // The `RAW` badge: the developer log is open, and the mark never drops while it is.
    if (!parts.badge.empty())
        ui::chip(m, rectOf(parts.badge, m), kRawBadge, ColorToken::stateWarn, ColorToken::raised);
}

} // namespace llmfun::tui::views
