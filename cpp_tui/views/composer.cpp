/// The composer (views/composer.h owns the contract and the anatomy).
///
/// One container, one multi-line field that grows with its draft, one meta row and
/// one action. What the file deliberately does *not* do: own the draft, own the
/// history list, or send. Those are the shipped store (`UserQueryState`) and the
/// frame layer's answers, so the field and the store cannot disagree about what a
/// send is.
#include "views/composer.h"

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"

#include "model/keymap.h"
#include "model/status_chips.h"
#include "model/thread_view.h"

#include "ui/icons.h"
#include "ui/layout.h"
#include "ui/metrics.h"
#include "ui/primitives.h"
#include "ui/text.h"
#include "ui/tokens.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>
#include <vector>

namespace llmfun::tui::views {

namespace {

using ui::ColorToken;

// ---------------------------------------------------------------------------
// Text helpers (the top bar's two: one measure, one clipped line)
// ---------------------------------------------------------------------------

/// `text`'s width in the profile's own unit: cells on the grid (a wide glyph counts
/// two) and pixels in the pixel profile (the atlas).
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

/// One line of text inside `box`, left-aligned at `x` and clipped to the box: the grid
/// cuts in cells with the profile's own ellipsis (a cell cannot be half drawn), the
/// pixel profile draws the whole line and the clip cuts it.
void lineAt(const ui::Metrics& m, const ui::Rect& box, float x, float y, std::string_view text,
            ColorToken ink) {
    if (box.w <= 0.0f || box.h <= 0.0f || text.empty() || x >= box.right() ||
        y + m.row > box.bottom() + 0.01f)
        return;
    const int room = static_cast<int>(std::max(0.0f, box.right() - x));
    if (room <= 0)
        return;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 col = ImGui::GetColorU32(ui::color(ink));
    dl->PushClipRect(ImVec2(box.x, box.y), ImVec2(box.right(), box.bottom()), true);
    if (m.textGrid) {
        const std::string fitted = ui::truncate(text, room);
        dl->AddText(ImVec2(x, y), col, fitted.c_str(), fitted.c_str() + fitted.size());
    } else {
        dl->AddText(ImVec2(x, y), col, text.data(), text.data() + text.size());
    }
    dl->PopClipRect();
}

/// The same line, right-aligned against `right` and clipped to `box`.
void lineRight(const ui::Metrics& m, const ui::Rect& box, float right, float y,
               std::string_view text, ColorToken ink) {
    lineAt(m, box, right - textUnits(m, text), y, text, ink);
}

/// A fill is a *background* in a cell renderer, so the glyphs of the frame before
/// would show through it. One run of spaces per row is what an opaque fill means on
/// the text grid (the shell's stub regions and the rail do the same).
void blankCells(const ui::Metrics& m, const ui::Rect& r, ColorToken fill) {
    if (!m.textGrid || m.row <= 0.0f)
        return;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const std::string blank(static_cast<std::size_t>(std::max(0.0f, r.w)), ' ');
    const ImU32 col = ImGui::GetColorU32(ui::color(fill));
    for (float row = r.y; row < r.bottom(); row += m.row)
        dl->AddText(ImVec2(r.x, row), col, blank.c_str());
}

/// Four filled bars one hairline thick inside `r` - the ring `ui::panel()` cannot draw
/// (its hairline is the `lineStrong` boundary; a focus ring is a different token).
void ringInside(const ui::Rect& r, ColorToken token, float thickness) {
    if (r.w <= 0.0f || r.h <= 0.0f || thickness <= 0.0f)
        return;
    const ImU32 col = ImGui::GetColorU32(ui::color(token));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(r.x, r.y), ImVec2(r.right(), r.y + thickness), col);
    dl->AddRectFilled(ImVec2(r.x, r.bottom() - thickness), ImVec2(r.right(), r.bottom()), col);
    dl->AddRectFilled(ImVec2(r.x, r.y), ImVec2(r.x + thickness, r.bottom()), col);
    dl->AddRectFilled(ImVec2(r.right() - thickness, r.y), ImVec2(r.right(), r.bottom()), col);
}

/// Hide ImGui's nav ring around the field in both profiles.
///
/// The ring is the border a multiline `InputText` draws around its child window when it
/// is focused through the navigation path (the startup focus is `SetKeyboardFocusHere`),
/// not the field's frame border. The caret already marks the focus, and the product wants
/// no border around the input, so the guard clears the flag *and* the auto flag
/// `InputTextEx` consults when it re-enables the cursor for a nav activation. The restore
/// matters: this view draws real buttons (`Prev`/`Next`, the action) in the same frame,
/// and leaving the flag false would take their rings away too.
class FieldNavCursorGuard {
public:
    FieldNavCursorGuard() {
        ImGuiContext* context = ImGui::GetCurrentContext();
        IM_ASSERT(context != nullptr); // a view only runs inside a frame
        if (context == nullptr)
            return;
        ImGuiContext& g = *context;
        saved_ = g.NavCursorVisible;
        savedAuto_ = g.IO.ConfigNavCursorVisibleAuto;
        g.NavCursorVisible = false;
        // `InputTextEx` calls `SetNavCursorVisibleAfterMove()` when it activates the
        // field through the nav path; that helper only sets the cursor when this flag is
        // on, so turning it off for the widget keeps the startup frame clean too.
        g.IO.ConfigNavCursorVisibleAuto = false;
        active_ = true;
    }

    ~FieldNavCursorGuard() {
        if (!active_)
            return;
        if (ImGuiContext* context = ImGui::GetCurrentContext()) {
            context->NavCursorVisible = saved_;
            context->IO.ConfigNavCursorVisibleAuto = savedAuto_;
        }
    }

