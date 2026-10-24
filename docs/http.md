# Writing an HTTP server

`std/http` is a small HTTP/1.1 server and client, written in Sword on top of
`std/net`. It does keep-alive, routing with path parameters, query strings,
timeouts, chunked transfer encoding, static files and TLS.

## A server

```sword
import "std/http"
import "std/strings"

struct Router {
    greeting string
}

func (r *Router) Serve(req *http.Request, mut res *http.Response) !void {
    if strings.Equal(req.Path, "/health") {
        res.Text(200, "ok")
        return
    }
    if strings.Equal(req.Path, "/") {
        res.Text(200, r.greeting)
        return
    }
    res.Text(404, "not found")
}

func main() !int {
    mut server := try http.Listen(8080)
    mut router := Router{greeting: "hello\n"}
    try server.Serve(&router)
    return 0
}
```

A handler is an interface, not a function value, the same way Go's
`http.Handler` is. That gives the handler somewhere to keep its own state —
`Router` above carries a greeting, and it could just as well carry a database
handle or a counter.

Note the `&`. Only pointers satisfy an interface in Sword, because boxing a
value would mean allocating.

## How it runs

One task per connection. The accept loop is the body of the scope rather than a
task in it, because only the body may spawn:

```sword
scope {
    for {
        c := s.listener.Accept() catch break
        spawn serveConn(c, h, s)
    }
}
```

