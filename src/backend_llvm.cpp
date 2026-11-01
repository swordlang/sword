#include "backend_llvm.h"

#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

// A split module names its structs by where they are first used, so that a
// package's module reads the same whatever else the program declared.
using StructNames = std::map<const Type *, std::string>;

std::string type_text(const Type *t, const StructNames *names) {
  switch (t->kind) {
  case TY_VOID: return "void";
  case TY_BOOL: return "i1";
  case TY_INT: return "i" + std::to_string(t->bits);
  case TY_FLOAT: return t->bits == 32 ? "float" : "double";
  case TY_PTR: case TY_RAWPTR: case TY_FUNC: return "ptr";
  case TY_OPT: case TY_ATOMIC: case TY_ENUM: return type_text(t->elem, names);
  case TY_SLICE: case TY_STRING: return "%slice";
  case TY_ARRAY:
    return "[" + std::to_string(t->count) + " x " + type_text(t->elem, names) +
           "]";
  case TY_STRUCT:
    if (names) return "%" + names->at(t);
    return "%" + t->name;
  }
  return "void";
}

// An integer narrower than 32 bits crosses a call widened, and which side
// widens it is the platform's choice: Apple's arm64 has the caller do it, and
// x86-64 has the callee return a widened bool. Naming the extension on every
// signature and every call, as clang does for C, is right on all of them, and
// is what lets a C function and a Sword one be called through the same value.
const char *ext(const Type *t) {
  if (t->kind == TY_ENUM) t = t->elem;
  if (t->kind == TY_BOOL) return " zeroext";
  if (t->kind == TY_INT && t->bits < 32)
    return t->is_signed ? " signext" : " zeroext";
  return "";
}

std::string quote(const std::string &bytes) {
  static const char *hex = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : bytes) {
    if (c == '"' || c == '\\' || c < 0x20 || c >= 0x7f) {
      out += '\\';
      out += hex[c >> 4];
      out += hex[c & 15];
    } else {
      out += (char)c;
    }
  }
  return out;
}

// Sword's entry point returns `int`, but the process exit status is 32 bits,
// so the real @main is a wrapper around it.
const char *entry_name(const std::string &name) {
  return name == "main.main" ? "sword_main" : nullptr;
}

struct Emitter {
  const IrModule &mod;
  FILE *out;
  // The functions this module defines: every one, or a single unit's when the
  // build is split. A split module names only what its own functions reach —
  // strings, vtables, structs, declarations — so that it reads the same
  // whatever the rest of the program holds.
  std::vector<const IrFunc *> mine;
  bool split = false;
  std::vector<int> string_ids, vtable_ids; // the program's id -> this module's
  std::vector<int> strings_used, vtables_used;
  std::vector<const Type *> reached;
  StructNames struct_names;
  std::set<std::string> callees;
  const IrFunc *fn = nullptr;
  std::vector<std::string> operand; // value id -> LLVM operand text
  std::set<std::string> intrinsics;
  std::set<std::string> declared; // names the source already declares extern
  // A call to a variadic C function has to name the whole signature, not just
  // the return type: that is what tells the backend which arguments travel
  // where the callee looks for them.
  std::map<std::string, std::string> variadic_type;

  Emitter(const IrModule &m, FILE *o) : mod(m), out(o) {
    for (const IrFunc &f : mod.funcs)
      if (!f.is_extern) mine.push_back(&f);
  }

  Emitter(const IrModule &m, const std::string &unit, FILE *o)
      : mod(m), out(o), split(true) {
    for (const IrFunc &f : mod.funcs)
      if (!f.is_extern && f.unit == unit) mine.push_back(&f);
  }

  std::string ll_type(const Type *t) const {
    return type_text(t, split ? &struct_names : nullptr);
  }

  int string_id(int64_t id) const { return split ? string_ids[id] : (int)id; }
  int vtable_id(int64_t id) const { return split ? vtable_ids[id] : (int)id; }

  void reach(const Type *t) {
    while (t->kind == TY_OPT || t->kind == TY_ATOMIC || t->kind == TY_ENUM ||
           t->kind == TY_ARRAY)
      t = t->elem;
    if (t->kind != TY_STRUCT || struct_names.count(t)) return;
    struct_names[t] = "t" + std::to_string(reached.size());
    reached.push_back(t);
    for (const Field &f : t->fields) reach(f.type);
  }

  void reach_signature(const IrFunc &f) {
    for (const Type *p : f.params) reach(p);
    reach(f.ret);
  }

