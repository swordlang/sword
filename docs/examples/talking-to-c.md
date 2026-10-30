# Talking to C

Planets sorted by C's `qsort`, with the comparison written in Sword, and a cube
root from the system's maths library.

Uses: [talking to C](../tour.md#talking-to-c), [structs](../tour.md#structs),
[C functions as values](../reference.md#c-functions-as-values),
[C libraries](../reference.md#c-libraries), [generics](../reference.md#generics).

```sword
import "std/io"

// From the C library every program already links.
extern func qsort(base [*]u8, n u64, size u64,
                  compare extern func([*]u8, [*]u8) i32)

// From libm, named the way -lm would name it.
extern "m" func cbrt(x f64) f64

struct Planet {
    name     string
    distance f64    // from the Sun, in millions of km
}

// qsort hands over two addresses and wants a negative, zero or positive
// answer; the Sword side turns the addresses back into planets.
func nearer(a [*]u8, b [*]u8) i32 {
    x := [*]Planet(a)[0..1][0].distance
    y := [*]Planet(b)[0..1][0].distance
    if x < y {
        return -1
    }
    if x > y {
        return 1
    }
    return 0
}

func main() !void {
    mut planets := [5]Planet{
        Planet{name: "Jupiter", distance: 778.5},
        Planet{name: "Mercury", distance: 57.9},
        Planet{name: "Mars", distance: 227.9},
        Planet{name: "Earth", distance: 149.6},
        Planet{name: "Venus", distance: 108.2},
    }
    qsort([*]u8(planets[..].ptr), planets.len, sizeof[Planet](), nearer)

    for p in planets {
        try io.Printf("{} {.1}\n", p.name, p.distance)
    }

    // Kepler: the square of a year grows as the cube of the distance. Jupiter's
    // year is 11.86 of ours, so it is the cube root of 11.86 squared AU out.
    year := 11.86
    try io.Printf("Jupiter by its year: {.2} AU\n", cbrt(year * year))
}
```
<!-- prints:
Mercury 57.9
Venus 108.2
Earth 149.6
Mars 227.9
Jupiter 778.5
Jupiter by its year: 5.20 AU
-->

## Declaring what C has

```sword
extern func qsort(base [*]u8, n u64, size u64,
                  compare extern func([*]u8, [*]u8) i32)
```

An `extern func` is a C function, declared by its own name with Sword types in
place of C's. There is no header to read and no binding to generate: the
declaration is the whole contract, and if it disagrees with C, the call is
wrong the way it would be in C.

`[*]u8` is a raw pointer, an address with no length, which is what C's `void *`
is. It is the only kind of pointer that crosses: Sword's `*T` and `[]T` carry
promises (never null, a length) that C would not keep.

`qsort` comes from the C library every program already links, so nothing more
is needed. `cbrt` lives in libm, and `extern "m"` says so, the way `-lm` would
on a C compiler's command line. The library is linked because the source asks
for it; whoever builds the program writes nothing.

## A comparison C can call

```sword
func nearer(a [*]u8, b [*]u8) i32 {
    x := [*]Planet(a)[0..1][0].distance
```

`nearer` is an ordinary Sword function, and passing it where `qsort` wants an
`extern func` converts it. That works because its signature has only scalars in
it, and with only scalars Sword calls the way C does. The conversion never goes
back: an address C hands over cannot become a Sword `func`, because nothing
could say which Sword function it is.

C hands the comparison two addresses into the array. `[*]Planet(a)` says what
is there, and `[0..1]` makes a slice of one planet from it, which is the
crossing back into checked memory; `[0]` is that planet. Nothing checks that
the address really holds a `Planet`. That is the promise a raw pointer makes.

## Handing over the array

```sword
qsort([*]u8(planets[..].ptr), planets.len, sizeof[Planet](), nearer)
```

`planets[..]` is the whole array as a slice, `.ptr` its address as a
`[*]Planet`, and `[*]u8(...)` the same address as bytes, which is what the
declaration asked for. `sizeof[Planet]()` is the size of one element, 24
bytes: a `string` is two words and an `f64` is one.

`qsort` writes into the array, so `planets` is `mut`. The compiler does not
check this one: once the address is a raw pointer it is out of the compiler's
sight, and C would write into an immutable array just the same. The `mut` is
there because it is true, and because Sword code after the call is entitled to
believe an array without it never changed.

## The answer

Kepler's third law says the square of a planet's year grows as the cube of its
distance from the Sun, so a year of 11.86 Earth years puts a planet at the cube
root of 11.86², 5.20 AU, and that is Jupiter. In the sorted table it is 778.5
million km, and 778.5 / 149.6, Earth's distance, is 5.20.
