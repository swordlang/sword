#include "parser.h"

namespace {

// Go's binary precedence, loosest to tightest.
int precedence(TokKind op) {
  switch (op) {
  case TK_OROR: return 1;
  case TK_ANDAND: return 2;
  case TK_EQ: case TK_NE: case TK_LT: case TK_LE: case TK_GT: case TK_GE:
    return 3;
  case TK_PLUS: case TK_MINUS: case TK_PIPE: case TK_CARET:
  case TK_PLUS_WRAP: case TK_MINUS_WRAP:
    return 4;
  case TK_STAR: case TK_SLASH: case TK_PERCENT: case TK_SHL: case TK_SHR:
  case TK_AMP: case TK_STAR_WRAP:
    return 5;
  default: return 0;
  }
}

bool is_assign_op(TokKind op) {
  switch (op) {
  case TK_ASSIGN: case TK_ADD_ASSIGN: case TK_SUB_ASSIGN:
  case TK_MUL_ASSIGN: case TK_DIV_ASSIGN: case TK_MOD_ASSIGN:
  case TK_ADD_WRAP_ASSIGN: case TK_SUB_WRAP_ASSIGN: case TK_MUL_WRAP_ASSIGN:
  case TK_AND_ASSIGN: case TK_OR_ASSIGN: case TK_XOR_ASSIGN:
  case TK_SHL_ASSIGN: case TK_SHR_ASSIGN:
    return true;
  default: return false;
  }
}

struct Parser {
  const std::vector<Token> &toks;
  Ast &ast;
  Node *unit;
  size_t i = 0;
  bool failed = false;
  int quiet = 0; // speculative parse: record failure without reporting it
  // `if p {` would otherwise read as a struct literal, so composite literals
  // are switched off while parsing the header of an if or for.
  int no_struct_lit = 0;

  Parser(const std::vector<Token> &t, Ast &a, Node *u)
      : toks(t), ast(a), unit(u) {}

  const Token &peek(size_t ahead = 0) const {
    size_t at = i + ahead;
    return toks[at < toks.size() ? at : toks.size() - 1];
  }
  TokKind kind(size_t ahead = 0) const { return peek(ahead).kind; }
  const Token &advance() { return toks[i < toks.size() - 1 ? i++ : i]; }

  bool match(TokKind want) {
    if (kind() != want) return false;
    advance();
    return true;
  }

  bool expect(TokKind want, const char *context) {
    if (match(want)) return true;
    fail("expected '%s' %s, found %s", tok_name(want), context,
         tok_name(kind()));
    return false;
  }

  template <class... Args> void fail(const char *fmt, Args... args) {
    if (!failed && !quiet) error(peek().pos, fmt, args...);
    failed = true;
  }

  void skip_terms() {
    while (kind() == TK_TERM) advance();
  }

  // Recovery: drop tokens until the next statement boundary so one syntax
  // error does not cascade into a wall of noise.
  void resync() {
    while (kind() != TK_EOF && kind() != TK_TERM && kind() != TK_RBRACE)
      advance();
    skip_terms();
    failed = false;
  }

  Node *make(NodeKind k, Pos pos) { return ast.make(k, pos); }

  // --- types -------------------------------------------------------------

  Node *type_expr() {
    Pos pos = peek().pos;
    if (match(TK_BANG)) {
      Node *n = make(ND_TYPE_ERR, pos);
      n->lhs = type_expr();
      return n->lhs ? n : nullptr;
    }
    if (match(TK_STAR)) {
      Node *n = make(ND_TYPE_PTR, pos);
      n->lhs = type_expr();
      return n->lhs ? n : nullptr;
    }
    if (match(TK_QUESTION)) {
      Node *n = make(ND_TYPE_OPT, pos);
      n->lhs = type_expr();
      return n->lhs ? n : nullptr;
    }
    if (kind() == TK_LBRACK) {
      advance();
      NodeKind k;
      uint64_t count = 0;

      if (match(TK_RBRACK)) {
        k = ND_TYPE_SLICE;
      } else if (kind() == TK_STAR && kind(1) == TK_RBRACK) {
        advance();
        advance();
        k = ND_TYPE_RAWPTR;
      } else if (kind() == TK_IDENT && peek().text == "_" && kind(1) == TK_RBRACK) {
        advance();
        advance();
        k = ND_TYPE_ARRAY;
        count = (uint64_t)-1; // length taken from the literal
      } else if (kind() == TK_INT) {
        count = advance().ival;
        if (!expect(TK_RBRACK, "after an array length")) return nullptr;
        k = ND_TYPE_ARRAY;
      } else if (kind() == TK_IDENT) {
        // A named constant, resolved when the type is: this package's, or
        // another's spelled the way an expression would (`crypto.Sha256Size`).
        Node *n = make(ND_TYPE_ARRAY, pos);
        n->ival = 0;
        n->name_pos = peek().pos;
        n->name = advance().text;
        if (kind() == TK_DOT && kind(1) == TK_IDENT) {
          advance();
          n->text = n->name;
          n->name_pos = peek().pos;
          n->name = advance().text;
        }
        if (!expect(TK_RBRACK, "after an array length")) return nullptr;
        n->lhs = type_expr();
        return n->lhs ? n : nullptr;
      } else {
        fail("expected an array length, ']' or '*', found %s",
             tok_name(kind()));
        return nullptr;
      }

      Node *n = make(k, pos);
      n->ival = count;
      n->lhs = type_expr();
      return n->lhs ? n : nullptr;
    }
    // `extern func(T) R` is a C function: the same shape, called the way C
    // calls, and opaque to everything the checker follows through a Sword one.
    if (kind() == TK_EXTERN && kind(1) == TK_FUNC) {
      advance();
      Node *n = type_expr();
      if (n) n->is_extern = true;
      return n;
    }
    // `func(T, mut U) R`. Parameters are types only: a name there would have
    // nothing to name.
    if (kind() == TK_FUNC) {
      advance();
      Node *n = make(ND_TYPE_FUNC, pos);
      if (!expect(TK_LPAREN, "after 'func' in a type")) return nullptr;
      if (!match(TK_RPAREN)) {
        do {
          if (kind() == TK_ELLIPSIS) {
            fail("a function type cannot end in '...'; a variadic C function"
                 " is called by its name");
            return nullptr;
          }
          bool is_mut = match(TK_MUT);
          Node *param = type_expr();
          if (!param) return nullptr;
          // `func(x i64)`: a name and then its type, which is how a function
          // written in place starts in Go or JavaScript.
          if (kind() == TK_IDENT) {
            fail("a function type lists types, not names; and if this is a "
                 "function written in place, Sword has no closures: name a "
                 "function declared at the top of a file");
            return nullptr;
          }
          param->is_mut = is_mut;
          n->kids.push_back(param);
        } while (match(TK_COMMA));
        if (!expect(TK_RPAREN, "after the parameters of a function type"))
          return nullptr;
      }
      // A result is optional, and `{` or a comma means there is none — as does
      // `(`, which is a conversion to a function type that returns nothing.
      if (kind() != TK_LBRACE && kind() != TK_COMMA && kind() != TK_RPAREN &&
          kind() != TK_LPAREN &&
          kind() != TK_TERM && kind() != TK_RBRACK && kind() != TK_ASSIGN &&
          kind() != TK_EOF) {
        n->lhs = type_expr();
        if (!n->lhs) return nullptr;
      }
      return n;
    }
    if (kind() == TK_IDENT) {
      Node *n = make(ND_TYPE_NAME, pos);
      n->name_pos = peek().pos;
      n->name = advance().text;
      if (kind() == TK_DOT && kind(1) == TK_IDENT) {
        advance();
        n->text = n->name; // the package qualifier
        n->name_pos = peek().pos;
        n->name = advance().text;
      }
      if (kind() != TK_LBRACK) return n;

      Node *inst = make(ND_TYPE_INST, pos);
      inst->lhs = n;
      advance();
      do {
        Node *arg = type_expr();
        if (!arg) return nullptr;
        inst->kids.push_back(arg);
      } while (match(TK_COMMA));
      return expect(TK_RBRACK, "after type arguments") ? inst : nullptr;
    }
    fail("expected a type, found %s", tok_name(kind()));
    return nullptr;
  }

