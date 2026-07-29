#!/usr/bin/env python3
"""
Swap/replace code blocks in source files.
Replaces old code with new code, supporting line ranges and markers.

Usage:
    python3 swap_code.py <file> --old <old_code_file> --new <new_code_file>
    python3 swap_code.py <file> --start N --end M --new <new_code_file> [--dry-run]

Examples:
    # Replace old code with new code
    python3 swap_code.py source/app.d --old consolidation.d --new consolidation_fixed.d

    # Replace lines 100-200 with new code
    python3 swap_code.py source/app.d --start 100 --end 200 --new new_code.d

    # Dry run to see what would change
    python3 swap_code.py source/app.d --old old.d --new new.d --dry-run
"""
import sys
import os
import argparse


def find_code_block(content, code):
    """Find the line range where code block appears."""
    content_lines = content.split("\n")
    code_lines = code.split("\n")

    if not code_lines:
        return None, None

    # Search for the first line of the code block
    first_line = code_lines[0].strip()
    if not first_line:
        return None, None

    for i, line in enumerate(content_lines):
        if first_line in line:
            # Verify the entire block matches
            match = True
            for j, code_line in enumerate(code_lines[1:], 1):
                if i + j >= len(content_lines):
                    match = False
                    break
                if code_line.strip() and code_line.strip() not in content_lines[i + j]:
                    match = False
                    break

            if match:
                return i + 1, i + len(code_lines)  # 1-based

    return None, None


def swap_code(content, old_code, new_code, start_line=None, end_line=None):
    """
    Replace old code with new code.

    Args:
        content: Original file content
        old_code: Code to replace (None if using line range)
        new_code: New code to insert
        start_line: Start line of code to replace (1-based)
        end_line: End line of code to replace (1-based)

    Returns:
        Tuple of (new_content, lines_replaced)
    """
    lines = content.split("\n")

    # Determine replacement range
    if start_line and end_line:
        replace_start = start_line - 1  # 0-based
        replace_end = end_line
    elif old_code:
        replace_start, replace_end = find_code_block(content, old_code)
        if replace_start is None:
            return None, 0
        replace_end -= 1  # Convert to exclusive end
    else:
        return None, 0

    # Replace code
    new_lines = new_code.split("\n")
    new_lines_list = lines[:replace_start] + new_lines + lines[replace_end:]

    return "\n".join(new_lines_list), replace_end - replace_start


def main():
    parser = argparse.ArgumentParser(
        description="Swap/replace code blocks in source files",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Replacement modes:
  --old <file> --new <file>  Replace old code with new code
  --start N --end M --new <file>  Replace line range with new code

Examples:
  %(prog)s file.d --old old.d --new new.d
  %(prog)s file.d --start 100 --end 200 --new new.d
  %(prog)s file.d --old old.d --new new.d --dry-run
"""
    )
    parser.add_argument("file", help="Source file to modify")
    parser.add_argument("--old", help="File containing old code to replace")
    parser.add_argument("--new", required=True, help="File containing new code")
    parser.add_argument("--start", type=int, help="Start line of code to replace")
    parser.add_argument("--end", type=int, help="End line of code to replace")
    parser.add_argument("--dry-run", action="store_true", help="Show changes without writing")

    args = parser.parse_args()

    # Validate arguments
    if not (args.old or (args.start and args.end)):
        print("ERROR: Specify --old or --start/--end")
        sys.exit(1)

    if not os.path.isfile(args.file):
        print(f"ERROR: File not found: {args.file}")
        sys.exit(1)

    if not os.path.isfile(args.new):
        print(f"ERROR: New code file not found: {args.new}")
        sys.exit(1)

    with open(args.file) as f:
        content = f.read()

    with open(args.new) as f:
        new_code = f.read()

    old_code = None
    if args.old:
        if not os.path.isfile(args.old):
            print(f"ERROR: Old code file not found: {args.old}")
            sys.exit(1)
        with open(args.old) as f:
            old_code = f.read()

    # Swap code
    new_content, lines_replaced = swap_code(
        content, old_code, new_code,
        start_line=args.start,
        end_line=args.end
    )

    if new_content is None:
        if args.old:
            print(f"ERROR: Old code not found in file")
        else:
            print(f"ERROR: Invalid line range")
        sys.exit(1)

    print(f"Replaced {lines_replaced} lines with {len(new_code.split(chr(10)))} lines")

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
