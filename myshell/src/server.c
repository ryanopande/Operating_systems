// server.c — Phase 3 server: accepts many clients over TCP at the same time, one thread per
// client. each thread runs that client's commands with the Phase 1 shell engine (parser +
// executor) and sends the output back. "exit" or a disconnect ends only that client.


#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "builtins.h"
#include "errors.h"
#include "executor.h"
#include "net.h"
#include "parser.h"
#include "shell.h"

// text appended to a reply that had to be cut at MAX_MESSAGE bytes
#define TRUNCATION_NOTICE "\n[output truncated]\n"

// one per connected client: allocated by main, freed by the client's own thread
typedef struct ClientInfo
{
    int  fd;
    int  id;
    char ip[INET_ADDRSTRLEN];
    int  port;
} ClientInfo;

// ---------------------------------------------------------------------------
// logging

// held while printing, so the text of two clients never mixes inside one line
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;

// print one "[TAG] [Client #N - ip:port] message" line and flush it right away so the log
// stays in step with what happens even when the server's output is piped into a file.
// ci is NULL for lines about the server itself. a message ending in "\n" leaves a blank line.
static void log_line(const ClientInfo *ci, const char *tag, const char *format, ...)
{
    va_list args;

    pthread_mutex_lock(&log_lock);
    printf("[%s] ", tag);
    if (ci != NULL)
        printf("[Client #%d - %s:%d] ", ci->id, ci->ip, ci->port);
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    putchar('\n');
    fflush(stdout);
    pthread_mutex_unlock(&log_lock);
}

// ---------------------------------------------------------------------------
// output capture

// what the runner process tells the server through its exit status
enum
{
    RUN_OK           = 0,
    RUN_FAILED       = 1,
    RUN_PARSE_ERROR  = 2,
    RUN_START_FAILED = 3,
    RUN_EXIT         = 4,
    RUN_EMPTY        = 5
};

// body of the forked runner process, it never returns. it points stdout and stderr at the
// temporary file and stdin at /dev/null. a file is used instead of a pipe on purpose: a
// pipe holds only ~64 KiB, so a command printing more than that would block forever while
// the server waits for it to finish. stdin goes to /dev/null so a command such as a bare
// "cat" ends immediately instead of waiting for input on the server's terminal.
// a runner is needed because all threads share the server's descriptors.
static void run_in_child(const char *line, FILE *capture)
{
    Pipeline pipeline;

    int null_fd = open("/dev/null", O_RDONLY);
    if (null_fd >= 0) {
        dup2(null_fd, STDIN_FILENO);
        close(null_fd);
    }
    dup2(fileno(capture), STDOUT_FILENO);
    dup2(fileno(capture), STDERR_FILENO);

    // parse errors are printed by parse_line() on stderr, so parsing happens inside
    // the capture too: the client then receives them exactly like command output.
    int parsed = parse_line(line, &pipeline);
    if (parsed == PARSE_ERROR)
        _exit(RUN_PARSE_ERROR);
    if (parsed == PARSE_EMPTY)
        _exit(RUN_EMPTY);
    if (is_exit_command(&pipeline))
        _exit(RUN_EXIT);

    int status = execute_pipeline(&pipeline);
    if (status < 0)
        _exit(RUN_START_FAILED);
    _exit(status != 0 ? RUN_FAILED : RUN_OK);
}

// return everything captured as a malloc'd, NUL-terminated string (*len is its length)
// and close the file. returns NULL if memory ran out.
static char *read_capture(FILE *file, size_t *len)
{
    // the child processes wrote through the shared file offset, so the end of the
    // file is also the size of the output.
    int  fd   = fileno(file);
    off_t end = lseek(fd, 0, SEEK_END);
    size_t size = end < 0 ? 0 : (size_t)end;
    int truncated = size > MAX_MESSAGE;
    if (truncated)
        size = MAX_MESSAGE - strlen(TRUNCATION_NOTICE);

    char *out = malloc(size + strlen(TRUNCATION_NOTICE) + 1);
    if (out != NULL) {
        size_t got = 0;
        lseek(fd, 0, SEEK_SET);
        while (got < size) {
            ssize_t n = read(fd, out + got, size - got);
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0)
                break;
            got += (size_t)n;
        }
        if (truncated) {
            memcpy(out + got, TRUNCATION_NOTICE, strlen(TRUNCATION_NOTICE));
            got += strlen(TRUNCATION_NOTICE);
        }
        out[got] = '\0';
        *len = got;
    }

    fclose(file);
    return out;
}

