# Changelog

## [Unreleased]

- "Columns & Tables" demo window (0986676)

### llmfun vendored-ImGui maintenance (not part of upstream imtui)

Maintainer record of the Dear ImGui upgrade in this vendored tree:

- Dear ImGui (github.com/ocornut/imgui) snapshot in `third-party/imgui/imgui/` upgraded 1.81 → 1.92.9b
  (upstream commit `f1cc2ae15e53a861a874c3034aae6798fde194ab`, tag `v1.92.9b`,
  IMGUI_VERSION_NUM 19291; ref recorded in `third-party/imgui/commit.txt`).
- The three llmfun patches previously carried on the 1.81 `imgui_draw.cpp` —
  `fe3b580` ("Replace REPL with a TUI"), `178d910` ("imtui: support utf-8 full
  range"), `799bfc9` ("Partial fix to imtui off by one render bug") — are
  re-applied onto the 1.92 sources (one combined patch block; see its header
  inside `third-party/imgui/imgui/imgui_draw.cpp`).
- New input contract (1.87+ removed `io.KeyMap`/`io.KeysDown`/`GetKeyIndex`):
  the ncurses backend now feeds `io.AddKeyEvent()` down+up pairs
  (modifiers as `ImGuiMod_*` pairs, mouse via `AddMousePosEvent` /
  `AddMouseButtonEvent`) and sets `io.ConfigInputTrickleEventQueue = true`
  explicitly in Init (`src/imtui-impl-ncurses.cpp:127`), so a same-frame
  down+up pair still registers as a press (trickling defers the up event
  to the next frame); the text backend is render-only
  (`ImTui_ImplText_Init` sets style + the legacy 1px font/atlas) and
  feeds no input events.
- Known stale file: `src/imtui-impl-emscripten.cpp` still targets the 1.81 API
  — it is excluded from the build (EMSCRIPTEN gate) and was deliberately not
  migrated.
- Cell-geometry behavior-delta audit 1.81 → 1.92 (pixel-snap, glyph-quad/cell
  mapping, wrap VS16, `RenderChar` ellipsis) with dispositions:
  `test/utf8_verification.md`, section "Task 9 (P1): behavior-delta audit
  1.81 → 1.92 — dispositions".

## [1.0.4] - 2021-04-03

- Update Dear ImGui to v1.81 ([#24](https://github.com/ggerganov/imtui/pull/24))
- Fix CMake install targets

## [1.0.3] - 2021-01-09

- Update Dear ImGui to v1.79
- Add Windows support with MSYS2 + PDCurses (39f34ea)

## [1.0.0] - 2020-12-15

- Initial release with Dear ImGui v1.77 backend

[unreleased]: https://github.com/ggerganov/imtui/compare/v1.0.4...HEAD
[1.0.4]: https://github.com/ggerganov/imtui/releases/tag/v1.0.4
[1.0.3]: https://github.com/ggerganov/imtui/releases/tag/v1.0.3
[1.0.0]: https://github.com/ggerganov/imtui/releases/tag/v1.0.0
