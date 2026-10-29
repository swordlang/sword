#include "lower.h"

#include <functional>
#include <map>
#include <set>

namespace {

// Two rules shape this pass:
//
//  * Locals become allocas with loads and stores rather than SSA values with
//    phi nodes. LLVM's mem2reg promotes them, which removes the trickiest part
//    of a compiler in exchange for needing -O1 or above.
//  * Aggregates live in memory. An expression of aggregate type evaluates to
//    the address of its data, and assigning one copies bytes.
struct Lowerer {
  TypeTable &types;
  IrModule &mod;
  Mode mode;
  IrFunc *fn = nullptr;
  int cur = 0;
  int sret = -1;   // hidden out pointer when the function returns an aggregate
  Type *ret = nullptr;
  std::vector<IrInst> entry_allocas;

  struct Deferred {
    Node *stmt;
    bool on_error_only;
    // A `lock` block's release rather than a user statement: the slot holds the
    // address of the guard, so every way out of the block unlocks it.
    int unlock_guard = -1;
    bool unlock_read = false;
  };
  // One list per open block. Leaving a block runs its list backwards; leaving
  // through an error runs the errdefer entries as well.
  std::vector<std::vector<Deferred>> scopes;

  struct Loop {
    int continue_bb;
    int break_bb;
    size_t depth; // how far to unwind defers when jumping out
  };
  std::vector<Loop> loops;

  // Open `scope` blobs, innermost last: a spawn joins the one it sits in.
  std::vector<int> scope_blobs;

  // Task bodies, emitted once every ordinary function is done so that
  // appending to the module cannot disturb the one being lowered.
  struct Thunk {
    std::string name;
    std::string callee;
    Type *args;
    Type *box; // the callee's !void type, or null when it cannot fail
    std::string unit;
  };
  std::vector<Thunk> thunks;

  // Every `extern blocking func` called somewhere. Each gets a wrapper with its
  // own signature, which callers reach instead of the C symbol, and a job the
  // io pool runs.
  std::vector<Node *> blocking;
  std::set<std::string> blocking_names;

  // The body of a `parallel for` runs on other workers, so the locals it
  // touches are reached through a block of pointers the loop hands over.
  struct Chunk {
    std::string name;
    Node *loop;
    Type *env;
    std::vector<Symbol *> captured;
    std::string unit;
  };
  std::vector<Chunk> chunks;
  // The loops being lowered that walk `0..xs.len` over a slice that cannot
  // change, as (loop variable, slice): `xs[i]` inside one needs no check.
  std::vector<std::pair<Symbol *, Symbol *>> walked;
  // The package being lowered. Thunks are numbered within it, so a package's
  // code reads the same whatever else the program holds, which is what lets
  // its object be reused by a split build.
  std::string unit;
  std::map<std::string, int> task_serial;

  std::string next_task_id() {
    return unit + std::to_string(task_serial[unit]++);
  }
  // Whether anything hashed a string, which is the only thing that needs the
  // loop written out.
  bool wants_hash_bytes = false;

  // Maps an enum onto the function that names its members, which is how a
  // boxed enum comes out as a name rather than a number.
  const std::unordered_map<Type *, Symbol *> *enum_names = nullptr;

  Lowerer(TypeTable &t, IrModule &m, Mode mo) : types(t), mod(m), mode(mo) {}

  int new_value(Type *type) {
    fn->value_type.push_back(type);
    return (int)fn->value_type.size() - 1;
  }

  int new_block() {
    IrBlock bb;
    bb.id = (int)fn->blocks.size();
    fn->blocks.push_back(bb);
    return bb.id;
  }

  bool terminated() const {
    const auto &insts = fn->blocks[cur].insts;
    if (insts.empty()) return false;
    switch (insts.back().op) {
    case IR_BR: case IR_CONDBR: case IR_RET: case IR_UNREACHABLE:
      return true;
    default:
      return false;
    }
  }

  void emit(IrInst in) {
    if (terminated()) return; // code after a terminator is unreachable
    fn->blocks[cur].insts.push_back(std::move(in));
  }

  void branch(int target) {
    IrInst in{};
    in.op = IR_BR;
    in.target = target;
    emit(in);
  }

  void cond_branch(int cond, int on_true, int on_false) {
    IrInst in{};
    in.op = IR_CONDBR;
    in.a = cond;
    in.target = on_true;
    in.target2 = on_false;
    emit(in);
  }

  // mem2reg only promotes allocas in the entry block, so they are collected
  // here and spliced in once the body is done.
  int alloca_slot(Type *type) {
    IrInst in{};
    in.op = IR_ALLOCA;
    in.dst = new_value(types.ptr(type));
    in.type = type;
    entry_allocas.push_back(in);
    return in.dst;
  }

  void store(int value, int slot, Type *type) {
    IrInst in{};
    in.op = IR_STORE;
    in.a = value;
    in.b = slot;
    in.type = type;
    emit(in);
  }

  int load(int slot, Type *type) {
    IrInst in{};
    in.op = IR_LOAD;
    in.dst = new_value(type);
    in.a = slot;
    in.type = type;
    emit(in);
    return in.dst;
  }

  void copy(int dst, int src, Type *type) {
    IrInst in{};
    in.op = IR_MEMCPY;
    in.a = dst;
    in.b = src;
    in.imm = size_of(type);
    in.type = type;
    emit(in);
  }

  int float_constant(double value, Type *type) {
    IrInst in{};
    in.op = IR_CONST;
    in.dst = new_value(type);
    in.fimm = value;
    in.type = type;
    emit(in);
    return in.dst;
  }

  int constant(int64_t value, Type *type) {
    IrInst in{};
    in.op = IR_CONST;
    in.dst = new_value(type);
    in.imm = value;
    in.type = type;
    emit(in);
    return in.dst;
  }

  // Two strings are equal when their lengths match and so do their bytes. The
  // length test comes first and short-circuits, so an empty string never calls
  // memcmp at all.
  int string_equal(int a, int b) {
    Type *word = types.usize_ty;
    Type *flag = types.bool_ty;
    int result = alloca_slot(flag);
    store(constant(0, flag), result, flag);

    int la = load(gep_slice_len(a), word);
    int lb = load(gep_slice_len(b), word);
    int bytes_bb = new_block();
    int done_bb = new_block();
    cond_branch(binop(IR_EQ, la, lb, flag), bytes_bb, done_bb);

    cur = bytes_bb;
    Type *opaque = types.rawptr(types.u8_ty);
    IrInst call{};
    call.op = IR_CALL;
    call.callee = "memcmp";
    call.type = types.named("i32");
    call.dst = new_value(call.type);
    call.args = {load(gep_slice_ptr(a), opaque), load(gep_slice_ptr(b), opaque),
                 la};
    emit(call);
    store(binop(IR_EQ, call.dst, constant(0, call.type), flag), result, flag);
    branch(done_bb);

    cur = done_bb;
    return load(result, flag);
  }

  int binop(IrOp op, int a, int b, Type *type) {
    IrInst in{};
    in.op = op;
    in.dst = new_value(type);
    in.a = a;
    in.b = b;
    in.type = type;
    emit(in);
    return in.dst;
  }

  int gep_field(int base, Type *type, int index) {
    IrInst in{};
    in.op = IR_GEP_FIELD;
    in.dst = new_value(types.ptr(type->fields[index].type));
    in.a = base;
    in.imm = index;
    in.type = type;
    emit(in);
    return in.dst;
  }

  int gep_named(int base, Type *type, const char *name) {
    return gep_field(base, type, find_field(type, name)->index);
  }

  int gep_index(int base, int index, Type *elem) {
    IrInst in{};
    in.op = IR_GEP_INDEX;
    in.dst = new_value(types.ptr(elem));
    in.a = base;
    in.b = index;
    in.type = elem;
    emit(in);
    return in.dst;
  }

  static IrOp ir_binop(TokKind op) {
    switch (op) {
    case TK_PLUS: case TK_PLUS_WRAP: return IR_ADD;
    case TK_MINUS: case TK_MINUS_WRAP: return IR_SUB;
    case TK_STAR: case TK_STAR_WRAP: return IR_MUL;
    case TK_SLASH: return IR_DIV;
    case TK_PERCENT: return IR_MOD;
    case TK_AMP: return IR_AND;
    case TK_PIPE: return IR_OR;
    case TK_CARET: return IR_XOR;
    case TK_SHL: return IR_SHL;
    case TK_SHR: return IR_SHR;
    case TK_EQ: return IR_EQ;
    case TK_NE: return IR_NE;
    case TK_LT: return IR_LT;
    case TK_LE: return IR_LE;
    case TK_GT: return IR_GT;
    case TK_GE: return IR_GE;
    default: return IR_ADD;
    }
  }

  static TokKind assign_arith(TokKind op) {
    switch (op) {
    case TK_ADD_ASSIGN: return TK_PLUS;
    case TK_SUB_ASSIGN: return TK_MINUS;
    case TK_MUL_ASSIGN: return TK_STAR;
    case TK_DIV_ASSIGN: return TK_SLASH;
    case TK_MOD_ASSIGN: return TK_PERCENT;
    case TK_ADD_WRAP_ASSIGN: return TK_PLUS_WRAP;
    case TK_SUB_WRAP_ASSIGN: return TK_MINUS_WRAP;
    case TK_AND_ASSIGN: return TK_AMP;
    case TK_OR_ASSIGN: return TK_PIPE;
    case TK_XOR_ASSIGN: return TK_CARET;
    case TK_SHL_ASSIGN: return TK_SHL;
    case TK_SHR_ASSIGN: return TK_SHR;
    default: return TK_STAR_WRAP;
    }
  }

  static bool is_slice_shaped(const Type *t) {
    return t->kind == TY_SLICE || t->kind == TY_STRING;
  }

  static bool is_address(const Type *t) {
    return t->kind == TY_PTR || t->kind == TY_RAWPTR || t->kind == TY_FUNC ||
           (t->kind == TY_OPT && t->elem->kind != TY_VOID);
  }

  static bool wraps(TokKind op) {
    return op == TK_PLUS_WRAP || op == TK_MINUS_WRAP || op == TK_STAR_WRAP;
  }

  // --- tasks --------------------------------------------------------------

  // Must match SWORD_SCOPE_SIZE in rt/sword_rt.h. Counted in words rather
  // than bytes so the slot is word-aligned: the runtime builds atomics in it,
  // and an unaligned atomic is a bus error on ARM.
  static const int64_t kScopeWords = 8;

  // Must match SWORD_CHUNKS in rt/sword_rt.h. It decides how many partial
  // answers a reduction can have, and the room for them is reserved here: too
  // few and the runtime writes past the end of the frame's slot.
  static const int64_t kChunks = 64;

  // How much frame the partial answers may take. An accumulator of the
  // program's own can be hundreds of bytes, and sixty-four of those would be
  // a surprising thing to find on a stack.
  static const int64_t kPartialBytes = 32 * 1024;

  int call_runtime(const char *name, std::vector<int> args, Type *ret) {
    IrInst in{};
    in.op = IR_CALL;
    in.callee = name;
    in.type = ret;
    in.args = std::move(args);
    if (ret->kind != TY_VOID) in.dst = new_value(ret);
    emit(in);
    return in.dst;
  }

