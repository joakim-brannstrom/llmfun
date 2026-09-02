#!/usr/bin/env python3
"""Build and run imtui tests.

Usage:
    python3 build_test.py          # Build and run tests
    python3 build_test.py --build  # Build only (llmfun_tui compilation check)
    python3 build_test.py --run    # Run only (must be built first)

Tests are Python-based since imtui's stripped-down ImGui lacks the full
API needed by imgui_markdown. The compilation test verifies that
imgui_markdown.h compiles correctly as part of the llmfun_tui build.

The runtime harness test_inline_code_runtime.cpp additionally compiles
imgui_markdown.h standalone against a fake ImGui namespace (g++ only, no
imtui) and asserts the exact rendered byte stream for every inline-code
edge-case matrix row.

The width-table unit test test_wcwidth.cpp compiles imgui_draw.cpp by TU
inclusion (IMTUI + IMGUI_USE_WCHAR32, exactly like the llmfun_tui build)
and asserts the Unicode cell-width vectors for emoji-presentation and
zero-width codepoints.

The ncurses fold-row rewrite test (binary imtui_ncurses_fold_redraw_test +
driver test_ncurses_fold_redraw.py) runs the real ncurses backend on a PTY
with a folded VS16 pair row across frames of content changes, replays the
raw output stream against a VS16-clustering terminal model and asserts the
final rows — the only layer where the fold-row rewrite corruption is
observable (the TScreen grid is internally consistent pre-fix, and tmux
counts the pair as 1 cell like ncurses' own model).

The grid-invariant regression test test_utf8_grid.cpp renders fixed text
rows through the real vendored imgui + imtui text backend (CMake target
imtui_utf8_grid_test, linked like llmfun_tui) into a TScreen grid and
asserts the grid-vs-terminal-width invariants restored by the UTF-8 width
fix: per-row grid extent matches an independent width oracle, wide emoji
land two cells after their base, and no zero-width codepoint leaks a grid
cell. It fails against the pre-fix width table (✅/❓ rows short by 1,
⚠️ row long by 1). Both it and test_wcwidth run TWICE: default, and with
the emoji env overrides set (LLMFUN_IMTUI_EMOJI_PRESENTATION=1 for the
grid test's VS16/combining-mark folding assertions,
LLMFUN_IMTUI_EMOJI_WIDTH=1 for the width-one emoji class).
"""

import subprocess
import sys
import os
import shutil

