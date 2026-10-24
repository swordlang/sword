#include "types.h"

#include <algorithm>

namespace {
const int64_t kWord = 8;

// Every error the standard library declares, in byte order, written by the
// Makefile from std/. They take the first codes whether the program imports
// them or not, so `error.Timeout` is the same number in every program — which
// is also what lets a package's compiled code be reused by another one.
const char *const kStdErrors[] = {
#include "std_errors.inc"
};
} // namespace

TypeTable::TypeTable() {
  for (const char *name : kStdErrors)
    error_list.push_back(ErrorDecl{name, "", "", false});

  Type t;

  t = Type{}; t.kind = TY_VOID;
  void_ty = add(t);
  by_name["void"] = void_ty;

  t = Type{}; t.kind = TY_BOOL; t.bits = 1;
  bool_ty = add(t);
  by_name["bool"] = bool_ty;

  struct { const char *name; int bits; bool is_signed; } ints[] = {
      {"i8", 8, true},    {"i16", 16, true},  {"i32", 32, true},
      {"i64", 64, true},  {"int", 64, true},  {"u8", 8, false},
      {"u16", 16, false}, {"u32", 32, false}, {"u64", 64, false},
      {"uint", 64, false},
  };
  for (auto &spec : ints) {
    t = Type{};
    t.kind = TY_INT;
    t.bits = spec.bits;
    t.is_signed = spec.is_signed;
    by_name[spec.name] = add(t);
  }
  int_ty = by_name["int"];
  u8_ty = by_name["u8"];
  usize_ty = by_name["uint"];

  for (int bits : {32, 64}) {
    t = Type{};
    t.kind = TY_FLOAT;
    t.bits = bits;
    by_name[bits == 32 ? "f32" : "f64"] = add(t);
  }

  t = Type{};
  t.kind = TY_STRING;
  t.elem = u8_ty;
  string_ty = add(t);
  by_name["string"] = string_ty;

  t = Type{};
  t.kind = TY_INT;
  t.bits = 16;
  t.name = "error";
  error_ty = add(t);
  by_name["error"] = error_ty;

  t = Type{};
  t.kind = TY_STRUCT;
  t.is_any = true;
  t.name = "any";
  any_ty = add(t);
  by_name["any"] = any_ty;
  {
    std::vector<Field> fields;
    // Named as an API rather than as internals: reading an `any` is just
    // reading these four fields.
    fields.push_back(Field{"Kind", by_name["u8"], 0, 0});
    fields.push_back(Field{"Int", by_name["i64"], 0, 0});
    fields.push_back(Field{"Real", by_name["f64"], 0, 0});
    fields.push_back(Field{"Text", string_ty, 0, 0});
    layout_struct(any_ty, std::move(fields));
  }

  t = Type{};
  t.kind = TY_OPT;
  t.untyped = true;
  t.elem = void_ty;
  untyped_nil = add(t);

  t = Type{};
  t.kind = TY_INT;
  t.bits = 64;
  t.is_signed = true;
  t.untyped = true;
  untyped_int = add(t);

  t = Type{};
  t.kind = TY_FLOAT;
  t.bits = 64;
  t.untyped = true;
  untyped_float = add(t);
}

Type *TypeTable::error_union(Type *value) {
  auto found = error_unions.find(value);
  if (found != error_unions.end()) return found->second;

  Type t;
  t.kind = TY_STRUCT;
  t.is_error_union = true;
  t.elem = value;
  t.name = "err." + std::to_string(error_unions.size());
  Type *type = add(t);

  std::vector<Field> fields;
  fields.push_back(Field{"code", error_ty, 0, 0});
  if (value->kind != TY_VOID) fields.push_back(Field{"value", value, 0, 0});
  layout_struct(type, std::move(fields));

  error_union_list.push_back(type);
  return error_unions[value] = type;
}