    FieldNavCursorGuard(const FieldNavCursorGuard&) = delete;
    FieldNavCursorGuard& operator=(const FieldNavCursorGuard&) = delete;

private:
    bool saved_ = false;
    bool savedAuto_ = true;
    bool active_ = false;
};

// ---------------------------------------------------------------------------
// The field's ImGui callback: the resize hook and the paste fold
// ---------------------------------------------------------------------------

/// What the field's callback needs between calls. `capture` non-null switches the
/// character filter into *capture* mode: this frame's characters are a large paste, so
/// they are folded and inserted by the composer instead of by the widget (see
/// `renderComposer`), which is what lets the size question be asked before anything
/// reaches the draft.
///
/// `charsSeen` is how the `/` rule tells a typed slash from a pasted one:
/// the filter runs for clipboard characters too (`InputTextFilterCharacter` is shared by
/// both paths) and ImGui does not tell the callback which one it was, but a paste is a
/// run of characters in one widget call and a typed slash is alone.
struct FieldContext {
    std::string* text = nullptr;
    std::string* capture = nullptr;
    bool afterCr = false;
    /// Set when a `/` on an empty line asked for the command palette: the character is
    /// taken instead of inserted (design §4.6).
    bool* palette = nullptr;
    std::size_t charsSeen = 0;
    int firstCaret = 0;
    bool firstHadSelection = false;
    bool slashTaken = false;
};

void appendUtf8(std::string& out, char32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

int fieldCallback(ImGuiInputTextCallbackData* data) {
    FieldContext* ctx = static_cast<FieldContext*>(data->UserData);
    if (ctx == nullptr)
        return 0;
    switch (data->EventFlag) {
    case ImGuiInputTextFlags_CallbackResize: {
        // The buffer is a `std::string`: the widget asks for room, the string grows and
        // hands its new data pointer back (the shipped composer's callback).
        ctx->text->resize(static_cast<std::size_t>(data->BufTextLen));
        data->Buf = ctx->text->data();
        return 0;
    }
    case ImGuiInputTextFlags_CallbackCharFilter: {
        const char32_t ch = static_cast<char32_t>(data->EventChar);
        if (ctx->capture != nullptr) {
            appendUtf8(*ctx->capture, ch);
            return 1; // taken: the composer inserts it, the widget must not
        }
        if (ctx->charsSeen == 0) {
            ctx->firstCaret = data->CursorPos;
            ctx->firstHadSelection = data->SelectionStart != data->SelectionEnd;
        }
        ++ctx->charsSeen;
        // `/` on an empty line is the palette's command entry (design §4.6). It is taken,
        // not inserted, so the query never starts with a slash the user meant as a
        // command - and a `/` inside a line stays an ordinary character.
        //
        // Only the *first* character of a widget call can be that entry: a pasted
        // `/usr/bin/env` is a run of characters (the caller puts the slash back when it
        // sees more than one), while a typed slash is alone. The text comes from the
        // composer's own buffer, not `data->Buf`: the character filter's callback data
        // carries the event and the caret only (`InputTextFilterCharacter`), and the
        // widget has already written the draft back.
        if (ch == U'/' && ctx->charsSeen == 1 && ctx->palette != nullptr && ctx->text != nullptr &&
            !ctx->firstHadSelection && lineEmptyAt(*ctx->text, data->CursorPos)) {
            ctx->slashTaken = true;
            *ctx->palette = true;
            return 1;
        }
        char32_t out = ch;
        if (!foldPasteChar(ctx->afterCr, ch, out))
            return 1;
        data->EventChar = static_cast<ImWchar>(out);
        return 0;
    }
    default:
        return 0;
    }
}

/// The caret's line in the *widget's* text, or -1 when the field is not being edited.
///
/// Read before the widget is called: `↑` on the first line is a history recall, and by
/// the time the widget has processed the press the caret may already have moved to
/// that line (which would turn one press into "move up, then recall").
int caretLineOf(ImGuiID id) {
    ImGuiInputTextState* state = ImGui::GetInputTextState(id);
    if (state == nullptr)
        return -1;
    const char* text = state->GetText();
    return caretLine(text != nullptr ? std::string_view(text) : std::string_view(),
                     std::max(0, state->GetCursorPos()));
}

/// Ask the live widget to re-read the draft on its next call, caret at the end.
///
/// This is the supported way to rewrite a buffer an active `InputText` owns (writing
/// the string alone is discarded - "changing buf while the InputText is active has no
/// effect"), and it keeps the keyboard: the history recall lands in the field the user is
/// typing in, one frame later, without dropping the focus. A composer-authored insertion
/// uses `reloadFieldTextAt` instead, so the next keystroke lands after the inserted block.
/// False when no widget owns the state (the field is not being edited), and the caller
/// falls back to the one-shot focus request.
bool reloadFieldText(ImGuiID id) {
    ImGuiInputTextState* state = ImGui::GetInputTextState(id);
    if (state == nullptr)
        return false;
    state->ReloadUserBufAndMoveToEnd();
    // The scroll belongs to the text that was there: a whole-buffer rewrite that starts
    // again at the top must not keep the offset of a text nobody is looking at any more.
    state->Scroll = ImVec2(0.0f, 0.0f);
    return true;
}

/// The same re-read, with the caret at `caret` (a byte offset into the *new* draft).
///
/// `SetSelection` writes the stb cursor, which `ReloadUserBufAndKeepSelection` then
/// remembers as the reload target; `ReloadUserBufAndMoveToEnd` would park the caret at
/// the end of the whole draft, so the next keystroke would land there instead of after
/// the block the composer just inserted. The reload clamps the position to the new text.
bool reloadFieldTextAt(ImGuiID id, std::size_t caret) {
    ImGuiInputTextState* state = ImGui::GetInputTextState(id);
    if (state == nullptr)
        return false;
    const int at = static_cast<int>(caret);
    state->SetSelection(at, at);
    state->ReloadUserBufAndKeepSelection();
    state->Scroll = ImVec2(0.0f, 0.0f);
    return true;
}

/// Ask the field to re-read the draft with the caret at `caret`, and remember the
/// request until the compare path consumes it (the next `InputTextEx` call is where a
/// reload is applied, so a plain request would be overwritten by that path's
/// `ReloadUserBufAndMoveToEnd`). `focusRequested` is the fallback when no widget owns
/// the text state yet.
void requestReload(ComposerState& st, ImGuiID id, std::size_t caret) {
    st.reloadCaret = caret;
    if (!reloadFieldTextAt(id, caret))
        st.focusRequested = true;
}

/// The widget's caret, as a byte offset into its text; -1 when the field is not being
/// edited (no caret to paste at).
int caretOffsetOf(ImGuiID id) {
    ImGuiInputTextState* state = ImGui::GetInputTextState(id);
    return state != nullptr ? std::max(0, state->GetCursorPos()) : -1;
}

/// Put `text` into `buffer` at byte offset `at`, clamped into the buffer; `npos` appends.
///
/// The `/` restore uses it to put a taken slash back in front of the payload the widget
/// already inserted; the paste itself goes through `applyEdit` so it can replace a
/// selection.
void insertAtOffset(std::string& buffer, std::size_t at, std::string_view text) {
    if (text.empty())
        return;
    const std::size_t where = std::min(at == std::string::npos ? buffer.size() : at, buffer.size());
    buffer.insert(where, std::string(text));
}

/// Replay one edit: `removed` bytes at `at` become `text`; `npos` appends.
void applyEdit(std::string& buffer, std::size_t at, std::size_t removed, std::string_view text) {
    const std::size_t start = std::min(at == std::string::npos ? buffer.size() : at, buffer.size());
    const std::size_t erase = std::min(removed, buffer.size() - start);
    if (text.empty()) {
        buffer.erase(start, erase);
        return;
    }
    buffer.replace(start, erase, text.data(), text.size());
}

/// Park a payload that is over the limit: the question, its size in characters (the
/// number the rule counts, `ui::utf8Count`) and the edit an `Insert` replays.
void parkPaste(PasteState& paste, const PastePlan& plan, std::size_t insertAt,
               std::size_t removeLen) {
    paste.pending = plan.text;
    paste.insertAt = insertAt;
    paste.removeLen = removeLen;
    paste.ask = true;
    paste.question = "Paste " + std::to_string(plan.chars) + " characters?";
}

/// The placeholder this profile and this width show (design §4.6's degradation).
const char* placeholderFor(const ui::Metrics& m, float w) {
    const bool compact = w < static_cast<float>(kComposerFullPlaceholderGu) * m.gu;
    if (m.textGrid)
        return compact ? kComposerPlaceholderCompactAscii : kComposerPlaceholderAscii;
    return compact ? kComposerPlaceholderCompact : kComposerPlaceholder;
}

} // namespace

// ---------------------------------------------------------------------------
// The paste policy
// ---------------------------------------------------------------------------

std::string normalisePaste(std::string_view raw) {
    std::string out;
    out.reserve(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        const char c = raw[i];
        if (c == '\r') {
            // CRLF folds to one LF; a lone CR (an old Mac line break, or a terminal that
            // sends only CR) becomes LF too, so the draft never carries a carriage return.
            if (i + 1 < raw.size() && raw[i + 1] == '\n')
                ++i;
            out.push_back('\n');
            continue;
        }
        out.push_back(c);
    }
    return out;
}

bool foldPasteChar(bool& afterCr, char32_t in, char32_t& out) {
    if (in == U'\r') {
        afterCr = true;
        out = U'\n';
        return true;
    }
    if (in == U'\n') {
        if (afterCr) { // the LF of a CRLF pair: the CR already produced the line break
            afterCr = false;
            return false;
        }
        out = U'\n';
        return true;
    }
    afterCr = false;
    out = in;
    return true;
}

PastePlan planPaste(std::string_view raw) {
    PastePlan plan;
    plan.text = normalisePaste(raw);
    // D-PASTE counts *characters*: a byte threshold would ask about a third of the CJK
    // text it promises (`ui::utf8Count`).
    plan.chars = static_cast<std::size_t>(ui::utf8Count(plan.text));
    plan.ask = plan.chars > kPasteAskAboveChars;
    return plan;
}

namespace {

/// The byte length a UTF-8 lead byte claims; a stray continuation byte or an invalid
/// lead is one byte (and one codepoint), which keeps the walk total.
std::size_t utf8Claim(unsigned char lead) {
    if (lead < 0x80)
        return 1;
    if ((lead & 0xE0) == 0xC0)
        return 2;
    if ((lead & 0xF0) == 0xE0)
        return 3;
    if ((lead & 0xF8) == 0xF0)
        return 4;
    return 1;
}

/// Decode one codepoint at `at`; `len` is the bytes consumed. A malformed sequence
/// consumes its lead byte only, so the diff stays on boundaries even for bad input.
char32_t decodeUtf8At(std::string_view text, std::size_t at, std::size_t& len) {
    const unsigned char lead = static_cast<unsigned char>(text[at]);
    len = utf8Claim(lead);
    if (len == 1 || at + len > text.size())
        return lead;
    char32_t cp = lead & (0x7Fu >> len);
    for (std::size_t i = 1; i < len; ++i) {
        const unsigned char c = static_cast<unsigned char>(text[at + i]);
        if ((c & 0xC0) != 0x80) {
            len = 1;
            return lead;
        }
        cp = (cp << 6) | (c & 0x3F);
    }
    return cp;
}

struct CodePointAt {
    std::size_t at;
    char32_t cp;
};

std::vector<CodePointAt> codepoints(std::string_view text) {
    std::vector<CodePointAt> out;
    out.reserve(text.size());
    for (std::size_t at = 0; at < text.size();) {
        std::size_t len = 0;
        const char32_t cp = decodeUtf8At(text, at, len);
        out.push_back({at, cp});
        at += len;
    }
    return out;
}

} // namespace

TextEdit diffText(std::string_view before, std::string_view after) {
    // Fast path: a byte-wise prefix/suffix scan, accepted only when both cuts land on
    // UTF-8 boundaries. That is every typed character and almost every paste, and it
    // keeps the per-keystroke compare allocation-free.
    const auto boundary = [](std::string_view text, std::size_t at) {
        return at >= text.size() || (static_cast<unsigned char>(text[at]) & 0xC0) != 0x80;
    };
    std::size_t prefix = 0;
    const std::size_t limit = std::min(before.size(), after.size());
    while (prefix < limit && before[prefix] == after[prefix])
        ++prefix;
    std::size_t suffix = 0;
    while (suffix < before.size() - prefix && suffix < after.size() - prefix &&
           before[before.size() - 1 - suffix] == after[after.size() - 1 - suffix])
        ++suffix;
    const std::size_t beforeEnd = before.size() - suffix;
    const std::size_t afterEnd = after.size() - suffix;
    if (boundary(before, prefix) && boundary(after, prefix) && boundary(before, beforeEnd) &&
        boundary(after, afterEnd)) {
        TextEdit edit;
        edit.start = prefix;
        edit.removed = beforeEnd - prefix;
        edit.inserted = std::string(after.substr(prefix, afterEnd - prefix));
        return edit;
    }

    // The slow, codepoint-aware diff: a CJK pair such as 中 (E4 B8 AD) and 丮 (E4 B8
    // AE) shares two bytes, so the byte cuts would start the payload on a continuation
    // byte and cut the removed range inside the old codepoint. Codepoint equality keeps
    // both sides on UTF-8 boundaries.
    const std::vector<CodePointAt> a = codepoints(before);
    const std::vector<CodePointAt> b = codepoints(after);
    prefix = 0;
    while (prefix < a.size() && prefix < b.size() && a[prefix].cp == b[prefix].cp)
        ++prefix;
    suffix = 0;
    while (suffix < a.size() - prefix && suffix < b.size() - prefix &&
           a[a.size() - 1 - suffix].cp == b[b.size() - 1 - suffix].cp)
        ++suffix;
    const std::size_t aEnd = a.size() - suffix;
    const std::size_t bEnd = b.size() - suffix;
    TextEdit edit;
    edit.start = prefix < a.size() ? a[prefix].at : before.size();
    const std::size_t codepointBeforeEnd = aEnd < a.size() ? a[aEnd].at : before.size();
    edit.removed = codepointBeforeEnd - edit.start;
    const std::size_t afterStart = prefix < b.size() ? b[prefix].at : after.size();
    const std::size_t codepointAfterEnd = bEnd < b.size() ? b[bEnd].at : after.size();
    edit.inserted = std::string(after.substr(afterStart, codepointAfterEnd - afterStart));
    return edit;
}

std::string insertedText(std::string_view before, std::string_view after) {
    return diffText(before, after).inserted;
}

void PasteState::dismiss() {
    pending.clear();
    insertAt = std::string::npos;
    removeLen = 0;
    question.clear();
    ask = false;
    captured.clear();
    runRaw.clear();
    runAt = std::string::npos;
    runRemoved = 0;
    inFlightUntilMs = 0;
}

bool pasteInFlight(const PasteState& paste, long long nowMs) {
    // `nowMs == 0` is "no clock": the window then behaves as closed, so a harness that
    // does not drive a clock is not permanently "in a paste".
    return nowMs > 0 && nowMs < paste.inFlightUntilMs;
}

// ---------------------------------------------------------------------------
// The rest of the input policy
// ---------------------------------------------------------------------------

int draftLines(std::string_view draft) {
    int lines = 1;
    for (const char c : draft)
        if (c == '\n')
            ++lines;
    return lines;
}

int composerRows(std::string_view draft) {
    return std::clamp(draftLines(draft), kComposerMinRows, kComposerMaxRows);
}

int caretLine(std::string_view text, int caret) {
    const int end = static_cast<int>(
        std::min<std::size_t>(text.size(), static_cast<std::size_t>(std::max(0, caret))));
    int line = 0;
    for (int i = 0; i < end; ++i)
        if (text[static_cast<std::size_t>(i)] == '\n')
            ++line;
    return line;
}

bool lineEmptyAt(std::string_view text, int caret) {
    const int at = static_cast<int>(
        std::min<std::size_t>(text.size(), static_cast<std::size_t>(std::max(0, caret))));
    // The line's start: one past the previous newline (0 on the first line).
    int start = at;
    while (start > 0 && text[static_cast<std::size_t>(start - 1)] != '\n')
        --start;
    // The line's end: the next newline, or the end of the draft.
    int end = at;
    while (end < static_cast<int>(text.size()) && text[static_cast<std::size_t>(end)] != '\n')
        ++end;
    // Empty means the two ends are the same cell - `at` lies between them by construction.
    return start == end;
}

SendTrigger sendTrigger(bool actionPressed, bool enterPressed, bool ctrlEnterPressed,
                        bool shiftEnterPressed, model::SendKey setting, bool pasteInFlightNow,
                        bool composing) {
    // The action is a click, so it can never be part of a payload or a composition.
    if (actionPressed)
        return SendTrigger::Action;
    // A paste's own newline is not the user's send (D-PASTE), and a composition's Enter
    // accepts the candidate (it is not a send). Then the gate: while it is closed no key
    // sends, whatever the setting says (plan/key_gate.md §3, D-KEYGATE).
    if (composing || pasteInFlightNow || !kEnterSends)
        return SendTrigger::None;
    switch (setting) {
    case model::SendKey::Enter:
        // `Enter` sends and `Shift+Enter` breaks the line - only meaningful where the
        // two are different chords, which is what `kEnterSends` records.
        return (enterPressed && !shiftEnterPressed) ? SendTrigger::Enter : SendTrigger::None;
    case model::SendKey::CtrlEnter:
        return ctrlEnterPressed ? SendTrigger::CtrlEnter : SendTrigger::None;
    case model::SendKey::EnterNewline:
        return SendTrigger::None;
    }
    return SendTrigger::None;
}

bool prepareSubmit(std::string_view draft, std::string& out) {
    // The resize callback keeps the widget's terminator inside the buffer, so the string
    // can end in NUL bytes; trim them before the newline strip, or the newline is never
    // the last byte and the strip silently does nothing (the shipped composer's note).
    std::size_t end = draft.size();
    while (end > 0 && draft[end - 1] == '\0')
        --end;
    // Enter in a multiline field may have inserted one trailing newline.
    if (end > 0 && draft[end - 1] == '\n')
        --end;
    std::string_view text = draft.substr(0, end);
    bool whitespaceOnly = true;
    for (const char c : text) {
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
            whitespaceOnly = false;
            break;
        }
    }
    if (whitespaceOnly)
        return false;
    out.assign(text);
    return true;
}