  // Numbers this module's strings and vtables from zero, in the order its
  // functions use them, and gathers every type and function they mention.
  void collect() {
    string_ids.assign(mod.strings.size(), -1);
    vtable_ids.assign(mod.vtables.size(), -1);
    for (const IrFunc *f : mine) {
      reach_signature(*f);
      for (const Type *t : f->value_type)
        if (t) reach(t);
      for (const IrBlock &bb : f->blocks)
        for (const IrInst &in : bb.insts) {
          if (in.type) reach(in.type);
          if (in.op == IR_STR_VALUE || in.op == IR_STR_DATA) {
            if (string_ids[in.imm] < 0) {
              string_ids[in.imm] = (int)strings_used.size();
              strings_used.push_back((int)in.imm);
            }
          } else if (in.op == IR_VTABLE) {
            if (vtable_ids[in.imm] < 0) {
              vtable_ids[in.imm] = (int)vtables_used.size();
              vtables_used.push_back((int)in.imm);
              for (const std::string &entry : mod.vtables[in.imm])
                callees.insert(entry);
            }
          } else if ((in.op == IR_CALL || in.op == IR_FUNCADDR) &&
                     !in.callee.empty()) {
            callees.insert(in.callee);
          }
        }
    }
    // By name, not in the program's order, which another package can change.
    std::map<std::string, const IrFunc *> by_name;
    for (const IrFunc &f : mod.funcs) by_name.emplace(f.name, &f);
    for (const std::string &name : callees) {
      auto found = by_name.find(name);
      if (found != by_name.end()) reach_signature(*found->second);
    }
  }

  const std::string &val(int id) const { return operand[id]; }
  std::string type_of(int id) const { return ll_type(fn->value_type[id]); }
  std::string typed(int id) const { return type_of(id) + " " + val(id); }
  std::string argument(int id) const {
    return type_of(id) + ext(fn->value_type[id]) + " " + val(id);
  }

  static int align_for(const Type *t) { return (int)align_of(t); }

  const char *arith(const IrInst &in) {
    bool sign = in.type->is_signed;
    if (in.type->kind == TY_FLOAT) {
      switch (in.op) {
      case IR_ADD: return "fadd";
      case IR_SUB: return "fsub";
      case IR_MUL: return "fmul";
      case IR_DIV: return "fdiv";
      case IR_MOD: return "frem";
      default: return nullptr;
      }
    }
    switch (in.op) {
    case IR_ADD: return "add";
    case IR_SUB: return "sub";
    case IR_MUL: return "mul";
    case IR_DIV: return sign ? "sdiv" : "udiv";
    case IR_MOD: return sign ? "srem" : "urem";
    case IR_AND: return "and";
    case IR_OR: return "or";
    case IR_XOR: return "xor";
    case IR_SHL: return "shl";
    case IR_SHR: return sign ? "ashr" : "lshr";
    default: return nullptr;
    }
  }

  const char *predicate(const IrInst &in) {
    const Type *operand = fn->value_type[in.a];
    if (operand->kind == TY_FLOAT) {
      switch (in.op) {
      case IR_EQ: return "oeq";
      // Unordered: a NaN is not equal to anything, itself included, so `x != x`
      // has to be true for one. With the ordered predicate both `==` and `!=`
      // answered false, which is the one thing that cannot be — and it is how
      // a NaN is recognised without reaching for its bits.
      case IR_NE: return "une";
      case IR_LT: return "olt";
      case IR_LE: return "ole";
      case IR_GT: return "ogt";
      case IR_GE: return "oge";
      default: return nullptr;
      }
    }
    bool sign = operand->is_signed;
    switch (in.op) {
    case IR_EQ: return "eq";
    case IR_NE: return "ne";
    case IR_LT: return sign ? "slt" : "ult";
    case IR_LE: return sign ? "sle" : "ule";
    case IR_GT: return sign ? "sgt" : "ugt";
    case IR_GE: return sign ? "sge" : "uge";
    default: return nullptr;
    }
  }

  std::string overflow_intrinsic(const IrInst &in) {
    const char *name = in.imm == IR_ADD ? "add"
                       : in.imm == IR_SUB ? "sub"
                                          : "mul";
    return std::string("llvm.") + (in.type->is_signed ? "s" : "u") + name +
           ".with.overflow." + ll_type(in.type);
  }

