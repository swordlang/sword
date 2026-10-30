#include "check.h"

#include "ir.h"
#include "package.h"

#include <algorithm>
#include <cctype>
#include <deque>
#include <functional>
#include <map>
#include <set>

#include <unordered_map>
#include <unordered_set>

namespace {

// The interface method a call through an interface lands in, which is where
// what it may write is written down.
const Field *called_method(const Node *call) {
  const Type *owner =
      call->lhs && call->lhs->lhs ? call->lhs->lhs->type : nullptr;
  if (owner && owner->kind == TY_PTR) owner = owner->elem;
  if (!owner || !owner->is_interface || call->ival >= owner->methods.size())
    return nullptr;
  return &owner->methods[call->ival];
}

struct Checker {
  Program &prog;
  Package &pkg;
  Ast &ast;
  TypeTable &types;

  std::vector<std::unordered_map<std::string, Symbol *>> scopes;
  // Type parameters in scope while an instantiation is being checked.
  std::vector<std::pair<std::string, Type *>> bindings;
  // `type Name = T` declarations of this package, by name. Each is resolved the
  // first time it is named — a struct field may use one declared further down,
  // and one may name a struct that is — and a name met again while its own type
  // is being resolved is a cycle.
  std::unordered_map<std::string, Node *> aliases;
  std::unordered_set<std::string> resolving_aliases;
  Type *ret_type = nullptr;
  int loop_depth = 0;
  std::vector<Node *> open_scopes;
  std::vector<int> scope_loop_base;
  // The loop depth inside each `parallel for` being checked: a `break` at
  // exactly that depth would stop one piece of the range, not the loop.
  std::vector<int> parallel_bodies;

  // What a scope's tasks reach into, and how. Two accesses to the same memory
  // are a race unless both only read, or they are pieces of one partition.
  struct Access {
    Symbol *root = nullptr;  // the binding the memory hangs off
    Symbol *via = nullptr;   // the name it was reached by, when that is another
    Symbol *chunk = nullptr; // set when the value is a piece of a partition
    bool writes = false;
    // A spawn inside a loop happens once per iteration, so its accesses have
    // to be compared against themselves.
    bool repeated = false;
    // Reaching an atomic or a shared is safe however many tasks do it: the
    // one goes through single instructions, the other through a mutex.
    bool is_synced = false;
    // The parent's own uses inside the scope run one after another; they race
    // only with its tasks.
    bool by_parent = false;
    // A spawn directly inside the partition loop that made its piece, with no
    // other loop of the scope around it: each turn reaches a different piece.
    bool own_piece = false;
    // The `if` and `switch` branches the spawn sits in, outermost first.
    std::vector<std::pair<Node *, int>> arms;
    Pos at;
  };
  std::vector<std::pair<Node *, int>> arms;

  void check_arm(Node *owner, int which, Node *body) {
    arms.push_back({owner, which});
    check_stmt(body);
    arms.pop_back();
  }

  // Two branches of one `if` or `switch` never both run in the same turn.
  static bool apart(const Access &a, const Access &b) {
    for (const auto &x : a.arms)
      for (const auto &y : b.arms)
        if (x.first == y.first && x.second != y.second) return true;
    return false;
  }
  std::vector<std::vector<Access>> scope_uses;

  // The `lock` blocks that are open, innermost last, and the lock depth each
  // open `scope` was entered at.
  struct HeldLock {
    Symbol *root;
    Pos at;
    bool read;
  };
  std::vector<HeldLock> held_locks;
  std::vector<size_t> scope_lock_base;

  // Tasks borrow the frame the scope sits in, so control may not leave the
  // block while they are still running. The join is the closing brace.
  bool leaving_scope(Node *n, const char *what) {
    if (open_scopes.empty()) return false;
    const char *kind =
        open_scopes.back()->kind == ND_SCOPE ? "scope" : "parallel for";
    error(n->pos,
          "cannot %s out of a '%s': its tasks are still using this frame, and "
          "the join is the closing brace",
          what, kind);
    return true;
  }

  Checker(Program &p, Package &k, TypeTable &t)
      : prog(p), pkg(k), ast(p.ast), types(t) {}

  // --- packages ----------------------------------------------------------

  Package *imported(const std::string &name) {
    auto found = pkg.import_names.find(name);
    if (found == pkg.import_names.end()) return nullptr;
    return prog.by_path[found->second];
  }

  // A name is visible outside its package only when it starts uppercase.
  template <class Map>
  auto find_in(Map &table, const std::string &name, bool same_package)
      -> decltype(table.begin()->second) {
    auto found = table.find(name);
    if (found == table.end()) return nullptr;
    if (!same_package && !exported(name)) return nullptr;
    return found->second;
  }

  // A struct type's name carries the package it was declared in, so this is
  // where a field access finds out whether it is at home. Types the compiler
  // synthesizes — error unions, optionals, `shared[T]` — carry no package and
  // belong to whoever holds one.
  bool declared_here(const Type *t) {
    // A generic instance carries its type arguments after a '$', and those have
    // packages of their own; only the part before it says where it was declared.
    std::string name = t->name.substr(0, t->name.find('$'));
    if (name.size() <= pkg.prefix.size()) return true;
    if (name.find('.') == std::string::npos) return true;
    if (name.compare(0, pkg.prefix.size(), pkg.prefix) != 0) return false;
    // A longer path that merely starts the same is a different package.
    return name.find('.', pkg.prefix.size()) == std::string::npos;
  }

  // A field is reachable from outside its package on the same terms as a type
  // or a function: only if it starts uppercase. Without this a package could
  // publish a type and keep nothing about it to itself.
  bool field_reachable(const Type *owner, const std::string &name) {
    return exported(name) || declared_here(owner);
  }

  // Plain name: a bound type parameter wins, then the package's own types,
  // then the builtins.
  Type *lookup_type(const std::string &name) {
    for (auto it = bindings.rbegin(); it != bindings.rend(); ++it)
      if (it->first == name) return it->second;
    auto found = pkg.type_names.find(name);
    if (found != pkg.type_names.end()) return found->second;
    if (Type *aliased = alias_type(name)) return aliased;
    return types.named(name);
  }

  // What an alias stands for, resolved on first use. It is resolved where it
  // was declared, at the top of the file: the type parameters of whatever
  // generic function happened to name it first are no business of its own.
  Type *alias_type(const std::string &name) {
    auto it = aliases.find(name);
    if (it == aliases.end()) return nullptr;
    Node *decl = it->second;
    if (decl->type) return decl->type;
    if (resolving_aliases.count(name)) {
      error(decl->pos, "type alias '%s' stands for itself", name.c_str());
      decl->type = types.void_ty;
      return decl->type;
    }
    resolving_aliases.insert(name);
    std::vector<std::pair<std::string, Type *>> outer;
    outer.swap(bindings);
    Type *t = resolve(decl->type_expr);
    bindings.swap(outer);
    resolving_aliases.erase(name);
    if (!decl->type) decl->type = t;
    pkg.type_names[name] = decl->type;
    return decl->type;
  }

  // Names first, so that an alias and a struct can refer to each other in any
  // order; what each stands for is settled once the structs exist.
  bool declare_aliases() {
    for (Node *decl : pkg.unit->kids) {
      if (decl->kind != ND_TYPE_ALIAS) continue;
      if (aliases.count(decl->name) || types.named(decl->name)) {
        error(decl->pos, "'%s' is already a type", decl->name.c_str());
        return false;
      }
      aliases[decl->name] = decl;
    }
    return true;
  }

  bool settle_aliases() {
    int before = error_count();
    for (Node *decl : pkg.unit->kids)
      if (decl->kind == ND_TYPE_ALIAS) alias_type(decl->name);
    return error_count() == before;
  }

  Type *lookup_type(Node *n) {
    if (!n->text.empty()) {
      Package *other = imported(n->text);
      if (!other) {
        error(n->pos, "'%s' is not an imported package", n->text.c_str());
        return nullptr;
      }
      Type *t = find_in(other->type_names, n->name, false);
      if (!t)
        error(n->pos, "package '%s' has no exported type '%s'",
              other->name.c_str(), n->name.c_str());
      return t;
    }
    return lookup_type(n->name);
  }

  void push_scope() { scopes.emplace_back(); }
  void pop_scope() { scopes.pop_back(); }

  Symbol *declare(const std::string &name, Type *type, bool is_mut, Pos pos) {
    auto &scope = scopes.back();
    auto found = scope.find(name);
    if (found != scope.end()) {
      error(pos, "'%s' is already declared in this scope", name.c_str());
      note(found->second->pos, "previous declaration is here");
      return found->second;
    }
    return scope[name] = ast.make_symbol(name, type, is_mut, pos);
  }

  Symbol *lookup(const std::string &name) {
    for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
      auto found = it->find(name);
      if (found != it->end()) return found->second;
    }
    return nullptr;
  }

  // --- types -------------------------------------------------------------

  // Aggregates cross our own call boundary as a pointer, which is not the C
  // ABI; the FFI boundary has to stay scalar.
  void c_boundary(const Type *fn, Pos pos, const std::string &what) {
    for (const Type *p : fn->params)
      if (is_aggregate(p))
        error(pos, "%s cannot take %s; pass .ptr and .len", what.c_str(),
              type_str(p).c_str());
    if (fn->ret && is_aggregate(fn->ret))
      error(pos, "%s cannot return %s", what.c_str(),
            type_str(fn->ret).c_str());
  }

  // What follows -l, and nothing that would reach the linker as an option or a
  // path: those belong to --link, where whoever builds the program can see them.
  static bool library_name(const std::string &name) {
    if (name.empty() || name[0] == '-' || name[0] == '.') return false;
    for (char c : name)
      if (!isalnum((unsigned char)c) && c != '_' && c != '-' && c != '.' &&
          c != '+')
        return false;
    return true;
  }

  // Two packages may each declare the same C function, and agree about it.
  void c_symbol(Node *fn) {
    if (!fn->library.empty() && !library_name(fn->library)) {
      error(fn->pos,
            "\"%s\" is not a library name; write what follows -l, such as"
            " \"z\" for libz, and give paths and options to --link",
            fn->library.c_str());
      return;
    }
    auto at = prog.c_symbols.emplace(fn->name, fn);
    Node *first = at.first->second;
    if (!at.second) {
      if (first->is_weak != fn->is_weak) {
        error(fn->pos, "'%s' is weak here and not where it was first declared",
              fn->name.c_str());
        note(first->pos, "first declared here");
        return;
      }
      if (!first->library.empty() && !fn->library.empty() &&
          first->library != fn->library) {
        error(fn->pos, "'%s' comes from \"%s\" here and from \"%s\" where it"
              " was first declared", fn->name.c_str(), fn->library.c_str(),
              first->library.c_str());
        note(first->pos, "first declared here");
        return;
      }
    } else if (fn->is_weak) {
      prog.weak_symbols.push_back(fn->name);
    }
    if (!fn->library.empty() &&
        std::find(prog.libraries.begin(), prog.libraries.end(),
                  fn->library) == prog.libraries.end())
      prog.libraries.push_back(fn->library);
  }

  Type *resolve(Node *n) {
    if (!n) return types.void_ty;
    switch (n->kind) {
    case ND_TYPE_NAME: {
      Type *t = lookup_type(n);
      if (!t && n->text.empty())
        error(n->pos, "unknown type '%s'", n->name.c_str());
      return t ? t : types.void_ty;
    }
    case ND_TYPE_PTR: return types.ptr(resolve(n->lhs));
    case ND_TYPE_FUNC: {
      std::vector<Type *> params;
      std::vector<bool> param_mut;
      for (Node *p : n->kids) {
        params.push_back(resolve(p));
        param_mut.push_back(p->is_mut);
      }
      Type *t = types.func(params, resolve(n->lhs), param_mut);
      if (n->is_extern) {
        t->is_extern = true;
        c_boundary(t, n->pos, "an extern function type");
      }
      return t;
    }
    case ND_TYPE_RAWPTR: return types.rawptr(resolve(n->lhs));
    case ND_TYPE_SLICE: return types.slice(resolve(n->lhs));
    case ND_TYPE_OPT: return types.opt(resolve(n->lhs));
    case ND_TYPE_ARRAY: {
      int64_t count = (int64_t)n->ival;
      if (!n->name.empty()) {
        Symbol *sym = nullptr;
        std::string shown = n->name;
        if (!n->text.empty()) {
          Package *other = imported(n->text);
          if (!other) {
            error(n->pos, "'%s' is not an imported package", n->text.c_str());
            return types.void_ty;
          }
          sym = find_in(other->globals, n->name, false);
          if (!sym) {
            error(n->pos, "package '%s' has no exported constant '%s'",
                  other->name.c_str(), n->name.c_str());
            return types.void_ty;
          }
          shown = n->text + "." + n->name;
        } else {
          sym = lookup(n->name);
        }
        if (!sym || !sym->const_value ||
            sym->const_value->kind != ND_INT_LIT) {
          error(n->pos, "'%s' is not an integer constant", shown.c_str());
          return types.void_ty;
        }
        count = (int64_t)sym->const_value->ival;
      }
      return types.array(resolve(n->lhs), count);
    }
    case ND_TYPE_INST: {
      Node *base = n->lhs;
      if (base->text.empty() && base->name == "atomic" &&
          !pkg.generic_types.count("atomic")) {
        if (n->kids.size() != 1) {
          error(n->pos, "atomic[T] takes one type argument");
          return types.void_ty;
        }
        Type *inner = resolve(n->kids[0]);
        if (inner->kind != TY_INT && inner->kind != TY_BOOL) {
          error(n->pos, "atomic[%s] is not supported; use an integer or bool",
                type_str(inner).c_str());
          return types.void_ty;
        }
        return types.atomic(inner);
      }
      // `chan[T]` is a type of the language spelled like a generic, and the
      // generic it becomes lives in the package the compiler imported for it.
      // Rewriting the node is all it takes: everything below already knows how
      // to instantiate another package's generic struct.
      if (base->text.empty() && base->name == "chan" &&
          !pkg.generic_types.count("chan") && imported("chan")) {
        if (n->kids.size() != 1) {
          error(n->pos, "chan[T] takes one type argument");
          return types.void_ty;
        }
        base->text = "chan";
        base->name = "Chan";
      }
      if (base->text.empty() && base->name == "shared" &&
          !pkg.generic_types.count("shared")) {
        if (n->kids.size() != 1) {
          error(n->pos, "shared[T] takes one type argument");
          return types.void_ty;
        }
        Type *inner = resolve(n->kids[0]);
        if (inner->kind == TY_VOID) {
          error(n->pos, "shared[void] has nothing to protect");
          return types.void_ty;
        }
        return types.shared(inner);
      }
      Package *owner = &pkg;
      Node *decl = nullptr;
      if (!base->text.empty()) {
        owner = imported(base->text);
        if (!owner) {
          error(base->pos, "'%s' is not an imported package",
                base->text.c_str());
          return types.void_ty;
        }
        auto found = owner->generic_types.find(base->name);
        if (found != owner->generic_types.end() && exported(base->name))
          decl = found->second;
      } else {
        auto found = pkg.generic_types.find(base->name);
        if (found != pkg.generic_types.end()) decl = found->second;
      }
      if (!decl) {
        error(n->pos, "'%s' is not a generic type", base->name.c_str());
        return types.void_ty;
      }

      std::vector<Type *> args;
      for (Node *arg : n->kids) args.push_back(resolve(arg));
      Type *made = instantiate_struct(decl, owner, args, n->pos);
      return made ? made : types.void_ty;
    }

    case ND_TYPE_ERR: {
      Type *value = resolve(n->lhs);
      if (value->is_error_union) {
        error(n->pos, "'!' cannot be applied twice");
        return value;
      }
      return types.error_union(value);
    }
    default:
      error(n->pos, "expected a type");
      return types.void_ty;
    }
  }

  // An untyped literal takes the type of its context. Walking the expression
  // lets `x i64 = 1 + 2` give both literals the right width before lowering.
  void apply_type(Node *n, Type *t) {
    if (!n || !n->type || !n->type->untyped) return;
    n->type = t;
    // An untyped literal that lands on the other kind of number has to move
    // its value across with it: everything downstream reads ival or fval by
    // the node kind, not by the type.
    if (n->kind == ND_INT_LIT && is_float(t)) {
      n->kind = ND_FLOAT_LIT;
      n->fval = (double)(int64_t)n->ival;
    } else if (n->kind == ND_FLOAT_LIT && is_integer(t)) {
      n->kind = ND_INT_LIT;
      n->ival = (uint64_t)(int64_t)n->fval;
    }
    apply_type(n->lhs, t);
    apply_type(n->rhs, t);
  }

  Type *settle(Node *n) {
    if (!n || !n->type || !n->type->untyped) return n ? n->type : types.void_ty;
    if (n->type->kind == TY_INT) apply_type(n, types.int_ty);
    else if (n->type->kind == TY_FLOAT) apply_type(n, types.named("f64"));
    return n->type;
  }

  // --- places ------------------------------------------------------------

  bool is_place(Node *n) {
    switch (n->kind) {
    case ND_IDENT: return n->sym && !n->sym->is_func;
    case ND_FIELD: return n->lhs->type->kind != TY_ARRAY || n->name != "len";
    case ND_INDEX: return true;
    case ND_UNARY: return n->op == TK_STAR;
    default: return false;
    }
  }

  // Mutability belongs to the binding a place is reached through: if the root
  // was not declared `mut`, nothing reachable from it can be written.
  Symbol *place_root(Node *n) {
    switch (n->kind) {
    case ND_IDENT: return n->sym;
    case ND_FIELD: case ND_INDEX: case ND_SLICE_EXPR: return place_root(n->lhs);
    case ND_UNARY: return n->op == TK_STAR ? place_root(n->lhs) : nullptr;
    default: return nullptr;
    }
  }

  bool require_mutable(Node *n, const char *what) {
    // `&x` hands over x itself.
    if (n->kind == ND_UNARY && n->op == TK_AMP)
      return require_mutable(n->lhs, what);
    Symbol *root = place_root(n);
    if (!root || root->is_mut) return true;
    if (n->kind == ND_IDENT)
      error(n->pos, "cannot %s immutable binding '%s'", what,
            root->name.c_str());
    else
      error(n->pos, "cannot %s data reached through immutable binding '%s'",
            what, root->name.c_str());
    note(root->pos, "declare it with 'mut' to allow mutation");
    return false;
  }

  // --- constants ----------------------------------------------------------

  // A constant has to be worked out at compile time, so its initialiser is
  // folded down to a single literal. Anything that cannot be folded is not a
  // constant, and says so.
  Node *fold(Node *e) {
    if (!e) return nullptr;
    switch (e->kind) {
    case ND_INT_LIT: case ND_FLOAT_LIT: case ND_BOOL_LIT: case ND_STRING_LIT:
      check_expr(e);
      return e;

    case ND_IDENT: {
      Symbol *sym = lookup(e->name);
      if (!sym || !sym->const_value) {
        error(e->pos, "'%s' is not a constant", e->name.c_str());
        return nullptr;
      }
      Node *value = sym->const_value;
      e->kind = value->kind;
      e->ival = value->ival;
      e->fval = value->fval;
      e->text = value->text;
      e->type = value->type;
      return e;
    }

    case ND_UNARY: {
      Node *inner = fold(e->lhs);
      if (!inner) return nullptr;
      if (e->op == TK_MINUS && inner->kind == ND_INT_LIT) {
        e->kind = ND_INT_LIT;
        e->ival = (uint64_t)(-(int64_t)inner->ival);
      } else if (e->op == TK_MINUS && inner->kind == ND_FLOAT_LIT) {
        e->kind = ND_FLOAT_LIT;
        e->fval = -inner->fval;
      } else if (e->op == TK_BANG && inner->kind == ND_BOOL_LIT) {
        e->kind = ND_BOOL_LIT;
        e->ival = inner->ival ? 0 : 1;
      } else if (e->op == TK_TILDE && inner->kind == ND_INT_LIT) {
        // Every bit of the type flipped, and only those: ~u8(0) is 255.
        e->kind = ND_INT_LIT;
        e->ival = ~inner->ival;
        const Type *t = inner->type;
        if (t && t->kind == TY_INT && !t->is_signed && t->bits < 64)
          e->ival &= (uint64_t(1) << t->bits) - 1;
      } else {
        error(e->pos, "'%s' is not allowed in a constant", tok_name(e->op));
        return nullptr;
      }
      e->type = inner->type;
      return e;
    }

    case ND_BINARY: {
      Node *a = fold(e->lhs);
      Node *b = fold(e->rhs);
      if (!a || !b) return nullptr;
      return fold_binary(e, a, b);
    }

    default:
      error(e->pos, "this is not something a constant can be built from");
      return nullptr;
    }
  }

  Node *fold_binary(Node *e, Node *a, Node *b) {
    bool floating = a->kind == ND_FLOAT_LIT || b->kind == ND_FLOAT_LIT;
    if (a->kind == ND_BOOL_LIT && b->kind == ND_BOOL_LIT) {
      bool left = a->ival != 0, right = b->ival != 0;
      bool result;
      switch (e->op) {
      case TK_ANDAND: result = left && right; break;
      case TK_OROR: result = left || right; break;
      case TK_EQ: result = left == right; break;
      case TK_NE: result = left != right; break;
      default:
        error(e->pos, "'%s' is not allowed between bools", tok_name(e->op));
        return nullptr;
      }
      e->kind = ND_BOOL_LIT;
      e->ival = result ? 1 : 0;
      e->type = types.bool_ty;
      return e;
    }

    // Two constant strings join at compile time, into the binary's read-only
    // data like any literal. Nothing is allocated, which is the only reason
    // `+` is allowed on strings here and nowhere else.
    if (a->kind == ND_STRING_LIT && b->kind == ND_STRING_LIT) {
      switch (e->op) {
      case TK_PLUS:
        e->kind = ND_STRING_LIT;
        e->text = a->text + b->text;
        e->type = a->type;
        return e;
      case TK_EQ: case TK_NE:
        e->kind = ND_BOOL_LIT;
        e->ival = (a->text == b->text) == (e->op == TK_EQ) ? 1 : 0;
        e->type = types.bool_ty;
        return e;
      default:
        error(e->pos, "'%s' is not allowed between strings", tok_name(e->op));
        return nullptr;
      }
    }

    bool numeric = (a->kind == ND_INT_LIT || a->kind == ND_FLOAT_LIT) &&
                   (b->kind == ND_INT_LIT || b->kind == ND_FLOAT_LIT);
    if (!numeric) {
      error(e->pos, "'%s' is not allowed in a constant", tok_name(e->op));
      return nullptr;
    }

    double left = a->kind == ND_FLOAT_LIT ? a->fval : (double)(int64_t)a->ival;
    double right = b->kind == ND_FLOAT_LIT ? b->fval : (double)(int64_t)b->ival;
    int64_t li = (int64_t)a->ival, ri = (int64_t)b->ival;

    switch (e->op) {
    case TK_EQ: case TK_NE: case TK_LT: case TK_LE: case TK_GT: case TK_GE: {
      bool result = e->op == TK_EQ   ? left == right
                    : e->op == TK_NE ? left != right
                    : e->op == TK_LT ? left < right
                    : e->op == TK_LE ? left <= right
                    : e->op == TK_GT ? left > right
                                     : left >= right;
      e->kind = ND_BOOL_LIT;
      e->ival = result ? 1 : 0;
      e->type = types.bool_ty;
      return e;
    }
    default:
      break;
    }

    if ((e->op == TK_SLASH || e->op == TK_PERCENT) && !floating && ri == 0) {
      error(e->pos, "this constant divides by zero");
      return nullptr;
    }
    if ((e->op == TK_SLASH || e->op == TK_PERCENT) && !floating &&
        li == INT64_MIN && ri == -1) {
      error(e->pos, "this constant overflows");
      return nullptr;
    }
    if ((e->op == TK_SHL || e->op == TK_SHR) && (ri < 0 || ri > 63)) {
      error(e->pos, "this constant shifts by %lld, outside 0..63",
            (long long)ri);
      return nullptr;
    }

    if (floating) {
      double result;
      switch (e->op) {
      case TK_PLUS: result = left + right; break;
      case TK_MINUS: result = left - right; break;
      case TK_STAR: result = left * right; break;
      case TK_SLASH: result = left / right; break;
      default:
        error(e->pos, "'%s' is not allowed between floats", tok_name(e->op));
        return nullptr;
      }
      e->kind = ND_FLOAT_LIT;
      e->fval = result;
      e->type = types.untyped_float;
      return e;
    }

    int64_t result;
    switch (e->op) {
    case TK_PLUS: result = li + ri; break;
    case TK_MINUS: result = li - ri; break;
    case TK_STAR: result = li * ri; break;
    case TK_SLASH: result = li / ri; break;
    case TK_PERCENT: result = li % ri; break;
    case TK_AMP: result = li & ri; break;
    case TK_PIPE: result = li | ri; break;
    case TK_CARET: result = li ^ ri; break;
    case TK_SHL: result = (int64_t)((uint64_t)li << ri); break;
    case TK_SHR: result = li >> ri; break;
    default:
      error(e->pos, "'%s' is not allowed in a constant", tok_name(e->op));
      return nullptr;
    }
    e->kind = ND_INT_LIT;
    e->ival = (uint64_t)result;
    e->type = types.untyped_int;
    return e;
  }

