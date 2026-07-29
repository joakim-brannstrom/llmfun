#!/usr/bin/env python3
"""
Insert code blocks at specific locations in source files.
Supports inserting before/after functions, line numbers, or markers.

Usage:
    python3 insert_code.py <file> --before <marker> --code <code_file>
    python3 insert_code.py <file> --after <marker> --code <code_file>
    python3 insert_code.py <file> --line N --code <code_file> [--dry-run]

Examples:
    # Insert before function appMain
    python3 insert_code.py source/app.d --before "void appMain" --code consolidation.d

    # Insert after line 100
    python3 insert_code.py source/app.d --line 100 --code new_code.d

    # Insert before marker string
    python3 insert_code.py source/app.d --before "// TODO: add feature" --code feature.d

    # Dry run to see what would change
    python3 insert_code.py source/app.d --before "void appMain" --code consolidation.d --dry-run
"""
import sys
import os
import argparse


def find_marker_line(content, marker, search_from=0):
    """Find the line number containing the marker string."""
    lines = content.split("\n")
    for i in range(search_from, len(lines)):
        if marker in lines[i]:
            return i + 1  # 1-based line number
    return None


def insert_code(content, code, before_marker=None, after_marker=None, line_num=None, before=True):
    """
    Insert code at specified location.

    Args:
        content: Original file content
        code: Code to insert
        before_marker: Insert before line containing this marker
        after_marker: Insert after line containing this marker
        line_num: Insert at this line number
        before: If True, insert before marker; if False, insert after

    Returns:
        Tuple of (new_content, insert_line)
    """
    lines = content.split("\n")

    # Determine insertion point
    insert_at = None

    if before_marker:
        insert_at = find_marker_line(content, before_marker)
        if insert_at is None:
            return None, None
        if not before:
            insert_at += 1

    elif after_marker:
        insert_at = find_marker_line(content, after_marker)
        if insert_at is None:
            return None, None
        insert_at += 1

    elif line_num:
        insert_at = line_num

    else:
        return None, None

    # Insert code
    code_lines = code.split("\n")
    new_lines = lines[:insert_at - 1] + code_lines + lines[insert_at - 1:]

    return "\n".join(new_lines), insert_at


def main():
    parser = argparse.ArgumentParser(
        description="Insert code blocks at specific locations in source files",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Insertion modes:
  --before <marker>  Insert before line containing marker
  --after <marker>   Insert after line containing marker
  --line N           Insert at line number N

Examples:
  %(prog)s file.d --before "void appMain" --code new.d
  %(prog)s file.d --after "// TODO" --code feature.d
  %(prog)s file.d --line 100 --code new.d --dry-run
"""
    )
    parser.add_argument("file", help="Source file to modify")
    parser.add_argument("--before", help="Insert before line containing this marker")
    parser.add_argument("--after", help="Insert after line containing this marker")
    parser.add_argument("--line", type=int, help="Insert at this line number")
    parser.add_argument("--code", required=True, help="File containing code to insert")
    parser.add_argument("--dry-run", action="store_true", help="Show changes without writing")

    args = parser.parse_args()

    # Validate arguments
    if not (args.before or args.after or args.line):
        print("ERROR: Specify --before, --after, or --line")
        sys.exit(1)

    if not os.path.isfile(args.file):
        print(f"ERROR: File not found: {args.file}")
        sys.exit(1)

    if not os.path.isfile(args.code):
        print(f"ERROR: Code file not found: {args.code}")
        sys.exit(1)

    with open(args.file) as f:
        content = f.read()

    with open(args.code) as f:
        code = f.read()

    # Insert code
    new_content, insert_line = insert_code(
        content, code,
        before_marker=args.before,
        after_marker=args.after,
        line_num=args.line
    )

    if new_content is None:
        marker = args.before or args.after or f"line {args.line}"
        print(f"ERROR: Marker not found: {marker}")
        sys.exit(1)

    print(f"Inserted {len(code.split(chr(10)))} lines at line {insert_line}")

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
