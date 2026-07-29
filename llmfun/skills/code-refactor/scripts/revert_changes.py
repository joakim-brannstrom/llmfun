#!/usr/bin/env python3
"""
Revert changes in source files using backup files.
Supports multiple backup versions and selective revert.

Usage:
    python3 revert_changes.py <file> [--backup <backup_file>] [--line-range N-M]
    python3 revert_changes.py <file> --auto [--line-range N-M]

Examples:
    # Revert from auto-generated backup
    python3 revert_changes.py source/app.d --auto

    # Revert from specific backup
    python3 revert_changes.py source/app.d --backup source/app.d.bak

    # Revert specific line range from backup
    python3 revert_changes.py source/app.d --auto --line-range 100-200

    # List available backups
    python3 revert_changes.py source/app.d --list-backups
"""
import sys
import os
import glob
import argparse
from datetime import datetime


def find_backups(file_path):
    """Find backup files for a given file."""
    backups = []
    base = file_path

    # Common backup patterns
    patterns = [
        f"{base}.bak",
        f"{base}.bak.*",
        f"{base}.orig",
        f"{base}.backup",
        f"{base}.backup.*",
        f"{base}~",
    ]

    for pattern in patterns:
        matches = glob.glob(pattern)
        for m in matches:
            if os.path.isfile(m):
                stat = os.stat(m)
                backups.append({
                    "path": m,
                    "size": stat.st_size,
                    "mtime": datetime.fromtimestamp(stat.st_mtime).isoformat(),
                })

    # Sort by modification time (newest first)
    backups.sort(key=lambda x: x["mtime"], reverse=True)
    return backups


def revert_file(file_path, backup_path, line_range=None):
    """Revert file from backup, optionally for specific line range."""
    if not os.path.isfile(backup_path):
        print(f"ERROR: Backup file not found: {backup_path}")
        return False

    with open(backup_path) as f:
        backup_content = f.read()

    if line_range:
        # Revert specific line range
        if not os.path.isfile(file_path):
            print(f"ERROR: File not found: {file_path}")
            return False

        with open(file_path) as f:
            current_lines = f.readlines()

        start, end = line_range
        backup_lines = backup_content.split("\n")

        # Replace lines in range
        new_lines = current_lines[:start - 1] + backup_lines[start - 1:end] + current_lines[end:]

        with open(file_path, "w") as f:
            f.writelines(new_lines)

        print(f"Reverted lines {start}-{end} from {backup_path}")
    else:
        # Full revert
        with open(file_path, "w") as f:
            f.write(backup_content)

        print(f"Reverted {file_path} from {backup_path}")

    return True


def create_backup(file_path):
    """Create a backup of the current file."""
    if not os.path.isfile(file_path):
        print(f"ERROR: File not found: {file_path}")
        return None

    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    backup_path = f"{file_path}.bak.{timestamp}"

    with open(file_path) as f:
        content = f.read()

    with open(backup_path, "w") as f:
        f.write(content)

    print(f"Created backup: {backup_path}")
    return backup_path


def main():
    parser = argparse.ArgumentParser(
        description="Revert changes in source files using backup files",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Backup patterns searched:
  <file>.bak, <file>.bak.*, <file>.orig, <file>.backup, <file>.backup.*, <file>~

Examples:
  %(prog)s file.d --auto
  %(prog)s file.d --backup file.d.bak
  %(prog)s file.d --auto --line-range 100-200
  %(prog)s file.d --list-backups
  %(prog)s file.d --create-backup
"""
    )
    parser.add_argument("file", help="Source file to revert")
    parser.add_argument("--auto", action="store_true", help="Use newest backup automatically")
    parser.add_argument("--backup", help="Specific backup file to use")
    parser.add_argument("--line-range", help="Line range to revert (N-M)")
    parser.add_argument("--list-backups", action="store_true", help="List available backups")
    parser.add_argument("--create-backup", action="store_true", help="Create backup before reverting")

    args = parser.parse_args()

    if not os.path.isfile(args.file):
        print(f"ERROR: File not found: {args.file}")
        sys.exit(1)

    # Parse line range
    line_range = None
    if args.line_range:
        try:
            start, end = map(int, args.line_range.split("-"))
            line_range = (start, end)
        except ValueError:
            print("ERROR: Invalid line range format. Use N-M (e.g., 100-200)")
            sys.exit(1)

    # List backups
    if args.list_backups:
        backups = find_backups(args.file)
        if backups:
            print(f"Available backups for {args.file}:")
            for b in backups:
                print(f"  {b['path']} ({b['size']} bytes, {b['mtime']})")
        else:
            print(f"No backups found for {args.file}")
        sys.exit(0)

    # Create backup
    if args.create_backup:
        create_backup(args.file)
        sys.exit(0)

    # Determine backup file
    backup_path = args.backup
    if args.auto and not backup_path:
        backups = find_backups(args.file)
        if backups:
            backup_path = backups[0]["path"]
            print(f"Using newest backup: {backup_path}")
        else:
            print(f"ERROR: No backups found for {args.file}")
            sys.exit(1)

    if not backup_path:
        print("ERROR: Specify --auto or --backup <file>")
        sys.exit(1)

    # Revert
    if revert_file(args.file, backup_path, line_range):
        print("Revert successful")
        sys.exit(0)
    else:
        print("Revert failed")
        sys.exit(1)


if __name__ == "__main__":
    main()
