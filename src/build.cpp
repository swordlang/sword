#include "build.h"

#include "backend_llvm.h"
#include "fetch.h"
#include "sha256.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <dirent.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

namespace {

bool for_apple(const std::string &target) {
  if (target.empty()) {
#ifdef __APPLE__
    return true;
#else
    return false;
#endif
  }
  return target.find("apple") != std::string::npos ||
         target.find("darwin") != std::string::npos;
}

// An object is only as good as the compiler that made it, and on macOS the
// clang on the PATH is a shim whose file never changes when Xcode does, so the
// version it reports is what goes into the key.
std::string clang_version() {
  std::string text;
  FILE *p = popen("clang --version 2>/dev/null", "r");
  if (!p) return text;
  char buffer[512];
  size_t n;
  while ((n = fread(buffer, 1, sizeof(buffer), p)) > 0) text.append(buffer, n);
  pclose(p);
  return text;
}

std::string unit_text(const IrModule &mod, const std::string &unit) {
  char *data = nullptr;
  size_t size = 0;
  FILE *f = open_memstream(&data, &size);
  emit_llvm_unit(mod, unit, f);
  fclose(f);
  std::string text(data, size);
  free(data);
  return text;
}

// An object is dated by its last use, not its making: a hit moves its time
// on, and what nothing has used for a month is taken away.
const time_t kUnused = 30 * 24 * 60 * 60;
const time_t kSweepEvery = 24 * 60 * 60;

bool use(const std::string &path) {
  return utimes(path.c_str(), nullptr) == 0;
}

time_t modified(const std::string &path) {
  struct stat info;
  return stat(path.c_str(), &info) == 0 ? info.st_mtime : 0;
}

// Once a day at most, so the look over the directory costs a build nothing.
// Two builds may both decide to sweep, and then the second finds less to do.
void sweep(const std::string &dir) {
  std::string stamp = dir + "/.swept";
  time_t now = time(nullptr);
  time_t last = modified(stamp);
  if (last && now - last < kSweepEvery) return;
  FILE *f = fopen(stamp.c_str(), "w");
  if (f) fclose(f);
  use(stamp);

  DIR *d = opendir(dir.c_str());
  if (!d) return;
  while (dirent *e = readdir(d)) {
    std::string name = e->d_name;
    if (name[0] == '.') continue;
    std::string path = dir + "/" + name;
    // An object half written by a build that died is a day old at most when
    // its build is still running.
    bool whole = name.size() > 2 && name.compare(name.size() - 2, 2, ".o") == 0;
    time_t age = now - modified(path);
    if (age > (whole ? kUnused : kSweepEvery)) unlink(path.c_str());
  }
  closedir(d);
}

struct Job {
  std::string ll, partial, object;
  pid_t pid = -1;
};

bool start(Job &job, const std::vector<std::string> &flags) {
  std::vector<std::string> args = {"clang"};
  args.insert(args.end(), flags.begin(), flags.end());
  for (const char *a : {"-x", "ir"}) args.push_back(a);
  args.push_back(job.ll);
  for (const char *a : {"-c", "-o"}) args.push_back(a);
  args.push_back(job.partial);
  std::vector<char *> argv;
  for (std::string &a : args) argv.push_back(&a[0]);
  argv.push_back(nullptr);
  return posix_spawnp(&job.pid, "clang", nullptr, nullptr, argv.data(),
                      environ) == 0;
}

} // namespace

std::vector<std::string> split_link_flags(const std::string &target) {
  if (for_apple(target)) return {"-Wl,-dead_strip"};
  return {"-Wl,--gc-sections"};
}

bool compile_split(const IrModule &mod, const std::string &opt_level,
                   const std::string &target, const std::string &scratch,
                   std::vector<std::string> &objects) {
  std::vector<std::string> flags = {"-O" + opt_level, "-Wno-override-module"};
  if (!target.empty()) flags.push_back("--target=" + target);
  // ld64 strips by atom without being asked; elsewhere a section per function
  // is what lets the linker see one.
  if (!for_apple(target)) {
    flags.push_back("-ffunction-sections");
    flags.push_back("-fdata-sections");
  }
  std::string how = clang_version();
  for (const std::string &f : flags) how += f + "\n";

  std::string dir = cache_root();
  mkdir(dir.c_str(), 0755);
  dir += "/build";
  if (mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST) {
    fprintf(stderr, "shield: cannot make '%s'\n", dir.c_str());
    return false;
  }

  std::vector<Job> pending;
  for (const std::string &unit : llvm_units(mod)) {
    std::string text = unit_text(mod, unit);
    std::string object = dir + "/" + sha256_hex(how + text) + ".o";
    objects.push_back(object);
    if (use(object)) continue;

    Job job;
    job.object = object;
    job.partial = object + "." + std::to_string(getpid());
    job.ll = scratch + "/" + std::to_string(pending.size()) + ".ll";
    FILE *ll = fopen(job.ll.c_str(), "w");
    if (!ll || fwrite(text.data(), 1, text.size(), ll) != text.size()) {
      if (ll) fclose(ll);
      fprintf(stderr, "shield: cannot write '%s'\n", job.ll.c_str());
      return false;
    }
    fclose(ll);
    pending.push_back(job);
  }

  long cores = sysconf(_SC_NPROCESSORS_ONLN);
  size_t width = cores > 0 ? (size_t)cores : 1;
  size_t next = 0, running = 0;
  bool ok = true;
  while (next < pending.size() || running > 0) {
    while (ok && running < width && next < pending.size()) {
      if (!start(pending[next], flags)) {
        fprintf(stderr, "shield: cannot run clang\n");
        ok = false;
        break;
      }
      next++;
      running++;
    }
    if (running == 0) break;
    int status = 0;
    pid_t done = waitpid(-1, &status, 0);
    if (done < 0) {
      if (errno == EINTR) continue;
      break;
    }
    for (Job &job : pending) {
      if (job.pid != done) continue;
      running--;
      job.pid = -1;
      unlink(job.ll.c_str());
      // Into place in one step, so a build running beside this one finds the
      // object whole or not at all.
      if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        rename(job.partial.c_str(), job.object.c_str());
      } else {
        unlink(job.partial.c_str());
        fprintf(stderr, "shield: clang failed while assembling %s\n",
                job.ll.c_str());
        ok = false;
      }
    }
  }
  for (size_t i = next; i < pending.size(); i++) unlink(pending[i].ll.c_str());
  if (ok) sweep(dir);
  return ok;
}

int clean_command(const std::vector<std::string> &args) {
  if (!args.empty()) {
    fprintf(stderr, "usage: shield clean\n");
    return 1;
  }
  std::string dir = cache_root() + "/build";
  DIR *d = opendir(dir.c_str());
  if (!d) {
    printf("nothing to remove\n");
    return 0;
  }
  int count = 0;
  long long bytes = 0;
  bool ok = true;
  while (dirent *e = readdir(d)) {
    std::string name = e->d_name;
    if (name == "." || name == "..") continue;
    std::string path = dir + "/" + name;
    struct stat info;
    if (stat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode)) continue;
    if (unlink(path.c_str()) != 0) {
      fprintf(stderr, "shield: cannot remove '%s'\n", path.c_str());
      ok = false;
      continue;
    }
    if (name[0] == '.') continue;
    count++;
    bytes += info.st_size;
  }
  closedir(d);
  rmdir(dir.c_str());
  printf("removed %d object%s, %.1f MiB\n", count, count == 1 ? "" : "s",
         bytes / (1024.0 * 1024.0));
  return ok ? 0 : 1;
}
