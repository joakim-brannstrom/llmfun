#!/usr/bin/env python3
"""
Find C/C++ library dependencies for D projects.
Searches common system paths and reports library locations.

Usage:
    python3 find_c_libs.py [libname] [--verbose]
    python3 find_c_libs.py --list-known

Examples:
    # Find specific library
    python3 find_c_libs.py mylib

    # Find multiple libraries
    python3 find_c_libs.py mylib miniorm

    # List all known libraries
    python3 find_c_libs.py --list-known

    # Verbose output with search paths
    python3 find_c_libs.py mylib --verbose
"""
import sys
import os
import glob
import argparse

# Known libraries to search for
KNOWN_LIBS = {
    "mylib": {
        "headers": ["mylib.h"],
        "libs": ["libmylib.so", "libmylib.a", "mylib.lib"],
        "description": "Custom C utility library",
    },
    "miniorm": {
        "headers": ["miniorm.h"],
        "libs": ["libminiorm.so", "libminiorm.a", "miniorm.lib"],
        "description": "Minimal ORM library",
    },
    "sqlite3": {
        "headers": ["sqlite3.h"],
        "libs": ["libsqlite3.so", "libsqlite3.a", "sqlite3.lib"],
        "description": "SQLite database library",
    },
    "curl": {
        "headers": ["curl/curl.h"],
        "libs": ["libcurl.so", "libcurl.a", "curl.lib"],
        "description": "HTTP client library",
    },
    "zlib": {
        "headers": ["zlib.h"],
        "libs": ["libz.so", "libz.a", "zlib.lib"],
        "description": "Compression library",
    },
}

# Common search paths
SEARCH_PATHS = [
    "/usr/include",
    "/usr/local/include",
    "/usr/lib",
    "/usr/local/lib",
    "/usr/lib/x86_64-linux-gnu",
    "/usr/local/lib/x86_64-linux-gnu",
    "./vendor",
    "./lib",
    "./deps",
    "./third_party",
    "./external",
]


def find_library(lib_name, verbose=False):
    """Search for a library in common paths."""
    result = {
        "name": lib_name,
        "found": False,
        "headers": [],
        "libs": [],
        "paths": [],
    }

    # Get library info
    lib_info = KNOWN_LIBS.get(lib_name, {
        "headers": [f"{lib_name}.h"],
        "libs": [f"lib{lib_name}.so", f"lib{lib_name}.a"],
        "description": f"Library {lib_name}",
    })

    # Search for headers
    for header in lib_info["headers"]:
        for path in SEARCH_PATHS:
            if verbose:
                print(f"  Checking {path}/{header}")
            full_path = os.path.join(path, header)
            if os.path.isfile(full_path):
                result["headers"].append(full_path)
                result["found"] = True
                if verbose:
                    print(f"    FOUND: {full_path}")

            # Also search recursively in vendor-like dirs
            if any(x in path for x in ["vendor", "deps", "third_party", "external"]):
                pattern = os.path.join(path, "**", header)
                matches = glob.glob(pattern, recursive=True)
                for m in matches:
                    if m not in result["headers"]:
                        result["headers"].append(m)
                        result["found"] = True
                        if verbose:
                            print(f"    FOUND: {m}")

    # Search for libraries
    for lib in lib_info["libs"]:
        for path in SEARCH_PATHS:
            if verbose:
                print(f"  Checking {path}/{lib}")
            full_path = os.path.join(path, lib)
            if os.path.isfile(full_path):
                result["libs"].append(full_path)
                result["found"] = True
                if verbose:
                    print(f"    FOUND: {full_path}")

    result["paths"] = list(set(result["headers"] + result["libs"]))
    return result


def list_known_libs():
    """List all known libraries."""
    print("Known libraries:")
    for name, info in KNOWN_LIBS.items():
        print(f"  {name}: {info['description']}")
        print(f"    Headers: {', '.join(info['headers'])}")
        print(f"    Libs: {', '.join(info['libs'])}")


def main():
    parser = argparse.ArgumentParser(
        description="Find C/C++ library dependencies for D projects",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Known libraries: mylib, miniorm, sqlite3, curl, zlib

Search paths:
  /usr/include, /usr/local/include
  /usr/lib, /usr/local/lib
  ./vendor, ./lib, ./deps, ./third_party, ./external

Examples:
  %(prog)s mylib
  %(prog)s mylib miniorm
  %(prog)s --list-known
  %(prog)s mylib --verbose
"""
    )
    parser.add_argument("libs", nargs="*", help="Library names to search for")
    parser.add_argument("--list-known", action="store_true", help="List all known libraries")
    parser.add_argument("--verbose", "-v", action="store_true", help="Verbose output")

    args = parser.parse_args()

    if args.list_known:
        list_known_libs()
        sys.exit(0)

    if not args.libs:
        # Search for all known libraries
        args.libs = list(KNOWN_LIBS.keys())

    results = []
    for lib_name in args.libs:
        if args.verbose:
            print(f"\nSearching for {lib_name}...")
        result = find_library(lib_name, args.verbose)
        results.append(result)

    # Print summary
    print(f"\n{'='*60}")
    print("LIBRARY SEARCH RESULTS")
    print(f"{'='*60}")

    for result in results:
        status = "FOUND" if result["found"] else "NOT FOUND"
        print(f"\n{result['name']}: {status}")
        if result["headers"]:
            print(f"  Headers:")
            for h in result["headers"]:
                print(f"    {h}")
        if result["libs"]:
            print(f"  Libraries:")
            for l in result["libs"]:
                print(f"    {l}")

    # Exit with error if any library not found
    missing = [r["name"] for r in results if not r["found"]]
    if missing:
        print(f"\nMissing libraries: {', '.join(missing)}")
        sys.exit(1)

    sys.exit(0)


if __name__ == "__main__":
    main()
