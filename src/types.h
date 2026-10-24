#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

enum TypeKind {
  TY_VOID,
  TY_BOOL,
  TY_INT,
  TY_FLOAT,
  TY_PTR,    // *T, never null
  TY_RAWPTR, // [*]T, no length, FFI only
  TY_SLICE,  // []T, pointer + length
  TY_STRING, // immutable []u8 with its own identity
  TY_ARRAY,  // [N]T, by value
  TY_OPT,    // ?T
  TY_ATOMIC, // atomic[T]: T's representation, reached only through its
             // own operations, which is what lets it cross between tasks
  TY_STRUCT,
  TY_ENUM, // a named set of integers: its own type, so no arithmetic on it
  TY_FUNC,
};

struct Type;

struct Field {
  std::string name;
  Type *type = nullptr;
  int64_t offset = 0;
  int index = 0; // position after layout, which is what the backend indexes by
};

struct Type {
  TypeKind kind = TY_VOID;
  int bits = 0;
  bool is_signed = false;
  bool untyped = false;
  bool is_extern = false; // struct laid out in declaration order, for C
  // `!T` is a struct of { code, value } underneath, which gives it layout,
  // field access and codegen for free. `?T` does the same for payloads that
  // have no spare bit pattern; over a pointer it is just the pointer.
  bool is_error_union = false;
  bool is_optional = false;
  // An interface is a { data, vtable } pair: a struct for layout purposes,
  // with the method signatures kept alongside for checking.
  bool is_interface = false;
  // `any`: what a variadic argument is boxed into. It carries the value and
  // enough of a tag to say what the value is.
  bool is_any = false;
  // `shared[T]` is a { guard, value } pair. The guard is a mutex the runtime
  // works in place and generated code never looks inside, and `lock` is the
  // only way to reach the value.
  bool is_shared = false;
  Type *elem = nullptr;
  int64_t count = 0; // TY_ARRAY length
  std::string name;  // TY_STRUCT
  std::vector<Field> fields;
  std::vector<Field> methods; // TY_STRUCT with is_interface
  std::vector<Type *> params;
  // TY_FUNC: which parameters the callee may write through. Part of the type,
  // because passing a function that writes where the caller expected one that
  // does not would hand out permission nobody granted.
  std::vector<bool> param_mut;
  // TY_FUNC on an extern: C's `...`. The call site is what decides how the
  // extra arguments travel, so the call has to carry the signature too.
  bool is_c_variadic = false;
  Type *ret = nullptr;
};

struct TypeTable {
  TypeTable();

  Type *named(const std::string &name);
  Type *ptr(Type *elem);
  Type *rawptr(Type *elem);
  Type *slice(Type *elem);
  Type *array(Type *elem, int64_t count);
  Type *opt(Type *elem);
  Type *atomic(Type *elem);
  Type *shared(Type *elem);
  Type *func(std::vector<Type *> params, Type *ret,
             std::vector<bool> param_mut = {});

  Type *declare_struct(const std::string &name);
  Type *declare_enum(const std::string &name, Type *width);
  Type *declare_interface(const std::string &name);
  // Assigns offsets. Non-extern structs are reordered widest-first so padding
  // does not leak into every instance.
  void layout_struct(Type *type, std::vector<Field> fields);
  // Lays every struct out again until no size moves. A struct's size depends on
  // the structs it holds by value, and declaration order says nothing about
  // which of those comes first — while `?T`, `!T` and `shared[T]` are built while
  // a field is being resolved, before the T they wrap may have a size at all.
  // Layout only ever grows a size, so settling terminates.
  void settle_layouts();

  Type *error_union(Type *value);
  // Every `!T` the program mentions, in creation order, so the backend can
  // emit a named type for each.
  const std::vector<Type *> &error_unions_made() const {
    return error_union_list;
  }
  // Optionals over a non-pointer payload are structs too, and need naming.
  const std::vector<Type *> &optionals_made() const { return optional_list; }
  const std::vector<Type *> &shareds_made() const { return shared_list; }
  // Instantiated generic structs, which the backend has to name as well.
  void note_instance(Type *type) { instance_list.push_back(type); }
  const std::vector<Type *> &instances_made() const { return instance_list; }