bool historyRecall(const std::vector<std::string>& entries, int& pos, std::string& draft,
                   std::string_view current, int step, int caretLineIndex, int lineCount,
                   std::string& out) {
    if (entries.empty() || step == 0)
        return false;
    // History is the field's: a press with no caret (the field is not being edited)
    // recalls nothing, and the caret's line is the gate that keeps `↑`/`↓` caret
    // commands inside a multi-line draft.
    if (caretLineIndex < 0)
        return false;
    if (step < 0 && caretLineIndex != 0)
        return false;
    if (step > 0 && caretLineIndex < lineCount - 1)
        return false;

    const int last = static_cast<int>(entries.size()) - 1;
    // The cursor belongs to the store and `tuiInitQueryHistory` resets it, but a clamp
    // keeps a replaced history list from indexing past its end.
    if (pos > last)
        pos = last;
    if (pos < -1)
        pos = -1;
    if (step < 0) {
        if (pos == -1) {
            // First recall: the draft is stashed so stepping back past the newest entry
            // hands it back (the shipped store's `draftBuf`).
            draft.assign(current);
        }
        if (pos >= last)
            return false; // already at the oldest entry
        ++pos;
    } else {
        if (pos < 0)
            return false; // not navigating: ↓ is the caret's
        --pos;
    }
    if (pos >= 0) {
        out = entries[static_cast<std::size_t>(last - pos)];
        return true;
    }
    // Past the newest entry: the draft comes back, and the stash is consumed.
    out = draft;
    draft.clear();
    return true;
}

