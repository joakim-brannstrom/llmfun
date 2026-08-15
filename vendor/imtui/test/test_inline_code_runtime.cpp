// test_inline_code_runtime.cpp
//
// Runtime behavioral verification for inline code spans in the vendored
// imgui_markdown single-header renderer (design section 6).
//
// The static-analysis tests (test_inline_code.py) cannot catch bookkeeping
// off-by-ones that silently duplicate or drop text, so this harness compiles
// imgui_markdown.h against a fake ImGui namespace whose stubs record every
// rendered byte range. It feeds each edge-case matrix row
// (plan/system_design.md section 5.6) plus regression rows through
// ImGui::Markdown and asserts:
//   (a) the rendered byte stream equals the expected text exactly (span
//       delimiters removed for closed spans, backticks preserved for
//       aborted/literal cases, no duplicated or dropped bytes), and
//   (b) INLINE_CODE callback pairs wrap exactly the expected ranges.
//
// Standalone C++11 program: no imtui, no real ImGui. Compiled and run by
// build_test.py, or directly:
//   g++ -std=c++11 -o test_inline_code_runtime test_inline_code_runtime.cpp
//   ./test_inline_code_runtime
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#define IMGUI_VERSION_NUM 19197 // >= 19197: CalcWordWrapPosition + GetFontSize path
                                // (llmfun_tui builds against vendored imgui 1.81, which
                                // takes the < 19197 CalcWordWrapPositionA path; both
                                // stubs return text_end, so the byte-range logic is
                                // identical in both branches)