  int func_address(const std::string &name) {
    IrInst in{};
    in.op = IR_FUNCADDR;
    in.dst = new_value(types.rawptr(types.u8_ty));
    in.callee = name;
    in.type = types.rawptr(types.u8_ty);
    emit(in);
    return in.dst;
  }

  // The guard is taken on the way in and released on every way out, which the
  // defer list already knows how to arrange.
  void lock_stmt(Node *n) {
    Type *box = n->lhs->type;
    if (box->kind == TY_PTR) box = box->elem;

    int base = expr(n->lhs);
    Type *opaque = types.rawptr(types.u8_ty);
    int holder = alloca_slot(opaque);
    store(gep_named(base, box, "guard"), holder, opaque);

    // `if lock` only goes in when the guard was free; the other branch runs
    // without it.
    int else_bb = -1, join = -1;
    if (n->is_try) {
      const char *attempt = n->is_read ? "sword_mutex_try_lock_read"
                                       : "sword_mutex_try_lock";
      int got = call_runtime(attempt, {load(holder, opaque)}, types.bool_ty);
      int then_bb = new_block();
      else_bb = n->els ? new_block() : -1;
      join = new_block();
      cond_branch(got, then_bb, else_bb >= 0 ? else_bb : join);
      cur = then_bb;
    } else {
      call_runtime(n->is_read ? "sword_mutex_lock_read" : "sword_mutex_lock",
                   {load(holder, opaque)}, types.void_ty);
    }

    n->sym->slot = alloca_slot(n->sym->type);
    store(gep_named(base, box, "value"), n->sym->slot, n->sym->type);

    scopes.emplace_back();
    scopes.back().push_back({nullptr, false, holder, n->is_read});
    stmt(n->body);
    emit_defers(scopes.size() - 1, false);
    scopes.pop_back();

    if (!n->is_try) return;
    branch(join);
    if (else_bb >= 0) {
      cur = else_bb;
      stmt(n->els);
      branch(join);
    }
    cur = join;
  }

  void scope_stmt(Node *n) {
    int blob = alloca_slot(types.array(types.usize_ty, kScopeWords));
    call_runtime("sword_scope_begin", {blob}, types.void_ty);

    scope_blobs.push_back(blob);
    stmt(n->body);
    scope_blobs.pop_back();

    // Leaving the block is the join, and the checker guarantees this is the
    // only way out.
    int code = call_runtime("sword_scope_end", {blob}, types.error_ty);
    if (n->form != 1) return;

    int ok = binop(IR_EQ, code, constant(0, types.error_ty), types.bool_ty);
    int fail_bb = new_block();
    int next_bb = new_block();
    cond_branch(ok, next_bb, fail_bb);

    cur = fail_bb;
    finish_with_error(code);
    cur = next_bb;
  }

  void spawn_stmt(Node *n) {
    Node *call = n->lhs;
    Type *sig = call->sym->type;
    std::string id = next_task_id();

    // The arguments are copied into one block the runtime owns, so the task
    // does not depend on the caller's frame staying put.
    Type *args = types.declare_struct("sword.args." + id);
    args->is_extern = true; // declaration order, so the thunk agrees
    std::vector<Field> fields;
    for (size_t i = 0; i < call->kids.size(); i++)
      fields.push_back(Field{"a" + std::to_string(i), sig->params[i], 0, 0});
    types.layout_struct(args, std::move(fields));
    mod.structs.push_back(args);

    int slot = alloca_slot(args);
    for (size_t i = 0; i < call->kids.size(); i++)
      assign_into(gep_field(slot, args, (int)i), call->kids[i],
                  sig->params[i]);

    std::string thunk = "sword.task." + id;
    thunks.push_back(
        {thunk, call->name,
         args, sig->ret->is_error_union ? sig->ret : nullptr, unit});

    call_runtime("sword_scope_spawn",
                 {scope_blobs.back(), func_address(thunk), slot,
                  constant(size_of(args), types.usize_ty)},
                 types.void_ty);
  }

  void emit_thunk(const Thunk &t, IrFunc &out) {
    fn = &out;
    cur = 0;
    sret = -1;
    ret = types.error_ty;
    entry_allocas.clear();
    loops.clear();
    scopes.clear();
    scope_blobs.clear();

    out.name = t.name;
    out.unit = t.unit;
    out.is_local = true;
    out.ret = types.error_ty;
    out.params.push_back(types.rawptr(types.u8_ty));

    int argp = new_value(types.rawptr(types.u8_ty));
    new_block();

    std::vector<int> call_args;
    int box = -1;
    if (t.box) {
      box = alloca_slot(t.box);
      call_args.push_back(box);
    }
    for (size_t i = 0; i < t.args->fields.size(); i++) {
      Type *field = t.args->fields[i].type;
      int at = gep_field(argp, t.args, (int)i);
      call_args.push_back(is_aggregate(field) ? at : load(at, field));
    }

    IrInst in{};
    in.op = IR_CALL;
    in.callee = t.callee;
    in.type = types.void_ty;
    in.args = std::move(call_args);
    emit(in);

    int code = t.box ? load(gep_named(box, t.box, "code"), types.error_ty)
                     : constant(0, types.error_ty);
    emit_ret(code, types.error_ty);

    auto &entry = out.blocks[0].insts;
    entry.insert(entry.begin(), entry_allocas.begin(), entry_allocas.end());
  }

  // --- blocking calls -----------------------------------------------------

  // The arguments, the answer and errno, in one block on the caller's stack,
  // which stays put while the task is down.
  Type *blocking_block(Node *decl) {
    std::string name = "sword.offload." + decl->name;
    if (Type *known = types.named(name)) return known;
    Type *block = types.declare_struct(name);
    block->is_extern = true;
    Type *sig = decl->sym->type;
    std::vector<Field> fields;
    for (size_t i = 0; i < sig->params.size(); i++)
      fields.push_back(Field{"a" + std::to_string(i), sig->params[i], 0, 0});
    fields.push_back(Field{"errno", types.named("i32"), 0, 0});
    if (sig->ret->kind != TY_VOID) fields.push_back(Field{"ret", sig->ret, 0, 0});
    types.layout_struct(block, std::move(fields));
    mod.structs.push_back(block);
    return block;
  }

  void start(IrFunc &out) {
    fn = &out;
    cur = 0;
    sret = -1;
    entry_allocas.clear();
    loops.clear();
    scopes.clear();
    scope_blobs.clear();
  }

  void finish(IrFunc &out) {
    auto &entry = out.blocks[0].insts;
    entry.insert(entry.begin(), entry_allocas.begin(), entry_allocas.end());
  }

  // What callers call: the same signature as the C function, handing the call
  // to the io pool and the task down until it is done. errno travels back with
  // the answer, so os.Errno right after the call means what it says.
  void emit_blocking_wrapper(Node *decl, IrFunc &out) {
    start(out);
    Type *sig = decl->sym->type;
    Type *block = blocking_block(decl);
    out.name = decl->name + ".blocking";
    out.ret = ret = sig->ret;
    out.params = sig->params;
    out.is_internal = true;
    std::vector<int> params;
    for (Type *p : sig->params) params.push_back(new_value(p));
    new_block();

    int slot = alloca_slot(block);
    for (size_t i = 0; i < params.size(); i++)
      store(params[i], gep_field(slot, block, (int)i), sig->params[i]);
    call_runtime("sword_offload", {func_address(decl->name + ".job"), slot},
                 types.named("i64"));
    int errno_at = (int)sig->params.size();
    call_runtime("sword_set_errno",
                 {load(gep_field(slot, block, errno_at), types.named("i32"))},
                 types.void_ty);
    if (sig->ret->kind == TY_VOID) emit_ret(-1, nullptr);
    else emit_ret(load(gep_field(slot, block, errno_at + 1), sig->ret), sig->ret);
    finish(out);
  }

  // What the io thread runs: the C call itself. errno is cleared first, so a
  // function that fails only through errno can be told from one that did not.
  void emit_blocking_job(Node *decl, IrFunc &out) {
    start(out);
    Type *sig = decl->sym->type;
    Type *block = blocking_block(decl);
    out.name = decl->name + ".job";
    out.ret = ret = types.named("i64");
    out.params.push_back(types.rawptr(types.u8_ty));
    out.is_internal = true;
    int argp = new_value(types.rawptr(types.u8_ty));
    new_block();

    call_runtime("sword_set_errno", {constant(0, types.named("i32"))}, types.void_ty);
    IrInst in{};
    in.op = IR_CALL;
    in.callee = decl->name;
    in.type = sig->ret;
    for (size_t i = 0; i < sig->params.size(); i++)
      in.args.push_back(load(gep_field(argp, block, (int)i), sig->params[i]));
    if (sig->ret->kind != TY_VOID) in.dst = new_value(sig->ret);
    emit(in);
    int errno_at = (int)sig->params.size();
    store(call_runtime("sword_errno", {}, types.named("i32")),
          gep_field(argp, block, errno_at), types.named("i32"));
    if (in.dst >= 0)
      store(in.dst, gep_field(argp, block, errno_at + 1), sig->ret);
    emit_ret(constant(0, types.named("i64")), types.named("i64"));
    finish(out);
  }

  // --- captures -----------------------------------------------------------

  void walk(Node *n, const std::function<void(Node *)> &visit) {
    if (!n) return;
    visit(n);
    for (Node *kid : n->kids) walk(kid, visit);
    walk(n->lhs, visit);
    walk(n->rhs, visit);
    walk(n->cond, visit);
    walk(n->body, visit);
    walk(n->els, visit);
  }

  // Anything declared inside the body belongs to the worker running it; the
  // rest has to come from the parent's frame.
  void captures_of(Node *loop, std::vector<Symbol *> &out) {
    std::set<Symbol *> local;
    local.insert(loop->sym);
    walk(loop->body, [&](Node *n) {
      if (n->kind == ND_VAR || n->kind == ND_CATCH ||
          (n->kind == ND_FOR && n->is_range) ||
          (n->kind == ND_IF && !n->name.empty()))
        if (n->sym) local.insert(n->sym);
    });

    std::set<Symbol *> seen;
    walk(loop->body, [&](Node *n) {
      if (n->kind != ND_IDENT || !n->sym) return;
      if (n->sym->is_func || local.count(n->sym)) return;
      if (seen.insert(n->sym).second) out.push_back(n->sym);
    });

    if (loop->reduce_sym && !seen.count(loop->reduce_sym))
      out.push_back(loop->reduce_sym);
  }

