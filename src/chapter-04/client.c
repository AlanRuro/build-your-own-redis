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

static void msg(const char *m) { fprintf(stderr, "%s\n", m); }

static void die(const char *m) {
  fprintf(stderr, "[%d] %s\n", errno, m);
  abort();
}

// ---------------------------------------------------------------
// IO helpers (same as server — keep reads/writes complete)
// ---------------------------------------------------------------

static int32_t read_full(int fd, char *buf, size_t n) {
  while (n > 0) {
    ssize_t rv = read(fd, buf, n);
    if (rv < 0) {
      if (errno == EINTR)
        continue;
      return -1;
    }
    if (rv == 0)
      return -1; // unexpected EOF
    assert((size_t)rv <= n);
    n -= (size_t)rv;
    buf += rv;
  }
  return 0;
}

static int32_t write_all(int fd, const char *buf, size_t n) {
  while (n > 0) {
    ssize_t rv = write(fd, buf, n);
    if (rv < 0) {
      if (errno == EINTR)
        continue;
      return -1;
    }
    if (rv == 0)
      return -1;
    assert((size_t)rv <= n);
    n -= (size_t)rv;
    buf += rv;
  }
  return 0;
}

// ---------------------------------------------------------------
// Send one request and read one response.
// Protocol: [ 4-byte length (little endian) | body ]
// ---------------------------------------------------------------

static int32_t query(int fd, const char *text) {
  uint32_t len = (uint32_t)strlen(text);
  if (len > k_max_msg)
    return -1;

  // send: [ 4-byte length | body ]
  char wbuf[4 + k_max_msg];
  memcpy(wbuf, &len, 4);
  memcpy(&wbuf[4], text, len);
  int32_t err = write_all(fd, wbuf, 4 + len);
  if (err)
    return err;

  // read response header
  char rbuf[4 + k_max_msg + 1];
  errno = 0;
  err = read_full(fd, rbuf, 4);
  if (err) {
    msg(errno == 0 ? "EOF" : "read() error");
    return err;
  }

  // parse response length
  memcpy(&len, rbuf, 4);
  if (len > k_max_msg) {
    msg("response too long");
    return -1;
  }

  // read response body
  err = read_full(fd, &rbuf[4], len);
  if (err) {
    msg("read() error");
    return err;
  }

  // print server response
  rbuf[4 + len] = '\0';
  printf("server says: %.*s\n", len, &rbuf[4]);
  return 0;
}

// ---------------------------------------------------------------
// main: connect and send several requests over the same connection
// ---------------------------------------------------------------

int main() {
  // create TCP socket
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
    die("socket()");

  // connect to 127.0.0.1:1234
  struct sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(1234);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (connect(fd, (const struct sockaddr *)&addr, sizeof(addr)) < 0) {
    die("connect()");
  }

  // send multiple requests over the same connection
  int32_t err;
  err = query(fd, "hello1");
  if (err)
    goto done;
  err = query(fd, "hello2");
  if (err)
    goto done;
  err = query(fd, "hello3");
  if (err)
    goto done;

done:
  close(fd);
  return 0;
}