// ---------------------------------------------------------------------------
// Fake ImGui namespace — must come BEFORE imgui_markdown.h
// ---------------------------------------------------------------------------
namespace ImGui {

struct ImVec2 {
    float x, y;
    ImVec2(float _x = 0, float _y = 0) : x(_x), y(_y) {}
};
struct ImVec4 {
    float x, y, z, w;
    ImVec4(float _x = 0, float _y = 0, float _z = 0, float _w = 0)
        : x(_x), y(_y), z(_z), w(_w) {}
};
typedef int ImGuiID;
typedef unsigned int ImGuiCol;
typedef void* ImTextureID;
typedef int ImGuiStyleVar;
typedef int ImGuiWindowFlags;
typedef int ImGuiHoveredFlags;
enum {
    ImGuiCol_Text = 0,
    ImGuiCol_TextDisabled = 1,
    ImGuiCol_Button = 2,
    ImGuiCol_ButtonHovered = 3,
    ImGuiCol_Border = 4,
};
typedef int ImGuiCond;
struct ImColor {
    ImColor() {}
    ImColor(float, float, float, float) {}
    ImColor(const ImVec4&) {}
};

// ---- recording state ------------------------------------------------------
// The current input buffer; byte ranges recorded by TextUnformatted and the
// format callback are offsets into it.
static const char* g_input = NULL;
static std::string g_rendered; // concatenation of every rendered byte range
static int g_bullets = 0;      // Bullet() calls (list items)

inline void TextUnformatted(const char* begin, const char* end) {
    g_rendered.append(begin, end);
}
inline void Text(const char*, ...) {} // tooltips only; not exercised here
inline void SameLine(float = 0.0f, float = -1.0f) {}
inline void NewLine() {} // layout, not text: the stream holds text bytes only
inline void Bullet() { ++g_bullets; }
inline void Indent(float = 0.0f) {}
inline void Unindent(float = 0.0f) {}
inline void Separator() {}
inline void PushStyleColor(ImGuiCol, const ImVec4&) {}
inline void PopStyleColor(int = 1) {}
inline void PushStyleVar(ImGuiStyleVar, float) {}
inline void PushStyleVar(ImGuiStyleVar, const ImVec2&) {}
inline void PopStyleVar(int = 1) {}
inline void PushTextWrapPos(float = 0.0f) {}
inline void PopTextWrapPos() {}
inline void PushItemWidth(float) {}
inline void PopItemWidth() {}
inline void SetTooltip(const char*, ...) {}
inline void PushID(const char*) {}
inline void PushID(ImGuiID) {}
inline void PopID() {}
inline void OpenPopup(const char*) {}
inline void BeginPopup(const char*) {}
inline void EndPopup() {}
inline bool IsItemHovered() { return false; }
inline bool IsMouseReleased(int) { return false; }
inline ImVec2 GetItemRectMin() { return ImVec2(); }
inline ImVec2 GetItemRectMax() { return ImVec2(); }
inline ImVec2 GetWindowPos() { return ImVec2(); }
inline ImVec2 GetCursorScreenPos() { return ImVec2(); }
inline ImVec2 GetCursorPos() { return ImVec2(); }
inline void SetCursorPos(const ImVec2&) {}
inline ImVec2 GetContentRegionAvail() { return ImVec2(1000.0f, 100.0f); }
inline ImVec2 GetContentRegionMax() { return ImVec2(1000.0f, 100.0f); }
inline ImVec2 GetWindowSize() { return ImVec2(1000.0f, 100.0f); }
inline float GetFontSize() { return 16.0f; }
inline float GetTextLineHeightWithSpacing() { return 20.0f; }
inline ImGuiID GetID(const char*) { return 0; }
inline void SetItemDefaultFocus() {}
inline void SetKeyboardFocusHere(int = 0) {}
inline bool IsItemClicked(int = 0) { return false; }
inline bool IsItemVisible() { return true; }
inline bool IsWindowAppearing() { return false; }
inline bool IsWindowHovered(ImGuiHoveredFlags = 0) { return false; }
inline bool IsMouseDragging(int, float = -1.0f) { return false; }
inline bool IsKeyPressed(int, bool = true) { return false; }
inline void SetScrollHereY(float = 0.5f) {}
inline void BeginGroup() {}
inline void EndGroup() {}
inline void InvisibleButton(const char*, const ImVec2&) {}
inline void PushFont(void*) {}
inline void PopFont() {}
inline void PushStyleColorV(ImGuiCol, const ImVec4&) {}
inline bool Button(const char*, const ImVec2& = ImVec2()) { return false; }
inline bool ImageButton(ImTextureID, const ImVec2&, const ImVec2&, const ImVec2&) {
    return false;
}
inline void Image(ImTextureID, const ImVec2&, const ImVec2&, const ImVec2&) {}
inline void ImageWithBg(ImTextureID, const ImVec2&, const ImVec2&, const ImVec2&,
                        const ImVec4&) {}
inline void ImageWithBg(ImTextureID, const ImVec2&, const ImVec2&, const ImVec2&) {}
inline void ImageWithBg(ImTextureID, const ImVec2&, const ImVec2&, const ImVec2&,
                        const ImVec4&, const ImVec4&) {}
inline bool Selectable(const char*, bool = false, int = 0, const ImVec2& = ImVec2()) {
    return false;
}

struct ImFont {
    const char* CalcWordWrapPositionA(float, const char* begin, const char* end,
                                      float) const {
        return end; // no wrapping
    }
    const char* CalcWordWrapPosition(float, const char* begin, const char* end,
                                     float) const {
        return end;
    }
};
inline ImFont* GetFont() {
    static ImFont f;
    return &f;
}
struct ImGuiIO {
    float FontGlobalScale = 1.0f;
};
inline ImGuiIO& GetIO() {
    static ImGuiIO io;
    return io;
}
struct ImGuiStyle {
    ImVec4 Colors[128];
};
inline ImGuiStyle& GetStyle() {
    static ImGuiStyle s;
    return s;
}
struct ImDrawList {
    void AddLine(const ImVec2&, const ImVec2&, unsigned int, float) {}
    void AddLine(const ImVec2&, const ImVec2&, const ImColor&, float) {}
    void AddRectFilled(const ImVec2&, const ImVec2&, unsigned int, float = 0.0f,
                       int = 0) {}
    void AddRect(const ImVec2&, const ImVec2&, unsigned int, float = 0.0f, int = 0,
                 float = 1.0f) {}
    void AddText(const ImVec2&, unsigned int, const char*, const char* = NULL) {}
};
inline ImDrawList* GetWindowDrawList() {
    static ImDrawList dl;
    return &dl;
}
inline ImVec2 CalcTextSize(const char*, const char* = NULL, bool = false,
                           float = -1.0f) {
    return ImVec2();
}
inline bool BeginChild(const char*, const ImVec2& = ImVec2(), bool = false, int = 0) {
    return false;
}
inline void EndChild() {}
inline void SetNextWindowPos(const ImVec2&, ImGuiCond = 0, const ImVec2& = ImVec2()) {}
inline void SetNextWindowSize(const ImVec2&, ImGuiCond = 0) {}
inline void SetNextWindowSizeConstraints(const ImVec2&, const ImVec2&) {}
inline void SetNextWindowBgAlpha(float) {}
inline bool Begin(const char*, bool* = NULL, ImGuiWindowFlags = 0) { return true; }
inline void End() {}
inline void Dummy(const ImVec2&) {}
inline void TextColored(const ImVec4&, const char*, ...) {}
inline void TextDisabled(const char*, ...) {}
inline void TextWrapped(const char*, ...) {}
inline void TextUnformatted(const char* text) {
    TextUnformatted(text, text + strlen(text));
}
inline void LabelText(const char*, const char*, ...) {}
inline bool TreeNode(const char*, ...) { return true; }
inline void TreePop() {}
inline void Spacing() {}
inline void SetNextItemOpen(bool, ImGuiCond = 0) {}
inline void RenderText(const ImVec2&, const char*, const char*, bool = false) {}

} // namespace ImGui