That shape is only affordable because a task waiting on a socket costs a stack
and not a thread — the task is put down and the worker goes to whoever has data.
See [Concurrency](concurrency.md#waiting).

Measured on ten cores, with keep-alive, which is what every real client does:

| Connections at once | |
|---|---|
| 33 | 198 000 req/s |
| 1 000 | 146 000 req/s, 12 threads, 34 MiB |
| 4 000 | 84 000 req/s, 12 threads |

Two thousand idle keep-alive connections sit in 65 MiB on twelve threads — about
33 KiB each, most of it the connection's own 16 KiB arena.

**Each connection has its own arena**, 16 KiB on its own stack. That is not an
optimisation, it is a requirement the compiler enforces: an allocator is mutable
state, and handing the same one to two tasks is a compile error.

`ServeWith(h, n)` caps how many connections are served at once; `Serve` allows
1024. Past the cap the server stops accepting and the kernel's backlog holds the
rest, which is what backpressure should look like from outside: a queue, not a
collapse. `s.Live()` says how many are in flight.

## Stopping

`Close` shuts the listener down, which ends the accept loop. The scope then
waits for every connection still being served, so shutting down drains rather
than cuts:

```sword
scope {
    spawn runServer(&server, &router)
    spawn runUntilDone(&server)
}
```

A plain `close` would not be enough — on BSD it does not wake a thread already
inside `accept`, so `Close` shuts the socket down first.

## Limits

Two, and they do different jobs:

```sword
try c.SetTimeout(time.Seconds(15))                    // any one wait
try c.SetDeadline(time.Now().Add(time.Seconds(30)))   // the whole exchange
```

The server sets both: fifteen seconds for a single wait, and thirty for one turn
of the keep-alive loop — waiting for a request and serving it. A timeout alone
cannot bound a client that dribbles a byte at a time, because no single wait ever
runs out.

No write to a response asks for `try`. The first one that fails is kept and
every write after it does nothing; once the handler returns, the server looks.
A failed response counts as a failed request, and is answered with a 500 if its
head has not gone yet — or, if it has, its connection is closed, since the client
already has part of something else.

A request is capped at 32 headers and 8 KiB of head. Past that the connection is
dropped rather than grown, which keeps a hostile client from deciding how much
memory the server uses.

## Streaming a reply

A response is normally collected, measured and sent with a `Content-Length`. When
the length is not known in advance — a long list, a file being read, an event
stream — say so and every write goes out as its own chunk:

```sword
func (h *Feed) Serve(req *http.Request, mut res *http.Response) !void {
    res.SetHeader("Content-Type", "text/event-stream")
    res.Stream(200)
    for update := h.updates.Recv() {
        res.Printf("data: {}\n\n", update)
        try res.Err()       // the client has gone: stop feeding it
    }
}
```

`Stream` sends the head immediately, so headers set after it are too late. The
handler returning ends the body. A handler that keeps working between writes —
a stream fed for minutes — asks `res.Err()` now and then, so a client that went
away does not keep it busy. Nothing else about writing changes — `Write`,
`WriteString` and `Printf` do what they always did, they just leave rather than
accumulate.

When the length *is* known before the body exists — a file, a blob out of a
database — `Send(status, length)` is the better half of the same idea. It puts the
head out with that length and writes straight through after it, so the client
learns how much is coming and nothing has to be held in memory to be measured:

```sword
func (h *Download) Serve(req *http.Request, mut res *http.Response) !void {
    res.SetHeader("Content-Type", "application/octet-stream")
    res.Send(200, h.size)
    for part in h.pieces() {
        res.Write(part)
    }
}
```

Write exactly that many bytes. Fewer leaves the client waiting for the rest; more
is a response that says one thing and does another.

A request arriving with `Transfer-Encoding: chunked` is decoded before the
handler sees it, in place in the read buffer: a chunk's bytes always sit further
along than where they end up, so no second buffer is needed.

## HTTPS

One line changes:

```sword
mut server := try http.ListenTLS(443, "chain.pem", "key.pem")
try server.Serve(&mux)
```

The handlers, the router, the streaming and the keep-alive are all the same — the
server reads and writes a `net.Stream`, and a TLS connection is one. `ListenOnTLS`
takes an address as well, for a server that is not on loopback.

Both files are PEM. The certificate one should be the **chain**, not just your
certificate: a client that cannot build a path to an authority it trusts refuses
the connection, and the intermediates are how it builds one. Both are read once at
startup, so a bad pair fails where somebody is watching rather than on the first
request.

A handshake that fails costs that one connection and nothing is written back.
There is no HTTP yet to answer with, and a client that offered a protocol from
2011 would not read a 400 anyway. TLS 1.2 is the floor.

### Asking who is calling

A server can require a certificate from the client as well, which is what people mean
by mutual TLS. `tls.ServerConfig` says what to present and what to ask for:

```sword
import "std/http"
import "std/tls"

func main() !int {
    mut conf := tls.NewServerConfig("chain.pem", "key.pem")
    conf.ClientCA = "clients.pem"        // the authority their certificates come from
    conf.Clients = tls.Ask.Required      // and a connection without one is refused

    mut mux := http.NewMux()
    mut server := try http.ListenWith(8443, conf)
    try server.Serve(&mux)
    return 0
}
```

`Ask` has three settings and the middle one is where accidents happen:

| | |
|---|---|
| `Ask.Nobody` | the default: anybody connects, and the server learns nothing about them |
| `Ask.Required` | a certificate signed by `ClientCA`, or the connection is refused |
| `Ask.Optional` | a certificate if they have one, and a connection either way |

Under `Required`, a handler may believe what the certificate says. Under `Optional`
it may not — both kinds of connection get through, so the handler has to look:

```sword
func (h *Site) Serve(req *http.Request, mut res *http.Response) !void {
    if req.PeerName.len == 0 {
        res.Text(401, "who are you\n")
        return
    }
    res.Printf("hello {}\n", req.PeerName)
}
```

`req.PeerName` is the subject of the certificate the client presented —
`/CN=worker-7` — and empty when there was none. It is read once per connection, not
per request, because a certificate cannot change while a connection lasts.

Asking for a certificate without an authority to check it against fails at startup
rather than being quietly ignored. That combination reads like security and is not.

For the client half, `Client.TLS` carries the certificate to present:

```sword
mut client := http.NewClient()
client.TLS.CAFile = "ca.pem"
client.TLS.CertFile = "worker.pem"
client.TLS.KeyFile = "worker-key.pem"
```

And to make the pair to test against, `tls.SignedBy` signs with an authority instead
of with itself:

```sword
try tls.SelfSigned("my-authority", "ca.pem", "ca-key.pem")
try tls.SignedBy("api.internal", "ca.pem", "ca-key.pem", "api.pem", "api-key.pem", false)
try tls.SignedBy("worker-7", "ca.pem", "ca-key.pem", "worker.pem", "worker-key.pem", true)
```

The last argument marks the certificate for client authentication. A strict peer
refuses one issued for the other job, and the message it gives will not help you.

`Free()` gives the certificate back, and it is separate from `Close()` on purpose:
`Close` stops the accept loop and is normally called from inside a task, at which
moment other connections are still handshaking against the certificate. Free it
after the scope has joined, or not at all if the server outlives everything.

For a certificate to develop against:

```sword
import "std/tls"

try tls.SelfSigned("127.0.0.1", "dev-cert.pem", "dev-key.pem")
```

That one is signed by nobody, so a client has to be told to trust it — which is
the whole point of the exercise. See [the TLS
reference](reference.md#stdtls) for what a client can be told.

### Calling one

```sword
res := try http.Fetch("https://example.com/health", &arena)
```

`Fetch` takes a URL because that is the only shape that can say `https`.
`GetTLS`, `PostTLS` and `DoTLS` take a host and port instead, for when those are
what you have. A redirect from `http` to `https` is followed like any other.

Trust is `Client.TLS`, a `tls.Config`:

```sword
mut client := http.NewClient()
client.TLS.CAFile = "dev-cert.pem"     // a private authority, or a self-signed one

res := try client.GetTLS("127.0.0.1", 8443, "/health", &arena)
```

Setting `Verify` to false instead turns TLS into encryption with no idea who is on
the other end. That is a reasonable thing to do against your own machine and never
anywhere else.

## Serving files

```sword
import "std/http"

func main() !int {
    mut site := http.NewFiles("public")

    mut mux := http.NewMux()
    try mux.Get("/static/{path...}", &site)
    try mux.Handle("HEAD", "/static/{path...}", &site)

    mut server := try http.Listen(8080)
    try server.Serve(&mux)
    return 0
}
```

`{path...}` catches the rest of the path — see the routing section — and `Files`
looks that up under its root. So the route decides what the URL looks like and the
handler decides what is on disk, and the two are free to differ.

A file goes out a piece at a time against the length the operating system already
knows, so a hundred-megabyte download costs one buffer rather than a hundred
megabytes. The Content-Type is guessed from the extension. A directory gets
`Index`, which is `index.html` unless you change it, and setting `Index` to an
empty string turns directories into 404s instead.

For one file rather than a tree:

```sword
func (h *Site) Serve(req *http.Request, mut res *http.Response) !void {
    try http.ServeFile(res, "public/index.html")
}
```

Every file goes out with `Last-Modified` and an `ETag`, so a browser that already
has it asks again with `If-None-Match` or `If-Modified-Since` and gets a 304 with
no body when nothing changed. The tag is made from the file's size and modification
time rather than a hash of its contents, which would mean reading the whole file to
answer a request that wants none of it; it is marked weak (`W/"..."`) for the same
reason, since it promises that nothing the filesystem saw happened, not that every
byte is the same. When a request carries both headers, the tag decides, as RFC 9110
says it must. `ServeFile` sends the same two headers but has no request to read the
conditions from, so it always answers in full.

**A path with `..` in it is refused, not resolved.** So are paths holding a NUL or
a backslash, and paths too long for the buffer they would be built in. Resolving
first and checking afterwards is where every traversal bug comes from, so this
never resolves at all.

Reading the file puts the task down like any other wait — the thread it borrows
comes from [the pool the runtime keeps](concurrency.md#what-cannot-be-put-down)
for calls no poller can answer. A hundred clients downloading do not cost a
hundred threads.

## Shutting down on a signal

This is what a server's last ten lines look like:

```sword
import "std/http"
import "std/os"

struct Health {
    started i64
}

func (h *Health) Serve(req *http.Request, mut res *http.Response) !void {
    res.Text(200, "ok\n")
}

func serve(s *http.Server, m *http.Mux) !void {
    try s.Serve(m)
}

func shutdown(s *http.Server) !void {
    try os.Catch(os.Signal.Terminate)
    try os.Catch(os.Signal.Interrupt)
    sig := try os.WaitSignal()
    s.Close()
}

func main() !int {
    mut health := Health{started: 0}
    mut srv := try http.ListenOn("", 8080, true)
    mut mux := http.NewMux()
    try mux.Get("/health", &health)

    scope {
        spawn serve(&srv, &mux)
        spawn shutdown(&srv)
    }
    return 0
}
```

`WaitSignal` is an ordinary task: a signal handler may do almost nothing safely,
so one writes a byte down a pipe and the waiting is reading the other end —
which costs a stack and not a thread, like every other wait here.

`Close` stops the accept loop, and the `scope` then waits for every connection
still being served. Nothing is interrupted; requests in flight finish.

`ListenOn("", 8080, true)` binds every interface and lets another process hold
the same port, which is how one is replaced by another without dropping
anything in between.

## Watching it run

A server nobody can see inside is a server nobody can run. Four counters on the
server, and the scheduler's own next to them:

```sword
import "std/runtime"

func (h *Admin) Serve(req *http.Request, mut res *http.Response) !void {
    r := runtime.Read()
    res.Printf("threads {} stacks {} running {}\n",
                   r.Threads, r.Stacks, r.Running())
    res.Status = 200
}
```

`s.Live()` is connections in flight, `s.Accepted()` how many have been taken
altogether, and `s.Served()` and `s.Failed()` requests answered and handlers that
did not. `runtime.Queued` is the one to watch: a queue that keeps growing means
the server is taking more than it can serve.

## Routing

The `if` chain above gets old quickly. `Mux` is a handler that dispatches to
other handlers by method and path:

```sword
import "std/http"

struct Users {
    seen u64
}

func (h *Users) Serve(req *http.Request, mut res *http.Response) !void {
    res.Printf("user {}\n", req.Param("id"))
}

struct Health {
    started i64
}

func (h *Health) Serve(req *http.Request, mut res *http.Response) !void {
    res.Text(200, "ok\n")
}

func main() !int {
    mut users := Users{seen: 0}
    mut health := Health{started: 0}

    mut mux := http.NewMux()
    try mux.Get("/health", &health)
    try mux.Get("/users/{id}", &users)
    try mux.Post("/users", &users)

    mut server := try http.Listen(8080)
    try server.Serve(&mux)
    return 0
}
```

A `{name}` matches exactly one path segment and leaves what it matched behind
for `req.Param("name")`. So `/users/{id}` matches `/users/7` and not
`/users/7/posts`. Routes are tried in the order you added them, trailing
slashes are ignored, and there are `Get`, `Post`, `Put`, `Delete` and the
general `Handle(method, pattern, h)`.

A `{name...}` at the end of a pattern matches the rest of the path instead,
slashes and all, which is how a whole subtree goes to one handler:
`/static/{path...}` matches `/static/css/app.css` and leaves `css/app.css` in
`req.Param("path")`. It matches `/static/` too, with nothing in the parameter —
which is the request that wants an index page.

Two answers come from the mux itself. A path no route matched is a 404. A path
that matched under a different method is a **405**, which is the more useful
reply: it tells the client that retrying with another verb might work.

A mux holds up to 64 routes in a fixed array — it never allocates, so it can
live on the stack of `main` and be shared by every accept loop.

## Reading a request

```sword
func (r *Router) Serve(req *http.Request, mut res *http.Response) !void {
    if !strings.Equal(req.Method, "POST") {
        res.Text(405, "method not allowed")
        return
    }

    kind := req.Headers.Get("Content-Type")     // case-insensitive
    if !strings.HasPrefix(kind, "application/json") {
        res.Text(415, "send me json")
        return
    }

    res.JSON(200, "{\"length\":0}")
    res.Write(req.Body)
}
```

`req.Path`, `req.Method` and the header strings all point into the connection's
read buffer. They are valid while the handler runs and not afterwards — copy
anything you mean to keep.

### The query string

`GET /search?q=ada+king&page=2` arrives split in three:

| Field | Value |
|---|---|
| `req.Target` | `/search?q=ada+king&page=2` |
| `req.Path` | `/search` |
| `req.RawQuery` | `q=ada+king&page=2` |

`req.Query("page")` gives `2`. It is still percent-encoded, because decoding
means writing somewhere and a request has no allocator — so decoding is a
separate step, with the space coming from you:

```sword
mut room := [64]u8{}
q := try http.Unescape(req.Query("q"), room[..])   // "ada king"
```

`http.Escape` goes the other way, for building a target out of a value that may
contain anything. Both fail with `error.NoSpace` rather than writing past the
end of what you gave them.

## The client

```sword
import "std/http"
import "std/mem"

func main() !int {
    mut backing := [65536]u8{}
    mut arena := mem.NewArena(backing[..])

    res := try http.Get("127.0.0.1", 8080, "/health", &arena)
    if res.Status != 200 {
        return 1
    }

    body := "{\"name\":\"sword\"}"
    posted := try http.Post("127.0.0.1", 8080, "/echo", "application/json",
                            []u8(body), &arena)
    return int(posted.Status / 100)
}
```

The response body lives in the allocator you pass in, so it stays valid until
you reset or free that allocator.

**A client keeps the connection it used.** The next request to the same host and
port goes down the same socket, which over TLS saves the handshake — most of what a
request costs. Five requests to one place is one connection.

```sword
mut client := http.NewClient()
defer client.Close()

for i in 0..3 {
    res := try client.Get("127.0.0.1", 8080, "/health", &arena)
}
```

`Close()` is not optional housekeeping: a kept connection holds a task on the
server for as long as it is open. The one-shot `http.Get`, `http.Post` and
`http.Fetch` own their client and close it for you.

Keeping state is why the client's methods take `mut`, and why a client belongs to
one task. Two tasks sharing one would be a race, and the checker says so.

A socket that has been sitting idle may have been closed by the far end without a
word, and the only way to find out is to use it. A request that fails on the kept
connection is retried on a fresh one — but only `GET` and `HEAD`, because anything
else may already have happened at the other end even though the answer never
arrived. For those, the failure comes back as `error.Interrupted` and retrying is
your decision.

A reply framed with `Transfer-Encoding: chunked` is decoded for you, and one with
no framing at all — no length, no chunks — is read until the connection closes.
Both matter: this package's own server sends the first for anything streamed.

Both of those go through a default `Client`, which gives up after 10 seconds
trying to connect and 30 seconds waiting for a reply. Build your own when those
are wrong:

```sword
import "std/time"

mut client := http.NewClient()
client.Connect = time.Millis(500)
client.Read = time.Seconds(5)

res := try client.Get("127.0.0.1", 8080, "/health", &arena)
```

The two limits are separate because they fail for different reasons. A host
that is not listening answers within a moment, so a short connect timeout
catches a wrong address without cutting off a server that is merely thinking. A
zero duration on either waits as long as the kernel would — which for connect
is over a minute, and is why the default is not zero.

### Redirects

Up to five are followed. `Redirects` is the limit, and setting it to zero hands
the 3xx back instead — which is what a client that wants to decide for itself
needs:

```sword
mut client := http.NewClient()
client.Redirects = 0

res := try client.Get("127.0.0.1", 8080, "/old", &arena)
if res.Status == 301 {
    moved := res.Headers.Get("Location")
}
```

301, 302 and 303 turn into a GET with no body, the way every client on the web
does it rather than the way the specification says. 307 and 308 keep the method
and the body, which is what they were added for. A `Location` pointing at `https`
fails rather than being fetched in the clear.

`ParseURL` is the same parser, if you need it:

```sword
url := try http.ParseURL("http://example.com:8080/a?b=1", "fallback.host", 80)
// url.Host "example.com", url.Port 8080, url.Target "/a?b=1", url.TLS false
```

A URL that is only a path keeps the host and port passed in, which is how a
relative `Location` is resolved.

## Testing a server without leaving the program

Because a server is just a task, a test can run both sides at once:

```sword
scope {
    spawn serve(&server, &router)
    spawn drive(port, &server, results)
}
```

`drive` makes its requests, writes what it found into a slice the parent owns,
and then calls `server.Close()` — which is what lets the scope join and the
program finish. `tests/http.sword` and `tests/httpkeepalive.sword` are exactly
this.

Both tasks are handed `&server`, and neither takes it as `mut`: serving and
closing only read the descriptor. Mark either parameter `mut` and the race
checker rejects the program, which is the correct answer to "two tasks are
writing the same thing".

## What is missing

There is no cookie or form parsing and no range requests, and the client does not
send chunked itself — the server reads chunked requests but the client does not
write them. The client keeps one connection rather than a pool, so calls to several
services in a row still pay for a handshake each time.
