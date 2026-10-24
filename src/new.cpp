#include "new.h"

#include "git.h"
#include "module.h"
#include "version.h"

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>

namespace {

bool identifier(const std::string &name) {
  if (name.empty() || !(isalpha((unsigned char)name[0]) || name[0] == '_'))
    return false;
  for (char c : name)
    if (!(isalnum((unsigned char)c) || c == '_')) return false;
  return true;
}

bool write_file(const std::string &path, const std::string &text) {
  FILE *f = fopen(path.c_str(), "wx");
  if (!f) {
    fprintf(stderr, "shield: cannot write '%s': %s\n", path.c_str(),
            strerror(errno));
    return false;
  }
  bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
  return fclose(f) == 0 && ok;
}

// A directory that is not there yet, or is there with nothing in it.
bool fresh(const std::string &dir) {
  DIR *handle = opendir(dir.c_str());
  if (!handle) return errno == ENOENT;
  bool empty = true;
  while (dirent *entry = readdir(handle))
    if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..")) empty = false;
  closedir(handle);
  return empty;
}

std::string program_main() {
  return "import \"std/io\"\n"
         "\n"
         "func greeting() string {\n"
         "    return \"hello, world\"\n"
         "}\n"
         "\n"
         "func main() !void {\n"
         "    try io.Printf(\"{}\\n\", greeting())\n"
         "}\n";
}

std::string program_test() {
  return "import \"std/testing\"\n"
         "\n"
         "func TestGreeting(mut t *testing.T) !void {\n"
         "    t.Equal(greeting(), \"hello, world\")\n"
         "}\n";
}

std::string library(const std::string &name) {
  return "package " + name + "\n"
         "\n"
         "// Twice answers x + x.\n"
         "func Twice(x int) int {\n"
         "    return x + x\n"
         "}\n";
}

std::string library_test(const std::string &name) {
  return "package " + name + "\n"
         "\n"
         "import \"std/testing\"\n"
         "\n"
         "func TestTwice(mut t *testing.T) !void {\n"
         "    t.Equal(Twice(21), 42)\n"
         "}\n";
}

} // namespace

int new_command(const std::vector<std::string> &args) {
  bool lib = false, unknown = false;
  std::vector<std::string> words;
  for (const std::string &a : args) {
    if (a == "--lib") lib = true;
    else if (!a.empty() && a[0] == '-') unknown = true;
    else words.push_back(a);
  }
  if (unknown || words.empty() || words.size() > 2) {
    fprintf(stderr, "usage: shield new [--lib] <module path> [package name]\n");
    return 1;
  }
  const std::string &path = words[0];
  if (!valid_module_path(path) || path_under(path, "std")) {
    fprintf(stderr, "shield: '%s' is not a module path: lower-case letters, "
                    "digits, '.', '-' and '_', in segments joined by '/', and "
                    "nothing under std\n",
            path.c_str());
    return 1;
  }

  // The directory is named like the repository a clone would make, and the
  // package like its importers will write it.
  std::string dir = path.substr(path.find_last_of('/') + 1);
  if (path_major(path)) {
    std::string before = path.substr(0, path.find_last_of('/'));
    dir = before.substr(before.find_last_of('/') + 1);
  }
  std::string name = words.size() == 2 ? words[1] : package_name_of(path);
  if (lib && !identifier(name)) {
    fprintf(stderr, "shield: '%s' cannot be written in a program; name the "
                    "package after the path: shield new --lib %s <name>\n",
            name.c_str(), path.c_str());
    return 1;
  }
  if (words.size() == 2 && !lib) {
    fprintf(stderr, "shield: a program's package is its main; a name is for a "
                    "library made with --lib\n");
    return 1;
  }
  if (words.size() == 2 && !identifier(words[1])) {
    fprintf(stderr, "shield: '%s' is not a name\n", words[1].c_str());
    return 1;
  }
  if (!fresh(dir)) {
    fprintf(stderr, "shield: '%s' is already there, and not empty\n", dir.c_str());
    return 1;
  }
  if (mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST) {
    fprintf(stderr, "shield: cannot make '%s': %s\n", dir.c_str(), strerror(errno));
    return 1;
  }

  int major = 0, minor = 0;
  sscanf(SWORD_VERSION, "%d.%d", &major, &minor);
  std::string mod = "module " + path + "\nsword " + std::to_string(major) + "." +
                    std::to_string(minor) + "\n";
  std::vector<std::string> made = {"sword.mod"};
  bool ok = write_file(dir + "/sword.mod", mod);
  if (lib) {
    std::string base = identifier(dir) ? dir : name;
    ok = ok && write_file(dir + "/" + base + ".sword", library(name)) &&
         write_file(dir + "/" + base + "_test.sword", library_test(name));
    made.push_back(base + ".sword");
    made.push_back(base + "_test.sword");
  } else {
    ok = ok && write_file(dir + "/main.sword", program_main()) &&
         write_file(dir + "/main_test.sword", program_test()) &&
         write_file(dir + "/.gitignore", "a.out\n");
    made.push_back("main.sword");
    made.push_back("main_test.sword");
  }
  if (!ok) return 1;

  // A repository of its own, unless it is going inside one already.
  bool inside = git({"-C", dir, "rev-parse", "--is-inside-work-tree"}).status == 0;
  bool repo = false;
  if (!inside) {
    Ran ran = git({"init", "--quiet", dir});
    repo = ran.status == 0;
  }

  std::string listing;
  for (const std::string &file : made) listing += (listing.empty() ? "" : ", ") + file;
  fprintf(stderr, "shield: made %s/ with %s%s\n", dir.c_str(), listing.c_str(),
          repo ? ", in a new git repository" : "");
  if (lib) fprintf(stderr, "        cd %s && shield test .\n", dir.c_str());
  else fprintf(stderr, "        cd %s && shield test . && shield . && ./a.out\n",
               dir.c_str());
  return 0;
}
