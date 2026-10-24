#include "lexer.h"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

namespace {

const std::unordered_map<std::string, TokKind> keywords = {
    {"package", TK_PACKAGE},   {"import", TK_IMPORT},
    {"func", TK_FUNC},         {"return", TK_RETURN},
    {"if", TK_IF},             {"else", TK_ELSE},
    {"for", TK_FOR},           {"in", TK_IN},
    {"switch", TK_SWITCH},     {"case", TK_CASE},
    {"select", TK_SELECT},
    {"default", TK_DEFAULT},
    {"break", TK_BREAK},       {"continue", TK_CONTINUE},
    {"mut", TK_MUT},           {"const", TK_CONST},
    {"struct", TK_STRUCT},     {"interface", TK_INTERFACE},
    {"enum", TK_ENUM},
    {"extern", TK_EXTERN},     {"det", TK_DET},
    {"defer", TK_DEFER},
    {"errdefer", TK_ERRDEFER},
    {"try", TK_TRY},           {"catch", TK_CATCH},
    {"orelse", TK_ORELSE},
    {"scope", TK_SCOPE},       {"spawn", TK_SPAWN},
    {"parallel", TK_PARALLEL}, {"reduce", TK_REDUCE},
    {"lock", TK_LOCK},
    {"true", TK_TRUE},         {"false", TK_FALSE},
    {"nil", TK_NIL},
};

// A newline ends a statement when the token before it could be the last one of
// a statement. Same rule as Go, which is why Sword source needs no semicolons.
bool ends_statement(TokKind kind) {
  switch (kind) {
  case TK_IDENT: case TK_INT: case TK_FLOAT: case TK_STRING:
  case TK_RPAREN: case TK_RBRACK: case TK_RBRACE:
  case TK_RETURN: case TK_BREAK: case TK_CONTINUE:
  case TK_TRUE: case TK_FALSE: case TK_NIL:
    return true;
  default:
    return false;
  }
}

int hex_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

struct Lexer {
  const Source &src;
  size_t i = 0;
  Pos pos;
  std::vector<Token> out;

  explicit Lexer(int file) : src(source_at(file)) { pos.file = file; }

  char peek(size_t ahead = 0) const {
    size_t at = i + ahead;
    return at < src.text.size() ? src.text[at] : '\0';
  }

  char advance() {
    char c = src.text[i++];
    if (c == '\n') {
      pos.line++;
      pos.col = 1;
    } else {
      pos.col++;
    }
    return c;
  }

  bool match(char c) {
    if (peek() != c) return false;
    advance();
    return true;
  }

  void push(TokKind kind, Pos at) {
    Token t;
    t.kind = kind;
    t.pos = at;
    out.push_back(t);
  }

  void newline() {
    if (!out.empty() && ends_statement(out.back().kind)) push(TK_TERM, pos);
    advance();
  }

  void block_comment() {
    Pos start = pos;
    advance(); // '/'
    advance(); // '*'
    int depth = 1;
    while (depth > 0) {
      if (i >= src.text.size()) {
        error(start, "unterminated block comment");
        return;
      }
      if (peek() == '/' && peek(1) == '*') {
        advance(); advance(); depth++;
      } else if (peek() == '*' && peek(1) == '/') {
        advance(); advance(); depth--;
      } else {
        advance();
      }
    }
  }

  void number() {
    Pos start = pos;
    std::string digits;
    int base = 10;

    if (peek() == '0' && (peek(1) == 'x' || peek(1) == 'b' || peek(1) == 'o')) {
      advance();
      char tag = advance();
      base = tag == 'x' ? 16 : tag == 'b' ? 2 : 8;
    }

    auto is_digit = [&](char c) {
      if (base == 16) return isxdigit((unsigned char)c) != 0;
      if (base == 2) return c == '0' || c == '1';
      if (base == 8) return c >= '0' && c <= '7';
      return isdigit((unsigned char)c) != 0;
    };

    while (is_digit(peek()) || peek() == '_') {
      char c = advance();
      if (c != '_') digits += c;
    }

    // A '.' only starts a fraction if it is not the range operator: in
    // `0..n` the dots belong to the range, not to the number.
    bool is_float = false;
    if (base == 10 && peek() == '.' && peek(1) != '.') {
      is_float = true;
      digits += advance();
      while (isdigit((unsigned char)peek()) || peek() == '_') {
        char c = advance();
        if (c != '_') digits += c;
      }
    }
    if (base == 10 && (peek() == 'e' || peek() == 'E')) {
      is_float = true;
      digits += advance();
      if (peek() == '+' || peek() == '-') digits += advance();
      while (isdigit((unsigned char)peek())) digits += advance();
    }

    if (digits.empty()) {
      error(start, "malformed numeric literal");
      return;
    }

    Token t;
    t.pos = start;
    if (is_float) {
      t.kind = TK_FLOAT;
      t.fval = strtod(digits.c_str(), nullptr);
    } else {
      t.kind = TK_INT;
      t.ival = strtoull(digits.c_str(), nullptr, base);
    }
    out.push_back(t);
  }

