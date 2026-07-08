#!/usr/bin/env python3
"""Build and run imtui tests.

Usage:
    python3 build_test.py          # Build and run tests
    python3 build_test.py --build  # Build only (llmfun_tui compilation check)
    python3 build_test.py --run    # Run only (must be built first)

Tests are Python-based since imtui's stripped-down ImGui lacks the full
API needed by imgui_markdown. The compilation test verifies that
imgui_markdown.h compiles correctly as part of the llmfun_tui build.
"""

import subprocess
import sys
import os
import shutil

def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else "both"

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
        test_script = os.path.join(testdir, "test_code_block.py")
        if not os.path.exists(test_script):
            print(f"Test script not found: {test_script}")
            return 1

        result = subprocess.run(
            [sys.executable, test_script],
            capture_output=True, text=True,
            cwd=testdir, timeout=60
        )
        print(result.stdout)
        if result.stderr:
            print(f"STDERR:\n{result.stderr}")

        if result.returncode == 0:
            print("\nAll tests passed.")
        else:
            print(f"\nSome tests failed (exit code {result.returncode}).")
            return result.returncode

    return 0

if __name__ == "__main__":
    sys.exit(main())