def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else "both"
    mode = mode.lstrip("-")  # accept --build/--run as documented in the README
    if mode not in ("both", "build", "run"):
        print(f"Unknown mode: {sys.argv[1]!r}. Usage: build_test.py [--build|--run]")
        return 2

    skipped = 0  # runtime harness not verifiable (g++ missing)
    combined = 0  # overall status; initialized here so --build mode
                  # (which never enters the run phase) still returns 0 on success
    print("=== Installing build tools ===")
    result = subprocess.run(
        ["apt-get", "update"],
        capture_output=True, text=True, timeout=120
    )
    print(f"apt-get update: exit={result.returncode}")

    result = subprocess.run(
        ["apt-get", "install", "-y", "build-essential", "cmake", "libncurses-dev"],
        capture_output=True, text=True, timeout=180
    )
    print(f"apt-get install: exit={result.returncode}")
    if result.returncode != 0:
        print(f"STDERR: {result.stderr[:500]}")
    else:
        print("SUCCESS")

    basedir = os.path.abspath(".")
    testdir = os.path.join(basedir, "llmfun", "vendor", "imtui", "test")
    builddir = os.path.join(basedir, "llmfun", "cpp_tui", "build")

    if mode in ("both", "build"):
        print(f"\n=== Building llmfun_tui (compilation check) ===")
        if os.path.exists(builddir):
            shutil.rmtree(builddir)
        os.makedirs(builddir, exist_ok=True)

        result = subprocess.run(
            ["cmake", ".."],
            capture_output=True, text=True,
            cwd=builddir, timeout=120
        )
        print(f"CMake exit code: {result.returncode}")
        if result.returncode != 0:
            print(result.stdout[-500:] if result.stdout else "")
            print(result.stderr[-500:] if result.stderr else "")
            print("CMake failed. Aborting.")
            return 1

        result = subprocess.run(
            ["make", "-j4"],
            capture_output=True, text=True,
            cwd=builddir, timeout=120
        )
        print(f"Make exit code: {result.returncode}")
        if result.returncode != 0:
            print(result.stdout[-500:] if result.stdout else "")
            print(result.stderr[-500:] if result.stderr else "")
            print("Build failed. Aborting.")
            return 1

        print("Build succeeded - imgui_markdown.h compiles correctly.")

    if mode in ("both", "run"):
        print(f"\n=== Running tests ===")
        combined = 0

        # Static-analysis test scripts (Python)
        for script_name in ("test_code_block.py", "test_inline_code.py"):
            test_script = os.path.join(testdir, script_name)
            if not os.path.exists(test_script):
                print(f"Test script not found: {test_script}")
                combined = 1
                continue

            result = subprocess.run(
                [sys.executable, test_script],
                capture_output=True, text=True,
                cwd=testdir, timeout=60
            )
            print(result.stdout)
            if result.stderr:
                print(f"STDERR:\n{result.stderr}")

            if result.returncode == 0:
                print(f"{script_name}: all tests passed.")
            else:
                print(f"{script_name}: some tests failed (exit code {result.returncode}).")
                combined = 1

        # Runtime harness (C++): exact rendered byte stream for every
        # inline-code edge-case matrix row, against a fake ImGui namespace.
        harness_src = os.path.join(testdir, "test_inline_code_runtime.cpp")
        harness_bin = os.path.join(testdir, "test_inline_code_runtime")
        try:
            result = subprocess.run(
                ["g++", "-std=c++11", "-O0", "-o", harness_bin, harness_src],
                capture_output=True, text=True,
                cwd=testdir, timeout=120
            )
        except FileNotFoundError:
            print("Harness NOT verified: g++ not found (install build-essential first).")
            result = None
        if result is None:
            skipped = 1
        elif result.returncode != 0:
            print(result.stdout[-500:] if result.stdout else "")
            print(result.stderr[-500:] if result.stderr else "")
            print("Harness compilation failed.")
            combined = 1
        else:
            result = subprocess.run(
                [harness_bin],
                capture_output=True, text=True,
                cwd=testdir, timeout=60
            )
            print(result.stdout)
            if result.stderr:
                print(f"STDERR:\n{result.stderr}")
            if result.returncode == 0:
                print("Runtime harness: all checks passed.")
            else:
                print(f"Runtime harness: some checks failed (exit code {result.returncode}).")
                combined = 1
            try:
                os.remove(harness_bin)
            except OSError:
                pass

        # Width-table unit test (C++): verifies imtui_wcwidth /
        # ImFontIMTuiCellWidth vectors (emoji-presentation codepoints are
        # width 2, zero-width format/VS/combining codepoints are width 0).
        # Compiles imgui_draw.cpp by TU inclusion; -ffunction-sections
        # -Wl,--gc-sections drop the unreferenced imgui functions so no
        # other translation unit needs to be linked.
        wc_src = os.path.join(testdir, "test_wcwidth.cpp")
        wc_bin = os.path.join(testdir, "test_wcwidth")
        try:
            result = subprocess.run(
                ["g++", "-std=c++11", "-O0", "-ffunction-sections",
                 "-Wl,--gc-sections", "-o", wc_bin, wc_src],
                capture_output=True, text=True,
                cwd=testdir, timeout=120
            )
        except FileNotFoundError:
            print("Width-table test NOT verified: g++ not found (install build-essential first).")
            result = None
        if result is None:
            skipped = 1
        elif result.returncode != 0:
            print(result.stdout[-500:] if result.stdout else "")
            print(result.stderr[-500:] if result.stderr else "")
            print("Width-table test compilation failed.")
            combined = 1
        else:
            result = subprocess.run(
                [wc_bin],
                capture_output=True, text=True,
                cwd=testdir, timeout=60
            )
            print(result.stdout)
            if result.stderr:
                print(f"STDERR:\n{result.stderr}")
            if result.returncode == 0:
                print("Width-table test: all checks passed.")
            else:
                print(f"Width-table test: some checks failed (exit code {result.returncode}).")
                combined = 1

            # Second run: LLMFUN_IMTUI_EMOJI_WIDTH=1 forces the
            # Emoji_Presentation=Yes class to width 1 (narrow-emoji terminal
            # mitigation). The binary reads the env var and adjusts its
            # expected emoji-class widths accordingly.
            result = subprocess.run(
                [wc_bin],
                capture_output=True, text=True,
                cwd=testdir, timeout=60,
                env={**os.environ, "LLMFUN_IMTUI_EMOJI_WIDTH": "1"}
            )
            print(result.stdout)
            if result.stderr:
                print(f"STDERR:\n{result.stderr}")
            if result.returncode == 0:
                print("Width-table test (LLMFUN_IMTUI_EMOJI_WIDTH=1): all checks passed.")
            else:
                print(f"Width-table test (LLMFUN_IMTUI_EMOJI_WIDTH=1): some checks failed (exit code {result.returncode}).")
                combined = 1
            try:
                os.remove(wc_bin)
            except OSError:
                pass

        # Headless grid-invariant regression test (C++): renders fixed text
        # rows through the REAL vendored imgui + imtui text backend into a
        # TScreen grid and asserts the grid-vs-terminal-width invariants
        # restored by the UTF-8 width fix. Built by the cmake phase (target
        # imtui_utf8_grid_test) into the llmfun_tui build dir; this block
        # only executes it.
        grid_bin = os.path.join(builddir, "imtui_utf8_grid_test")
        if os.path.exists(grid_bin):
            result = subprocess.run(
                [grid_bin],
                capture_output=True, text=True,
                cwd=testdir, timeout=120
            )
            print(result.stdout)
            if result.stderr:
                print(f"STDERR:\n{result.stderr}")
            if result.returncode == 0:
                print("Grid regression test: all checks passed.")
            else:
                print(f"Grid regression test: some checks failed (exit code {result.returncode}).")
                combined = 1

            # Second run: LLMFUN_IMTUI_EMOJI_PRESENTATION=1
            # switches the production renderer into VS16/combining-mark
            # folding mode (terminals that cluster VS16). The test asserts
            # the folding invariants: U+26A0 + U+FE0F folds into one cell
            # with chwidth == 2 and ch2 == U+FE0F, combining marks merge
            # into their base cell, and per-row extents follow the
            # folding-aware oracle.
            result = subprocess.run(
                [grid_bin],
                capture_output=True, text=True,
                cwd=testdir, timeout=120,
                env={**os.environ, "LLMFUN_IMTUI_EMOJI_PRESENTATION": "1"}
            )
            print(result.stdout)
            if result.stderr:
                print(f"STDERR:\n{result.stderr}")
            if result.returncode == 0:
                print("Grid regression test (folding mode): all checks passed.")
            else:
                print(f"Grid regression test (folding mode): some checks failed (exit code {result.returncode}).")
                combined = 1
        else:
            print("Grid regression test NOT found — run with --build first "
                  "(binary is produced by the cmake phase).")
            combined = 1

        # ncurses fold-row rewrite regression test: the C++ binary
        # imtui_ncurses_fold_redraw_test (built by the cmake phase, target
        # of the same name) drives the real ncurses
        # backend with a folded VS16 pair row across frames of content
        # changes; the Python driver spawns it on a PTY, replays the raw
        # output stream against a VS16-clustering terminal model and
        # asserts the final rows. The PTY layer is what makes the bug
        # observable (grid tests and tmux captures cannot: the TScreen is
        # internally consistent pre-fix and tmux counts the pair as 1 cell
        # like ncurses). Skipped (not failed) when the pty modules or the
        # binary are unavailable.
        fold_bin = os.path.join(builddir, "imtui_ncurses_fold_redraw_test")
        fold_driver = os.path.join(testdir, "test_ncurses_fold_redraw.py")
        if os.path.exists(fold_bin) and os.path.exists(fold_driver):
            try:
                result = subprocess.run(
                    [sys.executable, fold_driver, fold_bin],
                    capture_output=True, text=True,
                    cwd=testdir, timeout=120
                )
                print(result.stdout)
                if result.stderr:
                    print(f"STDERR:\n{result.stderr}")
                if result.returncode == 0:
                    print("ncurses fold-row rewrite test: all checks passed.")
                else:
                    print(f"ncurses fold-row rewrite test: some checks failed "
                          f"(exit code {result.returncode}).")
                    combined = 1
            except OSError as exc:
                print(f"ncurses fold-row rewrite test NOT verified: PTY driver "
                      f"failed to run ({exc}); treated as skipped.")
                skipped = 1
        else:
            print("ncurses fold-row rewrite test NOT found — run with --build "
                  "first (binary is produced by the cmake phase).")
            combined = 1

        # Max-width byte-stream test: run llmfun_tui on a PTY with
        # LLMFUN_TUI_MAX_WIDTH=CAP and assert no terminal data is written at
        # col >= CAP (the right margin). Uses probe_margin.c --run's
        # max_data_col (DATA writes only, excluding J/K clears and cursor
        # moves). PROBE_UNTIL cuts metrics at the standalone-mode
        # "smoke ok:" banner so only the rendered frames are measured. A
        # negative control (no cap) must leak data into the margin
        # (max_data_col >= CAP), proving the test discriminates rather than
        # passing trivially. Skipped (not failed) when the probe cannot run
        # on a PTY.
        r2_cap = 255
        r2_probe_src = os.path.join(basedir, "llmfun", "cpp_tui", "probe_margin.c")
        r2_probe_bin = os.path.join(testdir, "probe_margin")
        r2_tui = None
        for cand in (os.path.join(builddir, "llmfun_tui"),
                     os.path.join(basedir, "llmfun", "build", "tui", "llmfun_tui")):
            if os.path.exists(cand):
                r2_tui = cand
                break
        r2_repo = os.path.join(basedir, "llmfun")
        if not os.path.exists(r2_probe_src) or r2_tui is None:
            print("R2 max-width gate NOT verified: probe_margin.c or the "
                  "llmfun_tui binary is missing; treated as skipped.")
            skipped = 1
        else:
            r2_built = False
            try:
                br = subprocess.run(
                    ["gcc", "-w", "-o", r2_probe_bin, r2_probe_src, "-lncursesw"],
                    capture_output=True, text=True, cwd=testdir, timeout=120
                )
                r2_built = br.returncode == 0
            except FileNotFoundError:
                r2_built = False
            if not r2_built:
                print("R2 max-width gate NOT verified: could not build "
                      "probe_margin.c (gcc or libncursesw unavailable); "
                      "treated as skipped.")
                skipped = 1
            else:
                def r2_run(use_cap):
                    # Sanitize env: strip any ambient LLMFUN_TUI_MAX_WIDTH so
                    # the uncapped (negative control) run is truly uncapped.
                    env = {k: v for k, v in os.environ.items()
                           if k != "LLMFUN_TUI_MAX_WIDTH"}
                    env.update({"PROBE_W": "300", "PROBE_H": "50",
                                "PROBE_UNTIL": "smoke ok:"})
                    if use_cap:
                        env["LLMFUN_TUI_MAX_WIDTH"] = str(r2_cap)
                    return subprocess.run(
                        [r2_probe_bin, "--run", r2_tui, "--frames", "10"],
                        capture_output=True, text=True, cwd=r2_repo,
                        timeout=120, env=env
                    )

                def r2_parse(out):
                    mdc = None
                    for line in out.splitlines():
                        if line.startswith("max_data_col="):
                            mdc = int(line.split("=", 1)[1].strip())
                    if mdc is None:
                        return None
                    return mdc

                try:
                    capped = r2_run(True)
                    print(capped.stdout)
                    if capped.stderr:
                        print(f"STDERR:\n{capped.stderr}")
                    cp = r2_parse(capped.stdout)
                    if cp is None:
                        print("R2 max-width gate NOT verified: probe produced "
                              "no metrics (no PTY support?); treated as "
                              "skipped.")
                        skipped = 1
                    else:
                        r2_pass = cp < r2_cap
                        if r2_pass:
                            print(f"R2 max-width gate: capped run clean "
                                  f"(max_data_col={cp} < {r2_cap}).")
                        else:
                            print(f"R2 max-width gate FAILED: capped "
                                  f"max_data_col={cp} (want < {r2_cap}).")
                            combined = 1
                        # Negative control: with no cap the TUI uses the full
                        # window, so data MUST reach the margin (>= CAP).
                        uncap = r2_run(False)
                        print(uncap.stdout)
                        if uncap.stderr:
                            print(f"STDERR:\n{uncap.stderr}")
                        up = r2_parse(uncap.stdout)
                        if up is None:
                            print("R2 negative control NOT verified: probe "
                                  "produced no metrics; treated as skipped.")
                            skipped = 1
                        elif up < r2_cap:
                            print(f"R2 negative control FAILED: uncapped "
                                  f"max_data_col={up} < {r2_cap} -- the "
                                  f"gate would not discriminate; failing.")
                            combined = 1
                        else:
                            print(f"R2 negative control: uncapped run leaks "
                                  f"into the margin (max_data_col={up} "
                                  f">= {r2_cap}) -- gate discriminates.")
                            if r2_pass:
                                print("R2 max-width gate: all checks passed.")
                except (OSError, subprocess.TimeoutExpired) as exc:
                    print(f"R2 max-width gate NOT verified: PTY run failed "
                          f"({exc}); treated as skipped.")
                    skipped = 1
                finally:
                    try:
                        os.remove(r2_probe_bin)
                    except OSError:
                        pass

        if combined == 0 and skipped == 0:
            print("\nAll tests passed.")
        elif skipped:
            print("\nOne or more harnesses could not be verified (see the "
                  "messages above — e.g. missing g++ or no PTY support); "
                  "overall gate fails.")
        else:
            print("\nSome tests failed.")

    if skipped and combined == 0:
        return 1
    return combined

if __name__ == "__main__":
    sys.exit(main())
