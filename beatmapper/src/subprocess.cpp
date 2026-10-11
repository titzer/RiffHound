#include "subprocess.h"

#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

bool subprocess_open(Subprocess* p, const std::vector<std::string>& args, const char* err_path) {
    p->out = nullptr; p->pid = 0;
    if (args.empty()) return false;
    std::vector<char*> argv;
    for (const std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);

    int fds[2];
    if (pipe(fds) != 0) return false;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&fa, fds[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&fa, fds[0]);
    posix_spawn_file_actions_addclose(&fa, fds[1]);
    if (err_path) posix_spawn_file_actions_addopen(&fa, STDERR_FILENO, err_path,
                                                   O_WRONLY | O_CREAT | O_APPEND, 0644);
    else          posix_spawn_file_actions_addopen(&fa, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    pid_t pid = 0;
    int rc = posix_spawnp(&pid, argv[0], &fa, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    close(fds[1]);
    if (rc != 0) { close(fds[0]); return false; }
    p->out = fdopen(fds[0], "r");
    if (!p->out) close(fds[0]);
    p->pid = pid;
    return true;
}

int subprocess_close(Subprocess* p) {
    if (p->out) { fclose(p->out); p->out = nullptr; }
    if (p->pid <= 0) return -1;
    int st = 0;
    pid_t r;
    do r = waitpid(p->pid, &st, 0); while (r < 0 && errno == EINTR);
    p->pid = 0;
    if (r < 0) return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

std::vector<std::string> subprocess_shell_argv(const char* cmd, const char* arg) {
    return { "/bin/sh", "-c", std::string(cmd) + " \"$1\"", "sh", arg };
}
