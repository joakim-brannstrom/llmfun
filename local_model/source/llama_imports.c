/*
 * C import file for llama.cpp C API.
 *
 * D code imports this file via the -P preprocessor flag, making all
 * llama.cpp C symbols available without manual extern(C) bindings.
 *
 * NOTE: This relies on the D compiler's -P flag to treat preprocessed
 * output as extern(C) declarations. If changing compilers or build
 * systems, explicit extern(C) wrapping via a .di file may be needed.
 *
 * License: MPL-2.0
 */
#include <stdbool.h>

#include "llama.h"