  void inst(const IrInst &in) {
    switch (in.op) {
    case IR_CONST:
      return; // folded into the operand table, no instruction needed

    case IR_STR_VALUE:
    case IR_STR_DATA:
    case IR_VTABLE:
    case IR_FUNCADDR:
      return; // globals are referenced by name

    case IR_ALLOCA:
      fprintf(out, "  %s = alloca %s, align %d\n", val(in.dst).c_str(),
              ll_type(in.type).c_str(), align_for(in.type));
      break;

    case IR_LOAD:
      fprintf(out, "  %s = load %s, ptr %s, align %d\n", val(in.dst).c_str(),
              ll_type(in.type).c_str(), val(in.a).c_str(), align_for(in.type));
      break;

    case IR_STORE:
      fprintf(out, "  store %s %s, ptr %s, align %d\n",
              ll_type(in.type).c_str(), val(in.a).c_str(), val(in.b).c_str(),
              align_for(in.type));
      break;

    case IR_MEMCPY:
      fprintf(out,
              "  call void @llvm.memcpy.p0.p0.i64(ptr %s, ptr %s, i64 %lld, "
              "i1 false)\n",
              val(in.a).c_str(), val(in.b).c_str(), (long long)in.imm);
      break;

    case IR_ZERO:
      fprintf(out,
              "  call void @llvm.memset.p0.i64(ptr %s, i8 0, i64 %lld, "
              "i1 false)\n",
              val(in.a).c_str(), (long long)in.imm);
      break;

    case IR_GEP_FIELD:
      fprintf(out, "  %s = getelementptr inbounds %s, ptr %s, i32 0, i32 %lld\n",
              val(in.dst).c_str(), ll_type(in.type).c_str(), val(in.a).c_str(),
              (long long)in.imm);
      break;

    case IR_GEP_INDEX:
      fprintf(out, "  %s = getelementptr inbounds %s, ptr %s, i64 %s\n",
              val(in.dst).c_str(), ll_type(in.type).c_str(), val(in.a).c_str(),
              val(in.b).c_str());
      break;

    case IR_CAST: {
      const Type *from = fn->value_type[in.a];
      const Type *to = in.type;
      auto is_address = [](const Type *x) {
        return x->kind == TY_PTR || x->kind == TY_RAWPTR ||
               x->kind == TY_FUNC || x->kind == TY_OPT;
      };
      const char *how;
      if (is_address(from) && to->kind == TY_INT)
        how = "ptrtoint";
      else if (from->kind == TY_INT && is_address(to))
        how = "inttoptr";
      else if (from->kind == TY_FLOAT && to->kind == TY_FLOAT)
        how = from->bits > to->bits ? "fptrunc" : "fpext";
      else if (from->kind == TY_FLOAT)
        how = to->is_signed ? "fptosi" : "fptoui";
      else if (to->kind == TY_FLOAT)
        how = from->is_signed ? "sitofp" : "uitofp";
      else
        how = from->bits > to->bits ? "trunc"
              : from->is_signed     ? "sext"
                                    : "zext";
      fprintf(out, "  %s = %s %s to %s\n", val(in.dst).c_str(), how,
              typed(in.a).c_str(), ll_type(to).c_str());
      break;
    }

    case IR_OVF: {
      std::string name = overflow_intrinsic(in);
      std::string ret = "{ " + ll_type(in.type) + ", i1 }";
      intrinsics.insert("declare " + ret + " @" + name + "(" +
                        ll_type(in.type) + ", " + ll_type(in.type) + ")");
      fprintf(out, "  %%t%d = call %s @%s(%s, %s)\n", in.dst, ret.c_str(),
              name.c_str(), typed(in.a).c_str(), typed(in.b).c_str());
      fprintf(out, "  %s = extractvalue %s %%t%d, 1\n", val(in.dst).c_str(),
              ret.c_str(), in.dst);
      break;
    }

    case IR_ATOMIC_RMW: {
      sim_point();
      static const char *names[] = {"add", "sub", "and", "or",  "xor",
                                    "min", "max", "xchg", "fadd"};
      const char *name = names[in.imm];
      // Unsigned minimum and maximum are spelled differently.
      if ((in.imm == RMW_MIN || in.imm == RMW_MAX) && !in.type->is_signed)
        name = in.imm == RMW_MIN ? "umin" : "umax";
      if (in.type->kind == TY_BOOL) {
        fprintf(out, "  %%wide%d = zext i1 %s to i8\n", in.dst,
                val(in.b).c_str());
        fprintf(out, "  %%raw%d = atomicrmw %s ptr %s, i8 %%wide%d seq_cst\n",
                in.dst, name, val(in.a).c_str(), in.dst);
        fprintf(out, "  %s = trunc i8 %%raw%d to i1\n", val(in.dst).c_str(),
                in.dst);
        break;
      }
      if (in.dst >= 0)
        fprintf(out, "  %s = atomicrmw %s ptr %s, %s %s seq_cst\n",
                val(in.dst).c_str(), name, val(in.a).c_str(),
                ll_type(in.type).c_str(), val(in.b).c_str());
      else
        fprintf(out, "  %%drop%d = atomicrmw %s ptr %s, %s %s seq_cst\n", in.a,
                name, val(in.a).c_str(), ll_type(in.type).c_str(),
                val(in.b).c_str());
      break;
    }

    case IR_ATOMIC_LOAD:
      sim_point();
      // LLVM will not touch an i1 atomically, so a bool travels as a byte.
      if (in.type->kind == TY_BOOL) {
        fprintf(out, "  %%wide%d = load atomic i8, ptr %s seq_cst, align 1\n",
                in.dst, val(in.a).c_str());
        fprintf(out, "  %s = trunc i8 %%wide%d to i1\n", val(in.dst).c_str(),
                in.dst);
      } else {
        fprintf(out, "  %s = load atomic %s, ptr %s seq_cst, align %d\n",
                val(in.dst).c_str(), ll_type(in.type).c_str(),
                val(in.a).c_str(), align_for(in.type));
      }
      break;

    case IR_ATOMIC_STORE:
      sim_point();
      if (in.type->kind == TY_BOOL) {
        fprintf(out, "  %%wide%d = zext i1 %s to i8\n", in.a,
                val(in.a).c_str());
        fprintf(out, "  store atomic i8 %%wide%d, ptr %s seq_cst, align 1\n",
                in.a, val(in.b).c_str());
      } else {
        fprintf(out, "  store atomic %s %s, ptr %s seq_cst, align %d\n",
                ll_type(in.type).c_str(), val(in.a).c_str(),
                val(in.b).c_str(), align_for(in.type));
      }
      break;

    case IR_ATOMIC_CAS: {
      sim_point();
      if (in.type->kind == TY_BOOL) {
        fprintf(out, "  %%exp%d = zext i1 %s to i8\n", in.dst,
                val(in.b).c_str());
        fprintf(out, "  %%des%d = zext i1 %s to i8\n", in.dst,
                val(in.args[0]).c_str());
        fprintf(out,
                "  %%cas%d = cmpxchg ptr %s, i8 %%exp%d, i8 %%des%d seq_cst "
                "seq_cst\n",
                in.dst, val(in.a).c_str(), in.dst, in.dst);
        fprintf(out, "  %s = extractvalue { i8, i1 } %%cas%d, 1\n",
                val(in.dst).c_str(), in.dst);
        break;
      }
      std::string pair = "{ " + ll_type(in.type) + ", i1 }";
      fprintf(out, "  %%cas%d = cmpxchg ptr %s, %s %s, %s %s seq_cst seq_cst\n",
              in.dst, val(in.a).c_str(), ll_type(in.type).c_str(),
              val(in.b).c_str(), ll_type(in.type).c_str(),
              val(in.args[0]).c_str());
      fprintf(out, "  %s = extractvalue %s %%cas%d, 1\n", val(in.dst).c_str(),
              pair.c_str(), in.dst);
      break;
    }

    case IR_NEG:
      if (in.type->kind == TY_FLOAT)
        fprintf(out, "  %s = fneg %s %s\n", val(in.dst).c_str(),
                ll_type(in.type).c_str(), val(in.a).c_str());
      else
        fprintf(out, "  %s = sub %s 0, %s\n", val(in.dst).c_str(),
                ll_type(in.type).c_str(), val(in.a).c_str());
      break;

    case IR_NOT:
      fprintf(out, "  %s = xor i1 %s, true\n", val(in.dst).c_str(),
              val(in.a).c_str());
      break;

    case IR_SQRT:
      intrinsics.insert("declare double @llvm.sqrt.f64(double)");
      fprintf(out, "  %s = call double @llvm.sqrt.f64(%s)\n",
              val(in.dst).c_str(), typed(in.a).c_str());
      break;

    case IR_EQ: case IR_NE: case IR_LT: case IR_LE: case IR_GT: case IR_GE:
      fprintf(out, "  %s = %s %s %s %s, %s\n", val(in.dst).c_str(),
              fn->value_type[in.a]->kind == TY_FLOAT ? "fcmp" : "icmp",
              predicate(in), type_of(in.a).c_str(), val(in.a).c_str(),
              val(in.b).c_str());
      break;

    case IR_CALL: {
      std::string name = in.callee;
      if (const char *renamed = entry_name(name)) name = renamed;
      fputs("  ", out);
      if (in.dst >= 0) fprintf(out, "%s = ", val(in.dst).c_str());
      // An empty callee means the target came out of a vtable.
      auto variadic = variadic_type.find(name);
      const char *widened = ext(in.type);
      if (name.empty())
        fprintf(out, "call%s %s %s(", widened, ll_type(in.type).c_str(),
                val(in.a).c_str());
      else if (variadic != variadic_type.end())
        fprintf(out, "call%s %s @%s(", widened, variadic->second.c_str(),
                name.c_str());
      else
        fprintf(out, "call%s %s @%s(", widened, ll_type(in.type).c_str(),
                name.c_str());
      for (size_t i = 0; i < in.args.size(); i++)
        fprintf(out, "%s%s", i ? ", " : "", argument(in.args[i]).c_str());
      fputs(")\n", out);
      break;
    }

    case IR_BR:
      fprintf(out, "  br label %%bb%d\n", in.target);
      break;

    case IR_CONDBR:
      fprintf(out, "  br i1 %s, label %%bb%d, label %%bb%d\n",
              val(in.a).c_str(), in.target, in.target2);
      break;

    case IR_RET:
      if (in.a >= 0) fprintf(out, "  ret %s\n", typed(in.a).c_str());
      else fputs("  ret void\n", out);
      break;

    case IR_UNREACHABLE:
      fputs("  unreachable\n", out);
      break;

    default:
      fprintf(out, "  %s = %s %s %s, %s\n", val(in.dst).c_str(), arith(in),
              ll_type(in.type).c_str(), val(in.a).c_str(), val(in.b).c_str());
      break;
    }
  }

