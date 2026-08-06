# D Language: Scope Reduction with Local Functions

## Principle

Minimize variable scope. Tighter scope means fewer dependencies, clearer intent, and easier refactoring.

## Pattern: Extract to Local Function

When a block of code uses only a subset of the surrounding variables, extract it into a **local function**. Pass only the variables it needs as parameters. This makes data flow explicit and prevents accidental use of unrelated variables.

### Non-static local (closure)

A non-static local function captures variables from the enclosing scope by reference. Use this when the extracted code needs to **read and write** outer variables.

```d
// Before: var1 and var2 both in scope, unclear which is used where
void process(double a, double b) {
    int result;
    int temp;

    if (b > 0) {
        // Long complex block that only uses `result`, not `temp`
        result = heavyComputation(a, b);
        result *= 2;
        // ... more code ...
    }

    // `temp` used here
    temp = result + 1;
}

// After: `result` is explicitly captured, scope is clear
void process(double a, double b) {
    int result;
    int temp;

    void computeResult() {
        // Captures `result` by reference (read/write access)
        result = heavyComputation(a, b);
        result *= 2;
        // ... more code ...
    }

    if (b > 0) {
        computeResult();
    }

    temp = result + 1;
}
```

### Static local (preferred)

A `static` local function has **no access** to outer scope variables. All dependencies must be passed as parameters. This is the preferred style — it forces explicit data flow and avoids hidden dependencies.

```d
// Before: scattered logic, hard to see what each part needs
void process(double a, double b) {
    int result;
    int temp;

    if (b > 0) {
        result = heavyComputation(a, b);
        result *= 2;
        // ... more code ...
    }

    temp = result + 1;
}

// After: dependencies explicit via parameters, return value
void process(double a, double b) {
    int result;
    int temp;

    static int computeResult(double a, double b) {
        // No access to outer scope — all inputs are parameters
        int r = heavyComputation(a, b);
        r *= 2;
        // ... more code ...
        return r;
    }

    if (b > 0) {
        result = computeResult(a, b);
    }

    temp = result + 1;
}
```

## Comparison with Python

This pattern mirrors Python's nested function (closure) pattern:

```python
# Python equivalent of the non-static local
def process(a, b):
    result = 0
    temp = 0

    if b > 0:
        def compute_result():
            nonlocal result  # Explicit capture, like D's non-static local
            result = heavy_computation(a, b)
            result *= 2

        compute_result()

    temp = result + 1
```

## Guidelines

- **Prefer `static` locals** — they force explicit dependencies and are easier to reason about.
- **Use non-static locals** only when you genuinely need to modify outer variables (e.g., accumulating results).
- **Pass only what's needed** — if a local function doesn't use a variable, don't capture it.
- **Keep locals small** — if a local function grows beyond ~30 lines, consider extracting it to a module-level function instead.
