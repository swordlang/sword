#include "sword_chan.h"

#include "sword_rt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

// The header, then the slots. Padded to sixteen so the values behind it are
// aligned for anything Sword can put in a channel.
struct Chan {
  unsigned char guard[SWORD_GUARD_SIZE];
  int64_t elem;
  int64_t slots;   // how many values fit; 1 for a handover
  int64_t head;    // where the next value is taken from
  int64_t count;   // how many are in it
  int64_t waiting; // receivers parked on it
  int32_t closed;
  int32_t direct; // capacity zero: a handover rather than a queue
};

// As many cases as one select may have. The checker holds the language to the
// same number, so this is a second line of defence rather than a limit anybody
// meets.
const int64_t kMaxCases = 64;

const int64_t kHeader = (int64_t)((sizeof(Chan) + 15) / 16 * 16);

unsigned char *slot_at(Chan *c, int64_t index) {
  return (unsigned char *)c + kHeader + index * c->elem;
}

// Both of these run with the channel's guard held.
bool room_to_send(Chan *c) { return !c->closed && c->count < c->slots; }

// A handover with nobody waiting is not ready: putting the value in would be the
// wait that the caller was trying to avoid.
bool can_send_now(Chan *c) {
  return room_to_send(c) && (!c->direct || c->waiting > 0);
}

void put(Chan *c, const void *value) {
  int64_t at = (c->head + c->count) % c->slots;
  memcpy(slot_at(c, at), value, (size_t)c->elem);
  c->count++;
}

void take(Chan *c, void *into) {
  memcpy(into, slot_at(c, c->head), (size_t)c->elem);
  c->head = (c->head + 1) % c->slots;
  c->count--;
}

} // namespace

