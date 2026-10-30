#pragma once

#include "diag.h"
#include "lexer.h"
#include "types.h"

#include <deque>
#include <string>
#include <vector>

struct Symbol;

// `reduce_kind` when the combining is a function the program wrote rather than
// one of the operators the compiler knows. Outside the RmwKind range, since it
// is not an atomic operation and never was.
enum { kReduceUser = -1 };

enum NodeKind {
  ND_UNIT,
  ND_PACKAGE,
  ND_IMPORT,
  ND_FUNC,
  ND_PARAM,
  ND_STRUCT_DECL,
  ND_INTERFACE_DECL,
  ND_ENUM_DECL,  // kids are the members, type_expr the width
  ND_ENUM_MEMBER,
  ND_TYPE_ALIAS, // `type Name = T`: name, and the type it stands for in type_expr
  ND_CONST_DECL,
  ND_ERROR_DECL, // kids name the errors; each kid's text is its message
  ND_RECV,       // `<-ch`, an expression: the next value, or nothing
  ND_SEND,       // `ch <- v`, an expression that can fail
  ND_SELECT,     // kids are the cases
  ND_SELECT_CASE, // form: 0 receive, 1 send, 2 default
  ND_FIELD_DECL,

  ND_BLOCK,
  ND_VAR,
  ND_ASSIGN,
  ND_RETURN,
  ND_IF,
  ND_FOR,
  ND_EXPR_STMT,
  ND_BREAK,
  ND_CONTINUE,
  ND_DEFER, // is_errdefer tells the two forms apart
  ND_SCOPE, // structured concurrency: joins on the way out
  ND_SPAWN,
  ND_LOCK, // lock name := shared { ... }: holds the mutex for the block
  ND_SWITCH, // kids are the cases, cond is the subject
  ND_CASE,   // kids are the values to match; none of them means `default`

  ND_INT_LIT,
  ND_FLOAT_LIT,
  ND_BOOL_LIT,
  ND_STRING_LIT,
  ND_NIL_LIT,
  ND_ARRAY_LIT,
  ND_STRUCT_LIT,
  ND_FIELD_INIT,
  ND_IDENT,
  ND_BINARY,
  ND_UNARY,
  ND_CALL,
  ND_CONVERT,   // T(x)
  ND_ERROR_LIT, // error.Name
  ND_TRY,       // try e
  ND_CATCH,     // e catch fallback  /  e catch |name| block
  ND_ORELSE,    // e orelse fallback /  e orelse { block }
  ND_FIELD,     // base.name
  ND_INDEX,     // base[index]
  ND_SLICE_EXPR, // base[low..high]

  ND_TYPE_NAME,
  ND_TYPE_PTR,
  ND_TYPE_RAWPTR,
  ND_TYPE_SLICE,
  ND_TYPE_ARRAY,
  ND_TYPE_OPT,
  ND_TYPE_ERR,  // !T
  ND_TYPE_INST, // Name[A, B]
  ND_TYPE_FUNC, // func(T, mut U) R: kids are the parameters
};

// One struct for every node, chibicc style: fields are used selectively per
// kind. A class hierarchy would buy nothing here and cost a visitor.
struct Node {
  NodeKind kind = ND_UNIT;
  Pos pos;
  // Where the node's own name sits. `pos` marks the construct, which for a
  // declaration is the keyword; the editor needs the identifier itself.
  Pos name_pos;
  Type *type = nullptr; // filled in by the checker

  std::string name;
  std::string name2; // partition loop: the offset variable
  std::string text;  // string literal contents
  uint64_t ival = 0;
  double fval = 0;
  TokKind op = TK_EOF;
  bool is_mut = false;
  bool is_range = false;
  bool is_extern = false;
  // Written by the compiler rather than by the programmer: it gets internal
  // linkage, so LLVM drops it when nothing calls it.
  bool is_hidden = false;
  bool is_errdefer = false;
  bool is_parallel = false; // ND_FOR spread across workers
  // ND_LOCK: `lock read`, one of any number of readers; and `if lock`, which
  // takes the guard only if it is free and runs `els` when it is not.
  bool is_read = false;
  bool is_try = false;
  // ND_PARAM: gathers the rest of the arguments. On a call or an argument:
  // the gathered list is being passed straight through.
  bool is_variadic = false;
  // ND_FUNC on an extern declaration: C's own variadic, which gathers
  // nothing. What the call site writes after the fixed parameters is what the
  // callee reads, by the platform's rules rather than by ours.
  bool is_c_variadic = false;
  // ND_FUNC on an extern declaration: `extern "z" func ...` names the library
  // the symbol comes from, which is what puts it on the link line. `weak` says
  // the symbol may be missing when the program starts, and makes the name a
  // `?extern func` to be checked before it is called.
  std::string library;
  bool is_weak = false;
  // ND_FUNC: `det`, a claim that what this answers depends on its arguments
  // and on nothing else — not on the schedule, the thread count, the clock or
  // an address. The compiler checks it and it changes no generated code.
  bool is_det = false;
  // ND_FUNC on an extern: `blocking`, a C call that may hold its thread for a
  // long time, made on the io pool so a scheduler thread is not the one held.
  bool is_blocking = false;
  int form = 0;    // return/call/for: which shape the checker settled on
  // ND_STRUCT_LIT: ends in `else nil`, so every optional field it does not
  // name starts out empty.
  bool fills_nil = false;
  int reduce_kind = 0;    // which fold a parallel loop combines its pieces with
  // `reduce(f: acc)`: the function that combines two partial answers. The
  // compiler knows five operators; anything else is this, and it only has to
  // be called in the right order.
  Symbol *reduce_fn = nullptr;
  int variadic_at = -1;   // ND_CALL: where the gathered arguments start
  int vtable = -1;        // set when this value is wrapped in an interface
  Type *bind_to = nullptr; // the interface it is being wrapped into

