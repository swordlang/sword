#pragma once

#include "diag.h"

#include <string>
#include <vector>

// A module is a tree of packages with a sword.mod at its root, named by the
// path it is imported under: `github.com/ana/sqlite`, and its packages below it
// (`github.com/ana/sqlite/pool`).
struct Module {
  std::string path; // what `module` says
  std::string root; // the directory sword.mod is in
  int sword_major = 0, sword_minor = 0;

  struct Require {
    std::string path;
    std::string version;
    Pos at;
  };
  std::vector<Require> required;

  // `replace path => dir`, with dir made absolute against the module's root.
  struct Replace {
    std::string path;
    std::string dir;
    Pos at;
  };
  std::vector<Replace> replaces;

  // The directory an import path names inside this module or through one of its
  // replacements, or empty when it is neither.
  std::string resolve(const std::string &import_path) const;
};

// Looks for sword.mod in `dir` and every directory above it. Answers false only
// when one was found and could not be read; `found` says whether there was one.
bool find_module(const std::string &dir, Module &out, bool &found);

// The sword.mod at the root of a dependency, which has to be there.
bool read_module(const std::string &root, Module &out);

// Whether `version` reads as a version: v1.4.0, or a pseudo-version for a
// commit with no tag, v0.0.0-20261023-a1b2c3d4e5f6.
bool valid_version(const std::string &version);

// Negative, zero or positive as `a` comes before, is, or comes after `b`, in
// semantic version order: a pre-release comes before its release, which is
// what puts a pseudo-version before every tag.
int compare_versions(const std::string &a, const std::string &b);

// The major version a version string starts with.
int version_major(const std::string &version);

// The major version a path carries in its last element, `.../v2`, or 0.
int path_major(const std::string &path);

// Whether a version may be required under `path`: v0 and v1 under a plain
// path, vN under `.../vN`.
bool major_fits(const std::string &path, const std::string &version);

bool valid_module_path(const std::string &path);

// Whether `import_path` is `prefix` or a package below it.
bool path_under(const std::string &import_path, const std::string &prefix);

// The package name an import path gives, which is its last element unless that
// is a major version (`.../sqlite/v2` is package sqlite).
std::string package_name_of(const std::string &import_path);
