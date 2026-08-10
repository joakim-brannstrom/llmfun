# Self Improve Build Guide

## Verified Environment

- Container image: `llmfun/app:latest` (configured as an execution environment).
- Container working directory is `/workarea` (the workspace root). The repo is a
  subdirectory: `/workarea/llmfun`. All dub/git commands must `cd llmfun` first.
- git, dub, ldc2 and bash are installed in the image.

## Making Changes

1. Modify source code in `llmfun/source/`.
2. Build with `executeCommand(environmentTag="llmfun", command=["cd", "llmfun", "&&", "dub", "build"])`
   (dub must run inside the repo — see "Common Error Patterns").
3. Check exit code: 0 = success, non-zero = errors.
4. If errors occur, read the error output, identify the problematic file and line, then fix.
5. Repeat until build succeeds.

## Build Commands

### Full Project Build
```
executeCommand(environmentTag="llmfun", command=["cd", "llmfun", "&&", "dub", "build"])
```
Builds the entire llmfun project. `dub build` defaults to the `application`
configuration (main app, remote API only).

### Specific Configurations
```
executeCommand(environmentTag="llmfun", command=["cd", "llmfun", "&&", "dub", "build", "--config=application-with-local-model"])
executeCommand(environmentTag="llmfun", command=["cd", "llmfun", "&&", "dub", "build", "--config=llmfun_test"])
```

### Run Tests
```
executeCommand(environmentTag="llmfun", command=["cd", "llmfun", "&&", "dub", "test"])
```
Runs the inline unit tests.

### Single File Compilation
```
executeCommand(environmentTag="llmfun", command=["cd", "llmfun", "&&", "ldc2", "path/to/file.d"])
```
For simple single-file D compilation without project dependencies.

## executeCommand Argument Semantics

`executeCommand` joins the `command` array elements with spaces and runs the result
through a shell. Consequences:

- `command=["cd", "llmfun", "&&", "dub", "build"]` works (joined: `cd llmfun && dub build`).
- Do NOT use `command=["bash", "-c", "multi word command"]`: only the first word
  becomes the `-c` script and the rest are passed as positional args, so the
  command silently fails or runs in the wrong directory. Use the plain
  `["cd", "llmfun", "&&", ...]` form instead.
- A single-element command like `command=["bash /workarea/skills/llmfun-self-improve/scripts/build.sh"]`
  runs as-is.

## Helper Scripts

The scripts in `scripts/` wrap the verified commands and auto-locate the repo
(they search upward from the script location and honor `$LLMFUN_REPO`):

- `scripts/build.sh [--config=<name>]` - `dub build` in the repo
- `scripts/test.sh` - `dub test` in the repo
- `scripts/git.sh <args>` - any git command in the repo (e.g. `scripts/git.sh status`)

Example:
```
executeCommand(environmentTag="llmfun", command=["bash", "/workarea/skills/llmfun-self-improve/scripts/build.sh"])
```

The scripts may lose their executable bit when the skill is copied into the
workarea, so always invoke them explicitly with `bash` or `sh`:
`command=["bash", ".../scripts/build.sh"]` or `command=["sh", ".../scripts/build.sh"]`.
The scripts themselves invoke `find-repo.sh` via `sh` and therefore do not
require any exec permission.

If the scripts cannot locate the repo, fall back to the explicit
`cd llmfun && dub build` command form.

## Git Commands

The git repo root is `llmfun/` (= `/workarea/llmfun` in the container):

```
executeCommand(environmentTag="llmfun", command=["cd", "llmfun", "&&", "git", "status"])
executeCommand(environmentTag="llmfun", command=["cd", "llmfun", "&&", "git", "diff"])
executeCommand(environmentTag="llmfun", command=["cd", "llmfun", "&&", "git", "log", "--oneline", "-10"])
```

Read-only git commands are always safe. NEVER commit or push without explicit
human approval.

## Verification

After making changes:

1. **Always run `scripts/build.sh`** (or the explicit `cd llmfun && dub build`) to verify compilation succeeds.
2. Check the exit code: 0 means success, non-zero means errors.
3. If errors occur, read the error output, identify the problematic file and line, then fix.
4. Repeat until the build succeeds with exit code 0.

## Common Error Patterns

- **"No valid root package found"**: You ran dub outside the repo. Prefix the command with `cd llmfun &&` or use the helper scripts.
- **"Environment 'X' not found"**: Use the configured environment tag. Run `listEnvironments()` to see the available entries.
- **Import errors**: Check that module names match the pattern `import llm.module_name;`
- **Path errors**: Use `my.path.Path` for file paths
- **Logger errors**: Use `logger = std.logger` aliased import