int TypeTable::declare_error(const std::string &name,
                            const std::string &message,
                            const std::string &owner, const ErrorDecl **clash) {
  *clash = nullptr;
  for (size_t i = 0; i < error_list.size(); i++) {
    if (error_list[i].name != name) continue;
    if (!error_list[i].declared) {
      error_list[i] = ErrorDecl{name, message, owner, true};
      return (int)i + 1;
    }
    // Two packages may declare the same error, which is how `error.Timeout`
    // means one thing across a program — but only if they agree on what it says.
    if (!message.empty() && error_list[i].message.empty())
      error_list[i].message = message;
    else if (!message.empty() && error_list[i].message != message)
      *clash = &error_list[i];
    return (int)i + 1;
  }
  error_list.push_back(ErrorDecl{name, message, owner});
  return (int)error_list.size();
}

int TypeTable::error_code(const std::string &name) const {
  for (size_t i = 0; i < error_list.size(); i++)
    if (error_list[i].name == name) return error_list[i].declared ? (int)i + 1 : 0;
  return 0;
}

const TypeTable::ErrorDecl *TypeTable::error_at(int code) const {
  if (code <= 0 || (size_t)code > error_list.size()) return nullptr;
  return &error_list[(size_t)code - 1];
}

Type *TypeTable::add(Type t) {
  pool.push_back(std::move(t));
  return &pool.back();
}

Type *TypeTable::named(const std::string &name) {
  auto found = by_name.find(name);
  return found == by_name.end() ? nullptr : found->second;
}

Type *TypeTable::ptr(Type *elem) {
  auto found = ptrs.find(elem);
  if (found != ptrs.end()) return found->second;
  Type t;
  t.kind = TY_PTR;
  t.elem = elem;
  return ptrs[elem] = add(t);
}

Type *TypeTable::rawptr(Type *elem) {
  auto found = rawptrs.find(elem);
  if (found != rawptrs.end()) return found->second;
  Type t;
  t.kind = TY_RAWPTR;
  t.elem = elem;
  return rawptrs[elem] = add(t);
}

Type *TypeTable::slice(Type *elem) {
  auto found = slices.find(elem);
  if (found != slices.end()) return found->second;
  Type t;
  t.kind = TY_SLICE;
  t.elem = elem;
  return slices[elem] = add(t);
}

Type *TypeTable::array(Type *elem, int64_t count) {
  auto key = std::make_pair(elem, count);
  auto found = arrays.find(key);
  if (found != arrays.end()) return found->second;
  Type t;
  t.kind = TY_ARRAY;
  t.elem = elem;
  t.count = count;
  return arrays[key] = add(t);
}

Type *TypeTable::opt(Type *elem) {
  auto found = opts.find(elem);
  if (found != opts.end()) return found->second;

  // A pointer already has a spare bit pattern, so `?*T` is just the pointer
  // with null standing for absence. Anything else needs a flag beside it.
  if (elem->kind == TY_PTR || elem->kind == TY_RAWPTR ||
      elem->kind == TY_FUNC) {
    Type t;
    t.kind = TY_OPT;
    t.elem = elem;
    return opts[elem] = add(t);
  }

  Type t;
  t.kind = TY_STRUCT;
  t.is_optional = true;
  t.elem = elem;
  t.name = "opt." + std::to_string(optional_list.size());
  Type *type = add(t);

  std::vector<Field> fields;
  fields.push_back(Field{"has", bool_ty, 0, 0});
  fields.push_back(Field{"value", elem, 0, 0});
  layout_struct(type, std::move(fields));

  optional_list.push_back(type);
  return opts[elem] = type;
}

Type *TypeTable::atomic(Type *elem) {
  auto found = atomics.find(elem);
  if (found != atomics.end()) return found->second;
  Type t;
  t.kind = TY_ATOMIC;
  t.elem = elem;
  return atomics[elem] = add(t);
}

// A mutex beside the value it protects. All zeroes is an unlocked guard, so a
// fresh shared needs no constructor — which is what lets one sit in an array or
// a struct field.
Type *TypeTable::shared(Type *elem) {
  auto found = shareds.find(elem);
  if (found != shareds.end()) return found->second;

  Type t;
  t.kind = TY_STRUCT;
  t.is_shared = true;
  t.elem = elem;
  t.name = "shared." + std::to_string(shared_list.size());
  Type *type = add(t);

  std::vector<Field> fields;
  fields.push_back(
      Field{"guard", array(usize_ty, SWORD_GUARD_SIZE / 8), 0, 0});
  fields.push_back(Field{"value", elem, 0, 0});
  layout_struct(type, std::move(fields));

  shared_list.push_back(type);
  return shareds[elem] = type;
}