  void string_literal() {
    Pos start = pos;
    advance(); // opening quote
    std::string value;
    while (peek() != '"') {
      if (i >= src.text.size() || peek() == '\n') {
        error(start, "unterminated string literal");
        return;
      }
      char c = advance();
      if (c != '\\') {
        value += c;
        continue;
      }
      char esc = advance();
      switch (esc) {
      case 'n': value += '\n'; break;
      case 't': value += '\t'; break;
      case 'r': value += '\r'; break;
      case '0': value += '\0'; break;
      // A byte by its value, and ESC, which every terminal sequence begins
      // with. Without these a program that draws a screen cannot hold one of
      // those sequences in a constant at all.
      case 'e': value += '\x1b'; break;
      case 'x': {
        int hi = hex_value(peek()), lo = hex_value(peek(1));
        if (hi < 0 || lo < 0) {
          // Reported and stepped over: giving up on the literal here would
          // leave the rest of it to be read as code, and the second complaint
          // would be about the closing quote.
          error(pos, "'\\x' takes two hex digits, as in '\\x1b'");
          break;
        }
        advance();
        advance();
        value += (char)(hi * 16 + lo);
        break;
      }
      case '\\': case '"': case '\'': value += esc; break;
      default: error(pos, "unknown escape '\\%c'", esc); break;
      }
    }
    advance(); // closing quote

    Token t;
    t.kind = TK_STRING;
    t.pos = start;
    t.text = value;
    out.push_back(t);
  }

  void word() {
    Pos start = pos;
    std::string text;
    while (isalnum((unsigned char)peek()) || peek() == '_') text += advance();

    Token t;
    t.pos = start;
    auto found = keywords.find(text);
    t.kind = found == keywords.end() ? TK_IDENT : found->second;
    t.text = text;
    out.push_back(t);
  }

  void punct() {
    Pos start = pos;
    char c = advance();
    TokKind kind;

    switch (c) {
    case '(': kind = TK_LPAREN; break;
    case ')': kind = TK_RPAREN; break;
    case '{': kind = TK_LBRACE; break;
    case '}': kind = TK_RBRACE; break;
    case '[': kind = TK_LBRACK; break;
    case ']': kind = TK_RBRACK; break;
    case ',': kind = TK_COMMA; break;
    case ';': kind = TK_TERM; break;
    case '?': kind = TK_QUESTION; break;
    case '~': kind = TK_TILDE; break;
    case ':': kind = match('=') ? TK_DEFINE : TK_COLON; break;
    case '.':
      kind = !match('.') ? TK_DOT : match('.') ? TK_ELLIPSIS : TK_DOTDOT;
      break;
    case '=': kind = match('=') ? TK_EQ : TK_ASSIGN; break;
    case '!': kind = match('=') ? TK_NE : TK_BANG; break;
    case '&':
      kind = match('&') ? TK_ANDAND : match('=') ? TK_AND_ASSIGN : TK_AMP;
      break;
    case '|':
      kind = match('|') ? TK_OROR : match('=') ? TK_OR_ASSIGN : TK_PIPE;
      break;
    case '^': kind = match('=') ? TK_XOR_ASSIGN : TK_CARET; break;
    case '%': kind = match('=') ? TK_MOD_ASSIGN : TK_PERCENT; break;
    case '+':
      kind = match('=')   ? TK_ADD_ASSIGN
             : !match('%') ? TK_PLUS
             : match('=')  ? TK_ADD_WRAP_ASSIGN
                           : TK_PLUS_WRAP;
      break;
    case '-':
      kind = match('=')   ? TK_SUB_ASSIGN
             : !match('%') ? TK_MINUS
             : match('=')  ? TK_SUB_WRAP_ASSIGN
                           : TK_MINUS_WRAP;
      break;
    case '*':
      kind = match('=')   ? TK_MUL_ASSIGN
             : !match('%') ? TK_STAR
             : match('=')  ? TK_MUL_WRAP_ASSIGN
                           : TK_STAR_WRAP;
      break;
    case '/': kind = match('=') ? TK_DIV_ASSIGN : TK_SLASH; break;
    case '<':
      // `<-` is one token, so `a < -b` needs the space it reads better with —
      // the same rule Go has, and the same reason.
      kind = match('=')   ? TK_LE
             : match('-') ? TK_RECV
             : !match('<') ? TK_LT
             : match('=')  ? TK_SHL_ASSIGN
                           : TK_SHL;
      break;
    case '>':
      kind = match('=')   ? TK_GE
             : !match('>') ? TK_GT
             : match('=')  ? TK_SHR_ASSIGN
                           : TK_SHR;
      break;
    default:
      error(start, "unexpected character '%c'", c);
      return;
    }
    push(kind, start);
  }

