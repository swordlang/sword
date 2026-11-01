# A bytecode machine

A stack machine that runs a program written as bytes: an enum for the
instructions, a `switch` to dispatch them, and a table of function values for
the arithmetic.

Uses: [enums](../tour.md#enums), [errors](../tour.md#errors),
[function values](../tour.md#function-values), [methods](../tour.md#methods-and-interfaces),
[`switch`](../reference.md#switch).

```sword
import "std/io"

enum Op u8 {
    Push        // the byte after it is the value
    Add
    Sub
    Mul
    Dup
    Print
    Halt
}

error StackEmpty = "the program took more off the stack than it put on"
error StackFull = "the program put more on the stack than it holds"
error Truncated = "the program ends in the middle of an instruction"
error BadOp = "a byte that is no instruction"

struct Machine {
    stack [16]i64
    depth u64
}

func (mut m *Machine) Push(v i64) !void {
    if m.depth == m.stack.len {
        return error.StackFull
    }
    m.stack[m.depth] = v
    m.depth += 1
}

func (mut m *Machine) Pop() !i64 {
    if m.depth == 0 {
        return error.StackEmpty
    }
    m.depth -= 1
    return m.stack[m.depth]
}

func add(a i64, b i64) i64 { return a + b }
func sub(a i64, b i64) i64 { return a - b }
func mul(a i64, b i64) i64 { return a * b }

func run(code []u8) !void {
    // Indexed by the opcode, counting from Add.
    arithmetic := [3]func(i64, i64) i64{add, sub, mul}

    mut m := Machine{stack: [16]i64{}, depth: 0}
    mut pc u64 = 0
    for pc < code.len {
        op := Op(code[pc])
        pc += 1
        switch op {
        case Op.Push:
            if pc == code.len {
                return error.Truncated
            }
            try m.Push(i64(code[pc]))
            pc += 1
        case Op.Add, Op.Sub, Op.Mul:
            b := try m.Pop()
            a := try m.Pop()
            f := arithmetic[u8(op) - u8(Op.Add)]
            try m.Push(f(a, b))
        case Op.Dup:
            v := try m.Pop()
            try m.Push(v)
            try m.Push(v)
        case Op.Print:
            try io.Printf("{}\n", try m.Pop())
        case Op.Halt:
            return
        default:
            return error.BadOp
        }
    }
}

func main() !void {
    // (2 + 3) * 7, printed, then squared and printed
    program := [14]u8{
        u8(Op.Push), 2, u8(Op.Push), 3, u8(Op.Add),
        u8(Op.Push), 7, u8(Op.Mul),
        u8(Op.Dup), u8(Op.Print),
        u8(Op.Dup), u8(Op.Mul), u8(Op.Print),
        u8(Op.Halt),
    }
    try run(program[..])

    short := [3]u8{u8(Op.Push), 1, u8(Op.Add)}
    run(short[..]) catch |e| {
        try io.Printf("{}: {}\n", nameof(e), e.Message())
    }
    unknown := [1]u8{200}
    run(unknown[..]) catch |e| {
        try io.Printf("{}: {}\n", nameof(e), e.Message())
    }
}
```
<!-- prints:
35
1225
StackEmpty: the program took more off the stack than it put on
BadOp: a byte that is no instruction
-->

## Instructions as an enum

```sword
enum Op u8 {
    Push
    Add
    ...
}
```

Each member takes the next number from zero, and `u8` makes the enum a byte
wide, so a program is a `[]u8` and an instruction is one of its bytes.
`u8(Op.Push)` turns an instruction into its byte to write the program, and
`Op(code[pc])` turns a byte back into an instruction to run it.

That second conversion is not checked against the members. Byte 200 becomes an
`Op` that is none of them, and that is why the `switch` keeps its `default`: it
is the case for a program that is not well formed, and it answers
`error.BadOp`. Without a `default`, the compiler would insist on a case for every
member, and a byte outside them would match nothing.

## The stack

The stack is an array inside the struct, `[16]i64`, with a depth beside it. It
is part of the `Machine` value, which lives in `run`'s frame, so the machine
allocates nothing. `Push` and `Pop` fail with an error rather than reading past
either end, because a bytecode program is input, and input can be wrong.

`try m.Pop()` hands the error up from `run` to its caller, which is how the
second program, `Push 1, Add`, stops with `StackEmpty` when `Add` reaches for
a second number that is not there.

## A table of functions

```sword
arithmetic := [3]func(i64, i64) i64{add, sub, mul}
...
f := arithmetic[u8(op) - u8(Op.Add)]
try m.Push(f(a, b))
```

`Add`, `Sub` and `Mul` do the same thing except for one operation, so they
share a case, and the operation comes from an array indexed by the opcode. A
function value is the address of a top-level function, one word, and calling
it is an indirect call. There are no closures: what `f` can see is its two
arguments and nothing else. A table that needed state, say a machine with
registers, would hold an interface instead, with the state behind the pointer.

The order of the table follows the order of the enum, which is why the members
are declared in the order they are. An enum that grew a member between `Add`
and `Mul` would need the table to grow with it.
