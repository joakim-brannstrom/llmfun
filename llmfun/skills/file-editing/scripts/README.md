# File Editing Skill - Utility Scripts

Consolidated utility scripts for file editing operations.

## Scripts

### `insert_code.py`
Insert code blocks at specific locations in source files. Supports inserting before/after functions, line numbers, or markers.

```bash
# Insert before function appMain
python3 insert_code.py source/app.d --before "void appMain" --code consolidation.d

# Insert after line 100
python3 insert_code.py source/app.d --line 100 --code new_code.d

# Insert before marker string
python3 insert_code.py source/app.d --before "// TODO: add feature" --code feature.d

# Dry run
python3 insert_code.py source/app.d --before "void appMain" --code consolidation.d --dry-run
```

**Insertion modes:**
- `--before <marker>` — Insert before line containing marker
- `--after <marker>` — Insert after line containing marker
- `--line N` — Insert at line number N

### `swap_code.py`
Swap/replace code blocks in source files. Replaces old code with new code, supporting line ranges and markers.

```bash
# Replace old code with new code
python3 swap_code.py source/app.d --old consolidation.d --new consolidation_fixed.d

# Replace lines 100-200 with new code
python3 swap_code.py source/app.d --start 100 --end 200 --new new_code.d

# Dry run
python3 swap_code.py source/app.d --old old.d --new new.d --dry-run
```

**Replacement modes:**
- `--old <file> --new <file>` — Replace old code with new code
- `--start N --end M --new <file>` — Replace line range with new code

## Origin

These scripts were consolidated from ~5 one-off utility scripts that accumulated
in the workarea during development sessions. The original scripts were created
to insert, swap, and patch code blocks in specific files (e.g., app.d) as part
of implementing features from an implementation plan.
