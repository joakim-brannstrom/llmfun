#!/usr/bin/env python3
"""
Check brace balance in source files.
Reports unbalanced braces with line numbers and context.

Usage:
    python3 check_braces.py <file> [--simple] [--depth] [--stack] [--start N] [--end N]
"""
import sys
import os
import argparse


def check_simple(filepath):
    """Simple brace count (ignores strings and comments)."""
    with open(filepath) as f:
        content = f.read()

    opens = content.count("{")
    closes = content.count("}")
    balance = opens - closes

    print(f"File: {filepath}")
    print(f"  Open braces:  {opens}")
    print(f"  Close braces: {closes}")
    print(f"  Balance:      {balance}")

    if balance == 0:
        print("  Status: BALANCED")
    elif balance > 0:
        print(f"  Status: UNBALANCED ({balance} unclosed opening braces)")
    else:
        print(f"  Status: UNBALANCED ({abs(balance)} extra closing braces)")

    return balance == 0


def check_with_depth(filepath, start_line=None, end_line=None):
    """Track brace depth line by line."""
    with open(filepath) as f:
        lines = f.readlines()

    depth = 0
    max_depth = 0
    min_depth = 0
    errors = []

    for i, line in enumerate(lines, 1):
        if start_line and i < start_line:
            continue
        if end_line and i > end_line:
            break

        for ch in line:
            if ch == "{":
                depth += 1
                max_depth = max(max_depth, depth)
            elif ch == "}":
                depth -= 1
                min_depth = min(min_depth, depth)
                if depth < 0:
                    errors.append((i, "Unmatched closing brace '}'", line.rstrip()))

    if depth > 0:
        errors.append((len(lines), f"Unclosed opening braces (depth={depth})", ""))

    print(f"File: {filepath}")
    print(f"  Total lines: {len(lines)}")
    print(f"  Max depth:   {max_depth}")
    print(f"  Min depth:   {min_depth}")
    print(f"  Final depth: {depth}")

    if errors:
        print(f"\n  ERRORS ({len(errors)}):")
        for line_num, msg, context in errors:
            print(f"    Line {line_num}: {msg}")
            if context:
                print(f"      {context}")
    else:
        print("  Status: BALANCED")

    return len(errors) == 0


def check_with_stack(filepath):
    """Stack-based brace matching with full context."""
    with open(filepath) as f:
        lines = f.readlines()

    stack = []
    errors = []

    for i, line in enumerate(lines, 1):
        for ch in line:
            if ch == "{":
                stack.append(i)
            elif ch == "}":
                if not stack:
                    errors.append((i, "Unmatched closing brace '}'", line.rstrip()))
                else:
                    stack.pop()

    # Remaining items in stack are unclosed opening braces
    for line_num in stack:
        errors.append((line_num, "Unclosed opening brace '{'", lines[line_num - 1].rstrip()))

    print(f"File: {filepath}")
    print(f"  Total lines: {len(lines)}")

    if errors:
        print(f"\n  ERRORS ({len(errors)}):")
        for line_num, msg, context in sorted(errors):
            print(f"\n    Line {line_num}: {msg}")
            # Show context (3 lines before, 2 after)
            start = max(0, line_num - 3)
            end = min(len(lines), line_num + 2)
            for j in range(start, end):
                marker = ">>>" if j == line_num - 1 else "   "
                print(f"    {marker} {j+1}: {lines[j].rstrip()}")
    else:
        print("  Status: ALL BRACES MATCH")

    return len(errors) == 0


def main():
    parser = argparse.ArgumentParser(
        description="Check brace balance in source files",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  %(prog)s file.c                   Run all checks
  %(prog)s file.c --simple          Quick count only
  %(prog)s file.c --depth           Track depth line by line
  %(prog)s file.c --stack           Full context for errors
  %(prog)s file.c --depth --start 100 --end 200  Check line range
"""
    )
    parser.add_argument("file", help="Source file to check")
    parser.add_argument("--simple", action="store_true", help="Simple count only")
    parser.add_argument("--depth", action="store_true", help="Track depth line by line")
    parser.add_argument("--stack", action="store_true", help="Stack-based matching with context")
    parser.add_argument("--start", type=int, help="Start line for depth mode")
    parser.add_argument("--end", type=int, help="End line for depth mode")

    args = parser.parse_args()

    if not os.path.isfile(args.file):
        print(f"ERROR: {args.file} not found")
        sys.exit(1)

    if args.simple:
        ok = check_simple(args.file)
    elif args.depth:
        ok = check_with_depth(args.file, args.start, args.end)
    elif args.stack:
        ok = check_with_stack(args.file)
    else:
        # Default: run all checks
        ok1 = check_simple(args.file)
        print()
        ok2 = check_with_depth(args.file)

        if not ok1 or not ok2:
            print("\nRunning detailed stack check...")
            print()
            check_with_stack(args.file)

    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
