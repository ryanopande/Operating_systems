// message framing over a TCP socket, shared by the client and the server.

#include <arpa/inet.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>

#include "net.h"

// number of bytes in the length prefix of every message
#define HEADER_SIZE 4

// send exactly `len` bytes. send() may accept fewer bytes than asked for, so keep
// going until everything is written; a call interrupted by a signal is simply retried.
static int send_all(int fd, const char *buf, size_t len)
{
    while (len > 0) {
        ssize_t sent = send(fd, buf, len, 0);
        if (sent < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        buf += sent;
        len -= (size_t)sent;
    }
    return 0;
}

// receive exactly `len` bytes. returns 1 when all of them arrived, 0 when the peer
// closed the connection before the first byte (a clean end), and -1 on an error or when
// the connection closed in the middle of the data (a truncated message).
static int recv_all(int fd, char *buf, size_t len)
{
    size_t got = 0;

    while (got < len) {
        ssize_t n = recv(fd, buf + got, len - got, 0);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0) {
            if (got == 0)
                return 0;
            errno = ECONNRESET;
            return -1;
        }
        got += (size_t)n;
    }
    return 1;
}

int send_message(int fd, const char *data, size_t len)
{
    if (len > UINT32_MAX) {
        errno = EMSGSIZE;
        return -1;
    }

    // header and payload go out in a single send() so a tiny reply is one TCP segment
    // insted of two
    char *frame = malloc(HEADER_SIZE + len);
    if (frame == NULL)
        return -1;

    uint32_t net_len = htonl((uint32_t)len);
    memcpy(frame, &net_len, HEADER_SIZE);
    memcpy(frame + HEADER_SIZE, data, len);

    int result = send_all(fd, frame, HEADER_SIZE + len);
    free(frame);
    return result;
}

int recv_message(int fd, char **data, size_t *len, size_t max)
{
    uint32_t net_len;
    int status = recv_all(fd, (char *)&net_len, HEADER_SIZE);
    if (status <= 0)
        return status;

    size_t payload_len = ntohl(net_len);
    if (payload_len > max) {
        errno = EMSGSIZE;
        return -1;
    }

    // one extra byte for the terminating NUL so callers can treat the payload as a string
    char *buf = malloc(payload_len + 1);
    if (buf == NULL)
        return -1;

    // a zero-length payload is valid and needs no read
    if (payload_len > 0) {
        int got = recv_all(fd, buf, payload_len);
        if (got != 1) {
            // got == 0 peer vanished right after the header, which is a
            // truncated message just like a mid-payload close.
            int saved = (got == 0) ? ECONNRESET : errno;
            free(buf);
            errno = saved;
            return -1;
        }
    }

    buf[payload_len] = '\0';
    *data = buf;
    *len  = payload_len;
    return 1;
}