  void parallel_for(Node *n) {
    std::string id = next_task_id();
    std::vector<Symbol *> captured;
    captures_of(n, captured);

    Type *reduced = n->reduce_sym ? n->reduce_sym->type : nullptr;
    Type *env = types.declare_struct("sword.env." + id);
    env->is_extern = true;
    std::vector<Field> fields;
    for (size_t i = 0; i < captured.size(); i++)
      fields.push_back(
          Field{"c" + std::to_string(i), types.ptr(captured[i]->type), 0, 0});
    // A reduction keeps one partial answer per piece of the range, and this is
    // where the pieces are told to put them.
    if (reduced)
      fields.push_back(Field{"partials", types.ptr(reduced), 0, 0});
    types.layout_struct(env, std::move(fields));
    mod.structs.push_back(env);

    int block = alloca_slot(env);
    for (size_t i = 0; i < captured.size(); i++)
      store(captured[i]->slot, gep_field(block, env, (int)i),
            types.ptr(captured[i]->type));

    // One slot per piece, on this frame: the range is cut into at most this
    // many however long it is, so this needs no allocator. An accumulator of
    // its own can be large, so what is capped is the room they take rather
    // than only how many there are — and both numbers are known here, so the
    // cut still does not depend on anything the pool does.
    int64_t most = kChunks;
    int partials = -1;
    int pieces = -1;
    if (reduced) {
      int64_t each = size_of(reduced);
      if (each > 0 && each * most > kPartialBytes) most = kPartialBytes / each;
      if (most < 1) most = 1;
      partials = alloca_slot(types.array(reduced, most));
      store(partials, gep_field(block, env, (int)captured.size()),
            types.ptr(reduced));
      pieces = alloca_slot(types.usize_ty);
      store(constant(0, types.usize_ty), pieces, types.usize_ty);
    }

    std::string name = "sword.chunk." + id;
    chunks.push_back({name, n, env, captured, unit});

    int lo = to_word(expr(n->lhs));
    int hi = to_word(expr(n->rhs));
    int out = reduced ? pieces : constant(0, types.rawptr(types.u8_ty));
    call_runtime("sword_parallel_for",
                 {lo, hi, func_address(name), block, out,
                  constant(most, types.usize_ty)},
                 types.error_ty);
    if (reduced) combine_partials(n, partials, pieces, reduced);
  }

  // The partial answers are combined here, in the order the pieces were cut,
  // rather than folded in as each one finishes. For integers the two agree;
  // for floating point they do not, and the old way made the answer depend on
  // how many threads happened to be running.
  void combine_partials(Node *n, int partials, int pieces, Type *type) {
    Type *word = types.usize_ty;
    int slot = n->reduce_sym->slot;
    int at = alloca_slot(word);
    store(constant(0, word), at, word);

    int head = new_block();
    int body = new_block();
    int done = new_block();
    branch(head);

    cur = head;
    cond_branch(binop(IR_LT, load(at, word), load(pieces, word),
                      types.bool_ty),
                body, done);

    cur = body;
    int where = gep_index(partials, load(at, word), type);
    int next = body;
    // A reduction of the program's own: `acc = f(acc, piece)`, called here
    // rather than folded with an instruction, and in the same order.
    if (n->reduce_kind == kReduceUser) {
      IrInst in{};
      in.op = IR_CALL;
      in.callee = n->reduce_fn->name;
      if (is_aggregate(type)) {
        // The answer comes back through a pointer, and it cannot be the
        // accumulator itself: the callee reads both of its arguments.
        int made = alloca_slot(type);
        in.args.push_back(made);
        in.args.push_back(slot);
        in.args.push_back(where);
        in.type = types.void_ty;
        emit(in);
        copy(slot, made, type);
      } else {
        in.args.push_back(load(slot, type));
        in.args.push_back(load(where, type));
        in.type = type;
        in.dst = new_value(type);
        emit(in);
        store(in.dst, slot, type);
      }
      cur = next;
      store(binop(IR_ADD, load(at, word), constant(1, word), word), at, word);
      branch(head);
      cur = done;
      return;
    }
    int part = load(where, type);
    int acc = load(slot, type);
    switch (n->reduce_kind) {
    case RMW_MIN:
    case RMW_MAX: {
      int better = binop(n->reduce_kind == RMW_MIN ? IR_LT : IR_GT, part, acc,
                         types.bool_ty);
      int take = new_block();
      int skip = new_block();
      cond_branch(better, take, skip);
      cur = take;
      store(part, slot, type);
      branch(skip);
      cur = skip;
      next = skip;
      break;
    }
    case RMW_AND: store(binop(IR_AND, acc, part, type), slot, type); break;
    case RMW_OR: store(binop(IR_OR, acc, part, type), slot, type); break;
    default: store(binop(IR_ADD, acc, part, type), slot, type); break;
    }

    cur = next;
    store(binop(IR_ADD, load(at, word), constant(1, word), word), at, word);
    branch(head);

    cur = done;
  }

  void emit_chunk(const Chunk &c, IrFunc &out) {
    Node *loop = c.loop;
    fn = &out;
    cur = 0;
    sret = -1;
    ret = types.error_ty;
    entry_allocas.clear();
    loops.clear();
    scopes.clear();
    scope_blobs.clear();

    out.name = c.name;
    out.unit = c.unit;
    out.is_local = true;
    out.ret = types.error_ty;
    out.params = {types.rawptr(types.u8_ty), types.usize_ty, types.usize_ty,
                  types.usize_ty};

    int envp = new_value(types.rawptr(types.u8_ty));
    int lo = new_value(types.usize_ty);
    int hi = new_value(types.usize_ty);
    int piece = new_value(types.usize_ty); // which cut of the range this is
    new_block();
    scopes.emplace_back();

    std::vector<int> saved;
    for (size_t i = 0; i < c.captured.size(); i++) {
      Symbol *sym = c.captured[i];
      saved.push_back(sym->slot);
      sym->slot = load(gep_field(envp, c.env, (int)i), types.ptr(sym->type));
    }

    // Each worker sums into its own copy and folds it in once at the end, so
    // there is no contention per iteration.
    int shared = -1;
    int accumulator = -1;
    if (loop->reduce_sym) {
      Type *type = loop->reduce_sym->type;
      shared = loop->reduce_sym->slot;
      accumulator = alloca_slot(type);
      if (loop->reduce_kind == kReduceUser) {
        // The identity is the zero value, which is what a fresh accumulator of
        // anything means: nothing added yet.
        zero(accumulator, type);
      } else {
        int identity = is_float(type) ? float_constant(loop->fval, type)
                                      : constant((int64_t)loop->ival, type);
        store(identity, accumulator, type);
      }
      loop->reduce_sym->slot = accumulator;
    }

    Type *index = loop->type;
    int slot = alloca_slot(index);
    int outer = loop->sym->slot;
    loop->sym->slot = slot;
    store(narrow(lo, index), slot, index);
    int limit = narrow(hi, index);

    int cond_bb = new_block();
    int body_bb = new_block();
    int step_bb = new_block();
    int exit_bb = new_block();

    branch(cond_bb);
    cur = cond_bb;
    cond_branch(binop(IR_LT, load(slot, index), limit, types.bool_ty), body_bb,
                exit_bb);

    // Each piece is a part of the range the parent worked out from `xs.len`
    // before cutting it, so the same proof holds in every one.
    Symbol *slice = walked_slice(loop, true);
    if (slice) walked.push_back({loop->sym, slice});
    loops.push_back({step_bb, exit_bb, scopes.size()});
    cur = body_bb;
    stmt(loop->body);
    branch(step_bb);
    loops.pop_back();
    if (slice) walked.pop_back();

    cur = step_bb;
    store(binop(IR_ADD, load(slot, index), constant(1, index), index), slot,
          index);
    branch(cond_bb);

    cur = exit_bb;
    if (accumulator >= 0) {
      Type *type = loop->reduce_sym->type;
      // The answer for this piece of the range goes in the slot that belongs
      // to it, and the caller combines them in order once every piece is done.
      // Folding it in here with an atomic would have the order be whichever
      // piece finished first, which for floating point is a different answer.
      int base = load(gep_field(envp, c.env, (int)c.captured.size()),
                      types.ptr(type));
      int slot_for = gep_index(base, piece, type);
      if (is_aggregate(type)) copy(slot_for, accumulator, type);
      else store(load(accumulator, type), slot_for, type);
      loop->reduce_sym->slot = shared;
    }
    emit_ret(constant(0, types.error_ty), types.error_ty);

    loop->sym->slot = outer;
    for (size_t i = 0; i < c.captured.size(); i++)
      c.captured[i]->slot = saved[i];

    auto &entry = out.blocks[0].insts;
    entry.insert(entry.begin(), entry_allocas.begin(), entry_allocas.end());
  }

  // --- deferred statements ------------------------------------------------

  // Runs the deferred statements of every scope from the innermost down to
  // `down_to`, innermost first and in reverse declaration order.
  void emit_defers(size_t down_to, bool error_path) {
    for (size_t i = scopes.size(); i > down_to; i--) {
      const auto &list = scopes[i - 1];
      for (size_t j = list.size(); j > 0; j--) {
        if (terminated()) return;
        const Deferred &entry = list[j - 1];
        if (entry.unlock_guard >= 0) {
          int guard = load(entry.unlock_guard, types.rawptr(types.u8_ty));
          call_runtime(entry.unlock_read ? "sword_mutex_unlock_read"
                                         : "sword_mutex_unlock",
                       {guard}, types.void_ty);
        } else if (!entry.on_error_only || error_path) {
          stmt(entry.stmt);
        }
      }
    }
  }

  // --- safety checks -----------------------------------------------------

  // Splits the current block: when `ok` is false, report and abort.
  void guard(int ok, const std::string &message) {
    int fail_bb = new_block();
    int next_bb = new_block();
    cond_branch(ok, next_bb, fail_bb);

    cur = fail_bb;
    int id = mod.intern(message);
    IrInst data{};
    data.op = IR_STR_DATA;
    data.dst = new_value(types.rawptr(types.u8_ty));
    data.imm = id;
    emit(data);

    IrInst call{};
    call.op = IR_CALL;
    call.callee = "sword_panic";
    call.type = types.void_ty;
    call.args = {data.dst, constant((int64_t)message.size(), types.usize_ty)};
    emit(call);

    IrInst stop{};
    stop.op = IR_UNREACHABLE;
    emit(stop);

    cur = next_bb;
  }

  int checked_arith(IrOp op, int a, int b, Type *type, Pos pos) {
    int value = binop(op, a, b, type);
    if (!mode_checks(mode)) return value;
    if (op != IR_ADD && op != IR_SUB && op != IR_MUL) return value;

    IrInst test{};
    test.op = IR_OVF;
    test.dst = new_value(types.bool_ty);
    test.a = a;
    test.b = b;
    test.imm = op;
    test.type = type;
    emit(test);

    IrInst ok{};
    ok.op = IR_NOT;
    ok.dst = new_value(types.bool_ty);
    ok.a = test.dst;
    ok.type = types.bool_ty;
    emit(ok);

    guard(ok.dst, "panic: integer overflow at line " +
                      std::to_string(pos.line) + "\n");
    return value;
  }

  // Indices and slice bounds must reach the width a slice length uses before
  // they can be compared against it or fed to a GEP.
  // The runtime carries loop bounds as words; the induction variable may be
  // narrower.
  int narrow(int value, Type *type) {
    if (fn->value_type[value]->bits == type->bits) return value;
    IrInst in{};
    in.op = IR_CAST;
    in.dst = new_value(type);
    in.a = value;
    in.type = type;
    emit(in);
    return in.dst;
  }

