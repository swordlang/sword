#include "sword_net.h"
#include "sword_os.h"
#include "sword_rt.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace {

// For the calls that still stop the thread outright — name resolution is the
// one that matters. The scheduler covers the thread while it is gone.
struct Parked {
  Parked() { sword_blocking_enter(); }
  ~Parked() { sword_blocking_exit(); }
};

struct Resolve {
  const char *name;
  const char *service;
  const addrinfo *hints;
  addrinfo **found;
};

int64_t do_resolve(void *p) {
  Resolve *r = (Resolve *)p;
  return getaddrinfo(r->name, r->service, r->hints, r->found);
}

// Every socket is non-blocking underneath; waiting is the runtime's job rather
// than the kernel's. Inside a task that means putting the task down and letting
// the worker go elsewhere; outside one — the main thread before any scope —
// there is nothing to put down, so the thread waits on poll() instead.
void unblock(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// Two limits per descriptor, both in monotonic nanoseconds and both optional.
// The timeout bounds one wait; the deadline bounds the whole exchange, which is
// what keeps a client that sends a byte a second from living forever. The
// kernel's own SO_RCVTIMEO cannot do either, because the waiting is the
// runtime's now and not the kernel's.
struct Limits {
  int64_t timeout = 0;  // per wait, relative
  int64_t deadline = 0; // absolute, for everything on this socket
};

Vec<Limits> limits;
Mutex limits_lock;

Limits limits_of(int fd) {
  Lock held(limits_lock);
  if (fd < 0 || (size_t)fd >= limits.size()) return Limits{};
  return limits[fd];
}

Limits &limits_for(int fd) {
  if ((size_t)fd >= limits.size()) limits.resize((size_t)fd + 64);
  return limits[fd];
}

void forget_limits(int fd) {
  Lock held(limits_lock);
  if (fd >= 0 && (size_t)fd < limits.size()) limits[fd] = Limits{};
}

// A socket the kernel has just handed out, under a number that may have been
// another socket's a moment ago: nothing of that one's may carry over, neither
// its limits nor its generation.
void adopt(int fd) {
  unblock(fd);
  forget_limits(fd);
  sword_fresh_fd(fd);
}

// Whichever runs out first.
int64_t due_at(int fd) {
  Limits l = limits_of(fd);
  int64_t from_timeout = l.timeout > 0 ? sword_time_mono() + l.timeout : 0;
  if (l.deadline == 0) return from_timeout;
  if (from_timeout == 0) return l.deadline;
  return from_timeout < l.deadline ? from_timeout : l.deadline;
}

// Waits for one direction of a descriptor. -2 is the deadline, -1 a failure.
// `generation` is the descriptor's, read before the call that answered "not
// yet": a close in between fails the wait rather than leaving it on a number
// that may name something else by now.
int await(int fd, bool writable, uint64_t generation) {
  int64_t deadline = due_at(fd);
  if (sword_in_task()) {
    int got = sword_park_fd(fd, writable ? 1 : 0, deadline, generation);
    // -1 only comes back when there was no task to put down, which cannot
    // happen here; anything else is the poller's answer.
    return got;
  }

  // No task to put down: wait on the thread, and tell the scheduler so it can
  // cover for it.
  Parked parked;
  int wait = -1;
  if (deadline > 0) {
    int64_t left = deadline - sword_time_mono();
    if (left <= 0) return -2;
    wait = (int)((left + 999999) / 1000000);
  }
  pollfd watch;
  watch.fd = fd;
  watch.events = writable ? POLLOUT : POLLIN;
  watch.revents = 0;
  int ready = poll(&watch, 1, wait);
  if (ready == 0) return -2;
  if (ready < 0) return sword_errno() == EINTR ? 0 : -1;
  return 0;
}

} // namespace

