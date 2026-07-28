# Code Task Workflow — Detailed Steps

## Phase 1: Analyze the Task

- **Read the task description**: Understand exactly what needs to be built or changed.
- **Identify the scope**: Note which files are listed under "Changes Made" and what each change entails.
- **Extract acceptance criteria**: Determine what "done" means from the description and verification checklist.
- **Read the notes**: Check for any constraints, warnings, or context provided.

## Phase 2: Survey the Codebase

- **Read existing files**: Open every file listed in "Changes Made" and understand its current state.
- **Identify integration points**: Find where new code connects to existing code (imports, function calls, types).
- **Detect existing patterns**: Note naming conventions, error handling style, architecture patterns, and code organization.
- **Check dependencies**: Identify types, interfaces, and modules that your code depends on and whether they already exist.

## Phase 3: Plan the Implementation

- **Determine execution order**: Decide the sequence of edits within the task (e.g., add types before functions, add functions before callers).
- **Identify new vs. modified**: Separate files that are created new from files that are edited in place.
- **Plan test strategy**: Decide what to test and where tests belong before writing implementation code.
- **Anticipate pitfalls**: Think about edge cases, error paths, and failure modes specific to this task.

## Phase 4: Implement — Foundation Layer

- **Create new files first**: If the task requires new files, create them with proper structure and imports.
- **Define types and interfaces**: Add data structures, type aliases, enums, and interfaces before implementation logic.
- **Add imports**: Ensure all files have the necessary imports for both new and existing dependencies.
- **Scaffold function signatures**: Write function/method signatures with correct signatures before filling in bodies.

## Phase 5: Implement — Core Logic

- **Write the happy path first**: Implement the main success flow without error handling.
- **Add error handling**: Wrap the happy path with proper error handling for expected failure modes.
- **Add input validation**: Validate inputs at entry points (function arguments, API boundaries).
- **Wire connections**: Connect new code to existing code (callers, callbacks, event handlers).

## Phase 6: Implement — Tests

- **Write unit tests**: Cover the happy path, edge cases, and error paths.
- **Place tests correctly**: Follow the project's test directory structure and naming conventions.
- **Test in isolation**: Ensure tests don't depend on external services unless explicitly required.
- **Run tests**: Execute the test suite and verify all tests pass.

## Phase 7: Verify Completion

- **Compile check**: Ensure the code compiles without errors or warnings.
- **Trace execution**: Mentally follow the code flow from entry point to exit and confirm it matches the task description.
- **Check edge cases**: Verify that edge cases and error paths are handled as expected.
- **Acceptance criteria**: Go through each item in the task's verification checklist and confirm it is met.
- **Convention check**: Ensure code style, naming, and patterns match the rest of the codebase.

## Phase 8: Produce Output

Report the completed task using the output format template in `output-format.md`.