  // Declaration order does not matter, so a constant that refers to one
  // declared later is resolved on demand, with a marker to catch cycles.
  void declare_constants() {
    std::unordered_map<std::string, Node *> pending;
    for (Node *decl : pkg.unit->kids)
      if (decl->kind == ND_CONST_DECL) pending[decl->name] = decl;

    for (Node *decl : pkg.unit->kids) {
      if (decl->kind != ND_CONST_DECL || decl->sym) continue;
      resolve_constant(decl, pending);
    }
  }

  void resolve_constant(Node *decl,
                        std::unordered_map<std::string, Node *> &pending) {
    if (decl->sym) return;
    if (decl->is_extern) { // borrowed as the "being resolved" marker
      error(decl->pos, "constant '%s' is defined in terms of itself",
            decl->name.c_str());
      return;
    }
    decl->is_extern = true;

    // Anything this one mentions has to exist first.
    std::function<void(Node *)> ahead = [&](Node *e) {
      if (!e) return;
      if (e->kind == ND_IDENT) {
        auto found = pending.find(e->name);
        if (found != pending.end()) resolve_constant(found->second, pending);
      }
      ahead(e->lhs);
      ahead(e->rhs);
    };
    ahead(decl->rhs);

    decl->is_extern = false;
    Node *value = fold(decl->rhs);
    if (!value) return;

    Symbol *sym = declare(decl->name, value->type, false, decl->pos);
    sym->const_value = value;
    decl->sym = sym;
    pkg.globals[decl->name] = sym;
  }

  // --- sharing ------------------------------------------------------------

  // Copying a value is only safe when the copy owns everything it reaches. A
  // slice header copies, but its elements stay behind.
  bool shares_memory(const Type *t, int depth = 0) {
    if (!t || depth > 8) return false;
    switch (t->kind) {
    case TY_SLICE: case TY_PTR: case TY_RAWPTR:
      return true;
    case TY_STRING:
      return false; // immutable by definition
    case TY_ARRAY: case TY_OPT:
      return shares_memory(t->elem, depth + 1);
    case TY_STRUCT:
      if (t->is_interface) return true;
      for (const Field &f : t->fields)
        if (shares_memory(f.type, depth + 1)) return true;
      return false;
    default:
      return false;
    }
  }

  // Where shared memory comes from. Unlike place_root this also follows `&`
  // and pointer conversions: `&counter` reaches the same memory as `counter`.
  Symbol *share_root(Node *n) {
    if (!n) return nullptr;
    switch (n->kind) {
    case ND_IDENT:
      return n->sym;
    case ND_FIELD: case ND_INDEX: case ND_SLICE_EXPR:
      return share_root(n->lhs);
    case ND_UNARY:
      return n->op == TK_STAR || n->op == TK_AMP ? share_root(n->lhs)
                                                 : nullptr;
    case ND_CONVERT:
      return n->kids.empty() ? nullptr : share_root(n->kids[0]);
    default:
      return nullptr;
    }
  }

  // The bindings whose memory a binding's value reaches: its own, or, for a
  // copy of another's slice or pointer, that other's.
  static void canonical(Symbol *s, std::vector<Symbol *> &out) {
    const std::vector<Symbol *> &from = s->aliases;
    for (Symbol *r : from.empty() ? std::vector<Symbol *>{s} : from)
      if (std::find(out.begin(), out.end(), r) == out.end()) out.push_back(r);
  }

  void roots_of(Node *value, std::vector<Symbol *> &out) {
    if (!value) return;
    switch (value->kind) {
    case ND_STRUCT_LIT:
      for (Node *init : value->kids)
        if (init->kind == ND_FIELD_INIT && init->rhs && init->rhs->type &&
            shares_memory(init->rhs->type))
          roots_of(init->rhs, out);
      return;
    case ND_ARRAY_LIT:
      for (Node *kid : value->kids)
        if (kid->type && shares_memory(kid->type)) roots_of(kid, out);
      return;
    case ND_TRY:
      roots_of(value->lhs, out);
      return;
    case ND_ORELSE: case ND_CATCH:
      roots_of(value->lhs, out);
      roots_of(value->rhs, out);
      return;
    default:
      if (Symbol *root = share_root(value))
        if (!root->is_func) canonical(root, out);
    }
  }

  // `ys := xs`, `p := &n`, `h := H{xs: xs}`: the new binding reaches memory
  // another one names, and a task given one is touching the other. A binding
  // reassigned keeps every memory it was ever given, its own included. A call
  // answers with something of its own and is not followed.
  void alias(Symbol *sym, Node *value, bool declared) {
    if (!sym || !sym->type || !shares_memory(sym->type)) return;
    std::vector<Symbol *> roots;
    roots_of(value, roots);
    roots.erase(std::remove(roots.begin(), roots.end(), sym), roots.end());
    if (roots.empty()) return;
    if (sym->aliases.empty() && !declared) sym->aliases.push_back(sym);
    for (Symbol *r : roots)
      if (std::find(sym->aliases.begin(), sym->aliases.end(), r) ==
          sym->aliases.end())
        sym->aliases.push_back(r);
  }

  std::vector<Access> access_of(Node *value, bool writes) {
    Access use;
    use.writes = writes;
    use.at = value->pos;
    Symbol *root = share_root(value);
    if (!root) return {use};
    const Type *reached = root->type;
    while (reached && (reached->kind == TY_PTR || reached->kind == TY_RAWPTR))
      reached = reached->elem;
    use.is_synced = ::is_atomic(reached) || ::is_shared(reached);
    if (root->chunk_of) {
      use.chunk = root;
      root = root->chunk_of;
    }
    std::vector<Symbol *> roots;
    canonical(root, roots);
    std::vector<Access> uses;
    for (Symbol *r : roots) {
      use.root = r;
      use.via = use.chunk ? use.chunk : r != root ? root : nullptr;
      uses.push_back(use);
    }
    return uses;
  }

  static bool clashes(const Access &a, const Access &b) {
    if (!a.root || a.root != b.root) return false;
    if (a.is_synced || b.is_synced) return false;
    if (!a.writes && !b.writes) return false;
    if (a.by_parent && b.by_parent) return false;
    // A piece is one memory within a turn of its partition loop, so two tasks
    // given it then overlap, unless only one of them can be started.
    if (a.chunk && a.chunk == b.chunk && apart(a, b)) return false;
    return true;
  }

  // What to write instead when one memory goes to every turn's task: pieces
  // for something that can be cut, and otherwise a type that says it is
  // shared.
  void sharing_note(Symbol *root) {
    const char *name = root->name.c_str();
    const Type *t = root->type;
    while (t && (t->kind == TY_PTR || t->kind == TY_RAWPTR)) t = t->elem;
    if (!t) return;
    if (t->kind == TY_SLICE)
      note(root->pos,
           "walk it with 'for part in %s.chunks(n)' to give each task a piece "
           "of its own",
           name);
    else if (t->kind == TY_ARRAY)
      note(root->pos,
           "walk it with 'for part in %s[..].chunks(n)' to give each task a "
           "piece of its own",
           name);
    else if (t->kind == TY_INT || t->kind == TY_BOOL)
      note(root->pos, "to let every task change it, make it 'atomic[%s]'",
           type_str(t).c_str());
    else
      note(root->pos,
           "to let every task change it, make it 'shared[%s]' and reach it "
           "with 'lock'",
           type_str(t).c_str());
  }

  void record_use(const Access &use) {
    if (!use.root) return; // memory with no name behind it: nothing to compare

    if (use.repeated && !use.own_piece && clashes(use, use)) {
      error(use.at,
            "every turn of this loop hands the same memory to a new task, so "
            "the tasks would overlap in '%s'",
            use.root->name.c_str());
      if (use.chunk)
        note(use.chunk->pos,
             "'%s' is a piece of its own on each turn of this loop, and of no "
             "other",
             use.chunk->name.c_str());
      else
        sharing_note(use.root);
      return;
    }

    for (auto &level : scope_uses) {
      for (const Access &other : level) {
        if (!clashes(use, other)) continue;
        const char *mine = use.writes ? "writes" : "reads";
        const char *theirs = other.writes ? "written" : "read";
        std::string through =
            use.via ? std::string(", through '") + use.via->name + "',"
                    : std::string();
        error(use.at, "this %s '%s'%s while it is already %s concurrently",
              mine, use.root->name.c_str(), through.c_str(), theirs);
        note(other.at, "the other use is here");
        if (!use.chunk && use.root->type &&
            use.root->type->kind == TY_SLICE)
          note(use.root->pos,
               "to give tasks separate pieces, walk it with "
               "'for part in %s.chunks(n)'",
               use.root->name.c_str());
        return;
      }
    }
    if (!scope_uses.empty()) scope_uses.back().push_back(use);
  }

  void walk(Node *n, const std::function<void(Node *)> &visit) {
    if (!n) return;
    visit(n);
    // A spawned call is the task's business, and a partition header names the
    // slice it is about to split; neither is a use by the parent.
    if (n->kind == ND_SPAWN) return;
    bool partition = n->kind == ND_FOR && !n->is_range && !n->name.empty();
    for (Node *kid : n->kids) walk(kid, visit);
    if (!partition) {
      walk(n->lhs, visit);
      walk(n->rhs, visit);
    }
    walk(n->cond, visit);
    walk(n->body, visit);
    walk(n->els, visit);
  }

  // Which occurrences of a name sit under `base[i]`, with `i` the loop
  // variable. Those are the ones no other iteration can reach.
  void collect_indexed(Node *body, Symbol *index, std::set<Node *> &covered) {
    walk(body, [&](Node *e) {
      if (e->kind != ND_INDEX) return;
      if (!e->rhs || e->rhs->kind != ND_IDENT || e->rhs->sym != index) return;
      Node *base = e->lhs;
      while (base && base->kind != ND_IDENT) base = base->lhs;
      if (base) covered.insert(base);
    });
  }

  // Iterations run on different workers at the same time, so whatever the
  // body writes has to be reachable by exactly one of them.
  void check_parallel_body(Node *loop) {
    std::set<Symbol *> local;
    local.insert(loop->sym);
    walk(loop->body, [&](Node *n) {
      if (n->sym && (n->kind == ND_VAR || n->kind == ND_CATCH ||
                     (n->kind == ND_FOR && !n->name.empty()) ||
                     (n->kind == ND_IF && !n->name.empty())))
        local.insert(n->sym);
      if (n->index_sym) local.insert(n->index_sym);
    });

    if (loop->reduce_sym)
      walk(loop->body, [&](Node *n) {
        if (n->kind == ND_VAR && n->name == loop->text)
          error(n->pos,
                "'%s' is the reduction variable; declaring it again here means "
                "the loop accumulates into something nobody reads",
                n->name.c_str());
      });

    std::set<Node *> covered;
    collect_indexed(loop->body, loop->sym, covered);

    std::set<Symbol *> written;
    // `place` is written once per iteration; `how` names the call it was
    // written through, when it was not written by an assignment.
    auto note_write = [&](Node *place, const char *how) {
      Symbol *root = share_root(place);
      if (!root || local.count(root) || root == loop->reduce_sym) return;
      Node *base = place;
      while (base && base->kind != ND_IDENT) base = base->lhs;
      if (!base || !covered.count(base)) {
        bool indexable = root->type && (root->type->kind == TY_SLICE ||
                                        root->type->kind == TY_ARRAY);
        std::string through = how ? std::string(" through '") + how + "'" : "";
        if (indexable)
          error(place->pos,
                "every iteration would write '%s'%s; a 'parallel for' body may "
                "only write '%s[%s]'",
                root->name.c_str(), through.c_str(), root->name.c_str(),
                loop->name.c_str());
        else
          error(place->pos,
                "every iteration would write '%s'%s; to accumulate into it, "
                "write 'reduce(+: %s)' on the loop",
                root->name.c_str(), through.c_str(), root->name.c_str());
        return;
      }
      written.insert(root);
    };

    walk(loop->body, [&](Node *n) {
      if (n->kind == ND_ASSIGN) {
        note_write(n->lhs, nullptr);
        return;
      }
      // A write does not have to look like one. `c.Add(1)` and `bump(&c)`
      // reach the same memory as `c.n += 1`, and every iteration would do it;
      // only the shape is different. What the callee may write is in its
      // signature, which is the same thing `mut` on a parameter means
      // everywhere else.
      if (n->kind != ND_CALL) return;
      Type *sig = n->sym ? n->sym->type : nullptr;
      if (!sig && n->lhs && n->lhs->type && n->lhs->type->kind == TY_FUNC)
        sig = n->lhs->type;
      const Field *dynamic = n->form == CALL_DYNAMIC ? called_method(n)
                                                     : nullptr;
      if (dynamic) sig = dynamic->type;
      if (!sig) return;
      const char *how = n->lhs && !n->lhs->name.empty() ? n->lhs->name.c_str()
                                                        : nullptr;
      size_t skip = 0;
      if (dynamic && dynamic->writes) note_write(n->lhs->lhs, how);
      if (n->form == CALL_METHOD) {
        skip = 1;
        if (!sig->param_mut.empty() && sig->param_mut[0] && n->lhs &&
            n->lhs->lhs)
          note_write(n->lhs->lhs, how);
      }
      for (size_t i = 0; i < n->kids.size(); i++) {
        size_t at = i + skip;
        if (at < sig->param_mut.size() && sig->param_mut[at])
          note_write(n->kids[i], how);
      }
    });

    // Once a name is written by the loop, every other mention of it has to be
    // indexed too, or one iteration would read what another is writing.
    walk(loop->body, [&](Node *n) {
      if (n->kind != ND_IDENT || !n->sym || !written.count(n->sym)) return;
      if (covered.count(n)) return;
      error(n->pos,
            "'%s' is written by this loop, so it can only be reached as "
            "'%s[%s]'",
            n->sym->name.c_str(), n->sym->name.c_str(), loop->name.c_str());
    });
  }

  // Everything the parent itself touches inside the block races with the
  // tasks it started, because the join is at the closing brace.
  void check_parent_uses(Node *body) {
    std::set<Node *> written;
    // `part.len` reads the parent's own copy of the piece's header, not the
    // memory its task writes.
    std::set<Node *> header;
    walk(body, [&](Node *n) {
      if (n->kind == ND_ASSIGN) written.insert(n->lhs);
      if (n->kind == ND_FIELD && n->name == "len" && n->lhs &&
          n->lhs->kind == ND_IDENT && n->lhs->sym && n->lhs->sym->chunk_of)
        header.insert(n->lhs);
    });
    auto parent = [&](Node *place, bool writes) {
      for (Access use : access_of(place, writes)) {
        use.by_parent = true;
        record_use(use);
      }
    };
    walk(body, [&](Node *n) {
      if (n->kind == ND_ASSIGN)
        parent(n->lhs, true);
      else if (n->kind == ND_IDENT && n->sym && !n->sym->is_func &&
               !written.count(n) && !header.count(n))
        parent(n, false);
    });
  }

  // --- generics -----------------------------------------------------------

  // Instance names have to survive into the object file, so the type is
  // flattened to something an identifier can hold.
  static std::string mangle(const Type *t) {
    std::string text = type_str(t);
    std::string out;
    for (char c : text) {
      if (isalnum((unsigned char)c) || c == '_') out += c;
      else if (!out.empty() && out.back() != '.') out += '.';
    }
    return out.empty() ? "t" : out;
  }

  bool is_type_param(Node *decl, const std::string &name) {
    for (Node *tp : decl->tparams)
      if (tp->name == name) return true;
    return false;
  }

  // Matches a written parameter type against the type actually passed,
  // binding type parameters as it goes.
  bool unify(Node *decl, Node *pattern, Type *actual,
             std::vector<std::pair<std::string, Type *>> &bind) {
    if (!pattern || !actual) return false;
    switch (pattern->kind) {
    case ND_TYPE_NAME: {
      if (!pattern->text.empty() || !is_type_param(decl, pattern->name))
        return true; // a concrete type; the ordinary argument check covers it
      for (auto &entry : bind)
        if (entry.first == pattern->name) return type_eq(entry.second, actual);
      bind.push_back({pattern->name, actual});
      return true;
    }
    case ND_TYPE_PTR:
      return actual->kind == TY_PTR && unify(decl, pattern->lhs, actual->elem, bind);
    case ND_TYPE_RAWPTR:
      return actual->kind == TY_RAWPTR && unify(decl, pattern->lhs, actual->elem, bind);
    case ND_TYPE_SLICE:
      return actual->kind == TY_SLICE && unify(decl, pattern->lhs, actual->elem, bind);
    case ND_TYPE_ARRAY:
      return actual->kind == TY_ARRAY && unify(decl, pattern->lhs, actual->elem, bind);
    case ND_TYPE_OPT:
      return is_optional(actual) &&
             unify(decl, pattern->lhs, const_cast<Type *>(opt_payload(actual)), bind);
    case ND_TYPE_ERR:
      return actual->is_error_union &&
             unify(decl, pattern->lhs, actual->elem, bind);
    default:
      return true;
    }
  }

  bool satisfies(Type *arg, Type *constraint, Pos at) {
    if (!constraint->is_interface) {
      error(at, "a constraint must be an interface, got %s",
            type_str(constraint).c_str());
      return false;
    }
    Type *owner = arg->kind == TY_PTR ? arg->elem : arg;
    for (const Field &want : constraint->methods) {
      if (find_method(owner, want.name)) continue;
      error(at, "%s does not satisfy %s: no method '%s'",
            type_str(arg).c_str(), shown_name(constraint->name).c_str(),
            want.name.c_str());
      return false;
    }
    return true;
  }

  Symbol *instantiate(Node *generic, Package *owner,
                      const std::vector<Type *> &args, Pos at) {
    if (args.size() != generic->tparams.size()) {
      error(at, "'%s' takes %zu type argument%s, got %zu",
            generic->name.c_str(), generic->tparams.size(),
            generic->tparams.size() == 1 ? "" : "s", args.size());
      return nullptr;
    }

    std::string key = generic->name;
    for (Type *a : args) key += "$" + mangle(a);
    auto found = prog.instances.find(key);
    if (found != prog.instances.end()) return found->second;

    // A generic that calls itself on an ever-larger type would instantiate
    // forever; stop with a diagnostic instead of hanging.
    if (prog.instances.size() >= 4096) {
      error(at, "'%s' keeps instantiating new types; the recursion has no base "
                "case", generic->name.c_str());
      return nullptr;
    }

    std::vector<std::pair<std::string, Type *>> bind;
    for (size_t i = 0; i < args.size(); i++)
      bind.push_back({generic->tparams[i]->name, args[i]});

    // The signature is written in the package that declares the generic, so
    // it has to be resolved with that package's names in view, not the
    // caller's.
    Checker sub(prog, *owner, types);
    sub.see_package();
    sub.bindings = bind;

    for (size_t i = 0; i < args.size(); i++) {
      Node *tp = generic->tparams[i];
      if (!tp->type_expr) continue;
      if (!satisfies(args[i], sub.resolve(tp->type_expr), at)) return nullptr;
    }

    Node *copy = ast.clone(generic);
    copy->tparams.clear();
    copy->name = key;

    std::vector<Type *> params;
    for (Node *p : copy->kids) {
      p->type = sub.resolve(p->type_expr);
      params.push_back(p->type);
    }
    Type *ret = copy->type_expr ? sub.resolve(copy->type_expr) : types.void_ty;

    Symbol *sym = ast.make_symbol(key, types.func(params, ret), false,
                                  generic->pos);
    sym->is_func = true;
    sym->decl = copy;
    copy->sym = sym;

    prog.instances[key] = sym;
    owner->instances.push_back(copy);
    prog.pending.push_back(Program::Instance{owner, copy, bind});
    return sym;
  }

  // A generic struct becomes a real type only when it is given arguments. The
  // type is registered before its fields are resolved, so a field that points
  // back at it does not recurse forever.
  Type *instantiate_struct(Node *decl, Package *owner,
                           const std::vector<Type *> &args, Pos at) {
    if (args.size() != decl->tparams.size()) {
      error(at, "'%s' takes %zu type argument%s, got %zu", decl->name.c_str(),
            decl->tparams.size(), decl->tparams.size() == 1 ? "" : "s",
            args.size());
      return nullptr;
    }

    std::string key = owner->prefix + decl->name;
    for (Type *a : args) key += "$" + mangle(a);
    auto found = prog.struct_instances.find(key);
    if (found != prog.struct_instances.end()) return found->second;

    std::vector<std::pair<std::string, Type *>> bind;
    for (size_t i = 0; i < args.size(); i++)
      bind.push_back({decl->tparams[i]->name, args[i]});

    Type *type = types.declare_struct(key);
    type->is_extern = decl->is_extern;
    prog.struct_instances[key] = type;
    types.note_instance(type);

    Checker sub(prog, *owner, types);
    sub.see_package();
    sub.bindings = bind;

    std::vector<Field> fields;
    for (Node *f : decl->kids)
      fields.push_back(Field{f->name, sub.resolve(f->type_expr), 0, 0});
    types.layout_struct(type, std::move(fields));

    instantiate_methods(decl, owner, key, type, bind);
    return type;
  }

  void instantiate_methods(Node *decl, Package *owner, const std::string &key,
                           Type *type,
                           std::vector<std::pair<std::string, Type *>> bind) {
    auto templates = owner->generic_methods.find(decl->name);
    if (templates == owner->generic_methods.end()) return;

    for (Node *method : templates->second) {
      Node *copy = ast.clone(method);
      std::string plain = copy->name;
      copy->name = key + "." + plain;
      copy->kids.insert(copy->kids.begin(), copy->lhs);

      Checker sub(prog, *owner, types);
      sub.see_package();
      sub.bindings = bind;
      std::vector<Type *> params;
      for (Node *p : copy->kids) {
        p->type = sub.resolve(p->type_expr);
        params.push_back(p->type);
      }
      Type *ret = copy->type_expr ? sub.resolve(copy->type_expr)
                                  : types.void_ty;

      Symbol *sym = ast.make_symbol(copy->name, types.func(params, ret), false,
                                    method->pos);
      sym->is_func = true;
      sym->decl = copy;
      copy->sym = sym;

      prog.methods[type->name][plain] = sym;
      owner->instances.push_back(copy);
      prog.pending.push_back(Program::Instance{owner, copy, bind});
    }
  }