  int to_word(int value) {
    Type *type = fn->value_type[value];
    if (type->bits == 64) return value;
    IrInst in{};
    in.op = IR_CAST;
    in.dst = new_value(types.usize_ty);
    in.a = value;
    in.type = types.usize_ty;
    emit(in);
    return in.dst;
  }

  // The slice `for i in a..xs.len` walks, when every `xs[i]` in its body is in
  // range by construction. The loop variable cannot be assigned, and nothing
  // can change the header of an immutable binding: assigning it is refused and
  // `&xs` lends it read-only. In a `parallel for` a `mut` slice holds too: the
  // race checker lets the body write only `xs[i]`, never `xs` itself, and no
  // other task of the scope can reach it while the loop runs. A slice reached
  // through a pointer or a field is never one of these, because the same memory
  // can be lent `mut` under another name in the same call and shortened mid-loop.
  Symbol *walked_slice(Node *loop, bool parallel) {
    Node *hi = loop->rhs;
    if (!hi || hi->kind != ND_FIELD || hi->name != "len") return nullptr;
    Node *base = hi->lhs;
    if (!base || base->kind != ND_IDENT || !base->sym) return nullptr;
    Symbol *s = base->sym;
    if ((s->is_mut && !parallel) || s->is_func || !s->type) return nullptr;
    if (s->type->kind != TY_SLICE && s->type->kind != TY_STRING) return nullptr;
    return s;
  }

  bool proven_in_range(Node *index) {
    Node *base = index->lhs, *at = index->rhs;
    if (base->kind != ND_IDENT || at->kind != ND_IDENT) return false;
    for (const auto &w : walked)
      if (w.first == at->sym && w.second == base->sym) return true;
    return false;
  }

  void bounds_check(int index, int len, Pos pos) {
    if (!mode_checks(mode)) return;
    // Unsigned comparison catches negative indices too: they wrap to huge.
    int ok = binop(IR_LT, index, len, types.bool_ty);
    guard(ok, "panic: index out of bounds at line " +
                  std::to_string(pos.line) + "\n");
  }

  // --- places ------------------------------------------------------------

  // Address of the element data behind an indexable value.
  int data_pointer(Node *base, int *len_out) {
    Type *type = base->type;
    int value = expr(base);
    if (type->kind == TY_PTR) {
      type = type->elem;
      // `value` already is the address of the pointee.
    }

    switch (type->kind) {
    case TY_ARRAY:
      if (len_out) *len_out = constant(type->count, types.usize_ty);
      return value;
    case TY_SLICE: case TY_STRING: {
      if (len_out)
        *len_out = load(gep_slice_len(value), types.usize_ty);
      return load(gep_slice_ptr(value), types.rawptr(types.u8_ty));
    }
    case TY_RAWPTR:
      if (len_out) *len_out = -1;
      return value;
    default:
      if (len_out) *len_out = -1;
      return value;
    }
  }

  // A slice is { ptr, len }; both fields are reached the same way.
  int gep_slice_ptr(int base) {
    IrInst in{};
    in.op = IR_GEP_FIELD;
    in.dst = new_value(types.ptr(types.rawptr(types.u8_ty)));
    in.a = base;
    in.imm = 0;
    in.type = types.string_ty; // marks the { ptr, len } shape for the backend
    emit(in);
    return in.dst;
  }

  int gep_slice_len(int base) {
    IrInst in{};
    in.op = IR_GEP_FIELD;
    in.dst = new_value(types.ptr(types.usize_ty));
    in.a = base;
    in.imm = 1;
    in.type = types.string_ty;
    emit(in);
    return in.dst;
  }

  int addr(Node *n) {
    switch (n->kind) {
    case ND_IDENT:
      return n->sym->slot;

    case ND_UNARY: // *p
      return expr(n->lhs);

    case ND_FIELD: {
      Type *base = n->lhs->type;
      if (base->kind == TY_PTR) base = base->elem;
      int value = expr(n->lhs);
      if (base->kind == TY_SLICE || base->kind == TY_STRING)
        return n->name == "len" ? gep_slice_len(value) : gep_slice_ptr(value);
      const Field *field = find_field(base, n->name);
      return gep_field(value, base, field->index);
    }

    case ND_INDEX: {
      int len = -1;
      Type *base = n->lhs->type;
      bool checked = base->kind != TY_RAWPTR;
      int data = data_pointer(n->lhs, checked ? &len : nullptr);
      int index = to_word(expr(n->rhs));
      if (checked && len >= 0 && !proven_in_range(n))
        bounds_check(index, len, n->pos);
      return gep_index(data, index, n->type);
    }

    default:
      return expr(n);
    }
  }

  // --- expressions -------------------------------------------------------

  // `&&` and `||` must not evaluate their right side unless they have to, so
  // they lower to control flow with a slot holding the result.
  int short_circuit(Node *n) {
    int slot = alloca_slot(types.bool_ty);
    int lhs = expr(n->lhs);
    store(lhs, slot, types.bool_ty);

    int rhs_bb = new_block();
    int join = new_block();
    if (n->op == TK_ANDAND) cond_branch(lhs, rhs_bb, join);
    else cond_branch(lhs, join, rhs_bb);

    cur = rhs_bb;
    store(expr(n->rhs), slot, types.bool_ty);
    branch(join);

    cur = join;
    return load(slot, types.bool_ty);
  }

  int string_literal(Node *n) {
    IrInst in{};
    in.op = IR_STR_VALUE;
    in.dst = new_value(types.ptr(types.string_ty));
    in.imm = mod.intern(n->text);
    in.type = types.string_ty;
    emit(in);
    return in.dst;
  }

  int struct_literal(Node *n) {
    int slot = alloca_slot(n->type);
    for (Node *init : n->kids) {
      const Field *field = find_field(n->type, init->name);
      int at = gep_field(slot, n->type, field->index);
      assign_into(at, init->rhs, field->type);
    }
    return slot;
  }

  void zero(int dst, Type *type) {
    IrInst in{};
    in.op = IR_ZERO;
    in.a = dst;
    in.imm = size_of(type);
    in.type = type;
    emit(in);
  }

  int array_literal(Node *n) {
    int slot = alloca_slot(n->type);
    Type *elem = n->type->elem;
    if (n->kids.empty()) {
      zero(slot, n->type);
      return slot;
    }
    for (size_t i = 0; i < n->kids.size(); i++) {
      int at = gep_index(slot, constant((int64_t)i, types.usize_ty), elem);
      assign_into(at, n->kids[i], elem);
    }
    return slot;
  }

  int slice_expr(Node *n) {
    int len = -1;
    int data = data_pointer(n->lhs, &len);
    Type *elem = n->type->kind == TY_STRING ? types.u8_ty : n->type->elem;

    int low = n->cond ? to_word(expr(n->cond)) : constant(0, types.usize_ty);
    int high = n->rhs ? to_word(expr(n->rhs)) : len;

    if (mode_checks(mode) && len >= 0) {
      int ok = binop(IR_LE, low, high, types.bool_ty);
      guard(ok, "panic: slice bounds out of order at line " +
                    std::to_string(n->pos.line) + "\n");
      ok = binop(IR_LE, high, len, types.bool_ty);
      guard(ok, "panic: slice bounds out of range at line " +
                    std::to_string(n->pos.line) + "\n");
    }

    int slot = alloca_slot(n->type);
    store(gep_index(data, low, elem), gep_slice_ptr(slot),
          types.rawptr(types.u8_ty));
    store(binop(IR_SUB, high, low, types.usize_ty), gep_slice_len(slot),
          types.usize_ty);
    return slot;
  }

  // --- optionals ---------------------------------------------------------

  // `?*T` is the pointer itself; anything else carries a flag beside the
  // value. These two helpers hide which one is in play.
  int opt_present(int value, Type *type) {
    if (type->kind == TY_OPT)
      return binop(IR_NE, value, constant(0, type), types.bool_ty);
    return load(gep_named(value, type, "has"), types.bool_ty);
  }

  int opt_value(int value, Type *type) {
    if (type->kind == TY_OPT) return value; // same bits, narrower type
    Type *payload = const_cast<Type *>(opt_payload(type));
    int at = gep_named(value, type, "value");
    return is_aggregate(payload) ? at : load(at, payload);
  }

  int nil_value(Type *type) {
    if (type->kind == TY_OPT) return constant(0, type);
    int slot = alloca_slot(type);
    store(constant(0, types.bool_ty), gep_named(slot, type, "has"),
          types.bool_ty);
    return slot;
  }

  int lower_orelse(Node *n) {
    Type *box_type = n->lhs->type;
    Type *value = n->type;
    int box = expr(n->lhs);
    int has = opt_present(box, box_type);

    if (n->body) {
      int fail_bb = new_block();
      int next_bb = new_block();
      cond_branch(has, next_bb, fail_bb);

      cur = fail_bb;
      stmt(n->body);
      branch(next_bb);

      cur = next_bb;
      if (n->form == 1 || value->kind == TY_VOID) return -1;
      return opt_value(box, box_type);
    }

    int ok_bb = new_block();
    int fail_bb = new_block();
    int join = new_block();
    int slot = value->kind == TY_VOID ? -1 : alloca_slot(value);
    cond_branch(has, ok_bb, fail_bb);

    cur = ok_bb;
    if (slot >= 0) {
      int at = opt_value(box, box_type);
      if (is_aggregate(value)) copy(slot, at, value);
      else store(at, slot, value);
    }
    branch(join);

    cur = fail_bb;
    if (slot >= 0) assign_into(slot, n->rhs, value);
    else expr(n->rhs);
    branch(join);

    cur = join;
    if (slot < 0) return -1;
    return is_aggregate(value) ? slot : load(slot, value);
  }

  // Reads the payload out of an `!T` box. Void payloads have nothing to read.
  int payload_of(int box, Type *box_type, Type *value) {
    if (value->kind == TY_VOID) return -1;
    int at = gep_named(box, box_type, "value");
    return is_aggregate(value) ? at : load(at, value);
  }

  void finish_with_error(int code) {
    emit_defers(0, true);
    store(code, gep_named(sret, ret, "code"), types.error_ty);
    IrInst in{};
    in.op = IR_RET;
    emit(in);
  }

  int lower_try(Node *n) {
    Type *box_type = n->lhs->type;
    int box = expr(n->lhs);
    int code = load(gep_named(box, box_type, "code"), types.error_ty);
    int ok = binop(IR_EQ, code, constant(0, types.error_ty), types.bool_ty);

    int fail_bb = new_block();
    int next_bb = new_block();
    cond_branch(ok, next_bb, fail_bb);

    cur = fail_bb;
    finish_with_error(code);

    cur = next_bb;
    return payload_of(box, box_type, n->type);
  }

