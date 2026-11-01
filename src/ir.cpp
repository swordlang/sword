#include "ir.h"

namespace {

const char *op_name(IrOp op) {
  switch (op) {
  case IR_CONST: return "const";
  case IR_ALLOCA: return "alloca";
  case IR_LOAD: return "load";
  case IR_STORE: return "store";
  case IR_MEMCPY: return "memcpy";
  case IR_ZERO: return "zero";
  case IR_ADD: return "add";
  case IR_SUB: return "sub";
  case IR_MUL: return "mul";
  case IR_DIV: return "div";
  case IR_MOD: return "mod";
  case IR_AND: return "and";
  case IR_OR: return "or";
  case IR_XOR: return "xor";
  case IR_SHL: return "shl";
  case IR_SHR: return "shr";
  case IR_EQ: return "eq";
  case IR_NE: return "ne";
  case IR_LT: return "lt";
  case IR_LE: return "le";
  case IR_GT: return "gt";
  case IR_GE: return "ge";
  case IR_NEG: return "neg";
  case IR_NOT: return "not";
  case IR_CAST: return "cast";
  case IR_SQRT: return "sqrt";
  case IR_ATOMIC_RMW: return "atomic.rmw";
  case IR_ATOMIC_LOAD: return "atomic.load";
  case IR_ATOMIC_STORE: return "atomic.store";
  case IR_ATOMIC_CAS: return "atomic.cas";
  case IR_OVF: return "overflow";
  case IR_GEP_FIELD: return "field";
  case IR_GEP_INDEX: return "elem";
  case IR_STR_VALUE: return "str";
  case IR_STR_DATA: return "strdata";
  case IR_VTABLE: return "vtable";
  case IR_FUNCADDR: return "funcaddr";
  case IR_CALL: return "call";
  case IR_BR: return "br";
  case IR_CONDBR: return "condbr";
  case IR_RET: return "ret";
  case IR_UNREACHABLE: return "unreachable";
  }
  return "?";
}

} // namespace

void ir_print(const IrModule &mod, FILE *out) {
  for (size_t i = 0; i < mod.strings.size(); i++)
    fprintf(out, "str %zu = \"%s\"\n", i, mod.strings[i].c_str());
  if (!mod.strings.empty()) fputc('\n', out);

  for (Type *s : mod.structs) {
    fprintf(out, "struct %s {   // %lld bytes\n", s->name.c_str(),
            (long long)size_of(s));
    for (const Field &f : s->fields)
      fprintf(out, "  +%-3lld %s %s\n", (long long)f.offset, f.name.c_str(),
              type_str(f.type).c_str());
    fputs("}\n\n", out);
  }

  for (const IrFunc &fn : mod.funcs) {
    if (fn.is_extern) {
      fprintf(out, "extern func %s%s\n", fn.name.c_str(),
              fn.is_c_variadic ? " ..." : "");
      continue;
    }
    fprintf(out, "func %s(", fn.name.c_str());
    size_t base = fn.ret_by_pointer ? 1 : 0;
    if (fn.ret_by_pointer) fputs("%0 out, ", out);
    for (size_t i = 0; i < fn.params.size(); i++)
      fprintf(out, "%s%%%zu %s", i ? ", " : "", i + base,
              type_str(fn.params[i]).c_str());
    fprintf(out, ") %s {\n", type_str(fn.ret).c_str());

    for (const IrBlock &bb : fn.blocks) {
      fprintf(out, "bb%d:\n", bb.id);
      for (const IrInst &in : bb.insts) {
        fputs("  ", out);
        if (in.dst >= 0) fprintf(out, "%%%d = ", in.dst);
        fputs(op_name(in.op), out);

        switch (in.op) {
        case IR_CONST:
          fprintf(out, " %lld", (long long)in.imm);
          break;
        case IR_ALLOCA:
          fprintf(out, " %s", type_str(in.type).c_str());
          break;
        case IR_LOAD:
          fprintf(out, " %%%d", in.a);
          break;
        case IR_STORE:
          fprintf(out, " %%%d, %%%d", in.a, in.b);
          break;
        case IR_MEMCPY:
          fprintf(out, " %%%d, %%%d, %lld", in.a, in.b, (long long)in.imm);
          break;
        case IR_ZERO:
          fprintf(out, " %%%d, %lld", in.a, (long long)in.imm);
          break;
        case IR_GEP_FIELD:
          // Slices reuse this instruction for their { ptr, len } pair.
          if (in.type->kind != TY_STRUCT)
            fprintf(out, " %%%d, .%s", in.a, in.imm == 0 ? "ptr" : "len");
          else
            fprintf(out, " %%%d, %s.%s", in.a, in.type->name.c_str(),
                    in.type->fields[in.imm].name.c_str());
          break;
        case IR_GEP_INDEX:
          fprintf(out, " %%%d, %%%d", in.a, in.b);
          break;
        case IR_STR_VALUE: case IR_STR_DATA: case IR_VTABLE:
          fprintf(out, " %lld", (long long)in.imm);
          break;
        case IR_FUNCADDR:
          fprintf(out, " %s", in.callee.c_str());
          break;
        case IR_OVF:
          fprintf(out, " %s %%%d, %%%d", op_name((IrOp)in.imm), in.a, in.b);
          break;
        case IR_CALL:
          if (in.callee.empty()) fprintf(out, " %%%d(", in.a);
          else fprintf(out, " %s(", in.callee.c_str());
          for (size_t i = 0; i < in.args.size(); i++)
            fprintf(out, "%s%%%d", i ? ", " : "", in.args[i]);
          fputc(')', out);
          break;
        case IR_BR:
          fprintf(out, " bb%d", in.target);
          break;
        case IR_CONDBR:
          fprintf(out, " %%%d, bb%d, bb%d", in.a, in.target, in.target2);
          break;
        case IR_RET:
          if (in.a >= 0) fprintf(out, " %%%d", in.a);
          break;
        case IR_UNREACHABLE:
          break;
        default:
          fprintf(out, " %%%d", in.a);
          if (in.b >= 0) fprintf(out, ", %%%d", in.b);
          break;
        }
        fputc('\n', out);
      }
    }
    fputs("}\n\n", out);
  }
}