  bool starts_type() const {
    switch (kind()) {
    case TK_IDENT: case TK_STAR: case TK_QUESTION: case TK_LBRACK:
    case TK_BANG: case TK_FUNC: case TK_EXTERN:
      return true;
    default: return false;
    }
  }

  // --- expressions -------------------------------------------------------

  Node *struct_lit(const std::string &name, Pos pos) {
    Node *n = make(ND_STRUCT_LIT, pos);
    n->name_pos = pos;
    n->name = name;
    advance(); // '{'

    int saved = no_struct_lit;
    no_struct_lit = 0;
    skip_terms();
    while (kind() != TK_RBRACE && kind() != TK_EOF) {
      Node *init = make(ND_FIELD_INIT, peek().pos);
      if (kind() != TK_IDENT) {
        fail("expected a field name");
        no_struct_lit = saved;
        return nullptr;
      }
      init->name_pos = peek().pos;
      init->name = advance().text;
      if (!expect(TK_COLON, "after a field name")) {
        no_struct_lit = saved;
        return nullptr;
      }
      init->rhs = expr();
      if (!init->rhs) {
        no_struct_lit = saved;
        return nullptr;
      }
      n->kids.push_back(init);
      if (!match(TK_COMMA)) skip_terms();
      skip_terms();
    }
    no_struct_lit = saved;
    return expect(TK_RBRACE, "to close a struct literal") ? n : nullptr;
  }

  // `[4]i32{...}` is a literal, `[*]u8(p)` is a conversion. Both start with a
  // written-out type, so they share an entry point.
  Node *array_lit() {
    Pos pos = peek().pos;
    Node *type = type_expr();
    if (!type) return nullptr;

    if (kind() == TK_LPAREN) {
      advance();
      Node *n = make(ND_CONVERT, pos);
      n->type_expr = type;
      int saved = no_struct_lit;
      no_struct_lit = 0;
      Node *value = expr();
      no_struct_lit = saved;
      if (!value) return nullptr;
      n->kids.push_back(value);
      return expect(TK_RPAREN, "after a conversion") ? n : nullptr;
    }

    Node *n = make(ND_ARRAY_LIT, pos);
    n->type_expr = type;
    if (!expect(TK_LBRACE, "after an array type")) return nullptr;

    int saved = no_struct_lit;
    no_struct_lit = 0;
    skip_terms();
    while (kind() != TK_RBRACE && kind() != TK_EOF) {
      Node *item = expr();
      if (!item) {
        no_struct_lit = saved;
        return nullptr;
      }
      n->kids.push_back(item);
      if (!match(TK_COMMA)) skip_terms();
      skip_terms();
    }
    no_struct_lit = saved;
    return expect(TK_RBRACE, "to close an array literal") ? n : nullptr;
  }