  // `sizeof[T]()` and `alignof[T]()` are the two things a generic allocator
  // cannot compute for itself.
  bool builtin_size(Node *n, const std::string &name,
                    const std::vector<Type *> &args) {
    if (name != "sizeof" && name != "alignof") return false;
    if (args.size() != 1 || !n->kids.empty()) {
      error(n->pos, "'%s' takes one type argument and no values",
            name.c_str());
      return true;
    }
    n->kind = ND_INT_LIT;
    n->ival = name == "sizeof" ? (uint64_t)size_of(args[0])
                               : (uint64_t)align_of(args[0]);
    n->type = types.untyped_int;
    return true;
  }

  // --- interfaces ---------------------------------------------------------

  // A `*T` becomes an interface value by pairing the pointer with a vtable
  // built for that exact (type, interface) combination. Only pointers qualify:
  // boxing a value would mean allocating, and nothing allocates implicitly.
  bool bind_interface(Node *value, Type *iface) {
    Type *from = value->type;
    if (!from || from->kind != TY_PTR || from->elem->kind != TY_STRUCT ||
        from->elem->is_interface) {
      note(value->pos, "%s is satisfied by a pointer to a struct",
           iface->name.c_str());
      return false;
    }
    Type *owner = from->elem;

    std::vector<std::string> entries;
    for (const Field &want : iface->methods) {
      Symbol *have = find_method(owner, want.name);
      if (!have) {
        note(value->pos, "%s has no method '%s'", owner->name.c_str(),
             want.name.c_str());
        return false;
      }
      Type *got = have->type;
      bool same = type_eq(got->ret, want.type->ret) &&
                  got->params.size() == want.type->params.size() + 1;
      for (size_t i = 0; same && i < want.type->params.size(); i++)
        same = type_eq(got->params[i + 1], want.type->params[i]);
      if (!same) {
        note(value->pos, "%s.%s is %s, but %s wants %s",
             owner->name.c_str(), want.name.c_str(), type_str(got).c_str(),
             iface->name.c_str(), type_str(want.type).c_str());
        return false;
      }
      // A caller holding the interface checks its call against the
      // interface's signature, so a method that writes where the interface
      // does not say it may would write memory nobody lent it. Writing less
      // than the interface allows is fine.
      if (got->param_mut[0] && !want.writes) {
        note(value->pos,
             "%s.%s writes through its receiver, but %s does not say it may;"
             " declare it 'mut %s' in the interface",
             shown_name(owner->name).c_str(), want.name.c_str(),
             shown_name(iface->name).c_str(), want.name.c_str());
        return false;
      }
      for (size_t i = 0; i < want.type->params.size(); i++) {
        if (!got->param_mut[i + 1] || want.type->param_mut[i]) continue;
        note(value->pos,
             "%s.%s writes through parameter %zu, but %s does not say it may;"
             " mark that parameter 'mut' in the interface",
             shown_name(owner->name).c_str(), want.name.c_str(), i + 1,
             shown_name(iface->name).c_str());
        return false;
      }
      entries.push_back(have->name);
    }

    value->vtable = types.vtable(owner->name + ">" + iface->name, entries);
    value->bind_to = iface;
    return true;
  }

  // Every place a value flows into a declared type goes through here, so
  // interface conversion happens uniformly.
  bool convert(Node *value, Type *to) {
    if (assignable(value->type, to)) {
      // A value going into a `?T` that is a { has, value } pair becomes the
      // payload, and the box is built around it where it lands: a store knows
      // its destination, and a call argument builds from `bind_to`. Carrying
      // the box's type instead made the value be copied as if it already were
      // one — the caller handing over the payload's bytes and whatever sat
      // behind them as the flag.
      //
      // `nil` is the exception and is already an optional: it keeps the box's
      // type, and lowering makes an absent one.
      if (to->is_optional && value->type->kind != TY_OPT &&
          !type_eq(value->type, to)) {
        apply_type(value, const_cast<Type *>(opt_payload(to)));
        value->bind_to = to;
      } else {
        apply_type(value, to);
      }
      return true;
    }
    if (to->is_any) {
      settle(value);
      if (any_kind_of(value->type) < 0) {
        note(value->pos, "%s cannot be passed as 'any'",
             type_str(value->type).c_str());
        return false;
      }
      // Boxing happens at the call site, the same way an interface pair is
      // built there. Without this a plain `any` parameter — as opposed to a
      // gathered `...any` — receives the raw value where it expects a box.
      if (!value->type->is_any) value->bind_to = to;
      return true;
    }
    // `?Interface` taking a pointer to a struct: the pair is built for the
    // payload, and the box around it is what the value becomes. Where this is
    // stored, the destination says what to build — but a call argument is
    // built from this type alone, and leaving it as the bare interface had the
    // caller write sixteen bytes where the callee reads twenty-four, taking
    // the flag from whatever was on the stack behind them.
    if (is_optional(to)) {
      Type *payload = const_cast<Type *>(opt_payload(to));
      if (payload && payload->is_interface) {
        if (!bind_interface(value, payload)) return false;
        value->bind_to = to;
        return true;
      }
    }
    return to->is_interface && bind_interface(value, to);
  }

  // --- expressions -------------------------------------------------------

  Type *check_binary(Node *n) {
    Type *lhs = check_expr(n->lhs);
    Type *rhs = check_expr(n->rhs);
    if (!lhs || !rhs) return nullptr;

    if (n->op == TK_ANDAND || n->op == TK_OROR) {
      if (lhs->kind != TY_BOOL || rhs->kind != TY_BOOL) {
        error(n->pos, "'%s' needs bool operands, got %s and %s",
              tok_name(n->op), type_str(lhs).c_str(), type_str(rhs).c_str());
        return nullptr;
      }
      return n->type = types.bool_ty;
    }

    // A raw pointer can be offset. `*T` and `[]T` deliberately cannot: they
    // are the checked side of the boundary.
    if ((n->op == TK_PLUS || n->op == TK_MINUS) && lhs->kind == TY_RAWPTR &&
        is_integer(rhs)) {
      apply_type(n->rhs, types.usize_ty);
      settle(n->rhs);
      return n->type = lhs;
    }

    // Whichever side is concrete decides the type of the other.
    if (lhs->untyped && assignable(lhs, rhs)) { apply_type(n->lhs, rhs); lhs = rhs; }
    else if (rhs->untyped && assignable(rhs, lhs)) { apply_type(n->rhs, lhs); rhs = lhs; }

    if (!type_eq(lhs, rhs)) {
      error(n->pos, "mismatched operands for '%s': %s and %s",
            tok_name(n->op), type_str(lhs).c_str(), type_str(rhs).c_str());
      return nullptr;
    }

    bool is_ptr = lhs->kind == TY_PTR || lhs->kind == TY_RAWPTR;
    switch (n->op) {
    case TK_EQ: case TK_NE:
      // `opt == nil` asks whether it is absent. Two optionals cannot be
      // compared: that would mean comparing the payloads, and the payload of an
      // absent one does not exist.
      if (is_optional(lhs) &&
          (n->lhs->kind == ND_NIL_LIT || n->rhs->kind == ND_NIL_LIT)) {
        n->form = CMP_NIL;
        return n->type = types.bool_ty;
      }
      // Strings compare by content, which is the only comparison here that is
      // not a single instruction. Everything else is a value or an address.
      if (!is_numeric(lhs) && lhs->kind != TY_BOOL && !is_ptr &&
          lhs->kind != TY_STRING && lhs->kind != TY_ENUM) {
        error(n->pos, "cannot compare values of type %s",
              type_str(lhs).c_str());
        return nullptr;
      }
      return n->type = types.bool_ty;
    case TK_LT: case TK_LE: case TK_GT: case TK_GE:
      if (!is_numeric(lhs)) {
        error(n->pos, "cannot order values of type %s",
              type_str(lhs).c_str());
        return nullptr;
      }
      return n->type = types.bool_ty;
    case TK_SHL: case TK_SHR:
    case TK_AMP: case TK_PIPE: case TK_CARET:
      if (!is_integer(lhs)) {
        error(n->pos, "'%s' needs integer operands, got %s",
              tok_name(n->op), type_str(lhs).c_str());
        return nullptr;
      }
      return n->type = lhs;
    case TK_PERCENT:
      if (!is_numeric(lhs)) {
        error(n->pos, "'%%' needs numeric operands, got %s",
              type_str(lhs).c_str());
        return nullptr;
      }
      return n->type = lhs;
    default:
      if (!is_numeric(lhs)) {
        error(n->pos, "'%s' needs numeric operands, got %s",
              tok_name(n->op), type_str(lhs).c_str());
        // The one people reach for first. Joining two strings at run time
        // needs somewhere to put the answer, and memory comes from an
        // allocator the caller names.
        if (n->op == TK_PLUS && lhs->kind == TY_STRING)
          note(n->pos, "joining strings allocates: use strings.Concat(a, x, "
                       "y), which takes the allocator; '+' works between "
                       "constants");
        return nullptr;
      }
      return n->type = lhs;
    }
  }

  // `i32(x)` parses as a call; if the name is a type and nothing shadows it,
  // it is a conversion instead. Same rule as Go.
  Type *check_convert(Node *n, Type *target) {
    n->kind = ND_CONVERT;
    if (n->kids.size() != 1) {
      error(n->pos, "%s(...) converts exactly one value, got %zu",
            type_str(target).c_str(), n->kids.size());
      return nullptr;
    }
    Type *from = check_expr(n->kids[0]);
    if (!from) return nullptr;
    apply_type(n->kids[0], target);
    from = settle(n->kids[0]);

    // Writing out a conversion that would have happened anyway is always
    // fine, and a slice of bytes and a string share a representation: going
    // the other way is the programmer promising not to write through it.
    bool same_bytes =
        (from->kind == TY_SLICE && from->elem->bits == 8 &&
         target->kind == TY_STRING);
    bool into_atomic = target->kind == TY_ATOMIC &&
                       assignable(from, target->elem);
    // `shared[T](v)` is the only way to make one, and it starts unlocked.
    bool into_shared = target->is_shared && assignable(from, target->elem);
    if (into_shared) apply_type(n->kids[0], target->elem);
    bool from_atomic = from->kind == TY_ATOMIC &&
                       assignable(from->elem, target);
    if (into_atomic) apply_type(n->kids[0], target->elem);
    // An enum converts to and from its width, which is how a value that came
    // off the wire becomes one. It is not checked against the members: there is
    // no run-time table to check it against.
    bool enum_width = (from->kind == TY_ENUM && is_integer(target)) ||
                      (is_integer(from) && target->kind == TY_ENUM) ||
                      (from->kind == TY_ENUM && target->kind == TY_ENUM);
    bool ok = assignable(from, target) || same_bytes || into_atomic ||
              from_atomic || into_shared || enum_width ||
              (is_numeric(from) && is_numeric(target)) ||
              // Raw pointers are the unchecked side of the boundary, so they
              // reinterpret freely. `*T` converts out to one, never back in:
              // that would fabricate a non-null guarantee.
              (from->kind == TY_RAWPTR && target->kind == TY_RAWPTR) ||
              (from->kind == TY_PTR && target->kind == TY_RAWPTR) ||
              // An address as a number, and back. An allocator written in Sword
              // needs it — aligning memory means arithmetic on where it is, and
              // there is no other way to ask. Only at full width: a narrower
              // integer would drop half of an address without saying so.
              (from->kind == TY_RAWPTR && is_integer(target) &&
               target->bits == 64) ||
              (is_integer(from) && from->bits == 64 &&
               target->kind == TY_RAWPTR) ||
              // A C function is an address C handed over, typed by whoever
              // converts it; a Sword one is never made this way.
              (from->kind == TY_RAWPTR && target->kind == TY_FUNC &&
               target->is_extern) ||
              (from->kind == TY_FUNC && from->is_extern &&
               target->kind == TY_RAWPTR);
    if (!ok) {
      error(n->pos, "cannot convert %s to %s", type_str(from).c_str(),
            type_str(target).c_str());
      return nullptr;
    }
    return n->type = target;
  }

  Symbol *find_method(Type *owner, const std::string &name) {
    if (!owner || owner->kind != TY_STRUCT) return nullptr;
    auto by_type = prog.methods.find(owner->name);
    if (by_type == prog.methods.end()) return nullptr;
    auto found = by_type->second.find(name);
    return found == by_type->second.end() ? nullptr : found->second;
  }

  // Checks the argument list against a signature, skipping `skip` leading
  // parameters that the call site fills in for itself (the receiver).
  // Whether a parameter takes write access. A declaration says so on the
  // parameter; a function value has only its type, which carries the same
  // answer.
  bool wants_write(Type *sig, Node *param, size_t at) {
    if (param) return param->is_mut;
    return at < sig->param_mut.size() && sig->param_mut[at];
  }

  bool check_args(Node *n, Symbol *sym, size_t skip,
                  bool already_checked = false) {
    Type *sig = sym->type;
    size_t wanted = sig->params.size() - skip;

    Node *last = sym->decl && !sym->decl->kids.empty() ? sym->decl->kids.back()
                                                       : nullptr;
    if (last && last->is_variadic) {
      size_t fixed = wanted - 1;
      if (n->kids.size() < fixed) {
        error(n->pos, "'%s' takes at least %zu argument%s, got %zu",
              sym->name.c_str(), fixed, fixed == 1 ? "" : "s",
              n->kids.size());
        return false;
      }
      n->variadic_at = (int)fixed;
      Type *element = sig->params.back()->elem;

      // `f(xs...)` passes the list through instead of building a new one.
      if (n->kids.size() == fixed + 1 && n->kids.back()->is_variadic) {
        for (size_t i = 0; i < fixed; i++) {
          Type *arg = already_checked ? n->kids[i]->type
                                      : check_expr(n->kids[i]);
          if (!arg || !convert(n->kids[i], sig->params[i + skip])) {
            error(n->kids[i]->pos, "argument %zu of '%s' expects %s",
                  i + 1, sym->name.c_str(),
                  type_str(sig->params[i + skip]).c_str());
            return false;
          }
        }
        Node *rest = n->kids.back();
        Type *given = already_checked ? rest->type : check_expr(rest);
        if (!given) return false;
        if (!assignable(given, sig->params.back())) {
          error(rest->pos, "'%s' gathers %s, got %s", sym->name.c_str(),
                type_str(sig->params.back()).c_str(), type_str(given).c_str());
          return false;
        }
        n->is_variadic = true;
        return true;
      }

      for (size_t i = 0; i < n->kids.size(); i++) {
        Type *want = i < fixed ? sig->params[i + skip] : element;
        Type *arg = already_checked ? n->kids[i]->type
                                    : check_expr(n->kids[i]);
        if (!arg) return false;
        if (!convert(n->kids[i], want)) {
          error(n->kids[i]->pos, "argument %zu of '%s' expects %s, got %s",
                i + 1, sym->name.c_str(), type_str(want).c_str(),
                type_str(arg).c_str());
          return false;
        }
        Node *param = i < fixed && sym->decl ? sym->decl->kids[i + skip]
                                             : nullptr;
        if (wants_write(sig, param, i + skip) &&
            !require_mutable(n->kids[i], "pass"))
          return false;
      }
      return true;
    }

    // C's own `...` gathers nothing: what follows the declared parameters is
    // passed as written, so there is no type to convert it to — only the
    // question of whether C can read it at all.
    if (sig->is_c_variadic) {
      if (n->kids.size() < wanted) {
        error(n->pos, "'%s' takes at least %zu argument%s, got %zu",
              sym->name.c_str(), wanted, wanted == 1 ? "" : "s",
              n->kids.size());
        return false;
      }
      for (size_t i = wanted; i < n->kids.size(); i++) {
        if (!already_checked && !check_expr(n->kids[i])) return false;
        // Nothing here says what an untyped literal should be, so it takes the
        // default it would have taken standing on its own.
        Type *arg = settle(n->kids[i]);
        if (!arg) return false;
        if (is_aggregate(arg) || arg->kind == TY_STRING) {
          error(n->kids[i]->pos,
                "'%s' cannot pass %s through '...'; pass .ptr and .len",
                sym->name.c_str(), type_str(arg).c_str());
          return false;
        }
      }
    } else if (n->kids.size() != wanted) {
      error(n->pos, "'%s' takes %zu argument%s, got %zu",
            sym->name.c_str(), wanted, wanted == 1 ? "" : "s", n->kids.size());
      return false;
    }
    for (size_t i = 0; i < wanted; i++) {
      Type *want = sig->params[i + skip];
      Type *arg = already_checked ? n->kids[i]->type : check_expr(n->kids[i]);
      if (!arg) return false;
      if (!convert(n->kids[i], want)) {
        error(n->kids[i]->pos, "argument %zu of '%s' expects %s, got %s",
              i + 1, sym->name.c_str(), type_str(want).c_str(),
              type_str(arg).c_str());
        return false;
      }

      // A `mut` parameter hands the callee write access, so the caller must
      // be entitled to it in the first place.
      Node *param = sym->decl ? sym->decl->kids[i + skip] : nullptr;
      if (wants_write(sig, param, i + skip) &&
          !require_mutable(n->kids[i], "pass"))
        return false;
    }
    return true;
  }

  // The whole API of an atomic. Keeping it small is deliberate: every one of
  // these is a single machine instruction.
  Type *check_atomic_call(Node *n, Type *owner) {
    Node *field = n->lhs;
    Type *inner = owner->elem;
    const std::string &name = field->name;

    int wanted = 1;
    Type *result = inner;
    int kind = -1;
    if (name == "Load") {
      wanted = 0;
    } else if (name == "Store") {
      result = types.void_ty;
    } else if (name == "Swap") {
      kind = RMW_SWAP;
    } else if (name == "Add") {
      kind = RMW_ADD;
    } else if (name == "Sub") {
      kind = RMW_SUB;
    } else if (name == "And") {
      kind = RMW_AND;
    } else if (name == "Or") {
      kind = RMW_OR;
    } else if (name == "CompareSwap") {
      wanted = 2;
      result = types.bool_ty;
    } else {
      error(field->pos, "%s has no operation '%s'", type_str(owner).c_str(),
            name.c_str());
      return nullptr;
    }

    if (kind >= 0 && inner->kind != TY_INT) {
      error(field->pos, "'%s' needs an integer atomic, got %s", name.c_str(),
            type_str(owner).c_str());
      return nullptr;
    }
    if ((int)n->kids.size() != wanted) {
      error(n->pos, "'%s' takes %d argument%s, got %zu", name.c_str(), wanted,
            wanted == 1 ? "" : "s", n->kids.size());
      return nullptr;
    }
    for (Node *arg : n->kids) {
      if (!check_expr(arg)) return nullptr;
      if (!convert(arg, inner)) {
        error(arg->pos, "'%s' expects %s", name.c_str(),
              type_str(inner).c_str());
        return nullptr;
      }
    }

    n->form = 3; // atomic operation
    n->ival = (uint64_t)(kind >= 0 ? kind : 0);
    n->name = name;
    return n->type = result;
  }

  // The whole API of a shared value, beside `lock`. All three must be called
  // with the lock held: `Wait` lets go of it, puts the task down, and takes it
  // back before returning, which is what turns a mutex into something a queue
  // can be built out of.
  Type *check_shared_call(Node *n, Type *owner) {
    Node *field = n->lhs;
    const std::string &name = field->name;
    if (name != "Wait" && name != "Notify" && name != "NotifyAll") {
      error(field->pos, "%s has no operation '%s'; reach the value with 'lock'",
            type_str(owner).c_str(), name.c_str());
      return nullptr;
    }
    if (!n->kids.empty()) {
      error(n->pos, "'%s' takes no arguments", name.c_str());
      return nullptr;
    }
    // The list of waiters is only ever touched by whoever holds the guard
    // alone; readers hold it together.
    Symbol *root = share_root(field->lhs);
    for (const HeldLock &open : held_locks) {
      if (!root || open.root != root || !open.read) continue;
      error(n->pos, "'%s' needs 'lock', not 'lock read': readers hold the value "
                    "together, and only one holder may wait or wake",
            name.c_str());
      note(open.at, "the 'lock read' is here");
      return nullptr;
    }
    n->form = CALL_SHARED;
    n->ival = name == "Wait" ? 0 : (name == "Notify" ? 1 : 2);
    n->name = name;
    return n->type = types.void_ty;
  }

  Type *check_method_call(Node *n) {
    Node *field = n->lhs;
    Type *base = check_expr(field->lhs);
    if (!base) return nullptr;
    Type *owner = base->kind == TY_PTR ? base->elem : base;

    if (owner->kind == TY_ATOMIC) return check_atomic_call(n, owner);
    if (is_shared(owner)) return check_shared_call(n, owner);
    // An error is a code at run time, so its message comes from a table the
    // compiler writes, the same way its name does.
    if (type_eq(owner, types.error_ty) && field->name == "Message")
      return check_error_message(n);

    if (owner->is_interface) {
      for (size_t i = 0; i < owner->methods.size(); i++) {
        const Field &m = owner->methods[i];
        if (m.name != field->name) continue;
        if (m.writes &&
            !require_mutable(field->lhs, "call a mutating method on"))
          return nullptr;
        n->form = 2; // dynamic: the function pointer comes from the vtable
        n->ival = i;
        Symbol probe{field->name, m.type, false,      false, false,
                     nullptr,     nullptr, field->pos, -1,    nullptr, {}};
        if (!check_args(n, &probe, 0)) return nullptr;
        return n->type = m.type->ret;
      }
      error(field->pos, "%s has no method '%s'", owner->name.c_str(),
            field->name.c_str());
      return nullptr;
    }

    // A field holding a function value is called like a method would be, and
    // that is the point: `m.handler(req, res)` reads the same either way.
    if (const Field *held = find_field(owner, field->name)) {
      if (held->type->kind != TY_FUNC) {
        error(field->pos, "'%s' is not a function", field->name.c_str());
        return nullptr;
      }
      if (!check_expr(field)) return nullptr;
      return check_indirect_call(n, held->type, field->name);
    }

    Symbol *sym = find_method(owner, field->name);
    if (!sym) {
      error(field->pos, "%s has no method '%s'", type_str(owner).c_str(),
            field->name.c_str());
      return nullptr;
    }
    // A method's receiver is always a struct, so a temporary already lives in
    // memory by the time it is a value: `time.Since(start).AsMillis()` needs
    // nothing but its address. Writing into one is another matter — the value
    // is thrown away, so the write could only be a mistake.
    bool temporary = !is_place(field->lhs) && base->kind != TY_PTR;

    Node *recv = sym->decl->kids[0];
    if (recv->is_mut) {
      if (temporary) {
        error(field->pos,
              "'%s' writes through its receiver, and this value has nowhere to"
              " keep the change; bind it first",
              field->name.c_str());
        return nullptr;
      }
      if (!require_mutable(field->lhs, "call a mutating method on"))
        return nullptr;
    }

    n->form = 1; // method: the receiver is passed ahead of the arguments
    n->name = sym->name;
    n->sym = sym;
    if (!check_args(n, sym, 1)) return nullptr;
    return n->type = sym->type->ret;
  }

  // `mem.init(...)` looks exactly like a method call until you notice that
  // `mem` names a package rather than a value.
  Type *check_package_call(Node *n, Package *other) {
    Node *field = n->lhs;
    Symbol *sym = find_in(other->globals, field->name, false);
    // `colour.Shade(2)` is a conversion, and the same one it is inside the
    // package that declares Shade. A function of that name shadows it, which
    // is the rule at home too.
    if (!sym)
      if (Type *named = find_in(other->type_names, field->name, false))
        return check_convert(n, named);
    if (!sym) {
      error(field->pos, "package '%s' has no exported function or type '%s'",
            other->name.c_str(), field->name.c_str());
      return nullptr;
    }
    if (!sym->is_func) {
      error(field->pos, "'%s.%s' is not a function", other->name.c_str(),
            field->name.c_str());
      return nullptr;
    }
    if (sym->is_generic) return check_generic_call(n, sym, other, {});
    if (!present(n, sym)) return nullptr;
    n->sym = sym;
    n->name = sym->name;
    if (!check_args(n, sym, 0)) return nullptr;
    return n->type = sym->type->ret;
  }