  void signature(const IrFunc &f, const char *keyword, bool named) {
    const char *renamed = entry_name(f.name);
    const char *name = renamed ? renamed : f.name.c_str();
    std::string ret =
        f.ret_by_pointer ? "void" : ext(f.ret) + std::string(" ") + ll_type(f.ret);
    if (ret[0] == ' ') ret.erase(0, 1);

    fprintf(out, "%s %s @%s(", keyword, ret.c_str(), name);
    int index = 0;
    if (f.ret_by_pointer) {
      fputs(named ? "ptr %v0" : "ptr", out);
      index = 1;
    }
    for (size_t i = 0; i < f.params.size(); i++) {
      if (index) fputs(", ", out);
      std::string type = is_aggregate(f.params[i])
                             ? "ptr"
                             : ll_type(f.params[i]) + ext(f.params[i]);
      if (named) fprintf(out, "%s %%v%d", type.c_str(), index);
      else fputs(type.c_str(), out);
      index++;
    }
    if (f.is_c_variadic) fputs(index ? ", ..." : "...", out);
    fputc(')', out);
  }

  // `i32 (i32, i64, ...)`, which every call to that function has to repeat.
  std::string variadic_signature(const IrFunc &f) {
    std::string text = ll_type(f.ret) + " (";
    for (const Type *p : f.params)
      text += (is_aggregate(p) ? "ptr" : ll_type(p)) + std::string(", ");
    return text + "...)";
  }

