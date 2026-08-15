---
name: dlang
description: >-
  Build and test D programming language projects with dub. Use when working with
  D programs, compiling D code, running D tests, or managing D project dependencies.
  Triggers on: dlang, d language, d programming, dub, dub.sdl, dub.json,
  D project, D build, D test, compile D, run D.
version: 1.3.0
---

# D Language Skill

Work with D programming language projects using dub as the build system.

## When to Use

Use when building or testing D projects with dub, working with D source files,
configuring dub projects (dub.sdl or dub.json), or diagnosing build issues.

## Key Concepts

- A **dub project** is a directory containing a `dub.sdl` or `dub.json` file.

## Rules

- **Always use dub**: Build and test through dub, not manual compilation.
- **Use executeImage**: Use `executeCommand` with a D language environment to run dub commands.
  Example: `executeCommand(environmentTag="dlang2:latest", command=["cd", "<project-dir>", "&&", "dub", "build"])`
- **Check dub config first**: Read `dub.sdl` or `dub.json` before building to understand project structure.
- **Verify build output**: Check the `targetPath` directory (default: `./`) for compiled artifacts.

## Code Conventions

Full details in `references/code-conventions.md`. Key rules:

### Comments

- **Use ddoc**, not doxygen. Forms: `/** ... */`, `///`, `/+ ... +/`.
- **Module header:** Brief ddoc (1-3 lines) before `module` declaration.
- **Explain why, not what.** Concise: 1-2 lines. No hard-wrapping.

### String Handling

- **Prefer interpolated strings** over `std.format.format`.
- **Runtime interpolated strings need `.text`:** `i"value: $(var)".text` — without `.text` you get `AliasSeq`, not `string`. Requires `import std.conv : text;`.
- **No format specifiers in interpolated strings:** Cannot do `%.1f`, `%.80s`, `%(...)`. Use `format!"..."` for floats, `.join(" ")` for arrays, slicing for truncation (examples in references).
- **Prefer backtick-strings** when embedding `"` or `\`.

### Variable Initialization

- **NEVER initialize `string` with `= ""`.** D auto-initializes locals.
- **Prefer local function initialization** over scattered assignments.

### Naming & Formatting

- **Local imports** inside functions/structs where symbols are not pervasive
- **No magic numbers** without named constants

### Error Handling

- **No empty catch blocks.** Log via `std.logger`.
- **Silent catches for @safe:** Nested try/catch around logging allowed when
  `.collectException` can't be used. Innermost empty catch is the only exception.

### Attributes

- **@safe:** Mark whenever possible. Fix issues, don't downgrade.
- **@trusted:** Only when `@safe` not feasible. Keep minimal, verify inputs.
- **@system:** Default. Avoid; only for low-level ops (pointers, asm, C interop).
- **pure:** Add when no global/mutable state access.
- **nothrow:** Mark when no exceptions. Signals error-return patterns.

### Global State & Threading

- **Globals are thread-local (TLS) by default**: each thread gets its own copy; a registry filled on one thread is empty on others. Use `__gshared` or `shared` for cross-thread state. Details: `references/code-conventions.md`.

### General

- **ASCII only.** No emdash, unicode arrows, or non-ASCII.
- **No mid-sentence line breaks.** Let lines flow naturally.
- **Reuse existing infrastructure** over introducing new components.

## Workflow

1. **Identify the project**: Locate the `dub.sdl` or `dub.json` file.
2. **Read the configuration**: Understand project name, type, dependencies, and target path.
3. **Choose the command**: Use `build` to compile, `test` to run tests.
4. **Execute with dub**: Use `executeCommand` with a D language container environment and dub command (see Rules).
5. **Verify results**: Check output for errors or test results.

## Utility Scripts

See `scripts/README.md` for the script reference (syntax checks, dub builds, toolchain discovery).

## References

- Dub configuration guide: `references/dub-config.md`
- Code conventions: `references/code-conventions.md`
- Scripts: `scripts/README.md`
