#include "git.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

Ran git(const std::vector<std::string> &args) {
  Ran ran;
  int out[2], err[2];
  if (pipe(out) != 0) return ran;
  if (pipe(err) != 0) {
    close(out[0]);
    close(out[1]);
    return ran;
  }
  pid_t child = fork();
  if (child < 0) return ran;
  if (child == 0) {
    setenv("GIT_TERMINAL_PROMPT", "0", 1);
    setenv("TZ", "UTC0", 1);
    dup2(out[1], 1);
    dup2(err[1], 2);
    close(out[0]);
    close(out[1]);
    close(err[0]);
    close(err[1]);
    std::vector<char *> argv{const_cast<char *>("git")};
    for (const std::string &a : args) argv.push_back(const_cast<char *>(a.c_str()));
    argv.push_back(nullptr);
    execvp("git", argv.data());
    _exit(127);
  }
  close(out[1]);
  close(err[1]);
  pollfd watch[2] = {{out[0], POLLIN, 0}, {err[0], POLLIN, 0}};
  int open = 2;
  char buffer[4096];
  while (open > 0) {
    if (poll(watch, 2, -1) < 0) {
      if (errno == EINTR) continue;
      break;
    }
    for (int i = 0; i < 2; i++) {
      if (watch[i].fd < 0 || !(watch[i].revents & (POLLIN | POLLHUP))) continue;
      ssize_t n = read(watch[i].fd, buffer, sizeof(buffer));
      if (n > 0) {
        (i ? ran.err : ran.out).append(buffer, (size_t)n);
        continue;
      }
      close(watch[i].fd);
      watch[i].fd = -1;
      open--;
    }
  }
  int status = 0;
  waitpid(child, &status, 0);
  ran.status = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  return ran;
}

void explain(const Ran &ran) {
  if (ran.status == 127) {
    fprintf(stderr, "     this needs git, and there is none on the PATH\n");
    return;
  }
  size_t at = 0;
  while (at < ran.err.size()) {
    size_t end = ran.err.find('\n', at);
    if (end == std::string::npos) end = ran.err.size();
    if (end > at) fprintf(stderr, "     %s\n", ran.err.substr(at, end - at).c_str());
    at = end + 1;
  }
}

