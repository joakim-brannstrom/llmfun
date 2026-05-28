A guide for how to use dub with the D programming language. **Use this when** building or testing D projects with dub.
# D Programming Language and Dub

A dub project is a directory containing a "dub.sdl" or "dub.json" file.
It is built by executing "dub build" in that directory.

The tool executeDCodeWithDub is available to compile and execute tests.

# Minimal dub
This is a minimal dub configuration. It will read and compile all files in the source directory.

```sdl
name "program"
description "A minimal D application."

targetPath "build"
targetType "executable"
```
