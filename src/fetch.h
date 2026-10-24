#pragma once

#include "module.h"

#include <string>
#include <vector>

// A module a build reads besides its own, at the version minimal version
// selection settled on, and the directory its files are in.
struct Dep {
  std::string path;
  std::string version;
  std::string dir;
  bool replaced = false;
  // What its own sword.mod requires, which is all its packages may import
  // beyond themselves and the standard library.
  std::vector<std::string> required;
};

struct Deps {
  std::vector<Dep> modules;
  // The module an import path falls under, or null.
  const Dep *owner(const std::string &import_path) const;
};

struct Settle {
  // The editor never reaches the network: what is not in the cache yet stays
  // missing until a build fetches it.
  bool cache_only = false;
  // --locked: refuse to add anything to sword.sum, for a build that has to use
  // exactly what was committed.
  bool locked = false;
  // shield vendor reads the cache even when vendor/ is there, to rewrite it.
  bool ignore_vendor = false;
  // shield get drops the sword.sum lines nothing uses any more.
  bool tidy = false;
};

// ~/.sword, or SWORD_CACHE: fetched modules under mod/, built objects under
// build/.
std::string cache_root();

// Picks a version of everything `main` requires, directly or through another
// module, fetches what the cache is missing, and checks each against sword.sum.
bool settle_deps(const Module &main, const Settle &how, Deps &out);

// `shield get <path>[@version|@none]` and `shield vendor`.
int get_command(const std::vector<std::string> &args);
int vendor_command(const std::vector<std::string> &args);
