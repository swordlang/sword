# Fetching JSON

Fetches the JSON document swordlang.com publishes about its own build, over
HTTPS, and prints the commits it names. It talks to the network, so what it
prints depends on the day:

```
site abfbaa2
release 3bf468d
next 51f0cb7
```

Uses: [errors](../tour.md#errors), [memory](../tour.md#memory),
[the client](../http.md#the-client), [`std/json`](../reference.md#stdjson).

```sword
import "std/http"
import "std/io"
import "std/json"
import "std/mem"

// swordlang.com says which commits it was built from: the site's own, the
// release documented at /, and the development branch documented at /next/.
const Url = "https://swordlang.com/build.json"

func main() !int {
    mut backing := [65536]u8{}
    mut arena := mem.NewArena(backing[..])

    res := http.Fetch(Url, &arena) catch |e| {
        try io.Errorf("could not fetch {}: {}\n", Url, e.Message())
        return 1
    }
    if res.Status != 200 {
        try io.Errorf("{} answered {}\n", Url, res.Status)
        return 1
    }

    doc := json.Parse(res.Body, &arena) catch |e| {
        try io.Errorf("not JSON: {}\n", e.Message())
        return 1
    }
    root := doc.Root()
    if doc.Kind(root) != json.Kind.Object {
        try io.Errorf("expected an object, got {}\n", doc.Kind(root))
        return 1
    }
    for i in 0..doc.Len(root) {
        at := doc.At(root, i) orelse return 1
        commit := doc.Text(at)
        if commit.len < 7 {
            try io.Errorf("{} is not a commit\n", commit)
            return 1
        }
        try io.Printf("{} {}\n", doc.KeyAt(at), commit[0..7])
    }
    return 0
}
```

## One call

```sword
res := http.Fetch(Url, &arena) catch |e| {
```

`http.Fetch` takes a URL, which is the one form that can say `https`, and does
the rest: resolving the name, connecting, the TLS handshake, the request,
following redirects, and reading the reply whether it is sent with a length or
in chunks. The body is read into the allocator passed, so it lives as long as
the arena does.

Every way it can fail is an error: a name that does not resolve, a refused
connection, a certificate that does not check out, a timeout. They are caught
in one place and printed with their message, and the program exits with 1. A
status that is not 200 is not an error to `Fetch`, since the server did
answer, so it is checked separately.

## Walking the document

`json.Parse` reads the body into a flat list of nodes in the same arena. The
root should be an object; anything else is reported by its kind, which prints
as its name because `json.Kind` is an enum.

An object's members are reached by position: `doc.At(root, i)` is the i-th,
`doc.KeyAt` its key and `doc.Text` its value as a string. The values are
commit hashes, and the program prints the first seven characters of each, the
short form git uses, after checking each is that long. A document from a
server is input like any other.

Nothing here is freed one piece at a time. The response, the parsed document
and every string in them sit in one 64 KiB arena on `main`'s stack, and go
when `main` returns.
