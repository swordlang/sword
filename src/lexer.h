#pragma once

#include "diag.h"

#include <string>
#include <vector>

enum TokKind {
  TK_EOF,
  TK_TERM, // ';', written or inserted at a newline

  TK_IDENT,
  TK_INT,
  TK_FLOAT,
  TK_STRING,

  TK_PACKAGE, TK_IMPORT, TK_FUNC, TK_RETURN, TK_IF, TK_ELSE, TK_FOR, TK_IN,
  TK_SWITCH, TK_CASE, TK_DEFAULT,
  TK_BREAK, TK_CONTINUE, TK_MUT, TK_CONST, TK_STRUCT, TK_INTERFACE,
  TK_EXTERN, TK_DET, TK_DEFER, TK_ERRDEFER, TK_TRY, TK_CATCH, TK_ORELSE, TK_ENUM,
  TK_SELECT,
  TK_SCOPE, TK_SPAWN, TK_PARALLEL,
  TK_REDUCE, TK_LOCK, TK_TRUE, TK_FALSE, TK_NIL,

  TK_LPAREN, TK_RPAREN, TK_LBRACE, TK_RBRACE, TK_LBRACK, TK_RBRACK,
  TK_COMMA, TK_COLON, TK_DOT, TK_DOTDOT, TK_ELLIPSIS, TK_TILDE, TK_RECV,
  TK_QUESTION,

  TK_DEFINE, // :=
  TK_ASSIGN, TK_ADD_ASSIGN, TK_SUB_ASSIGN, TK_MUL_ASSIGN, TK_DIV_ASSIGN,
  TK_MOD_ASSIGN,
  TK_ADD_WRAP_ASSIGN, TK_SUB_WRAP_ASSIGN, TK_MUL_WRAP_ASSIGN,
  TK_AND_ASSIGN, TK_OR_ASSIGN, TK_XOR_ASSIGN,
  TK_SHL_ASSIGN, TK_SHR_ASSIGN,

  TK_PLUS, TK_MINUS, TK_STAR, TK_SLASH, TK_PERCENT,
  TK_PLUS_WRAP, TK_MINUS_WRAP, TK_STAR_WRAP, // +% -% *%, wrapping arithmetic

  TK_EQ, TK_NE, TK_LT, TK_LE, TK_GT, TK_GE,
  TK_ANDAND, TK_OROR, TK_BANG,
  TK_AMP, TK_PIPE, TK_CARET, TK_SHL, TK_SHR,
};

struct Token {
  TokKind kind = TK_EOF;
  Pos pos;
  int len = 0; // bytes of source, for editors that need to paint a span
  std::string text; // identifier spelling or string value
  uint64_t ival = 0;
  double fval = 0;
};

std::vector<Token> lex(int file);
const char *tok_name(TokKind kind);