// run `line` in a forked runner process and wait for it. returns the RUN_* code and the
// captured output, or -1 when that was not possible.
static int run_command(const char *line, char **output, size_t *len)
{
    FILE *capture = tmpfile();
    if (capture == NULL)
        return -1;

    pid_t pid = fork();
    if (pid < 0) {
        fclose(capture);
        return -1;
    }
    if (pid == 0)
        run_in_child(line, capture);

    // wait for our own runner only: waiting for any child could take another client's
    int wstatus;
    if (waitpid(pid, &wstatus, 0) < 0) {
        fclose(capture);
        return -1;
    }

    // a runner killed by a signal never reached _exit()
    int code = WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) : RUN_START_FAILED;

    *output = read_capture(capture, len);
    if (*output == NULL) {
        errno = ENOMEM;
        return -1;
    }
    return code;
}

// ---------------------------------------------------------------------------
// command handling

// copy `text` without its trailing newlines, used to show an error on a single log line.
static char *trimmed_copy(const char *text, size_t len)
{
    while (len > 0 && text[len - 1] == '\n')
        len--;

    char *copy = malloc(len + 1);
    if (copy != NULL) {
        memcpy(copy, text, len);
        copy[len] = '\0';
    }
    return copy;
}

// log a failed command the way the assignment figure shows it: the program name in
// "Command not found: name" is quoted, any other message is printed unchanged.
static void log_error_message(const ClientInfo *ci, const char *message)
{
    static const char prefix[] = "Command not found: ";

    if (strncmp(message, prefix, sizeof prefix - 1) == 0)
        log_line(ci, "ERROR", "Command not found: \"%s\"", message + sizeof prefix - 1);
    else
        log_line(ci, "ERROR", "%s", message);
}

// show the reply on the server console. a normal reply is printed as is, in the same
// log_line call as its [OUTPUT] line so no other client's line lands in the middle of it;
// a failure is logged as [ERROR] and the exact text that goes to the client is shown on one line.
static void log_reply(const ClientInfo *ci, const char *output, size_t len, int failed)
{
    if (failed) {
        char *message = trimmed_copy(output, len);
        if (message == NULL)
            return;
        log_error_message(ci, message);
        log_line(ci, "OUTPUT", "Sending error message to client: \"%s\"\n", message);
        free(message);
        return;
    }

    if (len == 0) {
        log_line(ci, "OUTPUT", "Command produced no output.\n");
        return;
    }

    log_line(ci, "OUTPUT", "Sending output to client:\n%s", output);
}

// what handle_command decided about the connection
enum
{
    SERVE_CONTINUE = 0,  // keep reading commands
    SERVE_STOP     = 1   // exit requested or the client is gone: end this session
};

// run one command line for one client through the Phase 1 parser and executor in a runner
// process, and send the text it printed back to that client.
static int handle_command(const ClientInfo *ci, const char *line)
{
    char  *output;
    size_t len;

    int code = run_command(line, &output, &len);
    if (code < 0) {
        log_line(ci, "ERROR", "Could not run command: %s", strerror(errno));
        static const char reply[] = "Server error: cannot run command.\n";
        return send_message(ci->fd, reply, sizeof reply - 1) < 0
               ? SERVE_STOP : SERVE_CONTINUE;
    }

    // "exit" ends only this client's session, nothing is sent back
    if (code == RUN_EXIT) {
        free(output);
        log_line(ci, "INFO", "Client requested disconnect. Closing connection.");
        return SERVE_STOP;
    }

    // a command counts as failrd when it could not be parsed or started, or when it
    // exited with an error status and printed something (that text is the error message).
    // a silent non-zero status such as "grep nomatch file" is not an error to report.
    int failed = code == RUN_PARSE_ERROR || code == RUN_START_FAILED
                 || (code == RUN_FAILED && len > 0);

    if (failed && len == 0) {
        free(output);
        output = strdup("Command execution failed.\n");
        if (output == NULL)
            return SERVE_STOP;
        len = strlen(output);
    }

    // only a line that reached the executor counts as executed
    if (code != RUN_EMPTY && code != RUN_PARSE_ERROR)
        log_line(ci, "EXECUTING", "Executing command: \"%s\"", line);
    if (code != RUN_EMPTY)
        log_reply(ci, output, len, failed);

    int sent = send_message(ci->fd, output, len);
    free(output);
    if (sent < 0) {
        log_line(ci, "ERROR", "Could not send reply to client: %s", strerror(errno));
        return SERVE_STOP;
    }
    return SERVE_CONTINUE;
}

