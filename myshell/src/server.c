// server.c — Phase 2 server: accepts one client over TCP, runs every command it sends
// with the Phase 1 shell engine (parser + executor) and sends the output back.


#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "builtins.h"
#include "errors.h"
#include "executor.h"
#include "net.h"
#include "parser.h"
#include "shell.h"

// text appended to a reply that had to be cut at MAX_MESSAGE bytes
#define TRUNCATION_NOTICE "\n[output truncated]\n"

// ---------------------------------------------------------------------------
// logging


// while a command runs, the server's own stdout is pointed at the capture file, so the
// log lines are written through this separate diplicate of the real terminal instead.
static FILE *log_stream;

// open the log stream on a private copy of the terminal's stdout. the copy is marked
// close-on-exec so the programs we run do not inherit it.
static int log_open(void)
{
    int fd = fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC, 3);
    if (fd < 0)
        return -1;

    log_stream = fdopen(fd, "w");
    if (log_stream == NULL) {
        close(fd);
        return -1;
    }
    return 0;
}

// print one "[TAG] message" line and flush it right away so the log stays in step
// with what happens even when the server's output is piped into a file.
static void log_line(const char *tag, const char *format, ...)
{
    va_list args;

    fprintf(log_stream, "[%s] ", tag);
    va_start(args, format);
    vfprintf(log_stream, format, args);
    va_end(args);
    fputc('\n', log_stream);
    fflush(log_stream);
}

// ---------------------------------------------------------------------------
// output capture

// state needed to put stdin/stdout/stderr back after a capture.
typedef struct Capture 
{
    FILE *file;      // temporary file that receives everything the command prints
    int   saved[3];  // copies of the original descriptors 0, 1 and 2
} Capture;

// duplicate `fd` into a descriptor above 2 that is closed automatically on exec.
static int save_fd(int fd)
{
    return fcntl(fd, F_DUPFD_CLOEXEC, 3);
}

// point stdout and stderr at a temporary file and stdin at /dev/null. a file is used
// instead of a pipe on purpose: a pipe holds only ~64 KiB, so a command printing more
// than that would block forever while the server waits for it to finish. stdin goes to
// /dev/null so a command such as a bare "cat" ends immediately instead of waiting for
// input on the server's terminal.
static int capture_begin(Capture *cap)
{
    fflush(stdout);
    fflush(stderr);

    cap->file = tmpfile();
    if (cap->file == NULL) {
        shell_perror("tmpfile");
        return -1;
    }

    for (int i = 0; i < 3; i++) {
        cap->saved[i] = save_fd(i);
        if (cap->saved[i] < 0) {
            shell_perror("dup");
            fclose(cap->file);
            for (int j = 0; j < i; j++)
                close(cap->saved[j]);
            return -1;
        }
    }

    int null_fd = open("/dev/null", O_RDONLY);
    int tmp_fd  = fileno(cap->file);
    if (null_fd >= 0) {
        dup2(null_fd, STDIN_FILENO);
        close(null_fd);
    }
    dup2(tmp_fd, STDOUT_FILENO);
    dup2(tmp_fd, STDERR_FILENO);
    return 0;
}

