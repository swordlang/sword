#include "fetch.h"

#include "git.h"
#include "sha256.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <map>
#include <set>
#include <sys/stat.h>
#include <unistd.h>

namespace {

bool is_file(const std::string &path) {
  struct stat info;
  return stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode);
}

bool is_dir(const std::string &path) {
  struct stat info;
  return stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

std::string parent_of(const std::string &path) {
  size_t cut = path.find_last_of('/');
  return cut == std::string::npos || cut == 0 ? "/" : path.substr(0, cut);
}

bool make_dirs(const std::string &path) {
  if (path.empty() || is_dir(path)) return true;
  if (!make_dirs(parent_of(path))) return false;
  return mkdir(path.c_str(), 0755) == 0 || errno == EEXIST;
}

bool slurp(const std::string &path, std::string &out) {
  FILE *f = fopen(path.c_str(), "rb");
  if (!f) return false;
  char buffer[65536];
  size_t n;
  out.clear();
  while ((n = fread(buffer, 1, sizeof(buffer), f)) > 0) out.append(buffer, n);
  bool ok = !ferror(f);
  fclose(f);
  return ok;
}

// Written beside the file and renamed over it, so two builds of one project
// never see each other's half-written sword.sum.
bool spit(const std::string &path, const std::string &data) {
  std::string tmp = path + ".tmp" + std::to_string(getpid());
  FILE *f = fopen(tmp.c_str(), "wb");
  if (!f) return false;
  bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
  ok = fclose(f) == 0 && ok;
  if (ok && rename(tmp.c_str(), path.c_str()) == 0) return true;
  unlink(tmp.c_str());
  return false;
}

std::vector<std::string> entries(const std::string &dir) {
  std::vector<std::string> names;
  DIR *handle = opendir(dir.c_str());
  if (!handle) return names;
  while (dirent *entry = readdir(handle)) {
    std::string name = entry->d_name;
    if (name != "." && name != "..") names.push_back(name);
  }
  closedir(handle);
  std::sort(names.begin(), names.end());
  return names;
}

// The cache is read-only on purpose, so taking a tree away starts by giving
// its directories back their write bit.
void remove_tree(const std::string &path) {
  struct stat info;
  if (lstat(path.c_str(), &info) != 0) return;
  if (!S_ISDIR(info.st_mode)) {
    unlink(path.c_str());
    return;
  }
  chmod(path.c_str(), 0755);
  for (const std::string &name : entries(path)) remove_tree(path + "/" + name);
  rmdir(path.c_str());
}

// What a module is, as far as a build is concerned: the files the compiler
// reads. Hidden directories (.git among them) are not, and neither is a
// vendor/ the module keeps for its own builds.
bool read_by_compiler(const std::string &name) {
  return name == "sword.mod" ||
         (name.size() > 6 && name.compare(name.size() - 6, 6, ".sword") == 0);
}

// Kept with the code, outside the hash: a copy of somebody's code goes with
// their terms.
bool license(const std::string &name) {
  std::string upper;
  for (char c : name) upper += (char)toupper((unsigned char)c);
  return upper.compare(0, 7, "LICENSE") == 0 ||
         upper.compare(0, 7, "LICENCE") == 0 ||
         upper.compare(0, 7, "COPYING") == 0 ||
         upper.compare(0, 6, "NOTICE") == 0;
}

void files_of(const std::string &root, const std::string &rel,
              std::vector<std::string> &out) {
  std::string dir = rel.empty() ? root : root + "/" + rel;
  for (const std::string &name : entries(dir)) {
    if (name[0] == '.') continue;
    std::string sub = rel.empty() ? name : rel + "/" + name;
    if (sub == "vendor") continue;
    std::string full = root + "/" + sub;
    struct stat info;
    if (lstat(full.c_str(), &info) != 0) continue;
    if (S_ISDIR(info.st_mode)) files_of(root, sub, out);
    else if (S_ISREG(info.st_mode) &&
             (read_by_compiler(name) || (rel.empty() && license(name))))
      out.push_back(sub);
  }
}

// One line per file the compiler reads, its own hash and its path, in byte
// order, and the hash of those lines. The same code hashes the same from a
// clone, the cache or vendor/, whatever else sits around it.
std::string module_hash(const std::string &root) {
  std::vector<std::string> files;
  files_of(root, "", files);
  std::sort(files.begin(), files.end());
  std::string listing;
  for (const std::string &rel : files) {
    size_t slash = rel.find_last_of('/');
    if (!read_by_compiler(slash == std::string::npos ? rel : rel.substr(slash + 1)))
      continue;
    std::string text;
    if (!slurp(root + "/" + rel, text)) return "";
    listing += sha256_hex(text) + "  " + rel + "\n";
  }
  return "sha256:" + sha256_hex(listing);
}

bool copy_tree(const std::string &from, const std::string &to) {
  std::vector<std::string> files;
  files_of(from, "", files);
  for (const std::string &rel : files) {
    std::string text;
    std::string dest = to + "/" + rel;
    if (!slurp(from + "/" + rel, text) || !make_dirs(parent_of(dest)) ||
        !spit(dest, text))
      return false;
    chmod(dest.c_str(), 0644);
  }
  return true;
}

// Files to 0444 and directories to 0555 below `dir`, so nobody edits a
// dependency in the cache by accident and wonders why it changed everywhere.
void freeze(const std::string &dir) {
  for (const std::string &name : entries(dir)) {
    std::string full = dir + "/" + name;
    if (is_dir(full)) {
      freeze(full);
      chmod(full.c_str(), 0555);
    } else {
      chmod(full.c_str(), 0444);
    }
  }
}

// One module per repository, at its root, so the path is the repository; a
// /v2 at the end names a major version kept in the same one.
std::string repository(const std::string &path) {
  std::string repo = path;
  if (path_major(path)) repo = path.substr(0, path.find_last_of('/'));
  return "https://" + repo + ".git";
}

bool is_pseudo(const std::string &version, std::string &date,
               std::string &commit) {
  size_t dash = version.find('-');
  if (dash == std::string::npos) return false;
  std::string pre = version.substr(dash + 1);
  size_t second = pre.find('-');
  if (second != 8 || pre.size() < 16) return false;
  date = pre.substr(0, 8);
  commit = pre.substr(9);
  return date.find_first_not_of("0123456789") == std::string::npos &&
         commit.find_first_not_of("0123456789abcdef") == std::string::npos;
}

std::string scratch_dir() {
  std::string base = cache_root() + "/tmp";
  if (!make_dirs(base)) return "";
  std::string pattern = base + "/fetch.XXXXXX";
  if (!mkdtemp(&pattern[0])) return "";
  return pattern;
}

std::string trimmed(const std::string &text) {
  size_t end = text.find_last_not_of(" \n\r\t");
  return end == std::string::npos ? "" : text.substr(0, end + 1);
}

// A clone of `path` in `dest`: of one tag, or of the history with no files
// checked out, for finding a commit in.
bool git_clone(const std::string &path, const std::string &tag,
           const std::string &dest) {
  std::string url = repository(path);
  Ran ran = !tag.empty()
                ? git({"-c", "advice.detachedHead=false", "clone", "--quiet",
                       "--depth", "1", "--branch", tag, url, dest})
                : git({"clone", "--quiet", "--filter=blob:none", "--no-checkout",
                       url, dest});
  if (ran.status == 0) return true;
  if (tag.empty())
    fprintf(stderr, "shield: cannot fetch %s from %s\n", path.c_str(), url.c_str());
  else
    fprintf(stderr, "shield: cannot fetch %s at %s from %s\n", path.c_str(),
            tag.c_str(), url.c_str());
  explain(ran);
  return false;
}

// The commit `rev` names in a clone — a commit, or a branch, which a fresh
// clone only has under origin/ — as its full hash and its day in UTC.
bool commit_of(const std::string &clone, const std::string &rev,
               std::string &hash, std::string &day) {
  Ran ran;
  for (const std::string &name : {rev, "origin/" + rev}) {
    ran = git({"-C", clone, "log", "-1", "--format=%H %cd",
               "--date=format-local:%Y%m%d", name + "^{commit}", "--"});
    if (ran.status == 0) break;
  }
  if (ran.status != 0) {
    fprintf(stderr, "shield: '%s' is not a version, a branch or a commit\n",
            rev.c_str());
    return false;
  }
  std::string line = trimmed(ran.out);
  size_t space = line.find(' ');
  if (space == std::string::npos) return false;
  hash = line.substr(0, space);
  day = line.substr(space + 1);
  return true;
}

// The cache keeps each version of each module once, in a directory named
// for both, and the hash of what is in it beside it.
struct Cache {
  std::string root = cache_root();

  std::string entry(const std::string &path, const std::string &version) const {
    return root + "/mod/" + path + "@" + version;
  }

  bool get(const std::string &path, const std::string &version, Pos at,
           bool cache_only, std::string &dir, std::string &hash) {
    dir = entry(path, version);
    std::string hash_file = dir + ".hash";
    if (is_dir(dir)) {
      if (slurp(hash_file, hash)) {
        hash = trimmed(hash);
        return true;
      }
      hash = module_hash(dir);
      spit(hash_file, hash + "\n");
      return !hash.empty();
    }
    if (cache_only) return false;

    std::string staging = scratch_dir();
    if (staging.empty()) {
      fprintf(stderr, "shield: cannot write to the cache in '%s'\n", root.c_str());
      return false;
    }
    fprintf(stderr, "shield: fetching %s %s\n", path.c_str(), version.c_str());
    std::string date, commit;
    bool pseudo = is_pseudo(version, date, commit);
    std::string checkout = staging + "/git";
    bool ok = git_clone(path, pseudo ? "" : version, checkout);
    if (ok && pseudo) {
      // The date is part of the version so versions sort; one that does not
      // match its commit would sort somewhere it does not belong.
      std::string full, day;
      ok = commit_of(checkout, commit, full, day);
      if (ok && day != date) {
        error(at, "%s names commit %s, which is from %s, not %s", version.c_str(),
              commit.c_str(), day.c_str(), date.c_str());
        ok = false;
      }
      if (ok) {
        Ran ran = git({"-C", checkout, "reset", "--quiet", "--hard", full});
        if (ran.status != 0) {
          explain(ran);
          ok = false;
        }
      }
    }
    std::string kept = staging + "/mod";
    if (ok && !(make_dirs(kept) && copy_tree(checkout, kept))) {
      fprintf(stderr, "shield: cannot copy %s into the cache\n", path.c_str());
      ok = false;
    }
    if (ok) {
      hash = module_hash(kept);
      freeze(kept);
      ok = make_dirs(parent_of(dir));
      // Another build may have fetched the same version meanwhile; its copy
      // is as good as this one.
      if (ok && rename(kept.c_str(), dir.c_str()) != 0 && !is_dir(dir)) ok = false;
      if (ok) {
        chmod(dir.c_str(), 0555);
        spit(hash_file, hash + "\n");
      }
    }
    remove_tree(staging);
    return ok;
  }
};

// sword.sum: `path version sha256:...`, one line per module version a build
// has read, sorted.
struct Sums {
  std::string file;
  std::map<std::pair<std::string, std::string>, std::string> lines;
  std::set<std::pair<std::string, std::string>> used;
  bool changed = false;

  bool read() {
    std::string text;
    if (!slurp(file, text)) return true;
    int id = add_source(file, text);
    const Source &src = source_at(id);
    bool ok = true;
    for (int n = 1; n <= (int)src.line_start.size(); n++) {
      std::string line = trimmed(src.line(n));
      if (line.empty()) continue;
      char path[1024], version[256], hash[128];
      char extra = 0;
      if (sscanf(line.c_str(), "%1023s %255s %127s %c", path, version, hash,
                 &extra) != 3 ||
          strncmp(hash, "sha256:", 7) != 0) {
        error(Pos{id, n, 1}, "expected '<path> <version> sha256:<hash>'");
        ok = false;
        continue;
      }
      lines[{path, version}] = hash;
    }
    return ok;
  }

  bool write() const {
    std::string text;
    for (const auto &line : lines)
      text += line.first.first + " " + line.first.second + " " + line.second + "\n";
    if (spit(file, text)) return true;
    fprintf(stderr, "shield: cannot write '%s'\n", file.c_str());
    return false;
  }

  // `at` is the require line that brought the module in, when there is one;
  // a module only vendor/ names has none.
  bool check(const std::string &path, const std::string &version,
             const std::string &hash, const Pos *at, bool locked) {
    used.insert({path, version});
    auto had = lines.find({path, version});
    if (had == lines.end()) {
      if (!locked) {
        lines[{path, version}] = hash;
        changed = true;
        return true;
      }
      std::string what = path + " " + version +
                         " is not in sword.sum, and --locked keeps it from "
                         "being added";
      if (at) error(*at, "%s", what.c_str());
      else fprintf(stderr, "shield: %s\n", what.c_str());
      return false;
    }
    if (had->second == hash) return true;
    std::string what = path + " " + version + " is not the code sword.sum "
                       "recorded for it";
    std::string why = "recorded " + had->second + ", and it is now " + hash +
                      ": the tag was moved, or the copy was changed";
    if (at) {
      error(*at, "%s", what.c_str());
      note(*at, "%s", why.c_str());
    } else {
      fprintf(stderr, "shield: %s\n     %s\n", what.c_str(), why.c_str());
    }
    return false;
  }

  void tidy() {
    for (auto it = lines.begin(); it != lines.end();) {
      if (used.count(it->first)) {
        ++it;
        continue;
      }
      it = lines.erase(it);
      changed = true;
    }
  }
};

// vendor/modules.txt: the versions `shield vendor` copied, one per line.
bool read_vendored(const std::string &file,
                   std::map<std::string, std::string> &out) {
  std::string text;
  if (!slurp(file, text)) return false;
  size_t at = 0;
  while (at < text.size()) {
    size_t end = text.find('\n', at);
    if (end == std::string::npos) end = text.size();
    std::string line = trimmed(text.substr(at, end - at));
    at = end + 1;
    if (line.empty() || line[0] == '#') continue;
    size_t space = line.find(' ');
    if (space == std::string::npos) continue;
    out[line.substr(0, space)] = line.substr(space + 1);
  }
  return true;
}

const Module::Replace *replacement(const Module &main, const std::string &path) {
  for (const Module::Replace &r : main.replaces)
    if (path_under(path, r.path)) return &r;
  return nullptr;
}

bool settle_from_vendor(const Module &main, bool locked, Sums &sums,
                        Deps &out) {
  std::string vendor = main.root + "/vendor";
  std::map<std::string, std::string> vendored;
  read_vendored(vendor + "/modules.txt", vendored);
  bool ok = true;
  // Without the network there is no graph to walk, so vendor/ is taken as
  // the answer and checked against what can be checked here: every module
  // sword.mod asks for is there, at least as new, and is the code sword.sum
  // recorded.
  for (const Module::Require &r : main.required) {
    if (replacement(main, r.path)) continue;
    auto had = vendored.find(r.path);
    if (had == vendored.end() || compare_versions(had->second, r.version) < 0) {
      error(r.at, "vendor/ %s; run 'shield vendor' after changing sword.mod",
            had == vendored.end()
                ? ("does not have " + r.path).c_str()
                : ("has " + r.path + " " + had->second).c_str());
      ok = false;
    }
  }
  for (const auto &v : vendored) {
    Dep dep;
    dep.path = v.first;
    dep.version = v.second;
    dep.dir = vendor + "/" + v.first;
    Module mod;
    if (!read_module(dep.dir, mod)) {
      ok = false;
      continue;
    }
    for (const Module::Require &r : mod.required) dep.required.push_back(r.path);
    // The vendored copy is in the project, where anybody can edit it, so it
    // is checked on every build rather than once when it was fetched.
    const Pos *at = nullptr;
    for (const Module::Require &r : main.required)
      if (r.path == dep.path) at = &r.at;
    if (!sums.check(dep.path, dep.version, module_hash(dep.dir), at, locked))
      ok = false;
    out.modules.push_back(dep);
  }
  return ok;
}

} // namespace