  void func(const IrFunc &f) {
    if (f.is_extern) {
      if (f.is_c_variadic) variadic_type[f.name] = variadic_signature(f);
      if (!declared.insert(f.name).second) return;
      signature(f, f.is_weak ? "declare extern_weak" : "declare", false);
      fputc('\n', out);
      return;
    }

    fn = &f;
    operand.assign(f.value_type.size(), "");
    for (size_t i = 0; i < operand.size(); i++)
      operand[i] = "%v" + std::to_string(i);

    // Constants and globals have no instruction in LLVM; they are spelled
    // inline wherever they are used.
    for (const IrBlock &bb : f.blocks) {
      for (const IrInst &in : bb.insts) {
        if (in.op == IR_CONST) {
          if (in.type->kind == TY_FLOAT) {
            double value = in.type->bits == 32 ? (double)(float)in.fimm
                                               : in.fimm;
            uint64_t bits;
            memcpy(&bits, &value, sizeof(bits));
            char buf[32];
            snprintf(buf, sizeof(buf), "0x%016llX",
                     (unsigned long long)bits);
            operand[in.dst] = buf;
          } else {
            operand[in.dst] = ll_type(in.type) == "ptr" && in.imm == 0
                                  ? "null"
                                  : std::to_string(in.imm);
          }
        }
        else if (in.op == IR_STR_VALUE)
          operand[in.dst] = "@strv." + std::to_string(string_id(in.imm));
        else if (in.op == IR_STR_DATA)
          operand[in.dst] = "@str." + std::to_string(string_id(in.imm));
        else if (in.op == IR_VTABLE)
          operand[in.dst] = "@vt." + std::to_string(vtable_id(in.imm));
        else if (in.op == IR_FUNCADDR)
          operand[in.dst] = "@" + in.callee;
      }
    }

    // Split, a function is reached from other objects, and hidden keeps it
    // out of the executable's exports all the same.
    const char *linkage = split ? (f.is_local ? "define internal" : "define hidden")
                                : (f.is_internal ? "define internal" : "define");
    signature(f, linkage, true);
    fputs(" #0 {\n", out);
    for (const IrBlock &bb : f.blocks) {
      fprintf(out, "bb%d:\n", bb.id);
      for (const IrInst &in : bb.insts) inst(in);
    }
    fputs("}\n\n", out);
  }

  // A program only gets the argument vector if it asks: `main` takes no
  // parameters otherwise, and nothing from the runtime gets linked in.
  bool wants_args() const {
    for (const IrFunc &f : mod.funcs)
      for (const IrBlock &bb : f.blocks)
        for (const IrInst &in : bb.insts)
          if (in.op == IR_CALL &&
              (in.callee == "sword_os_argc" || in.callee == "sword_os_arg"))
            return true;
    return false;
  }

