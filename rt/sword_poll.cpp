#include "sword_poll.h"
#include "sword_os.h"
#include "sword_rt.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/epoll.h>
#define SWORD_EPOLL 1
#else
#include <sys/event.h>
#define SWORD_KQUEUE 1
#endif

namespace {

// Several tasks may wait on the same descriptor and direction: a server with
// more than one acceptor has all of them sitting on one listener. Readiness
// wakes every one of them, and the losers find EAGAIN and come back — which is
// what the kernel means by a ready listener anyway.
struct Waiter {
  void *token = nullptr;
  uint64_t seq = 0; // which registration this was, so a stale timer is ignored
};

struct Slot {
  Vec<Waiter> waiting;
};

// Every forget moves a descriptor's generation on. A wait carries the
// generation its caller read before the call that answered "not yet", and one
// that no longer matches is for a descriptor closed since then — whatever the
// number names now. A number is handed out again as soon as it is closed, often
// to the next socket, sometimes to the poller's own pipe, and a wait that took it
// for the old descriptor slept for good.
//
// Read on every socket wait, so without the lock: chunks of atomics that are
// never moved or freed once made.
const size_t kGenChunk = 4096;
const size_t kGenChunks = 4096;
std::atomic<std::atomic<uint64_t> *> gen_chunks[kGenChunks];

std::atomic<uint64_t> *gen_slot(int fd, bool make) {
  if (fd < 0 || (size_t)fd / kGenChunk >= kGenChunks) return nullptr;
  std::atomic<std::atomic<uint64_t> *> &at = gen_chunks[(size_t)fd / kGenChunk];
  std::atomic<uint64_t> *chunk = at.load(std::memory_order_acquire);
  if (!chunk && make) {
    std::atomic<uint64_t> *fresh = (std::atomic<uint64_t> *)calloc(
        kGenChunk, sizeof(std::atomic<uint64_t>));
    if (at.compare_exchange_strong(chunk, fresh, std::memory_order_acq_rel))
      chunk = fresh;
    else
      free(fresh);
  }
  return chunk ? &chunk[(size_t)fd % kGenChunk] : nullptr;
}

uint64_t generation_of(int fd) {
  std::atomic<uint64_t> *g = gen_slot(fd, false);
  return g ? g->load(std::memory_order_acquire) : 0;
}

struct Timer {
  int64_t deadline;
  int slot;
  uint64_t seq;
};

struct Sleeper {
  uint64_t seq;
  void *token;
};

struct Poller {
  int handle = -1;         // the kqueue or epoll descriptor
  int nudge[2] = {-1, -1}; // a pipe, to cut a wait short when a deadline moves
  pthread_t thread = 0;
  bool thread_started = false;
  std::atomic<bool> stopping{false};
  sword_wake_fn wake = nullptr;

