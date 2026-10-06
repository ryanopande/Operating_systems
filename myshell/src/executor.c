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


static int redirect_fd(const char *path, int flags, int target_fd)
{
    int fd = open(path, flags, 0644);
    if (fd < 0) {
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
    close(fd);
    return 0;
}

static int apply_redirections(const Command *cmd)
{
    if (cmd->in_file &&
        redirect_fd(cmd->in_file, O_RDONLY, STDIN_FILENO) < 0)
        return -1;

    if (cmd->out_file) {
        int flags = O_WRONLY | O_CREAT | (cmd->out_append ? O_APPEND : O_TRUNC);
        if (redirect_fd(cmd->out_file, flags, STDOUT_FILENO) < 0)
            return -1;
    }

    if (cmd->err_file) {
        int flags = O_WRONLY | O_CREAT | (cmd->err_append ? O_APPEND : O_TRUNC);
        if (redirect_fd(cmd->err_file, flags, STDERR_FILENO) < 0)
            return -1;
    }

    return 0;
}


static void run_child(const Command *cmd, int in_fd, int out_fd, int in_pipeline)
{
    signal(SIGINT, SIG_DFL);

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

    if (errno == ENOENT) {
        shell_error(in_pipeline ? "Command not found in pipe sequence."
                                : "Command not found.");
        _exit(EXIT_NOT_FOUND);
    }
    shell_perror(cmd->argv[0]);
    _exit(EXIT_CANNOT_EXEC);
}

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
        return 128 + WTERMSIG(last_status);   
    return 0;
}

int execute_pipeline(const Pipeline *pl)
{
    pid_t pids[MAX_CMDS];
    int   nforked   = 0;
    int   prev_read = STDIN_FILENO;   
    int   failed    = 0;

    for (int i = 0; i < pl->number_of_commands && !failed; i++) {
        int is_last   = (i == pl->number_of_commands - 1);
        int pipefd[2] = { -1, -1 };

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
            if (!is_last)
                close(pipefd[0]);
            run_child(&pl->cmds[i], prev_read,
                      is_last ? STDOUT_FILENO : pipefd[1],
                      pl->number_of_commands > 1);
        }

        pids[nforked++] = pid;


        if (prev_read != STDIN_FILENO) {
            close(prev_read);
            prev_read = STDIN_FILENO;
        }

        if (!is_last) {
            close(pipefd[1]);
            prev_read = pipefd[0];
        }
    }

    if (prev_read != STDIN_FILENO)
        close(prev_read);

    int status = wait_for_children(pids, nforked);
    return failed ? -1 : status;
}
