# File Editing Skill - Utility Scripts

Consolidated utility scripts for file editing operations.

## Scripts

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
