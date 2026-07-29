#!/usr/bin/env python3
"""
Compute and verify MD5 hashes of source files.
Used to detect unintended changes and track file integrity.

Usage:
    python3 file_integrity.py [source_dir] [--hashes file] [--json]
"""
import os
import sys
import json
import hashlib
import argparse


def compute_md5(filepath):
    """Compute MD5 hash of a file."""
    h = hashlib.md5()
    try:
        with open(filepath, "rb") as f:
            for chunk in iter(lambda: f.read(8192), b""):
                h.update(chunk)
        return h.hexdigest()
    except (OSError, IOError) as e:
        print(f"WARNING: Could not read {filepath}: {e}", file=sys.stderr)
        return None


def scan_files(base_dir, extensions=(".c", ".h", ".d", ".cpp", ".py")):
    """Scan directory and compute hashes for all source files."""
    results = {}
    for root, dirs, fnames in os.walk(base_dir):
        for f in sorted(fnames):
            if any(f.endswith(ext) for ext in extensions):
                path = os.path.join(root, f)
                md5 = compute_md5(path)
                if md5:
                    results[path] = md5
    return results


def verify_hashes(results, known_hashes):
    """Verify current hashes against known values."""
    changed = []
    missing = []
    new = []

    for path, md5 in known_hashes.items():
        if path not in results:
            missing.append(path)
        elif results[path] != md5:
            changed.append((path, known_hashes[path], results[path]))

    for path in results:
        if path not in known_hashes:
            new.append(path)

    return changed, missing, new


def main():
    parser = argparse.ArgumentParser(description="Compute/verify MD5 hashes of source files")
    parser.add_argument("dir", nargs="?", default=".", help="Source directory or file (default: current)")
    parser.add_argument("--hashes", metavar="FILE", help="File with known hashes (JSON)")
    parser.add_argument("--json", action="store_true", help="Output as JSON")
    parser.add_argument("--ext", default=None, help="Comma-separated extensions")
    parser.add_argument("--file", action="store_true", help="Treat input as single file")

    args = parser.parse_args()

    extensions = tuple("." + e.strip(".") for e in args.ext.split(",")) if args.ext else (".c", ".h", ".d", ".cpp", ".py")

    # Handle single file mode
    if args.file or os.path.isfile(args.dir):
        md5 = compute_md5(args.dir)
        if md5:
            if args.json:
                print(json.dumps({args.dir: md5}, indent=2))
            else:
                print(f"{md5}  {args.dir}")
        sys.exit(0)

    if not os.path.isdir(args.dir):
        print(f"ERROR: {args.dir} is not a directory")
        sys.exit(1)


    extensions = tuple("." + e.strip(".") for e in args.ext.split(",")) if args.ext else (".c", ".h", ".d", ".cpp", ".py")

    if not os.path.isdir(args.dir):
        print(f"ERROR: {args.dir} is not a directory")
        sys.exit(1)

    results = scan_files(args.dir, extensions)

    if args.hashes:
        # Verify against known hashes
        try:
            with open(args.hashes) as f:
                known = json.load(f)
        except (OSError, json.JSONDecodeError) as e:
            print(f"ERROR: Could not load {args.hashes}: {e}")
            sys.exit(1)

        changed, missing, new = verify_hashes(results, known)

        if args.json:
            output = {
                "changed": [{"path": p, "expected": e, "actual": a} for p, e, a in changed],
                "missing": missing,
                "new": new,
            }
            print(json.dumps(output, indent=2))
        else:
            if changed:
                print(f"CHANGED FILES ({len(changed)}):")
                for path, expected, actual in changed:
                    print(f"  {path}")
                    print(f"    Expected: {expected}")
                    print(f"    Actual:   {actual}")

            if missing:
                print(f"\nMISSING FILES ({len(missing)}):")
                for path in missing:
                    print(f"  {path}")

            if new:
                print(f"\nNEW FILES ({len(new)}):")
                for path in new:
                    print(f"  {path}")

            if not changed and not missing and not new:
                print("OK: All files match known hashes")

        sys.exit(1 if (changed or missing) else 0)
    else:
        # Just compute and display hashes
        if args.json:
            print(json.dumps(results, indent=2))
        else:
            print(f"Directory: {args.dir}")
            print(f"Files:     {len(results)}")
            print()
            for path, md5 in sorted(results.items()):
                print(f"  {md5}  {path}")

            print(f"Files:     {len(results)}")
            print()
            for path, md5 in sorted(results.items()):
                print(f"  {md5}  {path}")


if __name__ == "__main__":
    main()
