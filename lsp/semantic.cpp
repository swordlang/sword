#include "semantic.h"

#include "../src/lexer.h"

#include <map>
#include <set>

// Keep in step with the indices used below; the editor is told this order.
const std::vector<std::string> kTokenTypes = {
    "namespace", "type",     "struct",   "interface", "parameter",
    "variable",  "property", "function", "method",    "keyword",
    "string",    "number",   "enumMember", "enum",
};

const std::vector<std::string> kTokenModifiers = {
    "declaration", "readonly", "defaultLibrary",
};

namespace {

enum Type {
  T_NAMESPACE, T_TYPE, T_STRUCT, T_INTERFACE, T_PARAMETER,
  T_VARIABLE, T_PROPERTY, T_FUNCTION, T_METHOD, T_KEYWORD,
  T_STRING, T_NUMBER, T_ENUM_MEMBER, T_ENUM,
};

enum Modifier {
  M_DECLARATION = 1,
  M_READONLY = 2,
  M_BUILTIN = 4,
};

struct Mark {
  int type;
  int modifiers;
};

bool is_keyword(TokKind kind) {
  return kind >= TK_PACKAGE && kind <= TK_NIL;
}

// The protocol counts columns in UTF-16 code units, so a line with accented
// text would otherwise paint in the wrong place.
int utf16_column(const std::string &line, int byte_col) {
  int units = 0;
  for (int i = 0; i < byte_col && i < (int)line.size();) {
    unsigned char c = (unsigned char)line[i];
    int width = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
    units += width == 4 ? 2 : 1; // beyond the BMP takes a surrogate pair
    i += width;
  }
  return units;
}

struct Classifier {
  int file;
  std::map<std::pair<int, int>, Mark> marks;
  std::set<Symbol *> parameters;

  void mark(Pos at, int type, int modifiers = 0) {
    if (at.file != file || at.line < 1) return;
    marks[{at.line, at.col}] = Mark{type, modifiers};
  }

  void visit(Node *n) {
    if (!n) return;
    switch (n->kind) {
    case ND_FUNC:
      // A receiver makes it a method; the checker leaves it on lhs even after
      // moving it into the parameter list.
      mark(n->name_pos, n->lhs ? T_METHOD : T_FUNCTION, M_DECLARATION);
      break;
    case ND_PARAM:
      if (n->sym) parameters.insert(n->sym);
      mark(n->name_pos, T_PARAMETER, M_DECLARATION);
      break;
    case ND_STRUCT_DECL:
      mark(n->name_pos, T_STRUCT, M_DECLARATION);
      break;
    case ND_INTERFACE_DECL:
      mark(n->name_pos, T_INTERFACE, M_DECLARATION);
      break;
    case ND_ENUM_DECL:
      mark(n->name_pos, T_ENUM, M_DECLARATION);
      break;
    case ND_TYPE_ALIAS:
      mark(n->pos, T_KEYWORD);
      mark(n->name_pos, T_TYPE, M_DECLARATION);
      break;
    case ND_ENUM_MEMBER:
      mark(n->name_pos, T_ENUM_MEMBER, M_DECLARATION | M_READONLY);
      break;
    case ND_FIELD_DECL:
      mark(n->name_pos, T_PROPERTY, M_DECLARATION);
      break;
    case ND_VAR:
      mark(n->name_pos, T_VARIABLE,
           M_DECLARATION | (n->is_mut ? 0 : M_READONLY));
      break;
    case ND_FOR:
      mark(n->name_pos, T_VARIABLE, M_DECLARATION | M_READONLY);
      break;
    case ND_CATCH:
      mark(n->name_pos, T_VARIABLE, M_DECLARATION | M_READONLY);
      break;
    case ND_LOCK:
      // The locked value is a binding for the length of the block, mutable
      // unless the lock is a reader's.
      mark(n->name_pos, T_VARIABLE,
           M_DECLARATION | (n->is_read ? M_READONLY : 0));
      break;
    case ND_TYPE_NAME:
      // `mem.Arena`: the qualifier sits at pos, the name at name_pos.
      if (!n->text.empty()) mark(n->pos, T_NAMESPACE);
      mark(n->name_pos, T_TYPE, builtin_type(n) ? M_BUILTIN : 0);
      break;
    case ND_STRUCT_LIT:
      mark(n->name_pos, T_TYPE);
      break;
    case ND_FIELD_INIT:
      mark(n->name_pos, T_PROPERTY);
      break;
    case ND_ERROR_LIT:
      mark(n->pos, T_NAMESPACE); // the `error` in `error.Name`
      mark(n->name_pos, T_ENUM_MEMBER);
      break;
    case ND_FIELD:
      // A package qualifier has no symbol behind it; a real field does.
      if (n->lhs && n->lhs->kind == ND_IDENT && !n->lhs->sym)
        mark(n->lhs->name_pos, T_NAMESPACE);
      mark(n->name_pos, T_PROPERTY);
      break;
    case ND_IDENT:
      if (n->sym && n->sym->is_func)
        mark(n->name_pos, T_FUNCTION);
      else if (n->sym && parameters.count(n->sym))
        mark(n->name_pos, T_PARAMETER);
      else if (n->sym)
        mark(n->name_pos, T_VARIABLE, n->sym->is_mut ? 0 : M_READONLY);
      break;
    default:
      break;
    }

    for (Node *kid : n->kids) visit(kid);
    for (Node *tp : n->tparams) visit(tp);
    visit(n->lhs);
    visit(n->rhs);
    visit(n->cond);
    visit(n->body);
    visit(n->els);
    visit(n->type_expr);
  }

