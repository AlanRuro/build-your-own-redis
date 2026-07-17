#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <assert.h>

const size_t k_max_msg = 4096;

static void msg(const char *msg) {
    fprintf(stderr, "%s\n", msg);
}

static void die(const char *msg) {
    int err = errno;
    fprintf(stderr, "[%d] %s\n", err, msg);
    abort();
}

static int32_t read_full(int fd, char *buf, size_t n) {
    while (n > 0) {
        ssize_t rv = read(fd, buf, n);
        if (rv <= 0) {
            return -1; // error, or unexpected EOF
        }
        assert((size_t)rv <= n);
        n -= (size_t)rv;
        buf += rv;
    }
    return 0;
}

static int32_t write_all(int fd, const char *buf, size_t n) {
    while (n > 0) {
        ssize_t rv = write(fd, buf, n);
        if (rv <= 0) {
            return -1; // error
        }
        assert((size_t)rv <= n);
        n -= (size_t)rv;
        buf += rv;
    }
    return 0;
}

static int32_t one_request(int connfd) {
    // 4 bytes header
    char rbuf[4 + k_max_msg + 1];
    errno = 0;

    // 1. Read the 4-byte length header
    int32_t err = read_full(connfd, rbuf, 4);
    if (err) {
        msg(errno == 0 ? "EOF" : "read() error");
        return err;
    }

    // 2. Parse the message length
    uint32_t len = 0;
    memcpy(&len, rbuf, 4); // assume little endian
    if (len > k_max_msg) {
        msg("too long");
        return -1;
    }

    // 3. Read the message body
    err = read_full(connfd, &rbuf[4], len);
    if (err) {
        msg("read() error");
        return err;
    }

    // 4. Print what the client sent
    rbuf[4 + len] = '\0'; // null-terminate for safety
    printf("client says: %.*s\n", len, &rbuf[4]);

    // 5. Send reply using the same [4-byte length | body] protocol
    const char reply[] = "world";
    char wbuf[4 + sizeof(reply)];
    len = (uint32_t)strlen(reply);
    memcpy(wbuf, &len, 4);
    memcpy(&wbuf[4], reply, len);
    return write_all(connfd, wbuf, 4 + len);
}

int main() {
    // 1. Create a TCP socket
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        die("socket()");
    }

    // 2. Allow reuse of the port immediately after restart
    int val = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));

    // 3. Bind to 0.0.0.0:1234
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(1234);        // port 1234, network byte order
    addr.sin_addr.s_addr = htonl(0);    // 0.0.0.0 — listen on all interfaces
    int rv = bind(fd, (const struct sockaddr *)&addr, sizeof(addr));
    if (rv) {
        die("bind()");
    }

    // 4. Start listening (backlog of 128 pending connections)
    rv = listen(fd, 128);
    if (rv) {
        die("listen()");
    }

    printf("Server listening on port 1234...\n");

    // 5. Accept and handle connections one at a time
    while (1) {
        struct sockaddr_in client_addr = {};
        socklen_t addrlen = sizeof(client_addr);
        int connfd = accept(fd, (struct sockaddr *)&client_addr, &addrlen);
        if (connfd < 0) {
            continue; // not fatal, try next
        }

        // Handle all requests from this connection
        while (1) {
            int32_t err = one_request(connfd);
            if (err) {
                break;
            }
        }
        close(connfd);
    }

    return 0;
}
