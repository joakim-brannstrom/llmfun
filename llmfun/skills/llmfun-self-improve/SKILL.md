---
name: llmfun-self-improve
description: >-
  Explore and improve the llmfun agentic framework. Use when looking to improve
  the agent's capabilities, fix framework issues, or enhance the implementation.
  Triggers on: self improvement, improve framework, enhance agent, fix llmfun,
  agent capabilities, framework changes, llmfun source, self-improve,
  improve myself.
version: 1.0.0
---

# Self Improve Skill

Explore and improve the llmfun agentic framework. This skill is for modifying
the agent's own implementation.

## When to Use

Use this skill when:
- Improving the agent's capabilities or behavior
- Fixing issues in the llmfun framework
- Enhancing the agentic framework implementation
- Modifying source code in the `llmfun/` directory

## Project Structure

```
llmfun/
├── source/              # Agentic framework source code
│   └── llm/             # Core LLM modules (agent, chat, tool_call, config)
│       └── metric/      # Self-monitoring modules (monitor, calculator)
├── vendor/llama.cpp/    # LLM executor (REST API)
│   └── include/         # Low-level API used by source/llm/llama
├── build/               # Build artifacts
├── build_llama/         # Compiled llama.cpp libraries
├── dub.sdl              # DUB build configuration
└── llama.mak            # Makefile for llama.cpp libraries
```

## Build System

- **DUB** is the build system. Config: `llmfun/dub.sdl`
- **llama.mak** builds llama.cpp libraries (referenced in `dub.sdl` preBuildCommands)
- Use `executeDCodeWithDub("llmfun")` for full project builds
- Use `executeCode` for simple single-file D compilation

## Code Conventions

- Source files in `llmfun/source/llm/` with module names like `llm.agent`, `llm.config`
- Imports use pattern: `import llm.module_name;`
- The `my.path` module provides a custom `Path` type
- Logging uses `logger = std.logger` aliased import

## Critical Rule

**Changes do not take effect automatically.** The running agent uses the previously
loaded binary. After making changes:

1. Build with `executeDCodeWithDub("llmfun")` to verify compilation
2. Inform the user that changes require **merging and restarting** the framework
3. The user must merge changes to main branch and restart for them to take effect

## Workflow

1. **Identify the change** — What needs to be improved or fixed.
2. **Locate the source** — Find the relevant files in `llmfun/source/`.
3. **Make the change** — Modify source code in `llmfun/source/`.
4. **Build and verify** — Run `executeDCodeWithDub("llmfun")`, check exit code (0 = success).
5. **Fix errors** — If compilation fails, read errors, fix issues, rebuild.
6. **Report** — Inform user of changes and remind them to merge and restart.

## References

- Detailed build and verification guide: `references/build-guide.md`
