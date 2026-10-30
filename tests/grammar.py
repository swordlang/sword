#!/usr/bin/env python3
"""docs/grammar.md is the grammar of the language, and this holds it to what the
parser does. Every Sword file in the tests, the standard library and the
documentation's whole programs must be a sentence of it, and the tests whose
expected error comes from the parser or the lexer must not be.

The ```ebnf blocks are read, turned into plain BNF, and run as an Earley
recognizer — which takes any context-free grammar, ambiguous or not — over the
tokens of each file, lexed the way src/lexer.cpp lexes them, including the `;`
a newline becomes."""

import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

KEYWORDS = set("""package import func return if else for in switch case select
default break continue mut const struct interface enum extern det defer errdefer
try catch orelse scope spawn parallel reduce lock true false nil""".split())

ENDS_STATEMENT = {"identifier", "int_lit", "float_lit", "string_lit", ")", "]",
                  "}", "return", "break", "continue", "true", "false", "nil"}

# Longest first, so that the first match is the token the lexer would take.
PUNCT = sorted("""+%= -%= *%= <<= >>= ... += -= *= /= %= &= |= ^= := == != <=
>= && || << >> <- .. +% -% *% + - * / % & | ^ ! < > = ( ) [ ] { } , : . ? ;
~""".split(), key=len, reverse=True)


def lex(text):
    toks = []  # (kind, text)
    i, n = 0, len(text)

    def push(kind, spelling):
        toks.append((kind, spelling))

    while i < n:
        c = text[i]
        if c == "\n":
            if toks and toks[-1][0] in ENDS_STATEMENT:
                push(";", ";")
            i += 1
        elif c in " \t\r":
            i += 1
        elif text.startswith("//", i):
            while i < n and text[i] != "\n":
                i += 1
        elif text.startswith("/*", i):
            depth, i = 1, i + 2
            while depth and i < n:
                if text.startswith("/*", i):
                    depth, i = depth + 1, i + 2
                elif text.startswith("*/", i):
                    depth, i = depth - 1, i + 2
                else:
                    i += 1
        elif c.isdigit():
            j = i
            if text[i] == "0" and i + 1 < n and text[i + 1] in "xbo":
                j = i + 2
                while j < n and (text[j].isalnum() or text[j] == "_"):
                    j += 1
                push("int_lit", text[i:j])
            else:
                while j < n and (text[j].isdigit() or text[j] == "_"):
                    j += 1
                is_float = False
                if j < n and text[j] == "." and not text.startswith("..", j):
                    is_float = True
                    j += 1
                    while j < n and (text[j].isdigit() or text[j] == "_"):
                        j += 1
                if j < n and text[j] in "eE":
                    is_float = True
                    j += 1
                    if j < n and text[j] in "+-":
                        j += 1
                    while j < n and text[j].isdigit():
                        j += 1
                push("float_lit" if is_float else "int_lit", text[i:j])
            i = j
        elif c == '"':
            j = i + 1
            while j < n and text[j] != '"':
                if text[j] == "\n":
                    raise SyntaxError("unterminated string")
                if text[j] == "\\":
                    esc = text[j + 1]
                    if esc == "x":
                        if not re.match(r"[0-9a-fA-F]{2}", text[j + 2:j + 4]):
                            raise SyntaxError("'\\x' needs two hex digits")
                        j += 2
                    elif esc not in "ntr0e\\\"'":
                        raise SyntaxError("unknown escape")
                    j += 1
                j += 1
            push("string_lit", text[i:j + 1])
            i = j + 1
        elif c.isalpha() or c == "_":
            j = i
            while j < n and (text[j].isalnum() or text[j] == "_"):
                j += 1
            word = text[i:j]
            push(word if word in KEYWORDS else "identifier", word)
            i = j
        else:
            for p in PUNCT:
                if text.startswith(p, i):
                    push(p, p)
                    i += len(p)
                    break
            else:
                raise SyntaxError("unexpected character %r" % c)
    if toks and toks[-1][0] in ENDS_STATEMENT:
        push(";", ";")
    return toks


# --- the grammar -------------------------------------------------------------