Type *TypeTable::func(std::vector<Type *> params, Type *ret,
                      std::vector<bool> param_mut) {
  Type t;
  t.kind = TY_FUNC;
  t.params = std::move(params);
  t.param_mut = std::move(param_mut);
  t.param_mut.resize(t.params.size(), false);
  t.ret = ret;
  return add(t);
}

Type *TypeTable::declare_interface(const std::string &name) {
  Type t;
  t.kind = TY_STRUCT;
  t.is_interface = true;
  t.name = name;
  Type *type = add(t);
  by_name[name] = type;

  Type *opaque = rawptr(u8_ty);
  std::vector<Field> fields;
  fields.push_back(Field{"data", opaque, 0, 0});
  fields.push_back(Field{"vtable", opaque, 0, 0});
  layout_struct(type, std::move(fields));
  return type;
}

int TypeTable::vtable(const std::string &key, std::vector<std::string> entries) {
  for (size_t i = 0; i < vtable_list.size(); i++)
    if (vtable_list[i].key == key) return (int)i;
  vtable_list.push_back(VTable{key, std::move(entries)});
  return (int)vtable_list.size() - 1;
}

// Members live in `fields`, each carrying its value in `offset`. The width is
// `elem`, which is what the machine sees; the type itself is distinct, so an
// enum never takes part in arithmetic by accident.
Type *TypeTable::declare_enum(const std::string &name, Type *width) {
  Type t;
  t.kind = TY_ENUM;
  t.name = name;
  t.elem = width;
  // Carrying the width's shape means the generic cast and compare paths in the
  // backend need to know nothing about enums.
  t.bits = width->bits;
  t.is_signed = width->is_signed;
  return by_name[name] = add(t);
}

Type *TypeTable::declare_struct(const std::string &name) {
  Type t;
  t.kind = TY_STRUCT;
  t.name = name;
  return by_name[name] = add(t);
}

void TypeTable::layout_struct(Type *type, std::vector<Field> fields) {
  if (!type->is_extern) {
    // Stable sort by descending alignment: every field then sits at a natural
    // offset and padding only ever appears once, at the end.
    std::stable_sort(fields.begin(), fields.end(),
                     [](const Field &a, const Field &b) {
                       return align_of(a.type) > align_of(b.type);
                     });
  }

  int64_t offset = 0;
  for (size_t i = 0; i < fields.size(); i++) {
    int64_t align = align_of(fields[i].type);
    offset = (offset + align - 1) / align * align;
    fields[i].offset = offset;
    fields[i].index = (int)i;
    offset += size_of(fields[i].type);
  }
  type->fields = std::move(fields);
}

void TypeTable::settle_layouts() {
  // The bound is the depth a struct can be nested to, not a guess: each pass
  // settles one more level, and a program deeper than this has a cycle, which is
  // reported elsewhere.
  for (int pass = 0; pass < 64; pass++) {
    bool moved = false;
    for (Type &t : pool) {
      if (t.kind != TY_STRUCT || t.fields.empty()) continue;
      int64_t before = size_of(&t);
      layout_struct(&t, t.fields);
      if (size_of(&t) != before) moved = true;
    }
    if (!moved) return;
  }
}