// receive commands from the connected client until it disconnects or asks to exit.
static void serve_client(const ClientInfo *ci)
{
    for (;;) {
        char  *command;
        size_t len;

        // commands are bounded by the shell's own line limit
        int status = recv_message(ci->fd, &command, &len, MAX_LINE - 1);
        if (status == 0) {
            log_line(ci, "INFO", "Client closed the connection.");
            return;
        }
        if (status < 0) {
            log_line(ci, "ERROR", "Could not receive command: %s", strerror(errno));
            return;
        }

        log_line(ci, "RECEIVED", "Received command: \"%s\"", command);
        int next = handle_command(ci, command);
        free(command);
        if (next == SERVE_STOP)
            return;
    }
}

// start function of every client thread: serve the client, then free its ClientInfo.
// the thread is detached, so nobody joins it.
static void *client_thread(void *arg)
{
    ClientInfo *ci = arg;

    serve_client(ci);

    // a runner started for another client may still hold a copy of this socket, so close()
    // alone would not end the connection. shutdown() does, and the client sees it at once.
    shutdown(ci->fd, SHUT_RDWR);
    close(ci->fd);
    log_line(NULL, "INFO", "Client #%d disconnected.", ci->id);
    free(ci);
    return NULL;
}

// ---------------------------------------------------------------------------
// socket setup

// turn the port text into a number, rejecting anything that is not 1-65535.
static int parse_port(const char *text)
{
    char *end;
    long  port = strtol(text, &end, 10);

    if (*text == '\0' || *end != '\0' || port < 1 || port > 65535)
        return -1;
    return (int)port;
}

// create a TCP socket bound to `port` on all interfaces and start listening.
// SO_REUSEADDR lets the server restart immediately without waiting for the previous
// run's connection to leave TIME_WAIT. returns the listening descriitor or -1.
static int create_listener(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        shell_perror("socket");
        return -1;
    }

    int yes = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes) < 0) {
        shell_perror("setsockopt");
        close(fd);
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons((uint16_t)port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        shell_perror("bind");
        close(fd);
        return -1;
    }

    // several clients connect at once, so queue as many as the system allows
    if (listen(fd, SOMAXCONN) < 0) {
        shell_perror("listen");
        close(fd);
        return -1;
    }
    return fd;
}

int main(int argc, char *argv[])
{
    if (argc > 2) {
        fprintf(stderr, "Usage: %s [port]\n", argv[0]);
        return EXIT_FAILURE;
    }

    int port = parse_port(argc == 2 ? argv[1] : DEFAULT_PORT);
    if (port < 0) {
        shell_error("Invalid port number (use 1-65535).");
        return EXIT_FAILURE;
    }

    // writing to a client that already left would otherwise kill the server with SIGPIPE;
    // ignoring it turns that into an ordinary send() error we can report.
    signal(SIGPIPE, SIG_IGN);

    int listen_fd = create_listener(port);
    if (listen_fd < 0)
        return EXIT_FAILURE;

    log_line(NULL, "INFO", "Server started, waiting for client connections...");

    // runs until the process is killed (Ctrl-C). only this thread changes next_id.
    int next_id = 0;
    for (;;) {
        struct sockaddr_in peer;
        socklen_t          peer_len = sizeof peer;

        int client_fd = accept(listen_fd, (struct sockaddr *)&peer, &peer_len);
        if (client_fd < 0) {
            log_line(NULL, "ERROR", "accept failed: %s", strerror(errno));
            continue;
        }

        ClientInfo *ci = malloc(sizeof *ci);
        if (ci == NULL) {
            log_line(NULL, "ERROR", "Out of memory, connection dropped.");
            close(client_fd);
            continue;
        }
        ci->fd   = client_fd;
        ci->id   = ++next_id;
        ci->port = ntohs(peer.sin_port);
        inet_ntop(AF_INET, &peer.sin_addr, ci->ip, sizeof ci->ip);

        log_line(NULL, "INFO", "Client #%d connected from %s:%d. Assigned to Thread-%d.",
                 ci->id, ci->ip, ci->port, ci->id);

        pthread_t thread;
        if (pthread_create(&thread, NULL, client_thread, ci) != 0) {
            log_line(NULL, "ERROR", "Could not create a thread for client #%d.", ci->id);
            close(client_fd);
            free(ci);
            continue;
        }
        pthread_detach(thread);
    }
}
