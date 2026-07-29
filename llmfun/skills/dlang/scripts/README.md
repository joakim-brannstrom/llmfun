# D Language Skill - Utility Scripts

Consolidated utility scripts for working with D programming language projects.

## Scripts

### `find_d_toolchain.py`
Find D compilers (ldc2, dmd, gdc) and dub in the system.
Reports location and version of each tool found.

```bash
python3 find_d_toolchain.py
```

### `check_d_syntax.py`
Check D source file(s) for syntax errors using ldc2 compiler.

```bash
# Check specific files
python3 check_d_syntax.py source/app.d source/llm/agent.d

# Check all .d files in a directory
python3 check_d_syntax.py --all source/

# Check a dub project's source directory
python3 check_d_syntax.py --project myproject/

# Add include directories
python3 check_d_syntax.py -I vendor/lib1 -I vendor/lib2 source/app.d
```

### `dub_build.py`
Build a dub project and verify the result.

```bash
# Build current directory
python3 dub_build.py

# Build specific project
python3 dub_build.py /path/to/project

# Force rebuild
python3 dub_build.py --force

# Run tests
python3 dub_build.py --test

# Specify compiler
python3 dub_build.py --compiler ldc2
```

### `count_d_loc.py`
Count lines of code in D source files.

```bash
# Count in source directory
python3 count_d_loc.py source/

# Output as JSON
python3 count_d_loc.py source/ --json

# Count specific extension
python3 count_d_loc.py source/ --ext .d
```

### `check_braces.py` (Python)
Check brace balance in a source file.

```bash
# Simple count
python3 check_braces.py file.d --simple

# Track depth line by line
python3 check_braces.py file.d --depth

# Stack-based matching with context
python3 check_braces.py file.d --stack

# Check specific line range
python3 check_braces.py file.d --depth --start 100 --end 200
```

### `check_braces.d` (D)
D-based brace balance checker. Compile and run:

```bash
ldc2 check_braces.d -of=check_braces && ./check_braces file.d
ldc2 check_braces.d -of=check_braces && ./check_braces file.d --simple
ldc2 check_braces.d -of=check_braces && ./check_braces file.d --depth
```

### `check_build_tools.py`
Quick verification that the build environment is set up.

```bash
# Quick check
python3 check_build_tools.py

# Verbose output with versions
python3 check_build_tools.py --verbose
```

# Verbose output with versions
python3 check_build_tools.py --verbose
```

### `find_c_libs.py`
Find C/C++ library dependencies in common system paths.

```bash
# Find specific library
python3 find_c_libs.py mylib

# Find multiple libraries
python3 find_c_libs.py mylib miniorm

# List all known libraries
python3 find_c_libs.py --list-known

# Verbose output with search paths
python3 find_c_libs.py mylib --verbose
```

Known libraries: mylib, miniorm, sqlite3, curl, zlib

### `gen_dub_deps.py`
Generate dub.sdl dependency entries for C/C++ libraries.

```bash
# Generate entry for mylib
python3 gen_dub_deps.py mylib

# With custom paths
python3 gen_dub_deps.py mylib --lib-path ./vendor/mylib --include-path ./vendor/mylib/include

# Generate for multiple libraries
python3 gen_dub_deps.py mylib miniorm sqlite3

# List known libraries
python3 gen_dub_deps.py --list-known
```

## Origin
These scripts were consolidated from ~20 one-off utility scripts that accumulated
in the workarea during development sessions. The original scripts were created
to solve specific, immediate problems (finding compilers, checking syntax,
counting lines, verifying builds) and have been generalized for reuse.