#include "../../imgui_markdown/imgui_markdown.h"

// ---------------------------------------------------------------------------
// Recording format callback
// ---------------------------------------------------------------------------

struct Call {
    int type;          // MarkdownFormatType value
    int level;
    bool start;
    int textOffset;    // offset of info.text into the current input (-1 if NULL)
    int textLength;
    int blockIdOffset; // offset of info.blockId into the current input (-1 if NULL)
    int blockIdLength;
};

static std::vector<Call> g_calls;

static void recordingCallback(const ImGui::MarkdownFormatInfo& info, bool start_) {
    Call c;
    c.type = (int)info.type;
    c.level = (int)info.level;
    c.start = start_;
    c.textOffset = (info.text && ImGui::g_input) ? (int)(info.text - ImGui::g_input) : -1;
    c.textLength = (int)info.textLength;
    c.blockIdOffset =
        (info.blockId && ImGui::g_input) ? (int)(info.blockId - ImGui::g_input) : -1;
    c.blockIdLength = (int)info.blockIdLength;
    g_calls.push_back(c);

    // The parser delegates code block rendering entirely to the format
    // callback (the default callback renders it with TextUnformatted), so
    // mirror that here to keep the rendered stream complete for fenced rows.
    if (start_ && info.type == ImGui::MarkdownFormatType::CODE_BLOCK && info.text &&
        info.textLength > 0) {
        ImGui::g_rendered.append(info.text, info.text + info.textLength);
    }
}

// ---------------------------------------------------------------------------
// Edge-case matrix rows (plan/system_design.md section 5.6) + regression rows
// ---------------------------------------------------------------------------

struct Row {
    const char* name;
    const char* input;
    const char* expectedStream;
    // Expected INLINE_CODE span contents, in render order, NULL-terminated.
    const char* spans[4];
    bool expectEmphasis;      // at least one EMPHASIS callback pair
    bool expectLink;          // at least one LINK callback pair
    bool expectNoLinkEmphasis;// no LINK and no EMPHASIS callback pairs
    int expectBullets;        // expected Bullet() count
    bool expectCodeBlock;     // at least one CODE_BLOCK callback pair
    const char* blockId;      // expected blockId of the CODE_BLOCK start call
};