  // One code per error name, global to the program: `error.Timeout` raised in
  // two packages is one error, which is what lets a caller handle it once. Code
  // 0 always means success. Declared rather than conjured, so a misspelled name
  // is a compile error instead of a new error nobody handles.
  struct ErrorDecl {
    std::string name;
    std::string message; // empty when declared without one
    std::string owner;   // the package prefix that declared it
    // False for a standard library error this program does not declare: its
    // code is kept for it, and nothing may use the name.
    bool declared = true;
  };
  // Returns the code. `clash` is set when the name is already declared with a
  // different message, which the caller reports.
  int declare_error(const std::string &name, const std::string &message,
                    const std::string &owner, const ErrorDecl **clash);
  // Zero when nothing declared it.
  int error_code(const std::string &name) const;
  const ErrorDecl *error_at(int code) const;
  const std::vector<ErrorDecl> &errors() const { return error_list; }

  // One vtable per (concrete type, interface) pair actually used.
  struct VTable {
    std::string key;
    std::vector<std::string> entries; // mangled Type.method names, in order
  };
  int vtable(const std::string &key, std::vector<std::string> entries);
  const std::vector<VTable> &vtables() const { return vtable_list; }

  Type *void_ty;
  Type *bool_ty;
  Type *int_ty;
  Type *u8_ty;
  Type *usize_ty;
  Type *string_ty;
  Type *error_ty;
  Type *any_ty;
  Type *untyped_nil;
  Type *untyped_int;
  Type *untyped_float;

private:
  Type *add(Type t);
  std::deque<Type> pool;
  std::unordered_map<std::string, Type *> by_name;
  std::unordered_map<Type *, Type *> ptrs, rawptrs, slices, opts, error_unions;
  std::unordered_map<Type *, Type *> atomics, shareds;
  std::map<std::pair<Type *, int64_t>, Type *> arrays;
  std::vector<ErrorDecl> error_list;
  std::vector<Type *> error_union_list;
  std::vector<Type *> optional_list;
  std::vector<Type *> shared_list;
  std::vector<Type *> instance_list;
  std::vector<VTable> vtable_list;
};

std::string type_str(const Type *t);
// Strips the program package's own prefix, which is for the linker and not for
// a person reading an error.
std::string shown_name(const std::string &name);
bool type_eq(const Type *a, const Type *b);
bool assignable(const Type *from, const Type *to);

bool is_numeric(const Type *t);
bool is_integer(const Type *t);
bool is_float(const Type *t);
bool is_atomic(const Type *t);
bool is_shared(const Type *t);

// How much room `shared[T]` sets aside for the mutex, in bytes. Generated code
// only ever passes its address to the runtime.
//
// **Must equal SWORD_GUARD_SIZE in rt/sword_rt.h.** The runtime builds a mutex
// and a wait list in this space; too little here and it writes past the end,
// which is a segfault a long way from the cause. The static_assert over there
// only catches the runtime growing, not this shrinking.
enum { SWORD_GUARD_SIZE = 32 };

// The tags an `any` can carry. std/fmt mirrors these; they are part of the
// language rather than of that package.
enum AnyKind {
  ANY_NONE = 0, ANY_BOOL = 1, ANY_INT = 2, ANY_UINT = 3,
  ANY_FLOAT = 4, ANY_STRING = 5, ANY_POINTER = 6,
};
int any_kind_of(const Type *t); // -1 when the type cannot be boxed

// `?T`: either a pointer using null as its tag, or a { has, value } pair.
bool is_optional(const Type *t);
const Type *opt_payload(const Type *t);

// Aggregates live in memory: an expression of aggregate type evaluates to its
// address, and assigning one copies bytes.
bool is_aggregate(const Type *t);

int64_t size_of(const Type *t);
int64_t align_of(const Type *t);

const Field *find_field(const Type *t, const std::string &name);