// restore the original descriptors and return everything captured as a malloc'd,
// NUL-terminated string (*len is its length). returns NULL if memory ran out.
static char *capture_end(Capture *cap, size_t *len)
{
    fflush(stdout);
    fflush(stderr);

    for (int i = 0; i < 3; i++) {
        dup2(cap->saved[i], i);
        close(cap->saved[i]);
    }

    // the child processes wrote through the shared file offset, so the end of the
    // file is also the size of the output.
    int  fd   = fileno(cap->file);
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

    fclose(cap->file);
    return out;
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
static void log_error_message(const char *message)
{
    static const char prefix[] = "Command not found: ";

    if (strncmp(message, prefix, sizeof prefix - 1) == 0)
        log_line("ERROR", "Command not found: \"%s\"", message + sizeof prefix - 1);
    else
        log_line("ERROR", "%s", message);
}

// show the reply on the server console. a normal reply is printed as is, directly
// followed by the next log line (like the figure); a failure is logged as [ERROR] and the exact text that
// goes to the client is shown on one line.
static void log_reply(const char *output, size_t len, int failed)
{
    if (failed) {
        char *message = trimmed_copy(output, len);
        if (message == NULL)
            return;
        log_error_message(message);
        log_line("OUTPUT", "Sending error message to client: \"%s\"", message);
        free(message);
        return;
    }

    if (len == 0) {
        log_line("OUTPUT", "Command produced no output.");
        return;
    }

    log_line("OUTPUT", "Sending output to client:");
    fputs(output, log_stream);
    if (output[len - 1] != '\n')
        fputc('\n', log_stream);
    fflush(log_stream);
}

// what handle_command decided about the connection
enum 
{
    SERVE_CONTINUE = 0,  // keep reading commands
    SERVE_STOP     = 1   // exit requested or the client is gone: shut down
};

// run one command line through the Phase 1 parser and executor, capturing everything it
// prints, and send that text back to the client.
static int handle_command(int client_fd, const char *line)
{
    static Pipeline pipeline;
    Capture cap;
    size_t  len = 0;

    // parse errors are printed by parse_line() on stderr, so parsing happens inside
    // the capture too: the client then receives them exactly like command output.
    if (capture_begin(&cap) < 0) {
        log_line("ERROR", "Could not set up output capture: %s", strerror(errno));
        static const char reply[] = "Server error: cannot run command.\n";
        return send_message(client_fd, reply, sizeof reply - 1) < 0
               ? SERVE_STOP : SERVE_CONTINUE;
    }

    int parsed = parse_line(line, &pipeline);
    int status = 0;

    if (parsed == PARSE_OK && is_exit_command(&pipeline)) {
        char *discard = capture_end(&cap, &len);
        free(discard);
        log_line("INFO", "Exit command received, shutting down.");
        return SERVE_STOP;
    }

    if (parsed == PARSE_OK) {
        log_line("EXECUTING", "Executing command: \"%s\"", line);
        status = execute_pipeline(&pipeline);
    }

    char *output = capture_end(&cap, &len);
    if (output == NULL) {
        log_line("ERROR", "Out of memory while collecting output.");
        return SERVE_STOP;
    }

    // a command counts as failrd when it could not be parsed or started, or when it
    // exited with an error status and printed something (that text is the error message).
    // a silent non-zero status such as "grep nomatch file" is not an error to report.
    int failed = parsed == PARSE_ERROR || (parsed == PARSE_OK && status != 0 && len > 0)
                 || (parsed == PARSE_OK && status < 0);

    if (failed && len == 0) {
        free(output);
        output = strdup("Command execution failed.\n");
        if (output == NULL)
            return SERVE_STOP;
        len = strlen(output);
    }

    if (parsed != PARSE_EMPTY)
        log_reply(output, len, failed);

    int sent = send_message(client_fd, output, len);
    free(output);
    if (sent < 0) {
        log_line("ERROR", "Could not send reply to client: %s", strerror(errno));
        return SERVE_STOP;
    }
    return SERVE_CONTINUE;
}

// receive commands from the connected client until it disconnects or asks to exit.
static void serve_client(int client_fd)
{
    for (;;) {
        char  *command;
        size_t len;

        // commands are bounded by the shell's own line limit
        int status = recv_message(client_fd, &command, &len, MAX_LINE - 1);
        if (status == 0) {
            log_line("INFO", "Client disconnected.");
            return;
        }
        if (status < 0) {
            log_line("ERROR", "Could not receive command: %s", strerror(errno));
            return;
        }

        log_line("RECEIVED", "Received command: \"%s\" from client.", command);
        int next = handle_command(client_fd, command);
        free(command);
        if (next == SERVE_STOP)
            return;
    }
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

    // backlog of 1: this phase serves a single client
    if (listen(fd, 1) < 0) {
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

    if (log_open() < 0) {
        shell_perror("log");
        return EXIT_FAILURE;
    }

    int listen_fd = create_listener(port);
    if (listen_fd < 0)
        return EXIT_FAILURE;

    log_line("INFO", "Server started, waiting for client connections...");

    int client_fd;
    do {
        client_fd = accept(listen_fd, NULL, NULL);
    } while (client_fd < 0 && errno == EINTR);

    // only one client is served, so the listening socket is no longer needed
    close(listen_fd);
    if (client_fd < 0) {
        shell_perror("accept");
        return EXIT_FAILURE;
    }

    log_line("INFO", "Client connected.");
    serve_client(client_fd);

    close(client_fd);
    log_line("INFO", "Server shut down.");
    return EXIT_SUCCESS;
}
