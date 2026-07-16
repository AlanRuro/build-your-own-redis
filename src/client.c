#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

const size_t k_max_msg = 4096;

static void msg(const char *msg) { fprintf(stderr, "%s\n", msg); }

static void die(const char *msg) {
  int err = errno;
  fprintf(stderr, "[%d] %s\n", err, msg);
  abort();
}

static int32_t read_full(int fd, char *buf, size_t n) {
  while (n > 0) {
    ssize_t rv = read(fd, buf, n);
    if (rv <= 0) {
      if (rv == EINTR) {
        continue;
      }
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

static int32_t query(int fd, const char *text) {
  uint32_t len = (uint32_t)strlen(text);
  if (len > k_max_msg) {
    return -1;
  }

  // 1. Send request: [4-byte length | body]
  char wbuf[4 + k_max_msg];
  memcpy(wbuf, &len, 4);       // write length header
  memcpy(&wbuf[4], text, len); // write body
  int32_t err = write_all(fd, wbuf, 4 + len);
  if (err) {
    return err;
  }

  // 2. Read the 4-byte reply header
  char rbuf[4 + k_max_msg + 1];
  errno = 0;
  err = read_full(fd, rbuf, 4);
  if (err) {
    msg(errno == 0 ? "EOF" : "read() error");
    return err;
  }

  // 3. Parse reply length
  memcpy(&len, rbuf, 4); // assume little endian
  if (len > k_max_msg) {
    msg("too long");
    return -1;
  }

  // 4. Read reply body
  err = read_full(fd, &rbuf[4], len);
  if (err) {
    msg("read() error");
    return err;
  }

  // 5. Print the server's reply
  rbuf[4 + len] = '\0'; // null-terminate for safety
  printf("server says: %.*s\n", len, &rbuf[4]);
  return 0;
}

int main() {
  // 1. Create a TCP socket
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    die("socket()");
  }

  // 2. Connect to the server at 127.0.0.1:1234
  struct sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(1234);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // 127.0.0.1
  int rv = connect(fd, (const struct sockaddr *)&addr, sizeof(addr));
  if (rv) {
    die("connect()");
  }

  // 3. Send multiple requests over the same connection
  int32_t err = query(fd, "hello1");
  if (err) {
    goto L_DONE;
  }
  err = query(fd, "hello2");
  if (err) {
    goto L_DONE;
  }
  err = query(fd, "hello3");
  if (err) {
    goto L_DONE;
  }

L_DONE:
  close(fd);
  return 0;
}
