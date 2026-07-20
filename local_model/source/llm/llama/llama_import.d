/**
 * Facade module that re-exports the C import bindings from `llama_imports`.
 *
 * The C import file `source/llama_imports.c` provides D with access to the
 * llama.cpp C API via the `-P` preprocessor flag. Its module name is
 * `llama_imports` (derived from the filename). This facade exists so that
 * consumer code (e.g. `llm.rag.embedder_llama`) can use the expected
 * module path `llm.llama.llama_import` instead of relying on the raw
 * source-root module name.
 *
 * License: MPL-2.0
 */
module llm.llama.llama_import;

public import llama_imports;
