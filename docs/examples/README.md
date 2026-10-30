# Sword by example

Complete programs, one subject each, explained a piece at a time. Each one
compiles as it is shown, and the output under it is what it prints: both are
checked on every change to the language. Read [the tour](../tour.md) first; these
pages use what it teaches without explaining it again, and link to it where
they lean on something.

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