def ebnf_blocks(path):
    text = open(path).read()
    return "\n".join(re.findall(r"```ebnf\n(.*?)```", text, re.S))


def tokenize_ebnf(src):
    return re.findall(r'"[^"]*"|[A-Za-z_][A-Za-z_0-9]*|[=|\[\]{}().]', src)


class Grammar:
    def __init__(self, src):
        self.rules = {}  # name -> list of alternatives (lists of symbols)
        self.fresh = 0
        toks = tokenize_ebnf(src)
        pos = 0
        while pos < len(toks):
            name = toks[pos]
            assert toks[pos + 1] == "=", "expected '=' after %s" % name
            pos += 2
            alts, pos = self.expression(toks, pos)
            assert toks[pos] == ".", "expected '.' ending %s, got %s" % (name, toks[pos])
            pos += 1
            assert name not in self.rules, "%s defined twice" % name
            self.rules[name] = alts

    def helper(self, alts):
        self.fresh += 1
        name = "_%d" % self.fresh
        self.rules[name] = alts
        return name

    def expression(self, toks, pos):
        alts = []
        seq, pos = self.term(toks, pos)
        alts.append(seq)
        while toks[pos] == "|":
            seq, pos = self.term(toks, pos + 1)
            alts.append(seq)
        return alts, pos

    def term(self, toks, pos):
        seq = []
        while toks[pos] not in ("|", ")", "]", "}", "."):
            t = toks[pos]
            if t == "(":
                alts, pos = self.expression(toks, pos + 1)
                assert toks[pos] == ")"
                seq.append(self.helper(alts))
            elif t == "[":
                alts, pos = self.expression(toks, pos + 1)
                assert toks[pos] == "]"
                seq.append(self.helper(alts + [[]]))
            elif t == "{":
                alts, pos = self.expression(toks, pos + 1)
                assert toks[pos] == "}"
                inner = self.helper(alts)
                rep = "_%d" % (self.fresh + 1)
                self.fresh += 1
                self.rules[rep] = [[], [rep, inner]]
                seq.append(rep)
            else:
                seq.append(t)
            pos += 1
        return seq, pos

    def undefined(self):
        missing = set()
        for alts in self.rules.values():
            for seq in alts:
                for s in seq:
                    if s[0].isupper() and s not in self.rules:
                        missing.add(s)
        return missing


OPERATORS = {"+", "-", "*", "/", "%", "&", "|", "^", "<<", ">>", "+%", "-%",
             "*%", "&&", "||"}


def matches(symbol, tok):
    kind, text = tok
    if symbol.startswith('"'):
        lit = symbol[1:-1]
        # `error`, `weak` and `_` are identifiers the grammar spells out.
        return kind == lit or (kind == "identifier" and text == lit)
    if symbol == "operator":
        return kind in OPERATORS
    return symbol == kind


def is_terminal(symbol):
    return symbol.startswith('"') or symbol[0].islower()


def recognize(g, start, toks):
    """Earley. An item is (rule, alternative, dot, origin)."""
    nullable = set()
    changed = True
    while changed:
        changed = False
        for name, alts in g.rules.items():
            if name in nullable:
                continue
            if any(all(s in nullable for s in seq) for seq in alts):
                nullable.add(name)
                changed = True

    chart = [dict() for _ in range(len(toks) + 1)]

    def add(k, item):
        if item not in chart[k]:
            chart[k][item] = True
            return True
        return False

    for a in range(len(g.rules[start])):
        add(0, (start, a, 0, 0))
    for k in range(len(toks) + 1):
        agenda = list(chart[k].keys())
        while agenda:
            item = agenda.pop()
            name, a, dot, origin = item
            seq = g.rules[name][a]
            if dot < len(seq):
                sym = seq[dot]
                if is_terminal(sym):
                    if k < len(toks) and matches(sym, toks[k]):
                        add(k + 1, (name, a, dot + 1, origin))
                else:
                    for b in range(len(g.rules[sym])):
                        new = (sym, b, 0, k)
                        if add(k, new):
                            agenda.append(new)
                    if sym in nullable:
                        new = (name, a, dot + 1, origin)
                        if add(k, new):
                            agenda.append(new)
            else:
                for (n2, a2, d2, o2) in list(chart[origin].keys()):
                    s2 = g.rules[n2][a2]
                    if d2 < len(s2) and s2[d2] == name:
                        new = (n2, a2, d2 + 1, o2)
                        if add(k, new):
                            agenda.append(new)
        if k < len(toks) and not chart[k + 1]:
            return k  # the first token nothing could take
    for (name, a, dot, origin) in chart[len(toks)]:
        if name == start and origin == 0 and dot == len(g.rules[name][a]):
            return None
    return len(toks)


