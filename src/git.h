#pragma once

#include <string>
#include <vector>

struct Ran {
  int status = -1; // 127 when there is no git to run
  std::string out, err;
};

// git with its output collected, and never a prompt: a build that stops to
// ask for a password looks exactly like one that hung.
Ran git(const std::vector<std::string> &args);

// What git said about a failure, indented under the line that says what failed.
void explain(const Ran &ran);
