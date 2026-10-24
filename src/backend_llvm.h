#pragma once

#include "ir.h"

#include <cstdio>
#include <string>
#include <vector>

// Emits textual LLVM IR. Keeping the backend behind this one call is what
// makes swapping in the in-process LLVM API later a contained change.
void emit_llvm(const IrModule &mod, FILE *out);

// A split build: one module per unit (IrFunc::unit), each defining only its
// own functions and declaring what it calls in the others.
std::vector<std::string> llvm_units(const IrModule &mod);
void emit_llvm_unit(const IrModule &mod, const std::string &unit, FILE *out);
