# D Programming Language - Lessons Learned

## Type System & Immutability

- **Use `immutable(StructType)[]`** for static template collections. Allows initialization in `shared static this()` with array literals without wrapper structs.
- **No `.dup` needed** with immutable data - return directly, no copying required.
- **Avoid `shared` + `immutable` complexity** - mixing these causes compilation issues. Stick to `immutable` for static data.
- **Use `@safe` over `@trusted`** when possible for compile-time guarantees.
- **`@nogc` restrictions** - `@nogc` functions can't call allocators (like `dup`, `~=` operator). Don't use `dup` with `@nogc`.
- **Immutable member caching** - Cache constant expressions like `SysTime(DateTime.init)` as `immutable` class members to avoid repeated construction:
  ```d
  immutable epoch = SysTime(DateTime.init);
  // Later: return (Clock.currTime - epoch).total!"msecs";
  ```

## JSON Value Access

- **Use `.boolean` not `.bool`** for JSON boolean access - `JSONValue["key"].boolean` is the correct property name.
- **Use `.count` not `.length`** on filtered ranges - `events.filter!(...).count` is preferred over `filter!(...).length` for lazy ranges.

## Data Structures

- **Use `Tuple!(T1, T2)`** instead of string-based encoding for paired data - `Tuple!(long, long)[string]` is cleaner than `"failures:total"` string parsing.
- **Prefer `tuple(a, b)`** for tuple construction over manual parsing.

## I/O Patterns

- **Use `std.stdio.File`** for file operations - `File(path, "a").writeln(data)` is idiomatic for append operations.
- **Use `std.datetime.Clock`** for timestamps - `(Clock.currTime - begin).total!"msecs"` provides millisecond precision. Optimize by caching `SysTime(DateTime.init)` as an `immutable` member.
- **Use `std.exception.collectException`** for error collection in loops - catches and aggregates exceptions without stopping iteration.
- **User is responsible for directory creation** - Don't over-engineer error handling for setup tasks like ensuring directories exist. Let the user create required directories.

## Sorting & Comparison

- **Use boolean comparison** for sort predicates - `sort!((a, b) { return rateA > rateB; })` is clearer than subtraction `rateB - rateA`.
- **Sort predicates return bool** - the sort function expects a boolean return, not a numeric difference.

## Statistics Calculation

- **Implement custom `mean()` and `sampleStdDev()`** instead of relying on `std.math` - provides more control and avoids dependency issues.
- **Sample standard deviation** uses `(N - 1)` denominator (Bessel's correction), not population stddev.
- **Use `std.math.sqrt` and `std.math.pow`** for math operations in custom functions.

## Module Organization

- **Use `private:` section label** to organize private module members clearly.
- **Consolidate related modules** under a common package (e.g., `llm.metric`) when components are tightly coupled.
- **Use PascalCase for class names** - `MetricMonitor` not `Monitor`.

## Constants Naming

- **Use PascalCase for class-level constants** - `MaxWarnings = 5` for instance-level constants.
- **Use SCREAMING_SNAKE_CASE for immutable module-level constants** - `MAX_EVENTS = 10_000` for module-level immutable values.

## Error Handling

- **Wrap I/O operations in try/catch** - Never let file persistence failures crash the agent.
- **Use `logger.tracef`** for internal debugging messages, not warnings.
- **Collect exceptions** when processing multiple items to prevent one failure from stopping all processing.
- **Use `format!"error: ..."`** consistently for error messages - makes them easily parseable and recognizable.
- **Null check before method calls** - Use `if (monitor !is null)` before calling monitor methods after error-isolated initialization.

## SumType for Non-Throwing Error Handling

- **Use `std.sumtype.SumType!(Success, Error)`** to return errors without throwing exceptions. This enables `nothrow` functions and forces callers to handle both cases explicitly.
- **Convention**: `SumType!(SuccessType, ErrorType)` — success first, error second. Construct with `SumType!(T,E)(value)` for success, `SumType!(T,E)(error)` for failure.
- **Pattern matching**: Use `.match!( (Success s) { ... }, (Error e) { ... } )` to destructure. Both branches must return the same type.
- **Idiomatic usage**: Replace throwing functions with SumType returns for operations that can fail (I/O, network, parsing). Callers must `.match` on the result, making error handling explicit and unavoidable.
- **Example**: `SumType!(JSONValue, LlamaRequestError) request(Chat chat) nothrow` — returns parsed JSON on success or a structured error on failure, never throws.

## Functional & Idiomatic D

- **Prefer `splitter.map.filter` chains** over manual foreach loops for data transformation.
- **Use `filter!`** for searching collections instead of manual foreach with if conditions.
- **`content.splitter('\n').filter!(a => !a.empty)`** - functional chain for parsing lines.
- **Use `.array`** to materialize lazy ranges when you need random access or multiple iterations.

## String Operations

- **`std.string.replace`** for simple pattern replacement - doesn't require regex.
- **`std.string.canFind`** for substring checking - cleaner than manual iteration.
- **Use `formattedWrite(buf, ...)`** instead of `format!` when building strings with `appender!string()`. More efficient - avoids intermediate string allocations.
- **`~=` operator** is not `@nogc` - allocates memory.
- **Don't over-engineer sanitization** - If a simple TODO stub is intentional, don't replace it with complex regex. Respect that some improvements are future work.

## Path Handling (Critical)

- **Always use `Path` type** for file paths, not `string`. Provides compile-time safety and prevents path manipulation bugs.
- **Use `~` operator** for Path concatenation - handles slashes correctly.
- **Use Path properties** like `.extension`, `.name` instead of string operations (`.endsWith()`, `baseName()`).
- **`SpanMode.shallow`** with `dirEntries` when filtering by extension - provides `.extension` property.
- **Reuse utilities from `llm.tool_call.utility`** - check there for existing validation/path functions before reimplementing.

## Input Validation

- **Validate user input early** before using in file operations - prevents security issues and runtime errors.
- **Use `checkAlphaNumUnderscore()`** for strategy/template names to prevent path traversal attacks.

## Common Pitfalls

- **Don't mix `std.file.write` with newline concatenation** - use `File.writeln()` instead of `write(data ~ "\n", "a")`.
- **Don't use `std.math.mean`/`stddev`** without checking version compatibility - custom implementations are more reliable.
- **Don't use string-based data encoding** when tuples or structs exist - use `Tuple!(T1, T2)[string]` instead of `"a:b"` string parsing.
- **Don't over-flag initialization issues** - Be careful about flagging uninitialized variables as critical without confirming the D version and context.
- **Don't over-engineer error isolation** - Simple try/catch is sufficient. Don't add unnecessary complexity like directory creation checks when the user handles setup.

## Tooling & Build

- **DUB configuration matters** - `dub.sdl` must exist in the package root for `executeDCodeWithDub` to work.
- **Source files reference** - Ensure `sourceFiles` in dub.sdl match actual library paths (symlinks vs versioned files).
- **Modules compile in isolation poorly** - The `executeCode` tool compiles single files without dependencies. Use `executeDCodeWithDub` from the package root instead.
