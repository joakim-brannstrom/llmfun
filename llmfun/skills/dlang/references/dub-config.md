# Dub Configuration Guide

## Project Structure

A dub project is a directory containing either:
- `dub.sdl` — Simple Configuration Language format
- `dub.json` — JSON format

## Building Projects

Build by executing `dub build` in the project directory. This reads the configuration and compiles all source files.

## Using executeDCodeWithDub

The `executeDCodeWithDub` tool compiles and executes D code with dub.

**Parameters:**
- `path`: Path to the project directory (containing dub.sdl or dub.json)
- `command`: Either `build` (compile) or `test` (run tests)

**Example usage:**
```
executeDCodeWithDub(path="my-project", command="build")
executeDCodeWithDub(path="my-project", command="test")
```

## Minimal dub.sdl

```sdl
name "program"
description "A minimal D application."

targetPath "build"
targetType "executable"
```

This configuration:
- Sets the project name to "program"
- Adds a description
- Outputs compiled artifacts to `build/` directory
- Builds as an executable (not a library)
- Automatically reads and compiles all files in the source directory

## Common targetTypes

| Type | Description |
|------|-------------|
| `executable` | Standalone program |
| `library` | Shared or static library |
| `sourceLibrary` | Header-only / source-only library |
| `dynamic-library` | Dynamic/shared library |
| `static-library` | Static library |

## Source File Conventions

By default, dub looks for source files in:
- `source/` directory for main source code
- `source/app.d` as the default entry point for executables
- Files matching `*.d` extension

## Dependencies

Add dependencies in dub.sdl:
```sdl
dependency "vibe-d" version "~>0.9.0"
dependency "stdx-allocator" version="~>2.0.0"
```

Or in dub.json:
```json
{
  "dependencies": {
    "vibe-d": "~>0.9.0",
    "stdx-allocator": "~>2.0.0"
  }
}
```