static const Row ROWS[] = {
    // --- design 5.6 edge-case matrix ---
    {"basic span", "`some_code`", "some_code", {"some_code", NULL}, false, false, false,
     0, false, NULL},
    {"span mid-line", "foo `bar` baz", "foo bar baz", {"bar", NULL}, false, false,
     false, 0, false, NULL},
    {"literal markdown in span", "`*not em* and [no link](x)`",
     "*not em* and [no link](x)", {"*not em* and [no link](x)", NULL}, false, false,
     true, 0, false, NULL},
    {"emphasis then span", "*em* `code`", "em code", {"code", NULL}, true, false,
     false, 0, false, NULL},
    {"two adjacent spans", "`a` `b`", "a b", {"a", "b", NULL}, false, false, false, 0,
     false, NULL},
    {"span then plain text", "`a`b", "ab", {"a", NULL}, false, false, false, 0, false,
     NULL},
    {"unclosed span at EOL", "`unclosed\n", "`unclosed", {NULL}, false, false, false, 0,
     false, NULL},
    {"unclosed span at EOF", "`unclosed", "`unclosed", {NULL}, false, false, false, 0,
     false, NULL},
    {"two backticks literal", "``", "``", {NULL}, false, false, false, 0, false, NULL},
    {"mid-line triple backticks literal", "foo ``` bar", "foo ``` bar", {NULL}, false,
     false, false, 0, false, NULL},
    // Both backtick pairs are literal: the approved lone-backtick guards
    // (prev/next char must not be a backtick) block span opens here, and
    // run-length matching is a documented non-goal (design 5.6 note).
    {"double-backtick delimiters literal", "``foo``", "``foo``", {NULL}, false, false,
     false, 0, false, NULL},
    {"lone-backtick rule", "`a``b`", "a`b`", {"a", NULL}, false, false, false, 0,
     false, NULL},
    // Heading render skips the '#' and the space after it, and the guard
    // blocks span opens inside headings, so the backticks render literally.
    {"span in heading unsupported", "# `code`", "`code`", {NULL}, false, false, false,
     0, false, NULL},
    {"backticks inside emphasis literal", "*em `code`*", "em `code`", {NULL}, true,
     false, false, 0, false, NULL},
    {"backticks inside link text literal", "[a `b`](url)", "a `b`", {NULL}, false,
     true, false, 0, false, NULL},
    // No bullet at zero indent: unordered lists require >= 2 leading spaces,
    // so "* " renders literally as pre-text (verified against the parser).
    {"list bullet then span (no indent)", "* `code`", "* code", {"code", NULL}, false,
     false, false, 0, false, NULL},
    // The two leading spaces become one Indent() (leadSpaceCount/2), not
    // text: the empty pre-text range renders through the indent machinery
    // and the span piece loses the indent (inherited emphasis wart, design
    // 5.6). Indent() is a no-op stub, so the stream holds only "code".
    {"indented span", "  `code`", "code", {"code", NULL}, false, false, false, 0,
     false, NULL},
    {"indented bullet then span", "  * `code`", "code", {"code", NULL}, false, false,
     false, 1, false, NULL},
    // --- regression rows ---
    {"fenced code block", "```cpp\nx\n```\n", "x\n", {NULL}, false, false, false, 0,
     true, "cpp"},
    {"fence with backtick content", "```\n`not span`\n```", "`not span`\n", {NULL},
     false, false, false, 0, true, ""},
    {"4-backtick fence", "````\nouter\n````", "outer\n", {NULL}, false, false, false, 0,
     true, ""},
    {"emphasis regression", "*bold* text", "bold text", {NULL}, true, false, false, 0,
     false, NULL},
    {"span cannot cross lines", "`a\nb`", "`ab`", {NULL}, false, false, false, 0,
     false, NULL},
    // The raw '\r' byte is part of the rendered line (pre-existing parser-wide
    // behavior: only '\n' triggers the line-end render).
    {"CRLF unclosed span", "`unclosed\r\nnext", "`unclosed\rnext", {NULL}, false, false,
     false, 0, false, NULL},
    {"tab inside span", "`tab\tchar`", "tab\tchar", {"tab\tchar", NULL}, false, false,
     false, 0, false, NULL},
};

// ---------------------------------------------------------------------------
// Driver
// ---------------------------------------------------------------------------

static int g_failures = 0;

static void escape(const std::string& s, std::string& out) {
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        switch (c) {
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        case '\\':
            out += "\\\\";
            break;
        default:
            out += c;
            break;
        }
    }
}

static bool callPresent(int type, bool start) {
    for (size_t i = 0; i < g_calls.size(); ++i) {
        if (g_calls[i].type == type && g_calls[i].start == start) {
            return true;
        }
    }
    return false;
}