  Type *type_from_expr(Node *e) {
    // `Entry[V]` reads as an index expression here; as a type argument it is
    // an instantiation, and it may nest.
    if (e->kind == ND_INDEX) {
      Node *inst = ast.make(ND_TYPE_INST, e->pos);
      Node *base = ast.make(ND_TYPE_NAME, e->lhs->pos);
      if (e->lhs->kind == ND_IDENT) {
        base->name = e->lhs->name;
      } else if (e->lhs->kind == ND_FIELD && e->lhs->lhs->kind == ND_IDENT) {
        base->text = e->lhs->lhs->name;
        base->name = e->lhs->name;
      } else {
        error(e->pos, "expected a type argument");
        return nullptr;
      }
      inst->lhs = base;
      inst->kids.push_back(as_type_expr(e->rhs));
      for (Node *more : e->kids) inst->kids.push_back(as_type_expr(more));
      for (Node *arg : inst->kids)
        if (!arg) return nullptr;
      return resolve(inst);
    }
    if (e->kind == ND_IDENT) {
      Node probe = *e;
      probe.kind = ND_TYPE_NAME;
      probe.text.clear();
      return lookup_type(&probe);
    }
    if (e->kind == ND_FIELD && e->lhs->kind == ND_IDENT) {
      Node probe = *e;
      probe.kind = ND_TYPE_NAME;
      probe.text = e->lhs->name;
      probe.name = e->name;
      return lookup_type(&probe);
    }
    error(e->pos, "expected a type argument");
    return nullptr;
  }

  // Rewrites an expression that was really a type into the type grammar.
  Node *as_type_expr(Node *e) {
    if (!e) return nullptr;
    if (e->kind == ND_IDENT) {
      Node *name = ast.make(ND_TYPE_NAME, e->pos);
      name->name = e->name;
      return name;
    }
    if (e->kind == ND_FIELD && e->lhs->kind == ND_IDENT) {
      Node *name = ast.make(ND_TYPE_NAME, e->pos);
      name->text = e->lhs->name;
      name->name = e->name;
      return name;
    }
    if (e->kind == ND_INDEX) {
      Node *inst = ast.make(ND_TYPE_INST, e->pos);
      inst->lhs = as_type_expr(e->lhs);
      if (!inst->lhs) return nullptr;
      inst->kids.push_back(as_type_expr(e->rhs));
      for (Node *more : e->kids) inst->kids.push_back(as_type_expr(more));
      for (Node *arg : inst->kids)
        if (!arg) return nullptr;
      return inst;
    }
    error(e->pos, "expected a type argument");
    return nullptr;
  }

  Type *check_generic_call(Node *n, Symbol *sym, Package *owner,
                           std::vector<Type *> args) {
    Node *generic = sym->decl;

    // No explicit type arguments: read them off the values being passed.
    bool checked = false;
    if (args.empty()) {
      if (n->kids.size() != generic->kids.size()) {
        error(n->pos, "'%s' takes %zu argument%s, got %zu",
              generic->name.c_str(), generic->kids.size(),
              generic->kids.size() == 1 ? "" : "s", n->kids.size());
        return nullptr;
      }
      std::vector<std::pair<std::string, Type *>> bind;
      for (size_t i = 0; i < n->kids.size(); i++) {
        if (!check_expr(n->kids[i])) return nullptr;
        unify(generic, generic->kids[i]->type_expr, settle(n->kids[i]), bind);
      }
      checked = true;
      for (Node *tp : generic->tparams) {
        Type *found = nullptr;
        for (auto &entry : bind)
          if (entry.first == tp->name) found = entry.second;
        if (!found) {
          error(n->pos,
                "cannot infer '%s' for '%s'; pass it as '%s[Type](...)'",
                tp->name.c_str(), generic->name.c_str(),
                generic->name.c_str());
          return nullptr;
        }
        args.push_back(found);
      }
    }

    Symbol *inst = instantiate(generic, owner, args, n->pos);
    if (!inst) return nullptr;
    n->sym = inst;
    n->name = inst->name;
    if (!check_args(n, inst, 0, checked)) return nullptr;
    return n->type = inst->type->ret;
  }

  // `f[i32](x)` parses as a call on an index expression; the index is really
  // a type argument list.
  Type *check_indexed_call(Node *n) {
    Node *index = n->lhs;
    Node *callee = index->lhs;

    std::vector<Node *> arg_exprs{index->rhs};
    for (Node *more : index->kids) arg_exprs.push_back(more);

    Symbol *sym = nullptr;
    Package *owner = &pkg;
    std::string shown;
    if (callee->kind == ND_IDENT) {
      shown = callee->name;
      // `chan[T](&arena, 8)`: the same rewrite as the type, into the call that
      // builds one. The allocator is explicit because everything here is —
      // which is the reason `make` from Go has no equivalent.
      if (callee->name == "chan" && !lookup("chan") &&
          !pkg.generic_types.count("chan") && imported("chan")) {
        Node *qualified = ast.make(ND_FIELD, callee->pos);
        Node *owner_name = ast.make(ND_IDENT, callee->pos);
        owner_name->name = "chan";
        qualified->lhs = owner_name;
        qualified->name = "New";
        index->lhs = qualified;
        return check_indexed_call(n);
      }
      bool builtin_box =
          (callee->name == "atomic" && !lookup("atomic") &&
           !pkg.generic_types.count("atomic")) ||
          (callee->name == "shared" && !lookup("shared") &&
           !pkg.generic_types.count("shared"));
      if (builtin_box) {
        if (arg_exprs.size() != 1) {
          error(n->pos, "%s[T] takes one type argument",
                callee->name.c_str());
          return nullptr;
        }
        Type *inner = type_from_expr(arg_exprs[0]);
        if (!inner) return nullptr;
        if (callee->name == "shared") {
          if (inner->kind == TY_VOID) {
            error(n->pos, "shared[void] has nothing to protect");
            return nullptr;
          }
          return check_convert(n, types.shared(inner));
        }
        if (inner->kind != TY_INT && inner->kind != TY_BOOL) {
          error(n->pos, "atomic[%s] is not supported; use an integer or bool",
                type_str(inner).c_str());
          return nullptr;
        }
        return check_convert(n, types.atomic(inner));
      }
      sym = lookup(callee->name);
      if (!sym) {
        std::vector<Type *> args;
        for (Node *e : arg_exprs) {
          Type *a = type_from_expr(e);
          if (!a) return nullptr;
          args.push_back(a);
        }
        if (builtin_size(n, callee->name, args)) return n->type;
      }
    } else if (callee->kind == ND_FIELD && callee->lhs->kind == ND_IDENT &&
               !lookup(callee->lhs->name)) {
      owner = imported(callee->lhs->name);
      shown = callee->name;
      if (owner) sym = find_in(owner->globals, callee->name, false);
    }

    if (!sym || !sym->is_generic) {
      error(n->pos, "'%s' is not a generic function", shown.c_str());
      return nullptr;
    }

    std::vector<Type *> args;
    for (Node *e : arg_exprs) {
      Type *a = type_from_expr(e);
      if (!a) return nullptr;
      args.push_back(a);
    }
    return check_generic_call(n, sym, owner, args);
  }

  // `nameof(k)` is the name of the member `k` is, or empty when it is not one.
  // It calls the function synthesized alongside the enum.
  // `hashof(x)`: one number per value, which is what a table needs to decide
  // where to keep a key. Defined for what `==` compares — integers, enums,
  // bools and strings — because a hash is only useful next to an equality that
  // agrees with it. A generic keyed by anything else is refused where it is
  // instantiated, which is where the caller can see what it asked for.
  Type *check_hashof(Node *n) {
    if (n->kids.size() != 1) {
      error(n->pos, "'hashof' takes one value");
      return nullptr;
    }
    if (!check_expr(n->kids[0])) return nullptr;
    Type *arg = settle(n->kids[0]);
    if (!arg) return nullptr;
    if (!is_integer(arg) && arg->kind != TY_ENUM && arg->kind != TY_BOOL &&
        arg->kind != TY_STRING) {
      error(n->kids[0]->pos,
            "'hashof' takes an integer, an enum, a bool or a string, got %s",
            type_str(arg).c_str());
      return nullptr;
    }
    n->form = CALL_HASH;
    return n->type = types.named("u64");
  }

  Type *check_nameof(Node *n) {
    if (n->kids.size() != 1) {
      error(n->pos, "'nameof' takes one enum value");
      return nullptr;
    }
    Type *arg = check_expr(n->kids[0]);
    if (!arg) return nullptr;
    // An error is a code at run time and nothing else, so its name has to come
    // from a table the compiler writes. Same shape as an enum's.
    if (type_eq(arg, types.error_ty)) return check_error_name(n);
    if (arg->kind != TY_ENUM) {
      error(n->kids[0]->pos, "'nameof' needs an enum or an error, got %s",
            type_str(arg).c_str());
      return nullptr;
    }
    auto found = prog.enum_names.find(arg);
    if (found == prog.enum_names.end()) {
      error(n->pos, "%s has no names to look up", type_str(arg).c_str());
      return nullptr;
    }
    n->form = CALL_DIRECT;
    n->sym = found->second;
    n->name = found->second->name;
    if (!check_args(n, found->second, 0, true)) return nullptr;
    return n->type = types.string_ty;
  }

  Type *check_error_name(Node *n) {
    if (!prog.error_name) {
      std::vector<Type *> params{types.error_ty};
      prog.error_name = ast.make_symbol(
          "error.name", types.func(params, types.string_ty), false, n->pos);
      prog.error_name->is_func = true;
    }
    n->form = CALL_DIRECT;
    n->sym = prog.error_name;
    n->name = prog.error_name->name;
    if (!check_args(n, prog.error_name, 0, true)) return nullptr;
    return n->type = types.string_ty;
  }

  // `e.Message()`: the text the error was declared with, or empty when it was
  // declared without one.
  Type *check_error_message(Node *n) {
    if (!prog.error_message) {
      std::vector<Type *> params{types.error_ty};
      prog.error_message = ast.make_symbol(
          "error.message", types.func(params, types.string_ty), false, n->pos);
      prog.error_message->is_func = true;
    }
    Node *subject = n->lhs->lhs;
    n->kids.insert(n->kids.begin(), subject);
    n->form = CALL_DIRECT;
    n->sym = prog.error_message;
    n->name = prog.error_message->name;
    if (!check_args(n, prog.error_message, 0, true)) return nullptr;
    return n->type = types.string_ty;
  }

  // `<-ch` and `ch <- v`. Rewritten into the calls that do the work, so that
  // `try`, `catch` and everything downstream see what they already understand.
  Node *channel_call(Node *n, Node *channel, const char *method) {
    Node *call = ast.make(ND_CALL, n->pos);
    Node *field = ast.make(ND_FIELD, n->pos);
    field->lhs = channel;
    field->name = method;
    field->name_pos = n->pos;
    call->lhs = field;
    return call;
  }

  // A channel is an instance of one generic struct, so this is what it is named.
  // Asked before the rewrite, so that a mistake reads as what was written rather
  // than as what it turns into.
  bool is_channel(const Type *t) {
    if (!t) return false;
    const Type *owner = t->kind == TY_PTR ? t->elem : t;
    const std::string mark = "std.chan.Chan$";
    return owner->name.compare(0, mark.size(), mark) == 0;
  }

  // What a channel carries. The struct behind `chan[T]` keeps its state as bytes,
  // so the element type is read back off the signature of `Recv`, which is the
  // one place it survives.
  Type *channel_elem(Type *chan_type) {
    const Type *owner = chan_type->kind == TY_PTR ? chan_type->elem : chan_type;
    auto methods = prog.methods.find(owner->name);
    if (methods == prog.methods.end()) return nullptr;
    auto recv = methods->second.find("Recv");
    if (recv == methods->second.end() || !recv->second->type) return nullptr;
    Type *ret = recv->second->type->ret;
    if (!ret || !is_optional(ret)) return nullptr;
    return const_cast<Type *>(opt_payload(ret));
  }

  // `select` waits on several channels at once and runs the case that could go.
  // The cases are rewritten here as far as they can be: the channel becomes the
  // address of its state, which is what the runtime is handed, and the binding
  // becomes a `?T` — the same thing `<-ch` produces, because a closed channel is
  // an answer and the body has to be able to see it.
  Type *check_select(Node *n) {
    if (n->kids.empty()) {
      error(n->pos, "a select with no cases has nothing to wait for");
      return nullptr;
    }
    // The runtime watches one guard per case out of a fixed array, and holding the
    // language to the same number is better than finding out at run time.
    if (n->kids.size() > 64) {
      error(n->pos, "a select takes at most 64 cases");
      return nullptr;
    }
    bool seen_default = false;
    for (Node *arm : n->kids) {
      if (arm->form == 2) {
        if (seen_default) {
          error(arm->pos, "a select has one 'default' at most");
          return nullptr;
        }
        seen_default = true;
        check_stmt(arm->body);
        continue;
      }

      Type *chan_type = check_expr(arm->lhs);
      if (!chan_type) return nullptr;
      if (!is_channel(chan_type)) {
        error(arm->lhs->pos, "a select case takes a channel, not %s",
              type_str(chan_type).c_str());
        return nullptr;
      }
      Type *elem = channel_elem(chan_type);
      if (!elem) {
        error(arm->lhs->pos, "cannot tell what this channel carries");
        return nullptr;
      }

      if (arm->form == 1) {
        if (!check_expr(arm->rhs)) return nullptr;
        if (!convert(arm->rhs, elem)) {
          error(arm->rhs->pos, "this channel carries %s, not %s",
                type_str(elem).c_str(), type_str(arm->rhs->type).c_str());
          return nullptr;
        }
        arm->type = elem;
      } else {
        arm->type = types.opt(elem);
      }

      // The channel expression becomes the address of its state. Everything the
      // runtime needs is that pointer, and the rewrite keeps lowering from having
      // to know about the package behind a channel.
      Node *address = ast.make(ND_CALL, arm->lhs->pos);
      Node *field = ast.make(ND_FIELD, arm->lhs->pos);
      field->lhs = arm->lhs;
      field->name = "Address";
      field->name_pos = arm->lhs->pos;
      address->lhs = field;
      if (!check_expr(address)) return nullptr;
      arm->lhs = address;

      push_scope();
      if (arm->form == 0 && !arm->name.empty())
        arm->sym = declare(arm->name, arm->type, false, arm->name_pos);
      check_stmt(arm->body);
      pop_scope();
    }
    return types.void_ty;
  }

  Type *check_recv(Node *n) {
    Type *from = check_expr(n->lhs);
    if (!from) return nullptr;
    if (!is_channel(from)) {
      error(n->pos, "'<-' takes a channel, not %s", type_str(from).c_str());
      return nullptr;
    }
    Node *call = channel_call(n, n->lhs, "Recv");
    *n = *call;
    return check_expr(n);
  }

  Type *check_send(Node *n) {
    Type *into = check_expr(n->lhs);
    if (!into) return nullptr;
    if (!is_channel(into)) {
      error(n->pos, "'<-' sends into a channel, not %s",
            type_str(into).c_str());
      return nullptr;
    }
    Node *call = channel_call(n, n->lhs, "Send");
    call->kids.push_back(n->rhs);
    *n = *call;
    return check_expr(n);
  }

  Type *check_call(Node *n) {
    if (n->lhs->kind == ND_INDEX) return check_indexed_call(n);
    if (n->lhs->kind == ND_FIELD) {
      Node *base = n->lhs->lhs;
      if (base->kind == ND_IDENT && !lookup(base->name))
        if (Package *other = imported(base->name))
          return check_package_call(n, other);
      return check_method_call(n);
    }
    if (n->lhs->kind != ND_IDENT) {
      error(n->pos, "only direct calls are supported for now");
      return nullptr;
    }
    if (n->lhs->name == "nameof" && !lookup("nameof")) return check_nameof(n);
    if (n->lhs->name == "hashof" && !lookup("hashof")) return check_hashof(n);
    // `close(ch)`, the third of the three things a channel needs a word for.
    if (n->lhs->name == "close" && !lookup("close") && n->kids.size() == 1) {
      Type *what = check_expr(n->kids[0]);
      if (!what) return nullptr;
      if (!is_channel(what)) {
        error(n->pos, "'close' takes a channel, not %s",
              type_str(what).c_str());
        return nullptr;
      }
      Node *call = channel_call(n, n->kids[0], "Close");
      *n = *call;
      return check_expr(n);
    }
    if (!lookup(n->lhs->name))
      if (Type *target = lookup_type(n->lhs->name))
        return check_convert(n, target);
    Symbol *sym = lookup(n->lhs->name);
    if (!sym) {
      error(n->lhs->pos, "undefined function '%s'", n->lhs->name.c_str());
      return nullptr;
    }
    if (!sym->is_func) {
      if (sym->type && sym->type->kind == TY_FUNC) {
        n->lhs->sym = sym;
        n->lhs->type = sym->type;
        return check_indirect_call(n, sym->type, sym->name);
      }
      error(n->lhs->pos, "'%s' is not a function", sym->name.c_str());
      return nullptr;
    }
    n->lhs->sym = sym;

    if (sym->is_generic) return check_generic_call(n, sym, &pkg, {});
    if (!present(n, sym)) return nullptr;
    n->sym = sym;
    n->name = sym->name;
    if (!check_args(n, sym, 0)) return nullptr;
    return n->type = sym->type->ret;
  }

  // A member is a constant of its own type, so it folds away like any other.
  Type *check_enum_member(Node *n, Type *type) {
    const Field *member = enum_member(type, n->name);
    if (!member) {
      error(n->pos, "%s has no member '%s'", type_str(type).c_str(),
            n->name.c_str());
      return nullptr;
    }
    n->kind = ND_INT_LIT;
    n->ival = (uint64_t)member->offset;
    return n->type = type;
  }

  Type *check_field(Node *n) {
    // `error.Name` parses as a field access. Nothing else is spelled that way,
    // so intercept it before the base is resolved as an expression.
    if (n->lhs->kind == ND_IDENT && n->lhs->name == "error" &&
        !lookup("error")) {
      int code = types.error_code(n->name);
      if (code == 0) {
        error(n->pos, "no error named '%s' is declared", n->name.c_str());
        note(n->pos, "declare it: error %s = \"what went wrong\"",
             n->name.c_str());
        return nullptr;
      }
      const TypeTable::ErrorDecl *decl = types.error_at(code);
      if (decl && decl->owner != pkg.prefix && !exported(n->name)) {
        error(n->pos, "error '%s' is not exported by %s", n->name.c_str(),
              package_shown(decl->owner).c_str());
        return nullptr;
      }
      n->kind = ND_ERROR_LIT;
      n->ival = (uint64_t)code;
      return n->type = types.error_ty;
    }

    // `Kind.Int` reads as a field access too, and the qualifier is a type.
    if (n->lhs->kind == ND_IDENT && !lookup(n->lhs->name)) {
      if (Type *named = lookup_type(n->lhs->name)) {
        if (named->kind == TY_ENUM) return check_enum_member(n, named);
      }
    }

    // `mem.MinCapacity` reads as a field access; the qualifier is a package.
    if (n->lhs->kind == ND_IDENT && !lookup(n->lhs->name)) {
      if (Package *other = imported(n->lhs->name)) {
        Symbol *sym = find_in(other->globals, n->name, false);
        if (sym && sym->const_value) {
          Node *value = sym->const_value;
          n->kind = value->kind;
          n->ival = value->ival;
          n->fval = value->fval;
          n->text = value->text;
          return n->type = value->type;
        }
        // `http.NotFound` without a call: the function itself, as a value.
        if (sym && sym->is_func) {
          n->kind = ND_IDENT;
          n->name = sym->name;
          n->sym = sym;
          return check_func_value(n, sym);
        }
        error(n->pos, "package '%s' has no exported constant or function '%s'",
              other->name.c_str(), n->name.c_str());
        return nullptr;
      }
    }

    // `json.Kind.Object`: the inner field access names the type.
    if (n->lhs->kind == ND_FIELD && n->lhs->lhs->kind == ND_IDENT &&
        !lookup(n->lhs->lhs->name)) {
      if (Package *other = imported(n->lhs->lhs->name)) {
        if (Type *named = find_in(other->type_names, n->lhs->name, false))
          if (named->kind == TY_ENUM) return check_enum_member(n, named);
      }
    }

    Type *base = check_expr(n->lhs);
    if (!base) return nullptr;
    if (base->is_error_union) {
      error(n->pos, "%s must be handled with 'try' or 'catch' first",
            type_str(base).c_str());
      return nullptr;
    }

    // A field reached through a pointer dereferences automatically.
    if (base->kind == TY_PTR) base = base->elem;

    if (base->kind == TY_SLICE || base->kind == TY_STRING ||
        base->kind == TY_ARRAY) {
      if (n->name == "len") return n->type = types.usize_ty;
      if (n->name == "ptr" && base->kind != TY_ARRAY)
        return n->type = types.rawptr(base->kind == TY_STRING ? types.u8_ty
                                                             : base->elem);
      error(n->pos, "%s has no field '%s'", type_str(base).c_str(),
            n->name.c_str());
      return nullptr;
    }

    const Field *field = find_field(base, n->name);
    if (!field) {
      error(n->pos, "%s has no field '%s'", type_str(base).c_str(),
            n->name.c_str());
      return nullptr;
    }
    if (!field_reachable(base, n->name)) {
      error(n->pos, "field '%s' of %s is not exported by its package",
            n->name.c_str(), type_str(base).c_str());
      return nullptr;
    }
    return n->type = field->type;
  }

  Type *element_of(Type *base) {
    switch (base->kind) {
    case TY_ARRAY: case TY_SLICE: case TY_RAWPTR: return base->elem;
    case TY_STRING: return types.u8_ty;
    default: return nullptr;
    }
  }

  Type *check_index(Node *n) {
    Type *base = check_expr(n->lhs);
    Type *index = check_expr(n->rhs);
    if (!base || !index) return nullptr;
    if (base->kind == TY_PTR) base = base->elem;

    Type *elem = element_of(base);
    if (!elem) {
      error(n->pos, "cannot index a value of type %s",
            type_str(base).c_str());
      return nullptr;
    }
    if (!is_integer(index)) {
      error(n->rhs->pos, "index must be an integer, got %s",
            type_str(index).c_str());
      return nullptr;
    }
    apply_type(n->rhs, types.usize_ty);
    settle(n->rhs);
    return n->type = elem;
  }

  Type *check_slice_expr(Node *n) {
    Type *base = check_expr(n->lhs);
    if (!base) return nullptr;
    if (base->kind == TY_PTR) base = base->elem;

    Type *elem = element_of(base);
    if (!elem) {
      error(n->pos, "cannot slice a value of type %s", type_str(base).c_str());
      return nullptr;
    }
    if (base->kind == TY_RAWPTR && (!n->cond || !n->rhs)) {
      error(n->pos,
            "slicing %s needs both bounds: it has no length of its own",
            type_str(base).c_str());
      return nullptr;
    }
    for (Node *bound : {n->cond, n->rhs}) {
      if (!bound) continue;
      Type *t = check_expr(bound);
      if (!t) return nullptr;
      if (!is_integer(t)) {
        error(bound->pos, "slice bound must be an integer, got %s",
              type_str(t).c_str());
        return nullptr;
      }
      apply_type(bound, types.usize_ty);
      settle(bound);
    }
    return n->type = base->kind == TY_STRING ? types.string_ty
                                             : types.slice(elem);
  }

