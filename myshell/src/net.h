// net.h — message framing over a TCP socket, shared by the client and the server.


#ifndef NET_H
#define NET_H

#include <stddef.h>

// port used by the server and the client when none is given on the command line
#define DEFAULT_PORT "8080"

// host the client connects to when none is given on the command line
#define DEFAULT_HOST "127.0.0.1"

// largest payload either side accepts in a reply (16 MiB)
#define MAX_MESSAGE (16 * 1024 * 1024)

// send one framed message of `len` bytes. returns 0 on success, -1 on failure (errno set).
int send_message(int fd, const char *data, size_t len);

// receive one framed message
int recv_message(int fd, char **data, size_t *len, size_t max);

#endif