  Node *lhs = nullptr;
  Node *rhs = nullptr;
  Node *cond = nullptr;
  Node *body = nullptr;
  Node *els = nullptr;
  Node *type_expr = nullptr;

  std::vector<Node *> kids;    // unit decls, block stmts, params, call args
  std::vector<Node *> tparams; // type parameters of a generic function
  Symbol *sym = nullptr;
  Symbol *reduce_sym = nullptr; // the variable a parallel loop accumulates
  Symbol *index_sym = nullptr;  // partition loop: where the piece starts
};

struct Symbol {
  std::string name;
  Type *type = nullptr;
  bool is_mut = false;
  bool is_func = false;
  bool is_generic = false; // has type parameters; only instances are compiled
  // A named constant folds to a literal at check time; uses are replaced by
  // a copy of it, so nothing survives into the generated code.
  Node *const_value = nullptr;
  // For a partition variable: the slice it was carved out of. Two pieces of
  // the same partition never overlap, which is what lets them cross into
  // different tasks — one per turn of the loop that walks it.
  Symbol *chunk_of = nullptr;
  Pos pos;
  int slot = -1;         // IR value holding this local's address
  Node *decl = nullptr;  // for functions: the declaration, for parameter info
  // For a binding that holds a copy of another's slice or pointer: the bindings
  // whose memory it reaches, so the race checker sees one memory under two
  // names. Empty when the binding's memory is its own.
  std::vector<Symbol *> aliases;
  // For a partition variable: how many loops enclose the body of the loop
  // that made it.
  int chunk_depth = 0;
};

// Owns the nodes and the symbols they point at, so both outlive the passes
// that create them.
struct Ast {
  Node *root = nullptr;

  Node *make(NodeKind kind, Pos pos) {
    pool.emplace_back();
    Node *n = &pool.back();
    n->kind = kind;
    n->pos = pos;
    return n;
  }

  // Deep copy with every resolved annotation cleared: an instantiation is
  // checked from scratch, with its type parameters bound to real types.
  Node *clone(const Node *src) {
    if (!src) return nullptr;
    Node *copy = make(src->kind, src->pos);
    copy->name_pos = src->name_pos;
    copy->name = src->name;
    copy->name2 = src->name2;
    copy->text = src->text;
    copy->ival = src->ival;
    copy->fval = src->fval;
    copy->op = src->op;
    copy->is_mut = src->is_mut;
    copy->is_range = src->is_range;
    copy->is_extern = src->is_extern;
    copy->is_errdefer = src->is_errdefer;
    copy->is_parallel = src->is_parallel;
    copy->is_variadic = src->is_variadic;
    copy->is_c_variadic = src->is_c_variadic;
    copy->is_det = src->is_det;
    copy->is_blocking = src->is_blocking;
    copy->fills_nil = src->fills_nil;
    copy->lhs = clone(src->lhs);
    copy->rhs = clone(src->rhs);
    copy->cond = clone(src->cond);
    copy->body = clone(src->body);
    copy->els = clone(src->els);
    copy->type_expr = clone(src->type_expr);
    for (const Node *kid : src->kids) copy->kids.push_back(clone(kid));
    for (const Node *tp : src->tparams) copy->tparams.push_back(clone(tp));
    return copy;
  }

  Symbol *make_symbol(const std::string &name, Type *type, bool is_mut,
                      Pos pos) {
    symbols.push_back(Symbol{name, type, is_mut, false, false, nullptr,
                             nullptr, pos, -1, nullptr, {}});
    return &symbols.back();
  }

private:
  // Deques: stable addresses across growth, and destructors still run.
  std::deque<Node> pool;
  std::deque<Symbol> symbols;
};