  Node *primary() {
    Pos pos = peek().pos;
    switch (kind()) {
    case TK_INT: {
      Node *n = make(ND_INT_LIT, pos);
      n->ival = advance().ival;
      return n;
    }
    case TK_FLOAT: {
      Node *n = make(ND_FLOAT_LIT, pos);
      n->fval = advance().fval;
      return n;
    }
    case TK_STRING: {
      Node *n = make(ND_STRING_LIT, pos);
      n->text = advance().text;
      return n;
    }
    case TK_NIL:
      advance();
      return make(ND_NIL_LIT, pos);
    case TK_TRUE: case TK_FALSE: {
      Node *n = make(ND_BOOL_LIT, pos);
      n->ival = advance().kind == TK_TRUE ? 1 : 0;
      return n;
    }
    case TK_LBRACK:
      return array_lit();
    // `extern func(i32) i32(p)`: a conversion, like `[*]u8(p)`.
    case TK_EXTERN: case TK_FUNC:
      return array_lit();
    case TK_IDENT: {
      std::string name = advance().text;
      if (kind() == TK_LBRACE && !no_struct_lit) return struct_lit(name, pos);

      // `pkg.Type{...}`: the qualifier is a package rather than a value, which
      // the three tokens after the name are enough to tell.
      if (kind() == TK_DOT && kind(1) == TK_IDENT && kind(2) == TK_LBRACE &&
          !no_struct_lit) {
        advance(); // '.'
        Pos at = peek().pos;
        std::string type = advance().text;
        Node *lit = struct_lit(type, at);
        if (lit) lit->text = name;
        return lit;
      }

      if (kind() == TK_LBRACK && !no_struct_lit) {
        size_t saved_i = i;
        bool saved_failed = failed;
        quiet++;
        i--; // back onto the name, so type_expr sees the whole thing
        Node *type = type_expr();
        quiet--;
        if (type && type->kind == ND_TYPE_INST && kind() == TK_LBRACE) {
          Node *lit = struct_lit(name, pos);
          if (lit) lit->type_expr = type;
          return lit;
        }
        i = saved_i;
        failed = saved_failed;
      }

      Node *n = make(ND_IDENT, pos);
      n->name_pos = pos;
      n->name = name;
      return n;
    }
    case TK_LPAREN: {
      advance();
      int saved = no_struct_lit;
      no_struct_lit = 0;
      Node *n = expr();
      no_struct_lit = saved;
      expect(TK_RPAREN, "after parenthesized expression");
      return n;
    }
    default:
      fail("expected an expression, found %s", tok_name(kind()));
      return nullptr;
    }
  }

  Node *index_or_slice(Node *base) {
    Pos pos = peek().pos;
    advance(); // '['

    int saved = no_struct_lit;
    no_struct_lit = 0;
    Node *low = kind() == TK_DOTDOT ? nullptr : expr();
    if (!low && kind() != TK_DOTDOT) {
      no_struct_lit = saved;
      return nullptr;
    }

    Node *n;
    if (match(TK_DOTDOT)) {
      n = make(ND_SLICE_EXPR, pos);
      n->lhs = base;
      n->cond = low;
      n->rhs = kind() == TK_RBRACK ? nullptr : expr();
    } else {
      n = make(ND_INDEX, pos);
      n->lhs = base;
      n->rhs = low;
      while (match(TK_COMMA)) {
        Node *more = expr();
        if (!more) { no_struct_lit = saved; return nullptr; }
        n->kids.push_back(more);
      }
    }
    no_struct_lit = saved;
    return expect(TK_RBRACK, "to close an index") ? n : nullptr;
  }

  Node *postfix() {
    Node *n = primary();
    while (n) {
      if (kind() == TK_LPAREN) {
        Node *call = make(ND_CALL, peek().pos);
        advance();
        call->lhs = n;
        int saved = no_struct_lit;
        no_struct_lit = 0;
        if (kind() != TK_RPAREN) {
          do {
            Node *arg = expr();
            if (!arg) { no_struct_lit = saved; return nullptr; }
            // `f(xs...)` hands over a list that is already gathered.
            if (match(TK_ELLIPSIS)) arg->is_variadic = true;
            call->kids.push_back(arg);
          } while (match(TK_COMMA));
        }
        no_struct_lit = saved;
        if (!expect(TK_RPAREN, "after call arguments")) return nullptr;
        n = call;
      } else if (kind() == TK_DOT) {
        Node *field = make(ND_FIELD, advance().pos);
        field->lhs = n;
        if (kind() != TK_IDENT) {
          fail("expected a field name after '.'");
          return nullptr;
        }
        field->name_pos = peek().pos;
        field->name = advance().text;
        n = field;
      } else if (kind() == TK_LBRACK) {
        n = index_or_slice(n);
      } else {
        break;
      }
    }
    return n;
  }

  Node *unary() {
    Pos pos = peek().pos;
    switch (kind()) {
    case TK_TRY: {
      Node *n = make(ND_TRY, advance().pos);
      n->lhs = unary();
      if (!n->lhs) return nullptr;
      // `try ch <- v`: what may fail is the send, so that is what `try` covers.
      // Everywhere else `try` takes the tightest thing it can, and this is the
      // one form where that would be the wrong half.
      if (kind() == TK_RECV) {
        Node *send = make(ND_SEND, advance().pos);
        send->lhs = n->lhs;
        send->rhs = binary(1);
        if (!send->rhs) return nullptr;
        n->lhs = send;
      }
      return n;
    }
    case TK_MINUS: case TK_BANG: case TK_TILDE: case TK_AMP: case TK_STAR: {
      Node *n = make(ND_UNARY, pos);
      n->op = advance().kind;
      n->lhs = unary();
      return n->lhs ? n : nullptr;
    }
    case TK_RECV: {
      Node *n = make(ND_RECV, advance().pos);
      n->lhs = unary();
      return n->lhs ? n : nullptr;
    }
    default:
      return postfix();
    }
  }

  Node *binary(int min_prec) {
    Node *lhs = unary();
    if (!lhs) return nullptr;

    while (true) {
      int prec = precedence(kind());
      if (prec < min_prec) return lhs;

      Node *n = make(ND_BINARY, peek().pos);
      n->op = advance().kind;
      n->lhs = lhs;
      n->rhs = binary(prec + 1); // all binary operators are left-associative
      if (!n->rhs) return nullptr;
      lhs = n;
    }
  }

