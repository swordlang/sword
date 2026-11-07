// Loading a library while the program runs. dlopen reads files and runs the
// library's initialisers, so it goes to the io pool like a file read and the
// task is put down meanwhile; dlclose runs finalisers and goes the same way.
//
// dlerror is per thread, and the call ran on an io thread: its message is
// copied out there, inside the job, into memory the caller owns. Read on the
// task's thread afterwards it would belong to whatever that thread did last.

#include "sword_rt.h"

#include <dlfcn.h>
#include <limits.h>
#include <string.h>

namespace {

struct Why {
  uint8_t *text;
  int64_t cap;
  int64_t *len;

  void say(const char *message) {
    if (!message) message = "";
    int64_t n = (int64_t)strlen(message);
    if (n > cap) n = cap;
    if (n > 0) memcpy(text, message, (size_t)n);
    *len = n;
  }
};

struct OpenCall {
  const char *path; // null: the program itself
  void *handle;
  Why why;
};

int64_t do_open(void *p) {
  OpenCall *call = (OpenCall *)p;
  dlerror();
  // Every symbol bound now rather than at its first call, so a missing one is
  // this call's failure and not a crash somewhere later; and nothing it brings
  // in leaks into the lookup of libraries loaded after it.
  call->handle = dlopen(call->path, RTLD_NOW | RTLD_LOCAL);
  if (!call->handle) call->why.say(dlerror());
  return call->handle ? 0 : -1;
}

struct CloseCall {
  void *handle;
  Why why;
};

int64_t do_close(void *p) {
  CloseCall *call = (CloseCall *)p;
  dlerror();
  if (dlclose(call->handle) == 0) return 0;
  call->why.say(dlerror());
  return -1;
}

} // namespace

extern "C" {

void *sword_dl_open(const char *path, int64_t path_len, uint8_t *why,
                    int64_t why_cap, int64_t *why_len) {
  *why_len = 0;
  Why said{why, why_cap, why_len};
  char name[PATH_MAX];
  if (path_len < 0 || path_len >= (int64_t)sizeof(name)) {
    said.say("the path is longer than the system allows");
    return nullptr;
  }
  if (memchr(path, 0, (size_t)path_len)) {
    said.say("the path has a zero byte in it");
    return nullptr;
  }
  memcpy(name, path, (size_t)path_len);
  name[path_len] = '\0';
  OpenCall call{path_len == 0 ? nullptr : name, nullptr, said};
  sword_offload(do_open, &call);
  return call.handle;
}

// A lookup reads tables already in memory, so it runs here. The message is
// cleared first because a symbol may legitimately be null, and only dlerror
// tells that apart from one that is missing.
void *sword_dl_symbol(void *handle, const char *symbol, int64_t symbol_len,
                      uint8_t *why, int64_t why_cap, int64_t *why_len) {
  *why_len = 0;
  Why said{why, why_cap, why_len};
  char name[1024];
  if (symbol_len <= 0 || symbol_len >= (int64_t)sizeof(name) ||
      memchr(symbol, 0, (size_t)symbol_len)) {
    said.say("that is not a symbol name");
    return nullptr;
  }
  memcpy(name, symbol, (size_t)symbol_len);
  name[symbol_len] = '\0';
  dlerror();
  void *found = dlsym(handle, name);
  if (found) return found;
  const char *message = dlerror();
  said.say(message ? message : "the symbol is there, and its address is null");
  return nullptr;
}

int32_t sword_dl_close(void *handle, uint8_t *why, int64_t why_cap,
                       int64_t *why_len) {
  *why_len = 0;
  CloseCall call{handle, Why{why, why_cap, why_len}};
  return (int32_t)sword_offload(do_close, &call);
}

// What a shared library's file name ends in here.
const char *sword_dl_suffix(int64_t *len) {
#ifdef __APPLE__
  static const char suffix[] = ".dylib";
#else
  static const char suffix[] = ".so";
#endif
  *len = (int64_t)sizeof(suffix) - 1;
  return suffix;
}

} // extern "C"
