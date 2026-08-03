---
name: dlang
description: >-
  Build and test D programming language projects with dub. Use when working with
  D programs, compiling D code, running D tests, or managing D project dependencies.
  Triggers on: dlang, d language, d programming, dub, dub.sdl, dub.json,
  D project, D build, D test, compile D, run D.
version: 1.0.0
---

# D Language Skill

Work with D programming language projects using dub as the build system.

## When to Use

Use this skill when:
- Building or testing D projects with dub
- Working with D source code files
- Configuring dub project files (dub.sdl or dub.json)
- Compiling or running D programs
- Diagnosing build environment issues
- Checking D source syntax or brace balance

## Key Concepts

- A **dub project** is a directory containing a `dub.sdl` or `dub.json` file.
- Build by executing `dub build` in the project directory.
- Use `executeDCodeWithDub` tool to compile and execute tests with commands: `build` or `test`.

## Rules

- **Always use dub**: Build and test through dub, not manual compilation.
- **Prefer executeDCodeWithDub**: Use this tool over generic code execution.
- **Check dub config first**: Read `dub.sdl` or `dub.json` before building to understand project structure.
- **Verify build output**: Check the `targetPath` directory (default: `./`) for compiled artifacts.

## Code Conventions

Full details in `references/code-conventions.md`. Key rules:

### Comments

- **Use ddoc**, not doxygen. Forms: `/** ... */`, `///`, `/+ ... +/`.
- **Module header:** Brief ddoc (1-3 lines) before `module` declaration.
- **Explain why, not what.** Concise: 1-2 lines. No hard-wrapping.
- **Write code first, comment later.** Never comment copied code.

### String Handling

- **Prefer interpolated strings** over `std.format.format`.
- **Runtime interpolated strings need `.text`:** `i"value: $(var)".text` — without `.text` you get `AliasSeq`, not `string`. Requires `import std.conv : text;`.
- **No format specifiers in interpolated strings:** Cannot do `%.1f`, `%.80s`, `%(...)`. Keep `format!"..."` for float formatting or use alternatives:
  - Float: `format!"%.1f"(value)` or embed: `i"$(format!"%.1f"(v)) units".text`
  - Truncation: `str[0 .. min(str.length, 80)]` instead of `%.80s`
  - Arrays: `.join(" ")` instead of `%(%-s %)`
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

### General

- **ASCII only.** No emdash, unicode arrows, or non-ASCII.
- **No mid-sentence line breaks.** Let lines flow naturally.
- **Reuse existing infrastructure** over introducing new components.

## Workflow

1. **Identify the project**: Locate the `dub.sdl` or `dub.json` file.
2. **Read the configuration**: Understand project name, type, dependencies, and target path.
3. **Choose the command**: Use `build` to compile, `test` to run tests.
4. **Execute with dub**: Call `executeDCodeWithDub` with the project path and command.
5. **Verify results**: Check output for errors or test results.

## Utility Scripts

See `scripts/README.md` for full documentation. Quick reference:

| Script | Purpose |
|--------|---------|
| `find_d_toolchain.py` | Find ldc2, dmd, gdc, dub |
| `check_d_syntax.py` | Syntax-check .d files via ldc2 |
| `dub_build.py` | Build/test dub projects |
| `count_d_loc.py` | Count LOC in D source |
| `check_braces.py` / `.d` | Verify brace balance |
| `check_build_tools.py` | Verify build environment |
| `find_c_libs.py` | Find C/C++ library dependencies |
| `gen_dub_deps.py` | Generate dub.sdl dependency entries |

## Minimal dub.sdl Configuration

```sdl
name "program"
targetPath "build"
targetType "executable"
```

This minimal config reads and compiles all files in the source directory.

## References

- Dub configuration guide: `references/dub-config.md`
- Code conventions: `references/code-conventions.md`
- Scripts: `scripts/README.md`