  // `catch` and `orelse` sit below every binary operator, so `a + b catch 0`
  // catches the whole sum rather than just `b`.
  Node *expr() {
    Node *lhs = binary(1);
    if (!lhs) return nullptr;

    // `ch <- v` is an expression that can fail, which is what lets `try` cover
    // it and `catch` handle it the way they do any other call.
    if (kind() == TK_RECV) {
      Node *send = make(ND_SEND, advance().pos);
      send->lhs = lhs;
      send->rhs = binary(1);
      if (!send->rhs) return nullptr;
      lhs = send;
    }

    if (kind() != TK_CATCH && kind() != TK_ORELSE) return lhs;

    NodeKind form = kind() == TK_CATCH ? ND_CATCH : ND_ORELSE;
    Node *n = make(form, advance().pos);
    n->lhs = lhs;
    // `catch { ... }` handles the error without naming it, and a bare jump
    // needs no braces: `orelse return err` reads better than `orelse { ... }`.
    if (kind() == TK_LBRACE) {
      n->body = block();
      return n->body ? n : nullptr;
    }
    if (kind() == TK_RETURN || kind() == TK_BREAK || kind() == TK_CONTINUE) {
      n->body = statement();
      return n->body ? n : nullptr;
    }
    if (kind() == TK_PIPE && form == ND_CATCH) {
      advance();
      if (kind() != TK_IDENT) {
        fail("expected an error name inside '|...|'");
        return nullptr;
      }
      n->name_pos = peek().pos;
      n->name = advance().text;
      if (!expect(TK_PIPE, "after the error name")) return nullptr;
      n->body = block();
      return n->body ? n : nullptr;
    }
    n->rhs = expr();
    return n->rhs ? n : nullptr;
  }

  Node *header_expr() {
    no_struct_lit++;
    Node *n = expr();
    no_struct_lit--;
    return n;
  }

  // --- statements --------------------------------------------------------

  Node *block() {
    Pos pos = peek().pos;
    if (!expect(TK_LBRACE, "to open a block")) return nullptr;

    Node *n = make(ND_BLOCK, pos);
    skip_terms();
    while (kind() != TK_RBRACE && kind() != TK_EOF) {
      Node *st = statement();
      if (st) n->kids.push_back(st);
      else resync();
      skip_terms();
    }
    expect(TK_RBRACE, "to close a block");
    return n;
  }

  // `switch x { case a, b: ... default: ... }`. No fallthrough, so a case body
  // ends where the next `case` begins.
  Node *switch_stmt() {
    Node *n = make(ND_SWITCH, advance().pos);
    n->cond = header_expr();
    if (!n->cond) return nullptr;
    if (!expect(TK_LBRACE, "to open a switch")) return nullptr;

    skip_terms();
    while (kind() != TK_RBRACE && kind() != TK_EOF) {
      Node *arm = make(ND_CASE, peek().pos);
      if (match(TK_DEFAULT)) {
        // No values: this is the one that runs when nothing matched.
      } else if (match(TK_CASE)) {
        do {
          Node *value = expr();
          if (!value) return nullptr;
          arm->kids.push_back(value);
        } while (match(TK_COMMA));
      } else {
        fail("expected 'case' or 'default'");
        return nullptr;
      }
      if (!expect(TK_COLON, "after the values of a case")) return nullptr;
      arm->body = case_body();
      n->kids.push_back(arm);
      skip_terms();
    }
    expect(TK_RBRACE, "to close a switch");
    return n;
  }

  // select {
  // case v := <-jobs:   the value, or nothing when the channel closed
  // case <-quit:        the same, thrown away
  // case out <- value:  when there is room for it
  // default:           when none of them could go
  // }
  Node *select_stmt() {
    Node *n = make(ND_SELECT, advance().pos);
    if (!expect(TK_LBRACE, "to open a select")) return nullptr;

    skip_terms();
    while (kind() != TK_RBRACE && kind() != TK_EOF) {
      Node *arm = make(ND_SELECT_CASE, peek().pos);
      if (match(TK_DEFAULT)) {
        arm->form = 2;
      } else if (match(TK_CASE)) {
        // `v := <-ch` is the only shape that starts with a name and `:=`.
        if (kind() == TK_IDENT && kind(1) == TK_DEFINE) {
          arm->name_pos = peek().pos;
          arm->name = advance().text;
          advance(); // :=
          if (kind() != TK_RECV) {
            fail("a select case binds the value of a receive: 'v := <-ch'");
            return nullptr;
          }
          advance(); // <-
          arm->form = 0;
          arm->lhs = expr();
          if (!arm->lhs) return nullptr;
        } else {
          Node *first = expr();
          if (!first) return nullptr;
          if (first->kind == ND_RECV) {
            arm->form = 0;
            arm->lhs = first->lhs; // the channel, without the binding
          } else if (first->kind == ND_SEND) {
            arm->form = 1;
            arm->lhs = first->lhs;
            arm->rhs = first->rhs;
          } else {
            fail("a select case is a receive or a send, nothing else");
            return nullptr;
          }
        }
      } else {
        fail("expected 'case' or 'default'");
        return nullptr;
      }
      if (!expect(TK_COLON, "after a select case")) return nullptr;
      arm->body = case_body();
      n->kids.push_back(arm);
      skip_terms();
    }
    expect(TK_RBRACE, "to close a select");
    return n;
  }

  Node *case_body() {
    Node *n = make(ND_BLOCK, peek().pos);
    skip_terms();
    while (kind() != TK_CASE && kind() != TK_DEFAULT && kind() != TK_RBRACE &&
           kind() != TK_EOF) {
      Node *st = statement();
      if (st) n->kids.push_back(st);
      else resync();
      skip_terms();
    }
    return n;
  }

