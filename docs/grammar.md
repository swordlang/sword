# Grammar

This is the syntax of Sword in Extended Backus–Naur Form: what the parser
accepts, and nothing about what the checker then asks of it. A program that
fits this grammar can still be refused — a type that does not exist, a race, a
`try` in a function that cannot fail — and those rules are in the
[reference](reference.md). Where the grammar is wider than the language on
purpose, the [last section](#wider-than-the-language) says so.

## Notation

The notation is the one Go's specification uses:

```
Production  = name "=" [ Expression ] "." .
Expression  = Term { "|" Term } .
Term        = Factor { Factor } .
Factor      = name | token | Group | Option | Repetition .
Group       = "(" Expression ")" .
Option      = "[" Expression "]" .
Repetition  = "{" Expression "}" .
```

`|` is a choice, `[ ]` is zero or one, `{ }` is zero or more, and `( )` groups.
A word in quotes is written as it stands. A name in `PascalCase` is a
production of the syntax; one in `lower_case` is a token, described in prose
or in the lexical rules.

## Lexical elements

Source is UTF-8. Spaces, tabs and carriage returns separate tokens and are
otherwise ignored. A line comment runs from `//` to the end of the line; a
block comment runs from `/*` to the matching `*/`, and block comments nest.

### Statement ends

A statement ends at a `;` or at a newline, and Sword source is written without
the first. A newline counts as `;` when the token before it could be the last
token of a statement:

- an identifier, or a number, string or `true`, `false`, `nil` literal;
- `)`, `]` or `}`;
- `return`, `break` or `continue`.

Everywhere else a newline is only space. That is why an operator at the end of
a line continues the expression on the next, while an operator at the start of
one does not:

```sword
total := first +
    second            // one expression
```

The end of the file counts as a newline. In the grammar, `";"` stands for
either.

`else` is the one place a `;` does not end anything. No statement starts with
`else`, so when one follows the `}` of an `if` on the next line, it continues
that `if`:

```sword
if ready {
    start()
}
else {
    wait()
}
```

### Identifiers and keywords

```
identifier = letter { letter | digit } .
letter     = "a" … "z" | "A" … "Z" | "_" .
digit      = "0" … "9" .
```

These are keywords and cannot be identifiers:

```
break     case      catch     const     continue  default   defer
det       else      enum      errdefer  extern    false     for
func      if        import    in        interface lock      mut
nil       orelse    package   parallel  reduce    return    scope
select    spawn     struct    switch    true      try
```

`error`, `type`, `weak`, `blocking` and `read` are not keywords. Each is
special in one position only — `error` and `type` at the start of a top-level
declaration followed by a name, `weak` and `blocking` straight after `extern`,
`read` after
`lock` when a name and `:=` follow it — and is an ordinary identifier
everywhere else. That is what keeps `error.Timeout` an expression, a field or a
variable called `type` a name like any other, and `lock read := t` a lock
whose binding is called `read`.

An identifier that begins with an upper-case letter is exported from its
package; that is a rule of the checker, not of the grammar.

### Numbers

```
int_lit     = decimal | "0x" hex_digit { hex_digit | "_" }
            | "0b" bin_digit { bin_digit | "_" }
            | "0o" oct_digit { oct_digit | "_" } .
decimal     = digit { digit | "_" } .
float_lit   = decimal "." { digit | "_" } [ exponent ]
            | decimal exponent .
exponent    = ( "e" | "E" ) [ "+" | "-" ] digit { digit } .
hex_digit   = digit | "a" … "f" | "A" … "F" .
bin_digit   = "0" | "1" .
oct_digit   = "0" … "7" .
```

An underscore is only a separator: `1_000_000` is one million. A `.` begins a
fraction only when it is not followed by another `.`, so `0..n` is a range and
not the number `0.` followed by `.n`.

### Strings

```
string_lit = `"` { char | escape } `"` .
escape     = `\n` | `\t` | `\r` | `\0` | `\e` | `\\` | `\"` | `\'`
           | `\x` hex_digit hex_digit .
```

A string literal does not cross a line. `\e` is ESC, the byte every terminal
sequence begins with; `\xHH` is any byte by its value. A string holds bytes, so
a literal may hold any UTF-8 text as it is.

### Operators and punctuation

```
+    -    *    /    %    +%   -%   *%
&    |    ^    ~    <<   >>   &&   ||   !
==   !=   <    <=   >    >=
=    :=   +=   -=   *=   /=   %=   +%=  -%=  *%=
&=   |=   ^=   <<=  >>=
<-   (    )    [    ]    {    }
,    :    .    ..   ...  ?    ;
```

The longest token wins, so `a<-b` is a send and not a comparison: a comparison
with a negative number is written `a < -b`. `+%`, `-%` and `*%` are the wrapping
forms of `+`, `-` and `*`. `~` is only ever the complement, flipping every bit
of an integer; xor is `^`.

## Source files

```ebnf
SourceFile  = { ";" } { TopLevel { ";" } } .
TopLevel    = PackageDecl | ImportDecl | ConstDecl | ErrorDecl
            | FuncDecl | StructDecl | InterfaceDecl | EnumDecl | AliasDecl .

PackageDecl = "package" identifier .
ImportDecl  = "import" [ identifier ] string_lit .
ConstDecl   = "const" identifier "=" Expr .
```

`package` names the package for whoever reads the file; the directory is what
decides it. An identifier before an import's path is the name the package goes
by in this one, in place of its own.

## Declarations

```ebnf
ErrorDecl   = "error" ErrorName { "," ErrorName } .
ErrorName   = identifier [ "=" string_lit ] .

FuncDecl    = [ "det" ] "func" [ Receiver ] identifier [ TypeParams ] Params [ Type ] Block
            | "extern" [ "weak" | "blocking" ] [ string_lit ] [ "det" ] "func" [ Receiver ] identifier
              CParams [ Type ] .
Receiver    = "(" [ "mut" ] identifier Type ")" .
TypeParams  = "[" TypeParam { "," TypeParam } "]" .
TypeParam   = identifier [ ":" Type ] .
Params      = "(" [ ParamGroup { "," ParamGroup } ] ")" .
CParams     = "(" [ ParamGroup { "," ParamGroup } [ "," "..." ] ] ")" .
ParamGroup  = ParamName { "," ParamName } [ "..." ] Type .
ParamName   = [ "mut" ] identifier .

StructDecl  = [ "extern" ] "struct" identifier [ TypeParams ]
              "{" { ";" } { FieldGroup { ";" } } "}" .
FieldGroup  = identifier { "," identifier } Type .

InterfaceDecl = "interface" identifier "{" { ";" } { MethodSpec { ";" } } "}" .
MethodSpec  = [ "mut" ] identifier Params [ Type ] .

EnumDecl    = "enum" identifier [ Type ] "{" { ";" } { EnumMember [ "," ] { ";" } } "}" .
EnumMember  = identifier [ "=" Expr ] .

AliasDecl   = "type" identifier "=" Type .
```

`a, b u64` gives both names the type written after the last, as in Go, and
`args ...T` gathers the rest of the arguments into a slice. An `extern`
function is one written in C: it has no body, cannot be generic, may end its
parameters in C's own bare `...`, and may name the library it comes from in
quotes. `weak` makes it a value that may be missing at run time. `blocking`
sends each call to the io pool, putting the task down rather than the thread,
and leaves the function callable only by name. `det` is a claim the checker verifies, not a hint.

An interface method has no receiver to write `mut` on, so `mut` goes ahead of
its name instead: it says the method writes through the value it is called on,
the way `(mut c *T)` does for a method of a struct.

`type Name = T` is another name for `T`, the same type in every respect: values
pass between the two without a conversion. An alias takes no type parameters,
though it may name an instance (`type Set = collections.Map[string, bool]`).

## Types

```ebnf
Type        = TypeName [ TypeArgs ]
            | "*" Type
            | "?" Type
            | "!" Type
            | "[" "]" Type
            | "[" "*" "]" Type
            | "[" ArrayLength "]" Type
            | [ "extern" ] FuncType .
TypeName    = identifier [ "." identifier ] .
TypeArgs    = "[" Type { "," Type } "]" .
ArrayLength = int_lit | "_" | identifier [ "." identifier ] .
FuncType    = "func" "(" [ FuncParam { "," FuncParam } ] ")" [ Type ] .
FuncParam   = [ "mut" ] Type .
```

`*T` is a pointer, `?T` an optional, `!T` a result that may be an error, `[]T`
a slice, `[*]T` a raw pointer for talking to C, and `[N]T` an array. The length
of an array is a number, a constant (`[Size]u8`, `[crypto.Sha256Size]u8`), or
`_`, which takes it from the literal that follows. A function type names
types only — there is nothing for a parameter name to name — and
`extern func(...)` is a C function, called the way C calls.

`chan[T]`, `shared[T]` and `atomic[T]` are type names with arguments, like any
generic struct.

## Statements

```ebnf
Block       = "{" { ";" } { Statement { ";" } } "}" .

Statement   = Deferrable
            | ( "defer" | "errdefer" ) Deferrable
            | VarDecl .
Deferrable  = Block
            | IfStmt | SwitchStmt | SelectStmt | ForStmt
            | "parallel" ForStmt
            | "return" [ Expr ]
            | "break"
            | "continue"
            | "scope" Block
            | "spawn" Expr
            | LockStmt
            | Expr [ AssignOp Expr ] .

VarDecl     = [ "mut" ] identifier ( ":=" Expr | Type "=" Expr ) .
AssignOp    = "=" | "+=" | "-=" | "*=" | "/=" | "%=" | "+%=" | "-%=" | "*%="
            | "&=" | "|=" | "^=" | "<<=" | ">>=" .

IfStmt      = "if" ( [ [ "mut" ] identifier ":=" ] Expr Block | LockStmt )
              [ [ ";" ] "else" ( IfStmt | Block ) ] .

ForStmt     = "for" [ "mut" ] identifier ":=" Expr Block
            | "for" [ ForHead ] [ Reduction ] Block .
ForHead     = [ identifier "," ] identifier "in" Expr [ ".." Expr ]
            | Expr .
Reduction   = "reduce" "(" Reducer ":" identifier ")" .
Reducer     = identifier [ "." identifier ] | operator .

SwitchStmt  = "switch" Expr "{" { ";" } { SwitchCase { ";" } } "}" .
SwitchCase  = ( "case" Expr { "," Expr } | "default" ) ":" { ";" } { Statement { ";" } } .

SelectStmt  = "select" "{" { ";" } { SelectCase { ";" } } "}" .
SelectCase  = ( "case" CommClause | "default" ) ":" { ";" } { Statement { ";" } } .
CommClause  = identifier ":=" "<-" Expr
            | "<-" Expr
            | Expr "<-" Expr .

LockStmt    = "lock" [ "read" ] identifier ":=" Expr Block .
```

A `return` takes an expression unless the statement ends where the word does.
`for` has four shapes: forever (`for { }`), while a condition holds
(`for ok { }`), over a range or a partition (`for i in 0..n { }`,
`for off, part in xs.chunks(n) { }`), and while an optional holds a value
(`for job := queue.Next() { }`). `parallel for` is the loop over a range or a
partition with its turns spread over the scheduler, and `reduce`, over a range, names what each piece
accumulates on its own — `reduce(+: total)`, `reduce(min: best)`,
`reduce(num.Merge: acc)`; `operator` there is any operator token. There is no
fallthrough between cases, so a case's statements end where the next `case`
begins.

The expression in the head of an `if`, `for`, `switch` or `lock` cannot be a
struct literal written bare, since `if p {` would otherwise read as one; put it
in parentheses.

## Expressions

```ebnf
Expr        = OrExpr [ "<-" OrExpr ] [ Handler ] .
Handler     = "catch" ( Block | "|" identifier "|" Block | Jump | Expr )
            | "orelse" ( Block | Jump | Expr ) .
Jump        = "return" [ Expr ] | "break" | "continue" .

OrExpr      = AndExpr { "||" AndExpr } .
AndExpr     = CmpExpr { "&&" CmpExpr } .
CmpExpr     = AddExpr { ( "==" | "!=" | "<" | "<=" | ">" | ">=" ) AddExpr } .
AddExpr     = MulExpr { ( "+" | "-" | "|" | "^" | "+%" | "-%" ) MulExpr } .
MulExpr     = Unary { ( "*" | "/" | "%" | "<<" | ">>" | "&" | "*%" ) Unary } .

Unary       = "try" Unary [ "<-" OrExpr ]
            | ( "-" | "!" | "~" | "&" | "*" | "<-" ) Unary
            | Postfix .

Postfix     = Primary { Call | Selector | Index | Slice } .
Call        = "(" [ Arg { "," Arg } ] ")" .
Arg         = Expr [ "..." ] .
Selector    = "." identifier .
Index       = "[" Expr { "," Expr } "]" .
Slice       = "[" [ Expr ] ".." [ Expr ] "]" .

Primary     = int_lit | float_lit | string_lit | "true" | "false" | "nil"
            | identifier
            | StructLit
            | ArrayLit
            | Conversion
            | "(" Expr ")" .

StructLit   = ( identifier [ TypeArgs ] | identifier "." identifier )
              "{" { ";" } { FieldInit [ "," ] { ";" } }
              [ "..." "nil" [ "," ] { ";" } ] "}" .
FieldInit   = identifier ":" Expr .
ArrayLit    = ( ArrayType | FuncType ) "{" { ";" } { Expr [ "," ] { ";" } } "}" .
Conversion  = ( ArrayType | [ "extern" ] FuncType ) "(" Expr ")" .
ArrayType   = "[" [ "*" | ArrayLength ] "]" Type .
```

`...nil` comes last in a struct literal, after the fields it names or alone:
every optional field it leaves out starts as `nil`.

Every binary operator is left-associative, at the five levels Go has, loosest
first:

| Level | Operators |
|---|---|
| 1 | `\|\|` |
| 2 | `&&` |
| 3 | `==` `!=` `<` `<=` `>` `>=` |
| 4 | `+` `-` `\|` `^` `+%` `-%` |
| 5 | `*` `/` `%` `<<` `>>` `&` `*%` |

Below all of them come the send and then `catch` and `orelse`, so
`a + b catch 0` handles the whole sum, and `(m.Get(k) orelse 0) != 5` needs its
parentheses. `catch |e| { }` names the error for its block; `catch { }`
handles it without a name, and `orelse return` or `catch break` needs no
braces.

`try` binds tighter than anything else: `try f().g()` tries the whole chain
of calls on its right, and `try a + b` tries only `a`. The one exception is a
send, `try ch <- v`, where what can fail is the send and that is what `try`
covers.

A conversion to a named type is a call: `u64(x)`, `string(bytes)`. Only a type
that does not start with a name — `[]u8("text")`, `[*]u8(p)`,
`extern func(i32) i32(p)` — needs the form of its own.

## Wider than the language

A few things fit the grammar and are then refused by the checker, because
there the message can say what to write instead:

- an array literal whose type is a slice or a raw pointer (`[]i32{1, 2}`): it
  has nowhere to live, so write `[_]i32{1, 2}` and slice it;
- a function type followed by braces, which is where a closure would be
  written and is read as an array literal — Sword has no closures;
- `spawn` of anything but a call;
- an expression standing alone that does nothing with its value — `x + 1`,
  `~b`, `x == 2`: only a call, a send or a receive (bare, or under `try`,
  `catch` or `orelse`) may be a statement by itself. Separators being optional,
  this is also what keeps `c := a ~ b` from reading as `c := a` and then `~b`.

Between statements, fields, elements and members the separator is optional to
the parser: a newline is the usual one, and a comma is allowed where a list
reads better with it.