  int lower_catch(Node *n) {
    Type *box_type = n->lhs->type;
    Type *value = n->type;
    int box = expr(n->lhs);
    int code = load(gep_named(box, box_type, "code"), types.error_ty);
    int ok = binop(IR_EQ, code, constant(0, types.error_ty), types.bool_ty);

    if (n->body) {
      // The handler block is required to leave, so reaching next_bb means the
      // value is there.
      int fail_bb = new_block();
      int next_bb = new_block();
      cond_branch(ok, next_bb, fail_bb);

      cur = fail_bb;
      if (n->sym) {
        n->sym->slot = alloca_slot(types.error_ty);
        store(code, n->sym->slot, types.error_ty);
      }
      stmt(n->body);
      branch(next_bb);

      cur = next_bb;
      // In statement position the handler may fall through, and then there is
      // no payload to speak of.
      if (n->form == 1) return -1;
      return payload_of(box, box_type, value);
    }

    int ok_bb = new_block();
    int fail_bb = new_block();
    int join = new_block();
    int slot = value->kind == TY_VOID ? -1 : alloca_slot(value);
    cond_branch(ok, ok_bb, fail_bb);

    cur = ok_bb;
    if (slot >= 0) {
      int at = gep_named(box, box_type, "value");
      if (is_aggregate(value)) copy(slot, at, value);
      else store(load(at, value), slot, value);
    }
    branch(join);

    cur = fail_bb;
    if (slot >= 0) assign_into(slot, n->rhs, value);
    else expr(n->rhs);
    branch(join);

    cur = join;
    if (slot < 0) return -1;
    return is_aggregate(value) ? slot : load(slot, value);
  }

  // An interface value is { data, vtable }; dispatch loads the slot at the
  // index the checker resolved and calls through it, with `data` as receiver.
  void dispatch(Node *n, IrInst &in) {
    Node *base = n->lhs->lhs;
    Type *iface = base->type->kind == TY_PTR ? base->type->elem : base->type;
    Type *opaque = types.rawptr(types.u8_ty);

    int value = base->type->kind == TY_PTR ? expr(base) : addr(base);
    int data = load(gep_named(value, iface, "data"), opaque);
    int table = load(gep_named(value, iface, "vtable"), opaque);
    int slot = gep_index(table, constant((int64_t)n->ival, types.usize_ty),
                         opaque);

    in.callee.clear();
    in.a = load(slot, opaque);
    in.args.push_back(data);
  }

  // A value being handed to an interface parameter needs the { data, vtable }
  // pair built somewhere first.
  int materialize(Node *value) {
    if (!value->bind_to) return expr(value);
    int slot = alloca_slot(value->bind_to);
    assign_into(slot, value, value->bind_to);
    return slot;
  }

  // Each of these is one instruction. The receiver is always an address:
  // `hits.Add(1)` on a value, or `p.Add(1)` through a pointer to one.
  int atomic_call(Node *n) {
    Node *base = n->lhs->lhs;
    Type *slot = base->type->kind == TY_PTR ? base->type->elem : base->type;
    Type *inner = slot->elem;
    int at = base->type->kind == TY_PTR ? expr(base) : addr(base);
    const std::string &name = n->lhs->name;

    if (name == "Load") {
      IrInst in{};
      in.op = IR_ATOMIC_LOAD;
      in.dst = new_value(inner);
      in.a = at;
      in.type = inner;
      emit(in);
      return in.dst;
    }

    if (name == "Store") {
      IrInst in{};
      in.op = IR_ATOMIC_STORE;
      in.a = expr(n->kids[0]);
      in.b = at;
      in.type = inner;
      emit(in);
      return -1;
    }

    if (name == "CompareSwap") {
      IrInst in{};
      in.op = IR_ATOMIC_CAS;
      in.dst = new_value(types.bool_ty);
      in.a = at;
      in.b = expr(n->kids[0]);
      in.args.push_back(expr(n->kids[1]));
      in.type = inner;
      emit(in);
      return in.dst;
    }

    IrInst in{};
    in.op = IR_ATOMIC_RMW;
    in.dst = new_value(inner);
    in.imm = (int64_t)n->ival;
    in.a = at;
    in.b = expr(n->kids[0]);
    in.type = inner;
    emit(in);
    return in.dst;
  }

  // The gathered arguments go into an array on the caller's frame, handed over
  // as a slice. The callee never learns it was called with loose arguments.
  int gather(Node *n, size_t from, Type *slice) {
    Type *element = slice->elem;
    size_t count = n->kids.size() - from;
    int backing = alloca_slot(types.array(element, (int64_t)count));
    for (size_t i = 0; i < count; i++) {
      int at = gep_index(backing, constant((int64_t)i, types.usize_ty),
                         element);
      assign_into(at, n->kids[from + i], element);
    }

    int header = alloca_slot(slice);
    store(backing, gep_slice_ptr(header), types.rawptr(types.u8_ty));
    store(constant((int64_t)count, types.usize_ty), gep_slice_len(header),
          types.usize_ty);
    return header;
  }

  // What C does to an argument that travels through `...`: anything narrower
  // than an int arrives as one, an f32 as an f64. Getting this wrong is not a
  // wrong number but garbage, since the callee reads the wider slot either way.
  int promoted(int value, Type *from) {
    if (from->kind == TY_FLOAT)
      return from->bits < 64 ? widen(value, types.named("f64")) : value;
    if (from->kind == TY_BOOL ||
        ((from->kind == TY_INT || from->kind == TY_ENUM) && from->bits < 32))
      return widen(value, types.named("i32"));
    return value;
  }

  // `hashof(x)`. An integer is mixed here and now — splitmix64's finaliser,
  // which is what keeps keys 0, 1 and 2 from landing in neighbouring slots —
  // and a string goes to `hash.bytes`, which is written once for the program
  // rather than inlined at every call.
  int hash_of(Node *arg) {
    Type *u64 = types.named("u64");
    Type *from = arg->type;
    if (from->kind == TY_STRING) {
      wants_hash_bytes = true;
      int s = expr(arg);
      IrInst in{};
      in.op = IR_CALL;
      in.callee = "hash.bytes";
      in.type = u64;
      in.args.push_back(load(gep_slice_ptr(s), types.rawptr(types.u8_ty)));
      in.args.push_back(load(gep_slice_len(s), types.usize_ty));
      in.dst = new_value(u64);
      emit(in);
      return in.dst;
    }
    int v = expr(arg);
    if (from->kind == TY_BOOL || from->bits != 64) v = widen(v, u64);
    return mix64(v);
  }

  // Multiplies wrap here on purpose, which is why these go through binop
  // rather than through the checked arithmetic a `*` would get.
  int mix64(int v) {
    Type *u64 = types.named("u64");
    auto shift_xor = [&](int value, int64_t by) {
      return binop(IR_XOR, value,
                   binop(IR_SHR, value, constant(by, u64), u64), u64);
    };
    v = shift_xor(v, 30);
    v = binop(IR_MUL, v, constant((int64_t)0xbf58476d1ce4e5b9ull, u64), u64);
    v = shift_xor(v, 27);
    v = binop(IR_MUL, v, constant((int64_t)0x94d049bb133111ebull, u64), u64);
    return shift_xor(v, 31);
  }

  int call(Node *n) {
    if (n->form == 3) return atomic_call(n);
    if (n->form == 5) return shared_call(n);
    if (n->form == 6) return hash_of(n->kids[0]);
    IrInst in{};
    in.op = IR_CALL;
    in.callee = n->name.empty() ? n->lhs->name : n->name;
    in.type = n->type;
    if (n->form == 0 && n->sym && n->sym->decl && n->sym->decl->is_blocking) {
      if (blocking_names.insert(in.callee).second)
        blocking.push_back(n->sym->decl);
      in.callee += ".blocking";
    }

    int result = -1;
    if (is_aggregate(n->type)) {
      result = alloca_slot(n->type);
      in.args.push_back(result); // hidden out pointer, ahead of the real args
      in.type = types.void_ty;
    }
    // Both a `T` and a `*T` receiver are passed as the address of the value:
    // an aggregate parameter is already a pointer the callee copies from.
    if (n->form == 1) {
      Node *base = n->lhs->lhs;
      in.args.push_back(base->type->kind == TY_PTR ? expr(base) : addr(base));
    } else if (n->form == 2) {
      dispatch(n, in);
    } else if (n->form == 4) {
      // The target is a value, so there is no name to call: the backend takes
      // it from `a`, the same way it does for a vtable entry.
      in.callee.clear();
      in.a = expr(n->lhs);
    }

    size_t fixed = n->variadic_at >= 0 ? (size_t)n->variadic_at
                                       : n->kids.size();
    // Past an extern's `...` the C promotions apply, and the callee reads the
    // promoted width whatever the call site wrote.
    size_t declared = n->sym && n->sym->type->is_c_variadic
                          ? n->sym->type->params.size()
                          : fixed;
    for (size_t i = 0; i < fixed; i++) {
      int arg = materialize(n->kids[i]);
      if (i >= declared) arg = promoted(arg, n->kids[i]->type);
      in.args.push_back(arg);
    }
    if (n->variadic_at >= 0) {
      // Already a list: hand it over rather than copying it into a new one.
      if (n->is_variadic) in.args.push_back(expr(n->kids.back()));
      else in.args.push_back(gather(n, fixed, n->sym->type->params.back()));
    }
    if (result < 0 && n->type->kind != TY_VOID) in.dst = new_value(n->type);
    emit(in);
    return result >= 0 ? result : in.dst;
  }

  // Wait, Notify and NotifyAll, all of which take the guard and nothing else.
  int shared_call(Node *n) {
    Node *base = n->lhs->lhs;
    Type *box = base->type;
    if (box->kind == TY_PTR) box = box->elem;
    int at = base->type->kind == TY_PTR ? expr(base) : addr(base);
    static const char *names[] = {"sword_mutex_wait", "sword_mutex_notify",
                                  "sword_mutex_notify_all"};
    call_runtime(names[n->ival], {gep_named(at, box, "guard")}, types.void_ty);
    return -1;
  }