# What the parser and the lexer say when they refuse something. A test that
# expects one of these is a test the grammar must refuse too; a new message in
# the parser that a test expects belongs here.
SYNTAX_ERRORS = [
    r"^expected ", r"a function type cannot end", r"only an extern function ends",
    r"'\.\.\.' needs a parameter", r"only one name can gather",
    r"an error's message has to be a string literal",
    r"an extern function cannot be generic", r"a library, 'weak' and 'blocking' belong",
    r"an extern function is weak or blocking, not both",
    r"'det' belongs to a function", r"a select case binds",
    r"a select case is a receive or a send", r"unterminated", r"malformed numeric",
    r"unexpected character", r"unknown escape", r"takes two hex digits",
    r"a type alias is written", r"a type alias takes no type parameters",
    r"a field takes no 'mut'", r"a field cannot gather",
    r"runs a statement on the way out", r"a function type lists types",
    r"with no field named, write",
]


def expects_syntax_error(text):
    for line in text.split("\n")[:5]:
        m = re.match(r"// expect-error: (.*)", line)
        if m:
            return any(re.search(p, m.group(1)) for p in SYNTAX_ERRORS)
    return False


# The documentation's whole programs, the same ones tests/docs.sh compiles.
def doc_programs():
    pages = glob.glob(os.path.join(ROOT, "docs", "*.md")) + [
        os.path.join(ROOT, p) for p in
        ("README.md", "CONTRIBUTING.md", "editors/README.md",
         "editors/vscode/README.md")]
    for page in sorted(pages):
        if not os.path.exists(page):
            continue
        inside, body, start = False, [], 0
        for number, line in enumerate(open(page).read().split("\n"), 1):
            if not inside and re.match(r"^```sword[ \t]*$", line):
                inside, body, start = True, [], number
            elif inside and line.startswith("```"):
                inside = False
                text = "\n".join(body) + "\n"
                if "func main(" in text:
                    yield "%s:%d" % (os.path.relpath(page, ROOT), start), text
            elif inside:
                body.append(line)


def sources():
    files = sorted(glob.glob(os.path.join(ROOT, "tests", "**", "*.sword"),
                         recursive=True) +
                   glob.glob(os.path.join(ROOT, "std", "**", "*.sword"),
                             recursive=True))
    for path in files:
        yield os.path.relpath(path, ROOT), open(path).read()
    yield from doc_programs()


def accepts(g, text):
    try:
        toks = lex(text)
    except SyntaxError as e:
        return False, str(e)
    at = recognize(g, "SourceFile", toks)
    if at is None:
        return True, ""
    near = " ".join(t[1] for t in toks[max(0, at - 6):at + 3])
    return False, "token %d of %d, near: %s" % (at, len(toks), near)


def main():
    g = Grammar(ebnf_blocks(os.path.join(ROOT, "docs", "grammar.md")))
    missing = g.undefined()
    if missing:
        print("FAIL grammar: undefined: " + ", ".join(sorted(missing)))
        return 1
    passed = failed = 0
    for name, text in sources():
        ok, why = accepts(g, text)
        refuse = expects_syntax_error(text)
        if ok and refuse:
            print("FAIL %s: the parser refuses this and the grammar does not" % name)
            failed += 1
        elif not ok and not refuse:
            print("FAIL %s: the grammar refuses this: %s" % (name, why))
            failed += 1
        else:
            passed += 1
    print("grammar: %d files agree with the parser, %d do not" % (passed, failed))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
