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
add_subdirectory(../vendor/imgui ${CMAKE_BINARY_DIR}/imgui)
add_subdirectory(../vendor/imtui ${CMAKE_BINARY_DIR}/imtui)
add_subdirectory(../vendor/imgui_markdown ${CMAKE_BINARY_DIR}/imgui_markdown)

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
    imgui_markdown
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

# Panel-level headless smoke harness for the session filter and the filter nav-focus hardening. Drives the real TuiState through the llmfun_tui frame loop (skipping only the terminal write) and asserts on the C++ state and the TScreen cell grid (labels, order, "no matches", UTF-8 validity, match highlight colors, nav/active id stability, log lines). Runs headless from the build dir:
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
    glfw
    ${CMAKE_DL_LIBS}
)

# Headless clamp test for tuiSetMaxWidth / the tuiRender clamp. Text backend only (no ncurses, no PTY) — runs a real TuiState through tuiRender and checks the grid width. Invoke: ./test_tui_maxwidth
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
    glfw
    ${CMAKE_DL_LIBS}
)

# Headless regression test for the widened clear contract: tuiClearOutput / C-API tuiClearChatMessages must reset ALL derived chat-display state — output lines, the open-group set (collapsed headers; stale positions suppressed header labels of replayed messages) and the in-flight stream message — not just outputLines. Pure state-level assertions; no terminal, no ImGui context, no rendering needed. Invoke:
# ./test_clear_output_state
add_executable(test_clear_output_state
    test_clear_output_state.cpp
)
add_dependencies(test_clear_output_state llmfun_tui_all_lib)
# tui.h (the C++ header) needs the imtui + imgui_markdown include paths and
# the IMGUI_USE_WCHAR32 definition; link the interface-providing targets for
# exactly that (their archives are already inside llmfun_tui_all_lib, so they
# contribute no extra objects).
target_link_libraries(test_clear_output_state PRIVATE
    llmfun_tui_all_lib
    imtui-ncurses
    imgui_markdown
    ncursesw
    glfw
    ${CMAKE_DL_LIBS}
)