  // `lock name := shared { }`, `lock read name := shared { }`, past the word
  // `lock`. `read` is only special here, before a name and `:=`, so a binding
  // called `read` still works: `lock read := shared`.
  Node *lock_stmt(Node *n) {
    if (kind() == TK_IDENT && peek().text == "read" && kind(1) == TK_IDENT &&
        kind(2) == TK_DEFINE) {
      advance();
      n->is_read = true;
    }
    if (kind() != TK_IDENT || kind(1) != TK_DEFINE) {
      fail("expected 'lock name := shared'");
      return nullptr;
    }
    n->name_pos = peek().pos;
    n->name = advance().text;
    advance(); // ':='
    n->lhs = header_expr();
    if (!n->lhs) return nullptr;
    n->body = block();
    return n->body ? n : nullptr;
  }

  Node *if_stmt() {
    Pos at = peek().pos;
    // `if lock c := shared { } else { }` takes the guard only if nobody has it.
    if (kind(1) == TK_LOCK) {
      advance(); // 'if'
      Node *n = lock_stmt(make(ND_LOCK, advance().pos));
      if (!n) return nullptr;
      n->pos = at;
      n->is_try = true;
      if (match(TK_ELSE)) n->els = kind() == TK_IF ? if_stmt() : block();
      return n;
    }
    Node *n = make(ND_IF, advance().pos);
    if (kind() == TK_MUT && kind(1) == TK_IDENT && kind(2) == TK_DEFINE) {
      advance();
      n->is_mut = true;
    }
    if (kind() == TK_IDENT && kind(1) == TK_DEFINE) {
      n->name_pos = peek().pos;
      n->name = advance().text;
      advance(); // ':='
    }
    n->cond = header_expr();
    if (!n->cond) return nullptr;
    n->body = block();
    if (match(TK_ELSE)) n->els = kind() == TK_IF ? if_stmt() : block();
    return n;
  }

  // `for {}`, `for cond {}` and `for i in a..b {}` all land here.
  Node *for_stmt() {
    Node *n = make(ND_FOR, advance().pos);

    // `for x := optional { }` runs while there is something there, which is the
    // shape a queue is drained with.
    if (kind() == TK_MUT && kind(1) == TK_IDENT && kind(2) == TK_DEFINE) {
      advance();
      n->is_mut = true;
    }
    if (kind() == TK_IDENT && kind(1) == TK_DEFINE) {
      n->name_pos = peek().pos;
      n->name = advance().text;
      advance(); // ':='
      n->form = 2; // FOR_OPTIONAL
      n->cond = header_expr();
      if (!n->cond) return nullptr;
      n->body = block();
      return n->body ? n : nullptr;
    }

    bool named = kind() == TK_IDENT && kind(1) == TK_IN;
    // `for off, part in xs.chunks(n)` also binds where the piece starts.
    bool pair = kind() == TK_IDENT && kind(1) == TK_COMMA &&
                kind(2) == TK_IDENT && kind(3) == TK_IN;
    if (named || pair) {
      if (pair) {
        n->name_pos = peek().pos;
        n->name2 = advance().text;
        advance(); // ','
      }
      n->name_pos = peek().pos;
      n->name = advance().text;
      advance(); // 'in'
      n->lhs = header_expr();
      if (!n->lhs) return nullptr;
      // `for i in a..b` walks a range; `for part in xs.chunks(n)` walks a
      // partition. A named loop with no range is the second form.
      if (match(TK_DOTDOT)) {
        n->is_range = true;
        n->rhs = header_expr();
        if (!n->rhs) return nullptr;
      }
    } else if (kind() != TK_LBRACE) {
      n->cond = header_expr();
      if (!n->cond) return nullptr;
    }

    // `reduce(+: total)` names a variable each worker accumulates privately.
    if (match(TK_REDUCE)) {
      if (!expect(TK_LPAREN, "after 'reduce'")) return nullptr;
      // `min` and `max` are names rather than operators, and a reduction of
      // your own is the name of a function — `merge` or `num.Merge`. The
      // spelling is kept either way and the checker sorts it out.
      if (kind() == TK_IDENT) {
        n->name2 = peek().text;
        n->op = advance().kind;
        if (match(TK_DOT)) {
          if (kind() != TK_IDENT) {
            fail("expected a function name after '.'");
            return nullptr;
          }
          n->name2 += ".";
          n->name2 += advance().text;
        }
      } else {
        n->op = advance().kind;
      }
      if (!expect(TK_COLON, "after the reduction operator")) return nullptr;
      if (kind() != TK_IDENT) {
        fail("expected the name of the variable being reduced");
        return nullptr;
      }
      n->text = advance().text;
      if (!expect(TK_RPAREN, "after the reduction")) return nullptr;
    }

    n->body = block();
    return n;
  }

  Node *var_decl(bool is_mut) {
    Pos pos = peek().pos;
    Node *n = make(ND_VAR, pos);
    n->is_mut = is_mut;
    n->name_pos = peek().pos;
    n->name = advance().text; // identifier

    if (!match(TK_DEFINE)) {
      n->type_expr = type_expr();
      if (!n->type_expr) return nullptr;
      if (!expect(TK_ASSIGN, "after a declared type")) return nullptr;
    }
    n->rhs = expr();
    return n->rhs ? n : nullptr;
  }

  // `x := e` needs no lookahead, but `x T = e` does: one token is not enough
  // to tell it from the expression statement `x * y`. Try it and rewind.
  bool starts_var_decl() {
    if (kind() != TK_IDENT) return false;
    if (kind(1) == TK_DEFINE) return true;
    if (!starts_type_at(1)) return false;

    size_t saved_i = i;
    bool saved_failed = failed;
    quiet++;
    advance();
    bool ok = type_expr() != nullptr && kind() == TK_ASSIGN;
    quiet--;
    i = saved_i;
    failed = saved_failed;
    return ok;
  }