std::string cache_root() {
  const char *set = getenv("SWORD_CACHE");
  if (set && *set) return set;
  const char *home = getenv("HOME");
  return std::string(home && *home ? home : ".") + "/.sword";
}

const Dep *Deps::owner(const std::string &import_path) const {
  const Dep *best = nullptr;
  for (const Dep &dep : modules)
    if (path_under(import_path, dep.path) &&
        (!best || dep.path.size() > best->path.size()))
      best = &dep;
  return best;
}

bool settle_deps(const Module &main, const Settle &how, Deps &out) {
  Sums sums;
  sums.file = main.root + "/sword.sum";
  if (!sums.read()) return false;
  if (!how.ignore_vendor && is_file(main.root + "/vendor/modules.txt")) {
    bool ok = settle_from_vendor(main, how.locked, sums, out);
    return ok && (!sums.changed || how.cache_only || sums.write());
  }

  // Minimal version selection: walk every version anybody asks for and keep
  // the newest of those for each path. A newer release that nobody asked for
  // never enters a build.
  struct Want {
    std::string path, version;
    Pos at;
  };
  std::vector<Want> work;
  for (const Module::Require &r : main.required)
    work.push_back({r.path, r.version, r.at});
  std::map<std::string, std::string> selected;
  std::map<std::string, Dep> read; // by path@version
  std::set<std::string> seen;
  Cache cache;
  bool ok = true;

  while (ok && !work.empty()) {
    Want want = work.back();
    work.pop_back();
    const Module::Replace *r = replacement(main, want.path);
    std::string key = r ? want.path : want.path + "@" + want.version;
    auto chosen = selected.find(want.path);
    if (chosen == selected.end() ||
        compare_versions(want.version, chosen->second) > 0)
      selected[want.path] = want.version;
    if (!seen.insert(key).second) continue;

    Dep dep;
    dep.path = want.path;
    dep.version = want.version;
    Module mod;
    if (r) {
      dep.replaced = true;
      dep.dir = r->dir + want.path.substr(r->path.size());
      // A directory being worked on may not have a sword.mod yet.
      if (is_file(dep.dir + "/sword.mod") && !read_module(dep.dir, mod)) {
        ok = false;
        continue;
      }
    } else {
      std::string hash;
      if (!cache.get(want.path, want.version, want.at, how.cache_only, dep.dir,
                     hash)) {
        if (!how.cache_only) ok = false;
        continue;
      }
      if (!sums.check(want.path, want.version, hash, &want.at, how.locked)) {
        ok = false;
        continue;
      }
      if (!read_module(dep.dir, mod)) {
        ok = false;
        continue;
      }
      if (mod.path != want.path) {
        error(want.at, "%s %s calls itself '%s' in its sword.mod", want.path.c_str(),
              want.version.c_str(), mod.path.c_str());
        ok = false;
        continue;
      }
    }
    for (const Module::Require &next : mod.required) {
      dep.required.push_back(next.path);
      work.push_back({next.path, next.version, next.at});
    }
    read[key] = dep;
  }

  for (const auto &pick : selected) {
    std::string key = replacement(main, pick.first)
                          ? pick.first
                          : pick.first + "@" + pick.second;
    auto found = read.find(key);
    if (found != read.end()) out.modules.push_back(found->second);
  }
  if (how.tidy) sums.tidy();
  if (sums.changed && !how.cache_only && !sums.write()) ok = false;
  return ok;
}

