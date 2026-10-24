#include "doc.h"

#include "diag.h"
#include "lexer.h"
#include "package.h"
#include "parser.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

bool is_dir(const std::string &path) {
  struct stat info;
  return stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

std::string trimmed(const std::string &text) {
  size_t start = text.find_first_not_of(" \t\r");
  if (start == std::string::npos) return "";
  size_t end = text.find_last_not_of(" \t\r");
  return text.substr(start, end - start + 1);
}

bool upper(const std::string &name) {
  return !name.empty() && isupper((unsigned char)name[0]);
}

// The comment written directly above a line, without its slashes: a blank line
// or code ends it, the way a reader would see where it stops belonging.
std::string doc_above(const Source &src, int line) {
  std::vector<std::string> said;
  for (int n = line - 1; n >= 1; n--) {
    std::string text = trimmed(src.line(n));
    if (text.compare(0, 2, "//") != 0) break;
    text = text.substr(2);
    if (!text.empty() && text[0] == ' ') text = text.substr(1);
    said.push_back(text);
  }
  std::reverse(said.begin(), said.end());
  std::string out;
  for (const std::string &s : said) out += s + "\n";
  return out;
}

// Braces in code, not in a string or a comment.
int brace_balance(const std::string &line) {
  int depth = 0;
  bool quoted = false;
  for (size_t i = 0; i < line.size(); i++) {
    char c = line[i];
    if (quoted) {
      if (c == '\\') i++;
      else if (c == '"') quoted = false;
      continue;
    }
    if (c == '"') quoted = true;
    else if (c == '/' && i + 1 < line.size() && line[i + 1] == '/') break;
    else if (c == '{') depth++;
    else if (c == '}') depth--;
  }
  return depth;
}

int paren_balance(const std::string &line) {
  int depth = 0;
  for (char c : line) depth += c == '(' ? 1 : c == ')' ? -1 : 0;
  return depth;
}

struct Item {
  Node *decl;
  const Source *src;
  std::string name;
  std::string receiver; // the type a method hangs off, empty otherwise
};

// What a declaration says, as it was written: a function up to its body, a
// type with its fields — the ones the package shows, anyway.
std::string written(const Item &item) {
  Node *decl = item.decl;
  const Source &src = *item.src;
  int first = decl->pos.line, lines = (int)src.line_start.size();
  std::string out;
  if (decl->kind == ND_FUNC) {
    if (decl->body) {
      for (int n = first; n < decl->body->pos.line; n++) out += src.line(n) + "\n";
      std::string last = src.line(decl->body->pos.line);
      return out + trimmed(last.substr(0, decl->body->pos.col - 1)) + "\n";
    }
    int depth = 0;
    for (int n = first; n <= lines; n++) {
      out += src.line(n) + "\n";
      depth += paren_balance(src.line(n));
      if (depth <= 0) break;
    }
    return out;
  }
  if (decl->kind != ND_STRUCT_DECL && decl->kind != ND_INTERFACE_DECL &&
      decl->kind != ND_ENUM_DECL)
    return src.line(first) + "\n";

  std::vector<std::string> body;
  int depth = 0;
  bool opened = false;
  for (int n = first; n <= lines; n++) {
    body.push_back(src.line(n));
    int change = brace_balance(src.line(n));
    if (change > 0 || src.line(n).find('{') != std::string::npos) opened = true;
    depth += change;
    if (opened && depth <= 0) break;
  }
  // A field or method only the package sees is left out, with the comment
  // above it, and a line says something was.
  bool hidden = false;
  std::vector<std::string> kept, pending;
  for (size_t i = 0; i < body.size(); i++) {
    std::string text = trimmed(body[i]);
    bool inner = i > 0 && i + 1 < body.size();
    if (inner && text.compare(0, 2, "//") == 0) {
      pending.push_back(body[i]);
      continue;
    }
    if (inner && decl->kind != ND_ENUM_DECL && !text.empty() && islower((unsigned char)text[0])) {
      hidden = true;
      pending.clear();
      continue;
    }
    kept.insert(kept.end(), pending.begin(), pending.end());
    pending.clear();
    if (i + 1 == body.size() && hidden && body.size() > 1)
      kept.push_back("    // and more that only the package sees");
    kept.push_back(body[i]);
  }
  for (const std::string &line : kept) out += line + "\n";
  return out;
}

// The first line alone, closed off: `struct Server { ... }`.
std::string headline(const Item &item) {
  std::string text = written(item);
  if (item.decl->kind == ND_FUNC) {
    std::string flat;
    for (char c : text) flat += c == '\n' ? ' ' : c;
    return trimmed(flat);
  }
  std::string first = trimmed(text.substr(0, text.find('\n')));
  if (text.find('\n') + 1 < text.size() && !first.empty() && first.back() == '{')
    first += " ... }";
  return first;
}

void print_doc(const std::string &doc) {
  size_t at = 0;
  while (at < doc.size()) {
    size_t end = doc.find('\n', at);
    std::string line = doc.substr(at, end - at);
    printf(line.empty() ? "\n" : "    %s\n", line.c_str());
    at = end + 1;
  }
}

// `(mut s *Server)` is a method of Server.
std::string receiver_of(const std::string &signature) {
  size_t open = signature.find('(');
  size_t func = signature.find("func");
  if (open == std::string::npos || func == std::string::npos || open < func)
    return "";
  if (trimmed(signature.substr(func + 4, open - func - 4)) != "") return "";
  size_t close = signature.find(')', open);
  std::string inside = signature.substr(open + 1, close - open - 1);
  size_t star = inside.find_last_of(" *");
  std::string type = trimmed(inside.substr(star == std::string::npos ? 0 : star + 1));
  size_t bracket = type.find('[');
  return bracket == std::string::npos ? type : type.substr(0, bracket);
}

struct Docs {
  std::string path, dir, name, about;
  std::vector<Item> items;

  bool load(Program &prog) {
    std::vector<std::string> files;
    DIR *handle = opendir(dir.c_str());
    if (!handle) return false;
    while (dirent *entry = readdir(handle)) {
      std::string file = entry->d_name;
      if (file.size() > 6 && file.compare(file.size() - 6, 6, ".sword") == 0 &&
          !(file.size() > 11 && file.compare(file.size() - 11, 11, "_test.sword") == 0))
        files.push_back(dir + "/" + file);
    }
    closedir(handle);
    std::sort(files.begin(), files.end());
    if (files.empty()) {
      fprintf(stderr, "shield: no .sword files in '%s'\n", dir.c_str());
      return false;
    }
    for (const std::string &file : files) {
      int id = load_source(file);
      if (id < 0) continue;
      std::vector<Token> tokens = lex(id);
      Node *unit = prog.ast.make(ND_UNIT, Pos{});
      if (error_count() > 0 || !parse(tokens, prog.ast, unit)) return false;
      const Source &src = source_at(id);
      for (Node *decl : unit->kids) {
        if (decl->kind == ND_PACKAGE) {
          name = decl->name;
          if (about.empty()) about = doc_above(src, decl->pos.line);
          continue;
        }
        Item item{decl, &src, decl->name, ""};
        if (decl->kind == ND_ERROR_DECL) {
          for (Node *one : decl->kids)
            if (upper(one->name)) item.name = one->name;
          if (!upper(item.name)) continue;
        } else if (decl->kind == ND_IMPORT || !upper(decl->name)) {
          continue;
        }
        if (decl->kind == ND_FUNC && decl->lhs) {
          item.receiver = receiver_of(src.line(decl->pos.line));
          if (!upper(item.receiver)) continue;
        }
        items.push_back(item);
      }
    }
    return true;
  }

  std::vector<const Item *> methods_of(const std::string &type) const {
    std::vector<const Item *> out;
    for (const Item &item : items)
      if (item.receiver == type) out.push_back(&item);
    return out;
  }

  void show_all() const {
    printf("package %s // import \"%s\"\n", name.c_str(), path.c_str());
    if (!about.empty()) {
      printf("\n");
      print_doc(about);
    }
    // Errors and constants first, then each type with its methods, then the
    // functions: the order a reader looks things up in.
    static const NodeKind order[] = {ND_ERROR_DECL, ND_CONST_DECL, ND_TYPE_ALIAS,
                                     ND_ENUM_DECL,  ND_STRUCT_DECL,
                                     ND_INTERFACE_DECL, ND_FUNC};
    for (NodeKind kind : order) {
      bool any = false;
      for (const Item &item : items) {
        if (item.decl->kind != kind || !item.receiver.empty()) continue;
        if (!any) printf("\n");
        any = true;
        printf("%s\n", headline(item).c_str());
        for (const Item *m : methods_of(item.name))
          printf("    %s\n", headline(*m).c_str());
      }
    }
  }

  void show(const Item &item) const {
    printf("package %s // import \"%s\"\n\n", name.c_str(), path.c_str());
    printf("%s", written(item).c_str());
    print_doc(doc_above(*item.src, item.decl->pos.line));
    bool type = item.decl->kind != ND_FUNC && item.decl->kind != ND_CONST_DECL &&
                item.decl->kind != ND_ERROR_DECL;
    if (!type) return;
    for (const Item *m : methods_of(item.name)) {
      printf("\n%s", written(*m).c_str());
      print_doc(doc_above(*m->src, m->decl->pos.line));
    }
  }
};

} // namespace

