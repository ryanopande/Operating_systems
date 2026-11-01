// client.c — Phase 2 client: shows a "$" prompt, sends every command to the server over
// TCP and prints whatever the server sends back.

#include <ctype.h>
#include <errno.h>
#include <netdb.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "errors.h"
#include "lineio.h"
#include "net.h"
#include "shell.h"

// open a TCP connection to host:port. returns the socket file descriptor on success, -1 on failure (errno set).
static int connect_to_server(const char *host, const char *port)
{
    struct addrinfo hints, *results, *addr;

    memset(&hints, 0, sizeof hints);
    hints.ai_family   = AF_INET;      // the server listens on an IPv4 socket
    hints.ai_socktype = SOCK_STREAM;  // TCP

    int rc = getaddrinfo(host, port, &hints, &results);
    if (rc != 0) {
        fprintf(stderr, "myshell: cannot resolve %s:%s: %s\n", host, port, gai_strerror(rc));
        return -1;
    }

    int fd = -1;
    for (addr = results; addr != NULL; addr = addr->ai_next) {
        fd = socket(addr->ai_family, addr->ai_socktype, addr->ai_protocol);
        if (fd < 0)
            continue;
        if (connect(fd, addr->ai_addr, addr->ai_addrlen) == 0)
            break;

        // remember why the last attmpt failed so the message below is accurate
        int saved = errno;
        close(fd);
        fd    = -1;
        errno = saved;
    }
    freeaddrinfo(results);

    if (fd < 0)
        shell_perror("connect");
    return fd;
}

// true when the line holds nothing but whitespace, so there is no point sending it.
static int is_blank(const char *line)
{
    for (; *line; line++)
        if (!isspace((unsigned char)*line))
            return 0;
    return 1;
}

// true when the first word of the line is "exit"
static int is_exit_line(const char *line)
{
    char word[8];
    return sscanf(line, "%7s", word) == 1 && strcmp(word, "exit") == 0;
}

int main(int argc, char *argv[])
{
    if (argc > 3) {
        fprintf(stderr, "Usage: %s [host] [port]\n", argv[0]);
        return EXIT_FAILURE;
    }

    const char *host = argc >= 2 ? argv[1] : DEFAULT_HOST;
    const char *port = argc == 3 ? argv[2] : DEFAULT_PORT;

    // a send() to a server that already closed would otherwise kill the client with
    // SIGPIPE instead of returning an error we can report.
    signal(SIGPIPE, SIG_IGN);

    int fd = connect_to_server(host, port);
    if (fd < 0)
        return EXIT_FAILURE;

    char line[MAX_LINE];
    int  status = EXIT_SUCCESS;

    for (;;) {
        fputs(PROMPT, stdout);
        fflush(stdout);

        // end of input(Ctrl-D) closing the socket,server the client is gone, and server shuts down.
        if (!read_line(line, sizeof line)) {
            putchar('\n');
            break;
        }
        if (is_blank(line))
            continue;

        if (send_message(fd, line, strlen(line)) < 0) {
            shell_perror("send");
            status = EXIT_FAILURE;
            break;
        }

        char  *reply;
        size_t len;
        int    got = recv_message(fd, &reply, &len, MAX_MESSAGE);

        // the server answers "exit" by shutting down without a reply, so a closed
        // connection right after an exit command is the normal way to finish.
        if (got == 0) {
            if (!is_exit_line(line)) {
                shell_error("Server closed the connection.");
                status = EXIT_FAILURE;
            }
            break;
        }
        if (got < 0) {
            shell_perror("recv");
            status = EXIT_FAILURE;
            break;
        }

        fwrite(reply, 1, len, stdout);
        free(reply);
    }

    close(fd);
    return status;
}