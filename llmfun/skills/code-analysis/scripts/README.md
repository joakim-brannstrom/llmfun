# Code Analysis Skill - Utility Scripts

Consolidated utility scripts for code analysis tasks.

## Scripts

### `check_braces.py`
Check brace balance in source files. Reports unbalanced braces with line numbers and context.

```bash
# Run all checks (default)
python3 check_braces.py file.c

# Quick count only
python3 check_braces.py file.c --simple

# Track depth line by line
python3 check_braces.py file.c --depth

# Full context for errors
python3 check_braces.py file.c --stack

# Check specific line range
python3 check_braces.py file.c --depth --start 100 --end 200
```

**Modes:**
- `--simple`: Counts opening and closing braces, reports balance
- `--depth`: Tracks brace depth line by line, finds where depth goes negative
- `--stack`: Stack-based matching, shows 3 lines of context for each error

### `count_loc.py`
Count lines of code in source files. Produces per-file breakdown and total. Supports language presets and brace analysis.

```bash
# Count in directory
python3 count_loc.py source/

# Count D source files only
python3 count_loc.py source/ --lang d

# Count C++ source files only
python3 count_loc.py source/ --lang cpp

# Output as JSON
python3 count_loc.py source/ --json

# With brace analysis
python3 count_loc.py source/ --braces

# Custom extensions
python3 count_loc.py source/ --ext .d,.di
```

**Language presets:**
- `--lang d`: `.d`, `.di` files
- `--lang cpp`: `.cpp`, `.hpp`, `.cxx`, `.hxx`, `.cc`, `.hh`, `.h` files
- `--lang c`: `.c`, `.h` files
- `--lang all`: All source files

**Features:**
- Per-file line count breakdown
- JSON output for automation
- Brace balance analysis (`--braces`)
- Custom extension support

**Default extensions:** `.c`, `.h`, `.d`, `.cpp`, `.py`

### `file_integrity.py`
Compute and verify MD5 hashes of source files. Detect unintended changes.

```bash
# Compute hashes for directory
python3 file_integrity.py source/

# Compute hash for single file
python3 file_integrity.py source/app.d --file

# Save hashes as JSON
python3 file_integrity.py source/ --json > hashes.json

# Verify against known hashes
python3 file_integrity.py source/ --hashes hashes.json

# Verify with JSON output
python3 file_integrity.py source/ --hashes hashes.json --json
```

**Output:** Lists changed, missing, and new files compared to known hashes.

**Features:**
- Single file hash computation (`--file`)
- Directory scanning with extension filtering
- JSON output for automation
- Hash verification against saved baseline
- Changed/missing/new file detection

**Output:** Lists changed, missing, and new files compared to known hashes.

## Origin

These scripts were consolidated from ~20 one-off utility scripts that accumulated
in the workarea during development sessions. The original scripts were created
to solve specific, immediate problems (checking braces, counting lines, verifying
file integrity) and have been generalized for reuse.