int doc_command(const std::vector<std::string> &args,
                const std::vector<std::string> &search) {
  if (args.size() != 1 || args[0].empty() || args[0][0] == '-') {
    fprintf(stderr, "usage: shield doc <package>[.<name>[.<method>]]\n"
                    "       shield doc std/http.Server\n");
    return 1;
  }
  const std::string &target = args[0];
  char cwd[4096];
  std::string here = getcwd(cwd, sizeof(cwd)) ? cwd : ".";

  // A path names the package and whatever follows the first dot of its last
  // element names something in it: the host of `example.com/lib.Server` keeps
  // its dot.
  Docs docs;
  std::string what;
  if (target == "." || target.compare(0, 2, "./") == 0 || target[0] == '/') {
    size_t dot = target.find('.', target.find_last_of('/') + 1);
    docs.dir = target == "." ? target : target.substr(0, dot);
    if (target != "." && dot != std::string::npos) what = target.substr(dot + 1);
    docs.path = docs.dir;
  } else {
    size_t slash = target.find_last_of('/');
    size_t dot = target.find('.', slash == std::string::npos ? 0 : slash + 1);
    docs.path = target.substr(0, dot);
    if (dot != std::string::npos) what = target.substr(dot + 1);
    docs.dir = find_package(docs.path, here, search);
    // `shield doc mem` is the one in std when there is no other.
    if (docs.dir.empty() && slash == std::string::npos) {
      docs.dir = find_package("std/" + docs.path, here, search);
      if (!docs.dir.empty()) docs.path = "std/" + docs.path;
    }
  }
  if (docs.dir.empty() || !is_dir(docs.dir)) {
    fprintf(stderr, "shield: cannot find package '%s'\n", docs.path.c_str());
    return 1;
  }
  Program prog;
  if (!docs.load(prog)) return 1;
  if (what.empty()) {
    docs.show_all();
    return 0;
  }

  size_t dot = what.find('.');
  std::string type = dot == std::string::npos ? what : what.substr(0, dot);
  std::string method = dot == std::string::npos ? "" : what.substr(dot + 1);
  for (const Item &item : docs.items) {
    bool found = method.empty() ? item.name == type && item.receiver.empty()
                                : item.name == method && item.receiver == type;
    if (!found) continue;
    docs.show(item);
    return 0;
  }
  fprintf(stderr, "shield: %s has nothing exported called '%s'\n",
          docs.path.c_str(), what.c_str());
  return 1;
}