  // Which generic struct a receiver names, if any: `*List[T]` and `List[T]`
  // both count.
  const std::string *generic_receiver(Node *type_expr) {
    Node *inner = type_expr;
    while (inner && inner->kind == ND_TYPE_PTR) inner = inner->lhs;
    if (!inner || inner->kind != ND_TYPE_INST) return nullptr;
    Node *base = inner->lhs;
    if (!base->text.empty()) return nullptr; // declared elsewhere, not here
    auto found = pkg.generic_types.find(base->name);
    return found == pkg.generic_types.end() ? nullptr : &found->first;
  }

  Type *check_struct_lit(Node *n) {
    Type *type = nullptr;
    if (n->type_expr) {
      type = resolve(n->type_expr);
    } else if (!n->text.empty()) {
      // `pkg.Type{...}`: look it up where it was declared.
      Node probe = *n;
      probe.kind = ND_TYPE_NAME;
      type = lookup_type(&probe);
    } else {
      type = lookup_type(n->name);
    }
    if (!type || type->kind != TY_STRUCT || type->is_interface) {
      error(n->pos, "'%s' is not a struct type", n->name.c_str());
      return nullptr;
    }

    // `else nil`: what the literal leaves out of an optional field it can
    // reach is written as `name: nil`, and checked as if it had been.
    if (n->fills_nil) {
      std::unordered_set<std::string> named;
      for (Node *init : n->kids) named.insert(init->name);
      for (const Field &field : type->fields) {
        if (named.count(field.name) || !is_optional(field.type) ||
            !field_reachable(type, field.name))
          continue;
        Node *init = ast.make(ND_FIELD_INIT, n->pos);
        init->name = field.name;
        init->rhs = ast.make(ND_NIL_LIT, n->pos);
        n->kids.push_back(init);
      }
    }

    std::unordered_set<std::string> seen;
    for (Node *init : n->kids) {
      const Field *field = find_field(type, init->name);
      if (!field) {
        error(init->pos, "%s has no field '%s'", n->name.c_str(),
              init->name.c_str());
        return nullptr;
      }
      if (!field_reachable(type, init->name)) {
        error(init->pos, "field '%s' of %s is not exported by its package",
              init->name.c_str(), shown_name(type->name).c_str());
        return nullptr;
      }
      if (!seen.insert(init->name).second) {
        error(init->pos, "field '%s' is set twice", init->name.c_str());
        return nullptr;
      }
      Type *value = check_expr(init->rhs);
      if (!value) return nullptr;
      if (!convert(init->rhs, field->type)) {
        error(init->rhs->pos, "field '%s' expects %s, got %s",
              init->name.c_str(), type_str(field->type).c_str(),
              type_str(value).c_str());
        return nullptr;
      }
      init->type = field->type;
    }

    for (const Field &field : type->fields) {
      if (seen.count(field.name)) continue;
      error(n->pos, "field '%s' of %s is not initialized",
            field.name.c_str(), n->name.c_str());
      bool optional_only = !n->fills_nil;
      for (const Field &other : type->fields)
        if (!seen.count(other.name) && !is_optional(other.type))
          optional_only = false;
      if (optional_only && n->kids.empty())
        note(n->pos, "write '%s{nil}' to leave every field empty",
             n->name.c_str());
      else if (optional_only)
        note(n->pos, "write 'else nil' to leave the optional fields empty");
      return nullptr;
    }
    return n->type = type;
  }

  Type *check_array_lit(Node *n) {
    Node *spec = n->type_expr;
    if (spec->kind == ND_TYPE_FUNC) {
      // `func() { ... }` in an expression is where a closure would be written.
      error(n->pos,
            "Sword has no closures: a function value names a function declared "
            "at the top of a file, and state a callback needs goes in a struct "
            "behind an interface");
      return nullptr;
    }
    if (spec->kind != ND_TYPE_ARRAY) {
      // `[]T{...}` would have to allocate, and Sword has no hidden allocation.
      error(n->pos,
            "array literals need a fixed length; write [%zu]T{...} and slice it",
            n->kids.size());
      return nullptr;
    }

    Type *elem = resolve(spec->lhs);
    // `[_]T` takes its length from the literal; anything else has it in the
    // type, which may have come from a named constant.
    int64_t count = spec->ival == (uint64_t)-1 ? (int64_t)n->kids.size()
                                               : resolve(spec)->count;
    // `[64]u8{}` is the zeroed buffer; anything else must be spelled out.
    if (n->kids.empty() && count > 0) return n->type = types.array(elem, count);
    if ((size_t)count != n->kids.size()) {
      error(n->pos, "array of length %lld initialized with %zu elements",
            (long long)count, n->kids.size());
      return nullptr;
    }

    for (Node *item : n->kids) {
      Type *t = check_expr(item);
      if (!t) return nullptr;
      if (!assignable(t, elem)) {
        error(item->pos, "array element expects %s, got %s",
              type_str(elem).c_str(), type_str(t).c_str());
        return nullptr;
      }
      apply_type(item, elem);
    }
    return n->type = types.array(elem, count);
  }

  Type *check_unary(Node *n) {
    if (n->op == TK_AMP) {
      Type *operand = check_expr(n->lhs);
      if (!operand) return nullptr;
      if (!is_place(n->lhs)) {
        error(n->pos, "cannot take the address of this expression");
        return nullptr;
      }
      return n->type = types.ptr(operand);
    }

    Type *operand = check_expr(n->lhs);
    if (!operand) return nullptr;

    switch (n->op) {
    case TK_STAR:
      if (operand->kind != TY_PTR && operand->kind != TY_RAWPTR) {
        error(n->pos, "cannot dereference a value of type %s",
              type_str(operand).c_str());
        return nullptr;
      }
      return n->type = operand->elem;
    case TK_BANG:
      if (operand->kind != TY_BOOL) {
        error(n->pos, "'!' needs a bool, got %s",
              type_str(operand).c_str());
        return nullptr;
      }
      return n->type = types.bool_ty;
    case TK_MINUS:
      if (!is_numeric(operand)) {
        error(n->pos, "'-' needs a numeric value, got %s",
              type_str(operand).c_str());
        return nullptr;
      }
      return n->type = operand;
    case TK_TILDE:
      if (!is_integer(operand)) {
        error(n->pos, "'~' needs an integer, got %s",
              type_str(operand).c_str());
        return nullptr;
      }
      return n->type = operand;
    default:
      error(n->pos, "operator '%s' is not implemented yet",
            tok_name(n->op));
      return nullptr;
    }
  }

  Type *check_try(Node *n) {
    if (!open_scopes.empty()) {
      error(n->pos, "'try' inside a 'scope' would leave without joining; do "
                    "the fallible work before the scope, or use 'catch'");
      return nullptr;
    }
    Type *t = check_expr(n->lhs);
    if (!t) return nullptr;
    if (!t->is_error_union) {
      error(n->pos, "'try' needs a value that can fail, got %s",
            type_str(t).c_str());
      return nullptr;
    }
    if (!ret_type->is_error_union) {
      error(n->pos,
            "'try' propagates an error, so this function must return '!'");
      return nullptr;
    }
    return n->type = t->elem;
  }

  Type *check_catch(Node *n) {
    Type *t = check_expr(n->lhs);
    if (!t) return nullptr;
    if (!t->is_error_union) {
      error(n->pos, "'catch' needs a value that can fail, got %s",
            type_str(t).c_str());
      return nullptr;
    }
    Type *value = t->elem;

    if (n->body) {
      push_scope();
      if (!n->name.empty())
        n->sym = declare(n->name, types.error_ty, false, n->pos);
      check_stmt(n->body);
      pop_scope();
      // The block produces no value, so when one is expected it has to leave
      // rather than fall out. With nothing to produce, falling out is fine.
      if (value->kind != TY_VOID && n->form != 1 && !always_leaves(n->body)) {
        error(n->pos,
              "this 'catch' block must return, break or continue: %s is "
              "expected here",
              type_str(value).c_str());
        return nullptr;
      }
      return n->type = value;
    }

    Type *fallback = check_expr(n->rhs);
    if (!fallback) return nullptr;
    if (!assignable(fallback, value)) {
      error(n->rhs->pos, "'catch' must produce %s, got %s",
            type_str(value).c_str(), type_str(fallback).c_str());
      return nullptr;
    }
    apply_type(n->rhs, value);
    return n->type = value;
  }

  Type *check_orelse(Node *n) {
    Type *t = check_expr(n->lhs);
    if (!t) return nullptr;
    if (!is_optional(t)) {
      error(n->pos, "'orelse' needs an optional value, got %s",
            type_str(t).c_str());
      return nullptr;
    }
    Type *value = const_cast<Type *>(opt_payload(t));

    if (n->body) {
      push_scope();
      check_stmt(n->body);
      pop_scope();
      if (value->kind != TY_VOID && n->form != 1 && !always_leaves(n->body)) {
        error(n->pos,
              "this 'orelse' block must return, break or continue: %s is "
              "expected here",
              type_str(value).c_str());
        return nullptr;
      }
      return n->type = value;
    }

    Type *fallback = check_expr(n->rhs);
    if (!fallback) return nullptr;
    if (!assignable(fallback, value)) {
      error(n->rhs->pos, "'orelse' must produce %s, got %s",
            type_str(value).c_str(), type_str(fallback).c_str());
      return nullptr;
    }
    apply_type(n->rhs, value);
    return n->type = value;
  }

  // A function named but not called is its own address. There are no closures,
  // so nothing is captured and nothing is allocated — which is also why a
  // generic has no value: there is no one function to point at.
  // A weak symbol may be missing when the program runs, and a call by name
  // would jump to address zero; the value is an optional to be looked at first.
  bool present(Node *call, Symbol *sym) {
    if (!sym->decl || !sym->decl->is_weak) return true;
    error(call->pos,
          "'%s' is weak and may be missing when the program runs; check it"
          " first: if f := %s { f(...) }",
          sym->name.c_str(), sym->name.c_str());
    return false;
  }

  Type *check_func_value(Node *n, Symbol *sym) {
    if (sym->is_generic) {
      error(n->pos,
            "'%s' is generic, so there is no single function to point at",
            sym->name.c_str());
      return nullptr;
    }
    // How the extra arguments travel is settled at the call site, from the
    // signature. A call through a value has none, and would put them where the
    // callee does not look.
    if (sym->type->is_c_variadic) {
      error(n->pos, "'%s' is variadic, so it can only be called by name",
            shown_name(sym->name).c_str());
      return nullptr;
    }
    // As a value it would be an ordinary extern func, and a call through it
    // would hold a scheduler thread with nothing on the page to say so.
    if (sym->decl && sym->decl->is_blocking) {
      error(n->pos, "'%s' is blocking, so it can only be called by name",
            shown_name(sym->name).c_str());
      return nullptr;
    }
    if (sym->decl && sym->decl->is_weak) return n->type = types.opt(sym->type);
    return n->type = sym->type;
  }

  // Calling a value rather than a name: a local, a parameter or a field of
  // function type.
  Type *check_indirect_call(Node *n, Type *signature,
                            const std::string &shown) {
    Symbol probe{shown, signature, false, false, false, nullptr,
                 nullptr, n->pos, -1, nullptr, {}};
    n->form = CALL_INDIRECT;
    n->name.clear();
    if (!check_args(n, &probe, 0)) return nullptr;
    return n->type = signature->ret;
  }

  Type *check_expr(Node *n) {
    if (!n) return nullptr;
    switch (n->kind) {
    case ND_ERROR_LIT: return n->type = types.error_ty;
    case ND_NIL_LIT: return n->type = types.untyped_nil;
    case ND_CONVERT: return check_convert(n, resolve(n->type_expr));
    case ND_ORELSE: return check_orelse(n);
    case ND_RECV: return check_recv(n);
    case ND_SEND: return check_send(n);
    case ND_TRY: return check_try(n);
    case ND_CATCH: return check_catch(n);
    case ND_INT_LIT: return n->type = types.untyped_int;
    case ND_FLOAT_LIT: return n->type = types.untyped_float;
    case ND_BOOL_LIT: return n->type = types.bool_ty;
    case ND_STRING_LIT: return n->type = types.string_ty;
    case ND_ARRAY_LIT: return check_array_lit(n);
    case ND_STRUCT_LIT: return check_struct_lit(n);
    case ND_IDENT: {
      Symbol *sym = lookup(n->name);
      if (!sym) {
        error(n->pos, "undefined identifier '%s'", n->name.c_str());
        return nullptr;
      }
      if (sym->const_value) {
        Node *value = sym->const_value;
        n->kind = value->kind;
        n->ival = value->ival;
        n->fval = value->fval;
        n->text = value->text;
        return n->type = value->type;
      }
      n->sym = sym;
      if (sym->is_func) return check_func_value(n, sym);
      return n->type = sym->type;
    }
    case ND_BINARY: return check_binary(n);
    case ND_UNARY: return check_unary(n);
    case ND_CALL: return check_call(n);
    case ND_FIELD: return check_field(n);
    case ND_INDEX: return check_index(n);
    case ND_SLICE_EXPR: return check_slice_expr(n);
    default:
      error(n->pos, "unexpected expression");
      return nullptr;
    }
  }

  // --- statements --------------------------------------------------------

  void check_condition(Node *n, const char *what) {
    Type *t = check_expr(n);
    if (t && t->kind != TY_BOOL)
      error(n->pos, "%s condition must be bool, got %s", what,
            type_str(t).c_str());
  }

  void check_assign(Node *n) {
    Type *target = check_expr(n->lhs);
    Type *value = check_expr(n->rhs);
    if (!target || !value) return;

    if (!is_place(n->lhs)) {
      error(n->lhs->pos, "cannot assign to this expression");
      return;
    }
    if (!require_mutable(n->lhs, "assign to")) return;
    if (n->op == TK_ASSIGN) alias(place_root(n->lhs), n->rhs, false);

    bool bitwise = n->op == TK_AND_ASSIGN || n->op == TK_OR_ASSIGN ||
                   n->op == TK_XOR_ASSIGN || n->op == TK_SHL_ASSIGN ||
                   n->op == TK_SHR_ASSIGN;
    if (bitwise && !is_integer(target)) {
      error(n->pos, "'%s' needs an integer target, got %s", tok_name(n->op),
            type_str(target).c_str());
      return;
    }
    if (n->op != TK_ASSIGN && !bitwise && !is_numeric(target)) {
      error(n->pos, "'%s' needs a numeric target, got %s",
            tok_name(n->op), type_str(target).c_str());
      return;
    }
    if (!convert(n->rhs, target)) {
      error(n->rhs->pos, "cannot assign %s to %s",
            type_str(value).c_str(), type_str(target).c_str());
      return;
    }
  }

  // A `return` in a fallible function has four shapes; which one it is decides
  // what lowering writes into the { code, value } pair.
  enum ReturnForm { RET_PLAIN, RET_WRAP, RET_ERROR, RET_FORWARD, RET_OK };
  // A comparison against `nil`, kept in `form`: it is a presence test rather
  // than a comparison of two values.
  enum CompareForm { CMP_VALUES, CMP_NIL };
  // How a call reaches its target, kept in `form`.
  enum CallForm {
    CALL_DIRECT, CALL_METHOD, CALL_DYNAMIC, CALL_ATOMIC, CALL_INDIRECT,
    CALL_SHARED, CALL_HASH
  };

  // Whether a place lives in this function's frame. The walk stops at the first
  // indirection: `local[i]` on an array is the frame, `local[i]` on a slice is
  // wherever the slice points, and `*p` is never here.
  bool in_this_frame(Node *place) {
    if (!place || !place->type) return false;
    switch (place->kind) {
    case ND_IDENT: {
      if (!place->sym || place->sym->is_func) return false;
      Type *t = place->sym->type;
      if (!t) return false;
      // A binding whose own value is a reference holds the reference here and
      // the thing itself elsewhere.
      return t->kind != TY_PTR && t->kind != TY_RAWPTR && t->kind != TY_SLICE &&
             t->kind != TY_STRING && t->kind != TY_FUNC;
    }
    case ND_FIELD:
      // Reaching a field through a pointer crosses out of the frame.
      if (place->lhs && place->lhs->type &&
          (place->lhs->type->kind == TY_PTR ||
           place->lhs->type->kind == TY_RAWPTR))
        return false;
      return in_this_frame(place->lhs);
    case ND_INDEX: case ND_SLICE_EXPR:
      if (place->lhs && place->lhs->type &&
          place->lhs->type->kind != TY_ARRAY)
        return false; // a slice, a string or a raw pointer: not this frame
      return in_this_frame(place->lhs);
    case ND_UNARY:
      return false; // `*p`: the frame holds the pointer, not what it points at
    default:
      return false;
    }
  }

  // A function may not hand back memory that lives in its own frame: the frame is
  // gone before the caller can look. Catches the direct shapes — `&local`,
  // `local[..]`, `&local[i]`, `&local.field` — which is where this mistake
  // actually gets made. A pointer laundered through another binding first is not
  // caught, and the documentation says so.
  bool escapes_frame(Node *value) {
    if (!value) return false;
    if (value->kind == ND_UNARY && value->op == TK_AMP)
      return in_this_frame(value->lhs);
    if (value->kind == ND_SLICE_EXPR) return in_this_frame(value);
    return false;
  }

  bool refuse_escape(Node *value) {
    if (!escapes_frame(value)) return false;
    error(value->pos, "this returns memory that lives in this function's frame");
    note(value->pos,
         "the frame is gone before the caller can read it; take the memory from"
         " an allocator, or write into something the caller gave you");
    return true;
  }

  void check_return(Node *n) {
    if (!ret_type->is_error_union) {
      n->form = RET_PLAIN;
      if (n->lhs) {
        Type *t = check_expr(n->lhs);
        if (!t) return;
        if (refuse_escape(n->lhs)) return;
        if (!convert(n->lhs, ret_type)) {
          error(n->lhs->pos,
                "cannot return %s from a function returning %s",
                type_str(t).c_str(), type_str(ret_type).c_str());
          return;
        }
      } else if (ret_type->kind != TY_VOID) {
        error(n->pos, "this function must return %s",
              type_str(ret_type).c_str());
      }
      return;
    }

    Type *payload = ret_type->elem;
    if (!n->lhs) {
      if (payload->kind != TY_VOID)
        error(n->pos, "this function must return %s",
              type_str(ret_type).c_str());
      n->form = RET_OK;
      return;
    }

    Type *t = check_expr(n->lhs);
    if (!t) return;
    if (refuse_escape(n->lhs)) return;
    if (type_eq(t, types.error_ty)) {
      n->form = RET_ERROR;
    } else if (type_eq(t, ret_type)) {
      n->form = RET_FORWARD;
    } else if (convert(n->lhs, payload)) {
      n->form = RET_WRAP;
    } else {
      error(n->lhs->pos, "cannot return %s from a function returning %s",
            type_str(t).c_str(), type_str(ret_type).c_str());
    }
  }

  // `lock c := table { ... }` holds the mutex for exactly the block. The
  // binding is a mutable pointer to the value inside, and there is no other way
  // to reach it — which is what makes the race checker's exemption safe.
  void check_lock(Node *n) {
    Type *t = check_expr(n->lhs);
    if (!t) return;
    Type *owner = t->kind == TY_PTR ? t->elem : t;
    if (!is_shared(owner)) {
      error(n->lhs->pos, "'lock' needs a shared value, got %s",
            type_str(t).c_str());
      return;
    }
    // No `mut` is asked for, the same way an atomic does not ask: the type
    // itself says it will be written by whoever holds the lock, and there is no
    // way to reach the value except by holding it.
    Symbol *root = share_root(n->lhs);
    for (const HeldLock &open : held_locks) {
      if (open.root != root || !root) continue;
      error(n->pos, "'%s' is already locked here", root->name.c_str());
      note(open.at, "the outer 'lock' on it is still open");
      return;
    }

    // A reader shares the value with other readers, so its binding is
    // immutable and nothing reached through it can be written.
    push_scope();
    n->sym =
        declare(n->name, types.ptr(owner->elem), !n->is_read, n->name_pos);
    held_locks.push_back({root, n->pos, n->is_read});
    check_stmt(n->body);
    held_locks.pop_back();
    pop_scope();
    // `if lock`: the other branch runs without the guard, and without the name.
    check_stmt(n->els);
  }

  // A task is a call and nothing else: everything it can reach is written on
  // the spawn line, which is what makes the sharing rules checkable.
  void check_spawn(Node *n) {
    if (open_scopes.empty() || open_scopes.back()->kind != ND_SCOPE) {
      error(n->pos, "'spawn' only exists directly inside a 'scope'");
      return;
    }
    // A task started under a lock the scope does not sit inside would still be
    // running after the lock is released.
    if (held_locks.size() > scope_lock_base.back()) {
      error(n->pos, "this task would outlive the 'lock' it starts under");
      note(held_locks.back().at,
           "put the 'scope' inside the 'lock' block, or the 'lock' inside the"
           " task");
      return;
    }
    if (n->lhs->kind != ND_CALL) {
      error(n->pos, "'spawn' takes a function call");
      return;
    }

    Type *result = check_expr(n->lhs);
    if (!result) return;
    if (n->lhs->form != 0) {
      error(n->pos, "'spawn' cannot call a method yet; call a function");
      return;
    }

    bool fails = result->is_error_union;
    if (fails && result->elem->kind != TY_VOID) {
      error(n->pos,
            "a spawned function cannot return a value, only '!void'; write "
            "the result into memory the caller owns");
      return;
    }
    if (!fails && result->kind != TY_VOID) {
      error(n->pos,
            "a spawned function cannot return a value; write it into memory "
            "the caller owns");
      return;
    }
    if (fails) open_scopes.back()->form = 1;

    // What the task can reach is exactly what is written on this line, and
    // `mut` on the parameter says whether it may write.
    bool in_loop = !scope_loop_base.empty() &&
                   loop_depth > scope_loop_base.back();
    Symbol *callee = n->lhs->sym;
    for (size_t i = 0; i < n->lhs->kids.size(); i++) {
      Type *param = callee->type->params[i];
      if (!shares_memory(param)) continue;
      Node *decl = callee->decl ? callee->decl->kids[i] : nullptr;
      for (Access use : access_of(n->lhs->kids[i], decl && decl->is_mut)) {
        use.repeated = in_loop;
        use.own_piece = use.chunk && use.chunk->chunk_depth == loop_depth &&
                        loop_depth == scope_loop_base.back() + 1;
        use.arms = arms;
        record_use(use);
      }
    }
  }

  // `for part in xs.chunks(n)` is the only way to hand pieces of one slice to
  // different tasks: the pieces cannot overlap, so the compiler does not have
  // to prove anything about the indices.
  // `switch x { case a, b: ... default: ... }`. No fallthrough, so `break` and
  // `continue` inside a case mean what they would anywhere else: they speak to
  // the loop around the switch, not to the switch.
  void check_switch(Node *n) {
    Type *subject = check_expr(n->cond);
    if (!subject) return;
    if (!is_integer(subject) && subject->kind != TY_BOOL &&
        subject->kind != TY_STRING && subject->kind != TY_ENUM) {
      error(n->cond->pos,
            "switch needs an integer, a bool, a string or an enum, got %s",
            type_str(subject).c_str());
      return;
    }
    subject = settle(n->cond);

    Node *fallback = nullptr;
    std::vector<Node *> values;
    std::unordered_set<int64_t> matched;
    int which = 0;
    for (Node *arm : n->kids) {
      if (arm->kids.empty()) {
        if (fallback) {
          error(arm->pos, "this switch already has a 'default'");
          note(fallback->pos, "the first one is here");
        }
        fallback = arm;
      }
      for (Node *value : arm->kids) {
        if (!check_expr(value)) continue;
        if (!convert(value, subject)) {
          error(value->pos, "cannot match %s against %s",
                type_str(value->type).c_str(), type_str(subject).c_str());
          continue;
        }
        for (Node *seen : values) {
          if (!same_constant(seen, value)) continue;
          error(value->pos, "this case is already covered");
          note(seen->pos, "the earlier one is here");
          break;
        }
        values.push_back(value);
        if (value->kind == ND_INT_LIT) matched.insert((int64_t)value->ival);
      }
      push_scope();
      check_arm(n, which++, arm->body);
      pop_scope();
    }

    // An enum is a closed set, so a switch over one without a `default` has to
    // account for every member. This is the whole reason to name the set.
    if (subject->kind == TY_ENUM && !fallback) {
      std::string missing;
      int left = 0;
      for (const Field &m : subject->fields) {
        if (matched.count(m.offset)) continue;
        left++;
        if (left <= 3) missing += (missing.empty() ? "" : ", ") + m.name;
      }
      if (left > 0) {
        if (left > 3) missing += ", and " + std::to_string(left - 3) + " more";
        error(n->pos, "this switch over %s does not cover %s",
              type_str(subject).c_str(), missing.c_str());
        note(n->pos, "add the missing cases, or a 'default'");
      }
    }
  }