namespace {

bool here(Module &main) {
  char cwd[4096];
  bool found = false;
  if (!getcwd(cwd, sizeof(cwd)) || !find_module(cwd, main, found)) return false;
  if (!found)
    fprintf(stderr, "shield: there is no sword.mod here or above; a module "
                    "starts with one that says 'module <path>'\n");
  return found;
}

// The tags of a module's repository that are versions it can be required at.
bool versions_of(const std::string &path, std::vector<std::string> &out) {
  Ran ran = git({"ls-remote", "--tags", "--refs", repository(path)});
  if (ran.status != 0) {
    fprintf(stderr, "shield: cannot list the versions of %s at %s\n",
            path.c_str(), repository(path).c_str());
    explain(ran);
    return false;
  }
  size_t at = 0;
  while (at < ran.out.size()) {
    size_t end = ran.out.find('\n', at);
    if (end == std::string::npos) end = ran.out.size();
    std::string line = ran.out.substr(at, end - at);
    at = end + 1;
    size_t ref = line.find("refs/tags/");
    if (ref == std::string::npos) continue;
    std::string tag = line.substr(ref + 10);
    std::string date, commit;
    if (valid_version(tag) && major_fits(path, tag) &&
        !is_pseudo(tag, date, commit))
      out.push_back(tag);
  }
  return true;
}

// The newest release, or the newest pre-release when there is nothing else.
std::string newest(const std::vector<std::string> &tags) {
  std::string best, best_pre;
  for (const std::string &tag : tags) {
    std::string &slot = tag.find('-') == std::string::npos ? best : best_pre;
    if (slot.empty() || compare_versions(tag, slot) > 0) slot = tag;
  }
  return best.empty() ? best_pre : best;
}

// A commit with no tag still gets a version, one that sorts before every
// release of its major: vN.0.0-<day>-<commit>.
bool pseudo_version(const std::string &path, const std::string &rev,
                    std::string &version) {
  std::string staging = scratch_dir();
  if (staging.empty()) return false;
  std::string checkout = staging + "/git";
  std::string hash, day;
  bool ok = git_clone(path, "", checkout) && commit_of(checkout, rev, hash, day);
  remove_tree(staging);
  if (!ok) return false;
  int major = path_major(path);
  version = "v" + std::to_string(major) + ".0.0-" + day + "-" + hash.substr(0, 12);
  return true;
}

// sword.mod with the require line for `path` set to `version`, or taken out
// when `version` is empty. The rest of the file, comments included, stays as
// it was written.
std::string with_require(const std::string &text, const std::string &path,
                         const std::string &version) {
  std::vector<std::string> lines;
  size_t at = 0;
  while (at < text.size()) {
    size_t end = text.find('\n', at);
    if (end == std::string::npos) end = text.size();
    lines.push_back(text.substr(at, end - at));
    at = end + 1;
  }
  int last_require = -1;
  bool done = false;
  for (size_t i = 0; i < lines.size(); i++) {
    char word[16], said[1024];
    if (sscanf(lines[i].c_str(), "%15s %1023s", word, said) != 2 ||
        strcmp(word, "require") != 0)
      continue;
    if (path != said) {
      last_require = (int)i;
      continue;
    }
    if (version.empty()) {
      lines.erase(lines.begin() + i);
      i--;
    } else {
      lines[i] = "require " + path + " " + version;
      last_require = (int)i;
    }
    done = true;
  }
  if (!done && !version.empty()) {
    std::string line = "require " + path + " " + version;
    if (last_require >= 0) {
      lines.insert(lines.begin() + last_require + 1, line);
    } else {
      if (!lines.empty() && !trimmed(lines.back()).empty()) lines.push_back("");
      lines.push_back(line);
    }
  }
  std::string out;
  for (const std::string &line : lines) out += line + "\n";
  return out;
}

} // namespace

