#!/usr/bin/env python3
"""
Count lines of code in D source files.
Produces a per-file breakdown and total.

Usage:
    python3 count_d_loc.py [source_dir] [--json]
"""
import os
import sys
import json
import argparse


def count_files(base_dir, extension=".d"):
    """Count lines in all files with given extension."""
    files = []
    total = 0

    for root, dirs, fnames in os.walk(base_dir):
        for f in sorted(fnames):
            if f.endswith(extension):
                path = os.path.join(root, f)
                try:
                    with open(path) as fh:
                        lines = sum(1 for _ in fh)
                    files.append({"path": path, "lines": lines})
                    total += lines
                except (OSError, IOError) as e:
                    print(f"WARNING: Could not read {path}: {e}", file=sys.stderr)

    # Sort by line count descending
    files.sort(key=lambda x: -x["lines"])
    return files, total


def main():
    parser = argparse.ArgumentParser(description="Count lines in D source files")
    parser.add_argument("dir", nargs="?", default="source", help="Source directory (default: source)")
    parser.add_argument("--json", action="store_true", help="Output as JSON")
    parser.add_argument("--ext", default=".d", help="File extension (default: .d)")

    args = parser.parse_args()

    if not os.path.isdir(args.dir):
        print(f"ERROR: {args.dir} is not a directory")
        sys.exit(1)

    files, total = count_files(args.dir, args.ext)

    if args.json:
        result = {
            "directory": args.dir,
            "total_lines": total,
            "file_count": len(files),
            "files": files,
        }
        print(json.dumps(result, indent=2))
    else:
        print(f"Directory: {args.dir}")
        print(f"Files:     {len(files)}")
        print(f"Total LOC: {total}")
        print()

        # Show top 20 files by size
        max_name_len = max((len(f["path"]) for f in files), default=20)
        for f in files[:20]:
            print(f"  {f['lines']:>6}  {f['path']}")

        if len(files) > 20:
            print(f"  ... and {len(files) - 20} more files")

        print()
        # Show all files
        print("--- All files ---")
        for f in files:
            print(f"  {f['lines']:>6}  {f['path']}")


if __name__ == "__main__":
    main()
