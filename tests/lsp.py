#!/usr/bin/env python3
"""Drives swordls over stdio and checks what comes back."""
import json
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SERVER = os.path.join(ROOT, "swordls")

failures = []


def check(name, ok, detail=""):
    if ok:
        return
    failures.append(name)
    print("FAIL %s%s" % (name, (": " + detail) if detail else ""))


class Client:
    def __init__(self):
        self.proc = subprocess.Popen(
            [SERVER], stdin=subprocess.PIPE, stdout=subprocess.PIPE, cwd=ROOT
        )
        self.next_id = 0

    def send(self, method, params, want_reply=True):
        body = {"jsonrpc": "2.0", "method": method, "params": params}
        if want_reply:
            self.next_id += 1
            body["id"] = self.next_id
        raw = json.dumps(body).encode()
        self.proc.stdin.write(b"Content-Length: %d\r\n\r\n" % len(raw) + raw)
        self.proc.stdin.flush()

    def read(self):
        length = 0
        while True:
            line = self.proc.stdout.readline()
            if not line:
                return None
            if line in (b"\r\n", b"\n"):
                break
            if line.lower().startswith(b"content-length:"):
                length = int(line.split(b":")[1])
        return json.loads(self.proc.stdout.read(length))

    def until(self, predicate, limit=8):
        for _ in range(limit):
            message = self.read()
            if message is None:
                return None
            if predicate(message):
                return message
        return None

    def close(self):
        self.send("shutdown", {})
        self.until(lambda m: "result" in m)
        self.send("exit", {}, want_reply=False)
        self.proc.stdin.close()
        self.proc.wait(timeout=5)


def write(tmp, name, text):
    path = os.path.join(tmp, name)
    with open(path, "w") as handle:
        handle.write(text)
    return path