extern "C" {

int64_t sword_chan_bytes(int64_t capacity, int64_t elem) {
  int64_t slots = capacity > 0 ? capacity : 1;
  if (elem <= 0) elem = 1;
  return kHeader + slots * elem;
}

void sword_chan_init(void *mem, int64_t capacity, int64_t elem) {
  Chan *c = (Chan *)mem;
  memset(c, 0, (size_t)kHeader); // an all-zero guard is an unlocked one
  c->elem = elem <= 0 ? 1 : elem;
  c->slots = capacity > 0 ? capacity : 1;
  c->direct = capacity > 0 ? 0 : 1;
}

int32_t sword_chan_send(void *chan, const void *value) {
  Chan *c = (Chan *)chan;
  sword_mutex_lock(c->guard);
  while (!room_to_send(c)) {
    if (c->closed) {
      sword_mutex_unlock(c->guard);
      return -1;
    }
    sword_mutex_wait(c->guard);
  }
  if (c->closed) {
    sword_mutex_unlock(c->guard);
    return -1;
  }
  put(c, value);
  sword_mutex_notify_all(c->guard);

  // A handover is not finished until somebody has taken it.
  while (c->direct && c->count > 0 && !c->closed)
    sword_mutex_wait(c->guard);
  sword_mutex_unlock(c->guard);
  return 0;
}

int32_t sword_chan_recv(void *chan, void *into) {
  Chan *c = (Chan *)chan;
  sword_mutex_lock(c->guard);
  c->waiting++;
  // A handover becomes sendable only once somebody is waiting for it, so a
  // `select` with a send case on this channel is waiting for exactly this and
  // nothing else will tell it.
  if (c->direct) sword_mutex_notify_watchers(c->guard);
  while (c->count == 0 && !c->closed) sword_mutex_wait(c->guard);
  c->waiting--;
  if (c->count == 0) {
    sword_mutex_unlock(c->guard);
    return 0;
  }
  take(c, into);
  sword_mutex_notify_all(c->guard);
  sword_mutex_unlock(c->guard);
  return 1;
}

int32_t sword_chan_try_send(void *chan, const void *value) {
  Chan *c = (Chan *)chan;
  sword_mutex_lock(c->guard);
  bool ok = can_send_now(c);
  if (ok) {
    put(c, value);
    sword_mutex_notify_all(c->guard);
  }
  sword_mutex_unlock(c->guard);
  return ok ? 1 : 0;
}

int32_t sword_chan_try_recv(void *chan, void *into) {
  Chan *c = (Chan *)chan;
  sword_mutex_lock(c->guard);
  bool ok = c->count > 0;
  if (ok) {
    take(c, into);
    sword_mutex_notify_all(c->guard);
  }
  sword_mutex_unlock(c->guard);
  return ok ? 1 : 0;
}

void sword_chan_close(void *chan) {
  Chan *c = (Chan *)chan;
  sword_mutex_lock(c->guard);
  c->closed = 1;
  sword_mutex_notify_all(c->guard);
  sword_mutex_unlock(c->guard);
}

int64_t sword_chan_len(void *chan) {
  Chan *c = (Chan *)chan;
  sword_mutex_lock(c->guard);
  int64_t n = c->count;
  sword_mutex_unlock(c->guard);
  return n;
}

// The capacity it was asked for, so a handover answers zero.
int64_t sword_chan_cap(void *chan) {
  Chan *c = (Chan *)chan;
  return c->direct ? 0 : c->slots;
}

int32_t sword_chan_closed(void *chan) {
  Chan *c = (Chan *)chan;
  sword_mutex_lock(c->guard);
  int32_t shut = c->closed;
  sword_mutex_unlock(c->guard);
  return shut;
}

int64_t sword_chan_select(void **chans, const int32_t *ops, void **values,
                          int64_t n, int32_t has_default, int32_t *got) {
  *got = 0;
  if (n <= 0) return has_default ? 0 : -1;
  if (n > kMaxCases) {
    fputs("sword: select with more cases than the runtime holds\n", stderr);
    abort();
  }

  // Where the sweep starts, moved along every time, so a case that is always
  // ready cannot starve the ones after it. One counter for the process is enough
  // — this is fairness, not accounting.
  static std::atomic<uint64_t> turn{0};
  uint64_t start = turn.fetch_add(1, std::memory_order_relaxed);

  // One waiter node per channel, on this task's stack: it is parked while they are
  // in use, so the stack is going nowhere.
  void *guards[kMaxCases];
  for (int64_t i = 0; i < n; i++) guards[i] = ((Chan *)chans[i])->guard;
  int64_t node_bytes = sword_mutex_watch_bytes() * n;
  void *nodes = alloca((size_t)node_bytes);

  while (true) {
    // Registered *before* the sweep below. A value that arrives between the sweep
    // and the park has to find this task on a list, or its wake is lost and the
    // task never wakes up again.
    sword_mutex_watch(guards, n, nodes);

    // A handover hands over only when somebody is waiting for it, so say so before
    // looking.
    for (int64_t i = 0; i < n; i++) {
      if (ops[i] != SWORD_CHAN_RECV) continue;
      Chan *c = (Chan *)chans[i];
      sword_mutex_lock(c->guard);
      c->waiting++;
      // The other side of the same coin: another task's select may be sitting on
      // a send case for this handover, and this is the change it is waiting for.
      if (c->direct) sword_mutex_notify_watchers(c->guard);
      sword_mutex_unlock(c->guard);
    }

    int64_t chose = -1;
    for (int64_t k = 0; k < n && chose < 0; k++) {
      int64_t i = (int64_t)((start + (uint64_t)k) % (uint64_t)n);
      Chan *c = (Chan *)chans[i];
      if (ops[i] == SWORD_CHAN_SEND) {
        if (sword_chan_try_send(c, values[i])) chose = i;
        continue;
      }
      // A receive that finds the channel closed and empty has its answer, and the
      // answer is "nothing more". That is the case firing, not the case waiting.
      sword_mutex_lock(c->guard);
      if (c->count > 0) {
        take(c, values[i]);
        sword_mutex_notify_all(c->guard);
        *got = 1;
        chose = i;
      } else if (c->closed) {
        *got = 0;
        chose = i;
      }
      sword_mutex_unlock(c->guard);
    }

    bool leaving = chose >= 0 || has_default != 0;

    // Every case a send, every channel closed: nothing will ever change, and
    // parking would be a hang with no explanation. A receive case is never in this
    // position — a closed channel answers it.
    bool hopeless = !leaving;
    for (int64_t i = 0; i < n && hopeless; i++) {
      if (ops[i] == SWORD_CHAN_RECV) hopeless = false;
      else if (!sword_chan_closed((Chan *)chans[i])) hopeless = false;
    }

    if (!leaving && !hopeless) sword_mutex_park();

    for (int64_t i = 0; i < n; i++) {
      if (ops[i] != SWORD_CHAN_RECV) continue;
      Chan *c = (Chan *)chans[i];
      sword_mutex_lock(c->guard);
      c->waiting--;
      sword_mutex_unlock(c->guard);
    }
    sword_mutex_unwatch(guards, n, nodes);

    if (chose >= 0) return chose;
    if (has_default) return n;
    if (hopeless) {
      fputs("sword: select cannot go on — every channel it sends to is closed\n",
            stderr);
      abort();
    }
  }
}
}