  // Only literals, which is all the checker can compare without evaluating.
  // A constant name has already been folded into one by the time it gets here.
  bool same_constant(Node *a, Node *b) {
    if (a->kind != b->kind) return false;
    switch (a->kind) {
    case ND_INT_LIT: case ND_BOOL_LIT: return a->ival == b->ival;
    case ND_STRING_LIT: return a->text == b->text;
    default: return false;
    }
  }

  // Which of the two named `for` loops this is, kept in `form`.
  enum ForForm { FOR_PARTITION, FOR_ELEMENTS, FOR_OPTIONAL };

  // `for x in xs` and `for part in xs.chunks(n)` are the same syntax, so which
  // loop it is comes from what is being walked.
  bool check_walk(Node *n) {
    Node *over = n->lhs;
    if (over->kind == ND_CALL && over->lhs->kind == ND_FIELD &&
        over->lhs->name == "chunks")
      return check_partition(n);
    return check_elements(n);
  }

  // One element at a time, copied, out of a slice, an array or a string. `for
  // i, x in xs` binds the position as well.
  bool check_elements(Node *n) {
    Type *type = check_expr(n->lhs);
    if (!type) return false;

    Type *elem = nullptr;
    if (type->kind == TY_SLICE || type->kind == TY_ARRAY) elem = type->elem;
    else if (type->kind == TY_STRING) elem = types.u8_ty;
    else {
      error(n->lhs->pos,
            "'for %s in ...' walks a slice, an array, a string or a partition "
            "written 'xs.chunks(n)'; for a range write 'for %s in a..b'",
            n->name.c_str(), n->name.c_str());
      return false;
    }
    if (!n->text.empty()) {
      error(n->pos, "a loop over elements has no reduction");
      return false;
    }

    n->type = type;
    n->form = FOR_ELEMENTS;
    // The element is a copy, so it is its own value and immutable the same way
    // a range variable is. Write through the slice to change anything.
    n->sym = declare(n->name, elem, false, n->name_pos);
    // An element that is itself a pointer reaches what the slice's owner does.
    alias(n->sym, n->lhs, true);
    if (!n->name2.empty())
      n->index_sym = declare(n->name2, types.usize_ty, false, n->name_pos);
    return true;
  }

  bool check_partition(Node *n) {
    Node *call = n->lhs;
    if (call->kids.size() != 1) {
      error(call->pos, "chunks() takes one size");
      return false;
    }
    n->form = FOR_PARTITION;

    Node *base = call->lhs->lhs;
    Type *type = check_expr(base);
    if (!type) return false;
    if (type->kind != TY_SLICE) {
      error(base->pos, "chunks() needs a slice, got %s", type_str(type).c_str());
      return false;
    }
    if (!is_place(base)) {
      error(base->pos, "chunks() needs a slice with a name behind it");
      return false;
    }

    Node *size = call->kids[0];
    Type *size_type = check_expr(size);
    if (!size_type) return false;
    if (!is_integer(size_type)) {
      error(size->pos, "chunk size must be an integer, got %s",
            type_str(size_type).c_str());
      return false;
    }
    apply_type(size, types.usize_ty);
    settle(size);

    // Flatten the call into base and size so lowering sees a plain loop.
    n->lhs = base;
    n->rhs = size;
    n->type = type;

    if (!n->text.empty()) {
      error(n->pos, "a partition loop has no reduction");
      return false;
    }

    Symbol *root = place_root(base);
    n->sym = declare(n->name, type, root && root->is_mut, n->pos);
    n->sym->chunk_of = root;
    n->sym->chunk_depth = loop_depth + 1;
    if (!n->name2.empty())
      n->index_sym = declare(n->name2, types.usize_ty, false, n->pos);
    return true;
  }

  // Each worker accumulates into a private copy, so the operator has to have
  // an identity and combine associatively.
  bool check_reduction(Node *n) {
    Symbol *sym = lookup(n->text);
    if (!sym) {
      error(n->pos, "undefined identifier '%s' in the reduction",
            n->text.c_str());
      return false;
    }
    if (!sym->is_mut) {
      error(n->pos, "'%s' is written by the reduction, so it must be 'mut'",
            n->text.c_str());
      note(sym->pos, "declare it with 'mut' to allow mutation");
      return false;
    }
    if (!reduction_kind(n, sym->type)) return false;
    n->reduce_sym = sym;
    return true;
  }

  // Each piece of the range starts from the operator's identity and keeps its
  // own answer; the pieces are combined in the order they were cut, which is
  // what makes the result the same however many threads ran it.
  bool reduction_kind(Node *n, Type *type) {
    bool floating = is_float(type);
    std::string named = n->name2;

    // A name that is not `min` or `max` is a function's: the program says how
    // two partial answers are combined, and the compiler only has to call it.
    if (!named.empty() && named != "min" && named != "max")
      return user_reduction(n, type, named);

    if (!is_integer(type) && !is_float(type)) {
      error(n->pos, "a reduction needs a number, got %s — or a function to "
                    "combine two of them",
            type_str(type).c_str());
      return false;
    }

    if (n->op == TK_PLUS) {
      n->reduce_kind = floating ? RMW_FADD : RMW_ADD;
      n->ival = 0;
      n->fval = 0;
      return true;
    }
    if (floating) {
      error(n->pos, "only '+' reduces a float, got '%s'",
            named.empty() ? tok_name(n->op) : named.c_str());
      return false;
    }
    if (n->op == TK_PIPE) {
      n->reduce_kind = RMW_OR;
      n->ival = 0;
      return true;
    }
    if (n->op == TK_AMP) {
      n->reduce_kind = RMW_AND;
      n->ival = (uint64_t)-1; // every bit set is the identity for and
      return true;
    }
    if (named == "min" || named == "max") {
      bool lowest = named == "max";
      n->reduce_kind = lowest ? RMW_MAX : RMW_MIN;
      n->ival = extreme(type, lowest);
      return true;
    }
    error(n->pos, "'%s' is not a reduction; use +, &, |, min, max, or the name"
                  " of a function that combines two",
          named.empty() ? tok_name(n->op) : named.c_str());
    return false;
  }

  // `reduce(num.Merge: total)`. The function combines two partial answers into
  // one, and each piece of the range starts from the zero value of the type —
  // which is why this is for the kinds of accumulator where zero means
  // "nothing yet", and why `&` and `min` keep their own identities.
  bool user_reduction(Node *n, Type *type, const std::string &named) {
    size_t dot = named.find('.');
    Symbol *fn = nullptr;
    if (dot == std::string::npos) {
      fn = lookup(named);
    } else if (Package *other = imported(named.substr(0, dot))) {
      fn = find_in(other->globals, named.substr(dot + 1), false);
    }
    if (!fn || !fn->is_func) {
      error(n->pos, "'%s' is not a function, so it cannot combine two %s",
            named.c_str(), type_str(type).c_str());
      return false;
    }
    if (fn->is_generic) {
      error(n->pos, "'%s' is generic, so there is no single function to call",
            named.c_str());
      return false;
    }
    Type *sig = fn->type;
    bool fits = sig && sig->kind == TY_FUNC && sig->params.size() == 2 &&
                !sig->is_c_variadic && type_eq(sig->params[0], type) &&
                type_eq(sig->params[1], type) && type_eq(sig->ret, type);
    if (!fits) {
      error(n->pos,
            "a reduction over %s needs a func(%s, %s) %s to combine two of"
            " them; '%s' is %s",
            type_str(type).c_str(), type_str(type).c_str(),
            type_str(type).c_str(), type_str(type).c_str(), named.c_str(),
            type_str(sig).c_str());
      return false;
    }
    for (size_t i = 0; i < sig->param_mut.size(); i++) {
      if (!sig->param_mut[i]) continue;
      error(n->pos,
            "'%s' writes through a parameter; a reduction combines two values"
            " and answers a third",
            named.c_str());
      return false;
    }
    n->reduce_kind = kReduceUser;
    n->reduce_fn = fn;
    return true;
  }

  // The smallest value of the type when `lowest`, the largest otherwise.
  static uint64_t extreme(const Type *type, bool lowest) {
    int bits = type->bits;
    if (!type->is_signed) return lowest ? 0 : (bits == 64 ? (uint64_t)-1
                                                          : ((uint64_t)1 << bits) - 1);
    uint64_t top = (uint64_t)1 << (bits - 1);
    // Negating the signed minimum is undefined, so the two's complement is
    // built in unsigned arithmetic, where wrapping is defined.
    return lowest ? (uint64_t)0 - top : top - 1;
  }

  void check_stmt(Node *n) {
    if (!n) return;
    switch (n->kind) {
    case ND_BLOCK:
      push_scope();
      for (Node *st : n->kids) check_stmt(st);
      pop_scope();
      break;

    case ND_VAR: {
      Type *declared = n->type_expr ? resolve(n->type_expr) : nullptr;
      Type *t = check_expr(n->rhs);
      if (!t) return;
      if (declared) {
        if (!convert(n->rhs, declared)) {
          error(n->rhs->pos, "cannot initialize %s with %s",
                type_str(declared).c_str(), type_str(t).c_str());
          return;
        }
        t = declared;
      } else {
        if (t->kind == TY_VOID) {
          error(n->pos, "'%s' cannot be bound to a void value",
                n->name.c_str());
          return;
        }
        if (t->kind == TY_OPT && t->untyped) {
          error(n->pos,
                "'%s := nil' has no type to infer; declare it, as in "
                "'%s ?*T = nil'",
                n->name.c_str(), n->name.c_str());
          return;
        }
        t = settle(n->rhs);
      }
      n->type = t;
      n->sym = declare(n->name, t, n->is_mut, n->pos);
      alias(n->sym, n->rhs, true);
      break;
    }

    case ND_ASSIGN:
      check_assign(n);
      break;

    case ND_RETURN:
      if (leaving_scope(n, "return")) return;
      check_return(n);
      break;

    case ND_SCOPE:
      open_scopes.push_back(n);
      scope_loop_base.push_back(loop_depth);
      scope_lock_base.push_back(held_locks.size());
      scope_uses.emplace_back();
      check_stmt(n->body);
      check_parent_uses(n->body);
      scope_uses.pop_back();
      scope_lock_base.pop_back();
      scope_loop_base.pop_back();
      open_scopes.pop_back();
      // A scope that can observe a failure has to have somewhere to report it.
      if (n->form == 1 && !ret_type->is_error_union)
        error(n->pos,
              "this 'scope' can fail, so the function around it must return '!'");
      break;

    case ND_SPAWN:
      check_spawn(n);
      break;

    case ND_LOCK:
      check_lock(n);
      break;

    case ND_SELECT:
      check_select(n);
      break;

    case ND_SWITCH:
      check_switch(n);
      break;

    case ND_DEFER:
      check_stmt(n->body);
      if (n->is_errdefer && !ret_type->is_error_union)
        error(n->pos,
              "'errdefer' only makes sense where an error can happen");
      break;

    case ND_IF:
      // `if x := opt { ... }` runs the body with the unwrapped value bound.
      if (!n->name.empty()) {
        Type *t = check_expr(n->cond);
        if (!t) return;
        if (!is_optional(t)) {
          error(n->cond->pos,
                "'if %s := ...' needs an optional value, got %s",
                n->name.c_str(), type_str(t).c_str());
          return;
        }
        push_scope();
        n->sym = declare(n->name, const_cast<Type *>(opt_payload(t)),
                         n->is_mut, n->pos);
        alias(n->sym, n->cond, true);
        check_arm(n, 0, n->body);
        pop_scope();
        check_arm(n, 1, n->els);
        break;
      }
      check_condition(n->cond, "if");
      check_arm(n, 0, n->body);
      check_arm(n, 1, n->els);
      break;

    case ND_FOR:
      push_scope();
      // `for x := optional { }`: run while it has something, with the value
      // bound, the same way `if x := optional` unwraps once.
      if (n->form == FOR_OPTIONAL) {
        Type *t = check_expr(n->cond);
        if (!t) { pop_scope(); return; }
        if (!is_optional(t)) {
          error(n->cond->pos,
                "'for %s := ...' needs an optional value, got %s",
                n->name.c_str(), type_str(t).c_str());
          pop_scope();
          return;
        }
        n->sym = declare(n->name, const_cast<Type *>(opt_payload(t)),
                         n->is_mut, n->name_pos);
        alias(n->sym, n->cond, true);
        loop_depth++;
        check_stmt(n->body);
        loop_depth--;
        pop_scope();
        break;
      }
      if (!n->is_range && !n->name.empty()) {
        if (!check_walk(n)) {
          pop_scope();
          return;
        }
        loop_depth++;
        check_stmt(n->body);
        loop_depth--;
        pop_scope();
        break;
      }
      if (n->is_parallel && !n->is_range) {
        error(n->pos, "'parallel for' needs a range, as in 'for i in 0..n'");
        pop_scope();
        return;
      }
      if (n->is_range) {
        Type *lo = check_expr(n->lhs);
        Type *hi = check_expr(n->rhs);
        if (!lo || !hi) { pop_scope(); return; }
        if (lo->untyped && !hi->untyped) { apply_type(n->lhs, hi); lo = hi; }
        else if (hi->untyped) { apply_type(n->rhs, lo); }
        lo = settle(n->lhs);
        settle(n->rhs);
        if (!is_integer(lo)) {
          error(n->lhs->pos, "range bounds must be integers, got %s",
                type_str(lo).c_str());
          pop_scope();
          return;
        }
        n->type = lo;
        // The loop variable is a fresh immutable binding each iteration.
        n->sym = declare(n->name, lo, false, n->pos);
      } else if (n->cond) {
        check_condition(n->cond, "for");
      }
      if (n->is_parallel) {
        // Iterations run on different workers, so the body may not carry
        // control out of the loop any more than a task body may.
        open_scopes.push_back(n);
        scope_loop_base.push_back(loop_depth);
        scope_lock_base.push_back(held_locks.size());
        if (!n->text.empty() && !check_reduction(n)) {
          scope_lock_base.pop_back();
          scope_loop_base.pop_back();
          open_scopes.pop_back();
          pop_scope();
          return;
        }
      }
      loop_depth++;
      if (n->is_parallel) parallel_bodies.push_back(loop_depth);
      check_stmt(n->body);
      if (n->is_parallel) parallel_bodies.pop_back();
      loop_depth--;
      if (n->is_parallel) {
        check_parallel_body(n);
        scope_lock_base.pop_back();
        scope_loop_base.pop_back();
        open_scopes.pop_back();
      }
      pop_scope();
      break;

    case ND_BREAK:
    case ND_CONTINUE:
      if (loop_depth == 0) {
        error(n->pos, "'%s' outside of a loop",
              n->kind == ND_BREAK ? "break" : "continue");
      } else if (n->kind == ND_BREAK && !parallel_bodies.empty() &&
                 loop_depth == parallel_bodies.back()) {
        error(n->pos, "a 'parallel for' has no order to break out of; "
                      "'continue' skips one index");
      } else if (!scope_loop_base.empty() &&
                 loop_depth <= scope_loop_base.back()) {
        leaving_scope(n, n->kind == ND_BREAK ? "break" : "continue");
      }
      break;

    case ND_EXPR_STMT: {
      // Nothing consumes the result here, so a handler block has nothing to
      // produce and may simply fall out.
      if (n->lhs->kind == ND_CATCH || n->lhs->kind == ND_ORELSE)
        n->lhs->form = 1;
      Type *t = check_expr(n->lhs);
      // Discarding a fallible result would be ignoring an error by accident,
      // which the language does not allow.
      if (t && t->is_error_union)
        error(n->lhs->pos,
              "%s cannot be discarded; handle it with 'try' or 'catch'",
              type_str(t).c_str());
      else if (t && !does_something(n->lhs))
        error(n->lhs->pos,
              "this value is worked out and then thrown away; a statement on "
              "its own has to call something, send or receive");
      break;
    }

    default:
      error(n->pos, "unexpected statement");
      break;
    }
  }

  // --- declarations ------------------------------------------------------

  // Does this statement definitely leave the function? Used to require a
  // return on every path without building a full control-flow graph.
  bool always_returns(Node *n) {
    if (!n) return false;
    switch (n->kind) {
    case ND_RETURN: return true;
    case ND_BLOCK:
      for (Node *st : n->kids)
        if (always_returns(st)) return true;
      return false;
    case ND_IF:
      return n->els && always_returns(n->body) && always_returns(n->els);
    case ND_FOR:
      return !n->is_range && !n->cond; // `for {}` only exits via return
    // Leaving a `lock` releases it on the way out, so a `return` inside one
    // counts the same as a `return` in a plain block.
    case ND_LOCK:
      if (n->is_try)
        return n->els && always_returns(n->body) && always_returns(n->els);
      return always_returns(n->body);
    // Only with a `default`: without one, falling past every case is a path.
    case ND_SWITCH: {
      // An exhaustive switch over an enum needs no default: the checker has
      // already made sure there is no path past it.
      bool closed = n->cond->type && n->cond->type->kind == TY_ENUM;
      for (Node *arm : n->kids) {
        if (!always_returns(arm->body)) return false;
        if (arm->kids.empty()) closed = true;
      }
      return closed;
    }
    default: return false;
    }
  }

  // Like always_returns, but `break` and `continue` count too: they are enough
  // to leave a `catch` block behind.
  bool always_leaves(Node *n) {
    if (!n) return false;
    switch (n->kind) {
    case ND_RETURN: case ND_BREAK: case ND_CONTINUE:
      return true;
    case ND_BLOCK:
      for (Node *st : n->kids)
        if (always_leaves(st)) return true;
      return false;
    case ND_IF:
      return n->els && always_leaves(n->body) && always_leaves(n->els);
    case ND_LOCK:
      if (n->is_try)
        return n->els && always_leaves(n->body) && always_leaves(n->els);
      return always_leaves(n->body);
    default:
      return always_returns(n);
    }
  }

  // What may stand alone as a statement: something with an effect. Separators
  // are optional, so `c := a ~ b` reads as `c := a` and then `~b`, and only
  // refusing the second keeps a typo from compiling into something else.
  static bool does_something(Node *e) {
    if (!e) return false;
    switch (e->kind) {
    case ND_CALL: case ND_SEND: case ND_RECV:
      return true;
    case ND_TRY: case ND_CATCH: case ND_ORELSE:
      return does_something(e->lhs);
    default:
      return false;
    }
  }

  void check_func(Node *fn) {
    // The signature pass gives up on a bad declaration without leaving a
    // symbol behind; there is nothing to check here.
    if (fn->is_extern || !fn->sym) return;
    ret_type = fn->sym->type->ret;
    push_scope();
    for (Node *p : fn->kids)
      p->sym = declare(p->name, p->type, p->is_mut, p->pos);
    check_stmt(fn->body);
    pop_scope();

    // A fallible function carrying no value can fall off the end: that path is
    // simply success.
    Type *payload = ret_type->is_error_union ? ret_type->elem : ret_type;
    if (payload->kind != TY_VOID && !always_returns(fn->body))
      error(fn->pos, "'%s' must return %s on every path",
            fn->name.c_str(), type_str(ret_type).c_str());
  }

  // A struct cannot contain itself by value, directly or through another
  // struct: its size would be infinite.
  bool contains_cycle(Type *type, std::vector<Type *> &open) {
    for (Type *seen : open)
      if (seen == type) return true;
    open.push_back(type);
    for (const Field &f : type->fields) {
      Type *t = f.type;
      while (t->kind == TY_ARRAY) t = t->elem;
      if (t->kind == TY_STRUCT && contains_cycle(t, open)) return true;
    }
    open.pop_back();
    return false;
  }

  // An enum is a named set of integers with its own type, so nothing arithmetic
  // reaches it by accident. A member with no value continues from the one
  // before, starting at zero.
  bool declare_enums() {
    std::vector<Node *> made;
    for (Node *decl : pkg.unit->kids) {
      if (decl->kind != ND_ENUM_DECL) continue;
      if (pkg.type_names.count(decl->name) || types.named(decl->name) ||
          aliases.count(decl->name)) {
        error(decl->pos, "'%s' is already a type", decl->name.c_str());
        return false;
      }
      Type *width = decl->type_expr ? resolve(decl->type_expr) : types.int_ty;
      if (!is_integer(width)) {
        error(decl->type_expr->pos, "an enum counts in integers, not %s",
              type_str(width).c_str());
        return false;
      }
      Type *type = types.declare_enum(pkg.prefix + decl->name, width);
      decl->type = type;
      pkg.type_names[decl->name] = type;

      std::vector<Field> members;
      int64_t next = 0;
      std::unordered_set<std::string> seen;
      for (Node *m : decl->kids) {
        if (!seen.insert(m->name).second) {
          error(m->pos, "duplicate member '%s'", m->name.c_str());
          return false;
        }
        if (m->rhs) {
          Node *folded = fold(m->rhs);
          if (!folded) return false;
          if (folded->kind != ND_INT_LIT) {
            error(m->rhs->pos, "an enum member is an integer");
            return false;
          }
          next = (int64_t)folded->ival;
        }
        for (const Field &earlier : members) {
          if (earlier.offset != next) continue;
          error(m->pos, "'%s' and '%s' are the same value",
                earlier.name.c_str(), m->name.c_str());
          return false;
        }
        members.push_back(Field{m->name, type, next, (int)members.size()});
        next++;
      }
      type->fields = std::move(members);
      made.push_back(made_name_func(decl));
    }
    // Appended after the walk, not during it.
    for (Node *fn : made) pkg.unit->kids.push_back(fn);
    return true;
  }

  Node *made_ident(Pos at, const std::string &name) {
    Node *n = ast.make(ND_IDENT, at);
    n->name = name;
    n->name_pos = at;
    return n;
  }

  Node *made_type_name(Pos at, const std::string &name) {
    Node *n = ast.make(ND_TYPE_NAME, at);
    n->name = name;
    n->name_pos = at;
    return n;
  }

  Node *made_return(Pos at, const std::string &text) {
    Node *value = ast.make(ND_STRING_LIT, at);
    value->text = text;
    Node *ret = ast.make(ND_RETURN, at);
    ret->lhs = value;
    Node *block = ast.make(ND_BLOCK, at);
    block->kids.push_back(ret);
    return block;
  }

  // Every enum gets a function from a value to its member's name, which is what
  // `nameof` calls. It is written as source rather than as IR, so the ordinary
  // pipeline checks and lowers it like anything else — and its name carries a
  // dot, which no identifier can, so nothing can call it by hand.
  Node *made_name_func(Node *decl) {
    Node *fn = ast.make(ND_FUNC, decl->pos);
    fn->name = decl->name + ".name";
    fn->name_pos = decl->name_pos;
    fn->is_hidden = true;

    Node *param = ast.make(ND_PARAM, decl->pos);
    param->name = "v";
    param->name_pos = decl->pos;
    param->type_expr = made_type_name(decl->pos, decl->name);
    fn->kids.push_back(param);
    fn->type_expr = made_type_name(decl->pos, "string");

    Node *sw = ast.make(ND_SWITCH, decl->pos);
    sw->cond = made_ident(decl->pos, "v");
    for (Node *m : decl->kids) {
      Node *arm = ast.make(ND_CASE, m->pos);
      Node *value = ast.make(ND_FIELD, m->pos);
      value->lhs = made_ident(m->pos, decl->name);
      value->name = m->name;
      value->name_pos = m->name_pos;
      arm->kids.push_back(value);
      arm->body = made_return(m->pos, m->name);
      sw->kids.push_back(arm);
    }
    // A value that is not a member has no name, which is what an empty string
    // says. The conversion into an enum is unchecked, so this is reachable.
    Node *fallback = ast.make(ND_CASE, decl->pos);
    fallback->body = made_return(decl->pos, "");
    sw->kids.push_back(fallback);

    Node *body = ast.make(ND_BLOCK, decl->pos);
    body->kids.push_back(sw);
    fn->body = body;
    return fn;
  }

