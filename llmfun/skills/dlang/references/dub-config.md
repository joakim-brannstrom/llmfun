# Dub Configuration Guide

## Project Structure

A dub project is a directory containing either:
- `dub.sdl` — Simple Configuration Language format
- `dub.json` — JSON format

## Building Projects

Build by executing `dub build` in the project directory. This reads the configuration and compiles all source files.

## Using executeImage

The `executeImage` tool runs commands in a container image. Use a D language image with dub.

**Parameters:**
- `image_name`: Container image with D toolchain (e.g., "dlang2:latest")
- `command`: Command elements to execute (e.g., ["dub", "build"])

**Example usage:**
```
executeImage(image_name="dlang2:latest", command=["cd", "<project-dir>", "&&", "dub", "build"])
executeImage(image_name="dlang2:latest", command=["cd", "<project-dir>", "&&", "dub", "test"])
```

The build target "syntax" is excellent for checking the syntax of the code
without generating any object code. Prefer using it if you do not need to
execute the tests.

```
executeImage(image_name="dlang2:latest", command=["cd", "<project-dir>", "&&", "dub", "build", "-b", "syntax"])
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
