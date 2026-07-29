#!/usr/bin/env python3
"""
Check build tool availability (cmake, make, gcc, g++, etc.).
Quick verification that the build environment is set up.

Usage:
    python3 check_build_tools.py [--verbose]
"""
import shutil
import sys


def check_tool(name, version_flag="--version"):
    """Check if a tool is available and get its version."""
    path = shutil.which(name)
    if not path:
        return None, None

    # Try to get version
    try:
        import subprocess
        result = subprocess.run(
            [path, version_flag],
            capture_output=True, text=True, timeout=5
        )
        if result.returncode == 0:
            version = result.stdout.strip().split("\n")[0]
        else:
            version = result.stderr.strip().split("\n")[0] if result.stderr else "unknown"
    except (FileNotFoundError, subprocess.TimeoutExpired, OSError):
        version = "unknown"

    return path, version


def main():
    verbose = "--verbose" in sys.argv

    # Essential build tools
    tools = {
        "cmake": "CMake build system",
        "make": "Make build tool",
        "gcc": "GNU C compiler",
        "g++": "GNU C++ compiler",
        "ar": "Archive tool (for .a files)",
    }

    # D-specific tools
    d_tools = {
        "ldc2": "LDC D compiler",
        "dmd": "DMD D compiler",
        "dub": "D build tool",
    }

    # Optional tools
    optional = {
        "ninja": "Ninja build system",
        "clang": "Clang C compiler",
        "clang++": "Clang C++ compiler",
        "pkg-config": "Package configuration tool",
    }

    print("Build Tool Availability Check")
    print("=" * 50)

    def check_group(title, tool_dict):
        print(f"\n{title}:")
        found = 0
        for name, desc in tool_dict.items():
            path, version = check_tool(name)
            if path:
                status = "OK"
                found += 1
                if verbose:
                    print(f"  [OK]   {name:12s} ({desc})")
                    print(f"         Path: {path}")
                    if version:
                        print(f"         Version: {version}")
                else:
                    v = f" ({version})" if version else ""
                    print(f"  [OK]   {name:12s}{v}")
            else:
                status = "MISSING"
                print(f"  [MISSING] {name:12s} ({desc})")
        return found, len(tool_dict)

    essential_found, essential_total = check_group("Essential Build Tools", tools)
    d_found, d_total = check_group("D Language Tools", d_tools)

    if verbose:
        optional_found, optional_total = check_group("Optional Tools", optional)
    else:
        # Just check if any optional tools exist
        optional_found = sum(1 for name in optional if shutil.which(name))
        optional_total = len(optional)

    print(f"\n{'=' * 50}")
    print(f"Essential: {essential_found}/{essential_total} found")
    print(f"D Tools:   {d_found}/{d_total} found")
    if verbose:
        print(f"Optional:  {optional_found}/{optional_total} found")

    # Summary
    print()
    if essential_found == essential_total and d_found >= 1:
        print("OK: Build environment looks good")
    elif essential_found >= essential_total - 1:
        print("WARNING: Most essential tools found")
    else:
        print("WARNING: Missing essential build tools")

    if d_found == 0:
        print("WARNING: No D compiler found")


if __name__ == "__main__":
    main()
