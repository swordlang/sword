#pragma once

#include "types.h"

#include <cstdio>
#include <deque>
#include <string>
#include <vector>

enum IrOp {
  IR_CONST,
  IR_ALLOCA,
  IR_LOAD,
  IR_STORE,
  IR_MEMCPY, // a = dst, b = src, imm = bytes
  IR_ZERO,   // a = dst, imm = bytes

  IR_ADD, IR_SUB, IR_MUL, IR_DIV, IR_MOD,
  IR_AND, IR_OR, IR_XOR, IR_SHL, IR_SHR,
  IR_EQ, IR_NE, IR_LT, IR_LE, IR_GT, IR_GE,
  IR_NEG, IR_NOT, IR_CAST,
  IR_SQRT, // a = an f64, rounded once as IEEE 754 requires
  // imm picks which of these a read-modify-write performs.
  IR_ATOMIC_RMW,   // a = destination, b = value, yields the previous value
  IR_ATOMIC_LOAD,  // a = source
  IR_ATOMIC_STORE, // a = value, b = destination
  IR_ATOMIC_CAS,   // a = destination, b = expected, args[0] = desired
  IR_OVF, // imm = the arithmetic IrOp being tested; yields the overflow bit

  IR_GEP_FIELD, // a = base, imm = field index, type = the struct
  IR_GEP_INDEX, // a = base, b = index, type = element
  IR_STR_VALUE, // imm = string id, yields a ptr to the {ptr, len} constant
  IR_STR_DATA,  // imm = string id, yields a ptr to the bytes
  IR_VTABLE,    // imm = vtable id, yields a ptr to the method table
  IR_FUNCADDR,  // callee = function name, yields its address

  IR_CALL,
  IR_BR,
  IR_CONDBR,
  IR_RET,
  IR_UNREACHABLE,
};

enum RmwKind {
  RMW_ADD, RMW_SUB, RMW_AND, RMW_OR, RMW_XOR,
  RMW_MIN, RMW_MAX, RMW_SWAP, RMW_FADD,
};

struct IrInst {
  IrOp op;
  int dst = -1;   // result value id, -1 when the instruction yields nothing
  int a = -1;     // first operand / condition / stored value
  int b = -1;     // second operand / store destination
  int64_t imm = 0;
  double fimm = 0; // IR_CONST of a floating point type
  Type *type = nullptr; // result type, or the type moved by load/store
  std::string callee; // empty for an indirect call, which uses `a` instead
  std::vector<int> args;
  int target = -1;  // branch destination
  int target2 = -1; // false destination of a conditional branch
};

struct IrBlock {
  int id = 0;
  std::vector<IrInst> insts;
};

struct IrFunc {
  std::string name;
  std::vector<Type *> params;
  Type *ret = nullptr;
  bool is_extern = false;
  // C's `...`: the declaration ends in it, and so must every call, or the
  // extra arguments travel where the callee does not look for them.
  bool is_c_variadic = false;
  bool is_weak = false; // may be missing at run time: its address is then null
  bool is_internal = false; // synthesized: droppable when unused
  // Reached only from the function that made it, so it never leaves its unit.
  bool is_local = false;
  // Aggregate returns go through a hidden pointer parameter, ahead of the
  // declared ones.
  bool ret_by_pointer = false;
  // Which object the function goes into when a build is split by package:
  // the package's prefix, that plus "inst" for the generic instances it owns,
  // or empty for what only the whole program can write.
  std::string unit;
  std::vector<IrBlock> blocks;
  std::vector<Type *> value_type; // indexed by value id
};

struct IrModule {
  // A deque: lowering holds a pointer to the function it is building while
  // task thunks are appended alongside it.
  std::deque<IrFunc> funcs;
  std::vector<std::string> strings;
  std::vector<Type *> structs; // every struct type the backend must name
  std::vector<std::vector<std::string>> vtables;
  // Built for the simulator: every atomic operation is also a point where the
  // simulated scheduler may switch tasks, which is where two tasks' order over
  // an atomic can be explored. Off, the code is exactly what it always was.
  bool sim_points = false;
  // Off in --mode=small: the tables are there for a debugger or a profiler
  // walking the stack, and a program built small has asked to do without.
  bool unwind_tables = true;
  // On in --mode=small. With IR as input, LLVM's size decisions read the
  // function's attribute, not clang's -Oz.
  bool optimize_size = false;

  int intern(const std::string &text) {
    for (size_t i = 0; i < strings.size(); i++)
      if (strings[i] == text) return (int)i;
    strings.push_back(text);
    return (int)strings.size() - 1;
  }
};

void ir_print(const IrModule &mod, FILE *out);