extern "C" {

// `host` is the address to bind, empty meaning every interface. Loopback used to
// be hard-coded here, which meant nothing could be served off the machine.
//
// `reuse_port` lets several sockets — several processes, usually — hold the
// same port and have the kernel spread connections between them. It is how a
// restart happens without dropping anything.
int32_t sword_net_listen_on(const char *host, int64_t host_len, int32_t port,
                            int32_t backlog, int32_t reuse_port) {
  if (sword_sim_on()) sword_sim_refuse("uses the network");
  sword_os_ignore_sigpipe();
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;

  int on = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
#ifdef SO_REUSEPORT
  if (reuse_port) setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on));
#endif

  sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons((uint16_t)port);
  if (host_len <= 0) {
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
  } else {
    char name[64];
    if (host_len >= (int64_t)sizeof(name)) {
      close(fd);
      return -1;
    }
    memcpy(name, host, (size_t)host_len);
    name[host_len] = '\0';
    if (inet_pton(AF_INET, name, &addr.sin_addr) != 1) {
      close(fd);
      return -1;
    }
  }

  if (bind(fd, (sockaddr *)&addr, sizeof(addr)) < 0 ||
      listen(fd, backlog) < 0) {
    close(fd);
    return -1;
  }
  adopt(fd);
  return fd;
}

int32_t sword_net_listen(int32_t port, int32_t backlog) {
  const char *loopback = "127.0.0.1";
  return sword_net_listen_on(loopback, 9, port, backlog, 0);
}

// Who is on the other end, written into `out` as dotted quad. Returns the port,
// or -1. A log or a rate limiter needs this and had no way to ask.
int32_t sword_net_peer(int32_t fd, char *out, int64_t out_len) {
  sockaddr_in addr;
  socklen_t len = sizeof(addr);
  if (getpeername(fd, (sockaddr *)&addr, &len) < 0) return -1;
  if (!inet_ntop(AF_INET, &addr.sin_addr, out, (socklen_t)out_len)) return -1;
  return ntohs(addr.sin_port);
}

// Needed because a test can ask for port 0 and then has to find out which one
// it actually got.
int32_t sword_net_port(int32_t fd) {
  sockaddr_in addr;
  socklen_t len = sizeof(addr);
  if (getsockname(fd, (sockaddr *)&addr, &len) < 0) return -1;
  return ntohs(addr.sin_port);
}

int32_t sword_net_accept(int32_t fd) {
  while (true) {
    uint64_t generation = sword_fd_generation(fd);
    int client = accept(fd, nullptr, nullptr);
    if (client >= 0) {
      int on = 1;
      setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
      adopt(client);
      return client;
    }
    int why = sword_errno();
    if (why == EINTR) continue; // a signal, not a failure
    if (why != EAGAIN && why != EWOULDBLOCK) return -1;
    // Nothing waiting. Put the task down rather than the thread.
    int ready = await(fd, false, generation);
    if (ready == -2) return -2;
    if (ready < 0) return -1;
  }
}

// Connect with a deadline. Going through a non-blocking connect and poll is
// the only way to bound it: the kernel's own connect timeout is over a minute
// and cannot be shortened per socket.
//
// 0 connected, -2 out of time, -1 refused or unreachable. The answer is the
// return value rather than errno, since a task that waited here may be reading
// errno on another thread by the time it asks.
static int connect_within(int fd, const sockaddr *addr, socklen_t len,
                          int64_t millis) {
  adopt(fd);
  if (millis > 0) {
    Lock held(limits_lock);
    limits_for(fd).timeout = millis * 1000000;
  }

  uint64_t generation = sword_fd_generation(fd);
  int result = connect(fd, addr, len);
  if (result == 0) return 0;
  if (sword_errno() != EINPROGRESS) return -1;

  int ready = await(fd, true, generation);
  if (ready == -2) return -2;
  if (ready < 0) return -1;

  int failure = 0;
  socklen_t size = sizeof(failure);
  if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &failure, &size) < 0) return -1;
  return failure == 0 ? 0 : -1;
}

