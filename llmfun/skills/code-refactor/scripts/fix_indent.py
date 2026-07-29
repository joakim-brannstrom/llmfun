#!/usr/bin/env python3
"""
Fix indentation issues in source files.
Handles space/tab inconsistencies and misaligned blocks.

Usage:
    python3 fix_indent.py <file> [--start N] [--end N] [--spaces N] [--dry-run]
    python3 fix_indent.py <file> --check [--start N] [--end N]

Examples:
    # Check indentation in line range
    python3 fix_indent.py source/app.d --check --start 100 --end 200

    # Fix extra spaces (remove 1 space from each line in range)
    python3 fix_indent.py source/app.d --start 100 --end 200 --remove-spaces 1

    # Convert tabs to spaces
    python3 fix_indent.py source/app.d --tabs-to-spaces 4

    # Dry run to see what would change
    python3 fix_indent.py source/app.d --start 100 --end 200 --dry-run
"""
import sys
import os
import argparse


def check_indentation(lines, start=None, end=None, indent_width=4):
    """Check indentation of lines and report issues."""
    issues = []

    for i, line in enumerate(lines):
        line_num = i + 1
        if start and line_num < start:
            continue
        if end and line_num > end:
            break

        # Skip empty lines
        if not line.strip():
            continue

        # Count leading whitespace
        leading = len(line) - len(line.lstrip())
        has_tabs = "\t" in line[:leading]
        spaces = line[:leading].count(" ")
        tabs = line[:leading].count("\t")

        # Check for mixed tabs and spaces
        if has_tabs and spaces > 0:
            issues.append((line_num, f"Mixed tabs ({tabs}) and spaces ({spaces})"))

        # Check for odd indentation (not multiple of indent_width)
        total_spaces = tabs * indent_width + spaces  # Assume tab = indent_width spaces
        if total_spaces % indent_width != 0:
            issues.append((line_num, f"Odd indentation: {total_spaces} spaces (not multiple of {indent_width})"))

    return issues

def fix_indentation(lines, start=None, end=None, remove_spaces=0, tabs_to_spaces=None):
    """Fix indentation issues in lines."""
    new_lines = list(lines)
    changes = 0

    for i, line in enumerate(lines):
        line_num = i + 1
        if start and line_num < start:
            continue
        if end and line_num > end:
            break

        # Skip empty lines
        if not line.strip():
            continue

        if remove_spaces > 0:
            # Remove N leading spaces from each line
            leading = len(line) - len(line.lstrip())
            if leading >= remove_spaces:
                new_lines[i] = line[remove_spaces:]
                changes += 1

        if tabs_to_spaces is not None:
            # Convert tabs to spaces
            leading = len(line) - len(line.lstrip())
            tabs = line[:leading].count("\t")
            if tabs > 0:
                spaces = " " * (tabs * tabs_to_spaces)
                rest = line[leading:]
                # Replace tabs with spaces in leading whitespace
                new_leading = line[:leading].replace("\t", " " * tabs_to_spaces)
                new_lines[i] = new_leading + rest
                changes += 1

    return new_lines, changes


def main():
    parser = argparse.ArgumentParser(
        description="Fix indentation issues in source files",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Common issues fixed:
  - Mixed tabs and spaces
  - Odd indentation (not multiple of N)
  - Extra leading spaces
  - Tab/space conversion

Examples:
  %(prog)s file.d --check --start 100 --end 200
  %(prog)s file.d --start 100 --end 200 --remove-spaces 1
  %(prog)s file.d --tabs-to-spaces 4
  %(prog)s source/ --recursive --check
  %(prog)s file.d --auto-fix
"""
    )
    parser.add_argument("file", help="Source file or directory to fix")
    parser.add_argument("--start", type=int, help="Start line number")
    parser.add_argument("--end", type=int, help="End line number")
    parser.add_argument("--check", action="store_true", help="Only check for issues")
    parser.add_argument("--remove-spaces", type=int, default=0, help="Remove N leading spaces")
    parser.add_argument("--tabs-to-spaces", type=int, help="Convert tabs to N spaces")
    parser.add_argument("--indent-width", type=int, default=4, help="Expected indent width (default: 4)")
    parser.add_argument("--recursive", action="store_true", help="Recursively scan directory")
    parser.add_argument("--ext", default=".d", help="File extension for recursive mode")
    parser.add_argument("--auto-fix", action="store_true", help="Auto-detect and fix common issues")
    parser.add_argument("--dry-run", action="store_true", help="Show changes without writing")

    args = parser.parse_args()

    # Handle directory mode
    if os.path.isdir(args.file):
        if not args.recursive:
            print("ERROR: Use --recursive for directory input")
            sys.exit(1)

        total_files = 0
        total_issues = 0
        ext = args.ext if args.ext.startswith(".") else f".{args.ext}"

        for root, dirs, fnames in os.walk(args.file):
            for f in sorted(fnames):
                if f.endswith(ext):
                    path = os.path.join(root, f)
                    total_files += 1

                    with open(path) as fh:
                        lines = fh.readlines()

                    issues = check_indentation(lines, indent_width=args.indent_width)
                    if issues:
                        total_issues += len(issues)
                        if args.check:
                            print(f"\n{path} ({len(issues)} issues):")
                            for line_num, msg in issues[:10]:
                                print(f"  Line {line_num}: {msg}")
                            if len(issues) > 10:
                                print(f"  ... and {len(issues) - 10} more")

        print(f"\nScanned {total_files} files, found {total_issues} issues")
        sys.exit(1 if total_issues else 0)

    if not os.path.isfile(args.file):
        print(f"ERROR: {args.file} not found")
        sys.exit(1)

    with open(args.file) as f:
        lines = f.readlines()

    if args.check:
        issues = check_indentation(lines, args.start, args.end, indent_width=args.indent_width)
        if issues:
            print(f"INDENTATION ISSUES ({len(issues)}):")
            for line_num, msg in issues:
                print(f"  Line {line_num}: {msg}")
            sys.exit(1)
        else:
            print("OK: No indentation issues detected")
            sys.exit(0)

    # Auto-fix mode
    if args.auto_fix:
        issues = check_indentation(lines, args.start, args.end, indent_width=args.indent_width)
        if not issues:
            print("No issues found")
            sys.exit(0)

        # Detect common patterns and fix
        has_tabs = any("\t" in line for line in lines)
        if has_tabs:
            print("Detected tabs, converting to spaces")
            lines, _ = fix_indentation(lines, args.start, args.end, tabs_to_spaces=args.indent_width)

    new_lines, changes = fix_indentation(
        lines, args.start, args.end,
        remove_spaces=args.remove_spaces,
        tabs_to_spaces=args.tabs_to_spaces
    )

    if changes == 0:
        print("No changes needed")
        sys.exit(0)

    print(f"Fixed {changes} lines")

    if args.dry_run:
        print("\nDRY RUN - changes not written")
    else:
        with open(args.file, "w") as f:
            f.writelines(new_lines)
        print(f"File updated")

    sys.exit(0)


if __name__ == "__main__":
    main()


if __name__ == "__main__":
    main()
