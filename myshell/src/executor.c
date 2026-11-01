#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>
#include "errors.h"
#include "executor.h"
#include "shell.h"


// open `path` with the given flags and make it replace descriptor `target_fd` (0, 1 or 2).
// returns 0 on success and -1 on failure after printing the error message.
static int redirect_fd(const char *path, int flags, int target_fd)
{
    int fd = open(path, flags, 0644);
    if (fd < 0) {
        // a missing file is only a "File not found." error when we were reading it;
        // for output files ENOENT means a bad directory, so report the system message
        if (errno == ENOENT && !(flags & O_CREAT))
            shell_error("File not found.");
        else
            shell_perror(path);
        return -1;
    }

    if (dup2(fd, target_fd) < 0) {
        shell_perror("dup2");
        close(fd);
        return -1;
    }
    close(fd);   // the duplicate on target_fd stays open, so the original is not needed
    return 0;
}

// map a redirection operator to the open() flags and the descriptor it replaces.
static void redirect_target(RedirType type, int *flags, int *target_fd)
{
    switch (type) {
    case REDIR_IN:
        *flags     = O_RDONLY;
        *target_fd = STDIN_FILENO;
        break;
    case REDIR_OUT:
        *flags     = O_WRONLY | O_CREAT | O_TRUNC;
        *target_fd = STDOUT_FILENO;
        break;
    case REDIR_OUT_APPEND:
        *flags     = O_WRONLY | O_CREAT | O_APPEND;
        *target_fd = STDOUT_FILENO;
        break;
    case REDIR_ERR:
        *flags     = O_WRONLY | O_CREAT | O_TRUNC;
        *target_fd = STDERR_FILENO;
        break;
    default: // REDIR_ERR_APPEND
        *flags     = O_WRONLY | O_CREAT | O_APPEND;
        *target_fd = STDERR_FILENO;
        break;
    }
}

// apply the redirections of one command in the order they were typed, like a real
// shell: each target is opened (and created or truncated) one after another, and the
// first failure stops everything so the program never runs. because stderr may already
// point at a file by the time a later redirection fails, the error message lands in
// that file instead of the terminal ("cat 2> err.txt < missing.txt" is silent on screen
// and err.txt receives "File not found.").
static int apply_redirections(const Command *cmd)
{
    for (int i = 0; i < cmd->number_of_redirs; i++) {
        const Redirect *redir = &cmd->redirs[i];
        int flags, target_fd;

        redirect_target(redir->type, &flags, &target_fd);
        if (redirect_fd(redir->file, flags, target_fd) < 0)
            return -1;
    }

    return 0;
}


// runs inside the forked child and never returns: it connects the pipe ends, applies the
// command's own redirections and then replaces itself with the program using execvp().
// in_fd / out_fd are the pipe ends for this command (or stdin / stdout when there is none).
// in_pipeline only selects which "command not found" message to print.
static void run_child(const Command *cmd, int in_fd, int out_fd, int in_pipeline)
{
    // restore default signal handling: the parent may ignore SIGINT (the shell) or
    // SIGPIPE (the server), and ignored signals survive exec. without this "yes | head"
    // would print "Broken pipe" errors instead of ending quietly.
    signal(SIGINT, SIG_DFL);
    signal(SIGPIPE, SIG_DFL);

    // pipe ends first, then the file redirections, so "cmd > file | other" sends the
    // output to the file and the next command sees an empty pipe, as in a real shell
    if (in_fd != STDIN_FILENO) {
        dup2(in_fd, STDIN_FILENO);
        close(in_fd);
    }
    if (out_fd != STDOUT_FILENO) {
        dup2(out_fd, STDOUT_FILENO);
        close(out_fd);
    }

    if (apply_redirections(cmd) < 0)
        _exit(EXIT_REDIRECT_FAILED);

    execvp(cmd->argv[0], cmd->argv);

    // execvp only returns when it failed
    if (errno == ENOENT) {
        if (in_pipeline)
            shell_error("Command not found in pipe sequence.");
        else
            shell_error_arg("Command not found", cmd->argv[0]);
        _exit(EXIT_NOT_FOUND);
    }
    shell_perror(cmd->argv[0]);
    _exit(EXIT_CANNOT_EXEC);
}

// wait for every child of the pipeline so the next prompt only appears after all of them
// finished. returns the exit status of the last command, like a real shell.
static int wait_for_children(const pid_t *pids, int count)
{
    int last_status = 0;

    for (int i = 0; i < count; i++) {
        int status;
        if (waitpid(pids[i], &status, 0) < 0) {
            shell_perror("waitpid");
            continue;
        }
        if (i == count - 1)
            last_status = status;
    }

    if (WIFEXITED(last_status))
        return WEXITSTATUS(last_status);
    if (WIFSIGNALED(last_status))
        return 128 + WTERMSIG(last_status);   // killed by a signal: shell convention
    return 0;
}

// run all commands of a pipeline. every command gets its own child process; the read end
// of the pipe from the previous command becomes its stdin and a new pipe carries its
// stdout to the next one. returns the last command's exit status, or -1 if a fork or
// pipe call failed (children already started are still waited for).
int execute_pipeline(const Pipeline *pl)
{
    pid_t pids[MAX_CMDS];
    int   nforked   = 0;
    int   prev_read = STDIN_FILENO;   // read end of the previous pipe (stdin for the first)
    int   failed    = 0;

    for (int i = 0; i < pl->number_of_commands && !failed; i++) {
        int is_last   = (i == pl->number_of_commands - 1);
        int pipefd[2] = { -1, -1 };

        // every command except the last needs a pipe to the next one
        if (!is_last && pipe(pipefd) < 0) {
            shell_perror("pipe");
            failed = 1;
            break;
        }

        pid_t pid = fork();
        if (pid < 0) {
            shell_perror("fork");
            if (!is_last) {
                close(pipefd[0]);
                close(pipefd[1]);
            }
            failed = 1;
            break;
        }

        if (pid == 0) {
            // child: close the read end it does not use, then become the program
            if (!is_last)
                close(pipefd[0]);
            run_child(&pl->cmds[i], prev_read,
                      is_last ? STDOUT_FILENO : pipefd[1],
                      pl->number_of_commands > 1);
        }

        pids[nforked++] = pid;

        // parent: close the pipe ends the child now owns. a reader only sees end-of-file
        // once every copy of the write end is closed, so leaving one open would hang.
        if (prev_read != STDIN_FILENO) {
            close(prev_read);
            prev_read = STDIN_FILENO;
        }

        if (!is_last) {
            close(pipefd[1]);
            prev_read = pipefd[0];
        }
    }

    // if we stopped early, the last pipe's read end is still open
    if (prev_read != STDIN_FILENO)
        close(prev_read);

    int status = wait_for_children(pids, nforked);
    return failed ? -1 : status;
}