// Zero waits as long as the kernel would. -2 separates "took too long" from
// "could not connect at all", the same way a read does.
int32_t sword_net_dial_timeout(const char *host, int64_t host_len, int32_t port,
                               int64_t millis) {
  if (sword_sim_on()) sword_sim_refuse("uses the network");
  char name[256];
  if (host_len <= 0 || host_len >= (int64_t)sizeof(name)) return -1;
  memcpy(name, host, (size_t)host_len);
  name[host_len] = '\0';

  addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;

  char service[16];
  snprintf(service, sizeof(service), "%d", port);

  sword_os_ignore_sigpipe();
  addrinfo *found = nullptr;
  Resolve call{name, service, &hints, &found};
  // Name resolution is one library call with no way into the middle of it, and
  // often the longest part of a dial. It goes to the pool that file I/O uses:
  // the task is put down, and the worker is free meanwhile. Bracketing the whole
  // dial in blocking hints was worse than slow — the hint is counted per thread,
  // and a task that came back on another one never gave it back.
  if (sword_offload(do_resolve, &call) != 0) return -1;

  int fd = -1;
  bool timed_out = false;
  for (addrinfo *at = found; at; at = at->ai_next) {
    fd = socket(at->ai_family, at->ai_socktype, at->ai_protocol);
    if (fd < 0) continue;
    int connected = connect_within(fd, at->ai_addr, at->ai_addrlen, millis);
    if (connected == 0) break;
    timed_out = connected == -2;
    // The wait put this socket on the poller, and a refused connect is a socket
    // already shut down whose event may fire as it is attached — the very case
    // in which a close beside kqueue hangs in the kernel for good. So it goes the
    // way every other socket goes, and its generation moves on with it.
    forget_limits(fd);
    sword_close_fd(fd);
    fd = -1;
  }
  freeaddrinfo(found);

  if (fd < 0) return timed_out ? -2 : -1;
  int on = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
  return fd;
}

int32_t sword_net_dial(const char *host, int64_t host_len, int32_t port) {
  return sword_net_dial_timeout(host, host_len, port, 0);
}

int32_t sword_net_timeout(int32_t fd, int64_t millis) {
  Lock held(limits_lock);
  limits_for(fd).timeout = millis > 0 ? millis * 1000000 : 0;
  return 0;
}

// An absolute point on the monotonic clock, after which nothing on this socket
// waits any longer. Zero clears it.
int32_t sword_net_deadline(int32_t fd, int64_t at_ns) {
  Lock held(limits_lock);
  limits_for(fd).deadline = at_ns;
  return 0;
}

// -2 rather than -1 for a timeout, so a caller can tell "nothing arrived in
// time" from "this connection is broken".
int64_t sword_net_read(int32_t fd, void *buf, int64_t len) {
  while (true) {
    uint64_t generation = sword_fd_generation(fd);
    ssize_t n = read(fd, buf, (size_t)len);
    if (n >= 0) return n;
    int why = sword_errno();
    if (why == EINTR) continue;
    if (why != EAGAIN && why != EWOULDBLOCK) return -1;
    int ready = await(fd, false, generation);
    if (ready == -2) return -2;
    if (ready < 0) return -1;
  }
}

int64_t sword_net_write(int32_t fd, const void *buf, int64_t len) {
  // Short writes are normal on a socket; the caller wants all or nothing.
  int64_t sent = 0;
  while (sent < len) {
    uint64_t generation = sword_fd_generation(fd);
    ssize_t n = write(fd, (const char *)buf + sent, (size_t)(len - sent));
    if (n < 0) {
      int why = sword_errno();
      if (why == EINTR) continue;
      if (why != EAGAIN && why != EWOULDBLOCK) return -1;
      int ready = await(fd, true, generation);
      if (ready == -2) return -2;
      if (ready < 0) return -1;
      continue;
    }
    if (n == 0) return -1;
    sent += n;
  }
  return sent;
}

int32_t sword_net_await(int32_t fd, int32_t writable, uint64_t generation) {
  return (int32_t)await((int)fd, writable != 0, generation);
}

int32_t sword_net_close(int32_t fd) {
  forget_limits(fd);
  return sword_close_fd(fd);
}

// A plain close does not wake a thread sitting in accept() on the same
// descriptor; shutting the socket down first does.
int32_t sword_net_stop(int32_t fd) {
  shutdown(fd, SHUT_RDWR);
  forget_limits(fd);
  return sword_close_fd(fd);
}
}

