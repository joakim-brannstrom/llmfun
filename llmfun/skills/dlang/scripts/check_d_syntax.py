#!/usr/bin/env python3
"""
Check D source file(s) for syntax errors using ldc2 compiler.
Compiles with -c flag (syntax check only, no linking).

Usage:
    python3 check_d_syntax.py <file.d> [file2.d ...]
    python3 check_d_syntax.py --all source/
    python3 check_d_syntax.py --project llmfun/
"""
import subprocess
import sys
import os
import argparse


def find_ldc2():
    """Find ldc2 compiler."""
    # Try common paths first
    paths = [
        "/opt/ldc/bin/ldc2",
        "ldc2",
        "ldc2-1.27",
        "ldmd2",
    ]
    for p in paths:
        try:
            result = subprocess.run([p, "--version"], capture_output=True, text=True, timeout=5)
            if result.returncode == 0:
                return p
        except (FileNotFoundError, subprocess.TimeoutExpired):
            continue

    print("ERROR: No D compiler (ldc2) found in PATH or common locations")
        sys.exit(1)


def check_file(filepath, include_dirs=None):
    """Check a single D file for syntax errors."""
    compiler = find_ldc2()

    if include_dirs is None:
        include_dirs = []

    # Build include flags
    flags = [compiler, "-c", "-o-", filepath]
    for inc in include_dirs:
        flags.extend(["-I", inc])

    result = subprocess.run(flags, capture_output=True, text=True, timeout=60)

    ok = result.returncode == 0
    status = "OK" if ok else "FAIL"
    print(f"  [{status}] {filepath}")

    if not ok:
        # Show first few error lines
        lines = result.stderr.strip().split("\n")
        for line in lines[:10]:
            print(f"         {line}")
        if len(lines) > 10:
            print(f"         ... and {len(lines) - 10} more lines")

    return ok


def check_all_in_dir(source_dir, include_dirs=None):
    """Check all .d files in a directory tree."""
    if include_dirs is None:
        include_dirs = []

    d_files = []
    for root, dirs, files in os.walk(source_dir):
        for f in sorted(files):
            if f.endswith(".d"):
                d_files.append(os.path.join(root, f))

    if not d_files:
        print(f"No .d files found in {source_dir}")
        return True

    print(f"Checking {len(d_files)} files in {source_dir}...")

    all_ok = True
    for f in d_files:
        if not check_file(f, include_dirs):
            all_ok = False

    return all_ok


def main():
    parser = argparse.ArgumentParser(description="Check D source files for syntax errors")
    parser.add_argument("files", nargs="*", help="D source files to check")
    parser.add_argument("--all", action="store_true", help="Check all .d files in given directories")
    parser.add_argument("--project", metavar="DIR", help="Check all .d files in project source dir")
    parser.add_argument("-I", "--include", action="append", default=[], help="Add include directory")

    args = parser.parse_args()

    if not args.files and not args.project:
        parser.print_help()
        sys.exit(1)

    all_ok = True

    if args.project:
        src = os.path.join(args.project, "source")
        if os.path.isdir(src):
            if not check_all_in_dir(src, args.include):
                all_ok = False
        else:
            print(f"ERROR: {src} is not a directory")
            all_ok = False

    for f in args.files:
        if args.all and os.path.isdir(f):
            if not check_all_in_dir(f, args.include):
                all_ok = False
        elif os.path.isfile(f):
            if not check_file(f, args.include):
                all_ok = False
        else:
            print(f"ERROR: {f} not found")
            all_ok = False

    sys.exit(0 if all_ok else 1)


if __name__ == "__main__":
    main()
