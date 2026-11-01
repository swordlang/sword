#pragma once

#include "ir.h"
#include "package.h"
#include "types.h"

// Build modes trade safety for speed, the way Zig's release modes do.
enum Mode {
  MODE_DEBUG, // checks on, no optimization
  MODE_SAFE,  // checks on, optimized
  MODE_FAST,  // checks off
  MODE_SMALL, // checks off, optimized for size
};

inline bool mode_checks(Mode mode) {
  return mode == MODE_DEBUG || mode == MODE_SAFE;
}

// Every way lowering drops a check, moves it or orders a sum, each of which
// can be switched off to measure what it buys (`--without=`), and `audit`
// (`--audit`), which keeps every bounds check that was dropped as one that
// aborts with a message of its own: a program that runs the same with it
// never needed one of them. Neither is part of the language; they are there
// for measuring the compiler.
struct Tuning {
  bool walked = true;     // `xs[i]` in `for i in a..xs.len`, `xs` immutable
  bool walked_mut = true; // the same in a `parallel for` with `xs` `mut`
  bool captures = true;   // a `parallel for`'s read-only captures copied in
  bool versioned = true;  // indexing tested once, before the loop
  bool split = true;      // the range split where `xs[j ± c]` could fall out
  bool deferred = true;   // an accumulating loop's overflow settled at the end
  bool lanes = true;      // a float `reduce(+)` in sixteen sums
  bool audit = false;
};

// Sets the piece `name` names off; false when no piece is called that.
bool tuning_without(Tuning &tuning, const std::string &name);

void lower(Program &prog, TypeTable &types, Mode mode, IrModule &mod,
           const Tuning &tuning = Tuning{});
