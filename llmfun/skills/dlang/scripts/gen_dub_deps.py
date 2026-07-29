#!/usr/bin/env python3
"""
Generate dub.sdl dependency entries for C/C++ libraries.
Helps configure D projects to link against system libraries.

Usage:
    python3 gen_dub_deps.py <libname> [--lib-path PATH] [--include-path PATH]
    python3 gen_dub_deps.py --list-known

Examples:
    # Generate entry for mylib
    python3 gen_dub_deps.py mylib

    # With custom paths
    python3 gen_dub_deps.py mylib --lib-path ./vendor/mylib --include-path ./vendor/mylib/include

    # Generate for multiple libraries
    python3 gen_dub_deps.py mylib miniorm sqlite3

    # List known libraries
    python3 gen_dub_deps.py --list-known
"""
import sys
import os
import argparse

# Known libraries with default dub.sdl entries
KNOWN_DEPS = {
    "mylib": {
        "description": "Custom C utility library",
        "default_entry": '''# mylib dependency
importPaths "vendor/mylib/include"
libs "mylib"
''',
    },
    "miniorm": {
        "description": "Minimal ORM library",
        "default_entry": '''# miniorm dependency
importPaths "vendor/miniorm/include"
libs "miniorm"
''',
    },
    "sqlite3": {
        "description": "SQLite database library",
        "default_entry": '''# sqlite3 dependency
libs "sqlite3"
''',
    },
    "curl": {
        "description": "HTTP client library",
        "default_entry": '''# curl dependency
libs "curl"
''',
    },
    "zlib": {
        "description": "Compression library",
        "default_entry": '''# zlib dependency
libs "z"
''',
    },
}


def generate_entry(lib_name, lib_path=None, include_path=None):
    """Generate dub.sdl entry for a library."""
    dep_info = KNOWN_DEPS.get(lib_name, {
        "description": f"Library {lib_name}",
        "default_entry": f'''# {lib_name} dependency
libs "{lib_name}"
''',
    })

    entry = dep_info["default_entry"]

    # Customize paths if provided
    if lib_path:
        entry = entry.replace(f'libs "{lib_name}"', f'libs "{lib_name}"\nlibPaths "{lib_path}"')

    if include_path:
        entry = entry.replace(f'importPaths "vendor/{lib_name}/include"', f'importPaths "{include_path}"')

    return entry


def main():
    parser = argparse.ArgumentParser(
        description="Generate dub.sdl dependency entries for C/C++ libraries",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Known libraries: mylib, miniorm, sqlite3, curl, zlib

Examples:
  %(prog)s mylib
  %(prog)s mylib --lib-path ./vendor/mylib --include-path ./vendor/mylib/include
  %(prog)s mylib miniorm sqlite3
  %(prog)s --list-known
"""
    )
    parser.add_argument("libs", nargs="*", help="Library names to generate entries for")
    parser.add_argument("--list-known", action="store_true", help="List all known libraries")
    parser.add_argument("--lib-path", help="Library search path")
    parser.add_argument("--include-path", help="Include search path")

    args = parser.parse_args()

    if args.list_known:
        print("Known libraries:")
        for name, info in KNOWN_DEPS.items():
            print(f"  {name}: {info['description']}")
        sys.exit(0)

    if not args.libs:
        # Generate for all known libraries
        args.libs = list(KNOWN_DEPS.keys())

    print("# dub.sdl dependency entries")
    print("# Add these to your dub.sdl file")
    print()

    for lib_name in args.libs:
        entry = generate_entry(lib_name, args.lib_path, args.include_path)
        print(entry)

    sys.exit(0)


if __name__ == "__main__":
    main()
