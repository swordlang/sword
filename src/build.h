#pragma once

#include "ir.h"

#include <string>
#include <vector>

// A split build: every package becomes an object of its own, kept in the cache
// under the hash of its LLVM IR and of how it was compiled, so a package that
// did not change is not handed to LLVM again. The objects come back in `objects`
// for the link.
bool compile_split(const IrModule &mod, const std::string &opt_level,
                   const std::string &target, const std::string &scratch,
                   std::vector<std::string> &objects);

// What the linker needs told to drop the functions nothing calls, which a
// whole-program module left to LLVM.
std::vector<std::string> split_link_flags(const std::string &target);

// `shield clean`: everything --split kept. Objects nothing has used for thirty
// days go by themselves.
int clean_command(const std::vector<std::string> &args);
