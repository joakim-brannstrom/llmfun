# Self Improve Build Guide

## Making Changes

1. Modify source code in `llmfun/source/`.
2. Build with `executeDCodeWithDub("llmfun")`.
3. Check exit code: 0 = success, non-zero = errors.
4. If errors occur, read the error output, identify the problematic file and line, then fix.
5. Repeat until build succeeds.

## Build Commands

### Full Project Build
```
executeDCodeWithDub(path="llmfun", command="build")
```
Builds the entire llmfun project with all dependencies.

### Run Tests
```
executeDCodeWithDub(path="llmfun", command="test")
```
Runs the test suite.

### Single File Compilation
```
executeCode(path="path/to/file.d", language="d")
```
For simple single-file D compilation without project dependencies.

## Verification

After making changes:

1. **Always run `executeDCodeWithDub("llmfun")`** to verify compilation succeeds.
2. Check the exit code: 0 means success, non-zero means errors.
3. If errors occur, read the error output, identify the problematic file and line, then fix.
4. Repeat until the build succeeds with exit code 0.

## Common Error Patterns

- **Import errors**: Check that module names match the pattern `import llm.module_name;`
- **Path errors**: Use `my.path.Path` for file paths
- **Logger errors**: Use `logger = std.logger` aliased import
- **Llama API errors**: Check `vendor/llama.cpp/include/` for low-level API usage

## Important: Changes Do Not Take Effect Automatically

The build system compiles the project, but the running agent continues using the
previously loaded binary. To make changes effective:

1. **Review and merge** the changes into the main branch
2. **Restart the agentic framework** to load the new binary

Always inform the user that changes have been made and that they need to restart
the framework for them to take effect.

## When Done

Inform the user of:
- What changes were made
- Whether the build succeeded
- That changes require merging and restarting the framework to take effect
