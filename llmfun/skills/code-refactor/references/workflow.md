# Code Refactor Workflow — Detailed Steps

## Phase 1: Identify Refactoring Targets

- **Scan for code smells**: Look for duplication, long functions, large classes, deep nesting.
- **Check naming**: Flag unclear variable, function, or class names.
- **Analyze structure**: Identify code that's misplaced (e.g., logic in wrong module, helpers at top).
- **Assess complexity**: Flag functions with too many branches, parameters, or responsibilities.
- **Check consistency**: Note deviations from project conventions and patterns.

## Phase 2: Plan Refactoring Tasks

- **Define scope**: Decide what to refactor in this pass (avoid changing too much at once).
- **Preserve behavior**: Identify all public interfaces and ensure they remain unchanged.
- **Break into tasks**: Plan incremental refactoring steps, each preserving correct behavior.
- **Check dependencies**: Note which files or modules will be affected.
- **Order by impact**: Prioritize high-impact refactors first.

## Phase 3: Execute Task — Extract

For extraction tasks:
- **Move code to appropriate functions, classes, or modules**.
- **Update callers** to use the new interface.
- **Verify behavior** is preserved.

## Phase 4: Execute Task — Rename

For renaming tasks:
- **Improve names** while maintaining clarity and consistency.
- **Update all references** to the renamed item.
- **Verify no broken references** remain.

## Phase 5: Execute Task — Simplify

For simplification tasks:
- **Remove dead code**, unused variables, and unreachable branches.
- **Combine conditions** where possible.
- **Simplify nested logic** by extracting helper functions.
- **Verify behavior** is preserved.

## Phase 6: Execute Task — Organize

For organization tasks:
- **Reorder code** logically (imports, constants, public members, private members, helpers).
- **Group related code** into appropriate modules.
- **Verify no imports are broken**.

## Phase 7: Verify Behavior Preservation

After all refactoring tasks:
- **Check interfaces**: Ensure all public signatures remain unchanged.
- **Trace execution**: Follow the code flow to verify behavior is identical.
- **Check edge cases**: Verify edge cases still work correctly after refactoring.
- **Run tests**: Ensure all existing tests still pass.

## Phase 8: Produce Output

Report refactoring tasks using the output format template in `output-format.md`.
