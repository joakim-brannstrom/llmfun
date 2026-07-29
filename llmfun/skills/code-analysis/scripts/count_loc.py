#!/usr/bin/env python3
"""
Count lines of code in source files.
Produces a per-file breakdown and total.

Usage:
    python3 count_loc.py [source_dir] [--json] [--ext .c,.h,.d]
    python3 count_loc.py [source_dir] --lang d
    python3 count_loc.py [source_dir] --lang cpp

Examples:
    # Count all source files
    python3 count_loc.py source/

    # Count D source files only
    python3 count_loc.py source/ --lang d

    # Count C++ source files only
    python3 count_loc.py source/ --lang cpp

    # Output as JSON
    python3 count_loc.py source/ --json

    # Custom extensions
    python3 count_loc.py source/ --ext .d,.di

    # With brace analysis
    python3 count_loc.py source/ --braces
"""
import os
import sys
import json
import argparse

# Predefined language extensions
LANG_EXTENSIONS = {
    "d": (".d", ".di"),
    "cpp": (".cpp", ".hpp", ".cxx", ".hxx", ".cc", ".hh", ".h"),
    "c": (".c", ".h"),
    "all": (".c", ".h", ".cpp", ".hpp", ".d", ".di"),
}


def count_files(base_dir, extensions=(".c", ".h", ".d")):
    """Count lines in all files with given extensions."""
    files = []
    total = 0

    for root, dirs, fnames in os.walk(base_dir):
        for f in sorted(fnames):
            if any(f.endswith(ext) for ext in extensions):
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


def analyze_braces(base_dir, extensions=(".d",)):
    """Analyze brace balance in files."""
    results = []

    for root, dirs, fnames in os.walk(base_dir):
        for f in sorted(fnames):
            if any(f.endswith(ext) for ext in extensions):
                path = os.path.join(root, f)
                try:
                    with open(path) as fh:
                        content = fh.read()
                    opens = content.count("{")
                    closes = content.count("}")
                    balanced = opens == closes
                    results.append({
                        "path": path,
                        "opens": opens,
                        "closes": closes,
                        "balanced": balanced,
                    })
                except (OSError, IOError) as e:
                    print(f"WARNING: Could not read {path}: {e}", file=sys.stderr)

    return results


def main():
    parser = argparse.ArgumentParser(description="Count lines in source files")
    parser.add_argument("dir", nargs="?", default=".", help="Source directory (default: current)")
    parser.add_argument("--json", action="store_true", help="Output as JSON")
    parser.add_argument("--ext", default=None, help="Comma-separated extensions (default: .c,.h,.d)")
    parser.add_argument("--lang", choices=list(LANG_EXTENSIONS.keys()), help="Language preset (d, cpp, c, all)")
    parser.add_argument("--braces", action="store_true", help="Include brace analysis")

    args = parser.parse_args()

    # Determine extensions
    if args.lang:
        extensions = LANG_EXTENSIONS[args.lang]
    elif args.ext:
        extensions = tuple("." + e.strip(".") for e in args.ext.split(","))
    else:
        extensions = (".c", ".h", ".d")

    if not os.path.isdir(args.dir):
        print(f"ERROR: {args.dir} is not a directory")
        sys.exit(1)

    files, total = count_files(args.dir, extensions)

    if args.braces:
        brace_results = analyze_braces(args.dir, extensions)
        unbalanced = [r for r in brace_results if not r["balanced"]]
    else:
        brace_results = []
        unbalanced = []

    if args.json:
        result = {
            "directory": args.dir,
            "total_lines": total,
            "file_count": len(files),
            "files": files,
        }
        if args.braces:
            result["braces"] = {
                "total_files": len(brace_results),
                "unbalanced": len(unbalanced),
                "files": brace_results,
            }
        print(json.dumps(result, indent=2))
    else:
        print(f"Directory: {args.dir}")
        print(f"Extensions: {','.join(extensions)}")
        print(f"Files:     {len(files)}")
        print(f"Total LOC: {total}")
        print()

        # Show top 20 files by size
        for f in files[:20]:
            print(f"  {f['lines']:>6}  {f['path']}")

        if len(files) > 20:
            print(f"  ... and {len(files) - 20} more files")

        print()

        # Show brace analysis if requested
        if args.braces:
            print(f"--- Brace Analysis ---")
            print(f"Files checked: {len(brace_results)}")
            print(f"Unbalanced:    {len(unbalanced)}")
            if unbalanced:
                print()
                print("Unbalanced files:")
                for r in unbalanced:
                    status = "OK" if r["balanced"] else "UNBALANCED"
                    print(f"  [{status}] {r['opens']}{{ {r['closes']}}}  {r['path']}")
            print()

        # Show all files
        print("--- All files ---")
        for f in files:
            print(f"  {f['lines']:>6}  {f['path']}")

        print()

        # Show top 20 files by size
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