// Names are qualified by the whole import path so that nothing in the object
// file can collide, but a diagnostic should read the way the program does: the
// user wrote `Kind` and `time.Duration`, not `main.Kind` and
// `std.time.Duration`. A generic instance carries its type arguments after a
// '$', and each of those is a name in its own right.
std::string shown_name(const std::string &name) {
  std::string out;
  size_t at = 0;
  while (at <= name.size()) {
    size_t end = name.find('$', at);
    if (end == std::string::npos) end = name.size();
    std::string piece = name.substr(at, end - at);
    size_t last = piece.rfind('.');
    if (last != std::string::npos) {
      size_t before = piece.rfind('.', last - 1);
      // The program's own package has no name to say; everything else keeps the
      // one segment the import made visible.
      piece = piece.compare(0, 5, "main.") == 0 || before == std::string::npos
                  ? piece.substr(last + 1)
                  : piece.substr(before + 1);
    }
    out += piece;
    if (end == name.size()) break;
    out += '$';
    at = end + 1;
  }
  return out;
}

std::string type_str(const Type *t) {
  if (!t) return "<none>";
  switch (t->kind) {
  case TY_VOID: return "void";
  case TY_BOOL: return "bool";
  case TY_INT: {
    if (t->untyped) return "untyped int";
    if (!t->name.empty()) return t->name;
    return (t->is_signed ? "i" : "u") + std::to_string(t->bits);
  }
  case TY_FLOAT:
    return t->untyped ? "untyped float" : "f" + std::to_string(t->bits);
  case TY_PTR: return "*" + type_str(t->elem);
  case TY_RAWPTR: return "[*]" + type_str(t->elem);
  case TY_SLICE: return "[]" + type_str(t->elem);
  case TY_STRING: return "string";
  case TY_ARRAY:
    return "[" + std::to_string(t->count) + "]" + type_str(t->elem);
  case TY_OPT: return t->untyped ? "nil" : "?" + type_str(t->elem);
  case TY_ATOMIC: return "atomic[" + type_str(t->elem) + "]";
  case TY_ENUM: return shown_name(t->name);
  case TY_STRUCT:
    if (t->is_error_union) return "!" + type_str(t->elem);
    if (t->is_optional) return "?" + type_str(t->elem);
    if (t->is_shared) return "shared[" + type_str(t->elem) + "]";
    return shown_name(t->name);
  case TY_FUNC: {
    std::string s = t->is_extern ? "extern func(" : "func(";
    for (size_t i = 0; i < t->params.size(); i++) {
      if (i) s += ", ";
      if (i < t->param_mut.size() && t->param_mut[i]) s += "mut ";
      s += type_str(t->params[i]);
    }
    if (t->is_c_variadic) s += t->params.empty() ? "..." : ", ...";
    s += ")";
    if (t->ret && t->ret->kind != TY_VOID) s += " " + type_str(t->ret);
    return s;
  }
  }
  return "?";
}

// Two function types take and give the same things, whoever is behind them.
static bool same_signature(const Type *a, const Type *b) {
  if (a->params.size() != b->params.size()) return false;
  if (a->is_c_variadic != b->is_c_variadic) return false;
  if (!type_eq(a->ret, b->ret)) return false;
  for (size_t i = 0; i < a->params.size(); i++) {
    if (!type_eq(a->params[i], b->params[i])) return false;
    if (a->param_mut[i] != b->param_mut[i]) return false;
  }
  return true;
}

bool type_eq(const Type *a, const Type *b) {
  if (a == b) return true;
  if (!a || !b || a->kind != b->kind) return false;
  switch (a->kind) {
  case TY_INT:
    // The name keeps `error` distinct from the u16 it is represented by.
    return a->bits == b->bits && a->is_signed == b->is_signed &&
           a->name == b->name;
  case TY_FLOAT: return a->bits == b->bits;
  case TY_PTR: case TY_RAWPTR: case TY_SLICE: case TY_OPT: case TY_ATOMIC:
    return type_eq(a->elem, b->elem);
  case TY_ARRAY:
    return a->count == b->count && type_eq(a->elem, b->elem);
  case TY_STRUCT: case TY_ENUM: return a->name == b->name;
  case TY_FUNC:
    return a->is_extern == b->is_extern && same_signature(a, b);
  default: return true;
  }
}

