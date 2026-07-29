# Code Refactor Skill - Utility Scripts

Consolidated utility scripts for code refactoring tasks.

## Scripts

### `fix_escaping.py`
Fix string escaping issues in source files. Handles cross-language escaping (e.g., writing D regex patterns from Python).

```bash
# Fix D regex escaping
python3 fix_escaping.py source/skill.d --regex-fix --language d

# Replace specific pattern
python3 fix_escaping.py file.d --pattern 'old' --replace 'new'

# Check for issues
python3 fix_escaping.py file.d --check --language d

# Dry run
python3 fix_escaping.py file.d --regex-fix --dry-run
```

**Key insight:** When writing D source from Python, use `chr()` for problematic chars:
```python
BS = chr(92)  # backslash
DQ = chr(34)  # double quote
SQ = chr(39)  # single quote
```

### `rename_ident.py`
Safely rename identifiers in source files. Handles word boundaries, imports, comments, and strings.

```bash
# Rename with word boundary
python3 rename_ident.py source/app.d --from _oldName --to newName --word-boundary

# Skip import lines (default)
python3 rename_ident.py source/app.d --from agent --to agent_

# Check for conflicts
python3 rename_ident.py source/app.d --from foo --to bar --check-conflicts

# Dry run
python3 rename_ident.py source/app.d --from old --to new --dry-run
```

**Safety features:**
- Skips import lines by default
- Skips comment lines by default
- Word boundary matching prevents partial matches
- Conflict detection warns about potential issues
- Member variable renaming (`--member-var` converts `_name` to `name_`)

### `revert_changes.py`
Revert changes in source files using backup files. Supports multiple backup versions and selective revert.

```bash
# Revert from auto-generated backup
python3 revert_changes.py source/app.d --auto

# Revert from specific backup
python3 revert_changes.py source/app.d --backup source/app.d.bak

# Revert specific line range from backup
python3 revert_changes.py source/app.d --auto --line-range 100-200

# List available backups
python3 revert_changes.py source/app.d --list-backups

# Create backup before making changes
python3 revert_changes.py source/app.d --create-backup
```

**Backup patterns searched:**
- `<file>.bak`, `<file>.bak.*`, `<file>.orig`, `<file>.backup`, `<file>.backup.*`, `<file>~`

### `fix_indent.py`
Fix indentation issues in source files. Handles space/tab inconsistencies and misaligned blocks.

```bash
# Check indentation in line range
python3 fix_indent.py source/app.d --check --start 100 --end 200

# Remove extra spaces
python3 fix_indent.py source/app.d --start 100 --end 200 --remove-spaces 1

# Convert tabs to spaces
python3 fix_indent.py source/app.d --tabs-to-spaces 4

# Auto-detect and fix
python3 fix_indent.py source/app.d --auto-fix

# Recursive directory scan
python3 fix_indent.py source/ --recursive --check --ext .d

# Dry run
python3 fix_indent.py source/app.d --start 100 --end 200 --dry-run
```

**Common issues fixed:**
- Mixed tabs and spaces
- Odd indentation (not multiple of N)
- Extra leading spaces
- Tab/space conversion

**Features:**
- Configurable indent width (`--indent-width`)
- Recursive directory scanning (`--recursive`)
- Auto-fix mode (`--auto-fix`)
- Line range targeting (`--start`/`--end`)

## Origin

These scripts were consolidated from ~30 one-off utility scripts that accumulated
in the workarea during development sessions. The original scripts were created
to solve specific, immediate problems (regex escaping, identifier renaming,
indentation fixing, change reverting) and have been generalized for reuse.