  // A member named through its type: `Kind.Int`. Nothing else is spelled that
  // way, so it does not collide with a field or a package.
  const Field *enum_member(const Type *type, const std::string &name) {
    if (!type || type->kind != TY_ENUM) return nullptr;
    for (const Field &m : type->fields)
      if (m.name == name) return &m;
    return nullptr;
  }

  bool declare_structs() {
    std::vector<Node *> decls;
    std::vector<Node *> ifaces;
    for (Node *decl : pkg.unit->kids) {
      bool is_struct = decl->kind == ND_STRUCT_DECL;
      if (!is_struct && decl->kind != ND_INTERFACE_DECL) continue;
      if (pkg.type_names.count(decl->name) || types.named(decl->name) ||
          aliases.count(decl->name)) {
        error(decl->pos, "'%s' is already a type", decl->name.c_str());
        return false;
      }
      // A generic struct is a template, not a type: it has no layout until
      // somebody names its arguments.
      if (is_struct && !decl->tparams.empty()) {
        pkg.generic_types[decl->name] = decl;
        continue;
      }

      // The registered name is qualified, so two packages may each have a
      // `Node` without colliding in the type table or in the object file.
      if (is_struct) {
        Type *type = types.declare_struct(pkg.prefix + decl->name);
        type->is_extern = decl->is_extern;
        decl->type = type;
        decls.push_back(decl);
      } else {
        decl->type = types.declare_interface(pkg.prefix + decl->name);
        ifaces.push_back(decl);
      }
      pkg.type_names[decl->name] = decl->type;
    }

    for (Node *decl : ifaces) {
      std::unordered_set<std::string> seen;
      for (Node *m : decl->kids) {
        if (!seen.insert(m->name).second) {
          error(m->pos, "duplicate method '%s'", m->name.c_str());
          return false;
        }
        std::vector<Type *> params;
        std::vector<bool> param_mut;
        for (Node *p : m->kids) {
          p->type = resolve(p->type_expr);
          params.push_back(p->type);
          param_mut.push_back(p->is_mut);
        }
        Type *ret = m->type_expr ? resolve(m->type_expr) : types.void_ty;
        decl->type->methods.push_back(
            Field{m->name, types.func(params, ret, param_mut), 0, 0, m->is_mut});
      }
    }

    // Names exist before fields are resolved, so structs can refer to each
    // other through pointers in any order.
    for (Node *decl : decls) {
      std::vector<Field> fields;
      std::unordered_set<std::string> seen;
      for (Node *f : decl->kids) {
        if (!seen.insert(f->name).second) {
          error(f->pos, "duplicate field '%s'", f->name.c_str());
          return false;
        }
        Field field;
        field.name = f->name;
        field.type = resolve(f->type_expr);
        fields.push_back(field);
      }
      types.layout_struct(decl->type, std::move(fields));
    }

    for (Node *decl : decls) {
      std::vector<Type *> open;
      if (contains_cycle(decl->type, open)) {
        error(decl->pos, "struct '%s' contains itself by value",
              decl->name.c_str());
        return false;
      }
    }

    // Sizes are settled rather than computed once. Laying out in declaration
    // order gave a struct holding one declared further down the file a size that
    // was missing it — silently, and only visible as a copy that moved too few
    // bytes. Cycles are already ruled out above, so this converges.
    types.settle_layouts();
    return true;
  }

  // A package prefix reads as the import made it visible: `std.net.` is `net`.
  static std::string package_shown(const std::string &prefix) {
    std::string name = prefix;
    if (!name.empty() && name.back() == '.') name.pop_back();
    if (name.empty()) return "this program";
    return shown_name(name);
  }

  // `error Timeout = "..."`. Codes are global to the program so that the same
  // name raised in two packages is one error; the message is global with them,
  // which is why two packages disagreeing about it is a mistake worth reporting.
  bool declare_errors() {
    for (Node *decl : pkg.unit->kids) {
      if (decl->kind != ND_ERROR_DECL) continue;
      for (Node *one : decl->kids) {
        const TypeTable::ErrorDecl *clash = nullptr;
        int code = types.declare_error(one->name, one->text, pkg.prefix, &clash);
        if (clash) {
          error(one->pos,
                "error '%s' is already declared with a different message",
                one->name.c_str());
          note(one->pos, "%s says \"%s\"", package_shown(clash->owner).c_str(),
               clash->message.c_str());
          return false;
        }
        pkg.error_codes[one->name] = code;
      }
    }
    return true;
  }

  bool run() {
    push_scope();
    if (!declare_errors()) return false;
    // Constants come first: a struct field may be an array whose length is
    // one of them.
    declare_constants();
    if (!declare_aliases()) return false;
    // Enums before structs: a field may be one.
    if (!declare_enums()) return false;
    if (!declare_structs()) return false;
    if (!settle_aliases()) return false;

    // Signatures first: within a package, declaration order does not matter.
    for (Node *fn : pkg.unit->kids) {
      if (fn->kind != ND_FUNC) continue;

      // A method becomes an ordinary function whose first parameter is the
      // receiver, named Type.method. Everything downstream then treats it as
      // any other function.
      // A method on a generic struct waits with it: its receiver type does
      // not exist until the struct is instantiated.
      if (fn->lhs) {
        if (const std::string *on = generic_receiver(fn->lhs->type_expr)) {
          pkg.generic_methods[*on].push_back(fn);
          continue;
        }
      }

      std::string method_of;
      if (fn->lhs) {
        Type *recv = resolve(fn->lhs->type_expr);
        Type *owner = recv->kind == TY_PTR ? recv->elem : recv;
        if (owner->kind != TY_STRUCT || owner->is_error_union ||
            owner->is_optional) {
          error(fn->lhs->pos, "%s cannot have methods",
                type_str(owner).c_str());
          continue;
        }
        if (fn->is_extern) {
          error(fn->pos, "an extern function cannot have a receiver");
          continue;
        }
        method_of = owner->name;
        fn->kids.insert(fn->kids.begin(), fn->lhs);
      }

      // A generic has no signature until it is instantiated: its parameter
      // types mention type parameters that are not types yet.
      if (!fn->tparams.empty()) {
        if (!method_of.empty()) {
          error(fn->pos, "a method cannot be generic yet");
          continue;
        }
        std::string plain = fn->name;
        Symbol *sym =
            declare(plain, types.func({}, types.void_ty), false, fn->pos);
        sym->is_func = true;
        sym->is_generic = true;
        fn->name = pkg.prefix + plain;
        sym->name = fn->name;
        sym->decl = fn;
        fn->sym = sym;
        pkg.globals[plain] = sym;
        continue;
      }

      std::vector<Type *> params;
      std::vector<bool> param_mut;
      for (size_t i = 0; i < fn->kids.size(); i++) {
        Node *p = fn->kids[i];
        p->type = resolve(p->type_expr);
        if (p->is_variadic) {
          if (i + 1 != fn->kids.size()) {
            error(p->pos, "only the last parameter can gather the rest");
            continue;
          }
          p->type = types.slice(p->type);
        }
        params.push_back(p->type);
        param_mut.push_back(p->is_mut);
      }
      Type *ret = fn->type_expr ? resolve(fn->type_expr) : types.void_ty;

      Symbol *sym;
      if (!method_of.empty()) {
        auto &table = prog.methods[method_of];
        if (table.count(fn->name)) {
          error(fn->pos, "%s already has a method '%s'",
                method_of.c_str(), fn->name.c_str());
          continue;
        }
        std::string plain = fn->name;
        fn->name = method_of + "." + plain;
        sym = ast.make_symbol(fn->name, types.func(params, ret, param_mut),
                              false, fn->pos);
        table[plain] = sym;
      } else {
        std::string plain = fn->name;
        sym = declare(plain, types.func(params, ret, param_mut), false,
                      fn->pos);
        // The name function of an enum is found by its type, not by its name.
        if (fn->is_hidden && !params.empty() && params[0]->kind == TY_ENUM)
          prog.enum_names[params[0]] = sym;
        // Extern names are C symbols and stay as written; everything else is
        // qualified so two packages can both define `init`.
        if (!fn->is_extern) fn->name = pkg.prefix + plain;
        sym->name = fn->name;
        pkg.globals[plain] = sym;
      }
      sym->is_func = true;
      sym->decl = fn;
      fn->sym = sym;
      if (!method_of.empty()) continue;

      if (fn->is_c_variadic) sym->type->is_c_variadic = true;

      if (fn->is_extern) {
        sym->type->is_extern = true;
        c_boundary(sym->type, fn->pos, "extern '" + fn->name + "'");
        c_symbol(fn);
        // A blocking call is made by name, through a wrapper that knows its
        // signature, and C's `...` is settled at each call site, with nothing
        // a wrapper could pass along.
        if (fn->is_blocking && fn->is_c_variadic)
          error(fn->pos, "'%s' cannot be blocking and take C's '...'",
                fn->name.c_str());
      }

      if (fn->name == "main" && pkg.prefix.empty()) {
        Type *payload = ret->is_error_union ? ret->elem : ret;
        if (payload->kind != TY_VOID && payload->kind != TY_INT)
          error(fn->pos, "'main' must return int, void, !int or !void");
        if (!params.empty())
          error(fn->pos, "'main' takes no parameters");
      }
    }

    for (Node *fn : pkg.unit->kids)
      if (fn->kind == ND_FUNC && fn->tparams.empty()) check_func(fn);

    pop_scope();
    return error_count() == 0;
  }

  // A checker made to resolve something another package declared starts with
  // nothing in scope: a constant named in an array length would not be found.
  void see_package() {
    push_scope();
    for (auto &entry : pkg.globals) scopes.back()[entry.first] = entry.second;
  }

  // An instantiation is checked later, with its type parameters bound and the
  // declaring package's names back in scope.
  bool run_instance(const Program::Instance &job) {
    see_package();

    size_t depth = bindings.size();
    for (auto &entry : job.bind) bindings.push_back(entry);
    check_func(job.decl);
    bindings.resize(depth);

    pop_scope();
    return error_count() == 0;
  }
};

} // namespace

namespace {

// --- determinism -----------------------------------------------------------
//
// `det` says that what a function answers depends on its arguments and on
// nothing else: not on how many threads ran it, not on the order they ran in,
// not on the clock, and not on an address. It is *inferred* for every function
// and only *checked* where it is written, so a program gets the guarantee
// without annotating anything — writing `det` asks to be told when it is lost,
// and the message says where.
//
// What takes it away is short, and every one of them is a way for something
// other than the arguments to reach the answer:
//
//   an atomic, a `shared`, a channel — the two tasks' order decides
//   an extern call — the clock, the operating system, malloc's addresses
//   a pointer turned into a number — the layout decides
//   a call through a value or an interface, unless every target is det
//   a call to something that is not det
//
// `scope`, `spawn` and `parallel for` are *not* on that list, which is the
// whole point: the race checker already says two tasks cannot touch the same
// memory, and a reduction combines its pieces in the order they were cut.
struct Determinism {
  struct Why {
    Pos at;
    const char *what = nullptr; // what was reached for, when it was local
    Symbol *through = nullptr;  // or the callee that lost it
  };

  Program &prog;
  TypeTable &types;
  std::map<Symbol *, Why> lost;             // functions known not to be det
  // An address turned into a number is a reason of its own: it is not carried
  // to the callers. A pointer is opaque to whoever gets it, so a library that
  // does arithmetic on one — an arena aligning what it hands out — does not
  // make its callers depend on the layout. Doing it *here* does.
  std::map<Symbol *, Why> local_lost;
  std::map<Symbol *, std::vector<Node *>> bodies;
  std::map<std::string, Symbol *> by_name;  // every function, by linker name
  std::vector<Symbol *> order;
  std::map<Symbol *, Node *> decls;
  // Reasons that name something, kept here so the text outlives the check.
  std::deque<std::string> texts;

  Determinism(Program &p, TypeTable &t) : prog(p), types(t) {}

  static void each(Node *n, const std::function<void(Node *)> &see) {
    if (!n) return;
    see(n);
    for (Node *kid : n->kids) each(kid, see);
    each(n->lhs, see);
    each(n->rhs, see);
    each(n->cond, see);
    each(n->body, see);
    each(n->els, see);
  }

  void mark(Symbol *fn, Pos at, const char *what, Symbol *through) {
    if (!fn || lost.count(fn)) return;
    lost[fn] = Why{at, what, through};
  }

  void mark_local(Symbol *fn, Pos at, const char *what) {
    if (!fn || local_lost.count(fn)) return;
    local_lost[fn] = Why{at, what, nullptr};
  }

  // Everything the program can reach through a value or a vtable. A call that
  // goes through one of those is det when every target is, which is worth
  // knowing precisely: an allocator that is an arena is det, and the same call
  // through an allocator that is malloc is not.
  std::set<Symbol *> by_value;
  std::map<std::string, std::set<Symbol *>> by_interface;

  void take(Node *decl) {
    if (decl->kind != ND_FUNC || !decl->sym || decl->is_extern) return;
    if (!decl->tparams.empty()) return; // only instances are compiled
    if (!bodies.count(decl->sym)) order.push_back(decl->sym);
    if (!decls.count(decl->sym)) decls[decl->sym] = decl;
    by_name[decl->sym->name] = decl->sym;
    bodies[decl->sym].push_back(decl->body);
  }

  void collect() {
    for (Package *pkg : prog.order) {
      for (Node *decl : pkg->unit->kids) take(decl);
      // A generic's instances live beside the package rather than in it, and
      // a channel is one of those: without them a `chan` looked like a call to
      // something nobody had ever heard of.
      for (Node *decl : pkg->instances) take(decl);
    }

    for (const TypeTable::VTable &table : types.vtables()) {
      size_t cut = table.key.find('>');
      if (cut == std::string::npos) continue;
      std::string iface = table.key.substr(cut + 1);
      for (const std::string &entry : table.entries)
        if (Symbol *fn = by_name.count(entry) ? by_name[entry] : nullptr)
          by_interface[iface].insert(fn);
        else
          by_interface[iface].insert(nullptr); // an entry nobody checked
    }
  }

  // The interface a dynamic call goes through, by name.
  static std::string interface_of(Node *call) {
    if (!call->lhs || !call->lhs->lhs) return "";
    Type *base = call->lhs->lhs->type;
    if (base && base->kind == TY_PTR) base = base->elem;
    return base && base->is_interface ? base->name : "";
  }

  void scan(Symbol *fn, Node *body) {
    std::set<Node *> callee_names;
    each(body, [&](Node *n) {
      if (n->kind == ND_CALL && n->lhs && n->lhs->kind == ND_IDENT)
        callee_names.insert(n->lhs);
    });

    each(body, [&](Node *n) {
      // A function named without being called is a value, and a call through
      // one can go anywhere the program takes an address.
      if (n->kind == ND_IDENT && n->sym && n->sym->is_func &&
          !callee_names.count(n))
        by_value.insert(n->sym);

      switch (n->kind) {
      case ND_LOCK:
        mark(fn, n->pos, "holds a shared value", nullptr);
        return;
      case ND_SEND:
      case ND_RECV:
        mark(fn, n->pos, "uses a channel", nullptr);
        return;
      case ND_SELECT:
        mark(fn, n->pos,
             "waits on several channels at once, and which is ready first"
             " decides",
             nullptr);
        return;
      case ND_CONVERT: {
        // An address turned into a number is the layout reaching the answer.
        Type *from = n->kids.empty() ? nullptr : n->kids[0]->type;
        bool address = from && (from->kind == TY_PTR || from->kind == TY_RAWPTR ||
                                from->kind == TY_FUNC);
        if (address && n->type && n->type->kind == TY_INT)
          mark_local(fn, n->pos, "turns an address into a number");
        return;
      }
      case ND_CALL:
        if (n->form == 3) mark(fn, n->pos, "reads or writes an atomic", nullptr);
        else if (n->form == 5) mark(fn, n->pos, "waits on a shared value", nullptr);
        else if (n->sym && n->sym->decl && n->sym->decl->is_extern)
          mark(fn, n->pos, "calls out of the language", nullptr);
        else if (n->form == 4 && n->lhs && n->lhs->type &&
                 n->lhs->type->kind == TY_FUNC && n->lhs->type->is_extern)
          mark(fn, n->pos, "calls out of the language", nullptr);
        return;
      default:
        return;
      }
    });
  }

  // The first target a call of this shape can reach that is not det, or null
  // when every one of them is. `found` says whether the targets are known at
  // all: an interface nothing implements in this program is not one of them.
  Symbol *stray_target(Node *call, bool &known) {
    known = true;
    if (call->form == 2) { // through an interface
      std::string iface = interface_of(call);
      auto at = by_interface.find(iface);
      if (iface.empty() || at == by_interface.end()) {
        known = false;
        return nullptr;
      }
      for (Symbol *target : at->second) {
        if (!target) {
          known = false;
          return nullptr;
        }
        if (lost.count(target)) return target;
      }
      return nullptr;
    }
    for (Symbol *target : by_value)
      if (lost.count(target)) return target;
    return nullptr;
  }

  // --- channels, Kahn's way ---------------------------------------------------
  //
  // A channel is where two tasks' order usually reaches the answer, and in
  // general it does. But Kahn showed in 1974 that a network of processes joined
  // by channels computes a function of its inputs when each channel has one
  // writer and one reader, and a reader can only wait for the next value — never
  // ask whether one is there yet. The order values arrive in is then the order
  // they were sent, which one task decided on its own.
  //
  // So a channel keeps a function det when, within every scope, at most one of
  // the things running at once sends on it (closing is part of sending) and at
  // most one receives. A spawn is one of those things, or many if it sits in a
  // loop; the body of a parallel loop is many; the scope's own function is one.
  // What each function does with a channel it is handed is worked out from its
  // body, through its calls, so the counting happens where the channel is
  // shared out. `select`, TrySend, TryRecv, Len and the rest ask what only the
  // timing knows and still take det away, and so does letting a channel go
  // anywhere the compiler cannot follow: a copy, a field, a return.

  enum { SENDS = 1, RECEIVES = 2 };

  static bool channel_type(const Type *t) {
    if (!t) return false;
    const Type *owner = t->kind == TY_PTR ? t->elem : t;
    const std::string mark = "std.chan.Chan$";
    return owner && owner->name.compare(0, mark.size(), mark) == 0;
  }

  static bool in_chan_package(const Symbol *s) {
    return s && s->name.compare(0, 9, "std.chan.") == 0;
  }

  // The method a call makes on a channel, or "" when it is not one. `New` is
  // the constructor, which the checker turns `chan[T](a, n)` into.
  static std::string channel_member_name(Node *call) {
    if (!call || call->kind != ND_CALL || !in_chan_package(call->sym)) return "";
    size_t dot = call->sym->name.rfind('.');
    std::string last = call->sym->name.substr(dot + 1);
    if (last.compare(0, 4, "New$") == 0) return "New";
    return last;
  }

  static bool channel_member(Node *call) {
    return !channel_member_name(call).empty();
  }

  struct Party {
    Node *spawn = nullptr; // null: the scope's own function
    int roles = 0;
    bool many = false;
    Pos at;
  };

  struct Shared {
    Node *scope;
    int loop_base;
    std::map<Symbol *, std::vector<Party>> uses;
  };

  // What each function does with each channel parameter, by position.
  std::map<Symbol *, std::vector<int>> param_roles;

  struct Walk {
    Symbol *fn;
    bool checking;
    std::vector<Shared> open;
    int loops = 0;
    int parallel = 0;
    std::map<Symbol *, int> roles; // everything this function does, per binding
  };

  bool is_channel_binding(Node *n) {
    return n && n->kind == ND_IDENT && n->sym && !n->sym->is_func &&
           channel_type(n->type);
  }

  void lose(Walk &w, Pos at, std::string text) {
    if (!w.checking) return;
    texts.push_back(std::move(text));
    mark(w.fn, at, texts.back().c_str(), nullptr);
  }

  void use(Walk &w, Symbol *channel, int roles, Node *spawn, Pos at) {
    if (!roles) return;
    w.roles[channel] |= roles;
    bool many = w.parallel > 0 ||
                (spawn && !w.open.empty() && w.loops > w.open.back().loop_base);
    for (Shared &s : w.open) s.uses[channel].push_back(Party{spawn, roles, many, at});
  }

  int roles_of(Symbol *callee, size_t index) {
    auto found = param_roles.find(callee);
    if (found == param_roles.end() || index >= found->second.size()) return 0;
    return found->second[index];
  }

  // A call that hands channels on: to a function whose parameters say what it
  // does with them, and nowhere else.
  void pass_on(Walk &w, Node *call, Node *spawn) {
    Symbol *callee = call->sym;
    bool followed = call->form == 0 && callee && decls.count(callee) &&
                    call->variadic_at < 0;
    for (size_t i = 0; i < call->kids.size(); i++) {
      Node *arg = call->kids[i];
      if (!is_channel_binding(arg)) {
        visit(w, arg, nullptr);
        continue;
      }
      if (!followed) {
        lose(w, arg->pos,
             "hands channel '" + shown_name(arg->sym->name) +
                 "' to something the compiler cannot follow");
        continue;
      }
      use(w, arg->sym, roles_of(callee, i), spawn, arg->pos);
    }
  }

  void visit(Walk &w, Node *n, Node *spawn) {
    if (!n) return;
    switch (n->kind) {
    case ND_SCOPE: {
      w.open.push_back(Shared{n, w.loops, {}});
      visit(w, n->body, nullptr);
      Shared done = std::move(w.open.back());
      w.open.pop_back();
      judge(w, done);
      return;
    }
    case ND_SPAWN:
      if (n->lhs && n->lhs->kind == ND_CALL) pass_on(w, n->lhs, n);
      return;
    case ND_FOR: {
      bool spread = n->is_parallel;
      w.loops++;
      if (spread) w.parallel++;
      for (Node *kid : n->kids) visit(w, kid, nullptr);
      visit(w, n->lhs, nullptr);
      visit(w, n->rhs, nullptr);
      visit(w, n->cond, nullptr);
      visit(w, n->body, nullptr);
      if (spread) w.parallel--;
      w.loops--;
      return;
    }
    case ND_CALL: {
      std::string member = channel_member_name(n);
      if (member == "New") {
        for (Node *kid : n->kids) visit(w, kid, nullptr);
        return;
      }
      if (!member.empty()) {
        Node *on = n->lhs && n->lhs->kind == ND_FIELD ? n->lhs->lhs : nullptr;
        for (Node *kid : n->kids) visit(w, kid, nullptr);
        if (!is_channel_binding(on)) {
          lose(w, n->pos,
               "reaches a channel through something the compiler cannot follow");
          return;
        }
        std::string name = shown_name(on->sym->name);
        if (member == "Send" || member == "Close")
          use(w, on->sym, SENDS, nullptr, n->pos);
        else if (member == "Recv")
          use(w, on->sym, RECEIVES, nullptr, n->pos);
        else if (member != "Free")
          lose(w, n->pos,
               "asks channel '" + name + "' something only the timing knows (" +
                   member + ")");
        return;
      }
      pass_on(w, n, nullptr);
      visit(w, n->lhs, nullptr);
      return;
    }
    case ND_VAR:
      // A binding made from another is a second name for the same channel.
      if (channel_type(n->type) && is_channel_binding(n->rhs ? n->rhs : n->lhs))
        lose(w, n->pos, "copies a channel, and the copies cannot be told apart");
      break;
    case ND_IDENT:
      if (is_channel_binding(n))
        lose(w, n->pos,
             "lets channel '" + shown_name(n->sym->name) +
                 "' go where the compiler cannot follow it");
      return;
    default:
      break;
    }
    for (Node *kid : n->kids) visit(w, kid, nullptr);
    visit(w, n->lhs, nullptr);
    visit(w, n->rhs, nullptr);
    visit(w, n->cond, nullptr);
    visit(w, n->body, nullptr);
    visit(w, n->els, nullptr);
  }

