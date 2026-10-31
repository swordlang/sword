#include "link.h"

#include <algorithm>
#include <cstdio>
#include <sstream>
#include <sys/stat.h>

namespace {

std::vector<std::string> words(const std::string &text) {
  std::vector<std::string> out;
  std::istringstream in(text);
  std::string word;
  while (in >> word) out.push_back(word);
  return out;
}

// What a command printed, or nothing when it failed. Only ever handed names the
// checker has already restricted to letters, digits and `_-.+`.
bool run(const std::string &command, std::string &output) {
  FILE *pipe = popen((command + " 2>/dev/null").c_str(), "r");
  if (!pipe) return false;
  char buf[4096];
  size_t n;
  output.clear();
  while ((n = fread(buf, 1, sizeof(buf), pipe)) > 0) output.append(buf, n);
  return pclose(pipe) == 0;
}

bool exists(const std::string &path) {
  struct stat st;
  return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

// The operating system's own libraries are never taken from an archive, even
// under --static: a static libm beside a dynamic libc is two copies of one
// library that each think they own the process.
bool from_the_system(const std::string &name) {
  static const char *names[] = {"c",     "m",     "pthread", "dl",  "rt",
                                "util",  "c++",   "c++abi",  "stdc++",
                                "gcc",   "gcc_s", "System",  "resolv"};
  for (const char *n : names)
    if (name == n) return true;
  return false;
}

// Directories the linker searches without being told, which a dynamic library
// can be found in at run time without an rpath.
bool searched_anyway(const std::string &dir) {
  for (const char *root : {"/lib", "/lib64", "/usr/lib", "/usr/lib64"}) {
    std::string r = root;
    if (dir == r || dir.compare(0, r.size() + 1, r + "/") == 0) return true;
  }
  return dir.compare(0, 8, "/System/") == 0;
}

// Where the C compiler looks for libraries, which is the list an archive has to
// be found in when it was not named by a -L. clang on Linux prints the GCC and
// multiarch directories; Apple's prints only its own, so the usual prefixes
// follow.
std::vector<std::string> default_dirs() {
  std::vector<std::string> dirs;
  std::string printed;
  if (run("clang -print-search-dirs", printed)) {
    std::istringstream lines(printed);
    std::string line;
    while (std::getline(lines, line)) {
      if (line.compare(0, 12, "libraries: =") != 0) continue;
      std::string list = line.substr(12);
      size_t start = 0;
      while (start <= list.size()) {
        size_t colon = list.find(':', start);
        if (colon == std::string::npos) colon = list.size();
        if (colon > start) dirs.push_back(list.substr(start, colon - start));
        start = colon + 1;
      }
    }
  }
  for (const char *d : {"/opt/homebrew/lib", "/usr/local/lib", "/usr/lib",
                        "/usr/lib64", "/lib", "/lib64"})
    dirs.push_back(d);
  return dirs;
}

struct Plan {
  bool prefer_static;
  std::vector<std::string> dirs; // -L directories seen so far, in order
  std::vector<std::string> rpaths;
  std::vector<std::string> defaults;
  bool defaults_read = false;

  void add_dir(const std::string &dir) {
    if (std::find(dirs.begin(), dirs.end(), dir) == dirs.end())
      dirs.push_back(dir);
  }

  // The first directory holding `lib<name><suffix>`, or "". Only the ones a
  // -L named, unless `anywhere`: the rest cost a run of clang to learn.
  std::string find(const std::string &name, const char *suffix,
                   bool anywhere) {
    std::string file = "lib" + name + suffix;
    for (const std::string &d : dirs)
      if (exists(d + "/" + file)) return d;
    if (!anywhere) return "";
    if (!defaults_read) {
      defaults = default_dirs();
      defaults_read = true;
    }
    for (const std::string &d : defaults)
      if (exists(d + "/" + file)) return d;
    return "";
  }

  // `-lname`, as an archive's path when --static asks and there is one. A
  // library the program declared has to have one; what it pulls in may not.
  bool library(const std::string &name, bool declared,
               std::vector<std::string> &out) {
    if (from_the_system(name)) {
      out.push_back("-l" + name);
      return true;
    }
    if (prefer_static) {
      std::string dir = find(name, ".a", true);
      if (!dir.empty()) {
        out.push_back(dir + "/lib" + name + ".a");
        return true;
      }
      if (declared) {
        fprintf(stderr,
                "shield: --static, but there is no lib%s.a; add the directory"
                " it is in with --link -L<dir>\n",
                name.c_str());
        return false;
      }
    }
    out.push_back("-l" + name);
    // A library found through a -L has to be findable when the program
    // starts too, not only now.
    for (const char *suffix : {".so", ".dylib"}) {
      std::string dir = find(name, suffix, false);
      if (dir.empty()) continue;
      if (!searched_anyway(dir) &&
          std::find(rpaths.begin(), rpaths.end(), dir) == rpaths.end())
        rpaths.push_back(dir);
      break;
    }
    return true;
  }

  // A line of linker arguments, with each -l taken through library().
  bool line(const std::vector<std::string> &args, const std::string &declared,
            std::vector<std::string> &out) {
    for (size_t i = 0; i < args.size(); i++) {
      const std::string &a = args[i];
      if (a == "-L" && i + 1 < args.size()) {
        add_dir(args[i + 1]);
        out.push_back(a);
        out.push_back(args[++i]);
      } else if (a.compare(0, 2, "-L") == 0) {
        add_dir(a.substr(2));
        out.push_back(a);
      } else if (a.compare(0, 2, "-l") == 0 && a.size() > 2) {
        std::string name = a.substr(2);
        if (!library(name, name == declared, out)) return false;
      } else {
        out.push_back(a);
      }
    }
    return true;
  }
};

// What pkg-config says a library needs, under the name the program used or
// with `lib` in front, which is how most .pc files are named when the two
// differ (libcurl for -lcurl). Empty when it does not know the library.
std::vector<std::string> pkg_config(const std::string &name, bool statically) {
  for (const std::string &package : {name, "lib" + name}) {
    std::string printed;
    std::string command = "pkg-config --libs ";
    if (statically) command += "--static ";
    if (run(command + package, printed)) return words(printed);
  }
  return {};
}

} // namespace

bool link_arguments(const LinkRequest &req, std::vector<std::string> &out) {
  Plan plan{req.prefer_static, {}, {}, {}, false};
  // The user's -L directories come first in the search, wherever they are on
  // the line: an archive asked for by name should be found where they said.
  for (size_t i = 0; i < req.user.size(); i++) {
    const std::string &a = req.user[i];
    if (a == "-L" && i + 1 < req.user.size()) plan.add_dir(req.user[i + 1]);
    else if (a.compare(0, 2, "-L") == 0) plan.add_dir(a.substr(2));
  }

  // A program that never reaches std/tls must start where libssl is missing.
  // The Makefile writes OpenSSL into the runtime's flags as these two words.
  std::vector<std::string> runtime;
  for (const std::string &word : words(req.runtime_flags))
    if (req.tls || (word != "-lssl" && word != "-lcrypto"))
      runtime.push_back(word);
  if (!plan.line(runtime, "", out)) return false;
  // The runtime's own libraries were found where the build found them, and its
  // programs have always started without being told where to look again.
  plan.rpaths.clear();
  out.insert(out.end(), req.user.begin(), req.user.end());

  for (const std::string &name : req.libraries) {
    std::vector<std::string> needed = pkg_config(name, req.prefer_static);
    if (needed.empty()) needed.push_back("-l" + name);
    // pkg-config may name the library differently from the program — zlib's
    // file says -lz — so whatever it lists first stands for the declared one.
    std::string declared = name;
    for (const std::string &a : needed)
      if (a.compare(0, 2, "-l") == 0) {
        declared = a.substr(2);
        break;
      }
    if (!plan.line(needed, declared, out)) return false;
  }

  for (const std::string &dir : plan.rpaths) out.push_back("-Wl,-rpath," + dir);

#ifdef __APPLE__
  // ld64 refuses an undefined symbol even when it is weak; -U lets it through,
  // and dyld then binds it to null when nothing provides it.
  for (const std::string &sym : req.weak_symbols)
    out.push_back("-Wl,-U,_" + sym);
#endif
  return true;
}
