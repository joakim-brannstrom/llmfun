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

## Key Concepts

- A **dub project** is a directory containing a `dub.sdl` or `dub.json` file.
- Build by executing `dub build` in the project directory.
- Use `executeDCodeWithDub` tool to compile and execute tests with commands: `build` or `test`.

## Rules

- **Always use dub**: Build and test D projects through dub, not manual compilation.
- **Use executeDCodeWithDub**: Prefer this tool over generic code execution for D projects.
- **Check dub config first**: Read `dub.sdl` or `dub.json` before building to understand project structure.
- **Verify build output**: Check the `targetPath` directory (default: `build/`) for compiled artifacts.

## Workflow

1. **Identify the project**: Locate the `dub.sdl` or `dub.json` file.
2. **Read the configuration**: Understand project name, type, dependencies, and target path.
3. **Choose the command**: Use `build` to compile, `test` to run tests.
4. **Execute with dub**: Call `executeDCodeWithDub` with the project path and command.
5. **Verify results**: Check output for errors or test results.

## Minimal dub.sdl Configuration

```sdl
name "program"
description "A minimal D application."

targetPath "build"
targetType "executable"
```

This minimal config reads and compiles all files in the source directory. For more configuration options, see `references/dub-config.md`.

## References

- Dub configuration guide: `references/dub-config.md`