  // One scope's worth of sharing: at most one sender and one receiver among
  // the things that run at once. A scope inside another also runs beside the
  // outer one's tasks, which is why every open scope hears about every use.
  void judge(Walk &w, Shared &s) {
    for (auto &[channel, parties] : s.uses) {
      std::map<Node *, int> senders, receivers; // by party; null is the function
      bool many_send = false, many_recv = false;
      for (const Party &p : parties) {
        if (p.roles & SENDS) {
          senders[p.spawn] = 1;
          many_send |= p.many;
        }
        if (p.roles & RECEIVES) {
          receivers[p.spawn] = 1;
          many_recv |= p.many;
        }
      }
      std::string name = shown_name(channel->name);
      if (senders.size() > 1 || many_send)
        lose(w, s.scope->pos,
             "lets more than one task at a time send on channel '" + name + "'");
      else if (receivers.size() > 1 || many_recv)
        lose(w, s.scope->pos,
             "lets more than one task at a time receive from channel '" + name +
                 "'");
    }
  }

  void kahn() {
    // What each function does with the channels it is handed, until nothing
    // new is learned; then once more, keeping what it finds.
    bool changed = true;
    while (changed) {
      changed = false;
      for (Symbol *fn : order) {
        if (in_chan_package(fn)) continue;
        Walk w{fn, false, {}, 0, 0, {}};
        for (Node *body : bodies[fn]) visit(w, body, nullptr);
        std::vector<int> roles;
        for (Node *param : decls[fn]->kids)
          roles.push_back(param->kind == ND_PARAM && param->sym
                              ? w.roles[param->sym]
                              : 0);
        if (param_roles[fn] != roles) {
          param_roles[fn] = roles;
          changed = true;
        }
      }
    }
    for (Symbol *fn : order) {
      if (in_chan_package(fn)) continue;
      Walk w{fn, true, {}, 0, 0, {}};
      for (Node *body : bodies[fn]) visit(w, body, nullptr);
    }
  }

  void settle() {
    bool changed = true;
    while (changed) {
      changed = false;
      for (Symbol *fn : order) {
        if (lost.count(fn)) continue;
        for (Node *body : bodies[fn]) {
          each(body, [&](Node *n) {
            if (n->kind != ND_CALL || lost.count(fn)) return;
            if (n->form == 2 || n->form == 4) {
              bool known = true;
              Symbol *stray = stray_target(n, known);
              if (stray || !known) {
                mark(fn, n->pos,
                     n->form == 2 ? "calls through an interface"
                                  : "calls through a function value",
                     stray);
                changed = true;
                return;
              }
            }
            // A channel's own methods are outside the language underneath;
            // whether using them keeps a function det is the Kahn check's
            // question, not this one's.
            if (n->sym && lost.count(n->sym) && !channel_member(n)) {
              mark(fn, n->pos, nullptr, n->sym);
              changed = true;
            }
          });
        }
      }
    }
  }

  // The chain from what was claimed to what lost it, since the reason is
  // rarely in the function that was marked.
  void blame(Symbol *fn) {
    if (!lost.count(fn)) {
      const Why &why = local_lost[fn];
      note(why.at, "%s %s", shown_name(fn->name).c_str(), why.what);
      return;
    }
    std::set<Symbol *> seen;
    Symbol *at = fn;
    while (at && !seen.count(at)) {
      seen.insert(at);
      const Why &why = lost[at];
      std::string here = shown_name(at->name);
      if (why.what && why.through)
        note(why.at, "%s %s, and %s is not det", here.c_str(), why.what,
             shown_name(why.through->name).c_str());
      else if (why.through)
        note(why.at, "%s calls %s, which is not det", here.c_str(),
             shown_name(why.through->name).c_str());
      else
        note(why.at, "%s %s", here.c_str(),
             why.what ? why.what : "cannot be checked");
      if (!why.through) return;
      at = why.through;
    }
  }

  void run() {
    collect();
    for (Symbol *fn : order)
      for (Node *body : bodies[fn]) scan(fn, body);
    kahn();
    settle();

    for (Package *pkg : prog.order)
      for (Node *decl : pkg->unit->kids) {
        if (decl->kind != ND_FUNC || !decl->is_det || !decl->sym) continue;
        if (!lost.count(decl->sym) && !local_lost.count(decl->sym)) continue;
        error(decl->pos,
              "'%s' is marked det, but what it answers can depend on more than"
              " its arguments",
              shown_name(decl->sym->name).c_str());
        blame(decl->sym);
      }
  }
};

// --- copies of read-only memory ----------------------------------------------

// Copying a slice or a pointer copies the way to some memory, not the memory,
// so a `mut` binding made from one that was lent read-only must not become a
// way to write it. The binding stays what `mut` says — it can be reassigned and
// its own fields written — and what it reaches through the copied part does not.
//
// Whether a call writes through what it is given is inferred for every function,
// to a fixed point, the way det is: a method that only moves `p.at` may be called
// on a parser that keeps a read-only `p.input`. A call through an interface or a
// function value could be anything, so it is taken to write everything it may.
struct Copies {
  // A part of a binding's own storage, by the fields that lead there ("" for
  // all of it), with the binding that lent the memory and where in that
  // binding it came from — a parameter's `p.a` is not the whole of `p`.
  struct Part {
    std::string path;
    Symbol *from;
    std::string origin;
  };
  using Parts = std::vector<Part>;

  // What a function may write through one of its parameters. For a pointer the
  // path is in what it points at, and `deeper` means the write goes on through
  // another pointer or slice found there; for anything else the path is in the
  // value itself, and every write recorded goes through it.
  struct Effect {
    std::string path;
    bool deeper;
    bool operator==(const Effect &o) const {
      return path == o.path && deeper == o.deeper;
    }
  };
  struct Summary {
    std::vector<std::vector<Effect>> writes;
    std::vector<bool> returns; // whether the result may reach this parameter
    bool operator==(const Summary &o) const {
      return writes == o.writes && returns == o.returns;
    }
  };

  // Where a place is: the binding it hangs off and the fields of that binding's
  // own storage, then, once a pointer or a slice is crossed, the fields past it,
  // and whether a second one was. `view` is an address of the place, or a slice
  // of an array, which reaches all of it.
  struct Reach {
    Symbol *root = nullptr;
    std::string path;
    bool through = false;
    std::string after;
    bool deeper = false;
    bool view = false;
  };

  // One argument, and what it lands in.
  struct Arg {
    Node *e;
    bool receiver_value; // a method's receiver given by value: its address goes
    size_t at;
    bool writes;
    Type *type;
  };

  Program &prog;
  std::map<Symbol *, Node *> decls;
  std::vector<Symbol *> order;
  std::map<Symbol *, Summary> summaries;

  // The function being walked. While summarising, its parameters are tracked as
  // sources of their own and `out` collects what happens to them.
  std::map<Symbol *, size_t> params;
  std::map<Symbol *, Parts> parts;
  Summary *out = nullptr;
  bool reporting = false;
  bool grew = false;

  explicit Copies(Program &p) : prog(p) {}

  static bool shares(const Type *t, int depth = 0) {
    if (!t || depth > 8) return false;
    switch (t->kind) {
    case TY_SLICE: case TY_PTR: case TY_RAWPTR:
      return true;
    case TY_ARRAY: case TY_OPT:
      return shares(t->elem, depth + 1);
    case TY_STRUCT:
      // An interface is reached only through its methods, so a copy of one
      // with no `mut` method can write nothing, whatever it points at.
      if (t->is_interface)
        return std::any_of(t->methods.begin(), t->methods.end(),
                           [](const Field &m) { return m.writes; });
      for (const Field &f : t->fields)
        if (shares(f.type, depth + 1)) return true;
      return false;
    default:
      return false;
    }
  }

  static std::string join(const std::string &at, const std::string &name) {
    return at.empty() ? name : at + "." + name;
  }

  // Whether `inner` is `outer` or a field somewhere inside it.
  static bool within(const std::string &inner, const std::string &outer) {
    if (outer.empty() || inner == outer) return true;
    return inner.size() > outer.size() &&
           inner.compare(0, outer.size(), outer) == 0 &&
           inner[outer.size()] == '.';
  }

  static void cross(Reach &r) {
    if (r.through) r.deeper = true;
    else r.through = true;
  }

  static void step(Reach &r, const std::string &name) {
    if (!r.through) r.path = join(r.path, name);
    else if (!r.deeper) r.after = join(r.after, name);
  }

  static bool reach(Node *n, Reach &r) {
    if (!n) return false;
    switch (n->kind) {
    case ND_IDENT:
      if (!n->sym || n->sym->is_func) return false;
      r.root = n->sym;
      return true;
    case ND_FIELD: {
      if (!reach(n->lhs, r)) return false;
      const Type *base = n->lhs->type;
      if (base && (base->kind == TY_PTR || base->kind == TY_RAWPTR)) cross(r);
      step(r, n->name);
      return true;
    }
    case ND_INDEX: {
      if (!reach(n->lhs, r)) return false;
      const Type *base = n->lhs->type;
      if (!base || base->kind != TY_ARRAY) cross(r);
      step(r, "[]");
      return true;
    }
    case ND_SLICE_EXPR: {
      if (!reach(n->lhs, r)) return false;
      const Type *base = n->lhs->type;
      if (base && base->kind == TY_ARRAY) r.view = true;
      return true;
    }
    case ND_UNARY:
      if (n->op != TK_STAR && n->op != TK_AMP) return false;
      if (!reach(n->lhs, r)) return false;
      if (n->op == TK_STAR) cross(r);
      else r.view = true;
      return true;
    case ND_CONVERT:
      return !n->kids.empty() && reach(n->kids[0], r);
    default:
      return false;
    }
  }

  // A binding lent read-only is a source of its own; so is a parameter while
  // its function is summarised. Either way, what was copied into it counts too.
  Parts parts_of(Symbol *s) {
    Parts own;
    if (params.count(s) || !s->is_mut) own.push_back({"", s, ""});
    auto at = parts.find(s);
    if (at != parts.end())
      own.insert(own.end(), at->second.begin(), at->second.end());
    return own;
  }

  void add_part(Symbol *s, const Part &part) {
    Parts &list = parts[s];
    for (const Part &had : list)
      if (had.path == part.path && had.from == part.from &&
          had.origin == part.origin)
        return;
    list.push_back(part);
    grew = true;
  }

  // Where in its own source the value at `r` sits, when the source is `r.root`
  // itself: past the pointer for a pointer parameter, in the value otherwise.
  static std::string origin_of(const Reach &r) {
    bool pointer = r.root->type && r.root->type->kind == TY_PTR;
    if (pointer) return r.through ? r.after : "";
    return r.path;
  }

  // The read-only parts of the value at `r`, relative to that value.
  void reached_parts(const Reach &r, const std::string &at, Parts &got) {
    for (const Part &part : parts_of(r.root)) {
      std::string origin = part.from == r.root ? origin_of(r) : part.origin;
      if (within(r.path, part.path)) {
        got.push_back({at, part.from, origin});
      } else if (!r.through && within(part.path, r.path)) {
        // Inside what is copied, a read-only part stays where it was, unless
        // the value is an address, which reaches the whole place.
        std::string rest = r.path.empty()
                               ? part.path
                               : part.path.substr(r.path.size() + 1);
        got.push_back({r.view ? at : join(at, rest), part.from, origin});
      }
    }
  }

  void value_parts(Node *n, const std::string &at, Parts &got) {
    if (!n || !n->type || !shares(n->type)) return;
    switch (n->kind) {
    case ND_STRUCT_LIT:
      for (Node *init : n->kids)
        if (init->kind == ND_FIELD_INIT)
          value_parts(init->rhs, join(at, init->name), got);
      return;
    case ND_ARRAY_LIT:
      for (Node *kid : n->kids) value_parts(kid, join(at, "[]"), got);
      return;
    case ND_TRY:
      value_parts(n->lhs, at, got);
      return;
    case ND_ORELSE: case ND_CATCH:
      value_parts(n->lhs, at, got);
      value_parts(n->rhs, at, got);
      return;
    case ND_CALL: {
      // What the callee may hand back of its arguments.
      std::vector<Arg> args;
      bool known = false;
      Symbol *callee = decode(n, args, known);
      for (const Arg &arg : args) {
        if (known && !summaries[callee].returns[arg.at]) continue;
        Parts theirs;
        if (arg.receiver_value) {
          Reach r;
          if (!reach(arg.e, r)) continue;
          r.view = true;
          reached_parts(r, "", theirs);
        } else {
          value_parts(arg.e, "", theirs);
        }
        for (const Part &part : theirs)
          got.push_back({at, part.from, part.origin});
      }
      return;
    }
    default:
      break;
    }
    Reach r;
    if (reach(n, r)) reached_parts(r, at, got);
  }

  // The arguments of a call with the parameters they land in, and the function
  // when the call names one whose summary is known.
  Symbol *decode(Node *n, std::vector<Arg> &args, bool &known) {
    known = false;
    Type *sig = nullptr;
    Symbol *sym = nullptr;
    size_t skip = 0;
    if (n->form == 0 || n->form == 1) { // a function, or a method
      sym = n->sym;
      if (!sym || !sym->type || sym->type->kind != TY_FUNC) return nullptr;
      sig = sym->type;
      known = summaries.count(sym) > 0;
      if (n->form == 1 && n->lhs && n->lhs->lhs) {
        Node *recv = n->lhs->lhs;
        bool mut = sym->decl && !sym->decl->kids.empty() &&
                   sym->decl->kids[0]->is_mut;
        bool pointer = recv->type && recv->type->kind == TY_PTR;
        args.push_back({recv, !pointer, 0, mut,
                        sig->params.empty() ? nullptr : sig->params[0]});
        skip = 1;
      }
    } else if (n->form == 2) { // through an interface
      const Field *method = called_method(n);
      if (!method) return nullptr;
      sig = method->type;
      // The interface value is a copy; what it points at is not, and a `mut`
      // method writes there.
      Node *recv = n->lhs->lhs;
      args.push_back({recv, false, 0, method->writes, recv->type});
    } else if (n->form == 4) { // through a function value
      sig = n->lhs ? n->lhs->type : nullptr;
    }
    if (!sig || sig->kind != TY_FUNC) return nullptr;
    for (size_t i = 0; i < n->kids.size(); i++) {
      size_t at = i + skip;
      if (at >= sig->params.size()) break;
      Node *param = sym && sym->decl && at < sym->decl->kids.size()
                        ? sym->decl->kids[at]
                        : nullptr;
      if (param && param->is_variadic) break;
      bool mut = param ? param->is_mut
                       : at < sig->param_mut.size() && sig->param_mut[at];
      args.push_back({n->kids[i], false, at, mut, sig->params[at]});
    }
    return sym;
  }

  void record(Symbol *param, const Reach &r) {
    if (!out || !r.through) return; // the parameter's own copy, reassigned
    size_t k = params[param];
    bool pointer = param->type && param->type->kind == TY_PTR;
    Effect e = pointer ? Effect{r.after, r.deeper} : Effect{r.path, true};
    for (const Effect &had : out->writes[k])
      if (had == e) return;
    out->writes[k].push_back(e);
  }

  // A parameter's memory written, or kept, by way of a copy of it.
  void escaped(Symbol *param, const std::string &origin) {
    if (!out || !params.count(param)) return;
    Effect e{origin, true};
    for (const Effect &had : out->writes[params[param]])
      if (had == e) return;
    out->writes[params[param]].push_back(e);
  }

  // A write at `r`: through what it crossed when `r.through`, or else into the
  // binding's own storage, which is the binding's to write.
  void written(Node *at, const Reach &r, const char *what) {
    if (params.count(r.root)) record(r.root, r);
    if (!r.through) return;
    for (const Part &part : parts_of(r.root)) {
      Symbol *from = part.from;
      if (from == r.root) continue; // its own parameter, or immutable: checked
      if (!within(r.path, part.path) && !within(part.path, r.path)) continue;
      if (out) {
        escaped(from, part.origin);
        continue;
      }
      if (!reporting) return;
      error(at->pos,
            "cannot %s this: '%s' was copied from immutable binding '%s', and "
            "what it reaches through that copy is read-only",
            what, r.root->name.c_str(), from->name.c_str());
      note(from->pos, "declare it with 'mut' to allow mutation");
      return;
    }
  }

  // Storing a value somewhere this cannot follow — through a pointer, into a
  // slice — would lose track of what it may reach, so a value with read-only
  // parts can only go into a binding's own storage.
  void store(Node *place, Node *value) {
    Parts got;
    value_parts(value, "", got);
    if (got.empty()) return;
    Reach r;
    if (reach(place, r) && !r.through && !r.view &&
        (r.root->is_mut || params.count(r.root))) {
      for (const Part &part : got)
        add_part(r.root, {join(r.path, part.path), part.from, part.origin});
      return;
    }
    if (out) {
      for (const Part &part : got) escaped(part.from, part.origin);
      return;
    }
    if (!reporting) return;
    error(value->pos,
          "this reaches memory through immutable binding '%s', and storing it "
          "here would let that memory be written",
          got[0].from->name.c_str());
    note(got[0].from->pos, "declare it with 'mut' to allow mutation");
  }

  void bind(Symbol *sym, Node *value) {
    if (!sym) return;
    Parts got;
    value_parts(value, "", got);
    for (const Part &part : got) add_part(sym, part);
  }

  // A `mut` parameter, or a mutating method, writes whatever its summary says
  // it does, which lands in the caller here.
  void call(Node *n) {
    std::vector<Arg> args;
    bool known = false;
    Symbol *callee = decode(n, args, known);
    for (const Arg &arg : args) {
      if (!arg.writes) continue;
      std::vector<Effect> effects =
          known ? summaries[callee].writes[arg.at]
                : std::vector<Effect>{Effect{"", true}};
      bool by_address = arg.receiver_value ||
                        (arg.e->kind == ND_UNARY && arg.e->op == TK_AMP);
      Node *place = arg.e->kind == ND_UNARY && arg.e->op == TK_AMP
                        ? arg.e->lhs
                        : arg.e;
      bool receiver = arg.receiver_value || (n->form == 2 && arg.e == n->lhs->lhs);
      const char *what = receiver ? "call a mutating method on" : "pass";
      Reach r;
      if (!reach(place, r)) continue;
      bool pointer = arg.type && arg.type->kind == TY_PTR;
      for (const Effect &e : effects) {
        Reach w = r;
        if (by_address) {
          // The callee writes the place itself, at the effect's path.
          if (!w.through) {
            if (!e.deeper) continue; // the caller's own storage
            w.path = join(w.path, e.path);
            w.through = true;
          } else {
            if (!w.deeper) w.after = join(w.after, e.path);
            w.deeper = w.deeper || e.deeper;
          }
        } else if (pointer) {
          // The pointer is a copy; what it points at is not.
          if (!w.through) {
            w.through = true;
            w.after = e.path;
            w.deeper = e.deeper;
          } else {
            w.deeper = true;
          }
        } else {
          if (!w.through) {
            w.path = join(w.path, e.path);
            w.through = true;
          } else {
            w.deeper = true;
          }
        }
        written(arg.e, w, what);
      }
    }
  }

  void visit(Node *n) {
    if (!n) return;
    switch (n->kind) {
    case ND_VAR:
      visit(n->rhs);
      bind(n->sym, n->rhs);
      return;
    case ND_ASSIGN: {
      visit(n->lhs);
      visit(n->rhs);
      Reach r;
      if (reach(n->lhs, r)) written(n->lhs, r, "assign to");
      if (n->op == TK_ASSIGN && n->lhs->type && shares(n->lhs->type))
        store(n->lhs, n->rhs);
      return;
    }
    case ND_IF:
      if (!n->name.empty()) bind(n->sym, n->cond);
      break;
    case ND_FOR:
      if (n->sym) bind(n->sym, n->form == 2 ? n->cond : n->lhs);
      break;
    case ND_CALL:
      call(n);
      break;
    case ND_RETURN:
      if (out && n->lhs) {
        Parts got;
        value_parts(n->lhs, "", got);
        for (const Part &part : got)
          if (params.count(part.from)) out->returns[params[part.from]] = true;
      }
      break;
    default:
      break;
    }
    for (Node *kid : n->kids) visit(kid);
    visit(n->lhs);
    visit(n->rhs);
    visit(n->cond);
    visit(n->body);
    visit(n->els);
  }

  void walk(Symbol *sym, bool summarising) {
    Node *decl = decls[sym];
    params.clear();
    parts.clear();
    if (summarising)
      for (size_t k = 0; k < decl->kids.size(); k++)
        if (decl->kids[k]->sym) params[decl->kids[k]->sym] = k;

    // What each binding holds settles first; a loop may copy into one after
    // the write that goes through it.
    out = nullptr;
    reporting = false;
    for (int round = 0; round < 64; round++) {
      grew = false;
      visit(decl->body);
      if (!grew) break;
    }

    if (summarising) {
      Summary now;
      now.writes.assign(decl->kids.size(), {});
      now.returns.assign(decl->kids.size(), false);
      out = &now;
      visit(decl->body);
      out = nullptr;
      summaries[sym] = now;
    } else {
      reporting = true;
      visit(decl->body);
      reporting = false;
    }
  }

  void take(Node *decl) {
    if (decl->kind != ND_FUNC || !decl->sym || decl->is_extern || !decl->body)
      return;
    if (!decl->tparams.empty()) return; // only instances are compiled
    if (decls.count(decl->sym)) return;
    decls[decl->sym] = decl;
    order.push_back(decl->sym);
  }

  void run() {
    for (Package *pkg : prog.order) {
      for (Node *decl : pkg->unit->kids) take(decl);
      for (Node *decl : pkg->instances) take(decl);
    }
    for (Symbol *sym : order) {
      size_t n = decls[sym]->kids.size();
      summaries[sym].writes.assign(n, {});
      summaries[sym].returns.assign(n, false);
    }
    // Summaries only grow, and only from what the program says, so this ends.
    for (bool changed = true; changed;) {
      changed = false;
      for (Symbol *sym : order) {
        Summary before = summaries[sym];
        walk(sym, true);
        if (!(summaries[sym] == before)) changed = true;
      }
    }
    for (Symbol *sym : order) walk(sym, false);
  }
};

} // namespace

bool check(Program &prog, TypeTable &types) {
  for (Package *pkg : prog.order)
    if (!Checker(prog, *pkg, types).run()) return false;

  Package *main = prog.main();
  if (main && !main->globals.count("main"))
    error(Pos{-1, 0, 0},
          "no 'main' function: a program needs 'func main() int' somewhere in "
          "the package being compiled");

  // Draining the worklist can add to it: an instance body may instantiate
  // something else.
  while (!prog.pending.empty()) {
    Program::Instance job = prog.pending.back();
    prog.pending.pop_back();
    if (!Checker(prog, *job.owner, types).run_instance(job)) return false;
  }
  if (error_count() > 0) return false;

  // Last, over the whole program: whether a function answers the same thing
  // every time is not a question about one package.
  Determinism(prog, types).run();
  Copies(prog).run();
  return error_count() == 0;
}
