#pragma once

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <vector>

// maximum allowed message body size
const size_t k_max_msg = 4096;

// ---------------------------------------------------------------
// Utilities
// ---------------------------------------------------------------

static void msg(const char *m) { fprintf(stderr, "%s\n", m); }

static void die(const char *m) {
  fprintf(stderr, "[%d] %s\n", errno, m);
  abort();
}

// set a file descriptor to non-blocking mode
static void fd_set_nb(int fd) {
  fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
}

// ---------------------------------------------------------------
// Buffer helpers (used by the event loop)
// ---------------------------------------------------------------

// append bytes to the back of a buffer
static void buf_append(std::vector<uint8_t> &buf, const uint8_t *data,
                       size_t len) {
  buf.insert(buf.end(), data, data + len);
}

// remove n bytes from the front of a buffer
static void buf_consume(std::vector<uint8_t> &buf, size_t n) {
  buf.erase(buf.begin(), buf.begin() + n);
}