// --- UDP ---------------------------------------------------------------------
//
// An address crosses into Sword as twenty bytes rather than as a sockaddr, whose
// layout differs between systems and between the two families: the family (4 or
// 6), sixteen bytes of address (an IPv4 one in the first four), the port in
// network order, and one byte that says a datagram did not fit.

namespace {

enum { kAddrBytes = 20, kTruncatedAt = 19 };

void put_addr(const sockaddr_storage &from, uint8_t *out) {
  memset(out, 0, kAddrBytes);
  if (from.ss_family == AF_INET) {
    const sockaddr_in *v4 = (const sockaddr_in *)&from;
    out[0] = 4;
    memcpy(out + 1, &v4->sin_addr, 4);
    memcpy(out + 17, &v4->sin_port, 2);
    return;
  }
  const sockaddr_in6 *v6 = (const sockaddr_in6 *)&from;
  // A dual-stack socket reports an IPv4 peer as ::ffff:a.b.c.d. It is an IPv4
  // peer, and it is written back as one.
  if (IN6_IS_ADDR_V4MAPPED(&v6->sin6_addr)) {
    out[0] = 4;
    memcpy(out + 1, (const uint8_t *)&v6->sin6_addr + 12, 4);
  } else {
    out[0] = 6;
    memcpy(out + 1, &v6->sin6_addr, 16);
  }
  memcpy(out + 17, &v6->sin6_port, 2);
}

// The sockaddr for `addr`, shaped for a socket of family `family`: an IPv4
// address sent from a dual-stack socket goes as a mapped one.
socklen_t get_addr(const uint8_t *addr, int family, sockaddr_storage &to) {
  memset(&to, 0, sizeof(to));
  if (family == AF_INET) {
    if (addr[0] != 4) return 0;
    sockaddr_in *v4 = (sockaddr_in *)&to;
    v4->sin_family = AF_INET;
    memcpy(&v4->sin_addr, addr + 1, 4);
    memcpy(&v4->sin_port, addr + 17, 2);
    return sizeof(sockaddr_in);
  }
  sockaddr_in6 *v6 = (sockaddr_in6 *)&to;
  v6->sin6_family = AF_INET6;
  if (addr[0] == 4) {
    uint8_t *raw = (uint8_t *)&v6->sin6_addr;
    raw[10] = 0xff;
    raw[11] = 0xff;
    memcpy(raw + 12, addr + 1, 4);
  } else {
    memcpy(&v6->sin6_addr, addr + 1, 16);
  }
  memcpy(&v6->sin6_port, addr + 17, 2);
  return sizeof(sockaddr_in6);
}

int family_of(int fd) {
  sockaddr_storage self;
  socklen_t len = sizeof(self);
  if (getsockname(fd, (sockaddr *)&self, &len) < 0) return -1;
  return self.ss_family;
}

// A C string of a Sword one, or false when it does not fit.
bool terminated(const char *host, int64_t host_len, char *into, size_t room) {
  if (host_len < 0 || (size_t)host_len >= room) return false;
  memcpy(into, host, (size_t)host_len);
  into[host_len] = '\0';
  return true;
}

// Every address this name has, of either family, through the io pool.
addrinfo *resolve_dgram(const char *name, int32_t port) {
  addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_DGRAM;
  // A name asks only for the families this machine has an address in, so an
  // IPv4-only host is not handed IPv6 results it cannot use. A numeric address
  // is taken as written: glibc applies that filter to "::1" as well, and on a
  // machine whose only IPv6 is loopback the answer was nothing at all.
  uint8_t probe[16];
  bool numeric = inet_pton(AF_INET, name, probe) == 1 ||
                 inet_pton(AF_INET6, name, probe) == 1;
  hints.ai_flags = numeric ? AI_NUMERICHOST : AI_ADDRCONFIG;
  char service[16];
  snprintf(service, sizeof(service), "%d", port);
  addrinfo *found = nullptr;
  Resolve call{name, service, &hints, &found};
  if (sword_offload(do_resolve, &call) != 0) return nullptr;
  return found;
}

int open_dgram(int family) {
  int fd = socket(family, SOCK_DGRAM, 0);
  if (fd < 0) return -1;
  adopt(fd);
  return fd;
}

} // namespace

