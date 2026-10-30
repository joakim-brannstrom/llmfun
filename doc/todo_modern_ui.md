# Appendix: The Workbench — a modern UI for llmfun

Status: design proposal — no code written yet.
Date: 2026-10-24.
Scope: the two render profiles (`cpp_tui/`: ImTui/ncurses and GLFW/OpenGL3), the C API
between D and C++, and the D-side UI message stream.
Related: `doc/tui_design.md` (current implementation), `doc/slash_commands.md`,
`doc/sessions.md`, `doc/todo.md`, `plan/appendix_agent_runtime.md`.
Audience: maintainers and whoever implements this.

How to read: §0–§3 are the argument and the new shell; §4–§7 are the design system and
the component catalogue; §8–§9 are the architecture and the plan. In a hurry: §0, §3.1,
§4.2, §9, §11.

Contents:

- [0. TL;DR](#0-tldr)
- [1. Diagnosis: why the current UI feels ad-hoc](#1-diagnosis-why-the-current-ui-feels-ad-hoc)
- [2. The idea: from chat log to workbench](#2-the-idea-from-chat-log-to-workbench)
- [3. The shell](#3-the-shell)
- [4. The design language: Ink](#4-the-design-language-ink)
- [5. Components](#5-components)
- [6. Signature interactions](#6-signature-interactions)
- [7. One spec, two fidelities](#7-one-spec-two-fidelities)
- [8. Architecture: how to build it without rewriting the world](#8-architecture-how-to-build-it-without-rewriting-the-world)
- [9. Roadmap](#9-roadmap)
- [10. Risks, non-goals, decisions](#10-risks-non-goals-decisions)
- [11. Final honest points](#11-final-honest-points)
- [Appendix A — P0 quick-win checklist](#appendix-a--p0-quick-win-checklist-concrete-low-risk)
- [Appendix B — v5 header sketch](#appendix-b--v5-header-sketch-for-review)
- [Appendix C — File fate](#appendix-c--file-fate)
- [Appendix D — Design review questions](#appendix-d--design-review-questions)

---

## 0. TL;DR

The current UI is a **chat log with a sidebar**. It is rendered twice — once into a
terminal cell grid, once into an 800×600 GLFW window with an embedded Roboto font — from
the same absolute-positioned layout code. Its visual language is: saturated header
bars for roles, collapsing headers for everything else, text-as-buttons, a run-on
status sentence, and an input field that reads as an empty hole.

The root cause is not Dear ImGui and not ncurses. It is the **boundary**. D sends a
seven-value type enum, a summary string, a body and a thinking blob — every other
structural fact is flattened into text before it crosses the C ABI:

```
"[3] read_file(tui_chat.cpp) [1.1s]"
"tool result ✅ writeFile: 42 lines"
"Context: 42k/128k tokens | 38.1 tok/s | Model: 'qwen3-coder' | Busy"
```

The UI cannot express structure it never receives. So it invents hierarchy from
string prefixes, renders some role headers black-on-dark, paints whole backgrounds to
say "this one is the answer", and hides other headers by making their text transparent.

This proposal does three things:

1. **A new information architecture — the Workbench.** A rail (where you have been),
   a thread of **turns** (ask → work → answer), an **inspector** (what the agent knows
   and did), a composer that can be seen, and a **now bar** that says what is
   happening at this second. Not a chat window with extras: a quiet instrument for
   watching an agent work.
2. **A design system — Ink.** Tokens, type, spacing, glyphs, motion — specified in
   **cells and pixels** so the terminal and the window become two fidelities of one
   design instead of two copies of the same hacks.
3. **A typed presentation model — UI API v5.** Turns and blocks, not summary strings:
   tool name, arguments, status, duration, tokens, sources, agents. This is what makes
   the UI able to look modern at all.

The report contains before/after mockups, a component catalogue, the colour and
type systems with measured contrast, the API sketch, a file-level implementation
plan with an explicit **MVP / merge gate** (≈5.5 weeks including one week of dogfood) and
the post-MVP program (≈10–11.5 weeks total), risks and non-goals.
The short version, if you read nothing else:

- Make the **composer** visible, make **Enter send**, make **Esc stop**.
- Kill **transparent headers** and **text-as-buttons**.
- Replace **role-coloured bars** with neutral surfaces and a thin role rule.
- Turn `Assistant: 4` accordions into a **run ribbon** + tool cards.
- Make the **status line** four chips and a **context meter**.
- Add a **command palette** so the product's capabilities stop being folklore.
- Split the logs: **Activity** for users, an **opt-in Raw Log** for developers.
- Send **structure**, not sentences, across the C boundary.

---

## 1. Diagnosis: why the current UI feels ad-hoc

### 1.1 What it looks like today (real render, not a mockup)

The following is a dump of the actual render pipeline — `ImTui_ImplText_RenderDrawData`
into a 100×30 `TScreen`, seeded with a few messages, sessions and a busy status. It was
produced headlessly with the committed TUI library (the same technique the
`test_*` harnesses use). Markdown falls back to plain text in this harness (no
`RichMd::Context`), so heading styling is absent; everything else is the real layout.

```
 0| Chat  Log
 1| New                       <   v
 2|Sessions --------------------  What would a modern UI look like? The current one feels ad-hoc and
 3|                               ugly.
 4| Fix the parser p... [3] .       Copy
 5| look at the failing ...       >  Assistant: 4
 6| RAG index redesign [5]  .     v
 7| add chunk overlap             ## Proposal
 8| Plan: modern UI [7]     .
 9| what would a modern ...       A three-column shell with a command palette.
10| Windows build [9]       .
11| link libglfw                  - sidebar
12|                               - thread
13|                               - inspector
14|                                 Copy
15|                               v
16|                               Writing the report now, section by section
17|                                 Copy
18|                              Thinking 0s
...
25|                              Context: 42k/128k tokens | 38.1 tok/s | Model: 'qwen3-coder' | Busy
26|                                                                                           Send
27|                                                                                           Prev
28|                                                                                           Next
29|
```

What to notice, row by row:

1. **Row 0** — navigation is a menu bar with two underlined words. Tabs are menus, and
   the only discoverable actions live in them.
2. **Rows 1–17** — the session rail is 30 columns of two-line buttons. Titles are
   ellipsized, counts are appended (`[3]`), previews are dimmed, and a `.` is the
   always-visible row menu. There are no dates, no groups, no running indicator, and
   no scrollbar — the design doc itself records that rows below the terminal height
   are unreachable (`doc/tui_design.md`, "Scrolling").
3. **Row 1 / row 5** — the first user message has **no visible header**, only a dangling
   `v`. The header is drawn *unframed* (no background fill) while the role colour for
   user/final-answer is `darkText` (black); black on the dark surface is invisible. The
   final answer's header disappears the same way. Separately, the code's "hide this
   header" mode sets the text alpha to 0 and leaves the arrow, which is where the
   floating glyphs come from (`tui_chat.cpp:247-266`, `:638-645`).
4. **Row 5** — the agent's work is an accordion labelled `Assistant: 4`. The most
   important narrative in the product — what the agent *did* — is a magic number.
5. **Rows 6, 15** — the final answer and the live stream are separated by bare `v`
   glyphs; `Copy` buttons float under every block in default widget style.
6. **Row 18** — `Thinking 0s` is raw text, not a state.
7. **Row 25** — the status is a sentence. Four unrelated facts (context, speed, model,
   busy) share one line and one colour.
8. **Rows 26–28** — `Send`, `Prev`, `Next` are a vertical stack of 1-row buttons at the
   far right; the input field is a dark-grey rectangle (background only — this dump
   prints glyphs, not cell colours) with **no border, no placeholder, no label, and no
   caret until clicked** — it reads as an empty hole. The Send widget is literally a
   read-only `InputText`
   (`tui_chat.cpp:817-823`) because that was the only way to make Tab reach it in
   imtui.
9. **The whole screen** — no gutter between the rail and the thread, no padding inside
   regions, no vertical rhythm. Text touches boxes; boxes touch the window edge.

Compare this with the same content and the same 100×30 terminal in §3.2.

In the GUI the same layout gets 800×600 pixels, Dear ImGui's default dark style with
~20 colour overrides, square corners, no typography, and a single font — Roboto-Medium,
one weight, one size. The GUI is not a different design; it is the TUI with bigger
cells.

### 1.2 Root causes (not symptoms)

| # | Symptom | Root cause | Where |
|---|---------|-----------|-------|
| 1 | Roles shown as full coloured bars | Style model says "each message type has a background"; the palette (`#73B3E6` sky blue, `#4DF24D` neon green) does the hierarchy that typography should do | `tui.h:50-95` (`ChatMessageStyle`) |
| 2 | Headers vanish; arrows float | Role colour is `darkText` for user/final-answer, but the header is drawn unframed (no fill), so the label is black-on-dark and invisible; the "hidden header" mode sets alpha 0 and leaves the arrow | `tui_chat.cpp:80-100`, `247-266`, `638-645` |
| 3 | Buttons look like text; hit boxes are one cell | Custom `renderButton` draws a blank ImGui button then overlays text; a second variant exists for title highlighting | `tui_widgets.cpp:7-38`, `session_panel.cpp:25-76` |
| 4 | Layout is absolute and brittle | Every region is `SetCursorPos(DisplaySize - ...)` with profile-specific constants; 20 `tuiIsTextGrid()` gates, 17 `colUnits()` conversions, 12 `audit:` comments justify metrics per widget | `tui_chat.cpp:507-858`, `session_panel.cpp` |
| 5 | One left panel only | Session rail and pipeline panel are mutually exclusive; showing agents hides sessions | `doc/tui_design.md`, "Mutual Exclusion" |
| 6 | Status is a sentence | D formats `"Context: …"`, `"38.1 tok/s"`, `"Model: …"`, `"Busy"` into one line; the UI can only print it | `app_agent/ui.d:421-425` |
| 7 | Tool calls are two chat messages | D flattens `StreamToolCall` / `ToolResponse` into strings with emoji (`✅`/`❌`) before they cross the boundary | `app_agent/package.d:280-294`, `ui.d:440-451` |
| 8 | No surface for the product | Tools, RAG, memory, skills, agents, metrics exist but appear only as chat text or typed commands | `doc/slash_commands.md` |
| 9 | The GUI looks like a debug tool | 800×600 default, one embedded font (Roboto-Medium), `StyleColorsDark` + overrides, no radii/shadows/type scale | `tui_backend_gui.cpp:37-49,73-88`, `tui.cpp:61-116`, `tui_chat.cpp:395-425` |
| 10 | The design doc is 1,171 lines | It documents the workarounds: cell/pixel gates, patches, accepted degradations | `doc/tui_design.md` |

Two more structural observations:

- **The UI is a renderer of history, not of work.** It only knows "messages". It cannot
  say "the agent is running `dub test` right now" because that fact has been reduced to
  a growing string in a temporary message.
- **The terminal is treated as a degraded GUI.** Hence the gating, the special cases,
  the "Terminal too small!" error page — instead of the terminal being the reference
  design with the GUI as the enriched one.

### 1.3 What is genuinely good and must survive

This is not a teardown. Several decisions are better than most terminal UIs:

- **The backend seam** (`cpp_tui/tui_backend.h`): init/newFrame/renderFrame/shutdown,
  one state owns its backend, Auto → GUI → TUI fallback resolved in C++ with precise
  reasons. Keep it; the new UI is still one core, two backends.
- **The pure C API**: POD `String` (ptr+len), opaque `TuiState*`, thread-local errors,
  additive versioning, D `ImportC`. It is a clean, testable boundary. Extend it; do not
  replace it with a framework.
- **Text-grid discipline**: 1-cell fonts, row-exact markdown, a `TScreen` that can be
  dumped in tests. This is why golden-screen testing is possible at all.
- **Real persistence and real data**: sessions, RAG, dialogue/reasoning indexes, tool
  metrics, pipelines. The new UI has something worth showing.
- **No heavyweight dependencies**: ncurses, Dear ImGui, GLFW, OpenGL. A single 50 MB
  binary that runs over SSH *and* on a desktop. Do not trade this away.

---

## 2. The idea: from chat log to workbench

### 2.1 Thesis

llmfun is not a chat app. It is an agent that reads files, runs commands, searches a
RAG, remembers across sessions, plans, codes, and runs multi-agent pipelines. The current
UI models the least interesting part — the text — and hides the rest.

The new UI models **the run**: what was asked, what the agent did, what it found, what
it produced, what it cost, and what it still needs from you.

> **A quiet instrument for watching an agent think and work.**
> The conversation is the spine; the work is inspectable in place.

### 2.2 Three pillars

1. **Thread as a run journal.** The unit is a *turn*, not a message: Ask → Work →
   Answer, with a one-line run ribbon and expandable detail. Long agent runs become
   readable narratives instead of 40 collapsible rows.
2. **Peripheral awareness.** The rail answers "where have I been?", the inspector
   answers "what does it know / what did it do?", the now bar answers "what is happening
   right now?". None of them require leaving the thread.
3. **Ambient command.** A command palette, inline slash autocomplete, a search that
   spans sessions *and* the dialogue/reasoning indexes, and a keymap overlay. Every
   capability is discoverable without a manual.

### 2.3 Jobs to be done

| The user's question | The UI answer |
|---|---|
| What is it doing right now? | Now bar: phase, current tool, elapsed, tok/s, Stop |
| Why does it believe that? | Answer card → *Why?* → Sources + Reasoning + Memory for that turn |
| What did it change? | Tool card → Canvas with diff, file, command output |
| Where was that session? | Rail with groups/search; universal search over dialogue + reasoning |
| How much context is left? | Context Lens with segment breakdown and compaction marker |
| Can I trust this action? | Approval cards for destructive tools; error cards with Retry |
| I want to read / demo this | Focus mode; density switch; light theme; clean screenshots |
| How do I do X? | Palette (Ctrl+K), `?` keymap, inline `/` autocomplete with descriptions |

### 2.4 Design principles — the ten rules

1. **The thread is the hero; chrome recedes.** No permanent decoration; regions earn
   their width.
2. **Rank with type and space before colour.** Size, weight, spacing, then hue.
3. **One accent.** Accent marks interaction and the active state. Nothing else.
4. **Every state is visible; nothing essential lives in hover.** Hover is a shortcut,
   never the only path.
5. **Every action is a command; every command is discoverable.** Palette + keymap +
   buttons are three views of one registry.
6. **Cells and pixels are two fidelities of one spec.** No component exists only in one.
7. **No dead ends.** Every error card has a next step; every empty state has a start.
8. **Motion explains change; it never decorates.** 120–180 ms, ease-out, reduced-motion
   aware.
9. **Density is a choice**, not an accident of layout arithmetic.
10. **The terminal is a first-class citizen, not a fallback.** The TUI is where this
    tool lives; the GUI is the same instrument with better typography.

### 2.5 Anti-patterns we will not ship

- Chat bubbles with tails, avatars of robots, gradient glows.
- Neon role backgrounds; meaning encoded in hue alone.
- Headers made invisible; text-as-button; hover-only actions.
- A status line that is a sentence.
- Horizontal scrolling for code; tables collapsed to one cell.
- Modal dialogs for simple choices; settings you cannot search.
- Emoji in the UI (`✅`/`❌` are in the current tool result strings); they break cell
  widths and render differently per terminal.

---

## 3. The shell

### 3.1 GUI wireframe (1280×820)

```
┌────────────────────────────────────────────────────────────────────────────────────────┐
│ llmfun   Plan: modern UI ✎                              Ctrl+K  Search…   ◐  ⚙         │
├────────────────────────────────────────────────────────────────────────────────────────┤
│ / Search…             │ YOU · 09:41                           │ CONTEXT                │
│                       │ what would a modern UI look like?     │ 42k / 128k             │
│ PINNED                │                                       │ ▓▓▓▓▓▓░░░░             │
│  ● Plan: …            │ ✓ 12.4s · 6 tools · 1 file · 3 sources│ compact at 115k        │
│ TODAY                 │ ┌ WORK ────────────────────────┐      │                        │
│  Fix parser           │ │ ✓ read_file   tui_chat.cpp   │      │ TOOLS          6       │
│  RAG redesign         │ │ ✓ executeCommand  dub test   │      │ read_file     ×3       │
│ YESTERDAY             │ └──────────────────────────────┘      │ execute…      ×1       │
│  Windows build        │                                       │ writeFile     ×1       │
│                       │ ANSWER                                │                        │
│                       │ The shell is three regions: a rail    │ SOURCES        3       │
│                       │ for where you have been, a thread     │ tui_chat.cpp 0.91      │
│                       │ for the run, and an inspector for     │ README.md    0.84      │
│                       │ what the agent knows…                 │ sessions.md  0.72      │
│                       │                                       │                        │
│                       │ ┌ code ────────────── copy ┐          │ AGENTS         2       │
│                       │ │ auto shell = …           │          │ planner     done       │
│                       │ └──────────────────────────┘          │ coder        run       │
│ + New session         │                                       │                        │
│                       │ qwen3-coder · 42k → 48k · 12.4s       │                        │
├────────────────────────────────────────────────────────────────────────────────────────┤
│ Ask anything…   / commands   @ files                   Plan ▸ qwen3-coder  ▸ Send      │
│ ctx ▓▓▓▓░░ 42k                                       ◐ streaming · 38.1 t/s            │
├────────────────────────────────────────────────────────────────────────────────────────┤
│ Enter send · ⇧Enter newline · Esc stop · ? help                                        │
│                                                                                        │
└────────────────────────────────────────────────────────────────────────────────────────┘
```

### 3.2 TUI wireframe (110×30, real cell layout)

```
┌────────────────────────────────────────────────────────────────────────────────────────────────────────────┐
│ llmfun · Plan: modern UI                                            Ctrl+K  commands      ● running 00:12  │
├────────────────────────────────────────────────────────────────────────────────────────────────────────────┤
│ SESSIONS                 │ ASK  09:41                                         │ CONTEXT                    │
│ / search…                │ what would a modern UI look like?                  │ 42k / 128k                 │
│ ▸ Fix the parser         │                                                    │ ██████░░░░                 │
│ ▸ RAG redesign           │ ✓ 12.4s · 6 tools · 1 file · 3 sources             │ compact at 115k            │
│ ● Plan: modern UI        │ ▾ WORK                                             │                            │
│ ▸ Windows build          │   ✓ read_file       tui_chat.cpp    240ms          │ TOOLS          6           │
│ ──────────────────────── │   ✓ executeCommand  dub test       9.1s            │ read_file     ×3           │
│ PINNED                   │   ✓ writeFile       docs/ui.md       12ms          │ executeCommand ×1          │
│ ▸ Memory tiers           │                                                    │ writeFile     ×1           │
│ TODAY                    │ ANSWER                                             │                            │
│ ▸ MCP server docs        │ The shell is three regions: a rail                 │ SOURCES        3           │
│ ▸ Refactor broker        │ where you have been, a thread for the              │ tui_chat.cpp 0.91          │
│                          │ run, and an inspector for what the                 │ README.md    0.84          │
│                          │ agent knows. The thread is a journal.              │ sessions.md  0.72          │
│                          │                                                    │                            │
│                          │ ┌ code ────────────────── copy ┐                   │ AGENTS         2           │
│ + New session            │ │ auto shell = ui::shell(state); │                 │ planner     done           │
│                          │ └────────────────────────────────┘                 │ coder        run           │
│                          │                                                    │                            │
│                          │ qwen3-coder · 42k → 48k · 12.4s                    │                            │
├────────────────────────────────────────────────────────────────────────────────────────────────────────────┤
│ Ask anything…   / commands   @ files                                           Plan ▸ qwen3-coder          │
│ ctx ████░░ 42k                                                                 ◐ streaming 38.1 tok/s      │
├────────────────────────────────────────────────────────────────────────────────────────────────────────────┤
│ Enter send · Shift+Enter newline · Esc stop · ? help                           ◐ 38.1 tok/s · 42k          │
│                                                                                                            │
└────────────────────────────────────────────────────────────────────────────────────────────────────────────┘
```

Both screens show the same information, in the same order, with the same words. That is
the contract. The GUI adds typography, radii, hover and shadows; the TUI adds no features
and loses no information. (GUI heights: top bar 48 px, now bar 28 px.)

### 3.3 Regions and responsive rules

| Region | GUI | TUI | Behaviour |
|---|---|---|---|
| Top bar | 48 px | 1 row | Session title (inline-editable), palette button, run state, settings |
| Rail | 208 px (26 gu; 56 collapsed) | 26 cells (7 collapsed) | Sessions; `Ctrl+B` toggles; persisted per profile |
| Thread | flex, max 800 px (100 gu) content | flex, max 100 cells content | The run journal; centered when wide |
| Inspector | 224 px (28 gu) | 28 cells | Context / Tools / Sources / Memory / Agents / Activity; `Ctrl+I` toggles |
| Composer | 96–240 px (text 1–8 rows) | 3–10 rows (text 1–8) | Grows with content, sticky, never overlapped |
| Now bar | 28 px | 1 row | Phase, current tool, elapsed, tok/s, Stop, key hints |

The **Raw Log** (§5.29) is not a permanent region: it is an opt-in drawer/overlay (GUI:
a conditional bottom row that resizes the thread; TUI: a full-screen overlay), closed by
default.

Breakpoints (same intent, different units):

| Width | GUI | TUI |
|---|---|---|
| ≥ 1200 px / ≥ 100 cells | rail + thread + inspector | rail + thread + inspector |
| 900–1199 px / 80–99 cells | rail + thread; inspector as overlay | rail collapsed to 7 cells; inspector overlay |
| 640–899 px / 60–79 cells | rail collapsed; inspector overlay | single column; rail/inspector overlays |
| < 640 px / < 60 cells | below the 640 px window minimum | **compact mode**: last turn + composer + now bar |
| < 40×15 cells | — | still the documented error page (backend floor), but only below it |

The current 40×15 "Terminal too small!" page moves from *the* small-screen behaviour to
*the last resort*. Between 40×15 and 80×24 the UI degrades to compact mode instead of
refusing to render.

### 3.4 Focus mode

`F11` (GUI) and `Ctrl+Shift+M` (TUI, where F11 belongs to the terminal): hides the rail,
inspector and top bar, centers the thread at 90 cells / 720 px, keeps the composer and now
bar. `Esc` exits. This is the "read the long answer" and "demo" mode; it also proves that
the thread can stand alone.

### 3.5 What replaces what

| Today | New |
|---|---|
| Menu bar `Chat / Log / View` | Top bar + palette; the user-facing Log becomes **Activity**; the raw stream becomes the opt-in **Raw Log** (§5.29) |
| Raw log stream (today's Log tab) | **Raw Log** drawer/overlay — developer-only, closed by default, never mixed with Activity |
| Pipeline panel (takes the rail slot) | Agent lanes in the inspector + a compact run ribbon |
| `Assistant: 4` accordion | Run ribbon + Work section with tool cards |
| Native rich_md tables (both profiles) | GUI keeps rich_md native tables; TUI uses the custom text-grid table block (§5.31) |
| Status sentence | Now bar + Context Lens + status chips |
| `Thinking 0s` | Now bar phase + per-block durations |
| `/help` wall of text | Palette, `?` keymap overlay, inline `/` completion |
| Options window (font scale) | Settings sheet (searchable) + theme/density |

---

## 4. The design language: **Ink**

Ink is the visual system. It has three themes — **Graphite** (default dark),
**Parchment** (light), **Native** (the terminal's own palette) — and one rule: a token
is the only way a colour, size or distance enters a component.

### 4.1 Aesthetic direction

Quiet, precise, editorial. Think a well-set technical book crossed with a debugger:
warm neutrals, generous line height, a single cool accent, thin rules, no chrome that
does not carry information. The agent's work is the colour; the shell is graphite.

Deliberately *not*: a ChatGPT clone, a Slack clone, a cyberpunk terminal, a glassmorphic
dashboard. The identity is "instrument", not "assistant".

In one paragraph, the GUI: a near-black graphite canvas; a 208 px (26 gu) rail of 13 px labels
and 12 px meta; a centred 800 px (100 gu) thread set in 15/24 Inter; hairline rules; one azure
accent on the primary action and the active row; answer cards that are one surface step
lighter with a 10 px radius; an inspector of quiet meters and ranked lists; hover that
lifts a row by 4% and fades in 120 ms. No gradients, no glow, no decoration that does not
carry information. The only saturated colour in the window is the agent's work.

| Aspect | Today | Workbench |
|---|---|---|
| Window | 800×600, no minimum | 1280×820, min 640×480, persisted |
| Fonts | one Roboto weight, one size | Inter 3 weights + JetBrains Mono, 6-step scale |
| Colour | ~25 hard-coded ImGui colours | 20 semantic tokens, 3 themes, measured contrast |
| Shape | 0 radius, no elevation | 4/6/10/12 px radii, surface steps, overlay shadows |
| Spacing | ImGui defaults + per-widget math | 8 px grid (1 gu), 2 densities |
| Navigation | menu bar `Chat / Log / View` | top bar + palette + `?` keymap |
| Messages | collapsing headers | turn cards, run ribbon, tool cards, answer card |
| Composer | dark box, fake Send, no placeholder | chips, completions, context meter, real Send/Stop |
| Status | one sentence | now bar + Context Lens + state glyphs |
| Extras | none | inspector, canvas, minimap, universal search |

### 4.2 Colour tokens — Graphite (dark)

| Token | Hex | ANSI-256 (approx.) | Contrast on surface | Use |
|---|---|---|---|---|
| `ink.canvas` | `#0B0D10` | 232 | — | Window background |
| `ink.surface` | `#12161C` | 233 | — | Rail, inspector, top/now bars |
| `ink.raised` | `#171C23` | 234 | — | Cards, composer, popovers |
| `ink.inset` | `#0E1116` | 232 | — | Code blocks, inputs, wells |
| `line.soft` | `#242B34` | 236 | 1.4:1 | Decorative hairlines, region separators |
| `line.strong` | `#5C6676` | 242 | **3.1:1** | Essential borders: inputs, cards, splitters |
| `text.hi` | `#E9EDF3` | 255 | **15.4:1** | Body, headings |
| `text.mid` | `#A8B1C0` | 247 | **8.4:1** | Secondary, previews |
| `text.low` | `#77808F` | 244 | **4.6:1** | Meta, timestamps |
| `text.dim` | `#545D6B` | 240 | 2.7:1 | Decorative only — never text |
| `accent` | `#6AA6FF` | 75 | **7.4:1** | Primary action, active row, links |
| `accent.quiet` | `#6AA6FF` @ 14% | — | — | Active row / selection fill |
| `role.you` | `#B49CFF` | 147 | **7.9:1** | User rule + label |
| `role.agent` | `#6AA6FF` | 75 | **7.4:1** | Agent rule + label |
| `role.tool` | `#6FC7B6` | 79 | **9.1:1** | Tool rule + label |
| `role.system` | `#8A94A6` | 246 | **5.9:1** | Notices, system |
| `state.ok` | `#4CC38A` | 78 | **8.2:1** | Success glyphs |
| `state.warn` | `#E5A458` | 179 | **8.5:1** | Warnings, compaction |
| `state.danger` | `#F06A6A` | 203 | **6.0:1** | Errors, destructive |

The contrast column is measured (WCAG 2.1 relative luminance), not estimated. Body text
targets ≥ 4.5:1; UI boundaries ≥ 3:1; `text.dim` is explicitly not allowed on text.

**Role is a 2 px (GUI) / 1 cell (TUI) rule and a label, never a filled background.** A
turn looks like:

```
▌ YOU   09:41
  what would a modern UI look like?
```

The rule is the only saturated element. This single change removes 80% of the "ugly".

### 4.3 Colour tokens — Parchment (light)

| Token | Hex | Contrast on `#FFFFFF` |
|---|---|---|
| `paper.canvas` | `#F7F5F0` | — |
| `paper.surface` | `#FBFAF7` | — |
| `paper.raised` | `#FFFFFF` | — |
| `line.soft` | `#E6E2D8` | decorative |
| `line.strong` | `#8F877A` | **3.6:1** (essential borders) |
| `text.hi` | `#16181D` | **17.8:1** |
| `text.mid` | `#4B5361` | **7.8:1** |
| `text.low` | `#646D7C` | **5.2:1** |
| `accent` | `#1F5FBF` | **6.1:1** |
| `role.you` | `#6B4FD8` | **5.6:1** |
| `role.agent` | `#1F5FBF` | **6.1:1** |
| `role.tool` | `#0F766E` | **5.5:1** |
| `role.system` | `#5A6473` | **6.0:1** |
| `state.ok` | `#1E7A4D` | **5.3:1** |
| `state.warn` | `#9A5B00` | **5.4:1** |
| `state.danger` | `#B3261E` | **6.5:1** |

Light mode is not an inversion: it is a paper surface with ink text and the same semantic
roles. Syntax highlighting switches to a light-optimised set (not the dark set brightened).

### 4.4 Colour tokens — Native (terminal)

`theme: native` uses only the terminal's 16 ANSI colours, so the UI inherits the user's
solarized/gruvbox/nord/whatever theme:

| Token | ANSI |
|---|---|
| canvas / surface | default background |
| raised | bright black (or reverse of default) |
| text.hi / mid / low | default foreground / white / bright black |
| accent | bright blue (fallback: bright cyan) |
| role.you | bright magenta |
| role.agent | bright cyan |
| role.tool | bright green |
| role.system | white |
| ok / warn / danger | bright green / bright yellow / bright red |

Plus attributes: labels **bold**, meta **dim**, active row **reverse**, focus **reverse +
bold**. No RGB is emitted in this theme, so `NO_COLOR` users still get a coherent UI
(the "no colour" variant of Native: attributes only).

### 4.5 Colour depth and degradation

The backend detects, in order: `NO_COLOR` → attributes only; `COLORTERM=truecolor|24bit`
→ RGB; `TERM` containing `256color` → xterm-256; otherwise 16 colours. The mapping is a
function of the token table, generated at build time into `ui/tokens_generated.h`, so no
component ever hard-codes an escape code. GUI is always RGB.

Rules that keep role colour meaningful in 16 colours: never rely on hue alone. Every
role has a **label** (`YOU`, `AGENT`, `TOOL`, `SYSTEM`), and every state has a **glyph**
(`✓`, `✕`, `◐`, `!`). Colour is redundant reinforcement.

### 4.6 Typography

GUI fonts (embedded, SIL OFL, subset to Latin + punctuation with a full-font fallback for
other scripts; fall back to the system font if the asset is missing):

- **UI**: Inter — Regular 15 px, Medium 13 px, SemiBold 20 px.
- **Mono**: JetBrains Mono — Regular 13 px.
- Fallback chain: system UI font (`Segoe UI`, `SF Pro Text`, `Cantarell`, `Noto Sans`),
  then the embedded Roboto-Medium that rich_md already ships.

| Style | GUI | TUI |
|---|---|---|
| Display (empty-state title) | Inter SemiBold 26/32 | bold, 1 row |
| Title (answer heading) | Inter SemiBold 20/28 | bold, 1 row |
| Body | Inter Regular 15/24 | 1 row per line |
| UI / label | Inter Medium 13/20 | bold, 1 row |
| Meta | Inter Regular 12/18 | dim, 1 row |
| Code | JetBrains Mono 13/20 | 1 row per line |

Line length: 60–100 characters. The GUI centers the thread at max 800 px (100 gu, ≈ 100
characters at 15 px Inter); the TUI caps content at 100 cells and centers it when the
terminal is wider. Long code lines wrap (GUI) or are clipped with a `…` marker (TUI) —
never a horizontal scrollbar across the whole thread.

Typography rules:

- Sentence case everywhere. No ALL CAPS except 10 px role labels (`YOU`, `TOOL`).
- Numerals tabular in metrics (`38.1 tok/s`, `12.4 s`).
- No more than three type sizes in one view.
- Headings in markdown keep their semantic role but use a restrained scale: h1 20 px,
  h2 17 px, h3 15 px bold; the TUI renders all of them 1 row, bold only for h1/h2.
- Links are accent-coloured and underlined on hover only; the TUI uses accent +
  underline.

### 4.7 Space, shape, elevation

**The grid unit (gu): 1 gu = 1 terminal cell = 8 px at 100% GUI scale.** This is the
bridge that makes one spec renderable twice. Vertically, 1 **row** = 1 terminal row =
24 px (GUI body line height). All component specs use `gu` and `rows`.

| Token | gu | GUI px | TUI cells |
|---|---|---|---|
| `space.0` | 0 | 0 | 0 |
| `space.1` | 0.5 | 4 | 0 (rounds to 0 — only for GUI-only optics) |
| `space.2` | 1 | 8 | 1 |
| `space.3` | 1.5 | 12 | 1 |
| `space.4` | 2 | 16 | 2 |
| `space.5` | 3 | 24 | 3 |
| `space.6` | 4 | 32 | 4 |
| `space.7` | 6 | 48 | 6 |

TUI rounding: 0.5 → 0, 1.5 → 1, 2.5 → 2 (floor), so the grid stays exact. A component
that needs `space.1` in the TUI simply has no padding — which is correct on a cell grid.

Shape:

- radius: `4 px` chips, `6 px` buttons/inputs, `10 px` cards, `12 px` panels, `999 px`
  pills. TUI: square corners (box drawing has no radii).
- borders: 1 px `line.soft`; focus: 2 px `accent` at 60%; selected: 2 px `accent`.
- elevation: surface steps (`canvas → surface → raised`) + a 1 px hairline; overlays
  (palette, popovers, toasts) add `0 8px 24px rgba(0,0,0,0.45)`. No shadows on inline
  cards. TUI overlays use a double-line border (`╔═╗`) or reverse-video title.
- no nested cards. If a card needs a card, it is a list row instead.

Density:

| | Comfortable | Compact |
|---|---|---|
| Turn gap | 16 px / 2 rows | 8 px / 1 row |
| Card padding | 12 px / 1 row | 8 px / 1 row |
| Session row | 2 lines (title + preview) | 1 line (title) |
| Inspector rows | 24 px | 20 px |
| TUI default | 2-line sessions, 1 blank row between turns | 1-line sessions, no blank row |

Density is a persisted setting; the TUI picks Compact automatically below 30 rows.

### 4.8 Iconography and glyphs

A small, single-width set. Every glyph has an ASCII fallback and a text label where the
meaning is critical. No emoji, ever (they are double-width in many terminals and render
as colour cartoons in the GUI's text font).

| Meaning | Unicode | ASCII | Used in |
|---|---|---|---|
| Collapsed / expand | `▸` `▾` | `>` `v` | Disclosures, tree rows |
| Success | `✓` | `+` | Tool card, run ribbon |
| Error | `✕` | `x` | Tool card, error card |
| Running | `◐ ◓ ◑ ◒` | bar, slash, dash, backslash | Now bar, tool card, session dot |
| Tool | `◆` | `*` | Block label, inspector tab |
| File | `▤` | `=` | Diff/file cards, sources |
| Terminal | `▶` | `>` | Command cards |
| Search | `/` | `/` | Palette, rail search |
| Agent | `✦` | `*` | Agent lanes |
| Duration | `◷` | `@` | Meta rows |
| Copy | `c` | `c` (same) | Hover actions |
| Warning | `!` | `!` | Context threshold, notices |
| Caret | `▍` | vertical bar | Streaming |
| Dot | `●` | `o` | Active session, unread |

The **wordmark** is simply `llmfun` in Inter SemiBold with a 6 px accent square to its
left in the GUI; in the TUI it is `llmfun` in bold. No logo project.

### 4.9 Motion

| Event | GUI | TUI |
|---|---|---|
| Hover / focus | 120 ms ease-out colour/background | instant |
| Panel / inspector toggle | 160 ms ease-out width/opacity | instant (redraw) |
| Palette open | 120 ms fade + 8 px rise | instant |
| Toast | 200 ms in, 120 ms out | 3 s status flash |
| Spinner | 900 ms/turn, only while busy | 4 frames @ 150 ms, only while busy |
| Caret | 1 s blink | steady block (no blink by default) |
| Scroll | smooth wheel (GUI) | cell scroll (terminal) |

`reduced motion` (setting, default follows the OS where detectable): no fades, no spinners
(static `◐`), no smooth scroll. The TUI already refreshes at 3 fps when idle; motion must
never force a higher frame rate.

---

## 5. Components

A component is specified once, as anatomy + states + a cell/pixel table. The implementation
lives in `ui/primitives.*` and `views/*`, never in a screen's layout code.

### 5.1 Top bar

**Anatomy:** `[mark] llmfun` · session title (inline editable, truncates with tooltip) ·
spacer · `Ctrl+K Search` · run pill (`● running 00:12` / `Ready`) · `RAW` badge (only
while the developer raw log is open) · settings gear.

- 48 px / 1 row. Background `surface`, bottom hairline.
- The title is the session title; for a pipeline run it becomes `Plan: <title>` with a
  breadcrumb to the main thread. Window title mirrors it (fixes `doc/todo.md`: "the title
  in the GUI should contain the session name").
- New sessions derive the title from the first user message, as today. An agent-proposed
  title is post-MVP (it costs a model call); do not tax the model in the MVP.
- Clicking the title starts inline rename; Enter commits, Esc cancels — the same action
  as the rail's rename, one registry entry.
- No menu bar. All former menus are palette entries. `Log` becomes the **Activity** tab;
  the raw stream stays in the separate **Raw Log** drawer (§5.29).
- The OS title bar stays native (no client-side chrome in v1); the in-window top bar is
  the application bar. Future multi-monitor work can revisit this.

### 5.2 Rail

**Anatomy:** search field · groups (`PINNED`, `TODAY`, `YESTERDAY`, `PREVIOUS 7 DAYS`,
`OLDER`) · session rows · footer (`+ New session`, settings, help).

- 208 px (26 gu) / 26 cells; collapsible to 56 px (7 gu) / 7 cells (a vertical strip with
  icons: search, new, active session dot). `Ctrl+B`.
- **First-run policy:** auto-collapse when GUI width < 1100 px or TUI < 100 columns;
  otherwise open. At the 110×30 reference it opens. A persisted user choice wins; the
  auto policy applies only when no preference exists.
- Groups are computed from `SessionMeta.updatedAt`, which already exists in the store
  (`session/types.d:40-44`) but is not yet sent to the UI. Empty groups are omitted.
- Pinned state is UI-local in `imgui2.conf` for the MVP (the simpler option); it can move
  to `SessionMeta.extra` later if cross-machine sync is wanted.
- Search is fzf over title + preview + last answer; results are highlighted with the
  existing `session_fuzzy` matcher (already excellent).
- Footer: `+ New session` is a quiet row, not a shouting button; `?` opens help.
- The rail is a list of **sessions**, not a second chat. No message counts unless they are
  the only differentiator; show relative time instead (`2h`, `3d`).
- TUI: search field is a real `InputText` with a `/` prefix; `/` focuses it from anywhere
  (unless the composer has focus). Rows use `▸`/`●` and are 1–2 rows depending on density.

### 5.3 Session row

**States:** rest, hover, active, running, unread, pending-switch (busy), rename, filtered-out.

```
▌ Plan: modern UI                        2h
  three regions: rail, thread, inspector
```

- Active: 2 px accent rule at the left + `accent.quiet` fill + `text.hi` title.
- Running: spinner glyph replaces the time; a `running` label in the tooltip.
- Unread: 6 px dot in the right gutter (cleared when opened).
- Hover (GUI): `⋯` menu appears at the right: Rename, Pin, Duplicate, Export, Delete.
  TUI: `m` opens the same menu; `r` renames, `d` deletes (with confirm), `p` pins.
- Truncation: two lines max (title, preview); ellipsis is UTF-8 safe. No `[N]` count
  suffix; counts move to the tooltip.
- Delete shows a confirm popover naming the session; destructive colour only on the
  action, not the row.

### 5.4 Turn

The thread is a list of turns. A turn is the unit of rendering, paging and navigation.

```
▌ YOU   09:41
  what would a modern UI look like?

  ✓ 12.4s · 6 tools · 1 file · 3 sources          ← Run ribbon
  ▾ WORK                                           ← collapsed by default when done
    ✓ read_file       tui_chat.cpp      240 ms
    ✓ executeCommand  dub test          9.1 s
  ANSWER
  The shell is three regions…

  qwen3-coder · 42k → 48k · 12.4s                  ← turn meta
```

- States: `queued`, `running`, `waiting_user`, `done`, `failed`, `cancelled`.
- A queued turn shows a ghost card ("Queued — will run after the current turn") and can
  be cancelled before it starts. (llmfun currently serializes queries; the UI should say
  so instead of silently dropping input.)
- Turns are separated by `space.6` (comfortable) / `space.4` (compact), not by rules.
- Semantic zoom: `z` collapses the focused turn to the ask only; `Z` expands all. New
  answers arrive expanded; old work collapses automatically after completion.
- The turn id is stable across streaming updates and session replay, so expansion state
  survives a refresh.

### 5.5 Run ribbon

One line that summarises the work. It replaces the `Assistant: 4` accordion and is the
single most informative element after the answer.

| State | Content |
|---|---|
| Running | `◐ 00:12 · executeCommand · 6 tools · Esc to stop` |
| Done | `✓ 12.4 s · 6 tools · 1 file · 3 sources` |
| Failed | `✕ failed after 4.2 s · executeCommand · [details]` |
| Cancelled | `– cancelled after 2.1 s · 3 tools` |
| Waiting | `◷ waiting for you · 1 question` |

- Click/Enter toggles the Work section; the chevron is part of the row.
- A tool name in the ribbon is a link to that tool card.
- Numbers are tabular; durations use one decimal below 60 s, `Mm Ss` above.
- TUI: exactly one row; clip with `…` and keep the state glyph + duration.

### 5.6 Work section and tool card

Tool call and tool result are **one card**, not two chat messages.

```
┌ WORK ────────────────────────────────────────────────── 3 tools ─┐
│ ✓ read_file          tui_chat.cpp                240 ms      │
│ ✓ executeCommand     dub test                    9.1 s       │
│ ✕ writeFile          docs/ui.md                  12 ms       │
│   └ permission denied: workarea is read-only      [retry]    │
└──────────────────────────────────────────────────────────────────┘
```

**Tool card anatomy:** status glyph · tool name (medium) · primary argument (one line,
monospace) · duration (right) · result summary (second line, dim) · actions (hover/focus:
copy, open in canvas, retry on error).

- Running: spinner + the current phase if known (`connecting`, `streaming`, `waiting`).
- Success: `✓` + `state.ok` glyph only; no green background.
- Error: `✕` + `state.danger` glyph + message + `[retry]`; the card stays expanded.
- Long results: collapsed to 3 lines with `show all (n lines)`; full text in the canvas.
- `writeFile` / edit tools with a diff: show `+42 −7` and a one-line snippet; opening
  shows the diff card.
- `read_file`: show path + line range; opening shows the file around that range.
- `executeCommand`: show the command line and exit code; output in the canvas.
- Args are rendered from JSON (`StreamToolCall.arguments`) as `key=value` chips when short,
  or a `json` code block when long — never `toPrettyString` glued into prose.
- TUI: the card is a bordered block only when expanded; collapsed cards are single rows
  (`✓ read_file  tui_chat.cpp  240ms`). Borders are 1-cell `┌─┐` when expanded, because a
  full card per tool would cost too many rows.

### 5.7 Reasoning disclosure

```
▸ Thinking · 3.2 s · 812 tokens
```

- Collapsed by default (`thinking: closed | open | auto`; **default `closed`**). `auto`
  opens while streaming and collapses on completion; `open` keeps it expanded. The
  duration/token count stays in the label.
- Body is markdown, muted (`text.mid`), 1 px left rule `line.soft`, no card, no "End ---"
  separator.
- Never label it "Model reasoning" per message; the turn has one reasoning disclosure per
  assistant step, nested under Work if there are several.
- TUI: `▸ Thinking · 3.2s` one row when collapsed; expanded content is indented 2 cells
  with a `│` guide.

### 5.8 Answer card

The answer is the only block with a visible container: `raised` background, 1 px
`line.soft`, 10 px radius, 12 px padding, and a 2 px `accent` rule on the left.

**Anatomy:** `ANSWER` label (accent, 10 px, tracked) · markdown body · footer meta
(`model · tokens · duration`) · actions (`Copy`, `Why?`, `Save`).

- `Why?` opens the inspector on this turn: Sources, Reasoning, Memory — the trust feature.
- `Copy` copies the raw markdown (not the rendered text). `Save` writes
  `answers/<session>-<n>.md` under the workarea (optional, Phase 5).
- Streaming: the card exists from the first token; the label becomes `ANSWER · writing`,
  a `▍` caret trails the last line, and the footer shows elapsed + tok/s.
- Errors never masquerade as answers: a failed run renders an error card in the answer
  slot with `Retry` and `Copy diagnostics`.
- TUI: the label row is the container; no background (terminal backgrounds are precious).
  A 1-cell `▌` accent rule on the left of the answer rows is enough.

### 5.9 Code and diff blocks

```
┌ ui/shell.cpp ──────────────────────────────── copy ┐
│ auto shell = ui::shell(state);                     │
└────────────────────────────────────────────────────┘
```

**MVP note:** rich_md's default code block (framed, monospaced, built-in `Copy`) ships in
P2. The header/line-number design below is a post-MVP `HostServices::RenderCodeBlock`
implementation; it is not a rich_md option.

- Header: language/path, line range, `copy`, `open`. This header is a
  **`HostServices::RenderCodeBlock`** implementation; rich_md's default is a plain framed
  block with a built-in `Copy` button (`rich_md.cpp`, `_RenderCodeBlockPlain`).
- GUI: syntax highlighting is a **build option** (`IMGUI_RICHMD_WITH_CODE_EDITOR`,
  ImGuiColorTextEdit; currently OFF). Post-MVP: enable it **GUI-only** and install a
  `RenderCodeBlock` that uses the editor when `!tuiIsTextGrid()`; the TUI stays plain.
  Do not write a custom lexer. Line numbers on hover, 13 px mono, 20 px line height,
  horizontal scroll *inside* the block only when a line exceeds the width; a subtle fade
  at the right edge signals it.
- Diff: `+`/`−` gutters, `state.ok`/`state.danger` at 14% background for added/removed
  lines, hunk headers dim, `open file at line` action.
- TUI: rich_md's default code block is a child window (`AutoResizeY`), and a child costs
  the 4-row minimum on the 1-cell grid; budget the rows or install a custom
  `RenderCodeBlock` that avoids child windows. No syntax colour by default; `+`/`-`
  gutters and bold for hunk headers. Long lines are clipped with `…` and `o` opens the
  canvas.
- Never wrap code inside the TUI (it destroys alignment); wrap in the GUI is opt-in.
- Keep `RenderRaw` for model output: `Render` resolves `![[...]]` transclusions and reads
  files; a message must never trigger that.

### 5.10 Notice and error cards

- **Notice** (`role.system`): one line, dim, no container: `Context compacted · 42k → 18k`.
- **Error**: card with `state.danger` rule, a plain-language title, the technical detail
  collapsed, and actions (`Retry`, `Copy diagnostics`, `Open log`).
- **Warning**: amber rule + `!`, used for context > 90%, tool permission, degraded
  markdown, missing image.
- Errors are actionable. "Unknown error" is not allowed; if the D side has no better
  message, show the operation and a copyable diagnostic id.

### 5.11 Streaming state

- One stable block id per assistant answer; updates replace content, never append a new
  message (today `tuiAddOutputLine` clears the stream, causing a flicker).
- Phase model: `queued → thinking → tooling → writing → done | failed | cancelled`.
  The now bar and the turn state read from the same enum.
- Auto-scroll: stick to bottom while the user is at the bottom; if the user scrolls up,
  stop following and show a `Jump to latest ↓` pill (GUI) / a `↓ latest` hint in the now
  bar (TUI). `End` returns. (Follow flag: task 13a; pill and hint: tasks 25/28.)
- The composer's Send button morphs into **Stop** (a quiet `accent.quiet` fill with
  `state.danger` text — not a red flood). `Esc` cancels. A cancelled turn keeps its
  partial answer with a `cancelled` marker — work is never silently discarded.
- First-token latency: after 300 ms show `Thinking…` with a spinner; after 2 s add
  `still waiting on the model` and the model name. No fake progress bars.

### 5.12 Composer

The composer is the most-used surface and currently the least legible. It gets a real
container, a placeholder, a caret and a button.

```
┌──────────────────────────────────────────────────────────────────────────────┐
│ Ask anything…   / commands   @ files                        Plan ▸ qwen3 ▸ Send │
│ ctx ████░░ 42k                                              ◐ streaming 38 t/s │
└──────────────────────────────────────────────────────────────────────────────┘
```

**Anatomy (top to bottom):**

1. **Attachment chips** (only when present): `@tui_chat.cpp`, `@rag:rfc`, `@skill:plan`.
   Each removable with Backspace/click.
2. **Text area** (multiline, auto-grow 1–8 rows) with the placeholder
   `Ask anything…  / commands  @ files`; the mode and model chips sit right-aligned on
   the text line (the TUI uses plain `Plan ▸ model` text). Markdown-ish input is plain
   text (no live preview), but `/` and `@` open completions.
3. **Meta line**: mini Context Lens left, streaming phase and tok/s in the middle when a
   run is active, `Send`/`Stop` right. Skills/RAG counts appear as quiet chips next to
   the mode when they are non-default.

Behaviour:

- `Enter` sends and `Shift+Enter` inserts a newline **only after the P0 key gate proves it
  in both backends with `InputTextMultiline` active** — the old TUI problem was precisely
  the active multiline field, not the idle screen. If the TUI cannot distinguish
  Shift+Enter, do **not** change Enter: use a same-chord alternative that works in both,
  or keep `Enter`=newline and the real Send button. No per-profile default.
- The gate is a real test: focus `InputTextMultiline`, press Enter and Shift+Enter, assert
  send vs newline in TUI and GUI. `Ctrl+Enter` is a fallback newline only if the same chord
  works in both.
- IME-safe: never send while a composition is active.
- `↑`/`↓` at the first/last line navigate history; the draft is preserved. Today history is
  bound to `Ctrl+Up/Down`, which the design doc notes "does not work".
- `Esc`: if a run is active, cancel it immediately (partial output is kept); if idle,
  clear the input; if empty, blur.
- `/` at the start of an empty line opens the command palette filtered to commands, with
  usage and description. `@` opens files (workarea), RAG sources, skills.
- Paste: multi-line paste stays in the composer; a paste of > 5,000 chars offers
  "attach as file" instead of flooding the input.
- Drag-and-drop (GUI): files become `@file` chips.
- The composer is never covered by the thread; the thread reserves its measured height
  (this is what the current absolute layout tries to do with `inputHeight`).
- TUI: the container is a 1-cell border; the placeholder is dim; the action reads `Send`
  at the right of the bottom row (no glyph dependence).

### 5.13 Chips

Small, 24 px / 1 row, radius 999, `raised` fill, `line.soft` border, 12 px label.

- `mode`: `Ask` (neutral), `Plan` (accent.quiet + accent text), `Code` (accent.quiet).
- `model`: `qwen3-coder` with a `▾`; tooltip shows context size and endpoint.
- `skills`: `3 skills`; popover lists them with toggles.
- `rag`: `llmfun · 12` (sources in the primary DB); popover lists DBs, counts, last sync.
- States: rest, hover, open, disabled (busy). Disabled chips explain why in a tooltip.
- TUI: `Plan ▸ qwen3-coder` as plain text with `▸` separators, no boxes (boxes cost cells).

### 5.14 Context meter

A 10-segment bar, 6 px tall in GUI, 1 cell in TUI:

```
██████░░░░  42k / 128k
```

- Segments fill left to right; colour: `accent` below 70%, `state.warn` at 70–89%,
  `state.danger` at ≥ 90%; the token text is always shown (colour is redundant).
- The compaction threshold is a 1 px tick at 90% (or the configured threshold), with a
  tooltip "compaction at 115k".
- Full Context Lens in the inspector; the composer meter is the glanceable version.
- TUI: `ctx ████░░ 42k`; block elements are the most widely supported 1-cell bar glyphs;
  the ASCII fallback is `#`/`.`.
### 5.15 Now bar

One row, always visible, the answer to "what is happening?".

| State | Content |
|---|---|
| Idle | `Ready · qwen3-coder · 42k/128k` + key hints |
| Thinking | `◐ thinking · 3.2 s · qwen3-coder` + `Esc stop` |
| Tooling | `◐ executeCommand · 9.1 s · 6 tools` + `Esc stop` |
| Writing | `◐ writing · 38.1 tok/s · 812 tokens` + `Esc stop` |
| Waiting | `◷ waiting for you · question 1 of 1` |
| Error | `✕ last run failed · executeCommand · [details]` |
| Offline | `! model server unreachable · [retry]` |

- Left: phase + details. Right: contextual key hints (`Enter send`, `Esc stop`,
  `Ctrl+K commands`) that change with focus. Hints are the discoverability layer; they
  are not decoration.
- The bar is `surface` with a top hairline; no colour background except a 2 px state rule
  at the far left.
- TUI: 1 row; truncate hints first, never the state.

### 5.16 Inspector

224 px (28 gu) / 28 cells, tabbed: **Context · Tools · Sources · Memory · Agents · Activity**.
`Ctrl+I` toggles; `Ctrl+1..6` selects a tab; the active tab persists. Default on first
run: open at ≥ 1200 px, collapsed otherwise (a saved preference wins).
- The inspector follows the **selected turn** by default (click a turn or a tool card);
  Context is global, the others are per-turn.
- Each tab has an empty state ("No sources used in this turn", "No memory read").
- TUI: tab strip as `[1] Context  [2] Tools …` with the active tab reverse-video; number
  keys select. Content scrolls independently.
- **Activity** is the **user-facing** half of the old Log tab: plain-language rows with a
  severity glyph (`i`, `!`, `✕`) and colour, a time, and an action where relevant
  (`Retry`, `Details`). Filter: info / warning / error; search. No module names, no line
  numbers, no stack traces — those live in the raw log (§5.29). It is quiet by default:
  no auto-scrolling live tail; a badge on the tab shows unread warnings/errors. This
  closes `doc/todo.md` "log messages sent to the TUI should have the severity so the
  heading can be colored differently" — severity becomes data, not a colour guess on a
  summary string. The MVP starts visible but quiet; P5 adds `Activity: visible | on-error`
  as a setting.
- The raw developer log is **not a tab** and never shares this pane; it is a separate,
  opt-in overlay (§5.29). A normal user can use the app for years without seeing it.
- On narrow widths the inspector becomes an overlay panel from the right, 90% width,
  with a title and `Esc` to close.

### 5.17 Context Lens

A stacked bar with a legend:

```
CONTEXT
42,180 / 131,072 tokens
████████████▓▓▓▓░░░░░░░
sys 8.1k · skills 3.4k · mem 2.0k · rag 9.2k · chat 19.5k
compact at 117,964 (90%)
```

- Segments: system + AGENTS.md (slate), skills (violet), memory (teal), RAG (steel blue,
  not amber — amber is reserved for warnings), conversation (accent), free (line.soft).
- Hover a segment → exact tokens and percentage; click → filters the Sources/Memory tab.
- A marker at the compaction threshold (the configured value, else 90%); when compaction
  happens, a notice appears in the thread and the bar animates once (GUI) / redraws (TUI).
- The totals come from `ServerStat` + the new `contextMax` field; no string parsing.
- TUI: two rows of bar + one legend row. If width < 24, show only `42k/128k` + `!` at 90%.

### 5.18 Tools timeline

- A list of tool runs for the selected turn: name, duration bar (relative), status,
  start offset.
- A tiny Gantt: `read_file ▕██░░░░░░▏ 240ms`, `exec ▕░░███░░░▏ 9.1s`. In the GUI the bars
  are 6 px tall, rounded; in the TUI 1 cell of `█`/`░`.
- Aggregate header: `6 calls · 9.6 s total · 1 failure`.
- Click → the tool card scrolls into view and expands.

### 5.19 Sources

- Per-turn RAG hits: source path/topic, score, a 2-line snippet, `open` (canvas).
- Grouped by database (primary/secondary); a footer shows `12 sources indexed`.
- If retrieval metadata is unavailable (Phase 4 instrumentation gap), the tab says
  "Retrieval details are not reported by this build" — no fake data.
- Future: clicking a source opens the chunk in the canvas with the surrounding text.

### 5.20 Memory

- Two groups: `Short-term` and `Long-term`, with the entries read or written during the
  turn highlighted.
- Each row: topic, one-line summary, tier badge, age.
- "Why did it know this?" is answered here: the memory block that was injected into the
  context, with its token cost.
- TUI: `short  mylib_then_handlers  ·  2d` with a `▸` to expand the summary.

### 5.21 Agents (pipeline)

- Lanes, one per agent: role, label, state (`queued/running/done/failed`), elapsed,
  tokens, current tool.
- A compact horizontal "mission strip" at the top of the run: `planner ✓ → coder ◐ →
  reviewer ·`.
- Click a lane → the thread filters to that agent's transcript (read-only), with a
  `Back to main` breadcrumb. This replaces the mutually-exclusive pipeline panel.
- Stop one agent / stop all (with confirm) in the lane menu.
- TUI: one row per agent; the active lane is marked `▸`; the strip fits in one row.

### 5.22 Palette

Overlay, 640 px wide, centered (max 70% of a narrow window), 12 results, grouped:

```
┌ Ctrl+K ───────────────────────────────────────────────────────────┐
│ test                                                             │
│                                                                  │
│ ACTIONS     Run tests (dub test)                 Ctrl+T          │
│ SESSIONS    Fix the parser panic - 2h                            │
│ COMMANDS    /skills - list and load skills                       │
│ FILES       cpp_tui/tui_chat.cpp                                 │
│ SOURCES     tui_design.md > Rendering internals                  │
│                                                                  │
│ ASK         Send "test" to the agent              Enter          │
└───────────────────────────────────────────────────────────────────┘
```

- Fuzzy matching, match highlighting, keyboard-first (`↑/↓`, `Enter`, `Tab` to cycle
  groups, `Esc`).
- Sections are ordered by confidence: actions, commands, sessions, files, sources, then
  the free-text fallback.
- Every action in the app is registered here (one registry, also used for the keymap
  overlay and the now-bar hints). This is how we guarantee "every action is a command".
- TUI: a bordered overlay 80% width, 60% height, `┌ Ctrl+K ┐` is replaced by `┌ Search ┐`;
  the TUI also opens it with `/`, the terminal-native gesture. Same sections and keys.

### 5.23 Canvas (artifact viewer)

The fourth surface, opened from a file/diff/tool/source: a right-side panel (GUI) or a
full-width overlay (TUI) with tabs per artifact.

- Header: path + line range + `copy path` + `open externally` (GUI: the OS handler, with
  consent).
- Body: code/diff/markdown/text, read-only.
- Footer: `Esc close`, `[`/`]` previous/next artifact.
- The canvas is how "the agent changed a file" becomes inspectable without leaving the
  app. It also hosts the RAG chunk viewer from `doc/todo.md`.

### 5.24 Ask and approval cards

The agent can ask the user a question or request permission; the run pauses in
`waiting_user`.

```
┌ QUESTION ────────────────────────────────────────────────┐
│ Which database should I migrate first?                   │
│ ▸ primary (llmfun.db)                                    │
│   secondary (docs.db)                                    │
│   type another answer…                                   │
│ [Answer]  [Cancel run]                                   │
└──────────────────────────────────────────────────────────┘
```

```
┌ APPROVAL ────────────────────────────────────────────────┐
│ Run `rm -rf build/`?                                     │
│ workarea: /workarea/llmfun · sandbox: write              │
│ [Allow once] [Always for this session] [Deny]            │
└──────────────────────────────────────────────────────────┘
```

- Keyboard: `1..9` picks an option, `Enter` confirms, `Esc` denies/cancels. The free-form
  option reveals an inline field inside the card (the composer stays disabled).
- The composer is disabled while a question is pending; the now bar says `waiting for you`.
- Approvals are opt-in (`toolApproval: auto | ask`), **default `auto` for now**; the
  ask/approval cards and the setting ship in P4, and the policy is revisited later.
  Destructive tools (`executeCommand`, `writeFile`, `removeFile`) are the first
  candidates.
- Runtime dependency: ask/approval needs an interrupt/resume seam in the agent loop (the
  sub-agent runtime's `waitFor` is the natural home). Until that exists, a blocking tool
  call can emulate it; the UI contract stays the same.
- This closes `doc/todo.md` "add UI where an agent can query the user for input" and gives
  the threat model a human-in-the-loop surface.

### 5.25 Toasts

- GUI: bottom-right, 360 px, max 3 stacked, `raised` + shadow, 4 px state rule, title +
  one line + optional action. Info/success auto-dismiss in 4 s; warnings 8 s; errors
  persist until dismissed.
- TUI: a single-line flash in the now bar for info/success (3 s); warnings/errors replace
  the now bar left side until acknowledged (`x`).
- Never use a toast for something that belongs in the thread (tool results, answers).

### 5.26 Empty, loading and error states

- **Empty thread** (new session): wordmark, one sentence ("Ask, plan, or code — the agent
  can read your workarea, search the RAG, and run tools."), three starter cards
  (`Summarize this workarea`, `Plan a feature`, `Find failing tests`), and `Ctrl+K` hint.
  Recent sessions appear in the rail; no fake example chat.
- **Loading session**: a 3-row skeleton for the rail and thread (GUI shimmer 1.2 s, TUI
  static `…`), never a blank screen.
- **Backend unavailable**: the now bar shows `! model server unreachable`, the composer is
  disabled with a tooltip and a retry action, and the error card explains the endpoint and
  offers `Retry` / `Open settings`.
- **Too small** (TUI): compact mode down to 40×15; the error page below that.

### 5.27 Settings

A searchable sheet (GUI modal, TUI overlay), sections:

- **Appearance**: theme (Graphite/Parchment/Native/Auto; default **Graphite**), density, font scale (existing
  `FontScaleMain`), UI font file (existing env var, now a field), mono font.
- **Behaviour**: send key (Enter/Ctrl+Enter), confirm quit while running, confirm delete,
  notifications, reduced motion.
- **Agent**: default model, context size (read-only), tool approval mode, RAG DB list,
  skills directory.
- **Advanced**: backend mode (auto/gui/tui), ini/settings path, log level, diagnostics
  (copy system info).

Persist in `ui.conf` (a sibling of `imgui2.conf`) during the transition; legacy window size
and `FontScaleMain` stay in `imgui2.conf` so a legacy save cannot clobber the new keys.
After legacy is removed, the two files merge into one. Same tolerant key=value parser:
unknown keys ignored, malformed values rejected. Settings changes apply on the next frame
(except backend mode, which needs a restart and says so).

### 5.28 Help overlay (`?`)

A single searchable page: the keymap (grouped), slash commands with descriptions (from
the D registry), and a "what is this?" paragraph per region. It replaces the `/help` wall
of text in the thread (which can still exist for scripts). `Esc` closes.

### 5.29 Developer surfaces — the Raw Log

**Principle: the diagnostic stream is not the user's narrative.** A normal user wants to
know what happened and what to do next; a developer wants the unfiltered event stream.
Those are two different products, so they get two different surfaces — and the raw one is
opt-in.

Three levels, three audiences:

| Surface | Audience | Content | Default |
|---|---|---|---|
| Thread notices | user | Human-readable events that affect the answer: `Context compacted · 42k → 18k`, `Tool failed · [retry]` | on |
| Activity tab | user | Operational history: time, severity, plain message, action. No module names, no line numbers, no stack traces | on |
| **Raw Log** | developer | Timestamp, level, module, function, line, message, raw tool/agent events | **off** |

**Raw Log anatomy** (GUI bottom drawer, TUI full-screen overlay):

```
┌ RAW LOG — developer ──────────────────────────── follow [x]  copy  save  close ┐
│ 09:41:02.183 INFO   app_agent.ui        ui.d:243     status: Context 42k/128k  │
│ 09:41:02.190 DEBUG  chat                chat.d:409   toolCall read_file {...}  │
│ 09:41:02.432 WARN   tool.broker         broker.d:187 tool not found: foo       │
│ 09:41:02.435 ERROR  app_agent.package   package.d:303 run failed: exit 1       │
└────────────────────────────────────────────────────────────────────────────────┘
```

- **Access:** `Ctrl+Shift+L` (both profiles, where the terminal delivers shift-modified
  letters), the `/rawlog` slash command, `--debug-ui` on the command line, or
  `uiLog: raw` in settings. **No environment variable.** It is **not** an inspector tab and
  not in the palette's normal groups; if it appears in the palette at all, it is under a
  `Developer` group with a `dev` badge. A user cannot click into it by accident while
  looking for Activity.
- **GUI:** a bottom drawer, 30% of the window height, resizable, monospace only, level
  colours, a filter box, `follow` (auto-scroll), pause, copy, save. It resizes the thread
  above it rather than floating over the composer.
- **TUI:** a full-screen overlay (a drawer would steal rows from the thread), `Esc`
  closes, `f` toggles follow, `/` filters, `y` copies the visible rows.
- **Visual identity is deliberately different:** monospace, square, no markdown, no cards,
  no type scale. A persistent `RAW` badge appears in the top bar / now bar while it is
  open, so it is impossible to forget that the developer view is capturing.
- **Persistence:** the *setting* persists; the drawer does **not** auto-open on restart.
  It is closed on every start unless `--debug-ui` is given.
- **One sink, one archive:** `tuiSetLogging` / `llmfun_ui_log.txt` remains the archival
  channel. The drawer is a view over the same sink, not a second logging system. `Save`
  exports the current view with the same format.
- **Level:** opening the raw log raises the in-process logger to `debug` (or `trace` with
  `uiLog: trace`) for that session and restores the previous level on close; the archive
  still follows the configured level.
- **Promotion rule:** a raw event becomes an Activity row only if it has a user-facing
  message and a severity; it becomes a thread notice only if it changes the user's next
  action. Everything else stays in the Raw Log. No raw line ever appears in the thread or
  in Activity.
- **Privacy:** the raw log may contain paths and tool arguments. It is local-only, never
  included in the model context, and never uploaded; `copy` copies to the clipboard on
  explicit request. This is the developer's own diagnostic surface, not a telemetry
  channel.

### 5.30 Diagrams (Mermaid)

**GUI:** rich_md's native Mermaid renderer (flowcharts, sequence and class diagrams) with
a `MERMAID` header, `view source` toggle, max-height/scroll and `copy source`. A diagram
that fails to parse shows the source plus a one-line error. **TUI:** Mermaid is disabled
per context; a compact `[mermaid diagram · N chars]` row plus the source in a code block,
or `open in GUI`.

- Do not promise full Mermaid: rich_md supports a subset (no themes, click, or markdown
  labels); complex diagrams can fail.
- Per-context switch: `RegisterFencedBlockRenderer("mermaid", ...)` on the TUI context;
  the GUI keeps the default registered by `CreateContext`.
- States: rendering, source, error, too-large (collapsed to a header + source).
- The diagram is an artifact: `o` opens it in the canvas; the source is always available.

### 5.31 Tables

**GUI:** rich_md's native tables — alignment, resizable columns, full inline markdown.
**TUI:** a first-party **text-grid table block**. rich_md's table renderer is not used:
on the 1-cell grid it paints header text onto the top border (`T`/`A` artifacts), drops
left corners on some rows, floods the row with cell backgrounds, and can blank the message
when the table is too wide.

```
┌───────────────┬───────┬───────┐
│ Tool          │ Calls │ Time  │
├───────────────┼───────┼───────┤
│ read_file     │     3 │ 240ms │
│ executeCommand│     1 │  9.1s │
└───────────────┴───────┴───────┘
```

- Parse only top-level GFM tables (header row, delimiter alignment, body, escaped pipes).
  A scanner tracks fenced code blocks; their contents are **never** table-parsed. Inline
  code is opaque: a `|` inside backticks is text, not a separator.
- Constants in `ui/table.h` (tweakable): `kCellPadding = 1`, `kMinColumnWidth = 4`,
  `kMinTableWidth = 20`, `kMaxTableColumns = 8`,
  `kTableBorderWidth(cols) = 3*cols + 1`. Width allocation: distribute the available
  content cells, cap each column at `max(kMinColumnWidth, floor(maxContent/cols))`, wrap
  the rest; if even the minimums do not fit, use the vertical layout.
- Cell widths use the cell-width table (CJK, wide glyphs, combining marks), not byte or
  character counts.
- Content-width columns, capped to the available text width; 1-cell padding; header
  bold/accent; **no background fill**; thin borders or ASCII `+ - |` fallback.
- Wrap long cells to multiple lines; row height is the max line count; alignment from
  `:---` / `---:` / `:--:`.
- If even a wrapped table cannot fit (too many columns), switch to a vertical key-value
  layout (`Tool: read_file`, `Calls: 3`, `Time: 240ms`) or source; never horizontal-scroll
  or blank.
- `s` toggles the markdown source; `y` copies the markdown; `o` opens the table/source in
  the canvas.
- MVP: plain cell text (strip `**`, links to labels, inline code as text); inline styling
  is P3. The legacy thread bridge preprocesses each table into a fenced `llmfun-table`
  block rendered by `RegisterFencedBlockRenderer` (no child window); v5 uses
  `TuiBlockKind_Table`.
- Tests: 103×82, 110×30, 120×40; long/wide tables; header artifacts; wrapping; source
  toggle; code-fence skipping.

---

## 6. Signature interactions

These are the moves that make the UI feel designed rather than assembled. Each one is
implemented once, in the component layer, and works in both profiles.

### 6.1 One registry, three views: the command surface

Everything the user can do is an entry in a **command registry**:

```cpp
struct Command {
    std::string id;          // "session.new", "run.cancel", "view.focus"
    std::string title;       // "New session"
    std::string group;       // "Session" | "View" | "Agent" | "Slash" | "Developer"
    std::string shortcut;    // "Ctrl+N" (display only)
    std::string keywords;    // extra fuzzy terms
    std::function<void()> run;
    std::function<bool()> enabled;
};
```

The registry feeds (a) the palette, (b) the `?` keymap overlay, (c) the contextual hints
in the now bar, and (d) the tooltips on buttons. A test asserts that every registry entry
is reachable and has a shortcut or a visible button. This is how "every action is a
command; every command is discoverable" is enforced mechanically, not by good intentions.

D-side slash commands are imported into the same registry (`/skills`, `/model`, `/plan`,
`/code`, …) with their usage and descriptions, so the palette is also the slash-command
browser. A command that only exists in D still appears in the UI.

### 6.2 Universal search — the product's memory, finally visible

`Ctrl+Shift+F` opens a full search surface with four stores, all of which already exist
in the backend:

| Section | Source | What it answers |
|---|---|---|
| Sessions | session store + fuzzy | "Where was that conversation?" |
| Dialogue | `queryDialogueHistory` (FTS5 + vector) | "What did we say about X?" |
| Reasoning | `queryReasoningHistory` | "Why did we decide that?" |
| Knowledge | `queryBestMatch` over the RAG | "What do the docs say?" |

Results show a snippet with highlighted terms, the session/turn it belongs to, and a
score; `Enter` jumps to the turn (or opens the RAG chunk in the canvas). This turns three
invisible indexes into a first-class feature — and it is the most "modern" thing the app
can do, because very few chat UIs can search *their own reasoning*.

### 6.3 The thread minimap

A 1-cell (TUI) / 10 px (GUI) strip on the right edge of the thread, aligned with the
scroll range:

```
│   ← viewport
▏
▎  ● answer
▏  ! error
▏  ◆ tools
▏  ● you
```

- Marks: answers (`accent`), errors (`danger`), tool-heavy turns (`role.tool`), user
  turns (`role.you`); the viewport is a translucent block.
- Click/drag jumps (GUI); `Alt+↑/↓` jumps between marks; `[`/`]` between answers (TUI).
- At 1000 messages the minimap is the only cheap "where am I" signal. It costs one
  column and no vertical space.

### 6.4 Semantic zoom

- `z` toggles the focused turn between "ask only" and full.
- `Z` expands/collapses all work sections.
- Long runs auto-collapse finished turns to ribbon + answer; the last turn stays open.
- The state is per turn and survives replay (keyed by stable turn id).

Semantic zoom is what makes a 50-turn agent session readable on 30 rows.

### 6.5 "Why?" — trust in one keystroke

Every answer card has a `Why?` action. It opens the inspector on that turn:

- **Sources**: the RAG chunks retrieved, with scores.
- **Reasoning**: the thinking that produced the answer.
- **Memory**: entries read/injected.
- **Tools**: what was executed and what it returned.

The point is not more data; it is a single, predictable place to answer "why should I
believe this?" — and it is also the debugger for the agent, which is what maintainers
will use every day.

### 6.6 Canvas flow

`o` or a click on a file/diff/tool/source opens the canvas. `[`/`]` cycle artifacts,
`Esc` closes, `c` copies the path, `Enter` opens the file at the line in `$EDITOR`
(TUI; configurable) or the OS handler (GUI, with confirmation). The thread never loses
its scroll position: the canvas is a sibling region, not a navigation.

### 6.7 Ask and approval flow

The run pauses, the now bar says `waiting for you`, the composer is disabled, and the card
is focused. Options are numbered; `1..9`, arrows + Enter, or free text. The answer is sent
as a structured event (`TuiUiAction::Answer`), not as a chat message, so the agent runtime
can resume deterministically (this pairs with the sub-agent runtime proposal's
`waitFor`/interrupt ideas).

### 6.8 Completion and attention

- When a run finishes while the GUI window is unfocused: a toast + a taskbar attention
  request (`glfwRequestWindowAttention`), never a modal.
- TUI: a bell (`\a`) only if `notifications: bell` is configured (default off), plus a
  `● done` marker in the rail and now bar.
- Sessions with unread completions show a dot in the rail until opened.
- Long runs (> 10 s) get a "notify me" affordance in the run ribbon — useful for the
  `duB test`-length tools.

### 6.9 Keyboard model

**Modifiers are global; plain keys belong to the focused text field.** That single rule
prevents the classic TUI conflict where `j` moves the list while you are typing.

Global (work while the composer is focused unless noted):

| Key | Action |
|---|---|
| `Ctrl+K` | Command palette |
| `Ctrl+Shift+F` | Universal search |
| `Ctrl+P` | Session quick switch (palette filtered to sessions) |
| `Ctrl+B` | Toggle rail |
| `Ctrl+I` | Toggle inspector |
| `Ctrl+1..6` | Inspector tabs |
| `Ctrl+N` | New session |
| `Ctrl+Shift+L` | Toggle the raw developer log (§5.29; off by default; `/rawlog` always works) |
| `Ctrl+R` | Retry the last failed turn |
| `F11` / `Ctrl+Shift+M` | Focus mode (GUI / TUI) |
| `?` | Help overlay (only when the composer is empty or unfocused) |
| `Ctrl+Q` | Quit (confirms if a run is active). The app disables IXON where possible; `/quit` and the palette always work if the terminal swallows it |
| `Ctrl+C` | GUI: copy selection; TUI: cancel the active run (no run: no-op; the TUI form requires raw mode, §8.12) |
| `Esc` | Close overlay → cancel run → clear composer → blur (first match) |
| `Alt+↑/↓` | Previous/next turn mark (where the terminal delivers Alt+arrows; `[`/`]` are the fallback) |
| `[` / `]` | Previous/next answer (thread focused) |
| `z` / `Z` | Semantic zoom (thread focused) |
| `y` | Copy the focused block (thread focused) |
| `o` | Open the focused block in the canvas |
| `End` | Jump to latest / re-enable follow |

Composer:

| Key | Action |
|---|---|
| `Enter` | Send |
| `Shift+Enter`, `Ctrl+Enter` | Newline |
| `↑`/`↓` at first/last line | History (the draft is preserved) |
| `/` at line start | Command completion |
| `@` | File/RAG/skill completion |
| `Tab` | Accept completion |
| `Esc` | Cancel run / clear / blur |

Rail (when focused):

| Key | Action |
|---|---|
| `↑`/`↓` | Move selection |
| `Enter` | Open session |
| `r` / `d` / `p` | Rename / delete (confirm) / pin |
| `m` | Row menu |
| `/` | Focus search |
| `n` | New session |

**Changes from today** (documented in the release notes and shown once as a one-time
hint): `Enter` sends; `Ctrl+Q` quits; `Ctrl+C` no longer quits (it copies in the GUI and
cancels a run in the TUI — the TUI form needs raw mode/ISIG off, §8.12); history moves
from the non-working `Ctrl+Up/Down` to `↑`/`↓` at the first/last line; `Esc` gains
cancel-run semantics. `Ctrl+L` remains "clear the view" (not the session); `/clear` still
clears the session.

### 6.10 Mouse model (GUI)

- Wheel scrolls the thread under the cursor; `Ctrl+wheel` changes the font scale (with a
  transient overlay showing `110%`).
- Click selects a turn (accent rule); double-click expands/collapses it.
- Hover reveals block actions (`copy`, `why`, `open`); actions are also in the context
  menu and the palette, never hover-only.
- Text selection is implemented by rich_md itself (mouse selection, Ctrl+C, right-click
  Copy / Copy as Markdown); llmfun sets `selectableText = true` in both profiles, so the
  design doc's "selection is off" statement is wrong — selection is activated. The real
  change is the **Ctrl+C binding and focus model**: the GUI copies the selection; the TUI
  either disables selection with `PushSelectableText(false)` and keeps Ctrl+C as quit, or
  moves quit to Ctrl+Q. Test both backends before committing. A selection that spans
  blocks copies markdown with role labels (`**You:** …`).
- Drag the rail's splitter to resize (240–360 px, persisted); double-click resets.
- Right-click on a session/turn/tool opens the same menu as the `⋯`/`m` affordance.
- The window title is `llmfun — <session title>`; closing prompts only if a run is active.

### 6.11 Onboarding

The first run shows the empty state with three starter actions and a `Ctrl+K` hint; the
`?` overlay is one keystroke away. No tour, no tooltips parade, no modal. The second run
starts in the last session (as today).

---

## 7. One spec, two fidelities

### 7.1 The fidelity contract

Every component must specify:

1. **Semantics** — what information it carries and in what order.
2. **States** — rest, hover, focused, active, disabled, running, error, empty.
3. **TUI anatomy** — rows, cells, glyphs, ASCII fallback, scroll behaviour.
4. **GUI anatomy** — px, type style, radius, elevation, hover/motion.
5. **Degradation** — what is dropped at 16 colours, 60 cells, 24 rows.
6. **Actions** — keyboard, mouse, palette entry.

A component with no TUI spec is not allowed to ship. This is the rule that prevents the
GUI from becoming a separate product again.

### 7.2 The grid rules (TUI)

- All geometry is integer cells. No half cells, no sub-cell padding.
- Text is placed at the cell containing its pen; the renderer already enforces this
  (`doc/tui_design.md`, "Text/background one-cell offset").
- One cell of padding inside a bordered region; two cells of gutter between regions.
- A bordered card costs 2 rows (`┌─┐`, `└─┘`); use borders only for interactive or
  expandable blocks. Collapsed rows are borderless.
- No glyph with East-Asian *ambiguous* width where a one-cell shift would break a box
  (`✓`, `●`, `▸` are ambiguous in some locales); the ASCII fallback is automatic for
  `TERM=linux`, `LC_CTYPE` with a CJK locale, or when `LLMFUN_ASCII=1`.
- Wide glyphs (CJK, emoji) are measured with the same width table the ncurses backend
  uses (`imtui_wcwidth`); the design avoids them in alignment-critical positions and
  falls back to ASCII when a glyph's width is uncertain.
- Never rely on background colour for a whole row; terminals with `background=black`
  and a transparent profile lose it. Use a leading glyph + foreground colour.
- The persistent-screen rule stays: any transient affordance (focus ring, caret, spinner)
  is repainted every frame or explicitly cleared. The design avoids single-frame states.

### 7.3 Units and metrics

```cpp
struct Metrics {
    bool textGrid;
    float gu;        // 1 cell (text) | 8 px * scale (GUI)
    float row;       // 1 cell      | lineHeight (GUI, 24 px)
    float pad1, pad2, pad3;
    float radius;
    float border;
    float railW, railCollapsedW, inspectorW, topBarH, nowBarH;
};
Metrics metricsFor(bool textGrid, float fontScale, float dpiScale);
```

- Horizontal sizes in `gu`, vertical in `rows`, both rounded per profile.
- Components receive `Metrics&` and `Rect`; they never read `DisplaySize` or call
  `SetCursorPos` with screen coordinates. `SetCursorScreenPos` is allowed only inside
  `ui/primitives.*`.
- The existing `colUnits()` becomes `metrics.gu(n)`; the existing `tuiIsTextGrid()`
  branches move into `metricsFor()` and the primitives. Target: **zero** profile gates in
  `views/*`.

### 7.4 Degradation ladder

| Capability | Full | Reduced | Minimal |
|---|---|---|---|
| Colour | 24-bit | 256 | 16 / no colour |
| Glyphs | Unicode box + icons | Unicode, ASCII icons | ASCII only |
| Width | ≥ 100 cells | 60–99 | 40–59 compact |
| Height | ≥ 30 rows | 24–29 | 15–23 compact |
| Motion | spinner + caret | spinner | static |
| Markdown | full rich_md | rich_md 1-cell | plain text |
| Tables | native rich_md (GUI); custom text-grid renderer (TUI) | custom TUI table block (MVP) | vertical key-value / source |

The ladder is explicit so "it looks bad at 80 columns" becomes a spec decision, not a
surprise.

### 7.5 What the GUI may add (and nothing else)

- Typography and font scale; DPI; smooth scroll; hover and focus transitions; shadows;
  rounded corners; text selection; drag-and-drop. One window; no multi-window UI.
- Syntax highlighting (optional rich_md code-editor build, GUI-only post-MVP) and diff
  colour (TUI keeps structure only).
  disables images in both profiles. If enabled in the GUI only, the TUI shows
  `[image: alt text]`.
- Toast stack; window attention request; OS clipboard.

Enrichments may not change order, wording, numbers, state names or keybindings. A
screenshot of the GUI and a dump of the TUI must be recognisably the same screen.

### 7.6 What never differs

- Information order within a turn: ask → ribbon → work → answer → meta.
- Labels (`YOU`, `AGENT`, `TOOL`, `ANSWER`, `WORK`, `SOURCES`).
- State vocabulary (`running`, `waiting for you`, `failed`, `cancelled`).
- Numbers (token counts, durations, tool counts).
- Keybindings and the command registry.
- Error text and diagnostics ids.

### 7.7 Performance budgets

| Metric | GUI | TUI |
|---|---|---|
| Frame time, idle | ≤ 4 ms CPU, no redraw when clean | ≤ 4 ms, 0 redraws when clean |
| Frame time, streaming | ≤ 16 ms (60 fps), 8 ms target on llvmpipe | ≤ 8 ms, 20–30 fps |
| Input latency | ≤ 1 frame (16 ms) | ≤ 50 ms |
| Session open (1000 msgs) | ≤ 150 ms to first paint | ≤ 150 ms |
| Memory per session | ≤ 2× the message bytes (height cache included) | same |
| Binary size delta | ≤ 1.5 MB (fonts + code) | 0 |

These are budgets, not hopes: a headless benchmark harness (1000-turn fixture, llvmpipe)
runs in CI and fails on regression.

### 7.8 Why not replace Dear ImGui?

Alternatives considered:

| Option | Why not |
|---|---|
| Qt / GTK | Heavy dependency, platform packaging, loses the shared immediate-mode core and the text-grid parity; a 50 MB static binary becomes a distro matrix. |
| Web UI (local server + browser) | Two runtimes, a network surface, a second styling system, no terminal parity; a possible *remote* mode later, not the desktop UI. |
| egui / Slint / Flutter | New toolchains and font stacks; the text backend does not exist; a rewrite with no parity guarantee. |
| A custom TUI framework | Re-implements layout, input, clipping and accessibility; ImGui already works on a cell grid. |

Dear ImGui 1.92 is a constraint we can design within. The problem was never the toolkit;
it was the absence of a design system and a data model.

---

## 8. Architecture: how to build it without rewriting the world

### 8.1 The root fix: send structure, not sentences

Today:

```
D agent thread                         C++ UI thread
Message / ToolCall / ToolResponse  ──►  format to string  ──►  ChatMessage{summary, text, type}
                                                               │
                                                               └─► CollapsingHeader + markdown
```

Every structural fact (tool name, arguments, success, duration, tokens, sources) is lost
before the UI sees it. The UI then guesses hierarchy from `[3]`, `"tool call:"`,
`"tool result ✅"` and the seven-value message type enum.

Proposed:

```
D agent thread                         C++ UI thread
Message / ToolCall / ToolResponse  ──►  TuiTurn / TuiBlock (typed, stable ids)
ServerStat / Session / Commands         │
                                        ├─► UiStore (revisioned, bounded)
                                        │
                                        ├─► views/turn, views/rail, views/inspector
                                        │
                                        └─► TuiUiAction queue ──► D
```

The UI becomes a projection of a typed model. Styling, grouping, collapsing, replay and
search all become functions of fields instead of string archaeology.

### 8.2 The model (C ABI, v5)

```c
typedef enum TuiBlockKind {
    TuiBlockKind_Prose     = 0,  // markdown body
    TuiBlockKind_Reasoning = 1,  // thinking, collapsed
    TuiBlockKind_Tool      = 2,  // call + result merged by callId
    TuiBlockKind_Code      = 3,
    TuiBlockKind_Diff      = 4,
    TuiBlockKind_Error     = 5,
    TuiBlockKind_Notice    = 6,
    TuiBlockKind_Ask       = 7,  // agent -> user question
    TuiBlockKind_Approval  = 8,  // permission request
    TuiBlockKind_Table     = 9,  // GFM table source; TUI text grid, GUI native

typedef enum TuiBlockStatus {
    TuiBlockStatus_Pending = 0, TuiBlockStatus_Running = 1, TuiBlockStatus_Ok = 2,
    TuiBlockStatus_Error = 3, TuiBlockStatus_Cancelled = 4,
} TuiBlockStatus;

typedef enum TuiRunState {
    TuiRunState_Queued = 0, TuiRunState_Running = 1, TuiRunState_WaitingUser = 2,
    TuiRunState_Done = 3, TuiRunState_Failed = 4, TuiRunState_Cancelled = 5,
} TuiRunState;

typedef struct TuiBlock {
    TuiBlockKind   kind;
    TuiBlockStatus status;
    uint32_t       flags;        // TuiBlockFlag_DefaultOpen | _Collapsible | _Selectable
    String         id;           // stable within the turn (tool callId, "answer", ...)
    String         title;        // "read_file", "ANSWER", "Thinking"
    String         meta;         // one-line summary / primary argument
    String         body;         // markdown, diff, output (owned copy)
    String         artifactId;   // canvas handle (path or blob id), optional
    int64_t        startedAtMs;  // epoch ms, 0 = unknown
    int64_t        durationMs;   // -1 = unknown
    int64_t        tokensIn, tokensOut;
} TuiBlock;

typedef struct TuiTurn {
    String        id;            // stable turn id
    uint64_t      ordinal;       // 1-based display ordinal (#12)
    TuiRunState   state;
    int64_t       createdAtMs;
    int64_t       durationMs;
    String        title;         // first user line, for the rail/minimap
    String        model;
    int64_t       contextUsed, contextMax;
    int64_t       tokensIn, tokensOut;
    TuiBlock*     blocks;
    size_t        blockCount;
} TuiTurn;

typedef struct TuiRuntimeStatus {
    int32_t  phase;              // idle|thinking|tooling|writing|waiting|error
    String   detail;             // current tool / model, one line
    int64_t  contextUsed, contextMax;
    int64_t  compactAt;
    double   tokensPerSecond;
    int64_t  elapsedMs;
    int32_t  pendingQuestions;
} TuiRuntimeStatus;

typedef struct TuiSessionInfo {
    String  id, title, preview, model;
    int64_t createdAtMs, updatedAtMs; // D converts SessionMeta's unix seconds
    int64_t messageCount;
    int32_t flags;               // active | running | unread | pinned
} TuiSessionInfo;

typedef struct TuiCommandInfo {
    String  name, aliases, usage, description; // aliases comma-separated
    int32_t argMode, group;
} TuiCommandInfo;

typedef enum TuiLogLevel {
    TuiLogLevel_Trace = 0, TuiLogLevel_Debug = 1, TuiLogLevel_Info = 2,
    TuiLogLevel_Warn = 3, TuiLogLevel_Error = 4,
} TuiLogLevel;

typedef struct TuiLogRecord {
    int64_t     timestampMs;
    TuiLogLevel level;
    String      module;      // LogEntry.moduleName
    String      file;        // LogEntry.file
    String      function;    // funcName
    int32_t     line;        // source line
    String      message;     // unformatted message
} TuiLogRecord;
```

Functions (all additive; v4 stays):

```c
// Bulk snapshot on session switch / replay (one copy, not N events).
void tuiSetTurns(TuiState*, const TuiTurn* turns, size_t count);
// Live updates: idempotent, keyed by id; bodies are copied.
void tuiTurnUpsert(TuiState*, const TuiTurn* turn);
void tuiBlockUpsert(TuiState*, String turnId, const TuiBlock* block);
void tuiTurnRemove(TuiState*, String turnId);
void tuiThreadClear(TuiState*);

void tuiSetRuntimeStatus(TuiState*, const TuiRuntimeStatus*);

void tuiSetSessions(TuiState*, const TuiSessionInfo* items, size_t count);
void tuiSetCommands(TuiState*, const TuiCommandInfo* items, size_t count);
void tuiLogAppend(TuiState*, const TuiLogRecord*); // structured; UI splits Activity/Raw

// Inspector snapshots (optional; each may be absent).
void tuiSetSources(TuiState*, String turnId, const TuiSource* items, size_t count);
void tuiSetMemoryPeek(TuiState*, String turnId, const TuiMemoryItem* items, size_t count);
void tuiSetAgents(TuiState*, const TuiAgentInfo* items, size_t count);

// UI -> D actions (one queue, typed).
int  tuiPollAction(TuiState*, TuiUiAction* out); // 1 = got one; caller frees strings
```

Design notes:

- **Copy-on-write into UI-owned memory.** `String` stays POD; the UI copies into
  `std::string`. D can free its buffers immediately after the call, as today.
- **Stable ids are mandatory.** `toolCallId` already exists on `Chat.ToolResponse`
  (`chat.d:953-962`), so a tool call and its result merge deterministically. Streaming
  updates target `(turnId, "answer")`; no more `tuiStreamChatMessageClear` flicker.
- **Throttle streaming.** D's `StreamMessageUpdater` currently sends one UI message per
  model chunk (`ui.d:440-451`). Coalesce to 20 Hz plus a final complete update; the UI
  cannot repaint faster than that anyway, and the mailbox stops carrying per-token
  traffic.
- **Numbers, not formatted strings.** `contextUsed/Max`, `tokensPerSecond`, `durationMs`,
  `tokensIn/Out` are fields. The UI formats them (`38.1 tok/s`, `12.4 s`, `42k/128k`).
- **No pre-baked emoji or prefixes.** Success is `TuiBlockStatus_Ok`, rendered as `✓`.
- **Logs are records, not strings.** `TuiLogger` currently formats
  `"%s - %s: %s [%s:%d]"` (`llm/tui/package.d`); v5 forwards the `LogEntry` fields
  (`logLevel`, `msg`, `moduleName`, `funcName`, `file`, `line`, `timestamp`) as
  `TuiLogRecord`. The UI decides what
  Activity shows (severity + plain message) and what only the Raw Log shows
  (module/function/line).
- **Bounded.** The store keeps a bounded window of turns/blocks in the same order of
  magnitude as today's 1000-message cap, with FIFO eviction; height caches evict with
  their turn.

### 8.3 What D must send (mapping)

| D source | New field | Notes |
|---|---|---|
| `Message.content` (role user/assistant) | prose block body | role decides the turn rule/label |
| `Message.thinking` | reasoning block body | duration from `processTime` |
| `Message.processTime` | block/turn `durationMs` | already computed |
| `ToolMessage.toolCalls[]` (`StreamToolCall.toolName`, `.arguments`) | tool block `title`, `meta` (args) | call side |
| `ToolResponse.toolCallId`, `.toolName`, `.success`, `.content`, `.processTime` | same tool block: status, body, duration | merge by callId |
| `ServerStat.context`, `.tokenCount`, `.predictedPerSecond` | `TuiRuntimeStatus`, turn meta | numbers |
| `agent_.modelContextSize`, `activeModelDisplayName()` | `contextMax`, `model` | numbers/strings |
| `SessionMeta.updatedAt` / `createdAt` (exists, `session/types.d:40-44`) | `TuiSessionInfo.updatedAtMs` | enables rail groups |
| `slashCommands()` registry | `TuiCommandInfo[]` | palette + `/` completion |
| Pipeline agents (`AgentStream`, pool) | `TuiAgentInfo[]` | inspector lanes |
| RAG retrieval results | `TuiSource[]` | needs instrumentation at the call site; Phase 4 |
| Memory reads (`memory_tier`) | `TuiMemoryItem[]` | needs instrumentation; Phase 4 |
| `LogEntry` (`logLevel`, `msg`, `moduleName`, `funcName`, `file`, `line`, `timestamp`) | `TuiLogRecord` | structured; Activity vs Raw Log split |
| `UiSessionItem` | `TuiSessionInfo` (extended) | keep the v4 path as an adapter |

If a source does not yet emit structured data (RAG/memory), the inspector shows "not
reported by this build" rather than fabricating it. Honesty beats a pretty empty panel.

### 8.4 UI -> D actions

```c
typedef enum TuiUiActionType {
    TuiUiAction_None = 0,
    TuiUiAction_Submit,          // text = query
    TuiUiAction_CancelRun,       // id = turn id (empty = current)
    TuiUiAction_RetryTurn,       // id = turn id
    TuiUiAction_AnswerQuestion,  // id = question id, text/index = answer
    TuiUiAction_Approval,        // id = request id, index = allow/always/deny
    TuiUiAction_SessionSelect, TuiUiAction_SessionNew, TuiUiAction_SessionRename,
    TuiUiAction_SessionDelete, TuiUiAction_SessionPin,
    TuiUiAction_Command,         // text = slash command line
    TuiUiAction_OpenArtifact,    // text = artifact id (local file: handled in UI; blob: D)
    TuiUiAction_SetModel, TuiUiAction_SetTheme,
} TuiUiActionType;

typedef struct TuiUiAction {
    TuiUiActionType type;
    String id;
    String text;
    int32_t index;
} TuiUiAction;
```

The D actor polls at most one action per frame (as today with `pollSessionAction`),
forwards it to the agent thread, and the agent thread executes it through the same
services the slash commands use. **No UI code calls agent internals**, exactly like the
current session-action queue.

### 8.5 Layout engine

One function computes the shell; every region is an explicit `Rect`:

```cpp
struct Rect { float x, y, w, h; };
struct LayoutPrefs { bool railOpen, inspectorOpen, focusMode; float railW; };

struct ShellLayout {
    Rect topBar, rail, thread, inspector, composer, nowBar, rawLog;
    bool inspectorOverlay, railOverlay, rawLogOpen;
};

ShellLayout computeShell(const Metrics& m, ImVec2 display, const LayoutPrefs& p);
```

- `views/shell.cpp` owns the split; `views/rail.cpp`, `thread.cpp`, `composer.cpp`,
  `inspector.cpp` receive their `Rect` and render inside it.
- No `SetCursorPos(DisplaySize.y - groupH)` arithmetic outside `computeShell`.
- Overlays (inspector on narrow widths, palette, settings) are positioned by the same
  layout, not by ad-hoc `ImGui::SetNextWindowSize` calls in three files.
- When the Raw Log is open, `computeShell` reserves its drawer rect (GUI) or its
  full-screen rect (TUI); no view computes it locally.
- The root window still uses `NoScrollbar | NoScrollWithMouse | NoMove | NoResize` and
  pins scroll to zero (`SetNextWindowScroll`), preserving the current invariant.

### 8.6 Metrics, tokens and primitives

```
ui/tokens.h        Theme tokens (Graphite, Parchment, Native), ColorDepth
ui/tokens_gen.cpp  token -> ImGuiCol_* and 256/16-colour mapping
ui/metrics.cpp     gu/row, font scale, DPI, profile metrics
ui/primitives.*    panel, card, chip, pill, rule, row, icon, meter, spinner,
                   disclosure, button, iconButton, tooltip, skeleton
ui/text.*          utf8 truncation, cell-width (wcwidth), wrap, ellipsis, tabular nums
ui/table.*         GFM table parser + text-grid formatter (TUI table block)
ui/icons.*         glyph table + ASCII fallback
ui/markdown.*      rich_md wrapper: RichMd::Style bridge, stable outer IDs,
                   selection policy, code-block host service, paged height hints
```

Rules:

- Components read `Theme` tokens only. `grep -r "ImVec4(" views/` must return nothing
  except theme definitions.
- `applyTheme(ThemeId, ColorDepth)` maps tokens to `ImGuiCol_*` once and sets
  `Style.WindowRounding`, `FrameRounding`, `ScrollbarRounding`, `FramePadding`,
  `ItemSpacing`, `WindowPadding` from metrics.
- `renderButton`-style "blank button + manual text" is replaced by a real widget with a
  label and a 1-row/24-px hit box. The TUI variant may still draw its label itself for
  cell-exact insets; that stays inside `primitives::button`.
- Every primitive has a `measure()` used by the layout, so rendering and measuring cannot
  drift. **Markdown is the exception**: rich_md has no measure API; its height is only
  known after a draw (see §8.7 and §8.12).

### 8.7 Thread rendering: bounded, not pixel-perfect

rich_md has no document model and no measure API, and it parses + draws every visible
fragment every frame. A pixel-perfect `ImGuiListClipper` over markdown heights is
therefore not implementable without a measure API or a scratch render pass. The plan is
**bounded rendering by turn**:

- Render the last N turns (N ≈ 30–50, profile-dependent) plus the streaming turn; show a
  `Load earlier` row above them. This bounds per-frame markdown cost regardless of
  session length.
- Collapse turns outside the window to a one-line summary (semantic zoom, §6.4); an
  expanded turn is rendered in full. Expanding/collapsing anchors the scroll to the
  clicked header so the view does not jump.
- Cache a turn's height **after** it was drawn, keyed by
  `(turn.id, turn.revision, contentWidth, density)`, for scroll anchoring and a generous
  overscan. Treat it as a hint, not a clipper contract; a streaming turn invalidates its
  height every update.
- Wrap each turn/block in a stable outer `ImGui::PushID(turnId/blockId)` before
  `RenderRaw`; rich_md's internal fragment ids are per-frame render order and would
  otherwise shift when the visible set changes.
- Drop height hints on width/density/theme change and on session switch.
- If a true clipper is later required, it needs an upstream rich_md measure API or a
  scratch render pass; neither is in P3. The P3 exit criterion is "1000 messages remain
  responsive via paging/collapsed turns", not "virtualised".
- **Defer measurement to P3.** rich_md re-parses/draws every visible fragment, so a
  markdown-heavy thread may miss the frame budget; if it does, collapse/page more
  aggressively. Do not block the design on a pre-MVP perf spike.

### 8.8 Render scheduling

Today the D actor ticks at 10 ms (2 ms while busy) and the GUI waits up to 1/60 s; the
TUI renders at 60/3 fps. The new loop renders only when something changed:

```
dirty = input | stream | status | layout | theme | animation
if (dirty) { render(); }
else { gui: glfwWaitEvents(); tui: poll input with timeout 100ms; }
```

- Streaming: 30 fps target (not 60); a text-only delta does not need 60.
- Animations: 60 fps for their 120–200 ms duration, then stop.
- Idle: block indefinitely; a mailbox event (new D message) wakes the actor.
- The actor tick remains for mailbox polling, but `tuiRender` is skipped when clean.
- This is also a battery/CPU win on llvmpipe, where 60 fps software rasterisation is the
  measured cost today (`doc/tui_design.md`, "Idle pacing").

### 8.9 File plan

```
cpp_tui/
  ui/           tokens, metrics, primitives, text, icons, markdown  (new)
  model/        ui_store.{h,cpp}: turns, blocks, sessions, commands,
                actions, revisions, bounds                         (new)
  views/        shell, rail, thread, blocks, composer, inspector,
                palette, canvas, settings, toasts, help, rawlog    (new)
  session_panel.cpp  -> views/rail.cpp (retired after cutover)
  tui_chat.cpp       -> split into views/thread.cpp + views/blocks.cpp
  tui_widgets.*      -> ui/primitives.* (retired)
  tui_settings.*     -> extended with theme/density/send-key, same parser
  tui_api.{h,cpp}    -> v5 functions + v4 adapters
  tui_backend_*.cpp  unchanged (the seam is good)
  test/              golden-grid fixtures, perf harness, contrast test
source/llm/
  app_agent/ui.d     typed emitters, no formatting
  app_agent/package.d  emit turns/blocks; keep v4 strings behind a flag
  app_agent/slash*.d expose command metadata
  tui/package.d      v5 bindings, action polling, store updates
doc/
  tui_design.md      rewritten around the Workbench + Ink (current doc becomes an
                     implementation appendix for the text rasteriser)
```

### 8.10 Testing strategy

| Test | What it guarantees |
|---|---|
| Golden grid snapshots (120×36, 100×30, 80×24, 60×20) | Layout and glyphs are stable; diffs are reviewable in text |
| Contrast test | Every text/background token pair ≥ 4.5:1 (or explicitly decorative) |
| Layout invariants | Regions tile the display, no overlap, min sizes, composer never covered |
| Command registry test | Every command has a shortcut or visible button and a palette entry |
| Keymap conflict test | No two bindings collide; plain keys never fire while a text field is focused |
| Composer key gate | Focus `InputTextMultiline`: Enter sends / Shift+Enter newline in TUI and GUI; if not, Enter stays newline and the Send button is the path |
| v5 round-trip | D structs → C ABI → UiStore → fields used by the views |
| Session replay | The same session renders identically twice; expansion state survives |
| Perf benchmark | 1000 messages (≈200 turns): frame budget, session-open time, memory ceiling |
| Developer surfaces | Raw log is closed by default; `Ctrl+Shift+L` / `/rawlog` toggles it; Activity contains no module/line data; no raw line ever reaches the thread |
| rich_md integration | Selection policy per profile; `RenderRaw` blocks transclusions; custom TUI table at 103×82/110×30/120×40 (no artifacts/blank); code-block row budget; markdown-heavy perf |
| Accessibility | Colour-blind simulation of the palette; no meaning by hue alone |
The existing headless harness pattern (`test_session_filter_smoke`, `test_tui_maxwidth`)
is exactly what golden-grid testing needs; no new machinery.

### 8.11 Migration (strangler, MVP first)

The migration deliberately has **two tracks**: an MVP that removes the pain and can replace
today's UI, and the post-MVP work that raises the ceiling. The typed model is the long
pole; it must not block the rescue.

**MVP track (≈4.5 weeks of build + 1 week of dogfood):**

1. **Tokens + metrics + primitives.** Replace `applyTheme`, `renderButton`,
   `renderSeparator`, the hard-coded `ImVec4`s. Visual change only; no data change.
2. **Shell.** Compute the layout; move the top bar, rail, composer and now bar to
   `views/*`; keep the legacy thread renderer behind the same shell.
3. **Legacy thread restyle.** Fix the invisible headers, replace role-coloured bars with
   role rules, keep markdown/thinking/copy/groups. No v5 model yet.
4. **MVP surfaces.** Activity + Raw Log, Agents tab, minimal Settings, minimal palette.
5. **Composer behaviour.** Enter send, Stop, placeholder, history; a setting restores
   the old Enter=newline behaviour.
6. **Parity tests, dogfood, `--legacy-ui` fallback.** Ship only when every current
   feature has a checked equivalent.

The shell consumes a thin adapter over the existing v4 messages; the v5 thread can
replace the legacy thread later without touching the shell. This is the deliberate
trade-off for speed: less elegance now, no rewrite later.

**Post-MVP track:**

7. **Store + adapter.** Add `model/ui_store` and v5 structs. `tuiAddChatMessage` creates
   a turn with one prose block; both renderers work; tests assert parity.
8. **Thread v5.** Turns, run ribbon, tool cards, reasoning, answer card, streaming
   identity; delete the legacy thread path.
9. **Full inspector, palette, search, canvas.** New surfaces on the typed model.
10. **Delete dead code** (`session_panel.cpp`, `tui_widgets.*`, the `showHeader` alpha
    hack, per-widget profile gates, the `--legacy-ui` path after one release).

Every step is shippable and reversible; no step requires the next one to compile.

### 8.12 rich_md: what it actually does (and what it costs the plan)

I read the vendored library (`README.md`, `rich_md.h`, `rich_md_host.h`,
`docs/architecture.md`, the code-block and selection code) after drafting the UI. It is
more capable than `doc/tui_design.md` suggests in some places, and more constrained in
one important place: **it has no document model and no measure API.**

**What it is:** md4c parser + immediate-mode renderer. `Render()` / `RenderRaw()` parse
and draw a fragment **every frame**; `rich_md.h` says so explicitly ("rich_md is
immediate mode, like Dear ImGui"), and `docs/architecture.md` confirms it "keeps no
document between frames, only caches (fonts, textures, formulas, diagrams)". `Render()`
unindents the text and resolves transclusions (file reads through `ReadAsset`);
`RenderRaw()` does neither. `MarkdownOptions::selectableText` defaults to **true**, and
llmfun sets it true in both profiles.

**Capabilities we should use instead of reinventing:**

- **Tables** (alignment, resizable columns), task lists, admonitions, `<details>`,
  autolinks, `<kbd>/<mark>`, and a heading callback (`OnHeading`) that can feed the
  thread outline/minimap.
- **Code blocks with a built-in `Copy` button**; syntax highlighting is a *build option*
  (`IMGUI_RICHMD_WITH_CODE_EDITOR`, ImGuiColorTextEdit), currently **OFF** in llmfun.
  Post-MVP it is enabled GUI-only via a profile-aware `RenderCodeBlock`.
- **Images** through `OnImage`/`UploadRgba`; URL download is a build option (OFF).
- **Mermaid** is ON in llmfun: native flowcharts, sequence and class diagrams with a
  documented subset (no themes, click, or markdown labels).
- **Selection** is built into rich_md: mouse selection, Ctrl+C copy, right-click menu with
  Copy / Copy as Markdown. Per-render control exists:
  `PushSelectableText` / `PopSelectableText` / `SetSelectableTextDefault`.
- **Host services** (`rich_md_host.h`): `RenderCodeBlock` is replaceable, so a custom
  Ink code-block header/line-numbers/copy can be installed without forking rich_md;
  `DefaultMergeFonts` can merge an icon font; `UploadRgba`, `ReadAsset`, `Download`,
  `Log` are replaceable.
- **Style hooks** (`RichMd::Style`): link, code, quote-bar and admonition colours,
  block/header gaps, `subSupScale`. These are separate from `ImGuiCol_*`.

**Constraints that change the plan:**

1. **No measure API / no document model.** `CalcTextSize` cannot reproduce rich_md's
   block layout (headings, lists, tables, code child windows, Mermaid). The §8.7
   "`CalcTextSize` + `ImGuiListClipper` over measured markdown heights" plan is not
   implementable as written. Use **turn-level bounded rendering**: render the last N
   turns plus a "Load earlier" row, collapse old turns to a fixed-height summary
   (semantic zoom), and treat any cached height as a hint, not a clipper contract. Do not
   promise pixel-perfect virtualization.
2. **Code blocks are child windows** (`BeginChild(..., AutoResizeY)`) with a built-in
   `Copy` button (`rich_md.cpp` `_RenderCodeBlockPlain`). On the 1-cell grid a child
   window costs the 4-row minimum. A custom header/line-number design must be a
   `HostServices::RenderCodeBlock` implementation, not a rich_md option.
3. **Syntax highlighting is a dependency choice**, not free: `IMGUI_RICHMD_WITH_CODE_EDITOR`
   pulls ImGuiColorTextEdit. The current build is OFF. Defer it to post-MVP and enable it
   **GUI-only**: turn the build option on and install a `HostServices::RenderCodeBlock`
   that uses the editor when `!tuiIsTextGrid()` and the plain block in the TUI. Do not
   write a custom lexer.
4. **Selection is activated**, and `doc/tui_design.md`'s "selection is off" statement is
   simply wrong. The real conflict is the **Ctrl+C binding** and window focus, not the
   absence of selection. Use `PushSelectableText(false)` in the TUI if Ctrl+C must stay
   quit; fix the GUI binding after testing both backends.
5. **Fragment IDs are per-frame render order**, not stable ids: `RenderRaw` does
   `PushID(fragmentCounter++)`. When paging or clipping changes the visible set, those
   ids shift and selection/expansion can jump. Wrap each block in an outer stable
   `ImGui::PushID(blockId)` before `RenderRaw`.
6. **`Render()` reads files** (transclusions). Keep `RenderRaw` for all model output and
   add a test with `![[...]]` in a message: a model must never trigger asset/file reads.
7. **Theme bridge is separate from ImGui colours.** Set `RichMd::Style` for link/code/
   quote/admonition colours and gaps; `ImGuiCol_*` alone will not theme markdown.
8. **Images and LaTeX are host/build options.** The TUI cannot draw textures; keep
   `OnImage=nullptr` there. GUI file images are feasible with `UploadRgba`; URL downloads
   need libcurl (OFF). LaTeX is a **post-MVP, last-priority, GUI-only** option: build with
   `IMGUI_RICHMD_WITH_LATEX=ON` (MicroTeX + FreeType) and install a `RenderLatex` that
   returns nullopt in the TUI (the source shows). It is not in the MVP; do the
   dependency/licensing review first.
9. **Per-frame cost is real.** Every visible fragment is parsed and re-drawn; markdown-
   heavy threads are expensive. Collapsing old turns and paging are the primary levers,
   not a display-list cache.
10. **Mermaid is cell-heavy in the TUI.** The clean split is per context: the GUI keeps
    rich_md's native Mermaid; the TUI registers a custom `mermaid` fenced-block renderer
    that shows a compact placeholder plus the source. `RegisterFencedBlockRenderer("mermaid",
    ...)` overrides the default registered by `CreateContext`. This is a small post-MVP
    item; the GUI wrapper adds a header, source toggle and max height.
11. **Native rich_md tables are not safe on the text grid, even when they fit.** At
    103×82 the render showed header text on the top border (`T`/`A` artifacts), missing
    left corners on some rows and a full-cell background flood; the headless probe showed
    a long 3-column table blanking at 110×30 (and a 4-column table blanking at 120×40),
    while short tables render. The fix is **not** a rich_md fit/wrap patch: the TUI uses a
    first-party **text-grid table renderer** (parse GFM, content-width columns, wrap, no
    background, source toggle), and the GUI keeps rich_md native tables. The legacy bridge
    preprocesses tables into a fenced `llmfun-table` block. This is MVP work; the upstream
    `TextGrid` table mode is optional post-MVP.
12. **TUI Ctrl+C is SIGINT, not an ImGui key.** The ncurses backend runs in cbreak with
    ISIG on, so `\x03` terminates the process before ImGui or rich_md sees it (the
    `scratch_app_key_smoke` probe records this as pre-existing). A TUI Ctrl+C copy/cancel
    requires raw mode (ISIG off) and an explicit handler — a backend change, not a keymap
    entry. Until then, keep Ctrl+C as quit and offer copy via the context menu or `y`.

**Consequences for the roadmap:** P2 sets the core `RichMd::Style` tokens (link, code,
quote bar, admonitions) so markdown matches Ink, adds the **custom TUI text-grid table
renderer** (MVP; native rich_md tables are GUI-only), and uses rich_md defaults for code
blocks (plain code with the built-in Copy; no custom lexer). P3's exit criterion is "1000
messages remain responsive via paging/collapsed turns", not "virtualised". P5 adds optional
**GUI-only syntax highlighting** (code editor build) and the **Mermaid TUI/GUI split** +
wrapper. P6 (the optional tail) adds **GUI-only LaTeX**. Tests must cover tables at
103×82/110×30/120×40, selection/focus, transclusion safety, code-block row budget, the
Mermaid split and a markdown-heavy perf fixture.

### 8.13 Upstream rich_md changes worth making

These are contributions to `vendor/imgui_rich_md`, not app-side hacks. The MVP table fix is
**app-side** (custom TUI renderer, §5.31/§8.12), so no upstream patch is required for the
merge; the rest are post-MVP.

| Change | Why | When |
|---|---|---|
| **Text-grid table mode** (optional) | An upstream alternative to the app-side custom renderer: ASCII tables with width fit/wrap, no cell backgrounds, no header/border offsets | Post-MVP (MVP uses the app-side renderer) |
| **Text-grid render mode** (`MarkdownOptions::renderMode = Pixels/TextGrid`) | One switch for ASCII tables, no code child windows, images/LaTeX/Mermaid off, cell-aware wrapping; removes app gates | Post-MVP |
| **Measure API** (`Measure(text, width) -> contentSize`, or `RenderRaw` returning it) | True virtualization and exact row budgets; rich_md currently has no document/measure model | Post-MVP |
| **Selection API** (`HasSelection`, `CopySelection`, `ClearSelection`, `OnSelectionChanged`) | Lets the host own Ctrl+C and block copy instead of relying on rich_md's internal key handling | Post-MVP |
| **Stable block IDs** (`RenderBlock(id, text)` or an explicit ID scope) | Paging/clipping shifts `fragmentCounter++`; selection/expansion can jump | Post-MVP (app shim now: outer `PushID`) |
| **Per-context runtime flags** for Mermaid/LaTeX/code editor | Build flags are coarse; the TUI wants Mermaid/LaTeX off while the GUI keeps them on | Post-MVP (app shims now) |
| **Code-block options** (language, line count, `copy`, `header`, `maxHeight`) | Custom Ink header/line numbers/copy; the current hook passes only `code, language` | Post-MVP |
| **Mermaid wrapper** (source toggle, max height, placeholder) | GUI diagrams need chrome; the TUI wants source/placeholder | Post-MVP (app wrapper now) |
| **Table block hook** | Tables are not fenced blocks; a hook enables ASCII/key-value layouts without patching the parser | Post-MVP |
| **Optional parse/AST cache** keyed by text hash | Avoid md4c re-parsing every frame on markdown-heavy threads; layout still draws | Post-MVP |

Rules:

- Do not fork rich_md in the MVP. Use the existing seams: `RenderRaw`, outer stable
  `PushID`, `RegisterFencedBlockRenderer` (including the `llmfun-table` bridge),
  `HostServices::RenderCodeBlock`, `HostServices::RenderLatex`, `RichMd::Style`.
- **The MVP table fix is app-side**: a custom text-grid table renderer, not a rich_md
  table patch. The upstream text-grid table mode is a post-MVP nice-to-have.
- Gate text-grid behavior on an option/profile (like the existing
  `ImTui_TextEncodingActive` convention) so the pixel path stays byte-identical.

### 8.14 UI capability handshake (post-MVP, P4)

The UI must tell D what it can render, so the agent can choose prose, tables, Mermaid or
LaTeX instead of guessing. Add an additive capability struct:

```c
typedef struct TuiCapabilities {
    int32_t apiVersion;
    int32_t backend;         // Tui | Gui
    int32_t cols, rows;
    int32_t colorDepth;      // 16 | 256 | 24-bit
    uint32_t flags;          // unicode | mouse | selection | focus | ...
    int32_t tables;          // none | text-grid | native | source
    int32_t mermaid;         // none | source | native
    int32_t latex;           // none | source | rendered
    int32_t images;          // none | file | url
    int32_t syntaxHighlight; // none | native
} TuiCapabilities;

TuiCapabilities tuiGetCapabilities(TuiState*);
```

- D reads it at startup and on resize/backend switch; the prompt builder injects a
  trusted context block, for example:
  `UI: TUI 110x30, 256 colours, Unicode, mouse. Markdown, tables: text-grid, code: plain, mermaid: source, latex: source, images: none. Prefer tables ≤ 4 columns and ≤ 80 chars.`
  (The example assumes the P5 Mermaid split; before it, the TUI reports `mermaid: native`.)
- **Send only when it changes.** Startup always sends. After that, compare the new struct
  with the previous one **field by field** (no hash) and send only on a difference; queue
  to a turn boundary and rate-limit to at most once per 30 s; coalesce multiple changes.
- **Build-aware:** use `RichMd::HasLatex()`, `HasCodeEditor()` and `HasUrlImages()` so a
  GUI build without the backend reports `latex: none` / `syntaxHighlight: none` /
  `images: none`, not a capability it cannot deliver.
- GUI: `tables: native, mermaid: native, latex: rendered (P6), images: file, syntax: native`.
  The model can then use the supported Mermaid subset and `$...$` only where they render.
- Capabilities are host-provided, never model- or tool-controlled; they are updated on
  resize/backend switch, and `/ui` shows them to the user.
- The handshake depends on the v5 status path but is additive; it can land before the
  inspector work. Tests: TUI/GUI round-trip, resize update, prompt content, no model
  control.

---

## 9. Roadmap

### 9.1 The MVP — the merge gate

**Definition:** a release that can replace today's UI for every current user without
losing a feature. It is not a demo and not the full Workbench.

**Scope:**

- New shell: top bar, rail, thread (restyled legacy renderer), composer, now bar,
  inspector frame with **Agents** and **Activity**.
- New surfaces: **Raw Log** overlay (developer-only, off by default), minimal
  **Settings**, minimal **Ctrl+K** palette.
- **Graphite theme only** in the MVP; Parchment/Native arrive in P5.
- **1280×820** GUI default; **110×30 is the reference terminal**; the 40×15 floor is unchanged.
- **TUI tables use a first-party text-grid renderer** (custom renderer, source toggle);
  GUI tables keep rich_md native. Native rich_md tables are not used in the TUI (header/
  border artifacts, background flood, width blanking; §5.31, §8.12).
- **Composer key gate:** `Enter` sends / `Shift+Enter` newline only if proven with
  `InputTextMultiline` active in both backends; otherwise keep `Enter`=newline and the
  real Send button (§5.12).
- **No v5 turn/block model** (small additive fields for log level and runtime status are
  allowed), no run ribbon/tool cards/answer card yet. The existing message groups are
  restyled and kept; no information is lost.
- Minimal Settings: font scale (GUI), send key, Raw Log toggle.
- **Everything in §9.2 is checked.**

**Important:** the MVP ships the **new shell around the restyled legacy thread**, not the
full turn/ribbon layout shown in §3.2/§5.4. The MVP thread has readable headers, neutral
surfaces and role rules, but keeps today's message groups; the Workbench turn model,
ribbon and tool cards arrive in P3.

**What the MVP is not:**

- Not the full inspector (Context/Tools/Sources/Memory are post-MVP).
- Not GUI-only syntax highlighting, the Mermaid TUI/GUI split, or GUI LaTeX (P5/P6).
- Not universal search, canvas, minimap, semantic zoom, ask/approval.
- Not the keymap overhaul: `Ctrl+C` keeps quitting, `Ctrl+Q` is added as an alias; the
  copy/cancel semantics change later with a migration note.

**Timeline:** P0 (3–5 days) + P1 (1–1.5 weeks) + P2 (2.5–3 weeks) ≈ 4.5 weeks of build,
plus one week of dogfood ≈ **5.5 weeks to the default flip** (range 5–6.5).

**Exit criteria:**

- Every row in §9.2 checked, with a test or a manual script.
- Core loop passes in both backends: start → session → ask → stream → stop → answer →
  copy → agents → activity/raw log → settings → quit.
- Golden screens at 120×36, 100×30, 80×24 and GUI 1280×820.
- Frame time and session-open time within 10% of today.
- One week of dogfood with no P0/P1 regression.
- `--legacy-ui` works; the default is flipped; docs and release notes are updated.

### 9.2 Parity checklist

| Area | Today | MVP | Verification |
|---|---|---|---|
| Thread rendering | markdown, code, links, thinking | same renderer, restyled | golden screens |
| Markdown tables | native rich_md tables have header/border artifacts and can blank | custom TUI text-grid renderer; no artifacts/blanking; source toggle | golden 103×82, 110×30, 120×40 |
| Streaming + thinking time | stream message, timer | same | streaming test |
| Tool call/result | two text messages | compact rows, same text | golden |
| Session list | panel, preview, count | rail, same data | session smoke |
| Session filter | fzf filter | same | existing fuzzy tests |
| Session select/new/rename/delete | actions queue | same queue | session smoke |
| Busy gating/pending select | guards | same | session smoke |
| Pipeline agents | left panel: main/clear/list/stream | **Agents** inspector tab, same controls | pipeline smoke |
| Log | Log tab, collapsing rows | **Activity** + **Raw Log** | log tests |
| Status | one sentence | chips (structured status if available, string split as fallback) | mapping test |
| Composer | multiline + fake Send + Prev/Next | visible composer, real Send/Stop, history | interaction tests |
| Enter behaviour | newline; send via fake field | Enter sends; setting restores old behaviour | interaction test |
| Font scale | GUI View→Options | Settings sheet | settings test |
| Window size/DPI/font env | persisted/backend | same | settings test |
| Backend auto/fallback/startup errors | C++ init | same | existing tests |
| Clipboard | xclip/GLFW | same | manual/unit |
| Minimum size | 40×15 error page | same | maxwidth test |
| Keys | Ctrl+C, Ctrl+L, End, Esc | same + Ctrl+Q alias | keymap test |
| Slash commands/help | typed | typed; palette lists top actions (command list can be static) | manual |
| File logging | `tuiSetLogging` | same | log test |
| Session persistence/replay | session files | same, no format change | session tests |
| One-shot CLI | unaffected | unaffected | CLI tests |

**New in MVP (not parity, but required for the redesign):** readable role headers,
neutral surfaces, top bar, rail, now bar, Activity/Raw Log, custom TUI table renderer,
minimal Settings, minimal palette, 1280×820 GUI.

**Deferred to post-MVP:** v5 turns/blocks, run ribbon, tool cards with args/diffs,
answer card, Context Lens, full inspector tabs, universal search, canvas, minimap,
semantic zoom, ask/approval, Parchment/Native themes, density, full settings,
virtualization (pixel-perfect), keymap overhaul.

### 9.3 Cutover and rollback

- **Flag:** `LLMFUN_UI=workbench|legacy` and `--ui=...`; `--ui` selects the UI
  generation while `--tui`/`--gui` still select the backend. Default `legacy` during
  development, `workbench` at the MVP release.
- **Merge gate:** the branch may merge only when §9.2 is green and `--legacy-ui` works;
  the default flip is a separate release commit.
- **Dogfood:** one week on `workbench` before the default flip, using the same
  sessions/settings files.
- **Compatibility:** no data migration. Both paths read the same session files. Window
  size and `FontScaleMain` stay in `imgui2.conf`; MVP-only settings live in a sibling
  `ui.conf` that legacy never writes, so a legacy save cannot clobber them. New keys are
  ignored by legacy; the two files merge after legacy is removed.
- **Release:** flip the default; keep `--legacy-ui` for one release. Document the one
  intentional behaviour change (Enter sends) and the setting to restore it.
- **Rollback:** `--legacy-ui` / `LLMFUN_UI=legacy`; if a blocker appears, flip the
  default back without touching data.
- **Removal:** delete the legacy path and the restyle shims after one release.

### 9.4 Full program (MVP and post-MVP)

P0–P2 are the MVP; P3–P5 are the post-MVP improvements; P6 is the optional GUI-math tail.
Effort is for one experienced developer familiar with the codebase; parallel work can
compress P3–P4.

| Phase | Scope | Effort | Exit criteria |
|---|---|---|---|
| **P0 — Quick wins** | first MVP slice: composer send/stop, visible input, minimal palette, neutral palette, status chips, session row + scrollbar, empty state, GUI 1280×820 | 3–5 days | First dogfood build; no behaviour regressions |
| **P1 — Foundation** | `ui/` tokens, metrics, primitives, icons; layout engine; theme switch; GUI style; golden-grid harness | 1–1.5 weeks | No hard-coded colours; 4 breakpoints render; contrast test green |
| **P2 — Shell + MVP surfaces** | top bar, rail (filter/select/new/rename/delete, scrollbar), legacy thread restyle, composer v2 (chips, meter, Send/Stop), now bar, Agents + Activity inspector, Raw Log, minimal Settings, minimal palette, rich_md Style bridge, custom TUI table renderer | 2.5–3 weeks | §9.2 parity green; MVP exit criteria met |
| **P3 — Thread v5** | v5 store, turns, run ribbon, work section, tool cards, reasoning, answer card, streaming identity, minimap, semantic zoom, table inline styling | 2–2.5 weeks | 50-turn session readable; tool failures retry; no flicker; 1000 messages responsive via paging/collapsed turns |
| **P4 — Intelligence** | full inspector (Context, Tools, Sources, Memory), Context Lens, Why?, canvas, universal search, ask/approval cards, UI capability handshake (§8.14) | 1.5–2 weeks | "Why?" answers with sources; search jumps to a turn; the model prompt reflects the active UI capabilities |
| **P5 — Polish** | Parchment + Native themes, density, full settings sheet, help overlay, focus mode, notifications, GUI-only rich_md syntax highlighting (code editor build), Mermaid TUI/GUI split + wrapper, accessibility audit, docs, onboarding | 1–1.5 weeks | Light theme ships; no contrast failures; keymap complete; docs rewritten |
| **P6 — Optional GUI math (after P5)** | GUI-only LaTeX via MicroTeX/FreeType; TUI keeps the source | 3–5 days + dependency/licensing review | Formulas render in the GUI; TUI source unchanged; build remains optional |

Dependencies: P0 → P1 → P2 (MVP); P3 needs the v5 store (can start in parallel with the
end of P2); P4 needs P3; P5 next; P6 is optional and last.

Totals: **MVP build ≈ 4.5 weeks** (P0–P2, range 4–5.5); **default flip ≈ 5.5 weeks**
including one week of dogfood (range 5–6.5); **post-MVP ≈ 5 weeks** (P3–P5), plus the
optional P6 (3–5 days + review); **full program ≈ 10–11.5 weeks** excluding the optional P6.

The custom TUI table renderer is the schedule risk in P2 (3–5 days). If it overruns, cut
P5 scope, not the MVP: TUI tables must not blank or show the native rich_md artifacts.

What to cut if time is short: universal search (keep the palette), the memory tab, the
canvas (keep tool detail inline), Parchment. What never to cut from the MVP: any row in
§9.2, the composer, readable headers, the session rail, the Agents/Activity split, the
legacy fallback.

---

## 10. Risks, non-goals, decisions

### 10.1 Risks

| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| Scope creep ("while we're here…") | High | High | Phase gates; P0 first; each phase shippable |
| Text grid cannot express the design | Medium | High | Dual-fidelity spec + golden grids at 60 cells before implementation |
| rich_md has no document/measure model | High | Medium | Bounded turn rendering, paging/collapsed turns, post-draw height hints; no pixel-perfect clipper; measure a markdown-heavy fixture |
| rich_md re-parses/draws every visible fragment | High | Medium | Bound visible turns, collapse old ones, 30 fps streaming, frame budget |
| C ABI churn breaks D | Low | High | Additive v5; v4 adapters; round-trip tests; version check |
| Font assets inflate the binary / licensing | Low | Low | Subset OFL fonts; embedded Roboto fallback; ≤ 1.5 MB budget |
| Users dislike changed keys (Enter/Ctrl+Q) | Medium | Medium | One-time hint, settings to restore old behaviour for one release |
| 16-colour / NO_COLOR looks flat | Medium | Medium | Native theme + glyph/label redundancy; tested in CI via token simulation |
| ImGui has no accessibility tree | High | Medium | Keyboard-complete, high contrast, text dumps; document the limitation |
| Pipeline/agent instrumentation too invasive | Medium | Medium | Inspector degrades honestly ("not reported"); ship what exists |
| Native rich_md TUI tables artifact/blank | High | High | Custom text-grid table renderer in P2; golden tests at 103×82/110×30/120×40 |
### 10.2 Non-goals

- A web or Electron front end; remote/multi-user sessions.
- Replacing Dear ImGui or the C ABI architecture.
- Images/rich media in the TUI (GUI images maybe later); LaTeX is GUI-only and optional
  (P6).
- A plugin UI API (slash commands stay the extension surface).
- Theming beyond the three built-in themes in v1.
- Editing files in the canvas (read-only; `$EDITOR`/OS handler opens externally).
- RTL/bidi layout in v1: the text layer is UTF-8 and fonts fall back for non-Latin
  scripts, but ImGui's layout engine is left-to-right.
- Cost estimation (local models dominate; tokens and tok/s are the honest metrics).

### 10.3 Decisions

All previously open questions are resolved; these are the decisions the implementation
plan should follow.

1. **Pinning sessions:** UI-local in `imgui2.conf` (the simpler option). Can move to
   `SessionMeta.extra` later if cross-machine sync is wanted.
2. **Message ordinals:** show `#12` dim in the turn meta only; never in the header.
3. **Ctrl+C:** change to copy/cancel and move quit to `Ctrl+Q`, with a one-time hint.
   This is post-MVP; the TUI form needs raw mode (§8.12). The MVP keeps Ctrl+C quit.
4. **Session titles:** derive from the first user message, as today. An agent-proposed
   title is post-MVP (it costs a model call); do not tax the model in the MVP.
5. **Inspector default:** open at ≥ 1200 px, collapsed otherwise.
6. **Compaction marker:** the configured threshold, else 90%.
7. **Reasoning visibility:** a setting `thinking: closed | open | auto`; **default
   closed**. `auto` opens while streaming and collapses when done.
8. **Ask/approval:** the ask/approval cards and the `toolApproval: auto | ask` setting
   ship in P4; **default `auto`** for now, revisit the policy later.
9. **Raw log activation:** CLI (`--debug-ui`), the `uiLog: raw` setting, and the
   `/rawlog` command; **no environment variable**. The drawer never auto-opens; the
   first open shows a one-line explanation/confirmation.

---

## 11. Final honest points

- **The typed model is the ceiling, but it is not the rescue.** The boundary is the
  highest-leverage long-term change; the users are complaining about the UI *now*. Fund
  the MVP (shell, composer, readable thread) first; start v5 in parallel only if it does
  not delay the merge. Painting alone buys a week, v5 buys years — do both, in that order.
- **The second is the composer.** A chat app where you cannot see the input, cannot press
  Enter to send, and cannot press Stop is not "ad-hoc", it is broken. P0 fixes this in
  days and changes how the tool feels immediately.
- **The third is the dead weight.** Transparent headers, text-as-buttons, the mutually
  exclusive left panel, the per-widget profile gates: deleting these is worth more than
  any new feature.
- **A design system that specifies cells and pixels is not bureaucracy.** It is the only
  way two renderers stay one product. The current 1,171-line design doc is the cost of
  not having one.
- **Do not chase "modern chat app".** The competition is not ChatGPT's UI; it is a
  debugger, a good terminal multiplexer, and a well-set book. llmfun's advantage is that
  it can *show the work* — tools, sources, memory, agents, diffs — and no chat app can.
- **The terminal is the soul.** The GUI should be the same instrument with better
  typography, not a separate product. If a design only works in pixels, it is not done.
- **Ship P0 in week one, the MVP by week five, the model after.** Taste is proven by the
  first screenshot, trust by the architecture; neither is a substitute for the other.

### 11.1 The bar we are aiming at

The inspiration is the current class of terminal coding agents (Codex, Claude Code,
opencode, and the DeepSeek/Pi harnesses), but the bar is **agency and trust**, not a chat
transcript. Those tools are moving targets; copying their current UI is chasing a
snapshot. Copy the invariants.

| Bar | What it means | llmfun today | Next |
|---|---|---|---|
| **Loop** | plan → act → verify → recover, with visible state | strong tools and pipeline; verification is often ad hoc | plan/todo execution with test/verify steps and stop conditions |
| **Trust** | permissions, sandbox, diff review, apply/reject, approvals | sandbox exists; no first-class diff/approval surface | Canvas diff/apply/reject; approval cards (P4) |
| **Context** | repo map, AGENTS.md, retrieval, compaction, memory | RAG and memory tiers are strong; repo map/compaction UI missing | Context Lens, Sources, AGENTS.md visibility |
| **Continuity** | resume, background work, branch/replay | sessions persist; resuming an interrupted run is missing | resume/background post-MVP |
| **Extensibility** | MCP, skills, commands, hooks | MCP server, skills, slash commands; command registry is fragmented | unified registry, MCP client surface, hooks |
| **UX** | keyboard-first, fast, legible | TUI fast; GUI dated; discoverability poor | the Workbench MVP |

**Lean into what is already different:** local/self-hosted models, RAG + memory tiers,
the multi-agent pipeline, one core for TUI and GUI, the C ABI, the MCP server and tool
metrics. Do not become a clone; make those legible in the Workbench.

**Order of substance after the MVP** (this does not renumber P3–P6; it orders the work
inside them and flags two items that need explicit tasks):

1. Diff/review/apply/reject in the Canvas, with test results attached.
2. Plan/todo execution with verification — “done” only when the test/check passes.
3. Approval modes and sandbox policy — `auto` now, but ready to tighten.
4. Context/compaction/AGENTS.md — the model context as a visible, editable object.
5. The subagent runtime (`plan/appendix_agent_runtime.md`); the pipeline is not a general
   runtime yet.
6. Session resume/background for long tasks.
7. An eval harness — the honest scoreboard.

**The level test** is not a screenshot. Run five scenarios against llmfun and one or two
reference tools: fix a failing test; implement a small feature from a plan with tests;
review and apply/reject a diff; resume an interrupted run; answer a repo question with
RAG citations. Measure success, wall time, tokens, human interventions and regressions.

**Do not:** clone a chat-app layout; add features that cannot be reviewed or undone;
assume a cloud model or a rich display; put Mermaid/LaTeX/theme work ahead of diffs,
verification and approvals. The Workbench is the face of this level; the loop and the
trust surface are the substance.

---

## Appendix A — P0 quick-win checklist (concrete, low risk)

| # | Change | Files | Notes |
|---|---|---|---|
| 1 | Real composer button + Enter send / Shift+Enter newline / Stop | `tui_chat.cpp:749-851` | Replace the read-only `InputText` Send; keep Tab behaviour. Gate: prove Enter/Shift+Enter with `InputTextMultiline` active in both backends; otherwise keep Enter=newline |
| 2 | Visible composer frame + placeholder + caret | `tui_chat.cpp`, `tui.cpp` (theme) | `FrameBg` for the input; `InputTextMultiline` with a real size |
| 3 | Kill invisible headers | `tui_chat.cpp:80-100`, `247-266`, `638-645` | Always draw a compact header row with a readable role colour; remove the alpha-0 branch |
| 4 | Neutral palette + role rule | `tui.h:50-95`, `tui.cpp:61-116` | `systemBg` → surface, role colours as foreground/label only |
| 5 | Status chips | `tui_chat.cpp:782-787`, `ui.d:421-425` | Split the existing status string on its separator as a stopgap; replace with `TuiRuntimeStatus` in P1 |
| 6 | Session rows: 1 line, scrollbar, no row-menu dot | `session_panel.cpp`, `tui.h:201-244` | `BeginChild` gets a vertical scrollbar; the row menu moves to `m`/hover |
| 7 | Empty state | `tui_chat.cpp` (thread) | Wordmark + 3 starter actions + `Ctrl+K` hint |
| 8 | GUI default 1280×820 + Roboto 16 + rounding/spacing | `tui_backend_gui.cpp:37-49`, `tui.cpp:61-116` | Persisted size still wins; no font asset change required (Roboto is embedded) |
| 9 | Palette skeleton over actions + sessions | new `views/palette.cpp` | Static action table for the MVP; command registry post-MVP |
| 10 | Ctrl+Q quit (confirm when running), Ctrl+K palette | `tui_chat.cpp`, `main.cpp` | Keep Ctrl+C behaviour for one release, log a hint |

## Appendix B — v5 header sketch (for review)

```c
#define TUI_API_VERSION 5

typedef struct TuiState TuiState;

// existing v4 functions unchanged: tuiCreateState, tuiInit, tuiRender,
// tuiBackendNewFrame/Render, tuiDestroyState, tuiAddChatMessage, ...
// v5 additions:
void tuiSetTurns(TuiState*, const TuiTurn*, size_t);
void tuiTurnUpsert(TuiState*, const TuiTurn*);
void tuiBlockUpsert(TuiState*, String turnId, const TuiBlock*);
void tuiTurnRemove(TuiState*, String turnId);
void tuiThreadClear(TuiState*);
void tuiSetRuntimeStatus(TuiState*, const TuiRuntimeStatus*);
void tuiSetSessions(TuiState*, const TuiSessionInfo*, size_t);
void tuiSetCommands(TuiState*, const TuiCommandInfo*, size_t);
void tuiSetSources(TuiState*, String turnId, const TuiSource*, size_t);
void tuiSetMemoryPeek(TuiState*, String turnId, const TuiMemoryItem*, size_t);
void tuiSetAgents(TuiState*, const TuiAgentInfo*, size_t);
int  tuiPollAction(TuiState*, TuiUiAction*);
void tuiFreeAction(TuiUiAction*);   // frees the owned Strings
```

Compatibility rule: if `tuiSetTurns`/`tuiTurnUpsert` has ever been called, the UI renders
the typed store and ignores v4 chat messages; otherwise it renders the legacy store. A
single boolean, tested both ways.

## Appendix C — File fate

| Today | Fate |
|---|---|
| `tui.cpp` (render dispatch) | Kept, slimmed: state accessors + `tuiRender` + theme application |
| `tui_chat.cpp` (955 lines) | Split into `views/thread.cpp` + `views/blocks.cpp` + `views/composer.cpp` |
| `session_panel.cpp` (729 lines) | Replaced by `views/rail.cpp` (fuzzy matcher kept as-is) |
| `tui_widgets.*` | Replaced by `ui/primitives.*` |
| `tui_common.*` | Absorbed by `ui/text.*` |
| `tui_backend_*.cpp` | Unchanged (the seam survives) |
| `tui_api.{h,cpp}` | v5 additions + v4 adapters; version 5 |
| `main.cpp` | Add `--frames` scenarios for the new golden screens |
| `doc/tui_design.md` | Rewritten; the rasteriser/patcher sections move to an implementation appendix |

## Appendix D — Remaining design review questions

Resolved decisions are in §10.3. These are the remaining design details:

1. Is the inspector the right home for tools, or should tool details open in the canvas
   directly?
2. Does the run ribbon belong above the work section or in the turn meta row?
3. Is `Why?` the right word, or should it be `Evidence` / `Sources`?
4. Should the palette's free-text fallback send to the agent, or require a modifier?
5. Is 30 fps streaming visibly worse than 60 in the GUI? (Measure on llvmpipe in P3.)

---

*End of proposal. The next artifact should be `plan/modern_ui_tasks.md`: the same phases
broken into single-session code tasks with acceptance criteria, MVP first — P0 → P1 → P2
(with the §9.2 parity checklist as the merge gate) — then P3 → P4 → P5, plus the optional
P6. The typed model (P3) starts after the MVP merges; it is the ceiling, not the rescue.*

## Appendix E - User Notes

### Agents Tab
- In Agents when I click on it I seem to only see the currently streamed message. I do not see see any more messages. In the old design I could at least see the last 10 messages.
    - I am also not seeing how many messages they have produced. In the old UI there was a [NR] beside the agent.
      Will this be added back later on (task 33+)?
- I also want to see tok/s for the active agent
- There is no highlight that there is activity under the Agents tab, like there is for the Activity tab
- In the last agent that is executed the used context is not shown.
- I only see the tool call part when I have an agent focus. No thinking, no content

### Session
- The animation when the active session is working is very annoying. Remove it (the one that goes /-\)

### Main window
- The D side should stop adding x and v to tool responses. The UI should instead show the success status and the D side just send over the data.

- Tool calls and responses need to be enhanced. They are very ugly now. It is just a json raw dump in screaming white color that hurt the eye.

- End that is visalized to mark the end of the thinking trace when streaming a message look very bad.
    End
    ----
  How can it be improved? Make a couple of proposals

- consolidation. There now seem to be duplicate UI elements for the same thing.
    - when an agent run I now see two different timers
        - top right "running" - I think this one should be removed. The one at the bottom left is enough.
        - bottom left "writing" (can also be other texts but the importent is the time)
        I think this is a bit unncessary. Can't they be consolidated?
    - Why do I seem to have two statuses for what the main agent is doing, the context and tok/s. I have one under "Ask anything / commands" and another one right under it divided in a left and right side.
        - I think the one in the input field is prettier looking but it is a repeat of the one a row down. It is better if it is moved down and replace the ctx that exist there. The thinking part is redundant and can be removed, the same with the tok/s
    - I see Ctrlk+K both at the top right and bottom right.
    - I seee the model two times in the bottom right. The one at the far bottom is enough so the one two rows up can be removed.
  
### Raw Log
- I cannot scroll horizontal when needed.
- It is very unclear which log messages I can open for more information. It seems to be only StreamResponse
- I see the records dropped counter increasing. It started increasing by a lot at 1500+. It like "marked" each message that it showed as dropped, but it wasn't dropped. So I think some counter that is used isn't working as expected.
    - When a record is dropped an empty "log message" should be added at its position so it is possible to see where it was dropped
- in settings what really do log level "raw"?
- "--debug-ui" should set the log level to trace

### Input field

history is not populated from session load.

In settings the enterNewline option for Send. How do I change it?

tui_api.cpp
- I added a remark about the use of a global variable that I would like you to fix.

# Misc
- The thread adapter is still v4. Why is that? Shouldn't we migrate to fully v5 and then basically "deprecate"/remove this adapter? Isn't v5 better?
- Need a "?", help button
- It is very annoying that while a "execute_command" is running a newline keep on being printed and removed.

