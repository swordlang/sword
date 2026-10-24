#pragma once

#include <string>

// SHA-256 of `data`, in lower-case hex. The compiler has its own because
// OpenSSL is optional, and sword.sum has to be checked either way.
std::string sha256_hex(const std::string &data);