int get_command(const std::vector<std::string> &args) {
  if (args.size() != 1 || args[0].empty() || args[0][0] == '-') {
    fprintf(stderr, "usage: shield get <module path>[@version | @commit | @none]\n");
    return 1;
  }
  Module main;
  if (!here(main)) return 1;
  std::string path = args[0], asked;
  size_t at = path.find('@');
  if (at != std::string::npos) {
    asked = path.substr(at + 1);
    path = path.substr(0, at);
  }
  if (!valid_module_path(path) || path_under(path, "std")) {
    fprintf(stderr, "shield: '%s' is not a module path\n", path.c_str());
    return 1;
  }
  if (path_under(path, main.path)) {
    fprintf(stderr, "shield: '%s' is part of this module, %s\n", path.c_str(),
            main.path.c_str());
    return 1;
  }

  std::string version;
  if (asked == "none") {
    // Taken out below; nothing to look up.
  } else if (asked.empty()) {
    std::vector<std::string> tags;
    if (!versions_of(path, tags)) return 1;
    version = newest(tags);
    if (version.empty() && !pseudo_version(path, "HEAD", version)) return 1;
    // Without a version, get only ever moves forward: a pre-release or a
    // commit asked for on purpose is newer than the last release.
    for (const Module::Require &r : main.required)
      if (r.path == path && compare_versions(r.version, version) > 0)
        version = r.version;
  } else if (valid_version(asked)) {
    if (!major_fits(path, asked)) {
      fprintf(stderr, "shield: %s is major version %d, which is required as "
                      "'%s/v%d'\n",
              asked.c_str(), version_major(asked), path.c_str(),
              version_major(asked));
      return 1;
    }
    version = asked;
  } else if (!pseudo_version(path, asked, version)) {
    return 1;
  }

  std::string file = main.root + "/sword.mod", before;
  if (!slurp(file, before)) {
    fprintf(stderr, "shield: cannot read '%s'\n", file.c_str());
    return 1;
  }
  bool had = false;
  for (const Module::Require &r : main.required) had = had || r.path == path;
  if (version.empty() && !had) {
    fprintf(stderr, "shield: sword.mod does not require %s\n", path.c_str());
    return 1;
  }
  if (!spit(file, with_require(before, path, version))) {
    fprintf(stderr, "shield: cannot write '%s'\n", file.c_str());
    return 1;
  }

  // What was asked for is only a minimum: something else may require a newer
  // version, and the build will use that one.
  Module after;
  bool found = false;
  Deps deps;
  Settle how;
  how.ignore_vendor = true;
  how.tidy = true;
  if (!find_module(main.root, after, found) || !settle_deps(after, how, deps)) {
    spit(file, before);
    return 1;
  }
  if (version.empty()) {
    fprintf(stderr, "shield: removed %s\n", path.c_str());
    return 0;
  }
  const Dep *dep = deps.owner(path);
  fprintf(stderr, "shield: %s %s\n", path.c_str(), version.c_str());
  if (dep && dep->path == path && dep->version != version)
    fprintf(stderr, "        a build uses %s, which another module requires\n",
            dep->version.c_str());
  if (is_file(main.root + "/vendor/modules.txt"))
    fprintf(stderr, "        vendor/ is now out of date; 'shield vendor' "
                    "rewrites it\n");
  return 0;
}