  bool starts_type_at(size_t ahead) const {
    switch (kind(ahead)) {
    case TK_IDENT: case TK_STAR: case TK_QUESTION: case TK_LBRACK:
    case TK_BANG: case TK_FUNC: case TK_EXTERN:
      return true;
    default: return false;
    }
  }

  Node *statement() {
    Pos pos = peek().pos;

    switch (kind()) {
    case TK_LBRACE: return block();
    case TK_IF: return if_stmt();
    case TK_SWITCH: return switch_stmt();
    case TK_SELECT: return select_stmt();
    case TK_FOR: return for_stmt();
    case TK_PARALLEL: {
      Pos at = advance().pos;
      if (kind() != TK_FOR) {
        fail("expected 'for' after 'parallel'");
        return nullptr;
      }
      Node *n = for_stmt();
      if (!n) return nullptr;
      n->is_parallel = true;
      n->pos = at;
      return n;
    }
    case TK_RETURN: {
      Node *n = make(ND_RETURN, advance().pos);
      if (kind() != TK_TERM && kind() != TK_RBRACE) {
        n->lhs = expr();
        if (!n->lhs) return nullptr;
      }
      return n;
    }
    case TK_BREAK:
      advance();
      return make(ND_BREAK, pos);
    case TK_CONTINUE:
      advance();
      return make(ND_CONTINUE, pos);
    case TK_SCOPE: {
      Node *n = make(ND_SCOPE, advance().pos);
      n->body = block();
      return n->body ? n : nullptr;
    }
    case TK_SPAWN: {
      Node *n = make(ND_SPAWN, advance().pos);
      n->lhs = expr();
      return n->lhs ? n : nullptr;
    }
    case TK_LOCK:
      return lock_stmt(make(ND_LOCK, advance().pos));
    case TK_DEFER: case TK_ERRDEFER: {
      Node *n = make(ND_DEFER, pos);
      n->is_errdefer = advance().kind == TK_ERRDEFER;
      // What runs on the way out has nowhere to put a name: nothing after it
      // in the block could see it.
      if (kind() == TK_MUT || starts_var_decl()) {
        fail("'%s' runs a statement on the way out, and a declaration there "
             "would bind a name nothing can use",
             n->is_errdefer ? "errdefer" : "defer");
        return nullptr;
      }
      n->body = statement();
      return n->body ? n : nullptr;
    }
    case TK_MUT:
      advance();
      if (!starts_var_decl()) {
        fail("expected 'mut name := value' or 'mut name T = value'");
        return nullptr;
      }
      return var_decl(true);
    default:
      break;
    }

    if (starts_var_decl()) return var_decl(false);

    Node *lhs = expr();
    if (!lhs) return nullptr;

    if (is_assign_op(kind())) {
      Node *n = make(ND_ASSIGN, peek().pos);
      n->op = advance().kind;
      n->lhs = lhs;
      n->rhs = expr();
      return n->rhs ? n : nullptr;
    }

    Node *n = make(ND_EXPR_STMT, pos);
    n->lhs = lhs;
    return n;
  }

  // --- declarations ------------------------------------------------------

  // `func f(a, b u64)` gives both names the type written after the last one,
  // the way Go does. `mut` stays attached to the name it precedes.
  bool named_group(std::vector<Node *> &out, NodeKind kind_of,
                   const char *what) {
    std::vector<Node *> waiting;
    bool field = kind_of == ND_FIELD_DECL;
    while (true) {
      Node *p = make(kind_of, peek().pos);
      if (field && kind() == TK_MUT) {
        fail("a field takes no 'mut': whether it can be written is decided by "
             "the binding the struct is reached through");
        return false;
      }
      p->is_mut = match(TK_MUT);
      if (kind() != TK_IDENT) {
        fail("expected %s", what);
        return false;
      }
      p->name_pos = peek().pos;
      p->name = advance().text;
      waiting.push_back(p);

      if (match(TK_COMMA)) continue;

      // `args ...T` gathers whatever is left of the argument list.
      if (field && kind() == TK_ELLIPSIS) {
        fail("a field cannot gather; '...' is for the last parameter of a "
             "function");
        return false;
      }
      bool gathers = match(TK_ELLIPSIS);
      Node *type = type_expr();
      if (!type) return false;
      if (gathers) {
        if (waiting.size() != 1) {
          fail("only one name can gather the rest of the arguments");
          return false;
        }
        waiting[0]->is_variadic = true;
      }
      for (size_t i = 0; i < waiting.size(); i++) {
        waiting[i]->type_expr = i == 0 ? type : ast.clone(type);
        out.push_back(waiting[i]);
      }
      return true;
    }
  }

  bool params_of(Node *fn, bool c_variadic_ok = false) {
    if (!expect(TK_LPAREN, "after a function name")) return false;
    if (kind() != TK_RPAREN) {
      do {
        // `extern func ioctl(fd i32, request u64, ...)` — C's own variadic,
        // which gathers nothing and is passed by the platform's rules. Sword's
        // own is `args ...T`, and gathers into a slice.
        if (kind() == TK_ELLIPSIS) {
          if (!c_variadic_ok) {
            fail("only an extern function ends in '...'; to gather the rest of"
                 " the arguments, write 'args ...T'");
            return false;
          }
          if (fn->kids.empty()) {
            fail("'...' needs a parameter before it, the way C does");
            return false;
          }
          advance();
          fn->is_c_variadic = true;
          break;
        }
        if (!named_group(fn->kids, ND_PARAM, "a parameter name")) return false;
      } while (match(TK_COMMA));
    }
    if (!expect(TK_RPAREN, "after parameters")) return false;
    if (kind() != TK_LBRACE && kind() != TK_TERM && starts_type())
      fn->type_expr = type_expr();
    return true;
  }