  // A name the compiler already knows, like i32 or string.
  static bool builtin_type(Node *n) {
    static const std::set<std::string> builtins = {
        "void", "bool", "string", "error", "int",  "uint", "i8",  "i16",
        "i32",  "i64",  "u8",     "u16",   "u32",  "u64",  "f32", "f64"};
    return n->text.empty() && builtins.count(n->name) > 0;
  }
};

} // namespace

std::vector<SemToken> semantic_tokens(int file, Program &prog) {
  Classifier classifier;
  classifier.file = file;

  // Parameters have to be known before uses are classified, so declarations
  // are collected in the same walk that runs before the token sweep.
  for (Package *pkg : prog.order) classifier.visit(pkg->unit);
  for (Package &pkg : prog.packages)
    if (pkg.unit) classifier.visit(pkg.unit);
  for (Package &pkg : prog.packages)
    for (Node *inst : pkg.instances) classifier.visit(inst);

  const Source &src = source_at(file);
  std::vector<SemToken> out;
  for (const Token &tok : lex(file)) {
    if (tok.kind == TK_EOF || tok.kind == TK_TERM || tok.len <= 0) continue;

    Mark mark{-1, 0};
    if (is_keyword(tok.kind)) {
      mark = {T_KEYWORD, 0};
    } else if (tok.kind == TK_STRING) {
      mark = {T_STRING, 0};
    } else if (tok.kind == TK_INT || tok.kind == TK_FLOAT) {
      mark = {T_NUMBER, 0};
    } else if (tok.kind == TK_IDENT) {
      auto found = classifier.marks.find({tok.pos.line, tok.pos.col});
      // Without a resolved symbol a name is still a name, not punctuation.
      mark = found != classifier.marks.end() ? found->second
                                             : Mark{T_VARIABLE, 0};
    } else {
      continue; // punctuation is left to the editor's own grammar
    }

    SemToken out_tok;
    out_tok.line = tok.pos.line - 1;
    out_tok.col = utf16_column(src.line(tok.pos.line), tok.pos.col - 1);
    out_tok.length = tok.len;
    out_tok.type = mark.type;
    out_tok.modifiers = mark.modifiers;
    out.push_back(out_tok);
  }
  return out;
}