def main():
    # A directory each: a file belongs to its package, and the server checks the
    # whole package. Two `main` functions in one directory is a broken package,
    # and it should say so rather than being a convenient fixture.
    tmp = tempfile.mkdtemp()
    good_dir = os.path.join(tmp, "good")
    bad_dir = os.path.join(tmp, "bad")
    pkg_dir = os.path.join(tmp, "pkg")
    alias_dir = os.path.join(tmp, "alias")
    iface_dir = os.path.join(tmp, "iface")
    for d in (good_dir, bad_dir, pkg_dir, alias_dir, iface_dir):
        os.mkdir(d)
    good = write(good_dir, "good.sword", "func main() int {\n    x := 21\n    return x * 2\n}\n")
    bad = write(bad_dir, "bad.sword", "func main() int {\n    return nope\n}\n")
    # `type` is an identifier to the lexer and a keyword only here.
    alias = write(alias_dir, "alias.sword",
                  "type Bytes = []u8\n\nfunc main() int {\n    b := Bytes(\"hi\")\n"
                  "    return int(b.len)\n}\n")

    # Two files, one package: the name comes from the other file, and used to be
    # reported as undefined because only the open file was ever loaded.
    write(pkg_dir, "helper.sword",
          "struct Point {\n    X i64\n    Y i64\n}\n\n"
          "func twice(n i64) i64 {\n    return n * 2\n}\n")
    together = write(pkg_dir, "main.sword",
                     "func main() int {\n"
                     "    p := Point{X: 3, Y: 4}\n"
                     "    return int(twice(p.X) + p.Y)\n"
                     "}\n")
    # An interface method has no receiver in its type, so whether it writes
    # through one has to be said apart.
    iface = write(iface_dir, "iface.sword",
                  "interface Counter {\n    mut Bump()\n    Get() i64\n}\n\n"
                  "func show(c Counter) i64 {\n    return c.Get()\n}\n\n"
                  "func main() int {\n    return 0\n}\n")

    client = Client()
    reply = client.send("initialize", {"processId": None, "rootUri": None}) or client.read()
    caps = reply["result"]["capabilities"]
    check("initialize advertises semantic tokens", "semanticTokensProvider" in caps)
    # Without this an editor never wires completion up at all — it is what sets
    # omnifunc in vim and Neovim.
    check("initialize advertises completion", "completionProvider" in caps)
    check("a dot triggers completion",
          "." in caps.get("completionProvider", {}).get("triggerCharacters", []))
    legend = caps.get("semanticTokensProvider", {}).get("legend", {})
    check("legend has token types", "keyword" in legend.get("tokenTypes", []))
    check("legend has modifiers", "declaration" in legend.get("tokenModifiers", []))

    # A file that does not check should come back with a diagnostic on the
    # offending line.
    client.send(
        "textDocument/didOpen",
        {"textDocument": {"uri": "file://" + bad, "languageId": "sword",
                          "version": 1, "text": open(bad).read()}},
        want_reply=False,
    )
    note = client.until(lambda m: m.get("method") == "textDocument/publishDiagnostics")
    diags = note["params"]["diagnostics"] if note else []
    check("error file reports a diagnostic", len(diags) == 1, str(diags))
    if diags:
        check("diagnostic points at line 2", diags[0]["range"]["start"]["line"] == 1)
        check("diagnostic names the identifier", "nope" in diags[0]["message"],
              diags[0]["message"])

    client.send(
        "textDocument/didOpen",
        {"textDocument": {"uri": "file://" + good, "languageId": "sword",
                          "version": 1, "text": open(good).read()}},
        want_reply=False,
    )
    note = client.until(lambda m: m.get("method") == "textDocument/publishDiagnostics")
    check("clean file reports nothing", note and not note["params"]["diagnostics"])

    client.send(
        "textDocument/didOpen",
        {"textDocument": {"uri": "file://" + together, "languageId": "sword",
                          "version": 1, "text": open(together).read()}},
        want_reply=False,
    )
    note = client.until(lambda m: m.get("method") == "textDocument/publishDiagnostics")
    diags = note["params"]["diagnostics"] if note else []
    check("a sibling file's names are not undefined", not diags, str(diags))

    def complete(uri, line, character):
        client.send("textDocument/completion",
                    {"textDocument": {"uri": "file://" + uri},
                     "position": {"line": line, "character": character}})
        reply = client.until(lambda m: "result" in m and "items" in m.get("result", {}))
        return [i["label"] for i in reply["result"]["items"]] if reply else []

    lines = open(together).read().split("\n")
    at = lines.index("    return int(twice(p.X) + p.Y)")

    names = complete(together, at, len("    return int(twice(p."))
    check("a value's fields are offered", names == ["X", "Y"], str(names))

    names = complete(together, at, len("    return int("))
    check("a sibling file's function is offered", "twice" in names, str(names[:8]))
    check("a sibling file's type is offered", "Point" in names)
    check("a local is offered", "p" in names)
    check("a keyword is offered", "return" in names)
    check("a builtin type is offered", "u64" in names)

    client.send(
        "textDocument/didOpen",
        {"textDocument": {"uri": "file://" + iface, "languageId": "sword",
                          "version": 1, "text": open(iface).read()}},
        want_reply=False,
    )
    client.until(lambda m: m.get("method") == "textDocument/publishDiagnostics")
    at = open(iface).read().split("\n").index("    return c.Get()")
    client.send("textDocument/completion",
                {"textDocument": {"uri": "file://" + iface},
                 "position": {"line": at, "character": len("    return c.")}})
    reply = client.until(lambda m: "result" in m and "items" in m.get("result", {}))
    details = {i["label"]: i.get("detail", "")
               for i in (reply["result"]["items"] if reply else [])}
    check("an interface method that writes says so",
          details.get("Bump", "").startswith("mut "), str(details))
    check("one that does not, does not",
          not details.get("Get", "mut").startswith("mut "), str(details))

    client.send("textDocument/semanticTokens/full",
                {"textDocument": {"uri": "file://" + good}})
    reply = client.until(lambda m: "result" in m and "data" in m.get("result", {}))
    data = reply["result"]["data"] if reply else []
    check("semantic tokens come back", len(data) >= 5 and len(data) % 5 == 0,
          "len=%d" % len(data))

    if data:
        types = legend["tokenTypes"]
        mods = legend["tokenModifiers"]
        # First token of `func main() int {` is the keyword itself.
        check("first token is a keyword", types[data[3]] == "keyword",
              types[data[3]])
        # Second is the function name, marked as a declaration.
        check("function name is classified", types[data[8]] == "function",
              types[data[8]])
        check("function name is a declaration",
              data[9] & (1 << mods.index("declaration")) != 0)
        kinds = [types[data[i]] for i in range(3, len(data), 5)]
        check("an immutable binding shows up as a variable", "variable" in kinds)
        check("the builtin type is classified", "type" in kinds, str(kinds))

    client.send("textDocument/semanticTokens/full",
                {"textDocument": {"uri": "file://" + alias}})
    reply = client.until(lambda m: "result" in m and "data" in m.get("result", {}))
    data = reply["result"]["data"] if reply else []
    if len(data) >= 10:
        types = legend["tokenTypes"]
        mods = legend["tokenModifiers"]
        check("'type' opening an alias is a keyword", types[data[3]] == "keyword",
              types[data[3]])
        check("an alias's name is a type", types[data[8]] == "type", types[data[8]])
        check("an alias's name is a declaration",
              data[9] & (1 << mods.index("declaration")) != 0)
    else:
        check("semantic tokens come back for an alias", False, "len=%d" % len(data))

    client.close()

    if failures:
        print("%d LSP check(s) failed" % len(failures))
        return 1
    print("LSP: all checks passed")
    return 0


sys.exit(main())
