# Sword by example

Complete programs, one subject each, explained a piece at a time. Each one
compiles as it is shown, and the output under it is what it prints: both are
checked on every change to the language. Read [the tour](../tour.md) first; these
pages use what it teaches without explaining it again, and link to it where
they lean on something.

## Algorithms and data structures

- [Merge sort](merge-sort.md): generic, with its scratch space handed in, then
  the halves sorted by two tasks.
- [A hash map from scratch](hash-map.md): open addressing, growing, every byte
  from an allocator.
- [Shortest paths](shortest-path.md): a binary heap by hand, and Dijkstra's
  algorithm over it.
- [A ring buffer](ring-buffer.md): fixed capacity, storage inside the value,
  nothing allocated.
- [Counting words](word-count.md): standard input into a map, sorted by count.

## Low level

- [Your own allocator](allocator.md): a bump allocator over a buffer, and one that
  wraps another and counts.
- [A bytecode machine](bytecode-vm.md): instructions as an enum, a `switch` to run
  them, a table of function values.
- [Reading a binary format](wav-header.md): a WAV header, little-endian integers,
  an error for each malformed field.
- [A hexdump](hexdump.md): offsets, hex and text in columns, written into a
  `fmt.Sink`.
- [Talking to C](talking-to-c.md): `qsort` with a comparison written in Sword, and a
  function from libm.
- [Bit tricks](bit-tricks.md): a bitset, counting set bits, flags packed into an
  integer.
- [Decoding UTF-8](decoding-utf8.md): by hand, with every way bytes can fail to be
  UTF-8.

## Concurrency

- [A parallel sum](parallel-sum.md): tasks over a partition by hand, then a
  `parallel for` whose answer does not depend on the threads.
- [A worker pool](worker-pool.md): a pipeline of channels, and where the second
  channel gets closed.
- [Waiting with a deadline](select-deadline.md): `select` on an answer and a
  timer, and why nothing is cancelled.
- [Counters: atomic and shared](counters.md): the same totals kept both ways,
  and what each can promise.

## Programs

- [A command-line tool](grep.md): a small grep, with flags and the exit
  statuses scripts expect.
- [An HTTP JSON API](json-api.md): two routes, an arena per request, and the
  program as its own client.
- [Fetching JSON](http-client.md): a document over HTTPS, read with `std/json`.
- [A chat server](chat-server.md): a task per connection and one shared room.
