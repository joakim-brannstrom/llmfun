A structured protocol for an LLM agent to improve code structure while preserving behavior. Use this when code is duplicated, overly complex, poorly organized, or violates project conventions.

# Refactoring Strategy (LLM Agent)
**Important**: The output must be refactoring tasks, not just descriptions. Code is produced by executing the tasks.
**Important**: Write the plan to a file and inform the user.

## 1. Identify Refactoring Targets

- **Scan for code smells**: Look for duplication, long functions, large classes, deep nesting.
- **Check naming**: Flag unclear variable, function, or class names.
- **Analyze structure**: Identify code that's misplaced (e.g., logic in wrong module, helpers at top).
- **Assess complexity**: Flag functions with too many branches, parameters, or responsibilities.
- **Check consistency**: Note deviations from project conventions and patterns.

## 2. Plan Refactoring Tasks

- **Define scope**: Decide what to refactor in this pass (avoid changing too much at once).
- **Preserve behavior**: Identify all public interfaces and ensure they remain unchanged.
- **Break into tasks**: Plan incremental refactoring steps, each preserving correct behavior.
- **Check dependencies**: Note which files or modules will be affected.
- **Order by impact**: Prioritize high-impact refactors first.

## 3. Execute Task: Extract

For extraction tasks:
- **Move code to appropriate functions, classes, or modules**.
- **Update callers** to use the new interface.
- **Verify behavior** is preserved.

## 4. Execute Task: Rename

For renaming tasks:
- **Improve names** while maintaining clarity and consistency.
- **Update all references** to the renamed item.
- **Verify no broken references** remain.

## 5. Execute Task: Simplify

For simplification tasks:
- **Remove dead code**, unused variables, and unreachable branches.
- **Combine conditions** where possible.
- **Simplify nested logic** by extracting helper functions.
- **Verify behavior** is preserved.

## 6. Execute Task: Organize

For organization tasks:
- **Reorder code** logically (imports, constants, public members, private members, helpers).
- **Group related code** into appropriate modules.
- **Verify no imports are broken**.

## 7. Verify Behavior Preservation

After all refactoring tasks:
- **Check interfaces**: Ensure all public signatures remain unchanged.
- **Trace execution**: Follow the code flow to verify behavior is identical.
- **Check edge cases**: Verify edge cases still work correctly after refactoring.
- **Run tests**: Ensure all existing tests still pass.

## 8. Output Format

Produce output in this structure:

```markdown
## Refactoring Tasks

### Task 1: Extract duplicate validation logic
- **Files**: [list of files]
- **Acceptance**: Validation logic extracted, all callers updated, tests pass

### Task 2: Rename unclear variable names
- **Files**: [list of files]
- **Acceptance**: All references updated, code compiles, tests pass

### Task 3: Simplify nested conditionals
- **Files**: [list of files]
- **Acceptance**: Logic simplified, behavior preserved, tests pass

## Code Produced
[Refactored code files - one file per code block]

## Verification
- [ ] Public interfaces unchanged
- [ ] All code paths still execute correctly
- [ ] Edge cases handled properly
- [ ] No new dependencies introduced
- [ ] All tests pass
```

