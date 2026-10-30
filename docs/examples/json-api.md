# An HTTP JSON API

A server with two routes, one that reads a JSON body and one that takes a
parameter from the path, each request working in an arena of its own. The
program is its own client too: it starts the server, makes five requests,
prints the answers and shuts the server down.

Uses: [methods and interfaces](../tour.md#methods-and-interfaces),
[memory](../tour.md#memory), [tasks](../tour.md#tasks),
[routing](../http.md#routing), [`std/json`](../reference.md#stdjson),
[testing a server](../http.md#testing-a-server-without-leaving-the-program).

```sword
import "std/bytes"
import "std/http"
import "std/io"
import "std/json"
import "std/mem"
import "std/strings"

// POST /stats with {"numbers": [...]} answers their count, sum and mean.
struct Stats {
}

func (s *Stats) Serve(req *http.Request, mut res *http.Response) !void {
    // Everything this request needs comes from here, and goes when it returns.
    mut backing := [16384]u8{}
    mut arena := mem.NewArena(backing[..])

    doc := json.Parse(req.Body, &arena) catch {
        res.JSON(400, "{\"error\":\"the body could not be read as JSON\"}")
        return
    }
    list := doc.Get(doc.Root(), "numbers") orelse {
        res.JSON(400, "{\"error\":\"no numbers\"}")
        return
    }
    if doc.Kind(list) != json.Kind.Array {
        res.JSON(400, "{\"error\":\"numbers is not a list\"}")
        return
    }
    mut sum f64 = 0.0
    for i in 0..doc.Len(list) {
        at := doc.At(list, i) orelse return error.OutOfRange
        sum += doc.Number(at)
    }

    mut out := try bytes.New(&arena, 128)
    mut w := json.NewWriter(&out)
    w.BeginObject()
    w.Key("count")
    w.Int(i64(doc.Len(list)))
    w.Key("sum")
    w.Number(sum, 2)
    if doc.Len(list) > 0 {
        w.Key("mean")
        w.Number(sum / f64(doc.Len(list)), 2)
    }
    w.EndObject()
    try w.Err()
    res.JSON(200, out.Str())
}

// GET /hello/{name}
struct Hello {
}

func (h *Hello) Serve(req *http.Request, mut res *http.Response) !void {
    mut backing := [1024]u8{}
    mut arena := mem.NewArena(backing[..])
    mut out := try bytes.New(&arena, 64)
    mut w := json.NewWriter(&out)
    w.BeginObject()
    w.Key("greeting")
    w.Str(try strings.Concat(&arena, "hello, ", req.Param("name")))
    w.EndObject()
    try w.Err()
    res.JSON(200, out.Str())
}

func serve(server *http.Server, mux *http.Mux) !void {
    try server.Serve(mux)
}

// The client side, in the same program: a few requests, then the server is
// told to stop, which is what lets the scope end.
func drive(port i32, server *http.Server) !void {
    mut backing := [65536]u8{}
    mut arena := mem.NewArena(backing[..])
    mut client := http.NewClient()

    for body in [3]string{"{\"numbers\": [3, 4, 5.5]}", "{\"numbers\": []}", "{\"numbers\": 7}"} {
        r := try client.Post("127.0.0.1", port, "/stats", "application/json", []u8(body), &arena)
        try io.Printf("POST /stats {} -> {} {}\n", body, r.Status, string(r.Body))
    }
    r := try client.Get("127.0.0.1", port, "/hello/sword", &arena)
    try io.Printf("GET /hello/sword -> {} {}\n", r.Status, string(r.Body))
    missing := try client.Get("127.0.0.1", port, "/nowhere", &arena)
    try io.Printf("GET /nowhere -> {}\n", missing.Status)

    client.Close()
    server.Close()
}

func main() !void {
    mut stats := Stats{}
    mut hello := Hello{}
    mut mux := http.NewMux()
    try mux.Post("/stats", &stats)
    try mux.Get("/hello/{name}", &hello)

    // Port 0: the system picks one that is free.
    server := try http.Listen(0)
    scope {
        spawn serve(&server, &mux)
        spawn drive(server.Port(), &server)
    }
}
```
<!-- prints:
POST /stats {"numbers": [3, 4, 5.5]} -> 200 {"count":3,"sum":12.50,"mean":4.17}
POST /stats {"numbers": []} -> 200 {"count":0,"sum":0.00}
POST /stats {"numbers": 7} -> 400 {"error":"numbers is not a list"}
GET /hello/sword -> 200 {"greeting":"hello, sword"}
GET /nowhere -> 404
-->

## A handler is an interface

```sword
func (s *Stats) Serve(req *http.Request, mut res *http.Response) !void
```

Any struct with that method is an `http.Handler`. `Stats` and `Hello` hold
nothing, but a handler that needed a database connection or a cache would keep
it in its fields, which is where state goes in a language with no closures. The
router, `http.Mux`, maps a method and a path to a handler; `{name}` in a
pattern matches one segment of the path, read back with `req.Param("name")`.

## An arena per request

```sword
mut backing := [16384]u8{}
mut arena := mem.NewArena(backing[..])
```

Each call to `Serve` starts with 16 KiB on its own stack and an arena over it.
The parsed document, the output buffer and the joined greeting all come from
there, and all of it is gone when `Serve` returns, without a single `free`.
Nothing a request did can outlive it, and nothing leaks when a handler returns
early.

The arena is a local, so two requests served at the same time on two
connections have two arenas: the server runs a task per connection, and each
task's `Serve` has its own frame.

A body bigger than the arena has room to parse fails with an error rather than
growing, and gets a 400. That is a limit worth having on a server: how much memory a
request may take is decided by the code, not by whoever sends it.

## Reading and writing JSON

`json.Parse` builds the whole document as one flat list of nodes, found by
index. `doc.Get(root, "numbers")` is the node for that key or nil; `doc.Kind`
says what the node is, and anything but a list gets a 400 with a reason.

The reply is written with `json.Writer`, which puts the commas and quotes in
and escapes strings. None of its calls take a `try`: the first thing that goes
wrong is kept and every call after it does nothing, and `w.Err()` at the end
says what it was. That keeps a function writing a large document from being
one `try` per line.

## Serving and asking in one program

```sword
scope {
    spawn serve(&server, &mux)
    spawn drive(server.Port(), &server)
}
```

The server listens on port 0, which lets the system pick a free one, and
`drive` asks it for the port. Both tasks receive `&server`, and neither as
`mut`: serving and closing only read it. `drive` closes the server when it is
done, the accept loop ends, and the scope can join. This is how a server is
tested without leaving the program, and why this page can show what it prints.
