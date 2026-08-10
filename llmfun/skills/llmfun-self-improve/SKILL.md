---
name: llmfun-self-improve
description: >-
  Explore and improve the llmfun agentic framework. Use when looking to improve
  the agent's capabilities, fix framework issues, or enhance the implementation.
  Triggers on: self improvement, improve framework, enhance agent, fix llmfun,
  agent capabilities, framework changes, llmfun source, self-improve,
  improve myself.
version: 1.3.0
---

# Self Improve Skill

Explore and improve the llmfun agentic framework. This skill is for modifying
the agent's own implementation.

Read `llmfun/AGENTS.md` before changing code. It holds the code conventions
and architecture rules that must be followed.

## When to Use

Use this skill when:
- Improving the agent's capabilities or behavior
- Fixing issues in the llmfun framework
- Enhancing the agentic framework implementation
- Modifying source code in the `llmfun/` directory

## Project Location

- The llmfun git repo is the `llmfun/` directory (container path `/workarea/llmfun`).
- Source code: `llmfun/source/llm/` (modules like `llm.agent`, `llm.config`).
- Runtime config and built-in skills: `llmfun/llmfun/` (nested directory).
- Full layout in `llmfun/AGENTS.md`.

## Verified Environment

- Container image: `llmfun/app:latest`.
- The container starts in `/workarea` (workspace root). The repo is NOT the
  working directory - always `cd llmfun` first or use the helper scripts.
- git, dub, ldc2, bash are all installed in the image.

## Build System

- DUB build system. Config: `llmfun/dub.sdl`.
- Build: `executeCommand(environmentTag="llmfun", command=["cd", "llmfun", "&&", "dub", "build"])`
- Test: `executeCommand(environmentTag="llmfun", command=["cd", "llmfun", "&&", "dub", "test"])`
- `dub build` defaults to config `application`. Other configs:
  `--config=application-with-local-model`, `--config=llmfun_test`.

## Helper Scripts

The `scripts/` directory wraps the verified commands and auto-locates the repo.
After the skill is loaded the scripts are at `skills/llmfun-self-improve/scripts/`
(container: `/workarea/skills/llmfun-self-improve/scripts/`).

- `scripts/build.sh [--config=<name>]` - build the project
- `scripts/test.sh` - run the test suite
- `scripts/git.sh <args>` - run git in the repo (e.g. `git.sh status`)

Invoke scripts with `bash` or `sh` (exec bit may be lost when copied):
`command=["bash", ".../scripts/build.sh"]`. See `references/build-guide.md`
for `executeCommand` argument semantics and command forms.

Example:
```
executeCommand(environmentTag="llmfun", command=["bash", "/workarea/skills/llmfun-self-improve/scripts/build.sh"])
```

## Git Workflow

- Repo root: `llmfun/` (container `/workarea/llmfun`). Check state first with
  `scripts/git.sh status`.
- Read-only commands (status, diff, log, branch) are safe any time.
- NEVER commit or push without explicit human approval (`llmfun/AGENTS.md`).

## Critical Rule

**Changes do not take effect automatically.** The running agent uses the
previously loaded binary. After making changes:

1. Build to verify compilation: `scripts/build.sh` (exit code 0 = success).
2. Inform the user that changes require merging and restarting the framework.
3. The user merges changes to the main branch and restarts for them to take effect.

## Workflow

1. **Identify the change** - What needs to be improved or fixed.
2. **Check repo state** - `scripts/git.sh status` to see branch and dirty files.
3. **Locate the source** - Find the relevant files in `llmfun/source/`.
4. **Make the change** - Modify source code in `llmfun/source/`.
5. **Build and verify** - Run `scripts/build.sh`, check exit code (0 = success).
6. **Fix errors** - If compilation fails, read errors, fix issues, rebuild.
7. **Report** - Inform user of changes and remind them to merge and restart.

## References

- Detailed build and verification guide: `references/build-guide.md`