extern "C" {

// Loopback when `host` is empty and `local` is set; every interface, IPv4 and
// IPv6 at once, when it is empty and `local` is not; otherwise the numeric
// address given, of either family.
int32_t sword_udp_listen(const char *host, int64_t host_len, int32_t port,
                         int32_t local) {
  if (sword_sim_on()) sword_sim_refuse("uses the network");
  sword_os_ignore_sigpipe();
  char name[64];
  if (!terminated(host, host_len, name, sizeof(name))) return -1;

  sockaddr_storage at;
  memset(&at, 0, sizeof(at));
  socklen_t len = 0;
  int family = AF_INET;
  bool dual = false;
  sockaddr_in *v4 = (sockaddr_in *)&at;
  sockaddr_in6 *v6 = (sockaddr_in6 *)&at;
  if (host_len == 0 && local) {
    v4->sin_family = AF_INET;
    v4->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    len = sizeof(sockaddr_in);
  } else if (host_len == 0) {
    family = AF_INET6;
    dual = true;
    v6->sin6_family = AF_INET6;
    v6->sin6_addr = in6addr_any;
    len = sizeof(sockaddr_in6);
  } else if (inet_pton(AF_INET, name, &v4->sin_addr) == 1) {
    v4->sin_family = AF_INET;
    len = sizeof(sockaddr_in);
  } else if (inet_pton(AF_INET6, name, &v6->sin6_addr) == 1) {
    family = AF_INET6;
    v6->sin6_family = AF_INET6;
    dual = IN6_IS_ADDR_UNSPECIFIED(&v6->sin6_addr);
    len = sizeof(sockaddr_in6);
  } else {
    return -1;
  }

  int fd = open_dgram(family);
  // A machine without IPv6 still has every IPv4 interface.
  if (fd < 0 && dual) {
    memset(&at, 0, sizeof(at));
    v4->sin_family = AF_INET;
    v4->sin_addr.s_addr = htonl(INADDR_ANY);
    len = sizeof(sockaddr_in);
    family = AF_INET;
    dual = false;
    fd = open_dgram(family);
  }
  if (fd < 0) return -1;
  if (family == AF_INET) v4->sin_port = htons((uint16_t)port);
  else v6->sin6_port = htons((uint16_t)port);

  int on = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
  if (family == AF_INET6) {
    int only = dual ? 0 : 1;
    setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &only, sizeof(only));
  }
  if (bind(fd, (sockaddr *)&at, len) < 0) {
    sword_net_close(fd);
    return -1;
  }
  return fd;
}

// A socket that sends to one peer and hears only from it. Connecting a UDP
// socket sends nothing; it is what lets a read report that nobody is listening
// at the other end, which an unconnected one never learns.
int32_t sword_udp_dial(const char *host, int64_t host_len, int32_t port) {
  if (sword_sim_on()) sword_sim_refuse("uses the network");
  sword_os_ignore_sigpipe();
  char name[256];
  if (host_len <= 0 || !terminated(host, host_len, name, sizeof(name))) return -1;
  addrinfo *found = resolve_dgram(name, port);
  if (!found) return -1;
  int fd = -1;
  for (addrinfo *at = found; at; at = at->ai_next) {
    fd = open_dgram(at->ai_family);
    if (fd < 0) continue;
    if (connect(fd, at->ai_addr, at->ai_addrlen) == 0) break;
    sword_net_close(fd);
    fd = -1;
  }
  freeaddrinfo(found);
  return fd;
}

// The first address a name has, into `out`. Numeric addresses are not looked
// up anywhere.
int32_t sword_udp_resolve(const char *host, int64_t host_len, int32_t port,
                          uint8_t *out) {
  char name[256];
  if (host_len <= 0 || !terminated(host, host_len, name, sizeof(name))) return -1;
  addrinfo *found = resolve_dgram(name, port);
  if (!found) return -1;
  sockaddr_storage copy;
  memset(&copy, 0, sizeof(copy));
  memcpy(&copy, found->ai_addr, found->ai_addrlen);
  put_addr(copy, out);
  freeaddrinfo(found);
  return 0;
}

