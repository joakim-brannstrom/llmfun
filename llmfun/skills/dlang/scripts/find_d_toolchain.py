#!/usr/bin/env python3
"""
Find D compilers (ldc2, dmd, gdc) and dub in the system.
Reports location and version of each tool found.
"""
import subprocess
import shutil
import os
import sys


def find_tool(name):
    """Find a tool via which, then try common paths."""
    # Try which first
    path = shutil.which(name)
    if path:
        return path

    # Try common locations
    common_paths = [
        f"/opt/ldc/bin/{name}",
        f"/usr/bin/{name}",
        f"/usr/local/bin/{name}",
    ]
    for p in common_paths:
        if os.path.exists(p):
            return p

    # Try find in /opt
    try:
        result = subprocess.run(
            ["find", "/opt", "-name", name, "-type", "f", "-executable", "-maxdepth", "4"],
            capture_output=True, text=True, timeout=10
        )
        if result.stdout.strip():
            return result.stdout.strip().split("\n")[0]
    except (subprocess.TimeoutExpired, FileNotFoundError):
        pass

    return None


def get_version(cmd, version_flag="--version"):
    """Get version string from a command."""
    try:
        result = subprocess.run(
            [cmd, version_flag],
            capture_output=True, text=True, timeout=10
        )
        if result.returncode == 0:
            # Return first line of output
            first_line = result.stdout.strip().split("\n")[0]
            return first_line
        return result.stderr.strip().split("\n")[0] if result.stderr else "unknown"
    except (subprocess.TimeoutExpired, FileNotFoundError, OSError):
        return "error getting version"


def main():
    print("=" * 60)
    print("D Language Toolchain Discovery")
    print("=" * 60)

    # D compilers
    compilers = {
        "ldc2": "LDC (LLVM-based D compiler)",
        "dmd": "DMD (reference D compiler)",
        "gdc": "GDC (GCC-based D compiler)",
        "ldmd2": "LDM2 (D-style front-end for LDC)",
    }

    print("\n--- D Compilers ---")
    found_compilers = []
    for name, desc in compilers.items():
        path = find_tool(name)
        if path:
            version = get_version(path)
            print(f"  {name:8s} ({desc})")
            print(f"    Path:    {path}")
            print(f"    Version: {version}")
            found_compilers.append((name, path))
        else:
            print(f"  {name:8s} ({desc}): NOT FOUND")

    # dub build tool
    print("\n--- Build Tools ---")
    dub_path = find_tool("dub")
    if dub_path:
        version = get_version(dub_path)
        print(f"  dub")
        print(f"    Path:    {dub_path}")
        print(f"    Version: {version}")
    else:
        print(f"  dub: NOT FOUND")

    # Also check for dub inside /opt/ldc
    if not dub_path and os.path.exists("/opt/ldc"):
        print("\n  Scanning /opt/ldc for dub...")
        for root, dirs, files in os.walk("/opt/ldc"):
            for f in files:
                if "dub" in f and not f.endswith(".md"):
                    full = os.path.join(root, f)
                    print(f"    Found: {full}")

    # Check PATH
    print(f"\n--- PATH ---")
    print(f"  {os.environ.get('PATH', 'not set')}")

    # Check DUB_HOME
    dub_home = os.environ.get("DUB_HOME", "not set")
    print(f"\n--- DUB_HOME ---")
    print(f"  {dub_home}")

    print()
    if not found_compilers and not dub_path:
        print("WARNING: No D compiler or dub found!")
        sys.exit(1)
    elif not found_compilers:
        print("WARNING: No D compiler found (only dub available)")
    elif not dub_path:
        print("WARNING: dub not found (compilers available)")
    else:
        print("OK: D toolchain found")


if __name__ == "__main__":
    main()