  // `main` is a task like any other, which is what lets it wait on a
  // descriptor: parking is a fiber's trick, and a program whose whole job is
  // one loop over one descriptor used to have to open a scope and spawn
  // itself. The runtime takes a thunk and an argument block, so the answer
  // comes back through a slot on the real stack.
  void main_task(const IrFunc &f) {
    fputs("define internal zeroext i16 @sword_main.task(ptr %args) #0 {\nbb0:\n",
          out);
    if (f.ret->is_error_union) {
      fputs("  %slot = load ptr, ptr %args, align 8\n"
            "  call void @sword_main(ptr %slot)\n",
            out);
    } else if (f.ret->kind == TY_VOID) {
      fputs("  call void @sword_main()\n", out);
    } else {
      std::string ret = ll_type(f.ret);
      fputs("  %slot = load ptr, ptr %args, align 8\n", out);
      fprintf(out, "  %%r = call %s @sword_main()\n", ret.c_str());
      fprintf(out, "  store %s %%r, ptr %%slot, align %d\n", ret.c_str(),
              (int)align_of(f.ret));
    }
    fputs("  ret i16 0\n}\n\n", out);
  }

  // Hands the thunk to the runtime, with the address of the slot its answer
  // goes in. The scope's own failure code says nothing here: the thunk always
  // succeeds and what main returned is in the slot.
  void run_main(const char *slot) {
    if (slot) {
      fprintf(out,
              "  %%args = alloca ptr, align 8\n"
              "  store ptr %s, ptr %%args, align 8\n"
              "  call i16 @sword_run_main(ptr @sword_main.task, ptr %%args,"
              " i64 8)\n",
              slot);
    } else {
      fputs("  call i16 @sword_run_main(ptr @sword_main.task, ptr null,"
            " i64 0)\n",
            out);
    }
  }

  void entry_wrapper(const IrFunc &f) {
    main_task(f);
    if (wants_args()) {
      fputs("define i32 @main(i32 %argc, ptr %argv) #0 {\nbb0:\n"
            "  call void @sword_os_set_args(i32 %argc, ptr %argv)\n",
            out);
    } else {
      fputs("define i32 @main() #0 {\nbb0:\n", out);
    }
    if (f.ret->is_error_union) {
      // A fallible main reports the error code as its exit status, and its
      // ordinary result when it succeeds.
      const Type *payload = f.ret->elem;
      const Field *code = find_field(f.ret, "code");
      std::string box = ll_type(f.ret);
      fprintf(out, "  %%box = alloca %s, align %d\n", box.c_str(),
              (int)align_of(f.ret));
      run_main("%box");
      fprintf(out,
              "  %%at = getelementptr inbounds %s, ptr %%box, i32 0, i32 %d\n",
              box.c_str(), code->index);
      fputs("  %code = load i16, ptr %at, align 2\n"
            "  %failed = icmp ne i16 %code, 0\n"
            "  br i1 %failed, label %bad, label %good\n"
            "bad:\n"
            // An exit status is eight bits, and a code of 256 cut to them is
            // 0, which says it worked. Past 255 every code says 255.
            "  %wide = zext i16 %code to i32\n"
            "  %over = icmp ugt i32 %wide, 255\n"
            "  %status = select i1 %over, i32 255, i32 %wide\n"
            "  ret i32 %status\n"
            "good:\n",
            out);
      if (payload->kind == TY_VOID) {
        fputs("  ret i32 0\n", out);
      } else {
        const Field *value = find_field(f.ret, "value");
        fprintf(out,
                "  %%vat = getelementptr inbounds %s, ptr %%box, i32 0, i32 %d\n",
                box.c_str(), value->index);
        fprintf(out, "  %%r = load %s, ptr %%vat, align %d\n",
                ll_type(payload).c_str(), (int)align_of(payload));
        if (payload->bits == 32)
          fputs("  ret i32 %r\n", out);
        else
          fprintf(out, "  %%res = trunc %s %%r to i32\n  ret i32 %%res\n",
                  ll_type(payload).c_str());
      }
    } else if (f.ret->kind == TY_VOID) {
      run_main(nullptr);
      fputs("  ret i32 0\n", out);
    } else {
      fprintf(out, "  %%box = alloca %s, align %d\n", ll_type(f.ret).c_str(),
              (int)align_of(f.ret));
      run_main("%box");
      fprintf(out, "  %%r = load %s, ptr %%box, align %d\n",
              ll_type(f.ret).c_str(), (int)align_of(f.ret));
      if (f.ret->bits == 32) {
        fputs("  ret i32 %r\n", out);
      } else {
        fprintf(out, "  %%code = trunc %s %%r to i32\n  ret i32 %%code\n",
                ll_type(f.ret).c_str());
      }
    }
    fputs("}\n\n", out);
  }