// A numeric address, with no lookup: 0, or -1 when it is not one.
int32_t sword_udp_parse(const char *host, int64_t host_len, int32_t port,
                        uint8_t *out) {
  char name[64];
  if (host_len <= 0 || !terminated(host, host_len, name, sizeof(name))) return -1;
  memset(out, 0, kAddrBytes);
  uint16_t net_port = htons((uint16_t)port);
  if (inet_pton(AF_INET, name, out + 1) == 1) {
    out[0] = 4;
  } else if (inet_pton(AF_INET6, name, out + 1) == 1) {
    out[0] = 6;
  } else {
    return -1;
  }
  memcpy(out + 17, &net_port, 2);
  return 0;
}

// The address as text, into `out`; its length, or -1.
int32_t sword_udp_text(const uint8_t *addr, char *out, int64_t out_len) {
  int family = addr[0] == 6 ? AF_INET6 : AF_INET;
  if (!inet_ntop(family, addr + 1, out, (socklen_t)out_len)) return -1;
  return (int32_t)strlen(out);
}

// One datagram into `buf`, its sender into `from`. What landed is returned, and
// `from` says when that was not all of it.
// -2 is the deadline, -3 nobody listening (a connected socket only), -1 the
// rest.
int64_t sword_udp_recv(int32_t fd, void *buf, int64_t len, uint8_t *from) {
  while (true) {
    sockaddr_storage peer;
    iovec part;
    part.iov_base = buf;
    part.iov_len = (size_t)len;
    msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_name = &peer;
    msg.msg_namelen = sizeof(peer);
    msg.msg_iov = &part;
    msg.msg_iovlen = 1;
    uint64_t generation = sword_fd_generation(fd);
    ssize_t n = recvmsg(fd, &msg, 0);
    if (n >= 0) {
      if (msg.msg_namelen > 0) put_addr(peer, from);
      else memset(from, 0, kAddrBytes);
      from[kTruncatedAt] = (msg.msg_flags & MSG_TRUNC) ? 1 : 0;
      return n;
    }
    int why = sword_errno();
    if (why == EINTR) continue;
    if (why == ECONNREFUSED) return -3;
    if (why != EAGAIN && why != EWOULDBLOCK) return -1;
    int ready = await(fd, false, generation);
    if (ready == -2) return -2;
    if (ready < 0) return -1;
  }
}

// One datagram, to `to`, or to the connected peer when `to` is null. A
// datagram goes whole or not at all. -3 when an earlier send turned out to
// have nobody at the other end.
int64_t sword_udp_send(int32_t fd, const void *buf, int64_t len,
                       const uint8_t *to) {
  sockaddr_storage where;
  socklen_t where_len = 0;
  if (to) {
    where_len = get_addr(to, family_of(fd), where);
    if (where_len == 0) return -1;
  }
  while (true) {
    uint64_t generation = sword_fd_generation(fd);
    ssize_t n = to ? sendto(fd, buf, (size_t)len, 0, (sockaddr *)&where, where_len)
                   : send(fd, buf, (size_t)len, 0);
    if (n >= 0) return n;
    int why = sword_errno();
    if (why == EINTR) continue;
    if (why == ECONNREFUSED) return -3;
    if (why != EAGAIN && why != EWOULDBLOCK && why != ENOBUFS) return -1;
    int ready = await(fd, true, generation);
    if (ready == -2) return -2;
    if (ready < 0) return -1;
  }
}

int32_t sword_udp_broadcast(int32_t fd, int32_t on) {
  int value = on ? 1 : 0;
  return setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &value, sizeof(value));
}

// The address this socket is bound to: which port port 0 turned into.
int32_t sword_udp_local(int32_t fd, uint8_t *out) {
  sockaddr_storage self;
  socklen_t len = sizeof(self);
  if (getsockname(fd, (sockaddr *)&self, &len) < 0) return -1;
  put_addr(self, out);
  return 0;
}
}
