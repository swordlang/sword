#pragma once

#include <string>
#include <vector>

// Everything that decides what follows a program's own object on the link line.
// The source says which libraries (`extern "z"`); the command line says how they
// are linked, because the same package is built both ways depending on where it
// is going.
struct LinkRequest {
  std::vector<std::string> libraries;    // `extern "z"`, first mention first
  std::vector<std::string> weak_symbols; // `extern weak`
  std::string runtime_flags;             // the line written beside the archive
  std::vector<std::string> user;         // --link, in the order given
  bool prefer_static = false;            // --static
  bool tls = true;                       // the program reaches std/tls
};

// One argument per element, to be quoted on the way to the shell. False, with
// the reason printed, when --static asks for an archive that is not there.
bool link_arguments(const LinkRequest &req, std::vector<std::string> &out);