  // error Name
  // error Name = "what went wrong"
  // error A, B, C
  Node *error_decl() {
    Node *n = make(ND_ERROR_DECL, advance().pos); // the word `error`
    while (true) {
      if (kind() != TK_IDENT) {
        fail("expected an error name");
        return nullptr;
      }
      Node *one = make(ND_IDENT, peek().pos);
      one->name = advance().text;
      if (match(TK_ASSIGN)) {
        if (kind() != TK_STRING) {
          fail("an error's message has to be a string literal, so that it costs"
               " no allocation");
          return nullptr;
        }
        one->text = advance().text;
        one->ival = 1; // a message was given, even if it is empty
      }
      n->kids.push_back(one);
      if (!match(TK_COMMA)) break;
    }
    return n;
  }

  Node *func_decl(bool is_extern) {
    Node *n = make(ND_FUNC, advance().pos);
    n->is_extern = is_extern;

    // `func (g *Grid) at(...)` declares a method; the receiver is kept aside
    // until the checker knows which type it belongs to.
    if (kind() == TK_LPAREN) {
      advance();
      Node *recv = make(ND_PARAM, peek().pos);
      recv->is_mut = match(TK_MUT);
      if (kind() != TK_IDENT) {
        fail("expected a receiver name");
        return nullptr;
      }
      recv->name_pos = peek().pos;
      recv->name = advance().text;
      recv->type_expr = type_expr();
      if (!recv->type_expr) return nullptr;
      if (!expect(TK_RPAREN, "after the receiver")) return nullptr;
      n->lhs = recv;
    }

    if (kind() != TK_IDENT) {
      fail("expected a function name");
      return nullptr;
    }
    n->name_pos = peek().pos;
    n->name = advance().text;

    // `func sort[T: Ord](xs []T)` — a constraint is optional, and is an
    // interface name when present.
    if (kind() == TK_LBRACK) {
      advance();
      do {
        Node *tp = make(ND_PARAM, peek().pos);
        if (kind() != TK_IDENT) {
          fail("expected a type parameter name");
          return nullptr;
        }
        tp->name_pos = peek().pos;
        tp->name = advance().text;
        if (match(TK_COLON)) {
          tp->type_expr = type_expr();
          if (!tp->type_expr) return nullptr;
        }
        n->tparams.push_back(tp);
      } while (match(TK_COMMA));
      if (!expect(TK_RBRACK, "after type parameters")) return nullptr;
      if (is_extern) {
        fail("an extern function cannot be generic");
        return nullptr;
      }
    }

    if (!params_of(n, is_extern)) return nullptr;
    if (!is_extern) n->body = block();
    return n;
  }

  Node *struct_decl(bool is_extern) {
    Node *n = make(ND_STRUCT_DECL, advance().pos);
    n->is_extern = is_extern;
    if (kind() != TK_IDENT) {
      fail("expected a struct name");
      return nullptr;
    }
    n->name_pos = peek().pos;
    n->name = advance().text;
    if (kind() == TK_LBRACK) {
      advance();
      do {
        Node *tp = make(ND_PARAM, peek().pos);
        if (kind() != TK_IDENT) {
          fail("expected a type parameter name");
          return nullptr;
        }
        tp->name_pos = peek().pos;
        tp->name = advance().text;
        if (match(TK_COLON)) {
          tp->type_expr = type_expr();
          if (!tp->type_expr) return nullptr;
        }
        n->tparams.push_back(tp);
      } while (match(TK_COMMA));
      if (!expect(TK_RBRACK, "after type parameters")) return nullptr;
    }
    if (!expect(TK_LBRACE, "to open a struct body")) return nullptr;

    skip_terms();
    while (kind() != TK_RBRACE && kind() != TK_EOF) {
      if (!named_group(n->kids, ND_FIELD_DECL, "a field name")) return nullptr;
      skip_terms();
    }
    return expect(TK_RBRACE, "to close a struct body") ? n : nullptr;
  }

  // An interface body is a list of signatures: a name, parameters, and an
  // optional result. No receiver and no body, but `mut` ahead of the name says
  // the method writes through the value it is called on.
  Node *interface_decl() {
    Node *n = make(ND_INTERFACE_DECL, advance().pos);
    if (kind() != TK_IDENT) {
      fail("expected an interface name");
      return nullptr;
    }
    n->name_pos = peek().pos;
    n->name = advance().text;
    if (!expect(TK_LBRACE, "to open an interface body")) return nullptr;

    skip_terms();
    while (kind() != TK_RBRACE && kind() != TK_EOF) {
      Node *m = make(ND_FUNC, peek().pos);
      m->is_mut = match(TK_MUT);
      if (kind() != TK_IDENT) {
        fail("expected a method name");
        return nullptr;
      }
      m->name_pos = peek().pos;
      m->name = advance().text;
      if (!params_of(m)) return nullptr;
      n->kids.push_back(m);
      skip_terms();
    }
    return expect(TK_RBRACE, "to close an interface body") ? n : nullptr;
  }

  // `enum Kind u8 { None; Bool = 3; Int }`. The width is optional and the
  // values are optional: a member with no value continues from the one before.
  Node *enum_decl() {
    Node *n = make(ND_ENUM_DECL, advance().pos);
    if (kind() != TK_IDENT) {
      fail("expected an enum name");
      return nullptr;
    }
    n->name_pos = peek().pos;
    n->name = advance().text;
    if (kind() != TK_LBRACE) {
      n->type_expr = type_expr();
      if (!n->type_expr) return nullptr;
    }
    if (!expect(TK_LBRACE, "to open an enum body")) return nullptr;

    skip_terms();
    while (kind() != TK_RBRACE && kind() != TK_EOF) {
      Node *m = make(ND_ENUM_MEMBER, peek().pos);
      if (kind() != TK_IDENT) {
        fail("expected a member name");
        return nullptr;
      }
      m->name_pos = peek().pos;
      m->name = advance().text;
      if (match(TK_ASSIGN)) {
        m->rhs = expr();
        if (!m->rhs) return nullptr;
      }
      n->kids.push_back(m);
      // A comma between members is allowed but not required, the same way a
      // newline is enough everywhere else.
      match(TK_COMMA);
      skip_terms();
    }
    return expect(TK_RBRACE, "to close an enum body") ? n : nullptr;
  }

