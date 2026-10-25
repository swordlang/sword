#include "complete.h"

#include <cctype>
#include <map>

namespace {

// LSP CompletionItemKind, the few of them that mean anything here.
enum Kind {
  K_METHOD = 2,
  K_FUNCTION = 3,
  K_FIELD = 5,
  K_VARIABLE = 6,
  K_MODULE = 9,
  K_KEYWORD = 14,
  K_ENUM_MEMBER = 20,
  K_CONSTANT = 21,
  K_STRUCT = 22,
  K_TYPE = 25,
};

bool part_of_name(char c) {
  return isalnum((unsigned char)c) || c == '_';
}

// Walks back over `a.b.c` and returns its pieces. Only names and dots: anything
// else ends the chain, which is the right answer for `f(x).` — there is nothing
// here that can resolve a call's result.
std::vector<std::string> qualifier_before(const std::string &text, size_t at) {
  std::vector<std::string> out;
  size_t i = at;
  while (true) {
    size_t end = i;
    while (i > 0 && part_of_name(text[i - 1])) i--;
    if (i == end) return out; // two dots in a row, or a dot after punctuation
    out.insert(out.begin(), text.substr(i, end - i));
    if (i == 0 || text[i - 1] != '.') return out;
    i--; // step over the dot and keep going
  }
}

void add(std::vector<Completion> &out, const std::string &label, int kind,
         const std::string &detail) {
  out.push_back(Completion{label, detail, kind});
}

// A declaration's name as the programmer wrote it: the package prefix is for the
// linker, and a method carries its type in front of it.
std::string plain_name(const std::string &name) {
  size_t cut = name.rfind('.');
  return cut == std::string::npos ? name : name.substr(cut + 1);
}

int kind_of_symbol(const Symbol *sym) {
  if (sym->is_func) return K_FUNCTION;
  if (sym->const_value) return K_CONSTANT;
  return K_VARIABLE;
}

int kind_of_type(const Type *t) {
  if (!t) return K_TYPE;
  if (t->kind == TY_ENUM) return K_TYPE;
  if (t->is_interface) return K_TYPE;
  return K_STRUCT;
}

std::string signature(const Symbol *sym) {
  return sym->type ? type_str(sym->type) : std::string();
}

// The package a name refers to, if it is one this file imported.
Package *package_named(const std::string &name, Package &owner, Program &prog) {
  auto named = owner.import_names.find(name);
  if (named == owner.import_names.end()) return nullptr;
  auto found = prog.by_path.find(named->second);
  return found != prog.by_path.end() ? found->second : nullptr;
}

// Which package the open file belongs to. Everything the editor offers without a
// qualifier comes from here.
Package *package_of(int file, Program &prog) {
  for (Package &pkg : prog.packages) {
    if (!pkg.unit) continue;
    for (Node *decl : pkg.unit->kids)
      if (decl->pos.file == file) return &pkg;
  }
  return prog.order.empty() ? nullptr : prog.order.back();
}

// Every binding declared before the cursor inside the function the cursor is in.
// Block scopes are not tracked — a name declared earlier in the same function is
// a good guess at what the cursor means by it — but function boundaries are,
// because a local from three functions up is never the answer.
struct Bindings {
  int file;
  int line;
  int from = 0; // the line the enclosing function starts on
  std::map<std::string, std::pair<int, Type *>> found;

  // The enclosing function is the last one that starts at or before the cursor.
  void locate(Node *unit) {
    for (Node *decl : unit->kids) {
      if (decl->kind != ND_FUNC || decl->pos.file != file) continue;
      if (decl->pos.line <= line && decl->pos.line > from) from = decl->pos.line;
    }
  }

  void note(const std::string &name, Pos at, Type *type) {
    if (name.empty() || !type || at.file != file) return;
    if (at.line > line || at.line < from) return;
    auto seen = found.find(name);
    if (seen == found.end() || seen->second.first <= at.line)
      found[name] = {at.line, type};
  }

