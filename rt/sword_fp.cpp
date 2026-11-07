// --audit=footprints (sword_rt.h). Nothing here runs in a program built
// without it, and dead stripping leaves it out of the binary.

#include "sword_rt.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

namespace {

struct Range {
  uintptr_t from;
  uintptr_t to;
  bool writes;
};

struct Footprint {
  Footprint *parent;
  void *key;
  const char *where;
  int64_t len;
  int64_t line;
  // start -> (end, writes). A range granted twice keeps the stronger mode.
  Vec<Range> ranges;
};

// One lock for all of it: this is a mode for checking, and a grant has to
// see every other live footprint as it is.
Mutex g_lock;
Vec<Footprint *> g_live;

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
  Footprint *fp = (Footprint *)calloc(1, sizeof(Footprint));
  fp->parent = (Footprint *)parent;
  fp->key = key;
  fp->where = where;
  fp->len = len;
  fp->line = line;
  Lock hold(g_lock);
  g_live.push_back(fp);
  return fp;
}

int32_t sword_fp_grant(void *handle, const void *at, int64_t bytes,
                       int32_t write) {
  if (bytes <= 0 || !at) return 0;
  Footprint *fp = (Footprint *)handle;
  uintptr_t from = (uintptr_t)at, to = from + (uintptr_t)bytes;
  Lock hold(g_lock);

  for (size_t i = 0; i < fp->ranges.size(); i++) {
    if (fp->ranges[i].from == from && fp->ranges[i].to >= to &&
        (fp->ranges[i].writes || !write))
      return 0;
  }

  for (const Footprint *other : g_live) {
    bool related = false;
    for (const Footprint *up = fp; up; up = up->parent)
      if (up == other) related = true;
    for (const Footprint *up = other->parent; up; up = up->parent)
      if (up == fp) related = true;
    if (related) continue;

    for (size_t i = 0; i < other->ranges.size() && other->ranges[i].from < to;
         i++) {
      uintptr_t lo = other->ranges[i].from, hi = other->ranges[i].to;
      if (hi <= from || (!write && !other->ranges[i].writes)) continue;
      overlap(other, fp, lo > from ? lo : from, hi < to ? hi : to);
    }
  }

  for (size_t i = 0; i < fp->ranges.size(); i++) {
    if (fp->ranges[i].from == from) {
      if (fp->ranges[i].to < to) fp->ranges[i].to = to;
      fp->ranges[i].writes = fp->ranges[i].writes || write;
      return 1;
    }
  }
  size_t pos = fp->ranges.size();
  while (pos > 0 && fp->ranges[pos - 1].from > from) pos--;
  fp->ranges.push_back(Range{from, to, write != 0});
  for (size_t i = fp->ranges.size() - 1; i > pos; i--)
    fp->ranges[i] = fp->ranges[i - 1];
  fp->ranges[pos] = Range{from, to, write != 0};
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
  Lock hold(g_lock);
  for (size_t i = 0; i < g_live.size();) {
    if (g_live[i]->key == key) {
      free(g_live[i]->ranges.data);
      free(g_live[i]);
      g_live.erase(g_live.begin() + i);
    } else {
      i++;
    }
  }
}

} // extern "C"
