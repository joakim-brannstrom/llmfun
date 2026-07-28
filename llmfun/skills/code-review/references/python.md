# Python Code Review Checklist

## Imports

- **Unused imports**: Flag any imports not used in the file.
- **Import order**: Standard library → Third-party → Local application (PEP 8 convention).
- **Wildcard imports**: `from module import *` should be avoided — obscures namespace and makes dependencies unclear.
- **Circular imports**: Check for import cycles that cause `ImportError` at runtime.
- **Relative imports**: Prefer explicit absolute imports for clarity; relative imports (`from . import x`) are acceptable within packages.

## Type Hints

- **Function signatures**: Check for consistent type annotations on parameters and return types.
- **`typing` module usage**: Prefer built-in generics (`list[str]`, `dict[str, int]`) over `typing.List`, `typing.Dict` in Python 3.9+.
- **`Optional` vs `| None`**: Use `x | None` union syntax (Python 3.10+) instead of `Optional[x]`.
- **`Any` overuse**: Flag excessive use of `Any` — it defeats type checking. Use `TypeVar` or protocols when possible.

## Error Handling

- **Bare `except:`**: Always catch specific exceptions, never bare `except:`. Use `except Exception:` as minimum.
- **Silent swallowing**: Flag `except: pass` without logging — hides bugs.
- **Exception chaining**: Use `raise NewError() from e` to preserve traceback context.
- **`try/finally` vs context managers**: Prefer `with` statements over manual `try/finally` for resource management.
- **Custom exceptions**: Should inherit from `Exception`, not `BaseException`.

## Resource Management

- **File handles**: Always use `with open(...) as f:` — never bare `open()` without explicit `close()`.
- **Database connections**: Use context managers or ensure `close()` in `finally` blocks.
- **Thread locks**: Use `with lock:` — never manual `acquire()`/`release()` without `finally`.
- **Generators**: Flag generators that allocate resources without `contextlib.closing` or proper cleanup.

## Concurrency

- **GIL awareness**: CPU-bound tasks should use `multiprocessing`, not `threading`.
- **Async/await consistency**: Don't mix sync and async code in the same call chain. Flag `await` in non-`async` functions.
- **Race conditions**: Shared mutable state between threads/async tasks needs proper synchronization.
- **Deadlock risk**: Multiple locks acquired in inconsistent order.

## Performance

- **String concatenation in loops**: Use `''.join(list)` instead of `s += chunk` in loops.
- **List comprehensions**: Prefer `[f(x) for x in items]` over `map(f, items)` for readability (unless `map` is faster for built-ins).
- **`is` vs `==`**: Use `is` for `None`, `True`, `False` identity checks. Use `==` for value comparison.
- **`in` operator**: Prefer `x in set` (O(1)) over `x in list` (O(n)) for membership testing.
- **`__slots__`**: Consider for classes with many instances and fixed attributes — reduces memory usage.

## Code Quality (PEP 8)

- **Line length**: Max 79 characters (or 88 for Black formatter).
- **Naming**: `snake_case` for functions/variables, `PascalCase` for classes, `UPPER_SNAKE_CASE` for constants.
- **Whitespace**: No trailing whitespace. Blank lines: 2 around top-level functions/classes, 1 between methods.
- **Docstrings**: Use triple quotes. Module, class, and function docstrings should follow a consistent style (Google, NumPy, or Sphinx).

## Common Pitfalls

- **Mutable default arguments**: `def f(x=[])` — the list is shared across calls. Use `def f(x=None): x = x or []`.
- **Late binding closures**: Loop variable captured in lambda/closure — all closures reference the same variable. Use `lambda x=i: x` to capture current value.
- **`for` loop variable leakage**: Loop variable persists after loop. Don't rely on it being undefined.
- **Shallow copy**: `copy.copy()` doesn't recurse — nested mutable objects are shared. Use `copy.deepcopy()` when needed.
- **`dict` mutation during iteration**: Causes `RuntimeError`. Iterate over a copy: `for k in list(d.keys()):`.
- **`== None` vs `is None`**: Always use `is None` — `==` can be overridden by `__eq__`.
- **Global variable modification**: `global` keyword required to reassign globals inside functions. Missing it creates a local variable instead.

## Testing

- **`assert` in production**: `assert` is stripped with `-O` flag. Never use for validation — use `if/raise`.
- **Test isolation**: Tests should not depend on execution order or shared state.
- **Mock usage**: Verify mocks are properly patched at the correct location (where used, not where defined).