  std::vector<Token> run() {
    while (i < src.text.size()) {
      size_t start = i;
      size_t before = out.size();
      char c = peek();
      if (c == '\n') {
        newline();
      } else if (c == ' ' || c == '\t' || c == '\r') {
        advance();
      } else if (c == '/' && peek(1) == '/') {
        while (i < src.text.size() && peek() != '\n') advance();
      } else if (c == '/' && peek(1) == '*') {
        block_comment();
      } else if (isdigit((unsigned char)c)) {
        number();
      } else if (c == '"') {
        string_literal();
      } else if (isalpha((unsigned char)c) || c == '_') {
        word();
      } else {
        punct();
      }
      // Whatever the branch produced, its span is what was just consumed.
      if (out.size() > before) out.back().len = (int)(i - start);
    }
    if (!out.empty() && ends_statement(out.back().kind)) push(TK_TERM, pos);
    push(TK_EOF, pos);
    return out;
  }
};

} // namespace

std::vector<Token> lex(int file) { return Lexer(file).run(); }

const char *tok_name(TokKind kind) {
  switch (kind) {
  case TK_EOF: return "end of file";
  case TK_TERM: return "end of statement";
  case TK_IDENT: return "identifier";
  case TK_INT: return "integer";
  case TK_FLOAT: return "float";
  case TK_STRING: return "string";
  case TK_PACKAGE: return "package";
  case TK_IMPORT: return "import";
  case TK_FUNC: return "func";
  case TK_RETURN: return "return";
  case TK_IF: return "if";
  case TK_ELSE: return "else";
  case TK_FOR: return "for";
  case TK_IN: return "in";
  case TK_BREAK: return "break";
  case TK_CONTINUE: return "continue";
  case TK_MUT: return "mut";
  case TK_CONST: return "const";
  case TK_STRUCT: return "struct";
  case TK_INTERFACE: return "interface";
  case TK_EXTERN: return "extern";
  case TK_DET: return "det";
  case TK_DEFER: return "defer";
  case TK_ERRDEFER: return "errdefer";
  case TK_TRY: return "try";
  case TK_CATCH: return "catch";
  case TK_ORELSE: return "orelse";
  case TK_SCOPE: return "scope";
  case TK_SPAWN: return "spawn";
  case TK_PARALLEL: return "parallel";
  case TK_REDUCE: return "reduce";
  case TK_LOCK: return "lock";
  case TK_ENUM: return "enum";
  case TK_SELECT: return "select";
  case TK_SWITCH: return "switch";
  case TK_CASE: return "case";
  case TK_DEFAULT: return "default";
  case TK_TRUE: return "true";
  case TK_FALSE: return "false";
  case TK_NIL: return "nil";
  case TK_LPAREN: return "(";
  case TK_RPAREN: return ")";
  case TK_LBRACE: return "{";
  case TK_RBRACE: return "}";
  case TK_LBRACK: return "[";
  case TK_RBRACK: return "]";
  case TK_COMMA: return ",";
  case TK_COLON: return ":";
  case TK_DOT: return ".";
  case TK_DOTDOT: return "..";
  case TK_ELLIPSIS: return "...";
  case TK_TILDE: return "~";
  case TK_RECV: return "<-";
  case TK_QUESTION: return "?";
  case TK_DEFINE: return ":=";
  case TK_ASSIGN: return "=";
  case TK_ADD_ASSIGN: return "+=";
  case TK_SUB_ASSIGN: return "-=";
  case TK_MUL_ASSIGN: return "*=";
  case TK_DIV_ASSIGN: return "/=";
  case TK_MOD_ASSIGN: return "%=";
  case TK_ADD_WRAP_ASSIGN: return "+%=";
  case TK_SUB_WRAP_ASSIGN: return "-%=";
  case TK_MUL_WRAP_ASSIGN: return "*%=";
  case TK_AND_ASSIGN: return "&=";
  case TK_OR_ASSIGN: return "|=";
  case TK_XOR_ASSIGN: return "^=";
  case TK_SHL_ASSIGN: return "<<=";
  case TK_SHR_ASSIGN: return ">>=";
  case TK_PLUS: return "+";
  case TK_MINUS: return "-";
  case TK_STAR: return "*";
  case TK_SLASH: return "/";
  case TK_PERCENT: return "%";
  case TK_PLUS_WRAP: return "+%";
  case TK_MINUS_WRAP: return "-%";
  case TK_STAR_WRAP: return "*%";
  case TK_EQ: return "==";
  case TK_NE: return "!=";
  case TK_LT: return "<";
  case TK_LE: return "<=";
  case TK_GT: return ">";
  case TK_GE: return ">=";
  case TK_ANDAND: return "&&";
  case TK_OROR: return "||";
  case TK_BANG: return "!";
  case TK_AMP: return "&";
  case TK_PIPE: return "|";
  case TK_CARET: return "^";
  case TK_SHL: return "<<";
  case TK_SHR: return ">>";
  }
  return "?";
}
