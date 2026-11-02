// --audit=footprints (sword_rt.h). Nothing here runs in a program built
// without it, and dead stripping leaves it out of the binary.

#include "sword_rt.h"

#include <map>
#include <mutex>
#include <set>
#include <stdio.h>
#include <stdlib.h>
#include <vector>

namespace {

struct Footprint {
  Footprint *parent;
  void *key;
  const char *where;
  int64_t len;
  int64_t line;
  // start -> (end, writes). A range granted twice keeps the stronger mode.
  std::map<uintptr_t, std::pair<uintptr_t, bool>> ranges;
};

// One lock for all of it: this is a mode for checking, and a grant has to
// see every other live footprint as it is.
std::mutex g_lock;
std::set<Footprint *> g_live;

[[noreturn]] void overlap(const Footprint *a, const Footprint *b,
                          uintptr_t from, uintptr_t to) {
  fprintf(stderr,
          "footprint overlap: the task at %.*s:%lld and the task at "
          "%.*s:%lld both reach [%#llx, %#llx), and one writes\n",
          (int)a->len, a->where, (long long)a->line, (int)b->len, b->where,
          (long long)b->line, (unsigned long long)from,
          (unsigned long long)to);
  fflush(stderr);
  abort();
}

} // namespace

extern "C" {

void *sword_fp_current(void) { return *sword_footprint_slot(); }

void *sword_fp_open(void *parent, void *key, const char *where, int64_t len,
                    int64_t line) {
  Footprint *fp = new Footprint{(Footprint *)parent, key, where, len, line, {}};
  std::lock_guard<std::mutex> hold(g_lock);
  g_live.insert(fp);
  return fp;
}

int32_t sword_fp_grant(void *handle, const void *at, int64_t bytes,
                       int32_t write) {
  if (bytes <= 0 || !at) return 0;
  Footprint *fp = (Footprint *)handle;
  uintptr_t from = (uintptr_t)at, to = from + (uintptr_t)bytes;
  std::lock_guard<std::mutex> hold(g_lock);

  auto mine = fp->ranges.find(from);
  if (mine != fp->ranges.end() && mine->second.first >= to &&
      (mine->second.second || !write))
    return 0;

  std::set<const Footprint *> above;
  for (const Footprint *up = fp; up; up = up->parent) above.insert(up);
  for (const Footprint *other : g_live) {
    if (above.count(other)) continue;
    // Whoever lent this memory to `fp` is an ancestor; a descendant of fp
    // cannot exist yet. Everyone else must not hold it in a way that clashes.
    bool related = false;
    for (const Footprint *up = other->parent; up; up = up->parent)
      if (up == fp) related = true;
    if (related) continue;
    // Every range that starts before this one ends: one granted earlier and
    // wider can start long before `from`.
    for (auto it = other->ranges.begin();
         it != other->ranges.end() && it->first < to; ++it) {
      uintptr_t lo = it->first, hi = it->second.first;
      if (hi <= from || (!write && !it->second.second)) continue;
      overlap(other, fp, lo > from ? lo : from, hi < to ? hi : to);
    }
  }
  auto &slot = fp->ranges[from];
  if (slot.first < to) slot.first = to;
  slot.second = slot.second || write;
  return 1;
}

void *sword_fp_enter(void *fp) {
  void **slot = sword_footprint_slot();
  void *previous = *slot;
  *slot = fp;
  return previous;
}

void sword_fp_leave(void *previous) { *sword_footprint_slot() = previous; }

void sword_fp_join(void *key) {
  std::lock_guard<std::mutex> hold(g_lock);
  for (auto it = g_live.begin(); it != g_live.end();) {
    if ((*it)->key == key) {
      delete *it;
      it = g_live.erase(it);
    } else {
      ++it;
    }
  }
}

} // extern "C"
