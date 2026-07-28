# D Language Code Review Checklist

## Imports

- **Redundant local imports**: If a symbol is already imported at module level, local imports inside functions are unnecessary noise. Remove them.
- **Aliased imports**: Check for consistent alias patterns (e.g., `import logger = std.logger;`) when modules share naming conventions.
- **Module-level vs local scope**: Prefer module-level imports. Local imports inside functions should only be used to avoid circular dependencies.

## Concurrency & Safety Attributes

- **`@safe` over `@trusted`**: Use `@safe` when possible for compile-time safety guarantees. `@trusted` should only bridge safe and unsafe code.
- **`@nogc` compliance**: Functions marked `@nogc` cannot call allocators (`dup`, `~=`, `new`). Flag any allocator calls in `@nogc` contexts.
- **`immutable` vs `shared`**: Mixing `shared` and `immutable` causes compilation issues. Prefer `immutable` for static/constant data.
- **`nothrow` with SumType**: When functions return `SumType!(Success, Error)`, they can be `nothrow`. Verify callers handle both cases via `.match()`.

## Type System

- **Immutable caching**: Constant expressions (like `SysTime(DateTime.init)`) should be cached as `immutable` members, not reconstructed on every call.
- **No `.dup` on immutable**: Immutable data doesn't need `.dup` — return directly.
- **JSON access**: Use `.boolean` (not `.bool`) for JSON boolean values. Use `.count` (not `.length`) on filtered ranges.
- **Tuple over string encoding**: Prefer `Tuple!(T1, T2)[string]` over `"a:b"` string parsing for paired data.

## Path Handling (Critical)

- **Always use `Path` type**, never `string` for file paths. Compile-time safety prevents path manipulation bugs.
- **Path concatenation**: Use `~` operator on Path types — handles slashes correctly.
- **Path properties**: Use `.extension`, `.name` instead of string operations like `.endsWith()` or `baseName()`.
- **Directory scanning**: Use `SpanMode.shallow` with `dirEntries` when filtering by extension.

## Error Handling

- **SumType pattern**: `SumType!(SuccessType, ErrorType)` — success first, error second. Callers must `.match()` on result.
- **Collect exceptions**: Use `std.exception.collectException` in loops — one failure shouldn't stop all processing.
- **Null checks**: Verify `if (obj !is null)` before method calls, especially after error-isolated initialization.
- **Error message format**: Use `format!"error: ..."` consistently for parseable error messages.

## Data Structures & Algorithms

- **Sort predicates return bool**: `sort!((a, b) { return a > b; })` — not subtraction-based comparison.
- **Functional chains**: Prefer `splitter.map.filter` over manual `foreach` loops for data transformation.
- **Materialize ranges**: Use `.array` when random access or multiple iterations are needed.
- **String building**: Use `formattedWrite(buf, ...)` with `appender!string()` instead of `format!` — avoids intermediate allocations.

## Naming Conventions

- **Class names**: PascalCase (`MetricMonitor`)
- **Module-level constants**: PascalCase (`MaxEvents = 10_000`)
- **Instance-level constants**: PascalCase (`MaxWarnings = 5`)
- **Module organization**: Use `private:` section label to organize private members

## Common Pitfalls

- **`~=` is not `@nogc`**: The array append operator allocates memory.
- **String `~` concatenation**: Inefficient in loops — use `appender!string()` instead.
- **`std.math.mean`/`stddev`**: Check version compatibility — custom implementations may be more reliable.
- **Over-initialization flags**: Don't flag uninitialized variables as critical without confirming D version and context.
- **Indentation consistency**: After edits, verify consistent indentation (e.g., 4-space vs 8-space within same struct block).