  // `type Bytes = []u8`: another name for a type that already exists, the same
  // type in every respect. `type` is a word only here, the way `error` is, so a
  // field or a variable called `type` keeps working.
  Node *alias_decl() {
    Node *n = make(ND_TYPE_ALIAS, advance().pos); // the word `type`
    n->name_pos = peek().pos;
    n->name = advance().text;
    if (kind() == TK_LBRACK) {
      fail("a type alias takes no type parameters; name an instance instead,"
           " as in 'type Set = collections.Map[string, bool]'");
      return nullptr;
    }
    if (kind() != TK_ASSIGN) {
      fail("a type alias is written 'type %s = T'; a new type is a struct or"
           " an enum", n->name.c_str());
      return nullptr;
    }
    advance();
    n->type_expr = type_expr();
    return n->type_expr ? n : nullptr;
  }

  Node *declaration() {
    // `package name` is documentation: the directory already decides the
    // package. `import "path"` is what actually pulls something in, and
    // `import name "path"` calls it something else in this package.
    if (kind() == TK_PACKAGE) {
      advance();
      if (kind() != TK_IDENT) {
        fail("expected a package name");
        return nullptr;
      }
      Node *n = make(ND_PACKAGE, peek().pos);
      n->name_pos = peek().pos;
      n->name = advance().text;
      return n;
    }
    if (kind() == TK_IMPORT) {
      Node *n = make(ND_IMPORT, advance().pos);
      if (kind() == TK_IDENT) {
        n->name_pos = peek().pos;
        n->name = advance().text;
      }
      if (kind() != TK_STRING) {
        fail("expected an import path in quotes");
        return nullptr;
      }
      n->text = advance().text;
      return n;
    }

    if (kind() == TK_CONST) {
      Node *n = make(ND_CONST_DECL, advance().pos);
      if (kind() != TK_IDENT) {
        fail("expected a constant name");
        return nullptr;
      }
      n->name_pos = peek().pos;
      n->name = advance().text;
      if (!expect(TK_ASSIGN, "after a constant name")) return nullptr;
      n->rhs = expr();
      return n->rhs ? n : nullptr;
    }

    // `error Timeout = "the peer did not answer"`, or a list of bare names.
    // `error` is not a keyword — `error.X` has always been an identifier the
    // checker intercepts — so this is recognised by shape: the word followed by
    // a name rather than by a dot.
    if (kind() == TK_IDENT && peek().text == "error" && kind(1) == TK_IDENT)
      return error_decl();
    if (kind() == TK_IDENT && peek().text == "type" && kind(1) == TK_IDENT)
      return alias_decl();

    bool is_extern = match(TK_EXTERN);
    // `extern weak "z" func f(...)` or `extern blocking "z" func f(...)`, one
    // or the other: a weak function is called through its value, and a
    // blocking one only by name. Neither word is reserved; each is special
    // only straight after `extern`.
    bool is_weak = false, is_blocking = false;
    std::string library;
    auto marked = [&](const char *word) {
      return is_extern && kind() == TK_IDENT && peek().text == word &&
             (kind(1) == TK_STRING || kind(1) == TK_FUNC || kind(1) == TK_DET ||
              (kind(1) == TK_IDENT &&
               (peek(1).text == "weak" || peek(1).text == "blocking")));
    };
    if (marked("weak") || marked("blocking")) {
      is_weak = advance().text == "weak";
      is_blocking = !is_weak;
      if (kind() == TK_IDENT &&
          (peek().text == "weak" || peek().text == "blocking")) {
        fail("an extern function is weak or blocking, not both: a weak one is"
             " called through its value, and a blocking one only by name");
        return nullptr;
      }
    }
    if (is_extern && kind() == TK_STRING) library = advance().text;
    // `det func f(...)`: a claim the compiler checks — what this answers is a
    // function of what it was given, and of nothing else.
    bool is_det = match(TK_DET);
    if (kind() == TK_FUNC) {
      Node *fn = func_decl(is_extern);
      if (fn) {
        fn->is_det = is_det;
        fn->is_weak = is_weak;
        fn->is_blocking = is_blocking;
        fn->library = library;
      }
      return fn;
    }
    if (is_weak || is_blocking || !library.empty()) {
      fail("a library, 'weak' and 'blocking' belong to an extern function; an"
           " extern struct is only a layout");
      return nullptr;
    }
    if (is_det) {
      fail("'det' belongs to a function");
      return nullptr;
    }
    if (kind() == TK_STRUCT) return struct_decl(is_extern);
    if (kind() == TK_INTERFACE && !is_extern) return interface_decl();
    if (kind() == TK_ENUM && !is_extern) return enum_decl();
    fail("expected 'func', 'struct', 'interface', 'enum', 'type' or 'const',"
         " found %s",
         tok_name(kind()));
    return nullptr;
  }

  bool run() {
    skip_terms();

    while (kind() != TK_EOF) {
      size_t before = i;
      Node *decl = declaration();
      if (decl) unit->kids.push_back(decl);
      else resync();
      skip_terms();
      if (i == before) advance();
    }

    return error_count() == 0;
  }
};

} // namespace

bool parse(const std::vector<Token> &tokens, Ast &ast, Node *unit) {
  return Parser(tokens, ast, unit).run();
}