  Mutex lock;
  // Indexed by descriptor, two slots each. Descriptors are small and dense, so
  // a vector beats a hash for the lookup that happens on every event.
  Vec<Slot> slots;
  // deadline -> (slot, seq), where a slot of -1 means the entry is a sleeper
  // waiting on the time alone rather than on any descriptor.
  Vec<Timer> timers;
  Vec<Sleeper> sleepers;
  uint64_t next_seq = 1;
  // Descriptors to close, for the poller thread to close. See sword_poll_close.
  // Each asks with a ticket and waits until `closed_upto` reaches it, so a
  // close has happened by the time the call that asked for it returns.
  Vec<int> closing;
  uint64_t close_asked = 0;
  uint64_t closed_upto = 0;
  Cond closed;
};

Poller &poller();

void insert_timer(Poller &p, int64_t deadline, int slot, uint64_t seq) {
  size_t pos = p.timers.size();
  while (pos > 0 && p.timers[pos - 1].deadline > deadline) pos--;
  p.timers.push_back(Timer{deadline, slot, seq});
  for (size_t i = p.timers.size() - 1; i > pos; i--)
    p.timers[i] = p.timers[i - 1];
  p.timers[pos] = Timer{deadline, slot, seq};
}

size_t slot_of(int fd, int writable) {
  return (size_t)fd * 2 + (writable ? 1 : 0);
}

void make_room(Poller &p, size_t slot) {
  if (p.slots.size() <= slot) p.slots.resize(slot + 64);
}

// Hands each token back exactly once: whoever takes it out of the slot owns the
// wake. A descriptor that becomes ready and times out in the same breath
// therefore wakes its tasks once, with whichever answer arrived first.
void deliver(Poller &p, size_t slot, int result) {
  Vec<Waiter> woken;
  {
    Lock held(p.lock);
    if (slot >= p.slots.size() || p.slots[slot].waiting.empty()) return;
    woken.swap(p.slots[slot].waiting);
  }
  for (const Waiter &w : woken) p.wake(w.token, result);
}

// A task waiting on the clock and nothing else.
void deliver_sleeper(Poller &p, uint64_t seq) {
  void *token = nullptr;
  {
    Lock held(p.lock);
    for (size_t i = 0; i < p.sleepers.size(); i++) {
      if (p.sleepers[i].seq != seq) continue;
      token = p.sleepers[i].token;
      p.sleepers[i] = p.sleepers.back();
      p.sleepers.pop_back();
      break;
    }
  }
  if (token) p.wake(token, SWORD_POLL_TIMEOUT);
}

// One waiter, for a deadline that belongs to it alone.
void deliver_one(Poller &p, size_t slot, uint64_t seq, int result) {
  void *token = nullptr;
  {
    Lock held(p.lock);
    if (slot >= p.slots.size()) return;
    auto &waiting = p.slots[slot].waiting;
    for (size_t i = 0; i < waiting.size(); i++) {
      if (waiting[i].seq != seq) continue;
      token = waiting[i].token;
      waiting.erase(waiting.begin() + (long)i);
      break;
    }
  }
  if (token) p.wake(token, result);
}

// False when the descriptor has been forgotten since `generation` was read, or
// the kernel refused it, which is what a closed one gets. Nothing will ever
// report on it, so the caller has to. The poller's own pipe is armed without a
// generation.
//
// Under the lock, and checked against the generation there, so a registration
// and the close that follows a forget never overlap: one that started finishes
// before the forget can move the generation on, and one that starts later sees
// it moved. With kqueue the close itself also happens under this lock, on the
// poller's thread — see close_pending.
bool arm(Poller &p, int fd, int writable, uint64_t generation, bool checked) {
  Lock held(p.lock);
  if (checked && generation_of(fd) != generation) return false;
#ifdef SWORD_KQUEUE
  struct kevent change;
  EV_SET(&change, fd, writable ? EVFILT_WRITE : EVFILT_READ,
         EV_ADD | EV_ONESHOT, 0, 0, nullptr);
  return kevent(p.handle, &change, 1, nullptr, 0, nullptr) == 0;
#else
  // epoll is per descriptor rather than per filter, so both directions share
  // one registration and the events are rebuilt from what is still waiting.
  size_t base = slot_of(fd, 0);
  epoll_event ev;
  memset(&ev, 0, sizeof(ev));
  ev.data.fd = fd;
  ev.events = EPOLLONESHOT;
  if (base < p.slots.size() && !p.slots[base].waiting.empty())
    ev.events |= EPOLLIN;
  if (base + 1 < p.slots.size() && !p.slots[base + 1].waiting.empty())
    ev.events |= EPOLLOUT;
  return epoll_ctl(p.handle, EPOLL_CTL_MOD, fd, &ev) == 0 ||
         epoll_ctl(p.handle, EPOLL_CTL_ADD, fd, &ev) == 0;
#endif
}

void disarm(Poller &p, int fd) {
#ifdef SWORD_KQUEUE
  struct kevent change[2];
  EV_SET(&change[0], fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
  EV_SET(&change[1], fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
  kevent(p.handle, change, 2, nullptr, 0, nullptr);
#else
  epoll_ctl(p.handle, EPOLL_CTL_DEL, fd, nullptr);
#endif
}

void nudge(Poller &p) {
  char one = 1;
  ssize_t ignored = write(p.nudge[1], &one, 1);
  (void)ignored;
}

// Milliseconds until the nearest deadline, or -1 when there is none. Capped so
// that a poller with nothing to do still notices a stop.
int next_timeout(Poller &p) {
  Lock held(p.lock);
  if (p.timers.empty()) return 200;
  int64_t now = sword_time_mono();
  int64_t soonest = p.timers[0].deadline;
  if (soonest <= now) return 0;
  int64_t millis = (soonest - now + 999999) / 1000000;
  return millis > 200 ? 200 : (int)millis;
}

void expire(Poller &p) {
  int64_t now = sword_time_mono();
  Vec<Timer> due;
  {
    Lock held(p.lock);
    while (!p.timers.empty() && p.timers[0].deadline <= now) {
      due.push_back(p.timers[0]);
      p.timers.erase(p.timers.begin());
    }
  }
  // A registration that has already been answered is simply not there any
  // more, so the sequence number is all the checking this needs.
  for (const Timer &entry : due) {
    if (entry.slot == -1) deliver_sleeper(p, entry.seq);
    else deliver_one(p, (size_t)entry.slot, entry.seq, SWORD_POLL_TIMEOUT);
  }
}

// The poller's own thread closes a socket, between one kevent and the next and
// under the lock every registration takes. On macOS a close that meets a kqueue
// registration of the same socket — one being added, or a knote that fired as it
// was attached, the socket already shut down — can wait in the kernel for good,
// in a state not even SIGKILL ends. Reproduced in plain C; which interleaving
// does it was not worth more stuck processes to pin down, since doing the close
// here rules every one of them out.
//
// Between the forget and this close the descriptor is still open, so a task
// that read its generation after the forget can still find it "not ready" and
// register. The generation moves on again here, and whoever registered in
// between is told, or the close would drop the registration without a word.
// The close itself, under the lock: the generation moves on once more, anyone
// registered is taken off to be told, and the registration goes before the
// descriptor does. Between the forget and here the descriptor was still open, and
// a task that read the forgotten generation could still find it "not ready" and
// register — the forget had already told everyone, and this is the only other
// word it gets. The lock is what keeps a registration from landing between the
// telling and the close.
//
// The generation moves on *after* the close. A task reads it without the lock
// and then makes its call, so one that read the new generation must find the
// descriptor already gone; moving it first let a task read the new one, find the
// descriptor still open and "not ready", and register with a generation nothing
// would ever move again — on the poller's own kqueue, once, which took the
// number the moment it was free.
void close_locked(Poller &p, int fd, Vec<Waiter> &woken) {
  for (int writable = 0; writable < 2; writable++) {
    size_t slot = slot_of(fd, writable);
    if (slot >= p.slots.size()) continue;
    for (const Waiter &w : p.slots[slot].waiting) woken.push_back(w);
    p.slots[slot].waiting.clear();
  }
  if (p.handle >= 0) disarm(p, fd);
  close(fd);
  if (std::atomic<uint64_t> *g = gen_slot(fd, true))
    g->fetch_add(1, std::memory_order_acq_rel);
}

void close_pending(Poller &p) {
  Vec<Waiter> woken;
  {
    Lock held(p.lock);
    for (int fd : p.closing) close_locked(p, fd, woken);
    p.closing.clear();
    p.closed_upto = p.close_asked;
  }
  p.closed.notify_all();
  for (const Waiter &w : woken) p.wake(w.token, SWORD_POLL_FAILED);
}

void drain_nudge(Poller &p) {
  char buffer[64];
  while (read(p.nudge[0], buffer, sizeof(buffer)) > 0) {
  }
}

void loop() {
  Poller &p = poller();
#ifdef SWORD_KQUEUE
  struct kevent events[256];
#else
  epoll_event events[256];
#endif

  while (!p.stopping.load(std::memory_order_acquire)) {
    int timeout = next_timeout(p);

#ifdef SWORD_KQUEUE
    timespec wait;
    wait.tv_sec = timeout / 1000;
    wait.tv_nsec = (long)(timeout % 1000) * 1000000;
    int count = kevent(p.handle, nullptr, 0, events, 256, &wait);
#else
    int count = epoll_wait(p.handle, events, 256, timeout);
#endif
    if (count < 0) {
      if (errno == EINTR) continue;
      break;
    }

    for (int i = 0; i < count; i++) {
#ifdef SWORD_KQUEUE
      int fd = (int)events[i].ident;
      bool writable = events[i].filter == EVFILT_WRITE;
      if (fd == p.nudge[0]) {
        drain_nudge(p);
        arm(p, p.nudge[0], 0, 0, false);
        continue;
      }
      deliver(p, slot_of(fd, writable), SWORD_POLL_READY);
#else
      int fd = events[i].data.fd;
      if (fd == p.nudge[0]) {
        drain_nudge(p);
        arm(p, p.nudge[0], 0, 0, false);
        continue;
      }
      uint32_t got = events[i].events;
      // A hangup or an error wakes both directions: whoever is waiting needs
      // to find out from the syscall itself what went wrong.
      bool broken = (got & (EPOLLERR | EPOLLHUP)) != 0;
      if ((got & EPOLLIN) || broken) deliver(p, slot_of(fd, 0), SWORD_POLL_READY);
      if ((got & EPOLLOUT) || broken) deliver(p, slot_of(fd, 1), SWORD_POLL_READY);
#endif
    }
    expire(p);
    close_pending(p);
  }
  close_pending(p);
}

Poller *g_poller = nullptr;
pthread_once_t g_poller_once = PTHREAD_ONCE_INIT;

Poller &poller() {
  pthread_once(&g_poller_once, [] {
    Poller *p = (Poller *)calloc(1, sizeof(Poller));
    p->handle = -1;
    p->nudge[0] = -1;
    p->nudge[1] = -1;
    p->next_seq = 1;
    p->lock.init();
    p->closed.init();
    g_poller = p;
  });
  return *g_poller;
}

} // namespace

extern "C" {

pthread_once_t started = PTHREAD_ONCE_INIT;
std::atomic<sword_wake_fn> g_wake_init = nullptr;

void init_poller() {
  Poller &p = poller();
#ifdef SWORD_KQUEUE
  int handle = kqueue();
#else
  int handle = epoll_create1(0);
#endif
  {
    // A close decides under this lock whether the poller is running.
    Lock held(p.lock);
    p.wake = g_wake_init.load(std::memory_order_acquire);
    p.handle = handle;
  }
  if (p.handle < 0) {
    fputs("sword: cannot create the poller\n", stderr);
    abort();
  }
  if (pipe(p.nudge) != 0) {
    fputs("sword: cannot create the poller's pipe\n", stderr);
    abort();
  }
  // Non-blocking, so draining it never parks the poller itself.
  for (int end = 0; end < 2; end++) {
    int flags = fcntl(p.nudge[end], F_GETFL, 0);
    fcntl(p.nudge[end], F_SETFL, flags | O_NONBLOCK);
  }
  arm(p, p.nudge[0], 0, 0, false);
  pthread_create(&p.thread, nullptr, [](void *) -> void * {
    loop();
    return nullptr;
  }, nullptr);
  p.thread_started = true;
}

void sword_poll_start(sword_wake_fn wake) {
  g_wake_init.store(wake, std::memory_order_release);
  pthread_once(&started, init_poller);
}

void sword_poll_wait(int fd, int writable, void *token, int64_t deadline_ns,
                     uint64_t generation) {
  Poller &p = poller();
  size_t slot = slot_of(fd, writable);
  uint64_t seq;
  bool gone;
  {
    Lock held(p.lock);
    make_room(p, slot);
    gone = generation_of(fd) != generation;
    if (!gone) {
      seq = p.next_seq++;
      p.slots[slot].waiting.push_back(Waiter{token, seq});
      if (deadline_ns > 0)
        insert_timer(p, deadline_ns, (int)slot, seq);
    }
  }
  // A descriptor can be forgotten between the call that answered EAGAIN and
  // this registration — a listener closed by another task while its acceptor
  // was on the way here. The forget already told everyone registered, which was
  // nobody yet; the generation is the only word this waiter will get.
  if (gone) {
    p.wake(token, SWORD_POLL_FAILED);
    return;
  }
  if (!arm(p, fd, writable, generation, true)) {
    deliver(p, slot, SWORD_POLL_FAILED);
    return;
  }
  // A deadline nearer than the one the poller is already waiting on has to cut
  // that wait short, or it would be noticed late.
  if (deadline_ns > 0) nudge(p);
}

void sword_poll_sleep(void *token, int64_t deadline_ns) {
  Poller &p = poller();
  {
    Lock held(p.lock);
    uint64_t seq = p.next_seq++;
    p.sleepers.push_back(Sleeper{seq, token});
    insert_timer(p, deadline_ns, -1, seq);
  }
  nudge(p);
}

// Whoever is waiting has to be told, not dropped: a descriptor is forgotten
// when it is about to be closed, and a task still parked on it would never be
// woken by anything else. The generation moves first, and before the poller need
// be running, so that nobody can slip in between the telling and the close.
void sword_poll_forget(int fd) {
  Poller &p = poller();
  {
    // Under the lock, so a registration already under way finishes first.
    Lock held(p.lock);
    if (std::atomic<uint64_t> *g = gen_slot(fd, true))
      g->fetch_add(1, std::memory_order_acq_rel);
  }
  if (p.handle < 0) return;
  disarm(p, fd);
  deliver(p, slot_of(fd, 0), SWORD_POLL_FAILED);
  deliver(p, slot_of(fd, 1), SWORD_POLL_FAILED);
}

uint64_t sword_poll_generation(int fd) { return generation_of(fd); }

// A close and the move of the generation that follows it happen together under
// the lock, and the kernel can hand the number out again between the two. A
// socket made in that moment read the generation from before the move, and its
// first wait was taken for one on the closed descriptor: a dial said it could
// not connect over a connection that had been made. Taking the lock once waits
// out any such close, so what is read afterwards is the generation the new
// descriptor keeps. Nothing changes; the lock is only there to be passed.
void sword_poll_fresh(int fd) {
  (void)fd;
  Poller &p = poller();
  Lock held(p.lock);
}

// The caller waits for the poller to have done it: a socket that is still open
// after Close returned would take a datagram meant for nobody, or keep its port
// from being listened on again. The wait is short — the nudge ends the poller's
// kevent at once — and it holds the thread rather than parking the task, since
// parking goes through this same poller.
//
// Everywhere else — epoll, or a poller that is not running — the close happens
// here, under the lock all the same. Whether the poller is running is decided
// under it too: a task can start the poller and register between a look at it
// and the close.
void sword_poll_close(int fd) {
  Poller &p = poller();
  p.lock.lock();
#ifdef SWORD_KQUEUE
  if (p.handle >= 0 && !p.stopping.load(std::memory_order_acquire)) {
    p.closing.push_back(fd);
    uint64_t ticket = ++p.close_asked;
    p.lock.unlock();
    nudge(p);
    p.lock.lock();
    while (p.closed_upto < ticket) p.closed.wait(p.lock);
    p.lock.unlock();
    return;
  }
#endif
  Vec<Waiter> woken;
  close_locked(p, fd, woken);
  p.lock.unlock();
  for (const Waiter &w : woken) p.wake(w.token, SWORD_POLL_FAILED);
}

void sword_poll_stop(void) {
  Poller &p = poller();
  if (p.handle < 0) return;
  {
    // Under the lock, so a close either is queued before the poller's last
    // pass or sees it is stopping and closes in place.
    Lock held(p.lock);
    p.stopping.store(true, std::memory_order_release);
  }
  nudge(p);
  if (p.thread_started) {
    pthread_join(p.thread, nullptr);
    p.thread_started = false;
  }
}
}
