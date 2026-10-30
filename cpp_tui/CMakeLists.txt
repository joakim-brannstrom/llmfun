cmake_minimum_required(VERSION 3.20)
project(llmfun_tui VERSION 0.1.0 LANGUAGES C CXX)

# C++17 for our sources (imtui uses C++11 internally, unaffected)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Align with imtui.mak: disable shared libs, disable curl, skip examples
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(IMTUI_SUPPORT_CURL OFF CACHE BOOL "" FORCE)
set(IMTUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)

# Bring in imtui as a subdirectory — builds imtui, imtui-ncurses, imgui-for-imtui
# imtui-ncurses transitively provides the PUBLIC include path, so no extra target_include_directories needed
add_subdirectory(../vendor/imgui ${CMAKE_BINARY_DIR}/imgui)
add_subdirectory(../vendor/imtui ${CMAKE_BINARY_DIR}/imtui)

# --- imgui_rich_md (rich markdown; md4c-based; no third-party deps in this config) ---
# rich_md expects a target named `imgui` from its parent; create the interface
# before add_subdirectory. Its bare "imgui.h"/"imgui_internal.h" includes need
# the core imgui dir on the path; IMGUI_USE_WCHAR32 propagates from imgui-for-imtui.
add_library(imgui INTERFACE)
target_link_libraries(imgui INTERFACE imgui-for-imtui)
target_include_directories(imgui INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/../vendor/imgui/imgui)
set(IMGUI_RICHMD_WITH_CODE_EDITOR OFF CACHE BOOL "" FORCE)
set(IMGUI_RICHMD_WITH_LATEX OFF CACHE BOOL "" FORCE)
set(IMGUI_RICHMD_WITH_DOWNLOAD_IMAGES OFF CACHE BOOL "" FORCE)
set(IMGUI_RICHMD_WITH_MERMAID ON CACHE BOOL "" FORCE)
set(IMGUI_RICHMD_EMBED_ASSETS ON CACHE BOOL "" FORCE)
# The GUI's default UI font (Roboto-Medium, copied from
# vendor/imgui/imgui/misc/fonts/) rides in rich_md's embedded-asset table: the pixel
# backend's configureUiFont() (tui_backend_gui.cpp) reads it via ReadAssetDefault and
# installs it as io.FontDefault, so the UI font family comes from the binary and not
# from the image (task 20, Q-06 settled as "keep Roboto"). Without the asset the chain
# falls back to a system sans and then to ImGui's built-in ProggyClean.
set(IMGUI_RICHMD_EMBED_EXTRA_FILES "fonts/Roboto/Roboto-Medium.ttf")
set(IMGUI_RICHMD_STB_IMAGE_IMPLEMENTATION ON CACHE BOOL "" FORCE)
set(IMGUI_RICHMD_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(IMGUI_RICHMD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(../vendor/imgui_rich_md ${CMAKE_BINARY_DIR}/imgui_rich_md)

find_package(Threads REQUIRED)

# GLFW — GUI backend dependency. Three-step discovery normalized to a target
# named `glfw`: package config → pkg-config (optional, may be absent) →
# find_library. The configure must fail here with an install hint when the dev
# package is missing, not at link time.
find_package(glfw3 3.0 QUIET)                 # config mode (needs glfw3Config.cmake)
if (NOT TARGET glfw)
    find_package(PkgConfig QUIET)
    if (PkgConfig_FOUND)
        pkg_check_modules(GLFW3 QUIET IMPORTED_TARGET glfw3)
        if (TARGET PkgConfig::GLFW3)
            add_library(glfw INTERFACE IMPORTED)
            # set_property, not target_link_libraries: the latter needs CMake
            # >= 3.11 for IMPORTED targets; this file declares 3.10.
            set_property(TARGET glfw PROPERTY INTERFACE_LINK_LIBRARIES PkgConfig::GLFW3)
        endif()
    endif()
endif()
if (NOT TARGET glfw)
    find_path(GLFW_INCLUDE_DIR GLFW/glfw3.h)
    find_library(GLFW_LIBRARY NAMES glfw glfw3)
    if (NOT GLFW_INCLUDE_DIR OR NOT GLFW_LIBRARY)
        message(FATAL_ERROR
            "GLFW not found. Install the GLFW dev package (Ubuntu/Debian: libglfw3-dev; "
            "EL7/EL9: glfw-devel from EPEL) and re-run cmake.")
    endif()
    add_library(glfw UNKNOWN IMPORTED)
    set_target_properties(glfw PROPERTIES IMPORTED_LOCATION "${GLFW_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${GLFW_INCLUDE_DIR}")
endif()

# Static library: implementation + C API layer
# Enables D code (and other languages) to link against the library independently
add_library(llmfun_tui_lib STATIC
    tui.cpp
    tui_backend_ncurses.cpp
    tui_backend_null.cpp
    tui_backend_gui.cpp
    tui_backend_title.cpp
    tui_settings.cpp
    tui_common.cpp
    tui_markdown_host.cpp
    session_panel.cpp
    tui_api.cpp
    ui/tokens.cpp
    ui/metrics.cpp
    ui/layout.cpp
    ui/text.cpp
    ui/icons.cpp
    ui/input.cpp
    ui/primitives.cpp
    ui/scroll.cpp
    ui/markdown.cpp
    ui/table.cpp
    model/status_chips.cpp
    model/activity.cpp
    model/toast.cpp
    model/rawlog_data.cpp
    model/thread_view.cpp
    model/keymap.cpp
    model/ui_conf.cpp
    model/session_rows.cpp
    model/ui_state.cpp
    views/blocks.cpp
    views/composer.cpp
    views/feedback.cpp
    views/help.cpp
    views/inspector.cpp
    views/nowbar.cpp
    views/palette.cpp
    views/rawlog.cpp
    views/rail.cpp
    views/settings.cpp
    views/shell.cpp
    views/thread.cpp
    views/topbar.cpp
    store/thread_store.cpp
)

target_include_directories(llmfun_tui_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# `model/ui_conf.cpp` turns each enum into the word the file stores with an
# exhaustive `switch` and no `default:` (`sendKeyWord`, `inspectorTabWord`), so a
# new enumerator must add its word or the writer silently persists its neighbour.
# The project enables no warnings and `assert()` is compiled out under NDEBUG, so
# make the invariant real for this TU: a missing case is a build failure, not a
# warning nobody reads. (The file is clean under this flag; the switch is the only
# enum dispatch in it.)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set_source_files_properties(model/ui_conf.cpp PROPERTIES
        COMPILE_OPTIONS "-Wswitch-enum;-Werror=switch-enum")
endif()

target_link_libraries(llmfun_tui_lib
PUBLIC imtui-ncurses imgui_rich_md
    PRIVATE Threads::Threads stdc++
)

# Vendored imgui GLFW/OpenGL3 backend translation units (GUI mode). They need
# the explicit imgui core + backends include dirs: the core dir is PRIVATE on
# imgui-for-imtui and the backend files include bare "imgui.h" (upstream's
# example build uses -I$(IMGUI_DIR) -I$(IMGUI_DIR)/backends). IMGUI_USE_WCHAR32
# reaches these sources through imgui-for-imtui's PUBLIC usage requirement.
set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../vendor/imgui/imgui)
target_sources(llmfun_tui_lib PRIVATE
    ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
)
# GLFW-including sources must not pull in GL headers; nothing here links an
# OpenGL library (the OpenGL3 backend dlopens the driver at run time - no -lGL).
set_source_files_properties(${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/tui_backend_gui.cpp
    PROPERTIES COMPILE_DEFINITIONS GLFW_INCLUDE_NONE)
target_include_directories(llmfun_tui_lib PRIVATE ${IMGUI_DIR} ${IMGUI_DIR}/backends)
# The GLFW usage requirements (include dir) must reach the TUs that include
# <GLFW/glfw3.h> — tui_backend_gui.cpp here, imgui_impl_glfw.cpp above —
# including when GLFW lives under a non-system prefix.
target_link_libraries(llmfun_tui_lib PRIVATE glfw)

add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/libllmfun_tui_all_lib.a
    COMMAND ${CMAKE_AR} -x $<TARGET_FILE:llmfun_tui_lib>
    COMMAND ${CMAKE_AR} -x $<TARGET_FILE:imtui-ncurses>
    COMMAND ${CMAKE_AR} -x $<TARGET_FILE:imtui>
    COMMAND ${CMAKE_AR} -x $<TARGET_FILE:imgui-for-imtui>
    COMMAND ${CMAKE_AR} -x $<TARGET_FILE:imgui_rich_md>
    COMMAND ${CMAKE_AR} rcs libllmfun_tui_all_lib.a *.o
    DEPENDS llmfun_tui_lib imtui-ncurses imtui imgui-for-imtui imgui_rich_md
)
add_custom_target(libllmfun_tui_all_lib ALL
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/libllmfun_tui_all_lib.a
)
add_library(llmfun_tui_all_lib STATIC IMPORTED)
set_target_properties(llmfun_tui_all_lib PROPERTIES
    IMPORTED_LOCATION ${CMAKE_CURRENT_BINARY_DIR}/libllmfun_tui_all_lib.a
)
add_dependencies(llmfun_tui_all_lib libllmfun_tui_all_lib)

add_executable(llmfun_tui
    main.cpp
)

add_dependencies(llmfun_tui llmfun_tui_all_lib)
# glfw + libdl: the combined archive carries the GUI backend objects.
target_link_libraries(llmfun_tui PRIVATE
    llmfun_tui_all_lib
    ncursesw
    glfw
    ${CMAKE_DL_LIBS}
)

# Headless v4 C-API contract test: none-state no-ops, the NULL-vs-None
# tuiRender asymmetry, destroy-without-init (a harness ImGui context must
# survive), a failed Gui init with a forced-empty display env (retryable, no
# leaked context, empty backend note), and the Auto/TTY matrix via forked
# children with pipe/PTY-controlled stdio. Invoke from the build dir:
#   ./test_tui_api_v4
# (the pty-auto-gui child mode expects a display; run it under xvfb-run).
add_executable(test_tui_api_v4
    test_tui_api_v4.cpp
)
add_dependencies(test_tui_api_v4 llmfun_tui_all_lib)
# imtui/imtui.h (and the IMGUI_USE_WCHAR32 definition) come from the
# interface-providing targets; their archives are already inside
# llmfun_tui_all_lib, so they contribute no extra objects.
target_link_libraries(test_tui_api_v4 PRIVATE
    llmfun_tui_all_lib
    imtui-ncurses
    ncursesw
    glfw
    ${CMAKE_DL_LIBS}
)

# Headless grid-invariant regression test for the imtui UTF-8 cell-width fix
# Links the REAL imtui + imgui-for-imtui
# (IMTUI / IMGUI_USE_WCHAR32 come from those targets, exactly like llmfun_tui);
# runs headless via ImTui_ImplText_* — no ncurses, no PTY. Built by the normal
# `make` (build_test.py gate) and executed from the build dir.
add_executable(imtui_utf8_grid_test
    ../vendor/imtui/test/test_utf8_grid.cpp
)
target_link_libraries(imtui_utf8_grid_test PRIVATE imtui)

# ncurses fold-row rewrite regression test. The binary drives the REAL ncurses backend (ImTui_ImplNcurses_DrawScreen) with a folded VS16 pair row across four frames of content changes. It must run on a PTY, so it is spawned and its raw output stream replayed by ../vendor/imtui/test/test_ncurses_fold_redraw.py (wired into build_test.py); the replayer models a VS16-clustering terminal and asserts the final row.
add_executable(imtui_arrow_text_channel_test
    ../vendor/imtui/test/test_arrow_text_channel.cpp
)
target_link_libraries(imtui_arrow_text_channel_test PRIVATE imtui)

add_executable(imtui_ncurses_fold_redraw_test
    ../vendor/imtui/test/test_ncurses_fold_redraw.cpp
)
target_link_libraries(imtui_ncurses_fold_redraw_test PRIVATE imtui-ncurses)

# Standalone unit test for the session fuzzy matcher. Links ONLY the standard library (no imtui/ncurses/ImGui), which also verifies that session_fuzzy.h compiles standalone (no TUI dependency). Built by the normal `make -f tui.mak` and run headless from the build dir.
add_executable(test_session_fuzzy
    test_session_fuzzy.cpp
)

# Chord-evidence probe for the key gate (plan/key_gate.md). Text mode drives the
# production ncurses backend on a pty with an InputTextMultiline active and
# injects each chord's terminal bytes; --gui drives the GLFW/OpenGL3 backend and
# injects at the ImGui IO boundary. Runs headless from the build dir:
#   ./probe_chords --term xterm
#   ./probe_chords --term linux
#   ./probe_chords --csiu
#   xvfb-run -s "-screen 0 1280x820x24" ./probe_chords --gui
add_executable(probe_chords
    test/probe_chords.cpp
)
add_dependencies(probe_chords llmfun_tui_all_lib)
# The probe includes "tui_backend.h" from the library's own directory (quoted
# includes from test/ do not reach the parent), plus the imtui/imgui include
# paths and IMGUI_USE_WCHAR32 that come with the interface targets linked below.
target_include_directories(probe_chords PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(probe_chords PRIVATE
    llmfun_tui_all_lib
    imtui-ncurses
    ncursesw
    glfw
    ${CMAKE_DL_LIBS}
)

# Standalone unit test for the GUI view settings (tui_settings): imgui2.conf
# path derivation, strict per-key parsing/clamping/fallbacks, save-side guards.
# Compiles the helper TU directly; links only the standard library (no
# GLFW/ImGui/display; tui_settings.cpp includes imgui.h/tui.h only outside the
# headless test build, see TUI_SETTINGS_HEADLESS_TEST below). Built by
# `make -f tui.mak`, run headless: ./test_settings
add_executable(test_settings
    test_settings.cpp
    tui_settings.cpp
)
# Keeps the ImGui-dependent renderOptionsWindow out of the test TU (see above).
target_compile_definitions(test_settings PRIVATE TUI_SETTINGS_HEADLESS_TEST)

# The workbench's own settings file (task 22, design §5.3 / D-UI-CONF): `ui.conf` is
# the sibling of `imgui2.conf` that only the workbench reads and writes, so a
# `TuiSettings` save cannot clobber a preference it has no field for. This harness pins the path
# derivation, the exact on-disk bytes, a full round-trip of every key and value, the
# `Version` gate, unknown/malformed/out-of-range handling, pin pruning, the atomic
# write (a temporary left behind by a killed writer is never read), the 2 s debounce,
# the read-only-directory warning, and that `saveTuiSettings` leaves `ui.conf`
# byte-identical.
#
# It compiles `model/ui_conf.cpp` and `tui_settings.cpp` directly and links only the
# standard library (no GLFW, no ImGui, no display, like test_settings), which also
# pins the module's layering: `model/` owns no renderer and no v4 payload.
# `tui_settings.cpp` needs TUI_SETTINGS_HEADLESS_TEST for the same reason as above.
#
# Run headless from the build dir; scratch files land under ./test_ui_settings_scratch:
#   ./test_ui_settings
add_executable(test_ui_settings
    test_ui_settings.cpp
    model/ui_conf.cpp
    tui_settings.cpp
)
target_compile_definitions(test_ui_settings PRIVATE TUI_SETTINGS_HEADLESS_TEST)

# Headless clamp test for tuiSetMaxWidth / the tuiRender clamp. Text backend only (no ncurses, no PTY) — runs a real TuiState through tuiRender and checks the grid width. Invoke: ./test_tui_maxwidth
add_executable(test_tui_maxwidth
    test_tui_maxwidth.cpp
)
add_dependencies(test_tui_maxwidth llmfun_tui_all_lib)
# tui.h (the C++ header) needs the imtui include paths and
# the IMGUI_USE_WCHAR32 definition; link the interface-providing targets for
# exactly that (their archives are already inside llmfun_tui_all_lib, so they
# contribute no extra objects).
target_link_libraries(test_tui_maxwidth PRIVATE
    llmfun_tui_all_lib
    imtui-ncurses
    ncursesw
    glfw
    ${CMAKE_DL_LIBS}
)

# Headless rich-markdown text-mode smoke test for the imgui_rich_md
# migration. Drives a real TuiState through tuiRender on the imtui TEXT
# backend (no ncurses, no PTY) with the real text-mode markdown profile
# (initMarkdownTextMode BEFORE the backend init, the 1-cell warm-up bakes)
# and pins the reference document's rows in order (chrome-agnostic scan),
# the wide/emoji row byte-exactly, the table/image/mermaid outcomes, and a
# clean stderr. Tripwire: fails if the warm-up recipe or the LLMFUN imgui
# measurement patch is reverted. It also pins the rich_md seam (ui/markdown.*):
# RenderRaw only (a named transclusion file must never be read), the stable
# block-id scope, the token theme in RichMd::Style, the per-profile selection
# policy, and that store/, views/ and model/ name no RichMd symbol.
# Invoke:
# ./test_rich_markdown_text < /dev/null
add_executable(test_rich_markdown_text
    test_rich_markdown_text.cpp
)
add_dependencies(test_rich_markdown_text llmfun_tui_all_lib)
# The source tree the "ui/markdown.* is the only rich_md seam" scan reads: a
# compile-time path (same seam as TEST_PRIMITIVES_SOURCE in test_primitives)
# rather than a CWD guess.
target_compile_definitions(test_rich_markdown_text PRIVATE
    TEST_RICH_MARKDOWN_SRC_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)
# tui.h (the C++ header) needs the imtui include paths and the
# IMGUI_USE_WCHAR32 definition; link the interface-providing targets for
# exactly that (their archives are already inside llmfun_tui_all_lib, so they
# contribute no extra objects).
# ui/markdown.h is the seam, and the theme case reads RichMd::Style back to prove
# the token bridge reached the library instead of only an ImGuiCol_* slot. Header
# use only: imgui_rich_md's objects are already inside llmfun_tui_all_lib, so the
# path is added here and nothing is linked twice.
target_include_directories(test_rich_markdown_text PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../vendor/imgui_rich_md
    ${CMAKE_CURRENT_SOURCE_DIR}/../vendor/imgui/imgui
)
target_link_libraries(test_rich_markdown_text PRIVATE
    llmfun_tui_all_lib
    imtui-ncurses
    ncursesw
    glfw
    ${CMAKE_DL_LIBS}
)

# Headless regression test for the widened clear contract: tuiClearOutput / C-API tuiClearChatMessages must reset ALL derived chat-display state — output lines, the open-group set (collapsed headers; stale positions suppressed header labels of replayed messages) and the in-flight stream message — not just outputLines. Pure state-level assertions; no terminal, no ImGui context, no rendering needed. Invoke:
# ./test_clear_output_state
add_executable(test_clear_output_state
    test_clear_output_state.cpp
)
add_dependencies(test_clear_output_state llmfun_tui_all_lib)
# tui.h (the C++ header) needs the imtui include paths and
# the IMGUI_USE_WCHAR32 definition; link the interface-providing targets for
# exactly that (their archives are already inside llmfun_tui_all_lib, so they
# contribute no extra objects).
target_link_libraries(test_clear_output_state PRIVATE
    llmfun_tui_all_lib
    imtui-ncurses
    ncursesw
    glfw
    ${CMAKE_DL_LIBS}
)


# Headless contrast / neutral-surface test for the thread theme (task 5): every
# text/background pair the thread draws reaches 4.5:1 in both profiles, no ImGui
# colour slot carries a role colour, and the canvas/surface pair stays neutral.
# Links llmfun_tui_all_lib so the binary sees the real applyTheme() and the real
# ChatMessageStyle defaults; runs headless (no ncurses, no PTY, no display):
#   ./test_theme_contrast [-v]
add_executable(test_theme_contrast
    test_theme_contrast.cpp
)
add_dependencies(test_theme_contrast llmfun_tui_all_lib)
# test_theme_contrast also measures the session rail's row inks (task 23), so it
# includes views/rail.h - whose quoted includes resolve against the cpp_tui root.
target_include_directories(test_theme_contrast PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
# tui.h (the C++ header) needs the imtui include paths and
# the IMGUI_USE_WCHAR32 definition; link the interface-providing targets for
# exactly that (their archives are already inside llmfun_tui_all_lib, so they
# contribute no extra objects).
target_link_libraries(test_theme_contrast PRIVATE
    llmfun_tui_all_lib
    imtui-ncurses
    ncursesw
    glfw
    ${CMAKE_DL_LIBS}
)

# Headless unit test for the status-chip splitter (task 6): D's one-line status
# sentence (formatStatusText, app_agent/ui.d:421-425) -> the four chips the status
# row shows, with "—" for a field the sentence does not carry. Compiles
# model/status_chips.cpp directly and links nothing else (like test_settings),
# which also pins the module's layering: no ImGui, no tui.h, no ChatMessage.
# Invoke from the build dir: ./test_status_chips
add_executable(test_status_chips
    test_status_chips.cpp
    model/status_chips.cpp
)

# Headless unit test for the thread-state derivation (task 8, design §4.11 /
# FR-19): the Ready/Empty/Loading/Offline precedence table, the starter-action
# table and the verbatim copy. Compiles model/thread_view.cpp directly and links
# nothing else (like test_status_chips), which pins the module's layering: no
# ImGui, no tui.h, no ChatMessage. Invoke from the build dir: ./test_thread_view
add_executable(test_thread_view
    test_thread_view.cpp
    model/thread_view.cpp
)

# GUI golden screens for the three thread states (task 8): drives the real GUI
# backend through tuiRender and reads each frame back through
# Backend::captureNextFramePpm into a P6 PPM (design 4.11 / FR-19; the pixel
# profile has no external screenshot tool in the test image). Needs a display:
#   xvfb-run -s "-screen 0 1280x820x24" ./test_thread_states_gui --out DIR
add_executable(test_thread_states_gui
    test_thread_states_gui.cpp
)
add_dependencies(test_thread_states_gui llmfun_tui_all_lib)
# tui.h (the C++ header) needs the imtui include paths and
# the IMGUI_USE_WCHAR32 definition; link the interface-providing targets for
# exactly that (their archives are already inside llmfun_tui_all_lib, so they
# contribute no extra objects).
target_link_libraries(test_thread_states_gui PRIVATE
    llmfun_tui_all_lib
    imtui-ncurses
    ncursesw
    glfw
    ${CMAKE_DL_LIBS}
)

# The top bar in the pixel profile (task 22a): a type step is absolute, so the bar measures the same
# numbers it draws, and the wordmark and palette hint are drawn in full instead of being clipped by
# boxes measured 15 % narrow. The text grid is blind to this (a TypeScope is a no-op there). Needs a
# display:
#   xvfb-run -s "-screen 0 1280x820x24" ./test_topbar_gui [--out DIR]
add_executable(test_topbar_gui
    test_topbar_gui.cpp
)
add_dependencies(test_topbar_gui llmfun_tui_all_lib)
# test_topbar_gui includes views/shell.h, views/topbar.h and tui.h, which reach model/ and ui/
# headers: the cpp_tui root has to be on the path (a quoted include only resolves next to its file).
target_include_directories(test_topbar_gui PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
# tui.h (the C++ header) needs the imtui include paths and
# the IMGUI_USE_WCHAR32 definition; link the interface-providing targets for
# exactly that (their archives are already inside llmfun_tui_all_lib, so they
# contribute no extra objects).
target_link_libraries(test_topbar_gui PRIVATE
    llmfun_tui_all_lib
    imtui-ncurses
    ncursesw
    glfw
    ${CMAKE_DL_LIBS}
)

# The feedback stack in the pixel profile (task 45, design §5.25 / FR-29): three items raised the
# way a copy, a save and a failed save raise them, drawn through the real GUI backend, captured to
# a PPM and read back - each card a `raised` surface at the rect the stack geometry answered, each
# card's 4 px rule in its severity's colour, and the gap between two cards not a card. The grid's
# presentation (the now bar's one-line flash) is `./test_nowbar`'s. Needs a display:
#   xvfb-run -s "-screen 0 1280x820x24" ./test_toast_gui [--out DIR]
add_executable(test_toast_gui
    test_toast_gui.cpp
)
add_dependencies(test_toast_gui llmfun_tui_all_lib)
# It includes views/shell.h, views/feedback.h and tui.h, which reach model/ and ui/ headers: the
# cpp_tui root has to be on the path.
target_include_directories(test_toast_gui PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_toast_gui PRIVATE
    llmfun_tui_all_lib
    imtui-ncurses
    ncursesw
    glfw
    ${CMAKE_DL_LIBS}
)

# GUI window defaults and chrome metrics (task 9, D-GEOM): a fresh profile opens
# at 1280x820, a persisted size wins, the 640x480 floor is applied to both the
# startup size and the window system, the chrome metrics replace ImGui's stock
# spacing/rounding in the pixel profile, and the text grid keeps its own cell
# metrics. Window and framebuffer sizes come from the backend's own capture
# (Task 8's PPM seam), not from a shared constant. Needs a display:
#   xvfb-run -s "-screen 0 1280x820x24" ./test_gui_window --out DIR
add_executable(test_gui_window
    test_gui_window.cpp
)
add_dependencies(test_gui_window llmfun_tui_all_lib)
# tui.h (the C++ header) needs the imtui include paths and
# the IMGUI_USE_WCHAR32 definition; link the interface-providing targets for
# exactly that (their archives are already inside llmfun_tui_all_lib, so they
# contribute no extra objects).
target_link_libraries(test_gui_window PRIVATE
    llmfun_tui_all_lib
    imtui-ncurses
    ncursesw
    glfw
    ${CMAKE_DL_LIBS}
)

# Palette overlay (task 10, §4.12): Ctrl+K opens it, Esc closes it, the overlay
# never moves the regions under it, filtering re-uses session_fuzzy.h, every row
# does what it says, and the result list draws viewport + one row of overscan (5
# rows at 80x24, 12 in the pixel profile). The default mode is the headless text
# grid; `--gui` drives the real GUI backend and writes the three PPMs. Needs a
# display only in that mode:
#   ./test_palette
#   xvfb-run -s "-screen 0 1280x820x24" ./test_palette --gui --out DIR
add_executable(test_palette
    test_palette.cpp
)
add_dependencies(test_palette llmfun_tui_all_lib)
# test_palette includes ui/layout.h (for the golden's overlay box) and views/palette.h, which reach
# ui/ and model/ headers: the cpp_tui root has to be on the path (a quoted include only resolves
# next to its file).
target_include_directories(test_palette PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(test_palette PRIVATE
    TEST_PALETTE_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/test/golden"
)
target_link_libraries(test_palette PRIVATE
    llmfun_tui_all_lib
    imtui-ncurses
    ncursesw
    glfw
    ${CMAKE_DL_LIBS}
)

# Keymap registry and key hygiene (task 11, D-KEYMAP / D-ESC): the table is
# collision-free per (chord, focus), every action is reachable, plain keys stay
# inert while a text field has the keyboard, `Ctrl+Q` quits (with the two-press
# confirmation while a run is active and one hint line in the UI log), `Ctrl+K`
# still toggles the palette through the registry, and `Esc` walks the documented
# chain overlay -> focus mode -> cancel -> clear -> blur one step per press (the
# focus-mode step is task 43, whose interaction scene also asks the rail's
# `Esc`-ownership predicate - hence the source dir on the include path).
# The pure registry checks use a test-local table, so the scoping rules are
# pinned independently of what the shipped table happens to bind.
# Invoke from the build dir: ./test_keymap
add_executable(test_keymap
    test_keymap.cpp
)
add_dependencies(test_keymap llmfun_tui_all_lib)
target_include_directories(test_keymap PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_keymap PRIVATE
    llmfun_tui_all_lib
    imtui-ncurses
    ncursesw
    glfw
    ${CMAKE_DL_LIBS}
)

# Token module and theme application (task 12, D-THEME / NFR-09): the generated
# palette is well formed (real xterm-256 entries, in-range ANSI-16 indices), every
# text/background pair the theme can draw reaches 4.5:1 and every essential
# boundary 3:1 at the depth that renders it, `text.dim` never reaches a
# text-bearing slot, applyTheme walks its binding table at every depth, the depth
# detection order holds against an injected environment, the size scales are the
# documented ones, a five-frame loop does not re-apply the theme, and the GUI type
# scale is *applied* (the body step is the pixel profile's base size, a type scope
# moves the size inside a frame and does nothing on the text grid) with no view
# using more than three steps. Links llmfun_tui_all_lib because the frame-count
# half drives the real tuiRender on the headless text grid. Invoke from the build
# dir: ./test_tokens [-v]
add_executable(test_tokens
    test_tokens.cpp
)
add_dependencies(test_tokens llmfun_tui_all_lib)
# The directory the "≤3 type sizes per view" scan reads. Same seam as
# TEST_LAYOUT_VIEWS_DIR in test_layout: the scan is the point, so the path is a
# compile-time definition rather than a CWD guess.
target_compile_definitions(test_tokens PRIVATE
    TEST_TOKENS_VIEWS_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/views\"
)
target_link_libraries(test_tokens PRIVATE
    llmfun_tui_all_lib
    imtui-ncurses
    ncursesw
    glfw
    ${CMAKE_DL_LIBS}
)

# Metrics module and the gate ratchet (task 13, D-METRICS / design 3.4): one
# Metrics struct owns every length in cells and rows (halves round down, the text
# grid stays cell-exact, the GUI scale is applied once), the live-profile
# accessor agrees with the backend that is up, and tools/gate_ratchet.txt states
# exactly which files still carry a v4 gate - tuiIsTextGrid, a hand-made ImVec4 or
# a v4 payload type. A gate in an unlisted file, or a row that changed, fails the
# run. Links llmfun_tui_all_lib because the live-profile half initializes the real
# ImTui text backend. Invoke from the build dir: ./test_metrics [-v]
add_executable(test_metrics
    test_metrics.cpp
)
add_dependencies(test_metrics llmfun_tui_all_lib)
target_link_libraries(test_metrics PRIVATE
    llmfun_tui_all_lib
    imtui-ncurses
    ncursesw
    glfw
    ${CMAKE_DL_LIBS}
)

# Text and width utilities (task 14, design 4.15): one UTF-8-safe module for cell
# width, truncation with an ellipsis, word wrapping and compact numerals. Measures
# in terminal cells - wide glyphs 2, combining marks 0 - with the same table the
# ncurses backend renders with. Compiles ui/text.cpp + ui/icons.cpp directly and
# links nothing else (like test_settings/test_thread_view), which pins both modules'
# layering: no ImGui, no tui.h, no v4 payload. The icons TU is here because
# truncate()'s default marker is ui::icon(Icon::Ellipsis); the binary pins
# LLMFUN_ASCII=0 at start-up so the default marker is deterministic.
# Invoke from the build dir: ./test_text [-v]
add_executable(test_text
    test_text.cpp
    ui/text.cpp
    ui/icons.cpp
)

# The glyph table (task 15, design 4.15 + plan/appendix_modern_ui.md 4.8/4.9): one
# character per state mark, each with a single-cell Unicode form and a plain ASCII
# fallback. Every glyph is one cell and one codepoint with no emoji, every fallback
# is printable ASCII, the detection order is pinned against an injected environment,
# the whole table is rendered as a legend through the real ImTui text backend at
# 80x24 and 40x15 in both modes - cell for cell, so a "?" substitution or a
# one-column drift fails the run - and the GUI's answer (pixelGlyphSet) is checked
# against what the shipped font atlases actually carry, so the pixel profile cannot
# silently draw marks its font cannot. Compiles ui/icons.cpp and ui/text.cpp directly
# (the module has no renderer dependency); links the app library for the legend half
# only. Invoke from the build dir:
#   ./test_icons [-v] [--dump DIR]
add_executable(test_icons
    test_icons.cpp
    ui/icons.cpp
    ui/text.cpp
)
add_dependencies(test_icons llmfun_tui_all_lib)
target_include_directories(test_icons PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
# The vendor fonts, so the atlas half can measure what the design's UI font carries
# (Roboto-Medium) as well as the embedded bitmap font the GUI ships by default.
target_compile_definitions(test_icons PRIVATE
    TEST_ICONS_VENDOR_FONTS="${CMAKE_CURRENT_SOURCE_DIR}/../vendor/imgui/imgui/misc/fonts"
)
target_link_libraries(test_icons PRIVATE
    llmfun_tui_all_lib
    imtui-ncurses
    ncursesw
    glfw
    ${CMAKE_DL_LIBS}
)

# The primitive library (task 16, design §4.15 / plan §8.6): the widget vocabulary the
# shell draws with, each primitive with a measure() its renderer draws from.
#
# The test compiles the `ui/` layer itself (primitives, text, icons, metrics, tokens) and
# links only the vendored imtui/imgui targets - no app library at all. Two things follow:
# the module is built the way a consumer builds it (no tui.h, no view header, no v4 payload
# can creep in unnoticed), and no object is compiled into two linked targets, which would
# otherwise become a "multiple definition" the first time a module test also links the app
# library that already contains that object.
#
# The two definitions hand it the shipped source (a colour literal in the module is a test
# failure) and the golden directory (one 100x30 screen per primitive). Rewrite the goldens
# with `./test_primitives --update` after an intentional rendering change.
add_executable(test_primitives
    test_primitives.cpp
    ui/primitives.cpp
    ui/text.cpp
    ui/icons.cpp
    ui/metrics.cpp
    ui/tokens.cpp
)
target_include_directories(test_primitives PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(test_primitives PRIVATE
    TEST_PRIMITIVES_SOURCE="${CMAKE_CURRENT_SOURCE_DIR}/ui/primitives.cpp"
    TEST_PRIMITIVES_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/test/golden"
)
target_link_libraries(test_primitives PRIVATE
    imtui-ncurses
    ncursesw
    ${CMAKE_DL_LIBS}
)

# The feedback queue and the toast primitive (task 45, design §5.25 / FR-29): the two lifetimes
# per profile, the queue's rules (one item per press, the profile's depth, a transient never
# evicts an unacknowledged warning, expiry on the injected clock, dismissal oldest-first), the
# card's measure in both profiles, and the card's drawing/controls read back from the real text
# backend - plus the stack's geometry (`views/feedback.cpp`), which is pure and needs no window.
#
# Like test_primitives it compiles the modules it tests and links only the vendored imtui/imgui
# targets - no app library, so no tui.h and no v4 payload can creep in.
#
# Invoke from the build dir: ./test_toast [-v]
add_executable(test_toast
    test_toast.cpp
    model/toast.cpp
    views/feedback.cpp
    ui/primitives.cpp
    ui/text.cpp
    ui/icons.cpp
    ui/metrics.cpp
    ui/tokens.cpp
)
target_include_directories(test_toast PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_toast PRIVATE
    imtui-ncurses
    ncursesw
    ${CMAKE_DL_LIBS}
)

# The text-grid table renderer (task 36, design §4.14 / D-TABLE / FR-20): GFM tables on a
# 1-cell grid where rich_md's own renderer is unsafe (C-05). The test pins the parser (header,
# alignment, escaped pipes, opaque inline code), the fence-aware detection, the documented
# width arithmetic (chrome identity, minimums, cap, CJK measured in cells), the vertical
# key-value fallback - and then renders tables through the real text backend to pin the
# drawing cell by cell: rules on the exact cells, text inside the box, alignment offsets, no
# filled cell, ASCII borders, clipping.
#
# Like test_primitives it compiles the `ui/` layer itself and links only the vendored
# imtui/imgui targets - no app library, so no tui.h, no view header and no v4 payload can
# creep in. The two definitions hand it the shipped source (a colour literal or a profile
# branch in the module is a test failure) and the golden directory. One screen per reference
# size: 103x82, 110x30, 120x40, plus an ASCII variant and a key-value one. Rewrite them with
# `./test_table --update` after a deliberate rendering change.
#
# Invoke from the build dir: ./test_table [-v] [--update]
add_executable(test_table
    test_table.cpp
    ui/table.cpp
    ui/text.cpp
    ui/icons.cpp
    ui/metrics.cpp
    ui/tokens.cpp
)
target_include_directories(test_table PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(test_table PRIVATE
    TEST_TABLE_SOURCE="${CMAKE_CURRENT_SOURCE_DIR}/ui/table.cpp"
    TEST_TABLE_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/test/golden"
)
target_link_libraries(test_table PRIVATE
    imtui-ncurses
    ncursesw
    ${CMAKE_DL_LIBS}
)

# The table bridge in the thread (task 37, design section 6.8, D-TABLE). The bridge's pure
# half (`ui::bridgeTables`: detection, the fence text, the source language, idempotence) is
# pinned by ./test_table without a frame; this harness pins the *rendering*: a message body's
# table drawn as a grid by the text-grid renderer, a body without a table unchanged, and a
# table inside a code fence
# left alone, the `s` source toggle, an unparseable fence that draws its source, and three
# goldens. It also scans the seam sources for what must *not* be there (a child window on the
# table path) and for the one profile question the bridge asks.
#
# Like test_rich_markdown_text it drives the real `tuiRender()` on the imtui TEXT backend with
# the real text-mode markdown profile (initMarkdownTextMode before ImTui_ImplText_Init), so a
# golden is what the terminal would show. The source paths are compile-time definitions, not
# CWD guesses. Invoke from the build dir: ./test_thread_table [-v] [--update] [--dump]
add_executable(test_thread_table
    test_thread_table.cpp
)
add_dependencies(test_thread_table llmfun_tui_all_lib)
target_compile_definitions(test_thread_table PRIVATE
    TEST_THREAD_TABLE_MARKDOWN_SOURCE="${CMAKE_CURRENT_SOURCE_DIR}/ui/markdown.cpp"
    TEST_THREAD_TABLE_STORE_SOURCE="${CMAKE_CURRENT_SOURCE_DIR}/store/thread_store.cpp"
    TEST_THREAD_TABLE_UI_SOURCE="${CMAKE_CURRENT_SOURCE_DIR}/ui/table.cpp"
    TEST_THREAD_TABLE_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/test/golden"
)
# tui.h needs the imtui include paths and IMGUI_USE_WCHAR32; the archives are already inside
# llmfun_tui_all_lib, so the interface targets contribute include paths only.
target_include_directories(test_thread_table PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../vendor/imgui/imgui
)
target_link_libraries(test_thread_table PRIVATE
    llmfun_tui_all_lib
    imtui-ncurses
    ncursesw
    glfw
    ${CMAKE_DL_LIBS}
)

# The layout engine (task 17, design §3.3, D-METRICS / D-GEOM / D-OVERLAY): one
# function turns a display size into the six tiled regions, the overlays and the
# compact-mode decision, so no view positions itself. This harness is a *property*
# test: every invariant (tiling, no overlap, minimum content, overlay boxes and
# z-order, the unit stamp) is checked at the documented reference sizes and then
# swept over thousands of grid and pixel sizes, including the Raw Log drawer.
#
# It compiles the ui/ layer itself and links only the vendored imtui/imgui targets
# (like test_primitives): no app library, so no tui.h, no view header and no v4
# payload can creep into the geometry unnoticed. `TEST_LAYOUT_VIEWS_DIR` is the
# directory the "no view reads the display size" check scans.
#
# Invoke from the build dir: ./test_layout [-v]
add_executable(test_layout
    test_layout.cpp
    ui/layout.cpp
    ui/metrics.cpp
    ui/tokens.cpp
)
target_include_directories(test_layout PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(test_layout PRIVATE
    TEST_LAYOUT_VIEWS_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/views\"
)
target_link_libraries(test_layout PRIVATE
    imtui-ncurses
    ncursesw
    ${CMAKE_DL_LIBS}
)

# The thread's bounded window (task 18, D-BOUNDS): which groups the thread draws and
# how many one press of `Load earlier` reveals. `threadWindow()`/`loadEarlierPage()`
# are pure - integers in, a struct out - so this harness needs no ImGui context, no
# renderer and no terminal; it links the app library only because the two functions
# live in `views/thread.cpp` next to the drawing code they feed. Invoke from the
# build dir: ./test_thread_window [-v]
add_executable(test_thread_window
    test_thread_window.cpp
)
add_dependencies(test_thread_window llmfun_tui_all_lib)
target_link_libraries(test_thread_window PRIVATE
    llmfun_tui_all_lib
    imtui-ncurses
    ncursesw
    glfw
    ${CMAKE_DL_LIBS}
)

# The frame's layout contract: the overlay layer draws on the tiling the shell
# painted, so a chord that changes a tiling preference after the shell has run cannot
# put the palette inside a thread rect the screen does not show. `layoutPrefsFor()`,
# `shellLayoutFor()` and `overlayLayoutFor()` are pure (the metrics come from the
# live profile, which is the pixel one in a harness), so this needs no ImGui context,
# no window and no terminal. Invoke from the build dir: ./test_shell_frame [-v]
add_executable(test_shell_frame
    test_shell_frame.cpp
)
add_dependencies(test_shell_frame llmfun_tui_all_lib)
# views/shell.h includes model/ and ui/ headers (and therefore needs the cpp_tui
# root on the path; a quoted include only resolves next to the including file).
target_include_directories(test_shell_frame PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_shell_frame PRIVATE
    llmfun_tui_all_lib
    imtui-ncurses
    ncursesw
    glfw
    ${CMAKE_DL_LIBS}
)

# The generated keymap document (task 41): `doc/ui_keys.md` has to match the binding registry
# (`model::uiKeysMarkdown()`), so a binding that changes without regenerating the document fails
# here instead of shipping a key table the build does not honour. The generator itself is
# `gen_ui_keys` (same text, printed). Headless: no context, no window, no terminal.
# Invoke from the build dir: ./test_ui_keys (or tools/gen_ui_keys.sh to regenerate).
add_executable(test_ui_keys
    test_ui_keys.cpp
    model/keymap_doc.cpp
    model/keymap.cpp
)
target_compile_definitions(test_ui_keys PRIVATE
    TEST_UI_KEYS_DOC="${CMAKE_CURRENT_SOURCE_DIR}/../doc/ui_keys.md")
target_include_directories(test_ui_keys PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_ui_keys PRIVATE
    imtui-ncurses
    ncursesw
    glfw
    ${CMAKE_DL_LIBS}
)

# The generator: `./gen_ui_keys > doc/ui_keys.md` (tools/gen_ui_keys.sh wraps it).
add_executable(gen_ui_keys
    tools/gen_ui_keys.cpp
    model/keymap_doc.cpp
    model/keymap.cpp
)
target_include_directories(gen_ui_keys PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(gen_ui_keys PRIVATE
    imtui-ncurses
    ncursesw
    glfw
    ${CMAKE_DL_LIBS}
)

# The top bar (task 22a, FR-02 / design §4.2): five states, the one degradation (the title
# truncates, the mark and the run pill survive), the inline rename seen through the real text
# backend, the window-title seam, and the shell that derives the bar's inputs from the
# aggregate and applies its answers. One 100x30 golden per state plus a narrow 60x20 one;
# rewrite them with `./test_topbar --update` after a deliberate rendering change.
# Invoke from the build dir: ./test_topbar [-v] [--update]
add_executable(test_topbar
    test_topbar.cpp
)
add_dependencies(test_topbar llmfun_tui_all_lib)
# test_topbar includes views/shell.h and tui.h, which reach model/ and ui/ headers: the
# cpp_tui root has to be on the path (a quoted include only resolves next to its file).
target_include_directories(test_topbar PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(test_topbar PRIVATE
    TEST_TOPBAR_SOURCE="${CMAKE_CURRENT_SOURCE_DIR}/views/topbar.cpp"
    TEST_TOPBAR_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/test/golden"
)
target_link_libraries(test_topbar PRIVATE
    llmfun_tui_all_lib
    imtui-ncurses
    ncursesw
    glfw
    ${CMAKE_DL_LIBS}
)

# The block renderer (task 26, design §4.5 / D-BLOCK): one v4 message drawn as a
# block - the role rule and label, the work group's quiet disclosure, the two-line tool
# row with its state glyph, the thinking disclosure, the code block's copy path - the
# focus ring and the copy control, the no-emoji rule measured on a real thread frame, and
# one 100x30 golden per message type plus an ASCII and a 16-colour variant. Rewrite them
# with `./test_blocks --update` after a deliberate rendering change.
# Invoke from the build dir: ./test_blocks [-v] [--update]
add_executable(test_blocks
    test_blocks.cpp
)
add_dependencies(test_blocks llmfun_tui_all_lib)
# test_blocks includes tui.h, views/thread.h and views/blocks.h, which reach model/ and
# ui/ headers: the cpp_tui root has to be on the path (a quoted include only resolves next
# to its file).
target_include_directories(test_blocks PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(test_blocks PRIVATE
    TEST_BLOCKS_SOURCE="${CMAKE_CURRENT_SOURCE_DIR}/views/blocks.cpp"
    TEST_BLOCKS_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/test/golden"
)
target_link_libraries(test_blocks PRIVATE
    llmfun_tui_all_lib
    imtui-ncurses
    ncursesw
    glfw
    ${CMAKE_DL_LIBS}
)

# Streaming v2 (task 38, design FR-08 / §4.5). The live answer's clock, its caret, the store's
# update throttle and the stable block identity, on a fake clock where the arithmetic is exact:
# one streaming frame against NFR-02's 8 ms, the `Load earlier` window that does not budget for
# the live answer, and four goldens (the caret, a cancelled answer, a reasoning-only stream and a
# stream whose last line is a table row).
#
# Like test_blocks it draws `views::renderThread()` into the real text backend, with the
# text-mode markdown profile armed (initMarkdownTextMode before ImTui_ImplText_Init), so a
# golden is what the terminal would show. The markdown-heavy answer it streams is a file
# (`test/fixtures/streaming.md`), fed one growing prefix per update.
# Invoke from the build dir: ./test_streaming [-v] [--update] [--dump]
add_executable(test_streaming
    test_streaming.cpp
)
add_dependencies(test_streaming llmfun_tui_all_lib)
# test_streaming includes tui.h, views/*.h and model/*.h, whose quoted includes resolve against
# the cpp_tui root (not against the test file's own directory).
target_include_directories(test_streaming PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(test_streaming PRIVATE
    TEST_STREAMING_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    TEST_STREAMING_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/test/fixtures/streaming.md"
    TEST_STREAMING_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/test/golden"
)
target_link_libraries(test_streaming PRIVATE
    llmfun_tui_all_lib
    imtui-ncurses
    ncursesw
    glfw
    ${CMAKE_DL_LIBS}
)

# The session rail (task 23, FR-03 / design §4.3 / D-RAIL / D-UNREAD): the list model
# (grouping across a day and a week boundary with an injected clock, the flat list an
# unknown timestamp forces, the ranked filter), the unread rule, the pure view rules
# (the 30-row/600-px threshold, the relative time, the visible window), the row inks'
# contrast, the drawn rail through the real text backend, the bounded rendering of a
# 2000-session fixture (with its frame budget), and the interaction (click/`m`/`r`/`d`/
# `p`/`/`, Enter and Esc). The binary pins TZ=UTC before it starts, so "today" is a
# fixed civil day. Invoke from the build dir: ./test_rail [-v]
add_executable(test_rail
    test_rail.cpp
)
add_dependencies(test_rail llmfun_tui_all_lib)
# test_rail includes views/rail.h and model/ headers, whose quoted includes resolve
# against the cpp_tui root (not against the test file's own directory).
target_include_directories(test_rail PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_rail PRIVATE
    llmfun_tui_all_lib
    imtui-ncurses
    ncursesw
    glfw
    ${CMAKE_DL_LIBS}
)

# The thread anchor and the Load earlier keys (task 25) through the real GUI backend.
# Needs a display: xvfb-run -s "-screen 0 1280x820x24" ./test_thread_anchor [--out DIR]
add_executable(test_thread_anchor
    test_thread_anchor.cpp
)
add_dependencies(test_thread_anchor llmfun_tui_all_lib)
target_include_directories(test_thread_anchor PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_thread_anchor PRIVATE
    llmfun_tui_all_lib
    imtui-ncurses
    ncursesw
    glfw
    ${CMAKE_DL_LIBS}
)

# The rail in the pixel profile (task 23): the same view through the real GLFW/OpenGL3
# backend at the design's 1280x820 window - the two-line threshold, the docked column,
# a frame that queues no session action, and a screenshot of that frame. Needs a
# display: xvfb-run -s "-screen 0 1280x820x24" ./test_rail_gui [--out DIR]
add_executable(test_rail_gui
    test_rail_gui.cpp
)
add_dependencies(test_rail_gui llmfun_tui_all_lib)
target_include_directories(test_rail_gui PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_rail_gui PRIVATE
    llmfun_tui_all_lib
    imtui-ncurses
    ncursesw
    glfw
    ${CMAKE_DL_LIBS}
)

# The composer (task 27, design §4.6 / FR-09 / D-PASTE): the paste policy (CRLF fold, the
# 5000-character question), the send rule (the action button sends; no key does while the
# key gate is closed), the submit handshake, history with draft preservation, auto-grow
# against `computeShell` at every supported size, the mini meter's thresholds and the
# chip/meter number agreement, the view drawn into a cell grid (empty/draft/busy/offline/
# paste-question), the interactions (paste bursts, the >5000 question, Esc clear/blur/
# cancel, `↑`/`↓`), the shell wiring, and one 100x30 + 40x15 golden per state. Rewrite
# them with `./test_composer --update` after a deliberate rendering change.
# Invoke from the build dir: ./test_composer [-v] [--update] [--print]
add_executable(test_composer
    test_composer.cpp
)
add_dependencies(test_composer llmfun_tui_all_lib)
# test_composer includes tui.h and views/shell.h, which reach model/ and ui/ headers: the
# cpp_tui root has to be on the path (a quoted include only resolves next to its file).
target_include_directories(test_composer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(test_composer PRIVATE
    TEST_COMPOSER_SOURCE="${CMAKE_CURRENT_SOURCE_DIR}/views/composer.cpp"
    TEST_COMPOSER_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/test/golden"
)
target_link_libraries(test_composer PRIVATE
    llmfun_tui_all_lib
    imtui-ncurses
    ncursesw
    glfw
    ${CMAKE_DL_LIBS}
)
# The now bar (task 28, design §4.7 / FR-11 / D-PHASE / D-FOLLOW): the seven phases and
# their words, the ordinal round-trip and the clamping, the structured/split chip identity,
# the degradation ladder (hints, then detail, then chips - never the state), `Esc stop`
# exactly while the Esc cancel step is open, the "there is more below" mark, the compact
# `RAW` badge, the state rule's contrast, the row through the real text backend, the shell
# derivation, and idle/running/offline goldens at 100x30 and 60x20. Rewrite them with
# `./test_nowbar --update` after a deliberate rendering change.
# Invoke from the build dir: ./test_nowbar [-v] [--update] [--print]
add_executable(test_nowbar
    test_nowbar.cpp
)
add_dependencies(test_nowbar llmfun_tui_all_lib)
# test_nowbar includes tui.h and views/shell.h, which reach model/ and ui/ headers: the
# cpp_tui root has to be on the path (a quoted include only resolves next to its file).
target_include_directories(test_nowbar PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(test_nowbar PRIVATE
    TEST_NOWBAR_SOURCE="${CMAKE_CURRENT_SOURCE_DIR}/views/nowbar.cpp"
    TEST_NOWBAR_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/test/golden"
)
target_link_libraries(test_nowbar PRIVATE
    llmfun_tui_all_lib
    imtui-ncurses
    ncursesw
    glfw
    ${CMAKE_DL_LIBS}
)

# The inspector panel (task 29, design §4.8 / D-INSPECTOR): the two tabs and their chords,
# the reserved digits, the `-1 auto` width rule against an explicit open, the panel's
# rectangle through `ui::computeShell()`, the frame's derivation (the open state, the floating
# shape, the tab, the unread badge), the Esc step that closes it, the `ui.conf` round trip of
# `InspectorOpen`/`InspectorTab`, the bounded body and the empty states, plus the 100x30 and
# 80x24 goldens. Rewrite them with `./test_inspector --update` after a deliberate rendering
# change. Invoke from the build dir: ./test_inspector [-v] [--update] [--print]
add_executable(test_inspector
    test_inspector.cpp
)
add_dependencies(test_inspector llmfun_tui_all_lib)
# test_inspector includes tui.h and views/shell.h, which reach model/ and ui/ headers: the
# cpp_tui root has to be on the path (a quoted include only resolves next to its file).
target_include_directories(test_inspector PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(test_inspector PRIVATE
    TEST_INSPECTOR_SOURCE="${CMAKE_CURRENT_SOURCE_DIR}/views/inspector.cpp"
    TEST_INSPECTOR_FRAME_SOURCE="${CMAKE_CURRENT_SOURCE_DIR}/tui.cpp"
    TEST_INSPECTOR_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/test/golden"
)
target_link_libraries(test_inspector PRIVATE
    llmfun_tui_all_lib
    imtui-ncurses
    ncursesw
    glfw
    ${CMAKE_DL_LIBS}
)

# The Activity tab (task 31, design §4.8 / FR-15 / D-ACTIVITY): the record projection (no
# module, function, file, line or stack field survives it), the promotion rule, the 500-row
# FIFO ring, the filter and the search, the quiet-by-default list with its unread badge, and
# the three goldens (empty, mixed levels, filtered). Rewrite them with
# `./test_activity --update` after a deliberate rendering change.
# Invoke from the build dir: ./test_activity [-v] [--update] [--print]
add_executable(test_activity
    test_activity.cpp
)
add_dependencies(test_activity llmfun_tui_all_lib)
# test_activity includes tui.h and views/shell.h, which reach model/ and ui/ headers: the
# cpp_tui root has to be on the path (a quoted include only resolves next to its file).
target_include_directories(test_activity PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(test_activity PRIVATE
    TEST_ACTIVITY_SOURCE="${CMAKE_CURRENT_SOURCE_DIR}/views/inspector.cpp"
    TEST_ACTIVITY_MODEL_SOURCE="${CMAKE_CURRENT_SOURCE_DIR}/model/activity.cpp"
    TEST_ACTIVITY_HEADER_SOURCE="${CMAKE_CURRENT_SOURCE_DIR}/model/activity.h"
    TEST_ACTIVITY_TUI_SOURCE="${CMAKE_CURRENT_SOURCE_DIR}/tui.cpp"
    TEST_ACTIVITY_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/test/golden"
)
target_link_libraries(test_activity PRIVATE
    llmfun_tui_all_lib
    imtui-ncurses
    ncursesw
    glfw
    ${CMAKE_DL_LIBS}
)

# The inspector's Agents tab (task 30, design §5.21 / FR-14 / D-BLOCK / D-CANCEL): the lane
# list the C ABI's pipeline mirror feeds, the mission strip, the row anatomy and its
# degradation (the state word survives a 28-cell column), the lane menu (stop one) and the
# two-press `Stop all` that reuses the run's stop path, the transcript drill-down with
# `Back to main`, the bounded body, the rail and the inspector standing at the same time, and
# the three goldens (lanes running, a failed lane, the drill-down). Rewrite them with
# `./test_agents_tab --update` after a deliberate rendering change.
# Invoke from the build dir: ./test_agents_tab [-v] [--update] [--print]
add_executable(test_agents_tab
    test_agents_tab.cpp
)
add_dependencies(test_agents_tab llmfun_tui_all_lib)
# test_agents_tab includes tui.h and views/shell.h, which reach model/ and ui/ headers: the
# cpp_tui root has to be on the path (a quoted include only resolves next to its file).
target_include_directories(test_agents_tab PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(test_agents_tab PRIVATE
    TEST_AGENTS_INSPECTOR_SOURCE="${CMAKE_CURRENT_SOURCE_DIR}/views/inspector.cpp"
    TEST_AGENTS_API_SOURCE="${CMAKE_CURRENT_SOURCE_DIR}/tui_api.cpp"
    TEST_AGENTS_MODEL_SOURCE="${CMAKE_CURRENT_SOURCE_DIR}/model/ui_state.cpp"
    TEST_AGENTS_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/test/golden"
)
target_link_libraries(test_agents_tab PRIVATE
    llmfun_tui_all_lib
    imtui-ncurses
    ncursesw
    glfw
    ${CMAKE_DL_LIBS}
)

# The floating inspector in the pixel profile (task 29, D-OVERLAY): at 900x600 the layout
# makes the panel the overlay shape, which only the real GLFW/OpenGL3 backend can draw -
# the grid half in `./test_inspector` cannot see the `##inspector_panel` child window's
# paint order. Needs a display:
#   xvfb-run -s "-screen 0 900x600x24" ./test_inspector_gui [--out DIR]
add_executable(test_inspector_gui
    test_inspector_gui.cpp
)
add_dependencies(test_inspector_gui llmfun_tui_all_lib)
# test_inspector_gui includes tui.h and views/shell.h, which reach model/ and ui/ headers:
# the cpp_tui root has to be on the path (a quoted include only resolves next to its file).
target_include_directories(test_inspector_gui PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_inspector_gui PRIVATE
    llmfun_tui_all_lib
    imtui-ncurses
    ncursesw
    glfw
    ${CMAKE_DL_LIBS}
)

# The Raw Log surface (task 33, design §4.9 / FR-16 / D-RAWLOG): the developer stream's row
# format, its filter, the `UiLog` floor, the save (path, no overwrite, never the workarea), the
# bounded window over 5000 records, the 10 fps follower, the access paths (`/rawlog`, the ABI,
# the palette row) and the frame's shapes (the grid's full-screen overlay, the GUI drawer that
# never covers the composer). Rewrite the goldens with `./test_rawlog --update` after a
# deliberate rendering change.
# Invoke from the build dir: ./test_rawlog [-v] [--update] [--print]
add_executable(test_rawlog
    test_rawlog.cpp
)
add_dependencies(test_rawlog llmfun_tui_all_lib)
# test_rawlog includes tui.h and views/shell.h, which reach model/ and ui/ headers: the
# cpp_tui root has to be on the path (a quoted include only resolves next to its file).
target_include_directories(test_rawlog PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(test_rawlog PRIVATE
    TEST_RAWLOG_VIEW_SOURCE="${CMAKE_CURRENT_SOURCE_DIR}/views/rawlog.cpp"
    TEST_RAWLOG_HEADER_SOURCE="${CMAKE_CURRENT_SOURCE_DIR}/views/rawlog.h"
    TEST_RAWLOG_MODEL_SOURCE="${CMAKE_CURRENT_SOURCE_DIR}/model/rawlog_data.cpp"
    TEST_RAWLOG_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/test/golden"
)
target_link_libraries(test_rawlog PRIVATE
    llmfun_tui_all_lib
    imtui-ncurses
    ncursesw
    glfw
    ${CMAKE_DL_LIBS}
)

# The minimal settings sheet (task 34, design §4.10 / FR-17 / R-11): the value catalogue the rows
# print (the send keys the key gate proves, the four `UiLog` words, the font scale's range and
# step), the three sections and their search, the keys (`Tab`, `Ctrl+P`/`Ctrl+N`, `Enter`, the click
# that selects before it edits), the frame wiring (the top bar's `Settings` entry, the `Esc` step,
# the z-order, the layout rectangle), the `ui.conf` round trip and the one-warning read-only rule,
# the copy payload and the five goldens. Rewrite them with `./test_settings_sheet --update` after a
# deliberate rendering change.
# Invoke from the build dir: ./test_settings_sheet [-v] [--update] [--print]
add_executable(test_settings_sheet
    test_settings_sheet.cpp
)
add_dependencies(test_settings_sheet llmfun_tui_all_lib)
# test_settings_sheet includes tui.h and views/shell.h, which reach model/ and ui/ headers: the
# cpp_tui root has to be on the path (a quoted include only resolves next to its file).
target_include_directories(test_settings_sheet PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(test_settings_sheet PRIVATE
    TEST_SETTINGS_VIEW_SOURCE="${CMAKE_CURRENT_SOURCE_DIR}/views/settings.cpp"
    TEST_SETTINGS_HEADER_SOURCE="${CMAKE_CURRENT_SOURCE_DIR}/views/settings.h"
    TEST_SETTINGS_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/test/golden"
)
target_link_libraries(test_settings_sheet PRIVATE
    llmfun_tui_all_lib
    imtui-ncurses
    ncursesw
    glfw
    ${CMAKE_DL_LIBS}
)

# The help page (task 42, design FR-27): the generated key section (one row per binding of the
# registry, spelled with the same `chordLabel()` the document uses), the palette's command table,
# one paragraph per region (the rail's must match the code), the search (substring, case-blind,
# wrapped prose), the keys (`Shift+/` opens it, a text field keeps it closed, `Esc` is the chain's
# first step), the layout rectangle and z-order, the frame wiring (`?` in the character queue, `/`
# in the search field, `Close`) and the three goldens. The Keys-vs-document check is what keeps the
# page from drifting away from `doc/ui_keys.md`. Rewrite the fixtures with
# `./test_help --update` after a deliberate rendering change.
# Invoke from the build dir: ./test_help [-v] [--update] [--print]
add_executable(test_help
    test_help.cpp
    model/keymap_doc.cpp
)
add_dependencies(test_help llmfun_tui_all_lib)
# test_help includes tui.h and views/shell.h, which reach model/ and ui/ headers: the
# cpp_tui root has to be on the path (a quoted include only resolves next to its file).
target_include_directories(test_help PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(test_help PRIVATE
    TEST_HELP_VIEW_SOURCE="${CMAKE_CURRENT_SOURCE_DIR}/views/help.cpp"
    TEST_HELP_HEADER_SOURCE="${CMAKE_CURRENT_SOURCE_DIR}/views/help.h"
    TEST_HELP_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/test/golden"
)
target_link_libraries(test_help PRIVATE
    llmfun_tui_all_lib
    imtui-ncurses
    ncursesw
    glfw
    ${CMAKE_DL_LIBS}
)

# The settings sheet in the pixel profile, where the type scale and the search field's frame height
# are real (a text cell has one size). It proves the sheet fills the box the layout answered, that
# the value column is drawn and not clipped away, that the font-scale row *changes* the scale and
# that the change reaches the next frame's metrics - the one setting whose effect the grid cannot
# show. Needs a display; runs headless under xvfb:
#   xvfb-run -s "-screen 0 1280x820x24" ./test_settings_gui [--out DIR]
add_executable(test_settings_gui
    test_settings_gui.cpp
)
add_dependencies(test_settings_gui llmfun_tui_all_lib)
# test_settings_gui includes tui.h and views/shell.h, which reach model/ and ui/ headers: the
# cpp_tui root has to be on the path (a quoted include only resolves next to its file).
target_include_directories(test_settings_gui PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_settings_gui PRIVATE
    llmfun_tui_all_lib
    imtui-ncurses
    ncursesw
    glfw
    ${CMAKE_DL_LIBS}
)

# The merge gate's core loop (task 40, design §8 / D-PARITY): one scripted session - start,
# session select, ask, stream, stop mid-stream, answer, copy, agents tab, activity tab, raw log
# open/close, settings change, quit - driven through the real frame pipeline (`tuiRender()`) and
# the real C ABI, with the golden screens of the whole shell at 120x36, 100x30, 80x24 and 60x20
# and the performance gate on the design's fixture set (1000 messages / 2000 sessions / 5000 raw
# records, on the real text backend).
#
# The default mode is the headless text grid (imtui backend, text-mode markdown profile, the
# `TScreen` pattern of the other harnesses); `--gui` drives the real GLFW/OpenGL3 backend under
# xvfb, which is where the pixel profile's steps and the 1280x820 capture live. The golden
# directory and the source tree are compile-time definitions, not CWD guesses.
# Invoke from the build dir:
#   ./parity_core_loop [-v] [--print] [--update]
#   xvfb-run -s "-screen 0 1280x820x24" ./parity_core_loop --gui --out DIR
add_executable(parity_core_loop
    test/parity_core_loop.cpp
)
add_dependencies(parity_core_loop llmfun_tui_all_lib)
# parity_core_loop includes tui.h, tui_api.h, views/*.h and model/*.h, whose quoted includes
# resolve against the cpp_tui root (not against the test file's own directory).
target_include_directories(parity_core_loop PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(parity_core_loop PRIVATE
    TEST_PARITY_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/test/golden"
)
# tui.h (the C++ header) needs the imtui include paths and the IMGUI_USE_WCHAR32 definition; the
# archives are already inside llmfun_tui_all_lib, so the interface targets contribute include
# paths only.
target_link_libraries(parity_core_loop PRIVATE
    llmfun_tui_all_lib
    imtui-ncurses
    ncursesw
    glfw
    ${CMAKE_DL_LIBS}
)
