#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

#include "../common/protocol.h"

// ---------------------------------------------------------------
// Per-connection state
//
// With an event loop, a single request can span multiple loop
// iterations (data arrives in pieces). All state must be stored
// explicitly in this struct between iterations.
// ---------------------------------------------------------------

struct Conn {
  int fd = -1;

  // flags that tell the event loop what this connection needs
  bool want_read = false;  // register POLLIN  for this fd
  bool want_write = false; // register POLLOUT for this fd
  bool want_close = false; // close and destroy this connection

  // incoming: bytes received from the client, not yet parsed
  // outgoing: bytes waiting to be sent back to the client
  std::vector<uint8_t> incoming;
  std::vector<uint8_t> outgoing;
};

// ---------------------------------------------------------------
// Application logic
// ---------------------------------------------------------------

// Try to parse and respond to one complete request in conn->incoming.
// Returns true if a full message was processed, false if more data needed.
// Can be called in a loop to drain multiple queued messages.
static bool try_one_request(Conn *conn) {
  // need at least 4 bytes for the length header
  if (conn->incoming.size() < 4) {
    return false;
  }

  // parse the 4-byte length header
  uint32_t len = 0;
  memcpy(&len, conn->incoming.data(), 4);
  if (len > k_max_msg) {
    msg("message too long");
    conn->want_close = true;
    return false;
  }

  // check if the full body has arrived
  if (4 + len > conn->incoming.size()) {
    return false; // body not complete yet, wait for more data
  }

  // we have a complete message — process it
  const uint8_t *request = &conn->incoming[4];
  printf("client says: %.*s\n", len, request);

  // echo the message back in the same [len | body] format
  buf_append(conn->outgoing, (const uint8_t *)&len, 4);
  buf_append(conn->outgoing, request, len);

  // remove the processed message from the incoming buffer
  buf_consume(conn->incoming, 4 + len);
  return true;
}

// Called when poll() signals POLLIN on a connection socket.
// Reads available bytes into conn->incoming, then tries to parse messages.
static void handle_read(Conn *conn) {
  uint8_t buf[64 * 1024];
  ssize_t rv = read(conn->fd, buf, sizeof(buf));
  if (rv < 0 && errno == EINTR) {
    return; // signal interrupted the read, try again next iteration
  }
  if (rv <= 0) {
    // rv == 0: client closed the connection (EOF)
    // rv <  0: real IO error
    conn->want_close = true;
    return;
  }

  // append received bytes to the incoming buffer
  buf_append(conn->incoming, buf, (size_t)rv);

  // try to process as many complete messages as are buffered
  while (try_one_request(conn)) {
  }

  // state transition: if we generated a response, switch to write mode
  if (conn->outgoing.size() > 0) {
    conn->want_read = false;
    conn->want_write = true;
  }
  // else: stay in want_read, waiting for more data
}

// Called when poll() signals POLLOUT on a connection socket.
// Drains as much of conn->outgoing as the kernel will accept.
static void handle_write(Conn *conn) {
  assert(conn->outgoing.size() > 0);

  ssize_t rv = write(conn->fd, conn->outgoing.data(), conn->outgoing.size());
  if (rv < 0 && errno == EINTR) {
    return; // interrupted, try again next iteration
  }
  if (rv < 0) {
    conn->want_close = true; // real write error
    return;
  }

  // remove the bytes that were successfully sent
  buf_consume(conn->outgoing, (size_t)rv);

  // state transition: if all data sent, switch back to read mode
  if (conn->outgoing.size() == 0) {
    conn->want_read = true;
    conn->want_write = false;
  }
  // else: still have data to send, stay in want_write
}

// ---------------------------------------------------------------
// Event loop helpers
// ---------------------------------------------------------------

// Called when poll() signals POLLIN on the listening socket.
// Accepts one pending connection and returns its Conn object.
static Conn *handle_accept(int listenfd) {
  struct sockaddr_in client_addr = {};
  socklen_t socklen = sizeof(client_addr);
  int connfd = accept(listenfd, (struct sockaddr *)&client_addr, &socklen);
  if (connfd < 0) {
    msg("accept() error");
    return NULL;
  }

  // make the connection non-blocking so read()/write() never block
  fd_set_nb(connfd);

  Conn *conn = new Conn();
  conn->fd = connfd;
  conn->want_read = true; // wait for the client's first request
  conn->want_write = false;
  conn->want_close = false;
  return conn;
}

// ---------------------------------------------------------------
// main: server setup + event loop
// ---------------------------------------------------------------

int main() {
  // create TCP socket
  int listenfd = socket(AF_INET, SOCK_STREAM, 0);
  if (listenfd < 0)
    die("socket()");

  // allow immediate port reuse after restart
  int val = 1;
  setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));

  // bind to 0.0.0.0:1234
  struct sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(1234);
  addr.sin_addr.s_addr = htonl(0);
  if (bind(listenfd, (const struct sockaddr *)&addr, sizeof(addr)) < 0) {
    die("bind()");
  }

  // start listening
  if (listen(listenfd, 128) < 0)
    die("listen()");

  // make the listening socket non-blocking
  fd_set_nb(listenfd);

  printf("Server (event loop) listening on port 1234...\n");

  // fd2conn: flat array indexed by fd value → Conn*
  // fds are small non-negative integers, so this is more efficient
  // than a hashtable and will be densely packed in practice
  std::vector<Conn *> fd2conn;

  // poll_args is rebuilt every iteration from the current connection states
  std::vector<struct pollfd> poll_args;

  // ---------------------------------------------------------------
  // THE EVENT LOOP
  // ---------------------------------------------------------------
  while (true) {

    // --- Step 1: build the poll list from current connection states ---
    poll_args.clear();

    // listening socket always goes first at index 0
    poll_args.push_back({listenfd, POLLIN, 0});

    // one entry per active connection
    for (Conn *conn : fd2conn) {
      if (!conn)
        continue;
      struct pollfd pfd = {conn->fd, POLLERR, 0}; // always watch errors
      if (conn->want_read)
        pfd.events |= POLLIN;
      if (conn->want_write)
        pfd.events |= POLLOUT;
      poll_args.push_back(pfd);
    }

    // --- Step 2: block until at least one fd is ready ---
    int rv = poll(poll_args.data(), (nfds_t)poll_args.size(), -1);
    if (rv < 0 && errno == EINTR) {
      continue; // signal interrupted poll, not an error — retry
    }
    if (rv < 0) {
      die("poll()");
    }

    // --- Step 3: handle the listening socket (always at index 0) ---
    if (poll_args[0].revents & POLLIN) {
      Conn *conn = handle_accept(listenfd);
      if (conn) {
        if (fd2conn.size() <= (size_t)conn->fd) {
          fd2conn.resize(conn->fd + 1, NULL);
        }
        fd2conn[conn->fd] = conn;
      }
    }

    // --- Step 4: handle connection sockets (index 1 onwards) ---
    for (size_t i = 1; i < poll_args.size(); i++) {
      uint32_t ready = poll_args[i].revents;
      Conn *conn = fd2conn[poll_args[i].fd];

      if (ready & POLLIN)
        handle_read(conn);
      if (ready & POLLOUT)
        handle_write(conn);

      // --- Step 5: destroy connections that are done ---
      if ((ready & POLLERR) || conn->want_close) {
        close(conn->fd);
        fd2conn[conn->fd] = NULL;
        delete conn;
      }
    }

  } // end event loop

  return 0;
}