  void visit(Node *n) {
    if (!n) return;
    switch (n->kind) {
    case ND_VAR: case ND_PARAM: case ND_FOR: case ND_CATCH: case ND_LOCK:
      note(n->name, n->name_pos, n->sym ? n->sym->type : n->type);
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
};

const Type *behind(const Type *t) {
  while (t && (t->kind == TY_PTR || t->kind == TY_RAWPTR)) t = t->elem;
  return t;
}

// Whether a type was declared in the package the cursor is in. A struct's name
// carries the package it came from, and a generic instance carries its type
// arguments after a '$' — those have packages of their own.
bool declared_in(const Type *t, const std::string &prefix) {
  std::string name = t->name.substr(0, t->name.find('$'));
  if (name.size() <= prefix.size()) return true;
  if (name.find('.') == std::string::npos) return true;
  if (name.compare(0, prefix.size(), prefix) != 0) return false;
  return name.find('.', prefix.size()) == std::string::npos;
}

// What `x.` offers: the fields of the struct, and the methods hanging off it.
// Another package's unexported fields are not offered, because they would not
// compile — the same rule the checker applies.
void members_of(const Type *type, const std::string &prefix, Program &prog,
                std::vector<Completion> &out) {
  const Type *owner = behind(type);
  if (!owner) return;

  if (owner->kind == TY_SLICE || owner->kind == TY_ARRAY ||
      owner->kind == TY_STRING) {
    add(out, "len", K_FIELD, "uint");
    if (owner->kind != TY_ARRAY)
      add(out, "ptr", K_FIELD, type_str(owner->elem) + "*");
    return;
  }
  if (owner->kind == TY_ENUM) {
    for (const Field &member : owner->fields)
      add(out, member.name, K_ENUM_MEMBER, type_str(owner));
    return;
  }
  if (owner->kind != TY_STRUCT) return;

  // An optional or an error union is handled with `orelse`, `try` or `catch`,
  // never by reaching inside, so offering its two fields would be a lie.
  if (owner->is_optional || owner->is_error_union) return;

  bool ours = declared_in(owner, prefix);
  for (const Field &f : owner->fields) {
    if (!ours && !exported(f.name)) continue;
    add(out, f.name, K_FIELD, type_str(f.type));
  }
  for (const Field &m : owner->methods) // an interface's method set
    add(out, m.name, K_METHOD, (m.writes ? "mut " : "") + type_str(m.type));

  auto methods = prog.methods.find(owner->name);
  if (methods == prog.methods.end()) return;
  for (const auto &entry : methods->second) {
    if (!ours && !exported(entry.first)) continue;
    add(out, entry.first, K_METHOD, signature(entry.second));
  }
}

// Everything a package makes visible to whoever imported it.
void exports_of(Package &pkg, std::vector<Completion> &out) {
  for (const auto &entry : pkg.globals) {
    if (!exported(entry.first)) continue;
    add(out, entry.first, kind_of_symbol(entry.second),
        signature(entry.second));
  }
  for (const auto &entry : pkg.type_names) {
    if (!exported(entry.first)) continue;
    add(out, entry.first, kind_of_type(entry.second), "type");
  }
  for (const auto &entry : pkg.generic_types) {
    if (!exported(entry.first)) continue;
    add(out, entry.first, K_STRUCT, "generic type");
  }
}

const char *const kKeywords[] = {
    "break", "case", "catch", "const", "continue", "default", "defer", "else",
    "enum", "errdefer", "extern", "false", "for", "func", "if", "import", "in",
    "interface", "lock", "mut", "nil", "orelse", "package", "parallel",
    "reduce", "return", "scope", "spawn", "struct", "switch", "true", "try",
};

const char *const kTypes[] = {
    "bool", "error", "f32", "f64", "i8", "i16", "i32", "i64", "int", "string",
    "u8", "u16", "u32", "u64", "uint", "void", "any", "atomic", "shared",
    "chan",
};

} // namespace

std::vector<Completion> completions(const std::string &before, int file,
                                   int line_number, Program &prog,
                                   TypeTable &types) {
  std::vector<Completion> out;

  // What is already typed is the editor's business to filter on, so it is cut
  // off here and only the shape of what comes before it matters.
  size_t end = before.size();
  while (end > 0 && part_of_name(before[end - 1])) end--;
  bool after_dot = end > 0 && before[end - 1] == '.';

  Package *pkg = package_of(file, prog);
  if (!pkg) return out;

  if (after_dot) {
    std::vector<std::string> chain = qualifier_before(before, end - 1);
    if (chain.empty()) return out;

    // `error.` is its own namespace: the errors this program declares, with the
    // sentence each of them says.
    if (chain.size() == 1 && chain[0] == "error") {
      for (const TypeTable::ErrorDecl &e : types.errors()) {
        if (!e.declared) continue;
        if (e.owner != pkg->prefix && !exported(e.name)) continue;
        add(out, e.name, K_ENUM_MEMBER, e.message);
      }
      return out;
    }

    if (chain.size() == 1) {
      if (Package *other = package_named(chain[0], *pkg, prog)) {
        exports_of(*other, out);
        return out;
      }
    }

    // A value, then: resolve the first name and walk the fields after it.
    Bindings bindings{file, line_number, 0, {}};
    if (pkg->unit) bindings.locate(pkg->unit);
    for (Package &p : prog.packages)
      if (p.unit) bindings.visit(p.unit);

    const Type *type = nullptr;
    auto local = bindings.found.find(chain[0]);
    if (local != bindings.found.end()) {
      type = local->second.second;
    } else {
      auto global = pkg->globals.find(chain[0]);
      if (global != pkg->globals.end()) type = global->second->type;
    }
    // Not a value at all: `Kind.` names a type, and what follows is a member.
    if (!type) {
      auto named = pkg->type_names.find(chain[0]);
      if (named != pkg->type_names.end()) type = named->second;
    }

    for (size_t i = 1; i < chain.size() && type; i++) {
      const Type *owner = behind(type);
      type = nullptr;
      if (!owner || owner->kind != TY_STRUCT) break;
      for (const Field &f : owner->fields)
        if (f.name == chain[i]) type = f.type;
    }
    if (type) members_of(type, pkg->prefix, prog, out);
    return out;
  }

  // No qualifier: the words of the language, the types, what this package has,
  // and the packages it imported.
  for (const char *word : kKeywords) add(out, word, K_KEYWORD, "");
  for (const char *name : kTypes) add(out, name, K_TYPE, "builtin type");

  for (const auto &entry : pkg->globals)
    add(out, plain_name(entry.first), kind_of_symbol(entry.second),
        signature(entry.second));
  for (const auto &entry : pkg->type_names)
    add(out, entry.first, kind_of_type(entry.second), "type");
  for (const auto &entry : pkg->generic_types)
    add(out, entry.first, K_STRUCT, "generic type");

  for (const std::string &path : pkg->imports) {
    auto found = prog.by_path.find(path);
    if (found != prog.by_path.end())
      add(out, found->second->name, K_MODULE, path);
  }

  Bindings bindings{file, line_number, 0, {}};
  if (pkg->unit) bindings.locate(pkg->unit);
  for (Package &p : prog.packages)
    if (p.unit) bindings.visit(p.unit);
  for (const auto &entry : bindings.found)
    add(out, entry.first, K_VARIABLE, type_str(entry.second.second));

  return out;
}
