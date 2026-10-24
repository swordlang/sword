#pragma once

#include <string>
#include <vector>

// `shield new [--lib] <module path> [package name]`: a directory holding a
// sword.mod, a program or a library, and a test for it.
int new_command(const std::vector<std::string> &args);
