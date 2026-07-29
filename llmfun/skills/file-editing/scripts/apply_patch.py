#!/usr/bin/env python3
"""
Apply patches to source files.
Supports simple text patches with markers and context.

Usage:
    python3 apply_patch.py <file> --patch <patch_file> [--dry-run]
    python3 apply_patch.py <file> --marker <marker> --add <code_file>

Examples:
    # Apply patch file
    python3 apply_patch.py source/app.d --patch debug_patch.patch

    # Add code before marker
    python3 apply_patch.py source/app.d --marker "// Commands" --add debug_cmd.d

    # Add code after marker
    python3 apply_patch.py source/app.d --marker "// Commands" --add debug_cmd.d --after

    # Dry run to see what would change
    python3 apply_patch.py source/app.d --patch debug_patch.patch --dry-run
"""
import sys
import os
import argparse


def apply_simple_patch(content, patch_content):
    """
    Apply a simple text patch.

    Patch format:
    --- old marker
    +++ new marker
    @@ -start,count +start,count @@
    - removed lines
    + added lines
     context lines
    """
    lines = content.split("\n")
    patch_lines = patch_content.split("\n")

    # Parse patch
    hunk_start = None
    hunk_old_count = 0
    hunk_new_count = 0
    hunk_lines = []

    for line in patch_lines:
        if line.startswith("@@"):
            # Parse hunk header
            parts = line.split()
            if len(parts) >= 2:
                old_range = parts[1].lstrip("-").split(",")
                new_range = parts[2].lstrip("+").split(",")
                hunk_start = int(old_range[0])
                hunk_old_count = int(old_range[1]) if len(old_range) > 1 else 1
                hunk_new_count = int(new_range[1]) if len(new_range) > 1 else 1
        elif line.startswith("-"):
            hunk_lines.append(("remove", line[1:]))
        elif line.startswith("+"):
            hunk_lines.append(("add", line[1:]))
        else:
            hunk_lines.append(("context", line))

    if hunk_start is None:
        return None, 0

    # Apply hunk
    new_lines = []
    content_idx = hunk_start - 1  # 0-based
    hunk_idx = 0

    # Copy lines before hunk
    new_lines.extend(lines[:content_idx])

    # Apply hunk
    while hunk_idx < len(hunk_lines) and content_idx < len(lines):
        op, text = hunk_lines[hunk_idx]

        if op == "remove":
            content_idx += 1
            hunk_idx += 1
        elif op == "add":
            new_lines.append(text)
            hunk_idx += 1
        elif op == "context":
            if text == lines[content_idx]:
                new_lines.append(text)
                content_idx += 1
                hunk_idx += 1
            else:
                print(f"WARNING: Context mismatch at line {content_idx + 1}")
                return None, 0

    # Copy remaining lines
    new_lines.extend(lines[content_idx:])

    return "\n".join(new_lines), hunk_new_count


def add_at_marker(content, marker, code, after=False):
    """Add code at marker location."""
    lines = content.split("\n")
    code_lines = code.split("\n")

    # Find marker
    marker_line = None
    for i, line in enumerate(lines):
        if marker in line:
            marker_line = i
            break

    if marker_line is None:
        return None, 0

    # Insert code
    insert_at = marker_line + 1 if after else marker_line
    new_lines = lines[:insert_at] + code_lines + lines[insert_at:]

    return "\n".join(new_lines), len(code_lines)


def main():
    parser = argparse.ArgumentParser(
        description="Apply patches to source files",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Patch modes:
  --patch <file>           Apply unified diff patch
  --marker <text> --add <file>  Add code at marker

Examples:
  %(prog)s file.d --patch patch.patch
  %(prog)s file.d --marker "// Commands" --add cmd.d
  %(prog)s file.d --marker "// Commands" --add cmd.d --after
  %(prog)s file.d --patch patch.patch --dry-run
"""
    )
    parser.add_argument("file", help="Source file to modify")
    parser.add_argument("--patch", help="Patch file to apply")
    parser.add_argument("--marker", help="Marker to find in file")
    parser.add_argument("--add", help="Code file to add at marker")
    parser.add_argument("--after", action="store_true", help="Add after marker (default: before)")
    parser.add_argument("--dry-run", action="store_true", help="Show changes without writing")

    args = parser.parse_args()

    # Validate arguments
    if not (args.patch or (args.marker and args.add)):
        print("ERROR: Specify --patch or --marker/--add")
        sys.exit(1)

    if not os.path.isfile(args.file):
        print(f"ERROR: File not found: {args.file}")
        sys.exit(1)

    with open(args.file) as f:
        content = f.read()

    if args.patch:
        if not os.path.isfile(args.patch):
            print(f"ERROR: Patch file not found: {args.patch}")
            sys.exit(1)

        with open(args.patch) as f:
            patch_content = f.read()

        new_content, lines_added = apply_simple_patch(content, patch_content)

        if new_content is None:
            print("ERROR: Failed to apply patch")
            sys.exit(1)

        print(f"Applied patch: {lines_added} lines added")

    elif args.marker and args.add:
        if not os.path.isfile(args.add):
            print(f"ERROR: Code file not found: {args.add}")
            sys.exit(1)

        with open(args.add) as f:
            code = f.read()

        new_content, lines_added = add_at_marker(content, args.marker, code, args.after)

        if new_content is None:
            print(f"ERROR: Marker not found: {args.marker}")
            sys.exit(1)

        position = "after" if args.after else "before"
        print(f"Added {lines_added} lines {position} marker")

    if new_content != content:
        if args.dry_run:
            print("\nDRY RUN - changes not written")
            print(f"File would change from {len(content)} to {len(new_content)} bytes")
        else:
            with open(args.file, "w") as f:
                f.write(new_content)
            print(f"File updated: {len(content)} -> {len(new_content)} bytes")
    else:
        print("No changes needed")

    sys.exit(0)


if __name__ == "__main__":
    main()