bool assignable(const Type *from, const Type *to) {
  if (type_eq(from, to)) return true;
  if (!from || !to) return false;
  // `nil` fits any optional, and a present value fits the optional over it.
  if (is_optional(to)) {
    if (from->kind == TY_OPT && from->untyped) return true;
    return assignable(from, opt_payload(to));
  }
  // An untyped integer fits a float, but not the other way round: that would
  // silently drop a fraction.
  if (from->untyped && from->kind == TY_INT)
    return to->kind == TY_INT || to->kind == TY_FLOAT;
  if (from->untyped && from->kind == TY_FLOAT) return to->kind == TY_FLOAT;
  // A string is a []u8 that promises not to change, so it converts one way.
  if (from->kind == TY_STRING && to->kind == TY_SLICE && to->elem->bits == 8)
    return true;
  // A Sword function whose signature C could have written is called the way C
  // calls, so it can be handed to C. Never the other way: a C function passed
  // off as a Sword one is a call the checker would think it can follow.
  if (from->kind == TY_FUNC && to->kind == TY_FUNC && to->is_extern &&
      !from->is_extern)
    return same_signature(from, to);
  return false;
}

bool is_numeric(const Type *t) {
  return t && (t->kind == TY_INT || t->kind == TY_FLOAT);
}

bool is_integer(const Type *t) { return t && t->kind == TY_INT; }

bool is_float(const Type *t) { return t && t->kind == TY_FLOAT; }

bool is_atomic(const Type *t) { return t && t->kind == TY_ATOMIC; }

bool is_shared(const Type *t) { return t && t->is_shared; }

int any_kind_of(const Type *t) {
  if (!t) return -1;
  switch (t->kind) {
  case TY_BOOL: return ANY_BOOL;
  case TY_INT: return t->is_signed ? ANY_INT : ANY_UINT;
  // An enum is boxable; which slot it lands in is decided at the call site,
  // where the name of its member can be looked up.
  case TY_ENUM: return t->is_signed ? ANY_INT : ANY_UINT;
  case TY_FLOAT: return ANY_FLOAT;
  case TY_STRING: return ANY_STRING;
  case TY_PTR: case TY_RAWPTR: return ANY_POINTER;
  default: return -1;
  }
}

bool is_optional(const Type *t) {
  return t && !t->untyped && (t->kind == TY_OPT || t->is_optional);
}

const Type *opt_payload(const Type *t) { return t ? t->elem : nullptr; }

bool is_aggregate(const Type *t) {
  if (!t) return false;
  switch (t->kind) {
  case TY_SLICE: case TY_STRING: case TY_ARRAY: case TY_STRUCT:
    return true;
  default:
    return false;
  }
}

int64_t size_of(const Type *t) {
  if (!t) return 0;
  switch (t->kind) {
  case TY_VOID: return 0;
  case TY_BOOL: return 1;
  case TY_INT: case TY_FLOAT: return t->bits / 8;
  case TY_PTR: case TY_RAWPTR: case TY_FUNC: return kWord;
  case TY_OPT: return size_of(t->elem); // ?*T reuses the null pointer as tag
  case TY_ATOMIC: case TY_ENUM: return size_of(t->elem);
  case TY_SLICE: case TY_STRING: return 2 * kWord;
  case TY_ARRAY: return t->count * size_of(t->elem);
  case TY_STRUCT: {
    if (t->fields.empty()) return 0;
    const Field &last = t->fields.back();
    int64_t align = align_of(t);
    int64_t size = last.offset + size_of(last.type);
    return (size + align - 1) / align * align;
  }
  }
  return 0;
}

int64_t align_of(const Type *t) {
  if (!t) return 1;
  switch (t->kind) {
  case TY_VOID: return 1;
  case TY_ARRAY: return align_of(t->elem);
  case TY_STRUCT: {
    int64_t align = 1;
    for (const Field &f : t->fields) align = std::max(align, align_of(f.type));
    return align;
  }
  default: {
    int64_t size = size_of(t);
    return size > kWord ? kWord : (size ? size : 1);
  }
  }
}

const Field *find_field(const Type *t, const std::string &name) {
  if (!t || t->kind != TY_STRUCT) return nullptr;
  for (const Field &f : t->fields)
    if (f.name == name) return &f;
  return nullptr;
}
