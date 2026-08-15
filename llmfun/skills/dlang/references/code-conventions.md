# D Code Conventions

## Comments

- **Use ddoc comments, NOT doxygen comments.** Ddoc forms: `/** ... */`, `///`, `/+ ... +/`.
- **Module header:** Every module must start with a brief ddoc comment (1-3 lines) introducing the module and what it does, placed before the `module` declaration.
- **Comments explain why, not what.** If the code is self-documenting, the comment is redundant.
- **Concise:** 1-2 lines max. Break longer explanations into separate short comments.
- **No hard-wrapping:** Do not wrap to a fixed column width. Let lines flow naturally.
- **No mid-sentence breaks:** Each comment line should be a complete thought.
- **Simple language:** Short words, short sentences.
- **Write code first, then add comments** only where genuinely needed.
- **Never add comments to copied code** that weren't there originally.

## String Handling

- **Prefer interpolated strings** over `std.format.format` / `formattedWrite`:

  ```d
  // Good
  auto msg = i"Found $(count) results for $(query)".text;

  // Bad
  auto msg = format!"Found %s results for %s"(count, query);
  ```

- **Prefer backtick-strings** when embedding `"` or `\`:

  ```d
  // Good
  auto path = `foo "embed" bar`;

  // Bad
  auto path = "foo \"embed\" bar";
  ```

## Variable Initialization

- **NEVER initialize `string` with `= ""`.** D auto-initializes all locals:

  ```d
  // Good
  string name;

  // Bad
  string name = "";
  ```

- **Prefer local function initialization** over unnecessary variable declarations:

  ```d
  // Good
  auto status = () {
      if (isOk) return "ok";
      if (isWarning) return "warn";
      return "error";
  }();

  // Bad
  string status;
  if (isOk) status = "ok";
  else if (isWarning) status = "warn";
  else status = "error";
  ```

## Naming & Formatting

- **K&R brace style** (opening brace on the same line)
- **Local imports** inside functions/structs where symbols are not pervasive
- **No magic numbers** without named constants
- **No wrapper functions** for stdlib symbols — use local imports at point of use

## Error Handling

- **No empty catch blocks.** Always log or handle caught exceptions:

  ```d
  catch (Exception e) {
      import std.logger : trace;
      trace(e.msg);
  }
  ```

- **Logging:** Use `std.logger` (not `stderr`) for diagnostic output.
- **Silent catches for @safe:** When `.collectException` cannot be used (e.g., due to
  `@safe` violations), wrap the throwing code in try/catch and nest another
  try/catch around the logging call. The innermost catch may be empty — this is
  the only place where an empty catch block is allowed:

  ```d
  try {
      // code that may throw
  } catch (Exception e) {
      try {
          import std.logger : trace;
          trace(e.msg);
      } catch (Exception innerE) {
          // Empty inner catch allowed — keeps the parent @safe and nothrow
      }
  }
  ```

## Attributes

- **@safe:** Mark functions `@safe` whenever possible. If the compiler rejects it,
  fix the underlying issue rather than downgrading the safety level.

  ```d
  // Good — simple, obviously safe
  bool isAgentMdTopic(string topic) @safe pure nothrow { ... }

  // Bad — unmarked function that could be @safe
  bool isAgentMdTopic(string topic) { ... }
  ```

- **@trusted:** Use only when `@safe` is not feasible. `@trusted` bridges `@safe`
  and `@system` — it must verify all inputs and ensure operations are memory-safe
  before exposing a `@safe` interface. Keep `@trusted` code minimal.

- **@system:** The default safety level. Avoid unless dealing with low-level
  operations (raw pointers, assembly, C interop). Never expose `@system`
  through a `@safe` interface without `@trusted` wrapping.

- **pure:** Add `pure` to functions that do not access or modify global/mutable
  state beyond their parameters.

- **nothrow:** Mark functions `nothrow` when they do not throw exceptions. Use it
  to signal error-return patterns (returning `SumType`, error codes, etc.) and
  let the compiler enforce the contract.

## Global State & Threading

- **Module-level globals are thread-local (TLS) by default.** Each thread gets
  its own copy. A global registry filled on one thread appears empty on other
  threads. Mark cross-thread globals `__gshared` (raw shared storage, no
  safeguards) or `shared` (compiler-checked access):

  ```d
  int counter;           // TLS: per-thread copy
  __gshared int gcount;  // shared across threads
  shared int scount;     // shared, compiler-checked
  ```

- **`static this()` runs once per thread.** Use `shared static this()` for
  program-wide initialization.

## General

- **ASCII only.** Avoid emdash, unicode arrows, or any non-ASCII characters. Use `-`, `->`, `x`, `...`.
- **Do not split lines mid-sentence** or force lines to fit a fixed character width.
- **Prefer reusing existing infrastructure** over introducing new components.