int vendor_command(const std::vector<std::string> &args) {
  if (!args.empty()) {
    fprintf(stderr, "usage: shield vendor\n");
    return 1;
  }
  Module main;
  if (!here(main)) return 1;
  std::string vendor = main.root + "/vendor";
  if (is_dir(vendor) && !is_file(vendor + "/modules.txt")) {
    fprintf(stderr, "shield: '%s' was not written by shield vendor, so it is "
                    "left alone\n",
            vendor.c_str());
    return 1;
  }
  Deps deps;
  Settle how;
  how.ignore_vendor = true;
  if (!settle_deps(main, how, deps)) return 1;

  remove_tree(vendor);
  std::string listing = "# Written by shield vendor: what a build reads in "
                        "place of the cache.\n";
  int copied = 0;
  for (const Dep &dep : deps.modules) {
    if (dep.replaced) continue;
    if (!copy_tree(dep.dir, vendor + "/" + dep.path)) {
      fprintf(stderr, "shield: cannot copy %s into vendor/\n", dep.path.c_str());
      return 1;
    }
    listing += dep.path + " " + dep.version + "\n";
    copied++;
  }
  if (!copied) {
    fprintf(stderr, "shield: nothing to vendor; this module requires nothing\n");
    return 0;
  }
  if (!spit(vendor + "/modules.txt", listing)) {
    fprintf(stderr, "shield: cannot write '%s/modules.txt'\n", vendor.c_str());
    return 1;
  }
  fprintf(stderr, "shield: vendored %d module%s\n", copied, copied == 1 ? "" : "s");
  return 0;
}