  int expr(Node *n) {
    switch (n->kind) {
    case ND_INT_LIT:
      return constant((int64_t)n->ival, n->type);

    case ND_FLOAT_LIT:
      return float_constant(n->fval, n->type);

    case ND_BOOL_LIT:
      return constant((int64_t)n->ival, types.bool_ty);

    case ND_STRING_LIT:
      return string_literal(n);

    case ND_ARRAY_LIT:
      return array_literal(n);

    case ND_STRUCT_LIT:
      return struct_literal(n);

    case ND_SLICE_EXPR:
      return slice_expr(n);

    case ND_IDENT:
      // A function named without being called is its address.
      if (n->sym->is_func) return func_address(n->sym->name);
      return is_aggregate(n->type) ? n->sym->slot : load(n->sym->slot, n->type);

    case ND_FIELD: case ND_INDEX: {
      // `a.len` on an array is known at compile time, with nothing to load.
      if (n->kind == ND_FIELD && n->lhs->type->kind == TY_ARRAY)
        return constant(n->lhs->type->count, types.usize_ty);
      int at = addr(n);
      return is_aggregate(n->type) ? at : load(at, n->type);
    }

    case ND_BINARY:
      if (n->op == TK_ANDAND || n->op == TK_OROR) return short_circuit(n);
      if (n->form == 1) { // a test against nil, not a comparison
        Node *box = n->lhs->kind == ND_NIL_LIT ? n->rhs : n->lhs;
        int present = opt_present(expr(box), box->type);
        if (n->op == TK_NE) return present;
        return binop(IR_EQ, present, constant(0, types.bool_ty),
                     types.bool_ty);
      }
      if (n->lhs->type->kind == TY_STRING &&
          (n->op == TK_EQ || n->op == TK_NE)) {
        int same = string_equal(expr(n->lhs), expr(n->rhs));
        if (n->op == TK_EQ) return same;
        return binop(IR_EQ, same, constant(0, types.bool_ty), types.bool_ty);
      }
      if (n->type->kind == TY_RAWPTR) {
        int base = expr(n->lhs);
        int offset = to_word(expr(n->rhs));
        if (n->op == TK_MINUS)
          offset = binop(IR_SUB, constant(0, types.usize_ty), offset,
                         types.usize_ty);
        return gep_index(base, offset, n->type->elem);
      }
      if (wraps(n->op) || !is_integer(n->type))
        return binop(ir_binop(n->op), expr(n->lhs), expr(n->rhs), n->type);
      return checked_arith(ir_binop(n->op), expr(n->lhs), expr(n->rhs),
                           n->type, n->pos);

    case ND_UNARY: {
      if (n->op == TK_AMP) return addr(n->lhs);
      if (n->op == TK_STAR) {
        int p = expr(n->lhs);
        return is_aggregate(n->type) ? p : load(p, n->type);
      }
      // Every bit flipped is a xor with every bit set, which a constant of -1
      // is at any width.
      if (n->op == TK_TILDE)
        return binop(IR_XOR, expr(n->lhs), constant(-1, n->type), n->type);
      IrInst in{};
      in.op = n->op == TK_BANG ? IR_NOT : IR_NEG;
      in.dst = new_value(n->type);
      in.a = expr(n->lhs);
      in.type = n->type;
      emit(in);
      return in.dst;
    }

    case ND_CALL:
      return call(n);

    case ND_ERROR_LIT:
      return constant((int64_t)n->ival, types.error_ty);

    case ND_NIL_LIT:
      return nil_value(n->type);

    case ND_TRY:
      return lower_try(n);

    case ND_ORELSE:
      return lower_orelse(n);

    case ND_CATCH:
      return lower_catch(n);

    case ND_CONVERT: {
      // An aggregate is already an address by the time it is a value, so a
      // conversion between two of the same shape has nothing to do. Pointers
      // likewise all look the same to the machine. A width match only means
      // something within one kind: i64 and f64 are both 64 bits.
      Type *source = n->kids[0]->type;
      if (is_slice_shaped(source) && is_slice_shaped(n->type))
        return expr(n->kids[0]);
      // An atomic is its payload, seen through a narrower door.
      if (source->kind == TY_ATOMIC || n->type->kind == TY_ATOMIC)
        return expr(n->kids[0]);
      // A fresh shared starts unlocked, and all-zeroes is what unlocked means.
      if (n->type->is_shared) {
        int slot = alloca_slot(n->type);
        zero(slot, n->type);
        Type *payload = n->type->elem;
        int inner = gep_named(slot, n->type, "value");
        int v = expr(n->kids[0]);
        if (is_aggregate(payload)) copy(inner, v, payload);
        else store(v, inner, payload);
        return slot;
      }

      int value = expr(n->kids[0]);
      Type *from = fn->value_type[value];
      if (is_address(from) && is_address(n->type)) return value;
      if (from->kind == n->type->kind && from->bits == n->type->bits)
        return value;
      // An enum is an integer of its width, so crossing between the two at the
      // same width moves nothing.
      auto counts = [](const Type *t) {
        return t->kind == TY_INT || t->kind == TY_ENUM;
      };
      if (counts(from) && counts(n->type) && from->bits == n->type->bits)
        return value;
      IrInst in{};
      in.op = IR_CAST;
      in.dst = new_value(n->type);
      in.a = value;
      in.type = n->type;
      emit(in);
      return in.dst;
    }

    default:
      return -1;
    }
  }

  // Boxes a value into an `any`: the tag comes from the static type, so there
  // is no reflection anywhere, only what the compiler already knew.
  void box_any(int at, Node *value, Type *any) {
    Type *source = value->type;
    int kind = any_kind_of(source);
    // An enum boxes as its member's name: printing `2` where the program says
    // `Kind.Int` would be technically true and useless. The number is still one
    // conversion away.
    Symbol *names = nullptr;
    if (source->kind == TY_ENUM && enum_names) {
      auto found = enum_names->find(source);
      if (found != enum_names->end()) {
        names = found->second;
        kind = ANY_STRING;
      }
    }
    zero(at, any);
    store(constant(kind, types.named("u8")),
          gep_named(at, any, "Kind"), types.named("u8"));

    if (names) {
      int slot = alloca_slot(types.string_ty);
      IrInst in{};
      in.op = IR_CALL;
      in.callee = names->name;
      in.type = types.void_ty;
      in.args = {slot, expr(value)}; // the hidden out pointer comes first
      emit(in);
      copy(gep_named(at, any, "Text"), slot, types.string_ty);
      return;
    }
    if (kind == ANY_STRING) {
      copy(gep_named(at, any, "Text"), expr(value), types.string_ty);
      return;
    }
    if (kind == ANY_FLOAT) {
      Type *wide = types.named("f64");
      int v = expr(value);
      if (source->bits != 64) v = widen(v, wide);
      store(v, gep_named(at, any, "Real"), wide);
      return;
    }

    // Everything else travels in the integer slot, widened to a word.
    Type *wide = types.named("i64");
    int v = expr(value);
    if (source->kind == TY_BOOL || source->bits != 64) v = widen(v, wide);
    store(v, gep_named(at, any, "Int"), wide);
  }

  int widen(int value, Type *to) {
    IrInst in{};
    in.op = IR_CAST;
    in.dst = new_value(to);
    in.a = value;
    in.type = to;
    emit(in);
    return in.dst;
  }

  // Writes the value of `value` into the address `at`, copying when the type
  // is an aggregate and wrapping when an optional is being given a payload.
  void assign_into(int at, Node *value, Type *type) {
    if (type->is_any && !(value->type && value->type->is_any)) {
      box_any(at, value, type);
      return;
    }
    // An optional first, then whatever the payload needs: a `?Interface` taking a
    // pointer to a struct has both a flag to set and a pair to build, and doing
    // the pair against the optional's own layout would write over the flag.
    if (type->is_optional && !type_eq(value->type, type)) {
      Type *payload = const_cast<Type *>(opt_payload(type));
      store(constant(1, types.bool_ty), gep_named(at, type, "has"),
            types.bool_ty);
      assign_into(gep_named(at, type, "value"), value, payload);
      return;
    }
    if (value->vtable >= 0) {
      // The node still has its own pointer type; the vtable the checker picked
      // is what turns the pair into an interface value.
      Type *opaque = types.rawptr(types.u8_ty);
      store(expr(value), gep_named(at, type, "data"), opaque);

      IrInst table{};
      table.op = IR_VTABLE;
      table.dst = new_value(opaque);
      table.imm = value->vtable;
      table.type = opaque;
      emit(table);
      store(table.dst, gep_named(at, type, "vtable"), opaque);
      return;
    }
    int v = expr(value);
    if (is_aggregate(type)) copy(at, v, type);
    else store(v, at, type);
  }

  // --- statements --------------------------------------------------------

  void if_stmt(Node *n) {
    if (!n->name.empty()) {
      Type *box_type = n->cond->type;
      int box = expr(n->cond);
      int has = opt_present(box, box_type);

      int then_bb = new_block();
      int else_bb = n->els ? new_block() : -1;
      int join = new_block();
      cond_branch(has, then_bb, else_bb >= 0 ? else_bb : join);

      cur = then_bb;
      n->sym->slot = alloca_slot(n->sym->type);
      int inner = opt_value(box, box_type);
      if (is_aggregate(n->sym->type)) copy(n->sym->slot, inner, n->sym->type);
      else store(inner, n->sym->slot, n->sym->type);
      stmt(n->body);
      branch(join);

      if (else_bb >= 0) {
        cur = else_bb;
        stmt(n->els);
        branch(join);
      }
      cur = join;
      return;
    }

    int cond = expr(n->cond);
    int then_bb = new_block();
    int else_bb = n->els ? new_block() : -1;
    int join = new_block();

    cond_branch(cond, then_bb, else_bb >= 0 ? else_bb : join);

    cur = then_bb;
    stmt(n->body);
    branch(join);

    if (else_bb >= 0) {
      cur = else_bb;
      stmt(n->els);
      branch(join);
    }
    cur = join;
  }

  void range_loop(Node *n) {
    Type *type = n->type;
    int slot = alloca_slot(type);
    n->sym->slot = slot;

    store(expr(n->lhs), slot, type);
    int limit = expr(n->rhs);

    int cond_bb = new_block();
    int body_bb = new_block();
    int step_bb = new_block();
    int exit_bb = new_block();

    branch(cond_bb);
    cur = cond_bb;
    cond_branch(binop(IR_LT, load(slot, type), limit, types.bool_ty), body_bb,
                exit_bb);

    Symbol *slice = walked_slice(n, false);
    if (slice) walked.push_back({n->sym, slice});
    loops.push_back({step_bb, exit_bb, scopes.size()});
    cur = body_bb;
    stmt(n->body);
    branch(step_bb);
    loops.pop_back();
    if (slice) walked.pop_back();

    cur = step_bb;
    store(binop(IR_ADD, load(slot, type), constant(1, type), type), slot, type);
    branch(cond_bb);

    cur = exit_bb;
  }

  // Walks a slice in fixed-size pieces, the last one short. Each iteration
  // rebuilds the piece in the same slot, which is fine because a task copies
  // the header it is given.
  // `for x in xs`: one element at a time, copied into the binding. The length
  // is read once, so growing the thing being walked mid-loop cannot run off
  // the end of what was there when it started.
  void element_loop(Node *n) {
    Type *elem = n->sym->type;
    Type *word = types.usize_ty;

    int len = -1;
    int data = data_pointer(n->lhs, &len);

    int index = alloca_slot(word);
    store(constant(0, word), index, word);
    int slot = alloca_slot(elem);
    n->sym->slot = slot;
    if (n->index_sym) n->index_sym->slot = index;

    int cond_bb = new_block();
    int body_bb = new_block();
    int step_bb = new_block();
    int exit_bb = new_block();

    branch(cond_bb);
    cur = cond_bb;
    cond_branch(binop(IR_LT, load(index, word), len, types.bool_ty), body_bb,
                exit_bb);

    cur = body_bb;
    int at = gep_index(data, load(index, word), elem);
    if (is_aggregate(elem)) copy(slot, at, elem);
    else store(load(at, elem), slot, elem);

    loops.push_back({step_bb, exit_bb, scopes.size()});
    stmt(n->body);
    branch(step_bb);
    loops.pop_back();

    cur = step_bb;
    store(binop(IR_ADD, load(index, word), constant(1, word), word), index,
          word);
    branch(cond_bb);

    cur = exit_bb;
  }

  void walk_loop(Node *n) {
    if (n->form == 1) element_loop(n); // FOR_ELEMENTS
    else partition_loop(n);
  }

