# C++ Code Review Checklist

## Memory Management

- **RAII compliance**: Resources (files, locks, memory, sockets) must be managed by objects with destructors. Flag raw `new`/`delete` or `malloc`/`free`.
- **Smart pointers**: Prefer `std::unique_ptr` (exclusive ownership) and `std::shared_ptr` (shared ownership). Flag raw owning pointers.
- **Circular references**: `shared_ptr` cycles cause memory leaks. Use `weak_ptr` to break cycles.
- **Rule of Five**: If a class defines any of destructor, copy constructor, copy assignment, move constructor, or move assignment — define all five.
- **Dangling pointers**: Flag pointers to stack-allocated objects returned from functions.
- **Double-free/use-after-free**: Check that ownership is clearly transferred, not shared implicitly.

## Const Correctness

- **`const` member functions**: Methods that don't modify state must be `const`.
- **`const` parameters**: Pass by `const T&` for read-only reference parameters.
- **`constexpr`**: Use for compile-time constants and functions. Prefer over `#define` macros.
- **`inline static constexpr`** (C++17+): For non-integral types in headers to avoid ODR violations. Without `inline`, each TU gets a separate copy.
- **`const` correctness in APIs**: Public APIs should expose `const` references, not mutable ones, when mutation isn't needed.

## Modern C++ Practices

- **`auto` usage**: Use `auto` for complex types but avoid when type clarity matters (e.g., `auto x = 0;` — is it `int`?).
- **Range-based for loops**: Prefer `for (auto& x : container)` over iterator loops.
- **`std::optional`** (C++17): Prefer over raw pointer or sentinel values for optional return values.
- **`std::variant`** (C++17): Prefer over `union` or base class polymorphism for type-safe unions.
- **`std::string_view`** (C++17): Use for read-only string parameters to avoid unnecessary copies.

## Exception Safety

- **Exception specifications**: Avoid `throw()` / `noexcept(false)`. Use `noexcept` when function truly cannot throw.
- **Strong exception guarantee**: Operations should either succeed or leave state unchanged. Flag partial updates on failure.
- **Destructor safety**: Destructors must be `noexcept`. Never throw from a destructor.
- **RAII for exception safety**: Resources acquired in constructor must be released in destructor — exceptions cannot leak resources.

## Template Metaprogramming

- **SFINAE vs concepts** (C++20): Prefer concepts for cleaner constraints. Flag overly complex SFINAE patterns.
- **Template instantiation bloat**: Deep template nesting can cause long compile times and large binaries.
- **`typename` keyword**: Required when accessing dependent types. Missing `typename` causes compilation errors.

## Common Pitfalls

- **Missing closing braces**: "qualified-id in declaration before '(' token" often means a missing `}` from a previous function. Trace brace depth to find the issue.
- **Signed/unsigned comparison**: Comparing `int` with `size_t` triggers warnings and can cause logic errors. Cast explicitly or use matching types.
- **Integer overflow**: Signed integer overflow is UB. Use `std::add_overflow` or check bounds before arithmetic.
- **Undefined behavior**: Uninitialized variables, out-of-bounds access, null pointer dereference, signed overflow.
- **Copy vs move**: Large objects should be moved (`std::move`) rather than copied. Flag unnecessary copies in function parameters/returns.
- **Virtual destructor**: Base classes with virtual functions must have virtual destructors.
- **vtable corruption**: Multiple inheritance with virtual base classes — ensure proper constructor initialization order.

## Header Files

- **Include guards**: `#pragma once` or traditional `#ifndef` guards required.
- **Forward declarations**: Prefer forward declarations over `#include` when possible — reduces dependencies and compile time.
- **Header dependencies**: Headers should be self-contained — include all dependencies they need.
- **Implementation in headers**: Only inline functions and templates in headers. Flag method definitions that don't belong.

## Build System

- **Compilation units**: Flag code that only compiles in specific build configurations without clear rationale.
- **ODR violations**: Same symbol defined differently across TUs. Use `inline` or move definitions to `.cpp` files.
- **Linker errors**: Unresolved externals often mean missing `#include` or incorrect template instantiation.

## UI Framework: ImGui (if applicable)

- **`TextUnformatted` multi-line**: Only applies cursor position/indent to the FIRST line. Subsequent lines reset to column 0. Split on `\n` and call once per line.
- **`CollapsingHeader`**: Never calls `TreePush` — no indentation scopes. Use `TreeNode` + `TreePop` for nesting, or manual `Indent()`/`Unindent()`.
