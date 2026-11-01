#pragma once

#include "package.h"
#include "types.h"

#include <string>
#include <utility>
#include <vector>

// What `shield det` lists: every function of the package being compiled,
// whether it is det, and when it is not, the chain from it to what took det
// away. A generic is det when every instance the program makes of it is.
struct DetListing {
  struct Function {
    Pos at;
    std::string name;
    bool det = true;
    // A library's function that is det only when what it is handed through
    // these interfaces is.
    std::vector<std::string> unless;
    std::vector<std::pair<Pos, std::string>> why;
  };
  std::vector<Function> functions;
  int uninstantiated = 0; // generics nothing in the program instantiates
};

// Checks every package in dependency order. Names are qualified with the
// package prefix as they are declared, so the rest of the compiler never has
// to think about packages again. With a listing to fill, the package being
// compiled may be a library: it needs no main.
bool check(Program &prog, TypeTable &types, DetListing *listing = nullptr);