  void partition_loop(Node *n) {
    Type *slice = n->type;
    Type *elem = slice->elem;
    Type *word = types.usize_ty;

    int len = -1;
    int data = data_pointer(n->lhs, &len);
    int size = to_word(expr(n->rhs));

    // Checked once per loop, not per iteration: a zero-sized piece would
    // never advance the offset.
    guard(binop(IR_NE, size, constant(0, word), types.bool_ty),
          "panic: chunks(0) would never advance, at line " +
              std::to_string(n->pos.line) + "\n");

    int offset = alloca_slot(word);
    store(constant(0, word), offset, word);

    int part = alloca_slot(slice);
    n->sym->slot = part;
    if (n->index_sym) n->index_sym->slot = offset;

    int cond_bb = new_block();
    int body_bb = new_block();
    int step_bb = new_block();
    int exit_bb = new_block();

    branch(cond_bb);
    cur = cond_bb;
    cond_branch(binop(IR_LT, load(offset, word), len, types.bool_ty), body_bb,
                exit_bb);

    cur = body_bb;
    int start = load(offset, word);
    int stop = binop(IR_ADD, start, size, word);
    // The final piece stops at the end of the slice rather than past it.
    int over = binop(IR_GT, stop, len, types.bool_ty);
    int clamp = alloca_slot(word);
    store(stop, clamp, word);
    int clamp_bb = new_block();
    int ready_bb = new_block();
    cond_branch(over, clamp_bb, ready_bb);
    cur = clamp_bb;
    store(len, clamp, word);
    branch(ready_bb);
    cur = ready_bb;
    stop = load(clamp, word);

    store(gep_index(data, start, elem), gep_slice_ptr(part),
          types.rawptr(types.u8_ty));
    store(binop(IR_SUB, stop, start, word), gep_slice_len(part), word);

    loops.push_back({step_bb, exit_bb, scopes.size()});
    stmt(n->body);
    branch(step_bb);
    loops.pop_back();

    cur = step_bb;
    store(binop(IR_ADD, load(offset, word), size, word), offset, word);
    branch(cond_bb);

    cur = exit_bb;
  }

  // A chain of compares in source order. No jump table: a switch here is
  // usually a handful of cases, and LLVM turns a dense chain into a table on
  // its own when it is worth it.
  void switch_stmt(Node *n) {
    bool text = n->cond->type->kind == TY_STRING;
    int subject = expr(n->cond);

    int exit_bb = new_block();
    std::vector<int> arms;
    int fallback_bb = exit_bb;
    for (Node *arm : n->kids) {
      arms.push_back(new_block());
      if (arm->kids.empty()) fallback_bb = arms.back();
    }

    for (size_t i = 0; i < n->kids.size(); i++) {
      for (Node *value : n->kids[i]->kids) {
        int same = text ? string_equal(subject, expr(value))
                        : binop(IR_EQ, subject, expr(value), types.bool_ty);
        int next_bb = new_block();
        cond_branch(same, arms[i], next_bb);
        cur = next_bb;
      }
    }
    branch(fallback_bb);

    for (size_t i = 0; i < n->kids.size(); i++) {
      cur = arms[i];
      scopes.emplace_back();
      stmt(n->kids[i]->body);
      emit_defers(scopes.size() - 1, false);
      scopes.pop_back();
      branch(exit_bb);
    }
    cur = exit_bb;
  }

  // `select`. Three arrays go to the runtime — the channels, what each case
  // wants, and where its value lives — and what comes back is the index of the
  // case that went. The runtime does the send or the receive itself, under the
  // channel's own lock: choosing first and acting afterwards would leave a gap
  // where a `default` could still end up waiting.
  void select_stmt(Node *n) {
    Type *opaque = types.rawptr(types.u8_ty);
    std::vector<Node *> cases;
    Node *fallback = nullptr;
    for (Node *arm : n->kids) {
      if (arm->form == 2) fallback = arm;
      else cases.push_back(arm);
    }

    int64_t count = (int64_t)cases.size();
    Type *chan_array = types.array(opaque, count > 0 ? count : 1);
    Type *op_array = types.array(types.named("i32"), count > 0 ? count : 1);
    int chans = alloca_slot(chan_array);
    int ops = alloca_slot(op_array);
    int values = alloca_slot(chan_array);

    std::vector<int> slots;
    for (size_t i = 0; i < cases.size(); i++) {
      Node *arm = cases[i];
      int at = constant((int64_t)i, types.usize_ty);
      store(expr(arm->lhs), gep_index(chans, at, opaque), opaque);
      store(constant(arm->form == 1 ? 1 : 0, types.named("i32")),
            gep_index(ops, at, types.named("i32")), types.named("i32"));

      // A send's value has to be somewhere the runtime can copy from; a
      // receive's needs somewhere to land. Same slot either way.
      Type *carried = arm->form == 1
                          ? arm->type
                          : const_cast<Type *>(opt_payload(arm->type));
      int slot = alloca_slot(carried);
      if (arm->form == 1) assign_into(slot, arm->rhs, carried);
      else zero(slot, carried);
      slots.push_back(slot);
      store(slot, gep_index(values, at, opaque), opaque);
    }

    int got = alloca_slot(types.named("i32"));
    store(constant(0, types.named("i32")), got, types.named("i32"));

    int chosen = call_runtime(
        "sword_chan_select",
        {chans, ops, values, constant(count, types.named("i64")),
         constant(fallback ? 1 : 0, types.named("i32")), got},
        types.named("i64"));

    int exit_bb = new_block();
    std::vector<int> arms;
    for (size_t i = 0; i < cases.size(); i++) arms.push_back(new_block());
    int fallback_bb = fallback ? new_block() : exit_bb;

    for (size_t i = 0; i < cases.size(); i++) {
      int same = binop(IR_EQ, chosen, constant((int64_t)i, types.named("i64")),
                       types.bool_ty);
      int next_bb = new_block();
      cond_branch(same, arms[i], next_bb);
      cur = next_bb;
    }
    branch(fallback_bb);

    for (size_t i = 0; i < cases.size(); i++) {
      cur = arms[i];
      scopes.emplace_back();
      Node *arm = cases[i];
      if (arm->form == 0 && arm->sym) {
        // The binding is a `?T`: the value if one came, nothing if the channel
        // was closed. Which of the two is what `got` says.
        Type *box = arm->type;
        Type *carried = const_cast<Type *>(opt_payload(box));
        int slot = alloca_slot(box);
        arm->sym->slot = slot;
        int had = binop(IR_NE, load(got, types.named("i32")),
                        constant(0, types.named("i32")), types.bool_ty);
        store(had, gep_named(slot, box, "has"), types.bool_ty);
        int inner = gep_named(slot, box, "value");
        if (is_aggregate(carried)) copy(inner, slots[i], carried);
        else store(load(slots[i], carried), inner, carried);
      }
      stmt(arm->body);
      emit_defers(scopes.size() - 1, false);
      scopes.pop_back();
      branch(exit_bb);
    }

    if (fallback) {
      cur = fallback_bb;
      scopes.emplace_back();
      stmt(fallback->body);
      emit_defers(scopes.size() - 1, false);
      scopes.pop_back();
      branch(exit_bb);
    }
    cur = exit_bb;
  }

  // `for x := optional { }`. The optional is evaluated afresh each turn, which
  // is what makes it a loop rather than an `if`.
  void optional_loop(Node *n) {
    Type *box_type = n->cond->type;
    int slot = alloca_slot(n->sym->type);
    n->sym->slot = slot;

    int cond_bb = new_block();
    int body_bb = new_block();
    int exit_bb = new_block();

    branch(cond_bb);
    cur = cond_bb;
    int box = expr(n->cond);
    cond_branch(opt_present(box, box_type), body_bb, exit_bb);

    cur = body_bb;
    int inner = opt_value(box, box_type);
    if (is_aggregate(n->sym->type)) copy(slot, inner, n->sym->type);
    else store(inner, slot, n->sym->type);

    loops.push_back({cond_bb, exit_bb, scopes.size()});
    stmt(n->body);
    branch(cond_bb);
    loops.pop_back();

    cur = exit_bb;
  }

  void cond_loop(Node *n) {
    int cond_bb = new_block();
    int body_bb = new_block();
    int exit_bb = new_block();

    branch(cond_bb);
    cur = cond_bb;
    if (n->cond) cond_branch(expr(n->cond), body_bb, exit_bb);
    else branch(body_bb);

    loops.push_back({cond_bb, exit_bb, scopes.size()});
    cur = body_bb;
    stmt(n->body);
    branch(cond_bb);
    loops.pop_back();

    cur = exit_bb;
  }

  void assign_stmt(Node *n) {
    Type *type = n->lhs->type;
    int at = addr(n->lhs);

    if (n->op == TK_ASSIGN) {
      assign_into(at, n->rhs, type);
      return;
    }
    TokKind arith = assign_arith(n->op);
    int value = expr(n->rhs);
    int current = load(at, type);
    int result = is_integer(type) && !wraps(arith)
                     ? checked_arith(ir_binop(arith), current, value, type,
                                     n->pos)
                     : binop(ir_binop(arith), current, value, type);
    store(result, at, type);
  }

  void emit_ret(int value, Type *type) {
    IrInst in{};
    in.op = IR_RET;
    if (value >= 0) {
      in.a = value;
      in.type = type;
    }
    emit(in);
  }

  // The return value is computed before any deferred statement runs, so a
  // `defer` that frees scratch memory cannot clobber what is being returned.
  void return_stmt(Node *n) {
    if (!ret->is_error_union) {
      // Through the out pointer this goes via assign_into, which is where an
      // optional gets wrapped around a plain value.
      if (sret >= 0 && n->lhs) {
        assign_into(sret, n->lhs, ret);
        emit_defers(0, false);
        emit_ret(-1, nullptr);
        return;
      }
      int value = n->lhs ? expr(n->lhs) : -1;
      emit_defers(0, false);
      emit_ret(value, n->lhs ? n->lhs->type : nullptr);
      return;
    }

    switch (n->form) {
    case 4: // bare return from a fallible function: success
      emit_defers(0, false);
      emit_ret(-1, nullptr);
      break;

    case 1: { // a plain value, wrapped as success
      assign_into(gep_named(sret, ret, "value"), n->lhs,
                  const_cast<Type *>(ret->elem));
      emit_defers(0, false);
      emit_ret(-1, nullptr);
      break;
    }

    case 2: // an error code
      finish_with_error(expr(n->lhs));
      break;

    default: { // forwarding someone else's !T: which path it is decides at run time
      int box = expr(n->lhs);
      copy(sret, box, ret);
      int code = load(gep_named(sret, ret, "code"), types.error_ty);
      int failed = binop(IR_NE, code, constant(0, types.error_ty),
                         types.bool_ty);

      int err_bb = new_block();
      int ok_bb = new_block();
      cond_branch(failed, err_bb, ok_bb);

      cur = err_bb;
      emit_defers(0, true);
      emit_ret(-1, nullptr);

      cur = ok_bb;
      emit_defers(0, false);
      emit_ret(-1, nullptr);
      break;
    }
    }
  }

