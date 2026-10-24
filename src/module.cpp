#include "module.h"

#include "version.h"

#include <cctype>
#include <cstdlib>
#include <sys/stat.h>

namespace {

bool is_file(const std::string &path) {
  struct stat info;
  return stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode);
}

bool is_dir(const std::string &path) {
  struct stat info;
  return stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

std::string absolute(const std::string &path) {
  char real[4096];
  return realpath(path.c_str(), real) ? std::string(real) : path;
}

// Lower case, digits and `. - _`, in segments joined by `/`. Narrower than a
// URL on purpose: an import path also has to become a symbol prefix, a
// directory name in the cache and a git URL, the same on every system.
bool valid_path(const std::string &path) {
  if (path.empty() || path.front() == '/' || path.back() == '/') return false;
  size_t start = 0;
  while (start <= path.size()) {
    size_t end = path.find('/', start);
    if (end == std::string::npos) end = path.size();
    std::string seg = path.substr(start, end - start);
    if (seg.empty() || seg == "." || seg == "..") return false;
    for (char c : seg)
      if (!(islower((unsigned char)c) || isdigit((unsigned char)c) ||
            c == '.' || c == '-' || c == '_'))
        return false;
    start = end + 1;
  }
  return true;
}

struct Line {
  std::vector<std::string> words;
  std::vector<int> cols;
};

// Words split on blanks, with `//` to the end of the line a comment.
Line split(const std::string &text) {
  Line line;
  size_t i = 0;
  while (i < text.size()) {
    if (text[i] == ' ' || text[i] == '\t' || text[i] == '\r') {
      i++;
      continue;
    }
    if (text.compare(i, 2, "//") == 0) break;
    size_t start = i;
    while (i < text.size() && text[i] != ' ' && text[i] != '\t' &&
           text[i] != '\r')
      i++;
    line.words.push_back(text.substr(start, i - start));
    line.cols.push_back((int)start + 1);
  }
  return line;
}

bool parse(const std::string &file, const std::string &root, Module &out) {
  int id = load_source(file);
  if (id < 0) {
    fprintf(stderr, "shield: cannot read '%s'\n", file.c_str());
    return false;
  }
  const Source &src = source_at(id);
  out.root = root;
  bool ok = true;
  bool named = false;
  int lines = (int)src.line_start.size();
  for (int n = 1; n <= lines; n++) {
    Line line = split(src.line(n));
    if (line.words.empty()) continue;
    const std::string &what = line.words[0];
    auto at = [&](size_t word) {
      return Pos{id, n, line.cols[word < line.cols.size() ? word : 0]};
    };

    if (what == "module") {
      if (named) {
        error(at(0), "a sword.mod names one module");
        ok = false;
      } else if (line.words.size() != 2) {
        error(at(0), "expected 'module <path>'");
        ok = false;
      } else if (!valid_path(line.words[1])) {
        error(at(1), "'%s' is not a module path: lower-case letters, digits, "
                     "'.', '-' and '_', in segments joined by '/'",
              line.words[1].c_str());
        ok = false;
      } else if (path_under(line.words[1], "std")) {
        error(at(1), "'std' is the standard library's; a module cannot be "
                     "named under it");
        ok = false;
      } else {
        out.path = line.words[1];
        named = true;
      }
    } else if (what == "sword") {
      int major = 0, minor = 0;
      char extra = 0;
      if (line.words.size() != 2 ||
          sscanf(line.words[1].c_str(), "%d.%d%c", &major, &minor, &extra) != 2) {
        error(at(0), "expected 'sword <major>.<minor>', as in 'sword 0.3'");
        ok = false;
        continue;
      }
      int have_major = 0, have_minor = 0;
      sscanf(SWORD_VERSION, "%d.%d", &have_major, &have_minor);
      if (major > have_major || (major == have_major && minor > have_minor)) {
        error(at(1), "this module needs Sword %d.%d, and this is %s",
              major, minor, SWORD_VERSION);
        ok = false;
      }
      out.sword_major = major;
      out.sword_minor = minor;
    } else if (what == "require") {
      if (line.words.size() != 3) {
        error(at(0), "expected 'require <path> <version>'");
        ok = false;
        continue;
      }
      const std::string &path = line.words[1];
      const std::string &version = line.words[2];
      if (!valid_path(path)) {
        error(at(1), "'%s' is not a module path", path.c_str());
        ok = false;
      } else if (!valid_version(version)) {
        error(at(2), "'%s' is not a version: a tag like v1.4.0, or a commit's "
                     "pseudo-version like v0.0.0-20261023-a1b2c3d",
              version.c_str());
        ok = false;
      } else if (version_major(version) >= 2 &&
                 path_major(path) != version_major(version)) {
        // Two majors are two modules, so a v2 is imported under its own path,
        // and the path says which one is meant.
        error(at(2), "%s is major version %d, which is required as '%s/v%d'",
              version.c_str(), version_major(version), path.c_str(),
              version_major(version));
        ok = false;
      } else if (path_major(path) && path_major(path) != version_major(version)) {
        error(at(2), "'%s' is major version %d, and %s is not",
              path.c_str(), path_major(path), version.c_str());
        ok = false;
      } else {
        bool twice = false;
        for (const Module::Require &had : out.required)
          if (had.path == path) twice = true;
        if (twice) {
          error(at(1), "'%s' is required twice", path.c_str());
          ok = false;
        } else {
          out.required.push_back({path, version, at(1)});
        }
      }
    } else if (what == "replace") {
      if (line.words.size() != 4 || line.words[2] != "=>") {
        error(at(0), "expected 'replace <path> => <directory>'");
        ok = false;
        continue;
      }
      std::string dir = line.words[3];
      if (dir.front() != '/') dir = root + "/" + dir;
      if (!is_dir(dir)) {
        error(at(3), "'%s' is not a directory", line.words[3].c_str());
        ok = false;
        continue;
      }
      out.replaces.push_back({line.words[1], absolute(dir), at(1)});
    } else {
      error(at(0), "'%s' is not something a sword.mod says; it has module, "
                   "sword, require and replace",
            what.c_str());
      ok = false;
    }
  }
  if (ok && !named) {
    error(Pos{id, 1, 1}, "a sword.mod starts by naming its module: "
                         "'module <path>'");
    ok = false;
  }
  return ok;
}

// Pre-release identifiers, compared the way semantic versioning does: digits
// as numbers and below anything with a letter, the rest as text.
int compare_pre(const std::string &a, const std::string &b) {
  size_t i = 0, j = 0;
  while (i < a.size() && j < b.size()) {
    size_t ei = a.find('.', i), ej = b.find('.', j);
    if (ei == std::string::npos) ei = a.size();
    if (ej == std::string::npos) ej = b.size();
    std::string x = a.substr(i, ei - i), y = b.substr(j, ej - j);
    bool xn = x.find_first_not_of("0123456789") == std::string::npos;
    bool yn = y.find_first_not_of("0123456789") == std::string::npos;
    if (xn && yn) {
      if (x.size() != y.size()) return x.size() < y.size() ? -1 : 1;
      if (x != y) return x < y ? -1 : 1;
    } else if (xn != yn) {
      return xn ? -1 : 1;
    } else if (x != y) {
      return x < y ? -1 : 1;
    }
    i = ei + 1;
    j = ej + 1;
  }
  bool a_left = i < a.size(), b_left = j < b.size();
  return a_left == b_left ? 0 : a_left ? 1 : -1;
}

} // namespace

