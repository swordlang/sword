#include "package.h"

#include "fetch.h"
#include "lexer.h"
#include "module.h"
#include "parser.h"

#include <algorithm>
#include <cctype>
#include <dirent.h>
#include <sys/stat.h>

namespace {

std::unordered_map<std::string, std::string> overlays;

bool is_directory(const std::string &path) {
  struct stat info;
  return stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

std::string parent_of(const std::string &path) {
  size_t cut = path.find_last_of('/');
  return cut == std::string::npos ? std::string(".") : path.substr(0, cut);
}

bool ends_with(const std::string &name, const std::string &suffix) {
  return name.size() > suffix.size() &&
         name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool is_test_file(const std::string &name) {
  return ends_with(name, "_test.sword");
}

// Sword files used to end in .sw, which GitHub counts as Sway. One left behind
// would otherwise be skipped without a word, and whatever it declared would
// turn up as undefined somewhere else, so it stops the build and says why.
bool reject_stale(const std::string &path) {
  if (!ends_with(path, ".sw")) return false;
  fprintf(stderr, "shield: '%s' ends in .sw; Sword files end in .sword\n",
          path.c_str());
  return true;
}

// The package's files, or false when a file in the directory was refused.
bool sword_files(const std::string &dir, bool with_tests,
                 std::vector<std::string> &files) {
  std::vector<std::string> stale;
  DIR *handle = opendir(dir.c_str());
  if (!handle) return true;
  while (dirent *entry = readdir(handle)) {
    std::string name = entry->d_name;
    if (ends_with(name, ".sw")) stale.push_back(dir + "/" + name);
    if (!ends_with(name, ".sword")) continue;
    // Tests live beside what they test, and an ordinary build must not drag
    // them into every program that imports the package.
    if (!with_tests && is_test_file(name)) continue;
    files.push_back(dir + "/" + name);
  }
  closedir(handle);
  // Directory order is not stable across filesystems; sort so that diagnostics
  // and generated code come out the same every time.
  std::sort(files.begin(), files.end());
  std::sort(stale.begin(), stale.end());
  return stale.empty() || !reject_stale(stale.front());
}

struct Loader {
  Program &prog;
  const std::vector<std::string> &search;
  std::vector<std::string> visiting;
  LoadMode mode = LOAD_BUILD;
  std::string label; // what the user named on the command line
  std::vector<std::string> only; // the tests asked for by name; empty is all
  // The module the program belongs to, when it has a sword.mod.
  Module module;
  bool in_module = false;
  // Everything the module requires, at the versions selected for this build.
  Deps deps;

  Loader(Program &p, const std::vector<std::string> &s) : prog(p), search(s) {}

  // The entry point of a test build, written as source and handed to the same
  // parser as everything else. Generating text rather than nodes keeps this
  // honest: whatever the language accepts, this has to be written in.
  bool add_test_main(Package &pkg) {
    // The same entry point either way: a benchmark is a function found by its
    // name, handed to a runner, like a test.
    bool benching = mode == LOAD_BENCH;
    const std::string prefix = benching ? "Benchmark" : "Test";
    const char *what = benching ? "benchmark" : "test";

    std::vector<std::string> found;
    for (Node *decl : pkg.unit->kids) {
      if (decl->kind != ND_FUNC || decl->lhs || decl->is_extern) continue;
      // The prefix on its own is not one of them, and each takes exactly the
      // one parameter. Whether it is the right type is the checker's business.
      if (decl->name.size() <= prefix.size() ||
          decl->name.compare(0, prefix.size(), prefix) != 0)
        continue;
      if (decl->kids.size() != 1) continue;
      found.push_back(decl->name);
    }
    if (found.empty()) {
      fprintf(stderr, "shield: no '%s...' functions in '%s'\n",
              prefix.c_str(), label.empty() ? pkg.dir.c_str() : label.c_str());
      return false;
    }
    // A name that matches nothing is a mistake, not a run of zero tests that
    // passes: a typo must not look like success.
    for (const std::string &name : only) {
      if (std::find(found.begin(), found.end(), name) != found.end()) continue;
      fprintf(stderr, "shield: no %s '%s' in '%s'\n", what, name.c_str(),
              label.empty() ? pkg.dir.c_str() : label.c_str());
      return false;
    }
    if (!only.empty()) {
      std::vector<std::string> picked;
      for (const std::string &name : found)
        if (std::find(only.begin(), only.end(), name) != only.end())
          picked.push_back(name);
      found = picked;
    }

    // Testing a program means not running it: its own main is renamed out of
    // reach rather than colliding with the one below. The dot is what keeps it
    // out of reach; a space would also be, but it ends a name in LLVM's text.
    for (Node *decl : pkg.unit->kids)
      if (decl->kind == ND_FUNC && !decl->lhs && decl->name == "main")
        decl->name = "main.under_test";

    const char *kind = benching ? "Bench" : "Case";
    const char *make = benching ? "NewBench" : "NewCase";
    const char *run = benching ? "RunBenches" : "Run";
    std::string src = "import \"std/testing\"\n\nfunc main() !int {\n";
    src += "    mut cases := [" + std::to_string(found.size()) + "]testing." +
           kind + "{";
    for (size_t i = 0; i < found.size(); i++) {
      src += i ? ",\n        " : "\n        ";
      src += std::string("testing.") + make + "(\"" + found[i] + "\", " +
             found[i] + ")";
    }
    src += "}\n    return try testing." + std::string(run) +
           "(cases[..])\n}\n";

    int id = add_source("<test main>", src);
    std::vector<Token> tokens = lex(id);
    if (error_count() > 0) return false;
    if (!parse(tokens, prog.ast, pkg.unit)) return false;

    for (Node *decl : pkg.unit->kids)
      if (decl->kind == ND_IMPORT && decl->text == "std/testing")
        pkg.imports.push_back(decl->text);
    return true;
  }

  bool parse_files(Package &pkg, const std::vector<std::string> &files) {
    if (files.empty()) {
      fprintf(stderr, "shield: no .sword files in '%s'\n", pkg.dir.c_str());
      return false;
    }
    pkg.unit = prog.ast.make(ND_UNIT, Pos{});
    for (const std::string &file : files) {
      auto edited = overlays.find(file);
      int id = edited != overlays.end() ? add_source(file, edited->second)
                                        : load_source(file);
      if (id < 0) {
        fprintf(stderr, "shield: cannot open '%s'\n", file.c_str());
        return false;
      }
      std::vector<Token> tokens = lex(id);
      if (error_count() > 0) return false;
      // `chan[T]` is written like a type and implemented like a package, and
      // nobody should have to import it to use a language feature. Noticing the
      // word here is enough: the import is added below, and an extra one for a
      // file that only mentions `chan` in passing costs a package nobody calls.
      for (const Token &t : tokens)
        if (t.kind == TK_IDENT && t.text == "chan") pkg.needs_chan = true;
      if (!parse(tokens, prog.ast, pkg.unit)) return false;
    }

    for (Node *decl : pkg.unit->kids)
      if (decl->kind == ND_IMPORT) pkg.imports.push_back(decl->text);
    if (pkg.needs_chan && pkg.import_path != "std/chan")
      pkg.imports.push_back("std/chan");
    return true;
  }

  std::string locate(const std::string &import_path) {
    // The standard library is found where it always was; a module cannot be
    // named under `std`, so nothing below can shadow it.
    if (in_module && import_path.compare(0, 4, "std/") != 0) {
      std::string dir = module.resolve(import_path);
      if (dir.empty())
        if (const Dep *dep = deps.owner(import_path))
          dir = dep->dir + import_path.substr(dep->path.size());
      if (!dir.empty()) return is_directory(dir) ? dir : "";
    }
    for (const std::string &root : search) {
      std::string candidate = root + "/" + import_path;
      if (is_directory(candidate)) return candidate;
    }
    return "";
  }

  static bool identifier(const std::string &name) {
    if (name.empty() || !(isalpha((unsigned char)name[0]) || name[0] == '_'))
      return false;
    for (char c : name)
      if (!(isalnum((unsigned char)c) || c == '_')) return false;
    return true;
  }

  // What importers call a package: the last element of its path, which the
  // `package` clause has to agree with when there is one. A repository is
  // often named `sword-sqlite`, which no program could write before a dot, and
  // then the clause is what names it.
  bool settle_name(Package &pkg) {
    Node *said = nullptr;
    for (Node *decl : pkg.unit->kids) {
      if (decl->kind != ND_PACKAGE) continue;
      if (said && said->name != decl->name) {
        error(decl->name_pos, "this file says package '%s', and another in "
                              "the same directory says '%s'",
              decl->name.c_str(), said->name.c_str());
        note(said->name_pos, "the other is here");
        return false;
      }
      if (!said) said = decl;
    }
    if (identifier(pkg.name)) {
      if (said && said->name != pkg.name) {
        error(said->name_pos, "this says package '%s', but it is imported as "
                              "'%s', and that is the name its importers write",
              said->name.c_str(), pkg.import_path.c_str());
        return false;
      }
      return true;
    }
    if (!said) {
      Pos at = pkg.unit->kids.empty() ? Pos{} : pkg.unit->kids.front()->pos;
      error(at, "'%s' cannot be written in a program; give the package a name "
                "with 'package <name>'",
            pkg.name.c_str());
      return false;
    }
    pkg.name = said->name;
    return true;
  }

  // Why an import was not found, when a module has something to say about
  // it: a package the module does not have, one the editor cannot see until a
  // build has fetched it, or a module nobody required.
  void required_note(const std::string &path, Pos at) {
    if (!in_module) return;
    for (const Module::Require &r : module.required) {
      if (!path_under(path, r.path)) continue;
      const Dep *dep = deps.owner(path);
      if (dep && dep->path == r.path)
        note(r.at, "%s %s has no package '%s'", r.path.c_str(),
             dep->version.c_str(), path.c_str());
      else
        note(r.at, "%s is required here, and a build fetches it", r.path.c_str());
      return;
    }
    // A host name comes first in a module path, and nothing else has a dot.
    std::string first = path.substr(0, path.find('/'));
    if (first.find('.') != std::string::npos)
      note(at, "sword.mod does not require it; 'shield get <module>' adds the "
               "module it is in");
  }

  // A package imports from its own module, the standard library and what its
  // module's sword.mod requires, and nothing else: no dependency enters a
  // build without somebody having asked for it.
  bool importable(const Package &pkg, const std::string &path) {
    if (!in_module || path_under(path, "std")) return true;
    if (pkg.import_path.empty() || path_under(pkg.import_path, module.path)) {
      if (path_under(path, module.path)) return true;
      for (const Module::Replace &r : module.replaces)
        if (path_under(path, r.path)) return true;
      for (const Module::Require &r : module.required)
        if (path_under(path, r.path)) return true;
      // The program's own directory and -I are searched as they always were.
      const Dep *other = deps.owner(path);
      if (!other) return true;
      std::string what = path == other->path
                             ? "'" + path + "'"
                             : "'" + path + "' is in " + other->path + ", which";
      error(import_pos(pkg, path), "%s is required only by another module; "
                                   "'shield get %s' requires it here",
            what.c_str(), other->path.c_str());
      return false;
    }
    const Dep *owner = deps.owner(pkg.import_path);
    if (!owner || path_under(path, owner->path)) return true;
    for (const std::string &r : owner->required)
      if (path_under(path, r)) return true;
    error(import_pos(pkg, path), "%s imports '%s', and its sword.mod does not "
                                 "require it",
          owner->path.c_str(), path.c_str());
    return false;
  }

  // A package is called by its name, so two imports that give the same one
  // would leave the second unreachable, and every use quietly meaning the
  // first: `example.com/deep` and `example.com/deep/v2` are both `deep`. One of
  // them has to be named at its import.
  bool name_imports(Package &pkg) {
    auto take = [&](const std::string &name, const std::string &path,
                    Pos at) {
      auto had = pkg.import_names.emplace(name, path);
      if (had.second || had.first->second == path) return true;
      error(at, "'%s' and '%s' are both called '%s'; give one of them another "
                "name where it is imported: import other \"%s\"",
            had.first->second.c_str(), path.c_str(), name.c_str(), path.c_str());
      return false;
    };
    for (Node *decl : pkg.unit->kids) {
      if (decl->kind != ND_IMPORT) continue;
      Package *other = prog.by_path[decl->text];
      if (!other) continue;
      if (!take(decl->name.empty() ? other->name : decl->name, decl->text,
                decl->name.empty() ? decl->pos : decl->name_pos))
        return false;
    }
    // An import no file wrote, the way `chan` arrives, goes by its own name.
    for (const std::string &path : pkg.imports) {
      Package *other = prog.by_path[path];
      bool written = false;
      for (const auto &named : pkg.import_names) written |= named.second == path;
      if (other && !written && !take(other->name, path, import_pos(pkg, path)))
        return false;
    }
    return true;
  }

  static Pos import_pos(const Package &pkg, const std::string &path) {
    Pos at = pkg.unit->kids.empty() ? Pos{} : pkg.unit->kids.front()->pos;
    for (Node *decl : pkg.unit->kids)
      if (decl->kind == ND_IMPORT && decl->text == path) at = decl->pos;
    return at;
  }

  // Depth-first, appending each package after its dependencies.
  Package *load(const std::string &import_path, const std::string &dir,
                Pos at) {
    auto found = prog.by_path.find(import_path);
    if (found != prog.by_path.end()) return found->second;

    for (const std::string &open : visiting) {
      if (open != import_path) continue;
      error(at, "import cycle through '%s'", import_path.c_str());
      return nullptr;
    }
    visiting.push_back(import_path);

    prog.packages.emplace_back();
    Package &pkg = prog.packages.back();
    pkg.import_path = import_path;
    pkg.dir = dir;
    // The program's own package is qualified too. Without a prefix its
    // functions land in the object file under the names they were written
    // with, and a program with a function called `read` or `shutdown` would
    // have the runtime call that instead of the one in libc.
    pkg.prefix = "main.";
    if (!import_path.empty()) {
      pkg.name = package_name_of(import_path);
      pkg.prefix = import_path + ".";
      for (char &c : pkg.prefix)
        if (c == '/') c = '.';
    }

    std::vector<std::string> files;
    if (!sword_files(dir, mode != LOAD_BUILD && import_path.empty(), files) ||
        !parse_files(pkg, files))
      return nullptr;
    if ((mode == LOAD_TESTS || mode == LOAD_BENCH) &&
        import_path.empty() && !add_test_main(pkg))
      return nullptr;
    if (!import_path.empty() && !settle_name(pkg)) return nullptr;

    for (const std::string &path : pkg.imports) {
      if (!importable(pkg, path)) return nullptr;
      std::string found_dir = locate(path);
      if (found_dir.empty()) {
        error(import_pos(pkg, path), "cannot find package '%s'", path.c_str());
        required_note(path, import_pos(pkg, path));
        return nullptr;
      }
      if (!load(path, found_dir, Pos{})) return nullptr;
    }
    if (!name_imports(pkg)) return nullptr;

    visiting.pop_back();
    prog.by_path[import_path] = &pkg;
    prog.order.push_back(&pkg);
    return &pkg;
  }
};

} // namespace

void set_overlay(const std::string &path, std::string text) {
  overlays[path] = std::move(text);
}

void clear_overlay(const std::string &path) { overlays.erase(path); }

bool exported(const std::string &name) {
  return !name.empty() && isupper((unsigned char)name[0]);
}

std::string find_package(const std::string &import_path, const std::string &from,
                         const std::vector<std::string> &search) {
  Program prog;
  Loader loader(prog, search);
  if (!find_module(from, loader.module, loader.in_module)) return "";
  if (loader.in_module) {
    Settle how;
    how.cache_only = true;
    settle_deps(loader.module, how, loader.deps);
  }
  return loader.locate(import_path);
}

bool load_program(const std::string &input,
                  const std::vector<std::string> &search, Program &out,
                  LoadMode mode, const std::vector<std::string> &only,
                  bool locked) {
  Loader loader(out, search);
  loader.mode = mode;
  loader.label = input;
  loader.only = only;
  if (!find_module(is_directory(input) ? input : parent_of(input),
                   loader.module, loader.in_module))
    return false;
  if (loader.in_module) {
    Settle how;
    how.cache_only = mode == LOAD_EDITOR;
    how.locked = locked;
    if (!settle_deps(loader.module, how, loader.deps)) return false;
  }

  if (is_directory(input)) return loader.load("", input, Pos{}) != nullptr;

  // A single file is its own package: compiling one file must not silently
  // pull in its neighbours.
  if (reject_stale(input)) return false;
  out.packages.emplace_back();
  Package &pkg = out.packages.back();
  pkg.dir = parent_of(input);
  pkg.prefix = "main.";
  if (!loader.parse_files(pkg, {input})) return false;
  if ((mode == LOAD_TESTS || mode == LOAD_BENCH) &&
      !loader.add_test_main(pkg))
    return false;

  for (const std::string &path : pkg.imports) {
    if (!loader.importable(pkg, path)) return false;
    std::string dir = loader.locate(path);
    if (dir.empty()) {
      error(Loader::import_pos(pkg, path), "cannot find package '%s'",
            path.c_str());
      loader.required_note(path, Loader::import_pos(pkg, path));
      return false;
    }
    if (!loader.load(path, dir, Pos{})) return false;
  }
  if (!loader.name_imports(pkg)) return false;

  out.by_path[""] = &pkg;
  out.order.push_back(&pkg);
  return true;
}
