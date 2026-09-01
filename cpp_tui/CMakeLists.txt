cmake_minimum_required(VERSION 3.10)
project(llmfun_tui VERSION 0.1.0 LANGUAGES CXX)

# C++17 for our sources (imtui uses C++11 internally, unaffected)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Align with imtui.mak: disable shared libs, disable curl, skip examples
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(IMTUI_SUPPORT_CURL OFF CACHE BOOL "" FORCE)
set(IMTUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)

# Bring in imtui as a subdirectory — builds imtui, imtui-ncurses, imgui-for-imtui
# imtui-ncurses transitively provides the PUBLIC include path, so no extra target_include_directories needed
add_subdirectory(../vendor/imtui ${CMAKE_BINARY_DIR}/imtui)
add_subdirectory(../vendor/imgui_markdown ${CMAKE_BINARY_DIR}/imgui_markdown)

# Ensure pthreads are available
find_package(Threads REQUIRED)

# Static library: implementation + C API layer
# Enables D code (and other languages) to link against the library independently
add_library(llmfun_tui_lib STATIC
    tui.cpp
    tui_common.cpp
    tui_widgets.cpp
    session_panel.cpp
    tui_chat.cpp
    tui_api.cpp
)

target_include_directories(llmfun_tui_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(llmfun_tui_lib
    PUBLIC imtui-ncurses imgui_markdown
    PRIVATE Threads::Threads stdc++
)

add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/libllmfun_tui_all_lib.a
    COMMAND ${CMAKE_AR} -x $<TARGET_FILE:llmfun_tui_lib>
    COMMAND ${CMAKE_AR} -x $<TARGET_FILE:imtui-ncurses>
    COMMAND ${CMAKE_AR} -x $<TARGET_FILE:imtui>
    COMMAND ${CMAKE_AR} -x $<TARGET_FILE:imgui-for-imtui>
    COMMAND ${CMAKE_AR} rcs libllmfun_tui_all_lib.a *.o
    DEPENDS llmfun_tui_lib imtui-ncurses imtui imgui-for-imtui
)
add_custom_target(libllmfun_tui_all_lib ALL
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/libllmfun_tui_all_lib.a
)
add_library(llmfun_tui_all_lib STATIC IMPORTED)
set_target_properties(llmfun_tui_all_lib PROPERTIES
    IMPORTED_LOCATION ${CMAKE_CURRENT_BINARY_DIR}/libllmfun_tui_all_lib.a
)
add_dependencies(llmfun_tui_all_lib libllmfun_tui_all_lib)

# Main executable — links against the static library
add_executable(llmfun_tui
    main.cpp
)

add_dependencies(llmfun_tui llmfun_tui_all_lib)
target_link_libraries(llmfun_tui PRIVATE
    llmfun_tui_all_lib
    ncursesw
)

# Headless grid-invariant regression test for the imtui UTF-8 cell-width fix
# (implementation plan Task 3). Links the REAL imtui + imgui-for-imtui
# (IMTUI / IMGUI_USE_WCHAR32 come from those targets, exactly like llmfun_tui);
# runs headless via ImTui_ImplText_* — no ncurses, no PTY. Built by the normal
# `make` (build_test.py gate) and executed from the build dir.
add_executable(imtui_utf8_grid_test
    ../vendor/imtui/test/test_utf8_grid.cpp
)
target_link_libraries(imtui_utf8_grid_test PRIVATE imtui)

# ncurses fold-row rewrite regression test (P3 Task 6 follow-up fix).
# The binary drives the REAL ncurses backend (ImTui_ImplNcurses_DrawScreen)
# with a folded VS16 pair row across four frames of content changes. It must
# run on a PTY, so it is spawned and its raw output stream replayed by
# ../vendor/imtui/test/test_ncurses_fold_redraw.py (wired into build_test.py);
# the replayer models a VS16-clustering terminal and asserts the final row.
add_executable(imtui_ncurses_fold_redraw_test
    ../vendor/imtui/test/test_ncurses_fold_redraw.cpp
)
target_link_libraries(imtui_ncurses_fold_redraw_test PRIVATE imtui-ncurses)

# Standalone unit test for the session fuzzy matcher (Phase 4 item 2, Task 1).
# Links ONLY the standard library (no imtui/ncurses/ImGui), which also
# verifies that session_fuzzy.h compiles standalone (no TUI dependency).
# Built by the normal `make -f tui.mak` and run headless from the build dir.
add_executable(test_session_fuzzy
    test_session_fuzzy.cpp
)

# Panel-level headless smoke harness for the session filter (Phase 4, Task 8,
# A29) and the filter nav-focus hardening (Phase 4, Task 11, A31). Drives the
# real TuiState through the llmfun_tui frame loop (skipping only the terminal
# write) and asserts on the C++ state and the TScreen cell grid (labels,
# order, "no matches", UTF-8 validity, match highlight colors, nav/active id
# stability, log lines). Runs headless from the build dir:
#   ./test_session_filter_smoke < /dev/null
add_executable(test_session_filter_smoke
    test_session_filter_smoke.cpp
)
add_dependencies(test_session_filter_smoke llmfun_tui_all_lib)
# tui.h (the C++ header) needs the imtui + imgui_markdown include paths and
# the IMGUI_USE_WCHAR32 definition; link the interface-providing targets for
# exactly that (their archives are already inside llmfun_tui_all_lib, so they
# contribute no extra objects).
target_link_libraries(test_session_filter_smoke PRIVATE
    llmfun_tui_all_lib
    imtui-ncurses
    imgui_markdown
    ncursesw
)

# Headless clamp test for tuiSetMaxWidth / the tuiRender clamp (Task 1).
# Text backend only (no ncurses, no PTY) — runs a real TuiState through
# tuiRender and checks the grid width. Invoke: ./test_tui_maxwidth
add_executable(test_tui_maxwidth
    test_tui_maxwidth.cpp
)
add_dependencies(test_tui_maxwidth llmfun_tui_all_lib)
# tui.h (the C++ header) needs the imtui + imgui_markdown include paths and
# the IMGUI_USE_WCHAR32 definition; link the interface-providing targets for
# exactly that (their archives are already inside llmfun_tui_all_lib, so they
# contribute no extra objects).
target_link_libraries(test_tui_maxwidth PRIVATE
    llmfun_tui_all_lib
    imtui-ncurses
    imgui_markdown
    ncursesw
)
