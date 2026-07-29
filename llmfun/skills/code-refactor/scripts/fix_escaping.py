#!/usr/bin/env python3
"""
Fix string escaping issues in source files.
Handles cross-language escaping (e.g., writing D regex patterns from Python).

Usage:
    python3 fix_escaping.py <file> --pattern <search> --replace <replace> [--dry-run]
    python3 fix_escaping.py <file> --regex-fix [--language d|c|cpp] [--dry-run]

Examples:
    # Fix D regex escaping in skill.d
    python3 fix_escaping.py source/skill.d --regex-fix --language d

    # Replace specific pattern
    python3 fix_escaping.py file.d --pattern 'old_pattern' --replace 'new_pattern'

    # Dry run to see what would change
    python3 fix_escaping.py file.d --regex-fix --language d --dry-run
"""
import sys
import os
import re
import argparse


def fix_d_regex_escaping(content):
    """
    Fix common D regex escaping issues.

    D string literals require double-escaping for regex patterns:
    - Python: r'"([^"]*)"'
    - D source: "\"\\\"([^\\\"]*)\\\"\""
    - File bytes: " + \\" + ( + [^ + \\" + ] + * + ) + \\" + "

    This function fixes common escaping mistakes in D regex patterns.
    """
    fixes = 0

    # Fix over-escaped backslashes in regex patterns
    # Common mistake: too many backslashes
    patterns = [
        # Fix quadruple backslash to double backslash in regex strings
        (r'regex\(\"\\\\\\\\\"', r'regex("\"'),
        (r'\\\\\\\\\"([^\\\\\\\\\"]*)\\\\\\\\\"\"', r'\\\"([^\\\"]*)\\\"\"'),

        # Fix raw string issues
        (r"regex\(r\"\\\\\"([^\\\\\"]*)\\\\\"\"", r'regex(r"\\\"([^\\\"]*)\\\"")'),

        # Fix single quote escaping
        (r"\\\\\\\\'([^\\\\\\\\']*)\\\\\\\\'", r"\\\\'([^\\\\']*)\\\\'"),
    ]

    for search, replace in patterns:
        if re.search(search, content):
            content = re.sub(search, replace, content)
            fixes += 1

    return content, fixes


def fix_d_string_escaping(content):
    """
    Fix D string literal escaping using explicit character construction.

    When writing D source code from Python, use chr() to construct
    problematic characters:
    - chr(92) = backslash
    - chr(34) = double quote
    - chr(39) = single quote
    """
    # This is a template function - actual fixing depends on the specific issue
    # The key insight: use chr() in Python to avoid escaping hell
    return content, 0


def check_escaping(content, language="d"):
    """Check for common escaping issues."""
    issues = []

    if language == "d":
        # Check for over-escaped backslashes
        if r"\\\\" in content:
            issues.append("Possible over-escaped backslashes (\\\\\\\\)")

        # Check for raw string issues
        if r'r\"' in content and r'\\\"' in content:
            issues.append("Mixed raw and escaped strings")

        # Check regex patterns
        regex_matches = re.findall(r'regex\([^)]+\)', content)
        for match in regex_matches:
            bs_count = match.count('\\')
            if bs_count > 6:
                issues.append(f"Suspicious regex with {bs_count} backslashes: {match[:50]}...")

    return issues


def main():
    parser = argparse.ArgumentParser(
        description="Fix string escaping issues in source files",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Common escaping issues:
  - D regex patterns written from Python (triple-escaping)
  - Over-escaped backslashes
  - Mixed raw and escaped strings

Tip: When writing D source from Python, use chr() for problematic chars:
  BS = chr(92)  # backslash
  DQ = chr(34)  # double quote
  SQ = chr(39)  # single quote
"""
    )
    parser.add_argument("file", help="Source file to fix")
    parser.add_argument("--pattern", help="Search pattern to replace")
    parser.add_argument("--replace", help="Replacement string")
    parser.add_argument("--regex-fix", action="store_true", help="Fix regex escaping issues")
    parser.add_argument("--language", choices=["d", "c", "cpp"], default="d", help="Source language")
    parser.add_argument("--dry-run", action="store_true", help="Show changes without writing")
    parser.add_argument("--check", action="store_true", help="Only check for issues")

    args = parser.parse_args()

    if not os.path.isfile(args.file):
        print(f"ERROR: {args.file} not found")
        sys.exit(1)

    with open(args.file) as f:
        content = f.read()

    if args.check:
        issues = check_escaping(content, args.language)
        if issues:
            print(f"ISSUES FOUND ({len(issues)}):")
            for issue in issues:
                print(f"  - {issue}")
            sys.exit(1)
        else:
            print("OK: No escaping issues detected")
            sys.exit(0)

    original = content
    fixes = 0

    if args.pattern and args.replace:
        count = content.count(args.pattern)
        content = content.replace(args.pattern, args.replace)
        fixes = count
        print(f"Replaced {count} occurrences of pattern")
    elif args.regex_fix:
        content, fixes = fix_d_regex_escaping(content)
        if fixes:
            print(f"Applied {fixes} regex escaping fixes")
        else:
            print("No regex escaping issues found")

    if content != original:
        if args.dry_run:
            print("\nDRY RUN - changes not written")
            print(f"File would change from {len(original)} to {len(content)} bytes")
        else:
            with open(args.file, "w") as f:
                f.write(content)
            print(f"File updated: {len(original)} -> {len(content)} bytes")
    else:
        print("No changes needed")

    sys.exit(0)


if __name__ == "__main__":
    main()