MeterLevel meterLevel(long long used, long long max) {
    const float fraction = contextFraction(used, max);
    if (fraction >= 0.9f)
        return MeterLevel::Danger;
    if (fraction >= 0.7f)
        return MeterLevel::Warn;
    return MeterLevel::Normal;
}

float contextFraction(long long used, long long max) {
    if (max <= 0 || used <= 0)
        return 0.0f;
    const double fraction = static_cast<double>(used) / static_cast<double>(max);
    return static_cast<float>(std::clamp(fraction, 0.0, 1.0));
}

// ---------------------------------------------------------------------------
// The view
// ---------------------------------------------------------------------------

void renderComposer(const ComposerInput& in, const ui::Rect& region, const ui::Metrics& m) {
    ComposerState* state = in.state;
    if (state == nullptr || in.buffer == nullptr || in.history == nullptr ||
        in.historyPos == nullptr || in.draft == nullptr)
        return;
    ComposerState& st = *state;

    // The answers are one-frame values: a stale `submit` would send again next frame.
    st.submit = false;
    st.cancelRequested = false;
    st.escapeConsumed = false;
    st.pasteInserted = false;
    st.pasteDiscarded = false;
    st.openPalette = false;

    // Fail closed: a region with no room, an unmetered profile or a rectangle in the
    // wrong unit paints nothing rather than a plausible composer in the wrong place.
    if (region.w <= 0.0f || region.h <= 0.0f || m.row <= 0.0f || m.gu <= 0.0f ||
        !ui::rectMatchesProfile(region, m))
        return;

    // The one-shot hand-off (a starter action's prompt, the empty state's) is applied
    // before the geometry, so the height this frame reports is the one the new draft
    // needs: the thread drew before this frame's composer, which is why the hand-off
    // exists at all.
    if (in.handoff != nullptr && !in.handoff->empty()) {
        *in.buffer = *in.handoff;
        in.handoff->clear();
        *in.historyPos = -1;
        in.draft->clear();
        st.paste.dismiss();
        st.reloadCaret = std::string::npos;
        st.focusRequested = true;
    }

    const float border = m.border;
    const float metaH = m.row;
    // What the draft wants, and what the region can hold. The layout clamps the height
    // (the thread keeps three rows), so a squeezed composer draws fewer rows than the
    // draft has and the field scrolls internally.
    const int wanted = composerRows(*in.buffer);
    const float roomForRows = std::floor((region.h - 2.0f * border - metaH) / m.row);
    const int fits = roomForRows >= 1.0f ? static_cast<int>(roomForRows) : 0;
    const int rows = std::clamp(std::min(wanted, fits), kComposerMinRows, kComposerMaxRows);
    // Reported upwards: the height the *content* needs (not the squeezed one), so a
    // draft that shrinks asks for less room again. `LayoutPrefs::composerH` clamps it.
    st.height = 2.0f * border + static_cast<float>(wanted) * m.row + metaH;
    st.rows = rows;

    const float innerX = region.x + border + m.pad1;
    const float innerW = std::max(0.0f, region.w - 2.0f * (border + m.pad1));
    const float textY = region.y + border;
    // Never taller than the region, even when the layout squeezed it below the one row
    // the anatomy asks for: a clipped field draws less, an overflowing one draws over
    // the now bar.
    const float textH = std::min(static_cast<float>(rows) * m.row,
                                 std::max(0.0f, region.bottom() - border - textY));
    const float metaY = textY + textH;
    const bool hasMeta =
        innerW > 0.0f && textH > 0.0f && metaY + metaH <= region.bottom() - border + 0.01f;

    // --- the container -----------------------------------------------------
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ui::panel(dl, m, region, ColorToken::raised, true);
    blankCells(m, region, ColorToken::raised);

    const ImGuiID fieldId = ImGui::GetID("##composer_input");
    const bool fieldActive = ImGui::GetActiveID() == fieldId;
    // The paste question is the composer's surface while it is up: the field is not
    // drawn, so "the composer owns this Esc" cannot come from the widget's active id.
    const bool ownsKeyboard = fieldActive || st.focusedLastFrame || st.paste.ask;
    const bool focusWasRequested = st.focusRequested;
    st.focused = false;

    // The frame's input shape, read once: a burst nobody could type is captured, and a
    // run already in flight extends through a small chunk of the same paste (a terminal
    // can split a 6000-character paste into a 4096-byte read plus the rest).
    const int queuedChars = ImGui::GetIO().InputQueueCharacters.Size;
    const bool capture = ownsKeyboard && (queuedChars > static_cast<int>(kPasteCaptureAboveChars) ||
                                          (!st.paste.runRaw.empty() && queuedChars > 1));
    const bool pastedFrame = ownsKeyboard && queuedChars > 1;

    // A paste that is still arriving is inserted as one payload when the burst ends: a
    // frame with no paste-sized characters (a typed key, an idle frame, a click). The
    // flush runs before this frame's widget, so a typed character lands after the text,
    // and before the Esc snapshot, so a cancel keeps the paste the user just made.
    if (!st.paste.ask && !st.paste.runRaw.empty() && !capture) {
        const PastePlan plan = planPaste(st.paste.runRaw);
        const std::size_t at = st.paste.runAt;
        const std::size_t removed = st.paste.runRemoved;
        st.paste.runRaw.clear();
        st.paste.runAt = std::string::npos;
        st.paste.runRemoved = 0;
        if (plan.ask) {
            // Nothing has reached the draft yet, so the question needs no revert.
            parkPaste(st.paste, plan, at, removed);
            st.focused = false;
            st.reloadCaret = std::string::npos;
        } else {
            applyEdit(*in.buffer, at, removed, plan.text);
            const std::size_t start =
                at == std::string::npos ? in.buffer->size() - plan.text.size() : at;
            requestReload(st, fieldId, start + plan.text.size());
        }
    }

    // The two facts a press is judged on, taken *before* the widget runs: the caret's
    // line (history's gate) and the draft as it stood (the cancel step must not eat
    // the message the user was writing; ImGui's cancel_edit rewrites the buffer inside
    // the widget call, so the copy has to exist first).
    const int caretBefore = fieldActive ? caretLineOf(fieldId) : -1;
    const int lines = draftLines(*in.buffer);
    const bool escKeyPressed = ownsKeyboard && ImGui::IsKeyPressed(ImGuiKey_Escape, false);
    const bool escPressed = in.acceptKeys && escKeyPressed;
    // The same press when a surface above this field owns it (the floating inspector, the
    // palette): that step is the frame's, but the widget's revert is the field's, so the
    // draft is copied for the undo below on either path.
    const bool overlayEscPressed = in.overlayOpen && escKeyPressed;
    const std::string draftBeforeEsc =
        (escPressed || overlayEscPressed) ? *in.buffer : std::string();

    // The composer's own keys, resolved through the one table (D-KEYMAP): the view asks
    // the registry what a pressed chord *means* and never assigns a chord of its own.
    const model::Action action =
        in.acceptKeys ? model::resolvePressed(
                            model::defaultKeymap(), model::Focus::Composer,
                            [](ImGuiKeyChord chord) { return ImGui::IsKeyChordPressed(chord); })
                      : model::Action::None;

    // --- the mode/model text, on the text line -----------------------------
    // Hidden, not truncated, when the field would fall below `kComposerMinTextGu`: the
    // query is the point of the row (design §4.6's degradation). Nothing is invented
    // for a mode the app does not have - an empty mode simply drops the separator.
    const char* separator = ui::icon(ui::Icon::DisclosureClosed, glyphSetFor(m));
    std::string modeModel;
    if (!in.mode.empty())
        modeModel = in.mode;
    const bool modelKnown = !in.model.empty() && in.model != model::kMissingChipValue;
    if (modelKnown) {
        if (!modeModel.empty()) {
            modeModel += ' ';
            modeModel += separator;
            modeModel += ' ';
        }
        modeModel += in.model;
    }
    float modeModelW = textUnits(m, modeModel);
    // The field keeps its minimum *and* the gap: the model name is the less important
    // half of the text line (design §4.6's degradation), and a field narrower than
    // `kComposerMinTextGu` is a field the user cannot read their query in.
    if (modeModelW > 0.0f &&
        innerW - (modeModelW + m.pad2) < static_cast<float>(kComposerMinTextGu) * m.gu) {
        modeModel.clear();
        modeModelW = 0.0f;
    }
    const float modeModelGap = modeModelW > 0.0f ? m.pad2 : 0.0f;

    ui::Rect textArea;
    textArea.x = innerX;
    textArea.y = textY;
    textArea.w = std::max(0.0f, innerW - modeModelW - modeModelGap);
    textArea.h = textH;
    textArea.cells = m.textGrid;
    if (modeModelW > 0.0f) {
        // Right-aligned on the text line's first row: the design's `Plan ▸ qwen3-coder`.
        lineRight(m, ui::Rect{innerX, textY, innerW, m.row, m.textGrid}, innerX + innerW, textY,
                  modeModel, ColorToken::textLow);
    }

    // --- the text area, or the paste question ------------------------------
    if (st.paste.ask) {
        // The question *replaces* the field: nothing is edited while it is up, so there
        // is no half-applied payload and `Esc` has exactly one meaning (discard). Its
        // answer is a real button beside it, and the action keeps its own label: a
        // running agent must not lose its `Stop` control because a question is up
        // (D-CANCEL is independent of the paste state).
        const ui::ButtonSpec insertSpec{"Insert", true, false, false};
        const float insertW =
            std::min(ui::measureButton(m, insertSpec).w, std::max(0.0f, textArea.w - m.pad1));
        const float insertH = std::min(m.row, textArea.h);
        ui::Rect insertRect{textArea.right() - insertW, textArea.y, insertW, insertH, m.textGrid};
        ui::Rect question = textArea;
        question.w = std::max(0.0f, textArea.w - insertW - m.pad1);
        lineAt(m, question, question.x, question.y, st.paste.question, ColorToken::textHi);
        if (insertW > 0.0f && insertH > 0.0f) {
            ui::button(insertSpec, m, insertRect);
            if (ui::buttonPressed(insertSpec, m, insertRect)) {
                const std::size_t at = st.paste.insertAt;
                const std::size_t removed = st.paste.removeLen;
                const std::size_t payloadSize = st.paste.pending.size();
                applyEdit(*in.buffer, at, removed, st.paste.pending);
                const std::size_t start =
                    at == std::string::npos ? in.buffer->size() - payloadSize : at;
                st.paste.dismiss();
                st.pasteInserted = true;
                st.focusRequested = true;
                requestReload(st, fieldId, start + payloadSize);
            }
        }
    } else {
        if (st.focusRequested) {
            st.focusRequested = false;
            ImGui::SetKeyboardFocusHere();
        }
        // The active field owns its own text, so a write from *outside* it - the submit
        // handshake's clear, a hand-off, a paste this view applies - is discarded by the
        // widget unless it is told to re-read the draft. One compare per frame is what
        // keeps the store and the field the same text.
        //
        // The scroll goes back to the start with it, because a rewrite is a new text:
        // `ImGuiInputTextState::Scroll` is the horizontal offset and the child window's own
        // scroll the vertical one, and a field left scrolled 4938 px into a draft that no
        // longer exists draws nothing at all (measured). `SetNextWindowScroll` is the only
        // handle the widget's child window offers; it applies to the next `BeginChild`,
        // which is this frame's field.
        if (ImGuiInputTextState* textState = ImGui::GetInputTextState(fieldId)) {
            if (st.reloadCaret != std::string::npos) {
                // A composer-authored edit: keep the caret it asked for. This branch has
                // to run *instead of* the end-of-draft reload, not before it - the widget's
                // `TextA` still holds the old text, so the compare would fire again and
                // overwrite the selection the edit just set.
                if (*in.buffer != textState->GetText()) {
                    const int caret = static_cast<int>(std::min(st.reloadCaret, in.buffer->size()));
                    textState->SetSelection(caret, caret);
                    textState->ReloadUserBufAndKeepSelection();
                    textState->Scroll = ImVec2(0.0f, 0.0f);
                    ImGui::SetNextWindowScroll(ImVec2(0.0f, 0.0f));
                }
                st.reloadCaret = std::string::npos;
            } else if (*in.buffer != textState->GetText()) {
                textState->ReloadUserBufAndMoveToEnd();
                textState->Scroll = ImVec2(0.0f, 0.0f);
                ImGui::SetNextWindowScroll(ImVec2(0.0f, 0.0f));
            }
        }
        // The draft and the caret as they stood *before* the widget ran: the snapshot lets
        // the size guard below undo a paste that inserted itself (the pixel profile's
        // clipboard path, which ImGui performs).
        const std::string draftBefore = *in.buffer;

        // Where a run starts is fixed by its first chunk: a paste replaces the selection,
        // so the destination is the selection start and the replaced span is its length;
        // without a selection it is the caret. The run then grows into a buffer of its
        // own, so the insertion point cannot drift while the terminal is still delivering
        // chunks.
        //
        // Neither backend reports bracketed paste: nothing asks the terminal for `?2004h`
        // (the ncurses/imtui backends never send it), and the payload's newline arrives as
        // an `Enter` *key* event rather than a character - `plan/key_gate.md` §8 leaves
        // bracketed paste out of scope for the gate, so the burst is the signal there is.
        if (capture && st.paste.runRaw.empty()) {
            st.paste.runAt = std::string::npos;
            st.paste.runRemoved = 0;
            if (ImGuiInputTextState* textState = ImGui::GetInputTextState(fieldId)) {
                int selectionStart = textState->GetSelectionStart();
                int selectionEnd = textState->GetSelectionEnd();
                if (selectionStart > selectionEnd)
                    std::swap(selectionStart, selectionEnd);
                const bool hasSelection = selectionStart != selectionEnd;
                const std::size_t at =
                    hasSelection ? static_cast<std::size_t>(std::max(0, selectionStart))
                                 : static_cast<std::size_t>(std::max(0, textState->GetCursorPos()));
                const std::size_t clamped = std::min(at, in.buffer->size());
                st.paste.runAt = clamped;
                if (hasSelection) {
                    const std::size_t end = static_cast<std::size_t>(std::max(0, selectionEnd));
                    st.paste.runRemoved =
                        std::min(end > clamped ? end - clamped : 0, in.buffer->size() - clamped);
                }
            }
        }
        FieldContext context;
        context.text = in.buffer;
        context.capture = capture ? &st.paste.captured : nullptr;
        context.palette = &st.openPalette;
        if (capture)
            st.paste.captured.clear();

        // The container *is* the well: the field paints no fill of its own (its
        // `FrameBg` slot is the `inset` well the theme binds, which inside a `raised`
        // container would read as a box in a box) and no border.
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ui::color(ColorToken::raised));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ui::color(ColorToken::raised));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ui::color(ColorToken::raised));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
        // A multiline field is a child window, and ImGui clamps a child to
        // `IMGUI_WINDOW_HARD_MIN_SIZE` (four rows): one text row would paint three rows
        // over the meta row. The clip rect is what holds the widget inside the rows this
        // composer reserved (measured: the widget's own draw respects it).
        ImGui::PushClipRect(ImVec2(textArea.x, textArea.y),
                            ImVec2(textArea.right(), textArea.bottom()), true);
        ImGui::SetCursorScreenPos(ImVec2(textArea.x, textArea.y));
        if (in.offline) {
            // Disabled controls are dimmed, not removed (design §4.11); the active id has
            // to be released by hand, or the field keeps a keyboard it appears not to
            // have (the shipped composer's note).
            if (fieldActive)
                ImGui::ClearActiveID();
            ImGui::BeginDisabled();
        }
        {
            // The ring the grid would draw around a focused item is suppressed for the
            // *field* only (its caret marks the focus); the guard puts the flag back so
            // the buttons this view draws below keep theirs.
            const FieldNavCursorGuard navCursor;
            ImGui::InputTextEx("##composer_input", placeholderFor(m, region.w), in.buffer->data(),
                               static_cast<int>(in.buffer->size() + 1),
                               ImVec2(textArea.w, textArea.h),
                               ImGuiInputTextFlags_Multiline | ImGuiInputTextFlags_CallbackResize |
                                   ImGuiInputTextFlags_CallbackCharFilter,
                               fieldCallback, &context);
        }
        if (in.offline)
            ImGui::EndDisabled();
        ImGui::PopClipRect();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(3);

        st.focused = ImGui::IsItemActive();

        // The `/` on an empty line was taken in the filter. Only a *typed* slash may open
        // the palette: when the same widget call went on to deliver more characters, the
        // slash was the first character of a paste, so it goes back where it was taken
        // from - the rest of the payload is already at that offset, which restores the
        // payload's order.
        if (context.slashTaken && context.charsSeen > 1) {
            st.openPalette = false;
            const int caretAfterPayload = caretOffsetOf(fieldId);
            insertAtOffset(*in.buffer, static_cast<std::size_t>(std::max(0, context.firstCaret)),
                           "/");
            const std::size_t caret =
                (caretAfterPayload >= 0
                     ? static_cast<std::size_t>(caretAfterPayload)
                     : static_cast<std::size_t>(std::max(0, context.firstCaret))) +
                1;
            requestReload(st, fieldId, caret);
        }

        // The payload this frame's characters were folded into, applied the one way
        // D-PASTE allows: normalised, asked about above the limit, never sent, and placed
        // where the paste started. The run is buffered rather than inserted chunk by
        // chunk: a multi-frame paste must be one payload, or its chunks would land in
        // frame order and the limit would be judged on one frame's slice.
        if (pastedFrame)
            st.paste.inFlightUntilMs = in.nowMs + kPasteFlightMs;
        if (!st.paste.captured.empty()) {
            st.paste.runRaw += st.paste.captured;
            st.paste.captured.clear();
            st.focused = false;
        } else if (*in.buffer != draftBefore) {
            // Something arrived in the draft that this view did not put there: the pixel
            // profile's clipboard paste is ImGui's own insertion (`stb_textedit_paste`),
            // so the text is already in when the view sees it. The edit is the difference
            // between the two drafts; over the limit it is undone and asked about, exactly
            // like a captured payload (D-PASTE applies to both profiles).
            const TextEdit edit = diffText(draftBefore, *in.buffer);
            const PastePlan plan = planPaste(edit.inserted);
            if (plan.ask) {
                *in.buffer = draftBefore;
                parkPaste(st.paste, plan, edit.start, edit.removed);
                requestReload(st, fieldId, edit.start);
                st.focused = false;
            }
        }
    }

    // --- the Esc chain's last two steps (D-ESC) ----------------------------
    // Steps 1 and 2 (close the overlay, cancel the run) belong to the frame layer; the
    // *last two* have to run here, after the widget, because ImGui's cancel_edit has
    // already reverted the buffer inside the call. The chain's order is
    // `model::resolveEscape`'s, so this site never re-decides it. A press an overlay owns
    // lands here too, but only to undo that revert: the step is the frame's, the text is
    // the field's.
    if (escPressed) {
        if (st.paste.ask) {
            // The question is the topmost surface here: one press answers it.
            st.paste.dismiss();
            st.pasteDiscarded = true;
            st.escapeConsumed = true;
            st.focusRequested = true;
        } else {
            const model::Action step = model::resolveEscape(
                {in.overlayOpen, in.busy && !in.stopping, !in.buffer->empty(), in.focusMode});
            if (step == model::Action::CancelRun) {
                // The frame owns the request; undo the widget's snapshot restore so the
                // draft survives the press (partial output is kept, and so is the text).
                if (*in.buffer != draftBeforeEsc)
                    *in.buffer = draftBeforeEsc;
                st.cancelRequested = true;
                st.escapeConsumed = true;
                // ImGui's cancel_edit released the field on this press. The chain's next
                // steps (clear, then blur) are *presses*, so the keyboard goes back to the
                // field the user was typing in - only the blur step leaves it released
                // (D-ESC: one step per press, not one step per focus).
                st.focusRequested = true;
            } else if (step == model::Action::ClearInput) {
                in.buffer->clear();
                *in.historyPos = -1;
                in.draft->clear();
                st.paste.dismiss();
                st.reloadCaret = std::string::npos;
                st.escapeConsumed = true;
                st.focusRequested = true;
            } else if (step == model::Action::Blur) {
                ImGui::ClearActiveID();
                st.focused = false;
                st.focusedLastFrame = false;
                st.escapeConsumed = true;
            } else if (step == model::Action::ExitFocusMode) {
                // The step is the frame's (task 43); the widget's cancel_edit is the field's, so
                // the draft goes back and the keyboard stays - D-ESC's rule that only the blur
                // step leaves the field. `escapeConsumed` stays false because the frame is the
                // one that performs this step, and a press may only be spent once.
                if (*in.buffer != draftBeforeEsc)
                    *in.buffer = draftBeforeEsc;
                st.focusRequested = true;
            }
            // CloseOverlay belongs to the frame - and `acceptKeys` is false while an
            // overlay owns the keyboard, so it cannot arrive here.
        }
    } else if (overlayEscPressed) {
        // The press was the overlay's step, not one of the field's: ImGui's cancel_edit
        // still reverted the buffer inside the widget call, so the draft goes back and the
        // keyboard stays here (only the Blur step leaves the field released). Without this
        // one press closes the panel and silently eats the text under it.
        if (*in.buffer != draftBeforeEsc)
            *in.buffer = draftBeforeEsc;
        st.focusRequested = true;
    }

    // --- history (design §4.6) ---------------------------------------------
    // After the widget, because the buffer is rewritten and the widget ignores a write
    // to a buffer it is editing: the active id is dropped and the one-shot focus
    // request re-reads the recalled text on the next frame (the shipped composer's
    // refocus, in one place).
    if (!st.escapeConsumed && !st.paste.ask &&
        (action == model::Action::HistoryPrev || action == model::Action::HistoryNext)) {
        const int step = action == model::Action::HistoryPrev ? -1 : 1;
        std::string recalled;
        if (historyRecall(*in.history, *in.historyPos, *in.draft, *in.buffer, step, caretBefore,
                          lines, recalled)) {
            *in.buffer = recalled;
            st.reloadCaret = std::string::npos;
            if (!reloadFieldText(fieldId))
                st.focusRequested = true;
            st.focused = false;
        }
    }

    // --- the meta row -------------------------------------------------------
    // Left: the mini context meter. Middle: the phase and the speed while a run is
    // active. Right: the one action. The now bar's chips stay canonical for the
    // numbers; this row only ever *reads* the same pair (`StatusChips::contextUsed/
    // contextMax`), so the two cannot disagree.
    const char* actionLabel = "Send";
    bool actionPrimary = true;
    bool actionEnabled = !in.offline;
    if (in.busy) {
        actionLabel = in.stopping ? "Stopping" : "Stop";
        actionPrimary = false;
        actionEnabled = actionEnabled && !in.stopping;
    }
    // A pending paste question does *not* take this control over: its `Insert` lives
    // beside the question, so a running agent keeps its `Stop` (D-CANCEL is independent
    // of the paste state). The idle `Send` *is* disabled while the question is up: it
    // would send the pre-paste draft and `applyComposerAnswers()` drops the pending
    // payload, which is not an answer to the question. Stop keeps its state.
    if (st.paste.ask && !in.busy)
        actionEnabled = false;
    // One width for every label, so Send -> Stop -> Send never resizes the row (the
    // widest label is `Stopping`; a narrower button is centred text inside the same
    // box).
    const ui::ButtonSpec actionSpec{actionLabel, actionEnabled, false, actionPrimary};
    const ui::ButtonSpec widestSpec{"Stopping", true, false, false};
    const float actionW =
        std::min(ui::measureButton(m, widestSpec).w, std::max(0.0f, innerW / 2.0f));
    const float actionX = innerX + std::max(0.0f, innerW - actionW);

    if (hasMeta) {
        ui::Rect metaRow{innerX, metaY, innerW, metaH, m.textGrid};

        // The meter. The full form (`ctx ████░░ 42k`) needs ~18 cells, so the grid drops
        // the bar below 80 cells and keeps the count (design §4.6); the pixel profile
        // always has room for its ten segments.
        const bool fullMeter =
            !m.textGrid || region.w >= static_cast<float>(kComposerFullMeterGu) * m.gu;
        if (st.meterUsed != in.contextUsed || st.meterMax != in.contextMax ||
            st.meterKnown != in.contextKnown) {
            st.meterUsed = in.contextUsed;
            st.meterMax = in.contextMax;
            st.meterKnown = in.contextKnown;
            // A missing capacity is not "0 tokens": the count is the used value only
            // when the sentence carried a pair to abbreviate *from*.
            if (!in.contextKnown) {
                // U+2014 is East-Asian *Ambiguous*: a CJK terminal may draw it two
                // cells wide, so the grid gets the ASCII floor (the model's own rule
                // for the chips' missing value).
                st.meterText = m.textGrid ? "-" : std::string(model::kMissingChipValue);
            } else {
                st.meterText = ui::formatCount(in.contextUsed);
            }
        }
        float meterX = metaRow.x;
        if (fullMeter && in.contextKnown && in.contextMax > 0) {
            const int segments = m.textGrid ? kMeterSegmentsGrid : kMeterSegmentsGui;
            const char* label = "ctx";
            lineAt(m, metaRow, meterX, metaRow.y, label, ColorToken::textLow);
            meterX += textUnits(m, label) + m.pad1;
            const ui::Size barSize = ui::measureMeter(m, segments);
            ui::Rect bar{meterX, metaRow.y + std::floor((metaRow.h - barSize.h) * 0.5f), barSize.w,
                         barSize.h, m.textGrid};
            ColorToken barInk = ColorToken::accent;
            switch (meterLevel(in.contextUsed, in.contextMax)) {
            case MeterLevel::Normal:
                break;
            case MeterLevel::Warn:
                barInk = ColorToken::stateWarn;
                break;
            case MeterLevel::Danger:
                barInk = ColorToken::stateDanger;
                break;
            }
            ui::meter(m, bar, segments, contextFraction(in.contextUsed, in.contextMax), barInk);
            // The compaction threshold, when the app knows one: a one-unit tick across
            // the bar's own row (`compactAt == 0` is "unknown" and draws nothing).
            if (in.compactAt > 0 && in.contextMax > 0) {
                const float fraction = contextFraction(in.compactAt, in.contextMax);
                const float tickX = bar.x + std::floor(fraction * bar.w);
                ImDrawList* tickList = ImGui::GetWindowDrawList();
                tickList->AddRectFilled(
                    ImVec2(std::max(bar.x, tickX - m.border * 0.5f), bar.y),
                    ImVec2(std::max(bar.x, tickX - m.border * 0.5f) + m.border, bar.y + bar.h),
                    ImGui::GetColorU32(ui::color(ColorToken::textHi)));
            }
            meterX = bar.right() + m.pad1;
        }
        lineAt(m, metaRow, meterX, metaRow.y, st.meterText, ColorToken::textLow);

        // History, on the text grid: two real 1-row buttons beside the action.
        //
        // `plan/key_gate.md` §4b measured that this backend delivers *neither* `↑`/`↓`
        // (they arrive as `Escape` + `[A`, which also releases the field) *nor*
        // `Ctrl+Up`/`Ctrl+Down` (no key event at all) - so a chord that nobody receives
        // is not a history path, and the two buttons the shipped composer draws stay
        // here (D-KEYGATE: the fallback column is a legitimate outcome). The pixel
        // profile has no use for them: both chords are delivered there, and the arrow
        // keys are the design's own gesture.
        const ui::ButtonSpec prevSpec{"Prev", true, false, false};
        const ui::ButtonSpec nextSpec{"Next", true, false, false};
        float historyLeft = actionX;
        bool prevPressed = false;
        bool nextPressed = false;
        if (m.textGrid && !in.history->empty() && hasMeta) {
            const float prevW = ui::measureButton(m, prevSpec).w;
            const float nextW = ui::measureButton(m, nextSpec).w;
            const float nextX = actionX - m.pad1 - nextW;
            const float prevX = nextX - prevW;
            // Only when the row can hold them beside the count and the action: a history
            // button that eats the action would trade a control for a hint.
            if (prevX >= meterX + textUnits(m, st.meterText) + m.pad1) {
                ui::Rect prevRect{prevX, metaRow.y, prevW, metaRow.h, m.textGrid};
                ui::Rect nextRect{nextX, metaRow.y, nextW, metaRow.h, m.textGrid};
                ui::button(prevSpec, m, prevRect);
                prevPressed = ui::buttonPressed(prevSpec, m, prevRect);
                ui::button(nextSpec, m, nextRect);
                nextPressed = ui::buttonPressed(nextSpec, m, nextRect);
                historyLeft = prevX;
            }
        }

        // Middle: the phase and the speed, only while a run is active. Two already
        // formatted values joined - the numbers themselves are never reformatted here.
        if (in.busy) {
            std::string busy = in.phase;
            if (!in.speed.empty() && in.speed != model::kMissingChipValue) {
                if (!busy.empty())
                    busy += " \u00b7 ";
                busy += in.speed;
            }
            if (!busy.empty()) {
                const float middleX = meterX + textUnits(m, st.meterText) + m.pad2;
                const float middleRoom = historyLeft - m.pad2 - middleX;
                if (middleRoom > 0.0f) {
                    ui::Rect middle{middleX, metaRow.y, middleRoom, metaRow.h, m.textGrid};
                    lineAt(m, middle, middleX, metaRow.y, busy, ColorToken::textMid);
                }
            }
        }

        // A button press is the user's whole gesture, so it does not need the caret gate
        // the arrow keys need: `(0, 1)` says "the draft is one line and the caret is on
        // it", which allows both directions (design §4.6's gate is about *keys* that
        // double as caret movement).
        if (prevPressed || nextPressed) {
            std::string recalled;
            if (historyRecall(*in.history, *in.historyPos, *in.draft, *in.buffer,
                              prevPressed ? -1 : 1, /*caretLineIndex=*/0, /*lineCount=*/1,
                              recalled)) {
                *in.buffer = recalled;
                if (!reloadFieldText(fieldId))
                    st.focusRequested = true;
                st.focused = false;
            }
        }

        // Right: the action. It is a real item (the read-only-`InputText` fake is gone):
        // `Send` sends, `Stop` requests the cancel the frame turns into the shipped
        // `stopAgent()` path (D-CANCEL), `Stopping` is disabled until the run ends. A
        // pending paste question does not take it over: its `Insert` sits beside the
        // question, so D-CANCEL never depends on the paste state.
        if (actionW > 0.0f) {
            ui::Rect actionRect{actionX, metaY + std::floor((metaH - m.row) * 0.5f), actionW, m.row,
                                m.textGrid};
            ui::button(actionSpec, m, actionRect);
            if (in.offline) {
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ui::tooltip(model::kComposerOfflineTooltip);
            } else if (in.busy) {
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ui::tooltip(in.stopping
                                    ? "Stop already requested; waiting for the agent to stop"
                                    : "Stop the running agent (partial output is kept)");
            } else if (st.paste.ask) {
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ui::tooltip("Answer the paste question first (Insert or Esc)");
            } else if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ui::tooltip("Send the query to the LLM (Enter breaks the line)");
            }
            if (actionEnabled && ui::buttonPressed(actionSpec, m, actionRect)) {
                if (in.busy) {
                    st.cancelRequested = true;
                } else {
                    const SendTrigger trigger =
                        sendTrigger(/*actionPressed=*/true, false, false, false, in.sendKey,
                                    pasteInFlight(st.paste, in.nowMs), /*composing=*/false);
                    if (trigger == SendTrigger::Action)
                        st.submit = true;
                }
            }
        }
    }

    // The focus ring is the container's state, not the field's: one container, one
    // boundary, and the ring reads on both profiles (1 unit on the grid, the design's
    // 2 px in the pixel profile).
    const bool ringed = st.focused || st.focusedLastFrame || focusWasRequested;
    if (ringed)
        ringInside(region, ColorToken::focusRing, m.textGrid ? border : 2.0f * border);

    st.focusedLastFrame = st.focused;
}

} // namespace llmfun::tui::views