  void stmt(Node *n) {
    switch (n->kind) {
    case ND_BLOCK:
      scopes.emplace_back();
      for (Node *st : n->kids) stmt(st);
      emit_defers(scopes.size() - 1, false);
      scopes.pop_back();
      break;

    case ND_DEFER:
      scopes.back().push_back({n->body, n->is_errdefer});
      break;

    case ND_VAR:
      n->sym->slot = alloca_slot(n->type);
      assign_into(n->sym->slot, n->rhs, n->type);
      break;

    case ND_ASSIGN:
      assign_stmt(n);
      break;

    case ND_RETURN:
      return_stmt(n);
      break;

    case ND_IF:
      if_stmt(n);
      break;

    case ND_SELECT:
      select_stmt(n);
      break;

    case ND_FOR:
      if (n->is_parallel) parallel_for(n);
      else if (n->form == 2) optional_loop(n); // FOR_OPTIONAL
      else if (n->is_range) range_loop(n);
      else if (!n->name.empty()) walk_loop(n);
      else cond_loop(n);
      break;

    case ND_BREAK:
      emit_defers(loops.back().depth, false);
      branch(loops.back().break_bb);
      break;

    case ND_CONTINUE:
      emit_defers(loops.back().depth, false);
      branch(loops.back().continue_bb);
      break;

    case ND_SCOPE:
      scope_stmt(n);
      break;

    case ND_SPAWN:
      spawn_stmt(n);
      break;

    case ND_LOCK:
      lock_stmt(n);
      break;

    case ND_SWITCH:
      switch_stmt(n);
      break;

    case ND_EXPR_STMT:
      expr(n->lhs);
      break;

    default:
      break;
    }
  }

  int string_value(const std::string &text) {
    IrInst in{};
    in.op = IR_STR_VALUE;
    in.dst = new_value(types.ptr(types.string_ty));
    in.imm = mod.intern(text);
    in.type = types.string_ty;
    emit(in);
    return in.dst;
  }

  // `error.name` and `error.message`: a code in, a string out. Written here
  // rather than as source because the set of errors is only complete once every
  // package has been checked, and this is the first place that is true.
  void error_table(IrFunc &out, bool messages) {
    fn = &out;
    cur = 0;
    sret = -1;
    entry_allocas.clear();
    loops.clear();
    scopes.clear();

    out.name = messages ? "error.message" : "error.name";
    out.params.push_back(types.error_ty);
    out.ret = ret = types.string_ty;
    out.ret_by_pointer = true;
    out.is_internal = true;

    sret = new_value(types.ptr(types.string_ty));
    int code = new_value(types.error_ty);
    new_block();

    // A chain rather than a table: error sets are small, and LLVM turns a dense
    // chain into a switch on its own.
    const std::vector<TypeTable::ErrorDecl> &all = types.errors();
    for (size_t i = 0; i < all.size(); i++) {
      if (!all[i].declared) continue;
      int same = binop(IR_EQ, code,
                       constant((int64_t)i + 1, types.error_ty), types.bool_ty);
      int hit = new_block();
      int next = new_block();
      cond_branch(same, hit, next);

      cur = hit;
      copy(sret, string_value(messages ? all[i].message : all[i].name),
           types.string_ty);
      emit_ret(-1, types.void_ty);
      cur = next;
    }
    // Code zero is success and has no name; so has anything past the set.
    copy(sret, string_value(""), types.string_ty);
    emit_ret(-1, types.void_ty);

    auto &entry = out.blocks[0].insts;
    entry.insert(entry.begin(), entry_allocas.begin(), entry_allocas.end());
  }

  // `hash.bytes`: FNV-1a over a run of bytes, for `hashof` over a string.
  // Written here rather than in Sword because nothing in the language can be
  // reached from lowering, and written once rather than at every call site.
  void hash_bytes(IrFunc &out) {
    fn = &out;
    cur = 0;
    sret = -1;
    entry_allocas.clear();
    loops.clear();
    scopes.clear();

    Type *u64 = types.named("u64");
    Type *raw = types.rawptr(types.u8_ty);
    out.name = "hash.bytes";
    out.params.push_back(raw);
    out.params.push_back(types.usize_ty);
    out.ret = ret = u64;
    out.is_internal = true;

    int ptr = new_value(raw);
    int len = new_value(types.usize_ty);
    new_block();

    // Both cross a block boundary, so they live in slots rather than values.
    int h = alloca_slot(u64);
    int at = alloca_slot(types.usize_ty);
    store(constant((int64_t)0xcbf29ce484222325ull, u64), h, u64);
    store(constant(0, types.usize_ty), at, types.usize_ty);

    int head = new_block();
    branch(head);
    cur = head;
    int i = load(at, types.usize_ty);
    int more = binop(IR_LT, i, len, types.bool_ty);
    int body = new_block();
    int done = new_block();
    cond_branch(more, body, done);

    cur = body;
    int byte = load(gep_index(ptr, i, types.u8_ty), types.u8_ty);
    int mixed = binop(IR_XOR, load(h, u64), widen(byte, u64), u64);
    // The multiply is meant to overflow, which is what FNV is.
    store(binop(IR_MUL, mixed, constant((int64_t)1099511628211ull, u64), u64),
          h, u64);
    store(binop(IR_ADD, i, constant(1, types.usize_ty), types.usize_ty), at,
          types.usize_ty);
    branch(head);

    cur = done;
    emit_ret(load(h, u64), u64);

    auto &entry = out.blocks[0].insts;
    entry.insert(entry.begin(), entry_allocas.begin(), entry_allocas.end());
  }

  void func(Node *decl, IrFunc &out) {
    fn = &out;
    cur = 0;
    sret = -1;
    entry_allocas.clear();
    loops.clear();
    scopes.clear();

    out.name = decl->name;
    out.ret = ret = decl->sym->type->ret;
    out.is_extern = decl->is_extern;
    out.is_c_variadic = decl->is_c_variadic;
    out.is_weak = decl->is_weak;
    // A program is one module, and nothing outside it calls a Sword function by
    // name — the runtime gets thunks, C gets addresses. Only the entry point is
    // seen from outside, so everything else is internal: LLVM drops what nothing
    // calls, and with it the C symbols only those functions needed.
    out.is_internal = decl->is_hidden || decl->name != "main.main";
    out.ret_by_pointer = is_aggregate(out.ret);
    for (Node *p : decl->kids) out.params.push_back(p->type);
    if (decl->is_extern) return;

    // Parameters occupy the first value ids; the backend names them the same.
    if (out.ret_by_pointer) sret = new_value(types.ptr(out.ret));
    int first_param = (int)fn->value_type.size();
    // Aggregates are passed as the address of the caller's value.
    for (Node *p : decl->kids)
      new_value(is_aggregate(p->type) ? types.ptr(p->type) : p->type);
    new_block();

    for (size_t i = 0; i < decl->kids.size(); i++) {
      Node *p = decl->kids[i];
      p->sym->slot = alloca_slot(p->type);
      // Aggregates arrive as a pointer and are copied: parameters are values.
      if (is_aggregate(p->type)) copy(p->sym->slot, first_param + (int)i, p->type);
      else store(first_param + (int)i, p->sym->slot, p->type);
    }

    // Start a fallible function off as success, so falling off the end of an
    // `!void` and every `return value` path are already correct.
    if (ret->is_error_union)
      store(constant(0, types.error_ty), gep_named(sret, ret, "code"),
            types.error_ty);

    stmt(decl->body);

    auto &entry = out.blocks[0].insts;
    entry.insert(entry.begin(), entry_allocas.begin(), entry_allocas.end());

    // Every block needs a terminator. A block left open either falls off the
    // end of a void function or was made unreachable by a return.
    for (IrBlock &bb : out.blocks) {
      bool open = bb.insts.empty();
      if (!open) {
        switch (bb.insts.back().op) {
        case IR_BR: case IR_CONDBR: case IR_RET: case IR_UNREACHABLE: break;
        default: open = true; break;
        }
      }
      if (!open) continue;

      IrInst in{};
      in.op = out.ret->kind == TY_VOID || out.ret_by_pointer ? IR_RET
                                                             : IR_UNREACHABLE;
      bb.insts.push_back(in);
    }
  }
};

} // namespace

void lower(Program &prog, TypeTable &types, Mode mode, IrModule &mod) {
  Lowerer lowerer(types, mod, mode);
  lowerer.enum_names = &prog.enum_names;

  for (Package *pkg : prog.order)
    for (Node *decl : pkg->unit->kids)
      // A generic struct has no type of its own; only its instantiations do,
      // and those are collected from the type table below.
      if ((decl->kind == ND_STRUCT_DECL || decl->kind == ND_INTERFACE_DECL) &&
          decl->type)
        mod.structs.push_back(decl->type);
  for (const auto &vt : types.vtables()) mod.vtables.push_back(vt.entries);
  // `any` belongs to the language rather than to a package, so nothing else
  // would have put it in front of the backend.
  mod.structs.push_back(types.any_ty);
  for (Type *t : types.error_unions_made()) mod.structs.push_back(t);
  for (Type *t : types.optionals_made()) mod.structs.push_back(t);
  for (Type *t : types.shareds_made()) mod.structs.push_back(t);
  for (Type *t : types.instances_made()) mod.structs.push_back(t);

  // Dependencies come first, so a package is always lowered after everything
  // it refers to.
  for (Package *pkg : prog.order) {
    lowerer.unit = pkg->prefix.empty() ? "main." : pkg->prefix;
    for (Node *decl : pkg->unit->kids) {
      // A generic is never compiled, only its instantiations are; and a
      // declaration with no symbol is a template or a rejected one.
      if (decl->kind != ND_FUNC || !decl->tparams.empty()) continue;
      if (!decl->sym) continue;
      mod.funcs.emplace_back();
      mod.funcs.back().unit = lowerer.unit;
      lowerer.func(decl, mod.funcs.back());
    }
    // Which instances a package owns depends on the program using it, so they
    // are kept apart from the code that does not.
    lowerer.unit += "inst.";
    for (Node *decl : pkg->instances) {
      mod.funcs.emplace_back();
      mod.funcs.back().unit = lowerer.unit;
      lowerer.func(decl, mod.funcs.back());
    }
  }
  lowerer.unit.clear();

  // After everything else: the tables are only complete once every error in the
  // program has been declared.
  if (prog.error_name) {
    mod.funcs.emplace_back();
    lowerer.error_table(mod.funcs.back(), false);
  }
  if (prog.error_message) {
    mod.funcs.emplace_back();
    lowerer.error_table(mod.funcs.back(), true);
  }
  if (lowerer.wants_hash_bytes) {
    mod.funcs.emplace_back();
    lowerer.hash_bytes(mod.funcs.back());
  }

  for (const auto &thunk : lowerer.thunks) {
    mod.funcs.emplace_back();
    lowerer.emit_thunk(thunk, mod.funcs.back());
  }
  for (const auto &chunk : lowerer.chunks) {
    mod.funcs.emplace_back();
    lowerer.emit_chunk(chunk, mod.funcs.back());
  }
  for (Node *decl : lowerer.blocking) {
    mod.funcs.emplace_back();
    lowerer.emit_blocking_wrapper(decl, mod.funcs.back());
    mod.funcs.emplace_back();
    lowerer.emit_blocking_job(decl, mod.funcs.back());
  }
}
