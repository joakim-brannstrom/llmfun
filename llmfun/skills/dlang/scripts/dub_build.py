#!/usr/bin/env python3
"""
Build a dub project and verify the result.

Usage:
    python3 dub_build.py [project_dir] [--force] [--test] [--compiler ldc2]
"""
import subprocess
import sys
import os
import argparse


def find_dub():
    """Find dub build tool."""
    paths = ["dub", "/opt/ldc/bin/dub"]
    for p in paths:
        try:
            result = subprocess.run([p, "--version"], capture_output=True, text=True, timeout=5)
            if result.returncode == 0:
                return p
        except (FileNotFoundError, subprocess.TimeoutExpired):
            continue

    # Try walking /opt/ldc
    if os.path.exists("/opt/ldc"):
        for root, dirs, files in os.walk("/opt/ldc"):
            for f in files:
                if f == "dub" or f.startswith("dub-"):
                    full = os.path.join(root, f)
                    try:
                        result = subprocess.run([full, "--version"], capture_output=True, text=True, timeout=5)
                        if result.returncode == 0:
                            return full
                    except (FileNotFoundError, subprocess.TimeoutExpired):
                        continue

    print("ERROR: dub not found")
    sys.exit(1)


def build(project_dir, force=False, compiler=None):
    """Build a dub project."""
    dub = find_dub()

    cmd = [dub, "build"]
    if force:
        cmd.append("--force")
    if compiler:
        cmd.extend(["--compiler", compiler])

    # Set PATH to include /opt/ldc/bin
    env = os.environ.copy()
    if os.path.exists("/opt/ldc/bin"):
        env["PATH"] = "/opt/ldc/bin:" + env.get("PATH", "")

    print(f"Running: {' '.join(cmd)}")
    result = subprocess.run(cmd, cwd=project_dir, capture_output=True, text=True, timeout=300, env=env)

    # Show last part of output
    stdout = result.stdout
    if len(stdout) > 3000:
        stdout = "... (truncated) ...\n" + stdout[-3000:]
    print(stdout)

    if result.stderr:
        stderr = result.stderr
        if len(stderr) > 2000:
            stderr = "... (truncated) ...\n" + stderr[-2000:]
        print("STDERR:", stderr)

    if result.returncode == 0:
        print(f"\nBUILD SUCCESS")
    else:
        print(f"\nBUILD FAILED (exit code {result.returncode})")

    return result.returncode == 0


def test(project_dir, compiler=None):
    """Run tests in a dub project."""
    dub = find_dub()

    cmd = [dub, "test"]
    if compiler:
        cmd.extend(["--compiler", compiler])

    env = os.environ.copy()
    if os.path.exists("/opt/ldc/bin"):
        env["PATH"] = "/opt/ldc/bin:" + env.get("PATH", "")

    print(f"Running: {' '.join(cmd)}")
    result = subprocess.run(cmd, cwd=project_dir, capture_output=True, text=True, timeout=300, env=env)

    stdout = result.stdout
    if len(stdout) > 5000:
        stdout = "... (truncated) ...\n" + stdout[-5000:]
    print(stdout)

    if result.returncode == 0:
        print(f"\nALL TESTS PASSED")
    else:
        print(f"\nTESTS FAILED (exit code {result.returncode})")

    return result.returncode == 0


def main():
    parser = argparse.ArgumentParser(description="Build a dub project")
    parser.add_argument("project", nargs="?", default=".", help="Project directory (default: current)")
    parser.add_argument("--force", action="store_true", help="Force rebuild")
    parser.add_argument("--test", action="store_true", help="Run tests instead of build")
    parser.add_argument("--compiler", choices=["ldc2", "dmd", "gdc"], help="Specify compiler")

    args = parser.parse_args()

    if not os.path.isdir(args.project):
        print(f"ERROR: {args.project} is not a directory")
        sys.exit(1)

    # Check for dub config
    has_sdl = os.path.exists(os.path.join(args.project, "dub.sdl"))
    has_json = os.path.exists(os.path.join(args.project, "dub.json"))
    if not has_sdl and not has_json:
        print(f"WARNING: No dub.sdl or dub.json found in {args.project}")

    if args.test:
        ok = test(args.project, args.compiler)
    else:
        ok = build(args.project, args.force, args.compiler)

    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
