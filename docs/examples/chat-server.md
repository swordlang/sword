# A chat server

A TCP server where every line one client sends reaches all the others. A task
per connection, and one shared room they all write through. It runs until it
is stopped, so this page shows no output; start it and connect with `nc` from
two terminals:

```
$ ./chat
chat on port 9000; connect with: nc 127.0.0.1 9000
```

Uses: [tasks](../tour.md#tasks), [the outside world](../tour.md#the-outside-world),
[shared structures](../concurrency.md#shared-structures),
[waiting](../concurrency.md#waiting), [`std/net`](../reference.md#stdnet).

```sword
import "std/io"
import "std/net"

// Everyone connected. The lock is what lets one task write to every other
// task's connection while they come and go.
struct Room {
    conns [256]net.Conn
    used  [256]bool
}

func join(room *shared[Room], c net.Conn) ?u64 {
    lock r := room {
        for seat in 0..r.used.len {
            if !r.used[seat] {
                r.used[seat] = true
                r.conns[seat] = c
                return seat
            }
        }
    }
    return nil
}

func leave(room *shared[Room], seat u64) {
    lock r := room {
        r.used[seat] = false
    }
}

// Sends one line to everybody but its author. A client that cannot take it
// is skipped; its own task will find the connection broken and leave.
func tell(room *shared[Room], from u64, line []u8) {
    lock r := room {
        for seat in 0..r.used.len {
            if r.used[seat] && seat != from {
                r.conns[seat].Write(line) catch {}
            }
        }
    }
}

// One task per connection: read whatever arrives and pass it on.
func talk(room *shared[Room], c net.Conn) {
    seat := join(room, c) orelse {
        c.WriteString("the room is full\n") catch {}
        c.Close()
        return
    }
    mut buf := [4096]u8{}
    for {
        n := c.Read(buf[..]) catch break
        if n == 0 {
            break
        }
        tell(room, seat, buf[0..n])
    }
    leave(room, seat)
    c.Close()
}

func main() !int {
    mut listener := try net.Listen(9000)
    try io.Printf("chat on port {}; connect with: nc 127.0.0.1 {}\n", listener.Port(), listener.Port())
    room := shared[Room](Room{conns: [256]net.Conn{}, used: [256]bool{}})
    scope {
        for {
            c := listener.Accept() catch break
            spawn talk(&room, c)
        }
    }
    return 0
}
```

## A task per connection

```sword
scope {
    for {
        c := listener.Accept() catch break
        spawn talk(&room, c)
    }
}
```

Each accepted connection gets a task, and the task does the obvious thing:
read, pass it on, read again. A read with nothing to read puts the task down
and hands its thread to another, so a thousand quiet connections cost a
thousand small stacks and a few threads, not a thousand threads. Code that
looks blocking is how a Sword server is written.

The accept loop is the body of the `scope` rather than a task inside it,
because only the body of a `scope` may spawn. When `Accept` fails, the loop
ends and the closing brace waits for every connection still open.

## The room

```sword
struct Room {
    conns [256]net.Conn
    used  [256]bool
}
```

Every task has to reach every other task's connection: to join, to leave, and
to write each line to the others. That is one structure written by many tasks,
so it is a `shared[Room]`, and `lock` is the only way into it. A plain `Room`
lent to two tasks would not compile.

A seat is an index into the two arrays. `join` takes the first free one, or
answers nil when all 256 are taken, and the connection is told so and closed.
`leave` frees the seat when the client goes. `Room` is two arrays inside a
value, so the room allocates nothing however many come and go.

## Passing a line on

`tell` writes to every other connection while holding the lock, so two lines
sent at once arrive in the same order everywhere. The cost is that one slow
client holds the lock while its write waits, and the room waits with it. A
larger server gives each client a channel of outgoing lines and a task that
drains it, so a slow reader only falls behind on its own.

A write that fails is skipped rather than handled. The client it was for has
gone, and its own task will find out on its next read, give up the seat and
close the connection, which keeps every connection closed by the task that
owns it.

`net.Listen` binds the loopback address only, so this server can be reached
from the machine it runs on and nowhere else. `net.ListenOn` takes the address
to bind for one that should be reachable from outside.
