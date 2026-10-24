#pragma once

#include <stdbool.h>
#include <stdint.h>

// The ABI between generated code and the scheduler. Every task is a thunk the
// compiler writes: it unpacks a copied argument block and returns an error
// code, where zero means success.
extern "C" {

typedef uint16_t (*sword_task_fn)(void *args);
// The index says which piece of the range this is, counting from zero. A
// reduction keeps its partial answer there, so that combining them is a walk
// in order rather than whatever order the workers happened to finish in.
typedef uint16_t (*sword_chunk_fn)(void *env, int64_t lo, int64_t hi,
                                   int64_t index);

// Opaque to generated code, which only ever reserves this much word-aligned
// space on its own frame and passes the address along. The alignment matters:
// there are atomics in here.
enum { SWORD_SCOPE_SIZE = 64 };

void sword_scope_begin(void *scope);
void sword_scope_spawn(void *scope, sword_task_fn fn, const void *args,
                       int64_t size);
uint16_t sword_scope_end(void *scope);

// `main` runs as a task too, so that waiting on a descriptor there costs a
// stack rather than the thread. The entry point the compiler writes calls this
// with a thunk around the program's own main.
uint16_t sword_run_main(sword_task_fn fn, const void *args, int64_t size);

// How many pieces a range is cut into, at most. Fixed rather than one per
// worker on purpose: what a reduction combines has to be the same list in the
// same order whether the program runs on one thread or on sixteen. Enough
// pieces that stealing can even out a body whose cost varies.
//
// Must equal kChunks in src/lower.cpp, which reserves room for one partial
// answer per piece on the caller's frame.
enum { SWORD_CHUNKS = 64 };

// Answers the first error any piece returned, and writes how many pieces there
// were — which is how many partial answers the caller has to combine.
// `most` is the caller's own cap on the number of pieces: it reserved room for
// one partial answer each, and how much room that is depends on the type.
uint16_t sword_parallel_for(int64_t lo, int64_t hi, sword_chunk_fn fn,
                            void *env, int64_t *pieces, int64_t most);

// Brackets a call that parks the thread in the kernel. A parked thread is not
// scheduler capacity, so the pool hires a replacement for as long as it is
// gone: blocking I/O then costs a thread instead of a core. Cheap and safe to
// call when no pool is running, which is the common case for a program that
// never spawns anything.
void sword_blocking_enter(void);
void sword_blocking_exit(void);

// The guard inside a `shared[T]`, which generated code only ever passes the
// address of. All-zero bytes are an unlocked guard, so a shared needs no
// constructor and can sit in an array or a struct field.
// Must equal SWORD_GUARD_SIZE in src/types.h, which is what decides how much
// room the generated code actually leaves.
enum { SWORD_GUARD_SIZE = 32 };

void sword_mutex_lock(void *guard);
void sword_mutex_unlock(void *guard);

// Any number of readers, or one writer. Wait and Notify need the writer's.
void sword_mutex_lock_read(void *guard);
void sword_mutex_unlock_read(void *guard);

// The guard if it is free now; false, without waiting, when it is not.
bool sword_mutex_try_lock(void *guard);
bool sword_mutex_try_lock_read(void *guard);

// A condition to wait on, which is what turns a mutex into something you can
// build a queue out of. All three must be called with the guard held.
// `wait` releases it, puts the task down, and takes it back on the way out.
void sword_mutex_wait(void *guard);
void sword_mutex_notify(void *guard);
void sword_mutex_notify_all(void *guard);

// Waits for a descriptor without holding the thread. Only a task can be put
// down, so this answers -1 when there is none and the caller waits the old way.
// 0 means ready, -2 means the deadline passed.
int sword_park_fd(int32_t fd, int32_t writable, int64_t deadline_ns,
                  uint64_t generation);
int32_t sword_in_task(void);

// The simulator (SWORD_SIM_SEED): whether it is on, its monotonic clock, the
// random bytes it gives in place of the kernel's, a point where it may switch
// tasks, and the end of a run that does something it does not simulate yet.
int32_t sword_sim_on(void);
int64_t sword_sim_mono(void);
uint64_t sword_sim_entropy(void);
void sword_sim_point(void);
void sword_sim_refuse(const char *what);
// errno, read on the thread the caller is on now. Code that may have parked
// reads it through this rather than directly — see sword_rt.cpp.
int sword_errno(void);
// And written, for an `extern blocking` call: the C function ran on an io
// thread, and its errno is handed back to the thread the task is on now.
void sword_set_errno(int value);
// The same for the clock: waits without holding the thread, and answers -1 when
// there is no task to put down.
int sword_park_timer(int64_t deadline_ns);
// Drops anything waiting on a descriptor about to be closed.
void sword_forget_fd(int32_t fd);
// For a socket just made or accepted, before its generation is first read.
void sword_fresh_fd(int32_t fd);
// Read before a call that may answer "not yet", and handed to the wait.
uint64_t sword_fd_generation(int32_t fd);
// Forgets a socket and closes it; how the close happens is the poller's call.
int32_t sword_close_fd(int32_t fd);

// Runs one call on a thread kept for calls that cannot be put down — file I/O,
// name resolution — and puts the calling task down meanwhile. Outside a task it
// runs the call here, behind the blocking hints.
int64_t sword_offload(int64_t (*fn)(void *), void *arg);

// What the scheduler is doing right now. Laid out to match `runtime.Stats` on
// the Sword side field for field, which is why that one is an extern struct.
struct sword_stats {
  int64_t threads;  // workers, including any hired to cover a parked one
  int64_t queued;   // spawned and not yet picked up
  int64_t parked;   // threads stopped in the kernel
  int64_t stacks;   // task stacks alive, in use or pooled
  int64_t started;  // tasks begun since the program did
  int64_t finished; // and tasks ended
  int64_t stack_bytes; // how much stack each task gets
  int64_t io_threads;  // threads set aside for calls that cannot be put down
};

void sword_runtime_stats(struct sword_stats *out);

// Mutex and condition, on a blob of SWORD_GUARD_SIZE bytes the caller owns.
void sword_mutex_lock(void *blob);
void sword_mutex_unlock(void *blob);
void sword_mutex_wait(void *blob);
void sword_mutex_notify(void *blob);
void sword_mutex_notify_all(void *blob);
// Waiting on several guards at once, in three steps. The caller provides
// `sword_mutex_watch_bytes()` bytes per guard and keeps them alive across all
// three. `watch` registers *before* the caller checks what it is waiting for,
// which is what keeps a change that lands in between from being lost; `park` puts
// the task down, and returns at once if anything notified since `watch`.
int64_t sword_mutex_watch_bytes(void);
void sword_mutex_watch(void **blobs, int64_t n, void *nodes);
void sword_mutex_park(void);
void sword_mutex_unwatch(void **blobs, int64_t n, void *nodes);
// Wakes everything watching this guard except the caller, for a change that no
// `wait` is looking for but a `select` on another task might be. Held, like the
// rest of them.
void sword_mutex_notify_watchers(void *blob);
}