  // A failed safety check writes its message to stderr and aborts. It is only
  // emitted when something can reach it. The line is appended here, so a
  // program keeps one copy of each message rather than one per check; the whole
  // of it still goes out in one write, which another thread's cannot split.
  // The longest message is under 64 bytes, and a line under 21 digits.
  void panic_helper() {
    if (!declared.count("write"))
      fputs("declare i64 @write(i32, ptr, i64)\n", out);
    if (!declared.count("abort"))
      fputs("declare void @abort()\n", out);
    fputs("\ndefine internal void @sword_panic(ptr %msg, i64 %len, i64 %line)"
          " noreturn #0 {\n"
          "bb0:\n"
          "  %buf = alloca [96 x i8]\n"
          "  %digits = alloca [24 x i8]\n"
          "  call void @llvm.memcpy.p0.p0.i64(ptr %buf, ptr %msg, i64 %len, i1 false)\n"
          "  br label %bb1\n"
          "bb1:\n"
          "  %n = phi i64 [ %line, %bb0 ], [ %q, %bb1 ]\n"
          "  %at = phi i64 [ 24, %bb0 ], [ %next, %bb1 ]\n"
          "  %q = udiv i64 %n, 10\n"
          "  %r = urem i64 %n, 10\n"
          "  %r8 = trunc i64 %r to i8\n"
          "  %c = add i8 %r8, 48\n"
          "  %next = sub i64 %at, 1\n"
          "  %p = getelementptr i8, ptr %digits, i64 %next\n"
          "  store i8 %c, ptr %p\n"
          "  %more = icmp ne i64 %q, 0\n"
          "  br i1 %more, label %bb1, label %bb2\n"
          "bb2:\n"
          "  %count = sub i64 24, %next\n"
          "  %dst = getelementptr i8, ptr %buf, i64 %len\n"
          "  call void @llvm.memcpy.p0.p0.i64(ptr %dst, ptr %p, i64 %count, i1 false)\n"
          "  %end = getelementptr i8, ptr %dst, i64 %count\n"
          "  store i8 10, ptr %end\n"
          "  %body = add i64 %len, %count\n"
          "  %total = add i64 %body, 1\n"
          "  %w = call i64 @write(i32 2, ptr %buf, i64 %total)\n"
          "  call void @abort()\n"
          "  unreachable\n"
          "}\n\n",
          out);
  }

  // The scheduler lives in libsword_rt.a; generated code only needs to know
  // the shapes it calls.
  void runtime_declarations() {
    static const struct {
      const char *name;
      const char *decl;
    } runtime[] = {
        {"sword_scope_begin", "declare void @sword_scope_begin(ptr)"},
        {"sword_scope_spawn",
         "declare void @sword_scope_spawn(ptr, ptr, ptr, i64)"},
        {"sword_scope_end", "declare i16 @sword_scope_end(ptr)"},
        {"sword_parallel_for",
         "declare i16 @sword_parallel_for(i64, i64, ptr, ptr, ptr, i64)"},
        {"sword_mutex_lock", "declare void @sword_mutex_lock(ptr)"},
        {"sword_mutex_unlock", "declare void @sword_mutex_unlock(ptr)"},
        {"sword_mutex_lock_read", "declare void @sword_mutex_lock_read(ptr)"},
        {"sword_mutex_unlock_read",
         "declare void @sword_mutex_unlock_read(ptr)"},
        {"sword_mutex_try_lock",
         "declare zeroext i1 @sword_mutex_try_lock(ptr)"},
        {"sword_mutex_try_lock_read",
         "declare zeroext i1 @sword_mutex_try_lock_read(ptr)"},
        {"sword_mutex_wait", "declare void @sword_mutex_wait(ptr)"},
        {"sword_mutex_notify", "declare void @sword_mutex_notify(ptr)"},
        {"sword_chan_select",
         "declare i64 @sword_chan_select(ptr, ptr, ptr, i64, i32, ptr)"},
        {"sword_mutex_notify_all",
         "declare void @sword_mutex_notify_all(ptr)"},
        {"sword_offload", "declare i64 @sword_offload(ptr, ptr)"},
        {"sword_errno", "declare i32 @sword_errno()"},
        {"sword_set_errno", "declare void @sword_set_errno(i32)"},
    };
    bool any = false;
    for (const auto &entry : runtime) {
      // std/os declares sword_errno itself, and one declaration is enough.
      if (!calls(entry.name) || declared.count(entry.name)) continue;
      fprintf(out, "%s\n", entry.decl);
      any = true;
    }
    if (mod.sim_points) {
      fputs("declare void @sword_sim_point()\n", out);
      any = true;
    }
    if (calls("memcmp") && !declared.count("memcmp")) {
      fputs("declare i32 @memcmp(ptr, ptr, i64)\n", out);
      any = true;
    }
    // These are called by the entry wrapper rather than by lowered code.
    if (wants_args()) {
      fputs("declare void @sword_os_set_args(i32, ptr)\n", out);
      any = true;
    }
    for (const IrFunc *f : mine)
      if (f->name == "main.main") {
        fputs("declare i16 @sword_run_main(ptr, ptr, i64)\n", out);
        any = true;
      }
    if (any) fputc('\n', out);
  }

