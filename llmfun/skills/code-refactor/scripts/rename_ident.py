#!/usr/bin/env python3
"""
Safely rename identifiers in source files.
Handles word boundaries, imports, comments, and strings.

Usage:
    python3 rename_ident.py <file> --from <old> --to <new> [--dry-run]
    python3 rename_ident.py <file> --from <old> --to <new> --skip-imports --dry-run

Examples:
    # Rename member variable
    python3 rename_ident.py source/app.d --from _conf --to conf_

    # Rename with word boundary check
    python3 rename_ident.py source/app.d --from agent --to agent_ --word-boundary

    # Skip import lines
    python3 rename_ident.py source/app.d --from foo --to bar --skip-imports

    # Dry run to see what would change
    python3 rename_ident.py source/app.d --from old --to new --dry-run
"""
import sys
import os
import re
import argparse


def rename_in_content(content, old, new, word_boundary=False, skip_imports=True, skip_comments=True):
    """
    Rename identifier in content with safety checks.

    Args:
        content: File content
        old: Old identifier name
        new: New identifier name
        word_boundary: Only match whole words
        skip_imports: Don't rename in import lines
        skip_comments: Don't rename in comments

    Returns:
        Tuple of (new_content, count_of_changes)
    """
    lines = content.split("\n")
    new_lines = []
    count = 0

    for line in lines:
        stripped = line.strip()

        # Skip import lines
        if skip_imports and stripped.startswith("import "):
            new_lines.append(line)
            continue

        # Skip pure comment lines
        if skip_comments and (stripped.startswith("//") or stripped.startswith("/*") or stripped.startswith("*")):
            new_lines.append(line)
            continue

        # Apply rename
        if word_boundary:
            # Use word boundary regex
            pattern = r'\b' + re.escape(old) + r'\b'
            matches = len(re.findall(pattern, line))
            line = re.sub(pattern, new, line)
            count += matches
        else:
            # Simple string replace
            matches = line.count(old)
            line = line.replace(old, new)
            count += matches

        new_lines.append(line)

    return "\n".join(new_lines), count


def check_conflicts(content, old, new):
    """Check for potential naming conflicts."""
    conflicts = []

    # Check if new name already exists as a different entity
    if new in content:
        # Check if it's used in imports
        for line in content.split("\n"):
            if line.strip().startswith("import ") and new in line:
                conflicts.append(f"New name '{new}' appears in import: {line.strip()}")

        # Check if it's a type name (capitalized)
        if new[0].isupper():
            conflicts.append(f"New name '{new}' starts with capital - might conflict with type")

    return conflicts


def rename_member_var(content, old_prefix="_", new_suffix="_", skip_imports=True, skip_comments=True):
    """
    Rename member variables by converting prefix to suffix pattern.

    Common pattern: _memberName -> memberName_

    Args:
        content: File content
        old_prefix: Prefix to remove (default: "_")
        new_suffix: Suffix to add (default: "_")
        skip_imports: Don't rename in import lines
        skip_comments: Don't rename in comments

    Returns:
        Tuple of (new_content, count_of_changes)
    """
    lines = content.split("\n")
    new_lines = []
    count = 0

    for line in lines:
        stripped = line.strip()

        # Skip import lines
        if skip_imports and stripped.startswith("import "):
            new_lines.append(line)
            continue

        # Skip pure comment lines
        if skip_comments and (stripped.startswith("//") or stripped.startswith("/*") or stripped.startswith("*")):
            new_lines.append(line)
            continue

        # Find member variable patterns: _identifier (not followed by another underscore)
        # Pattern: word boundary, underscore, identifier start, identifier chars
        pattern = r'\b_(' + re.escape(old_prefix).lstrip('_') + r')([A-Z][a-zA-Z0-9]*)\b'

        def replace_member(match):
            full = match.group(0)
            prefix = match.group(1)
            name = match.group(2)
            # Convert _Name to name_ (lowercase first char, add suffix)
            new_name = name[0].lower() + name[1:] + new_suffix
            return new_name

        new_line, n = re.subn(pattern, replace_member, line)
        count += n
        new_lines.append(new_line)

    return "\n".join(new_lines), count


def main():
    parser = argparse.ArgumentParser(
        description="Safely rename identifiers in source files",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Safety features:
  - Skips import lines by default
  - Skips comment lines by default
  - Word boundary matching prevents partial matches
  - Conflict detection warns about potential issues

Examples:
  %(prog)s file.d --from _oldName --to newName
  %(prog)s file.d --from agent --to agent_ --word-boundary
  %(prog)s file.d --from foo --to bar --dry-run
  %(prog)s file.d --member-var  # Convert _name to name_
"""
    )
    parser.add_argument("file", help="Source file to modify")
    parser.add_argument("--from", dest="old", help="Old identifier name")
    parser.add_argument("--to", dest="new", help="New identifier name")
    parser.add_argument("--member-var", action="store_true", help="Rename member vars (_name -> name_)")
    parser.add_argument("--word-boundary", action="store_true", help="Only match whole words")
    parser.add_argument("--skip-imports", action="store_true", default=True, help="Skip import lines (default)")
    parser.add_argument("--no-skip-imports", action="store_true", help="Don't skip import lines")
    parser.add_argument("--skip-comments", action="store_true", default=True, help="Skip comments (default)")
    parser.add_argument("--no-skip-comments", action="store_true", help="Don't skip comments")
    parser.add_argument("--dry-run", action="store_true", help="Show changes without writing")
    parser.add_argument("--check-conflicts", action="store_true", help="Check for naming conflicts")

    args = parser.parse_args()

    # Validate arguments
    if not args.member_var and not (args.old and args.new):
        print("ERROR: Specify --from/--to or --member-var")
        sys.exit(1)

    if not os.path.isfile(args.file):
        print(f"ERROR: {args.file} not found")
        sys.exit(1)

    with open(args.file) as f:
        content = f.read()

    skip_imports = not args.no_skip_imports
    skip_comments = not args.no_skip_comments

    # Check for conflicts
    if args.check_conflicts and args.old and args.new:
        conflicts = check_conflicts(content, args.old, args.new)
        if conflicts:
            print("POTENTIAL CONFLICTS:")
            for c in conflicts:
                print(f"  - {c}")
            sys.exit(1)
        else:
            print("OK: No naming conflicts detected")

    # Apply renaming
    if args.member_var:
        new_content, count = rename_member_var(
            content, skip_imports=skip_imports, skip_comments=skip_comments
        )
        print(f"Renamed {count} member variables (_name -> name_)")
    else:
        new_content, count = rename_in_content(
            content, args.old, args.new,
            word_boundary=args.word_boundary,
            skip_imports=skip_imports,
            skip_comments=skip_comments
        )
        print(f"Found {count} occurrences of '{args.old}'")

    if count == 0:
        print("No changes made")
        sys.exit(0)

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


    skip_imports = not args.no_skip_imports
    skip_comments = not args.no_skip_comments

    new_content, count = rename_in_content(
        content, args.old, args.new,
        word_boundary=args.word_boundary,
        skip_imports=skip_imports,
        skip_comments=skip_comments
    )

    if count == 0:
        print(f"No occurrences of '{args.old}' found")
        sys.exit(0)

    print(f"Found {count} occurrences of '{args.old}'")

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
