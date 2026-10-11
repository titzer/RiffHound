#pragma once

#include <stdio.h>
#include <sys/types.h>
#include <string>
#include <vector>

// Run a program with an argument vector (no shell, so paths with quotes,
// `$` or backticks pass through untouched) and read its stdout.  stderr goes
// to `err_path` (appended) or /dev/null.  argv[0] is looked up on PATH when
// it has no slash.  Returns nullptr when the spawn fails.
struct Subprocess { FILE* out; pid_t pid; };
bool subprocess_open(Subprocess* p, const std::vector<std::string>& argv,
                     const char* err_path = nullptr);
// Close the pipe and reap the child.  Returns its exit code, -1 if it died
// on a signal or never started.
int  subprocess_close(Subprocess* p);

// argv for a user-supplied shell command (BM_*_CMD) given one more argument:
// `sh -c 'CMD "$1"' sh ARG`, so ARG is never parsed by the shell.
std::vector<std::string> subprocess_shell_argv(const char* cmd, const char* arg);
