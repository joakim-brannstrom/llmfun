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

    # Install build tools (needed every session)
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

    if mode in ("both", "build"):
        print(f"\n=== Building llmfun_tui (compilation check) ===")
        builddir = os.path.join(basedir, "llmfun", "cpp_tui", "build")
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

        if combined == 0 and skipped == 0:
            print("\nAll tests passed.")
        elif skipped:
            print("\nRuntime harness NOT verified (g++ missing); overall gate fails.")
        else:
            print("\nSome tests failed.")

    if skipped and combined == 0:
        return 1
    return combined

if __name__ == "__main__":
    sys.exit(main())
