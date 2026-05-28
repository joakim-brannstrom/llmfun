Self Improvement Instructions for an LLM. How to explore the implementation of the agentic framework and explore improvements to it. **Use this when** looking to improve the agent's capabilities, fix issues, or enhance the framework.

# File Structure
The implementation is located in the directory `llmfun/`.

The directory structure in `llmfun/`:

- `source/`: The source of the agentic framework that drives the agent, tooling and utility.
- `source/llm/`: Core LLM integration modules (agent, chat, tool_call, config, etc.)
- `source/llm/metric/`: Self-monitoring modules (monitor, calculator)
- `vendor/llama.cpp/`: The source of the LLM executor that provides the REST API that the agentic framework uses.
    - `vendor/llama.cpp/include/`: Low-level API used by `source/llm/llama`.
- `build/`: The build artifacts
- `build_llama/`: `llama.cpp` compiled to the necessary dynamic libraries.
- `llama.mak`: Makefile for building llama.cpp libraries.

# Build System

- The project uses **DUB** as the build system.
- The `dub.sdl` file is in the project root (`llmfun/dub.sdl`).
- The `llama.mak` file is in the project root (`llmfun/llama.mak`).
- The preBuildCommands in `dub.sdl` reference `llama.mak` for building llama.cpp libraries.

# Code Changes

- Modify the source code in `llmfun/source/`.
- Build the changes with the tool `executeDCodeWithDub("llmfun")`.
- If compilation fails, read the error output and fix the specific issue.
- Use `executeCode` for simple single-file D compilation (no project dependencies).
- Use `executeDCodeWithDub` for full project builds with all dependencies.

# Verification

- After making changes, always run `executeDCodeWithDub("llmfun")` to verify compilation succeeds.
- Check the exit code: 0 means success, non-zero means errors.
- If errors occur, read the error output, identify the problematic file and line, then fix.

# Common Patterns

- Source files are in `llmfun/source/llm/` with module names like `llm.agent`, `llm.config`, etc.
- Imports use the pattern `import llm.module_name;`.
- The `my.path` module provides a custom `Path` type for file paths.
- Logging uses `logger = std.logger` aliased import.

# Important: Changes Do Not Take Effect Automatically

- **All changes made to source files require the user to merge them and restart the agentic framework before they take effect.**
- The build system compiles the project, but the running agent continues using the previously loaded binary.
- To make changes effective, the user must:
  1. Review and merge the changes into the main branch
  2. Restart the agentic framework to load the new binary
- Always inform the user that changes have been made and that they need to restart the framework for them to take effect.

# When Done

Inform the user of what changes were made and whether the build succeeded.
Remind the user that changes require merging and restarting the framework to take effect.