bool path_under(const std::string &import_path, const std::string &prefix) {
  return import_path == prefix ||
         (import_path.size() > prefix.size() &&
          import_path.compare(0, prefix.size(), prefix) == 0 &&
          import_path[prefix.size()] == '/');
}

int path_major(const std::string &path) {
  size_t cut = path.find_last_of('/');
  if (cut == std::string::npos || cut + 2 > path.size()) return 0;
  if (path[cut + 1] != 'v') return 0;
  int major = 0;
  for (size_t i = cut + 2; i < path.size(); i++) {
    if (!isdigit((unsigned char)path[i])) return 0;
    major = major * 10 + (path[i] - '0');
  }
  return major >= 2 ? major : 0;
}

int version_major(const std::string &version) {
  return atoi(version.c_str() + 1);
}

bool major_fits(const std::string &path, const std::string &version) {
  int major = version_major(version);
  return path_major(path) ? path_major(path) == major : major < 2;
}

bool valid_module_path(const std::string &path) { return valid_path(path); }

int compare_versions(const std::string &a, const std::string &b) {
  long x[3] = {}, y[3] = {};
  sscanf(a.c_str(), "v%ld.%ld.%ld", &x[0], &x[1], &x[2]);
  sscanf(b.c_str(), "v%ld.%ld.%ld", &y[0], &y[1], &y[2]);
  for (int i = 0; i < 3; i++)
    if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
  size_t pa = a.find('-'), pb = b.find('-');
  if (pa == std::string::npos || pb == std::string::npos)
    return pa == pb ? 0 : pa == std::string::npos ? 1 : -1;
  return compare_pre(a.substr(pa + 1), b.substr(pb + 1));
}

bool read_module(const std::string &root, Module &out) {
  std::string file = root + "/sword.mod";
  if (!is_file(file)) {
    fprintf(stderr, "shield: '%s' has no sword.mod\n", root.c_str());
    return false;
  }
  return parse(file, root, out);
}

bool valid_version(const std::string &version) {
  if (version.size() < 6 || version[0] != 'v') return false;
  size_t i = 1;
  for (int part = 0; part < 3; part++) {
    size_t start = i;
    while (i < version.size() && isdigit((unsigned char)version[i])) i++;
    if (i == start) return false;
    if (part < 2) {
      if (i >= version.size() || version[i] != '.') return false;
      i++;
    }
  }
  if (i == version.size()) return true;
  if (version[i] != '-' || i + 1 == version.size()) return false;
  for (i++; i < version.size(); i++)
    if (!isalnum((unsigned char)version[i]) && version[i] != '.' &&
        version[i] != '-')
      return false;
  return true;
}

std::string package_name_of(const std::string &import_path) {
  size_t cut = import_path.find_last_of('/');
  std::string last =
      cut == std::string::npos ? import_path : import_path.substr(cut + 1);
  if (path_major(import_path) && cut != std::string::npos) {
    size_t before = import_path.find_last_of('/', cut - 1);
    return before == std::string::npos
               ? import_path.substr(0, cut)
               : import_path.substr(before + 1, cut - before - 1);
  }
  return last;
}

std::string Module::resolve(const std::string &import_path) const {
  if (!path.empty() && path_under(import_path, path))
    return root + import_path.substr(path.size());
  // Only the module being built replaces anything: a dependency redirecting
  // somebody else's code would be a dependency deciding what everybody runs.
  for (const Replace &r : replaces)
    if (path_under(import_path, r.path)) return r.dir + import_path.substr(r.path.size());
  return "";
}

bool find_module(const std::string &dir, Module &out, bool &found) {
  found = false;
  std::string at = absolute(dir);
  while (true) {
    std::string file = at + "/sword.mod";
    if (is_file(file)) {
      found = true;
      return parse(file, at, out);
    }
    size_t cut = at.find_last_of('/');
    if (cut == std::string::npos || at == "/") return true;
    at = cut == 0 ? "/" : at.substr(0, cut);
  }
}
