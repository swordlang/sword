#pragma once

#include <string>
#include <vector>

// `shield doc std/http`, `shield doc std/http.Server`, `shield doc
// std/http.Server.Close`: what a package exports, as it was written, with the
// comments above it.
int doc_command(const std::vector<std::string> &args,
                const std::vector<std::string> &search);