  // Before an atomic operation, in a build for the simulator.
  void sim_point() {
    if (mod.sim_points) fputs("  call void @sword_sim_point()\n", out);
  }

  bool calls(const std::string &name) const {
    for (const IrFunc *f : mine)
      for (const IrBlock &bb : f->blocks)
        for (const IrInst &in : bb.insts)
          if (in.op == IR_CALL && in.callee == name) return true;
    return false;
  }

  bool uses_panic() const { return calls("sword_panic"); }

  void run() {
    fputs("; generated by shield\n\n", out);
    if (split) collect();

    // Both are part of the language rather than of any program, and `any`
    // refers to `slice`, so neither is worth making conditional.
    fputs("%slice = type { ptr, i64 }\n", out);

    std::vector<const Type *> structs(mod.structs.begin(), mod.structs.end());
    if (split) structs = reached;
    for (const Type *s : structs) {
      fprintf(out, "%s = type { ", ll_type(s).c_str());
      for (size_t i = 0; i < s->fields.size(); i++)
        fprintf(out, "%s%s", i ? ", " : "", ll_type(s->fields[i].type).c_str());
      fputs(" }\n", out);
    }
    fputc('\n', out);

    size_t strings = split ? strings_used.size() : mod.strings.size();
    for (size_t i = 0; i < strings; i++) {
      const std::string &text = mod.strings[split ? strings_used[i] : i];
      fprintf(out, "@str.%zu = private unnamed_addr constant [%zu x i8] c\"%s\"\n",
              i, text.size(), quote(text).c_str());
      fprintf(out,
              "@strv.%zu = private unnamed_addr constant %%slice "
              "{ ptr @str.%zu, i64 %zu }\n",
              i, i, text.size());
    }
    if (strings) fputc('\n', out);

    size_t vtables = split ? vtables_used.size() : mod.vtables.size();
    for (size_t i = 0; i < vtables; i++) {
      const auto &entries = mod.vtables[split ? vtables_used[i] : i];
      fprintf(out, "@vt.%zu = private unnamed_addr constant [%zu x ptr] [", i,
              entries.size());
      for (size_t j = 0; j < entries.size(); j++)
        fprintf(out, "%sptr @%s", j ? ", " : "", entries[j].c_str());
      fputs("]\n", out);
    }
    if (vtables) fputc('\n', out);

    // Extern declarations first: the panic helper needs to know whether the
    // source already declared write or abort.
    for (const IrFunc &f : mod.funcs)
      if (f.is_extern && (!split || callees.count(f.name))) func(f);
    if (split) {
      std::set<std::string> defined;
      for (const IrFunc *f : mine) defined.insert(f->name);
      for (const IrFunc &f : mod.funcs) {
        if (f.is_extern || defined.count(f.name) || !callees.count(f.name))
          continue;
        signature(f, "declare hidden", false);
        fputc('\n', out);
      }
    }
    if (!mod.funcs.empty()) fputc('\n', out);

    bool panics = uses_panic();
    if (panics) panic_helper();
    runtime_declarations();

    const IrFunc *entry = nullptr;
    for (const IrFunc *f : mine) {
      func(*f);
      if (f->name == "main.main") entry = f;
    }
    if (entry) entry_wrapper(*entry);

    bool uses_memcpy = panics;
    bool uses_memset = false;
    for (const IrFunc *f : mine)
      for (const IrBlock &bb : f->blocks)
        for (const IrInst &in : bb.insts) {
          if (in.op == IR_MEMCPY) uses_memcpy = true;
          if (in.op == IR_ZERO) uses_memset = true;
        }
    if (uses_memcpy)
      fputs("declare void @llvm.memcpy.p0.p0.i64(ptr, ptr, i64, i1)\n", out);
    if (uses_memset)
      fputs("declare void @llvm.memset.p0.i64(ptr, i8, i64, i1)\n", out);
    for (const std::string &decl : intrinsics) fprintf(out, "%s\n", decl.c_str());

    // Nothing in Sword unwinds: a panic aborts and the runtime throws nothing,
    // so no function needs a landing pad. The tables a debugger or a profiler
    // walks the stack with are kept unless the program asked to be small.
    fprintf(out, "\nattributes #0 = { nounwind%s%s }\n",
            mod.unwind_tables ? " uwtable(sync)" : "",
            mod.optimize_size ? " minsize optsize" : "");
  }
};

} // namespace

void emit_llvm(const IrModule &mod, FILE *out) { Emitter(mod, out).run(); }

std::vector<std::string> llvm_units(const IrModule &mod) {
  std::vector<std::string> units;
  std::set<std::string> seen;
  for (const IrFunc &f : mod.funcs)
    if (!f.is_extern && seen.insert(f.unit).second) units.push_back(f.unit);
  return units;
}

void emit_llvm_unit(const IrModule &mod, const std::string &unit, FILE *out) {
  Emitter(mod, unit, out).run();
}