static void checkRow(const Row& row) {
    ImGui::g_input = row.input;
    ImGui::g_rendered.clear();
    ImGui::g_bullets = 0;
    g_calls.clear();

    ImGui::MarkdownConfig cfg;
    cfg.formatCallback = recordingCallback;
    ImGui::Markdown(row.input, strlen(row.input), cfg);
    ImGui::g_input = NULL;

    bool ok = true;

    // (a) exact rendered byte stream
    if (ImGui::g_rendered != row.expectedStream) {
        std::string want, got;
        escape(row.expectedStream, want);
        escape(ImGui::g_rendered, got);
        std::printf("FAIL [%s] stream: expected <%s> got <%s>\n", row.name, want.c_str(),
                    got.c_str());
        ok = false;
    }

    // (b) INLINE_CODE callback pairs wrap the right ranges
    std::vector<const Call*> starts;
    std::vector<const Call*> ends;
    for (size_t i = 0; i < g_calls.size(); ++i) {
        if (g_calls[i].type == (int)ImGui::MarkdownFormatType::INLINE_CODE) {
            (g_calls[i].start ? starts : ends).push_back(&g_calls[i]);
        }
    }
    if (starts.size() != ends.size()) {
        std::printf("FAIL [%s] unbalanced INLINE_CODE pairs: %zu starts, %zu ends\n",
                    row.name, starts.size(), ends.size());
        ok = false;
    }
    int expectedSpans = 0;
    while (row.spans[expectedSpans]) {
        ++expectedSpans;
    }
    if ((int)starts.size() != expectedSpans) {
        std::printf("FAIL [%s] INLINE_CODE span count: expected %d got %zu\n", row.name,
                    expectedSpans, starts.size());
        ok = false;
    } else {
        for (int i = 0; i < expectedSpans; ++i) {
            const Call* c = starts[(size_t)i];
            bool inBounds = c->textOffset >= 0 &&
                            c->textOffset + c->textLength <= (int)strlen(row.input);
            std::string got =
                inBounds
                    ? std::string(row.input + c->textOffset, (size_t)c->textLength)
                    : std::string();
            if (!inBounds || got != row.spans[i]) {
                std::string want, gotEsc;
                escape(row.spans[i], want);
                escape(got, gotEsc);
                std::printf("FAIL [%s] INLINE_CODE span %d: expected <%s> got <%s> "
                            "(offset %d, length %d)\n",
                            row.name, i, want.c_str(), gotEsc.c_str(), c->textOffset,
                            c->textLength);
                ok = false;
            }
        }
    }

    // optional per-row expectations
    if (row.expectEmphasis &&
        !(callPresent((int)ImGui::MarkdownFormatType::EMPHASIS, true) &&
          callPresent((int)ImGui::MarkdownFormatType::EMPHASIS, false))) {
        std::printf("FAIL [%s] expected an EMPHASIS callback pair\n", row.name);
        ok = false;
    }
    if (row.expectLink &&
        !(callPresent((int)ImGui::MarkdownFormatType::LINK, true) &&
          callPresent((int)ImGui::MarkdownFormatType::LINK, false))) {
        std::printf("FAIL [%s] expected a LINK callback pair\n", row.name);
        ok = false;
    }
    if (row.expectNoLinkEmphasis &&
        (callPresent((int)ImGui::MarkdownFormatType::LINK, true) ||
         callPresent((int)ImGui::MarkdownFormatType::EMPHASIS, true))) {
        std::printf("FAIL [%s] link/emphasis parsing must stay inside the span\n",
                    row.name);
        ok = false;
    }
    if (ImGui::g_bullets != row.expectBullets) {
        std::printf("FAIL [%s] bullet count: expected %d got %d\n", row.name,
                    row.expectBullets, ImGui::g_bullets);
        ok = false;
    }
    if (row.expectCodeBlock) {
        const Call* cb = NULL;
        for (size_t i = 0; i < g_calls.size(); ++i) {
            if (g_calls[i].type == (int)ImGui::MarkdownFormatType::CODE_BLOCK &&
                g_calls[i].start) {
                cb = &g_calls[i];
                break;
            }
        }
        const Call* cbEnd = NULL;
        for (size_t i = 0; i < g_calls.size(); ++i) {
            if (g_calls[i].type == (int)ImGui::MarkdownFormatType::CODE_BLOCK &&
                !g_calls[i].start) {
                cbEnd = &g_calls[i];
                break;
            }
        }
        if (!cb || !cbEnd) {
            std::printf("FAIL [%s] expected a balanced CODE_BLOCK callback pair\n",
                        row.name);
            ok = false;
        } else {
            bool idOk = cb->blockIdOffset >= 0 &&
                        cb->blockIdOffset + cb->blockIdLength <= (int)strlen(row.input) &&
                        std::string(row.input + cb->blockIdOffset,
                                    (size_t)cb->blockIdLength) == row.blockId;
            if (!idOk) {
                std::printf("FAIL [%s] CODE_BLOCK blockId: expected <%s>\n", row.name,
                            row.blockId);
                ok = false;
            }
            if (cb->textLength <= 0) {
                std::printf("FAIL [%s] CODE_BLOCK content should be non-empty\n",
                            row.name);
                ok = false;
            }
        }
    }

    if (ok) {
        std::string want;
        escape(row.expectedStream, want);
        std::printf("PASS [%s] stream <%s>\n", row.name, want.c_str());
    } else {
        ++g_failures;
    }
}

int main() {
    const size_t rowCount = sizeof(ROWS) / sizeof(ROWS[0]);
    for (size_t i = 0; i < rowCount; ++i) {
        checkRow(ROWS[i]);
    }
    if (g_failures == 0) {
        std::printf("ALL RUNTIME CHECKS PASSED (%u rows)\n", (unsigned)rowCount);
        return 0;
    }
    std::printf("%d RUNTIME CHECK(S) FAILED\n", g_failures);
    return 1;
}
