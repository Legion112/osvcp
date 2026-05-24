/*
 * Exercise 2: select()-based event-loop TCP server
 *
 * Handles multiple concurrent connections without threads.
 * The protocol: client sends a line (terminated by '\n'), server replies
 * with the current time of day.  The connection stays open so the client
 * can send several requests on the same socket.
 *
 * Key select() rules followed here:
 *   1. Rebuild the read-fd_set from scratch before *every* call to select()
 *      (select() modifies the set in-place to report which fds are ready).
 *   2. Track max_fd accurately; it must equal the highest fd in the set + 1.
 *   3. Always check errno == EINTR for interrupted calls.
 *   4. A readable listen_fd means a new connection is ready to accept().
 *   5. A readable client fd with read() == 0 means the peer closed (EOF).
 *   6. Never call recv()/read() without a prior select() telling it is ready,
 *      otherwise it may block and starve all other clients.
 *
 * Usage:
 *   ./select-server [port]   (default port: 8081)
 *
 * Test (open several terminals, all at once):
 *   nc localhost 8081       # type anything + Enter to get the time back
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>

#define DEFAULT_PORT  8081
#define BACKLOG       16
#define MAX_CLIENTS   64        /* must stay below FD_SETSIZE (typically 1024) */
#define REQ_BUF_SIZE  512

static volatile int running = 1;

static void handle_sigint(int sig) {
    (void)sig;
    running = 0;
}

/* -------------------------------------------------------------------------
 * Per-connection state
 * ---------------------------------------------------------------------- */
typedef struct {
    int  fd;
    char ip[INET_ADDRSTRLEN];
    int  port;
    char buf[REQ_BUF_SIZE]; /* partial line accumulator                    */
    int  buf_len;
} Client;

static Client clients[MAX_CLIENTS];
static int    num_clients = 0;

/* -------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------- */
static void current_time_str(char *out, size_t len) {
    time_t now = time(NULL);
    strftime(out, len, "%Y-%m-%d %H:%M:%S", localtime(&now));
}

static int create_listen_socket(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); exit(EXIT_FAILURE); }

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons((uint16_t)port),
        .sin_addr.s_addr = INADDR_ANY,
    };
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); exit(EXIT_FAILURE);
    }
    if (listen(fd, BACKLOG) < 0) {
        perror("listen"); exit(EXIT_FAILURE);
    }
    return fd;
}

/* Remove client at index i by swapping with the last slot. */
static void remove_client(int i) {
    close(clients[i].fd);
    printf("[%s:%d] disconnected (%d clients remaining)\n",
           clients[i].ip, clients[i].port, num_clients - 1);
    clients[i] = clients[--num_clients];
}

/* Add a freshly accepted socket to the client table. */
static void add_client(int fd, struct sockaddr_in *addr) {
    if (num_clients >= MAX_CLIENTS) {
        fprintf(stderr, "too many clients – rejecting fd %d\n", fd);
        close(fd);
        return;
    }
    Client *c = &clients[num_clients++];
    c->fd      = fd;
    c->buf_len = 0;
    c->port    = ntohs(addr->sin_port);
    inet_ntop(AF_INET, &addr->sin_addr, c->ip, sizeof(c->ip));
    printf("[%s:%d] connected (%d clients)\n", c->ip, c->port, num_clients);
}

/*
 * Process all complete lines (ending in '\n') buffered for client c.
 * Returns 0 normally, -1 if the client should be removed.
 */
static int process_client_data(Client *c) {
    /* Scan for newline-terminated requests inside the buffer. */
    while (1) {
        char *nl = memchr(c->buf, '\n', (size_t)c->buf_len);
        if (!nl) break;   /* no complete request yet */

        /* We have a complete request [buf .. nl]. */
        *nl = '\0';
        char time_buf[64];
        current_time_str(time_buf, sizeof(time_buf));

        char response[128];
        int rlen = snprintf(response, sizeof(response),
                            "Current time: %s\n", time_buf);

        if (write(c->fd, response, (size_t)rlen) < 0) {
            perror("write");
            return -1;
        }
        printf("[%s:%d] request='%s' -> %s", c->ip, c->port, c->buf, response);

        /* Consume the processed request from the buffer. */
        int consumed = (int)(nl - c->buf) + 1; /* including the '\n' */
        c->buf_len -= consumed;
        memmove(c->buf, nl + 1, (size_t)c->buf_len);
    }

    /* Guard against a client flooding us with data that has no newlines. */
    if (c->buf_len >= REQ_BUF_SIZE - 1) {
        fprintf(stderr, "[%s:%d] buffer overflow – closing\n", c->ip, c->port);
        return -1;
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * Event loop
 * ---------------------------------------------------------------------- */
static void event_loop(int listen_fd) {
    while (running) {

        /* --- Step 1: build the read fd_set from scratch every iteration. ---
         *
         * select() modifies the set to show which fds are ready, so we
         * cannot reuse the same set across calls.                          */
        fd_set read_fds;
        FD_ZERO(&read_fds);

        FD_SET(listen_fd, &read_fds);
        int max_fd = listen_fd;

        for (int i = 0; i < num_clients; i++) {
            FD_SET(clients[i].fd, &read_fds);
            if (clients[i].fd > max_fd)
                max_fd = clients[i].fd;
        }

        /* --- Step 2: wait until at least one fd is ready. ---
         *
         * timeout == NULL  → block indefinitely until something is ready.
         * max_fd + 1       → select() checks fds 0 … max_fd.             */
        int ready = select(max_fd + 1, &read_fds, NULL, NULL, NULL);

        if (ready < 0) {
            if (errno == EINTR) break;  /* SIGINT received */
            perror("select");
            break;
        }

        /* --- Step 3: check which fds select() marked as ready. --- */

        /* New incoming connection? */
        if (FD_ISSET(listen_fd, &read_fds)) {
            struct sockaddr_in client_addr;
            socklen_t addr_len = sizeof(client_addr);
            int client_fd = accept(listen_fd,
                                   (struct sockaddr *)&client_addr, &addr_len);
            if (client_fd < 0) {
                if (errno != EINTR) perror("accept");
            } else {
                add_client(client_fd, &client_addr);
            }
        }

        /* Readable client sockets?
         * Iterate backwards so that remove_client() (swap-with-last) does
         * not skip an unprocessed entry.                                   */
        for (int i = num_clients - 1; i >= 0; i--) {
            if (!FD_ISSET(clients[i].fd, &read_fds))
                continue;

            /* Read whatever is available — but only because select() said
             * this fd is ready, so read() will not block.                 */
            int space = REQ_BUF_SIZE - 1 - clients[i].buf_len;
            ssize_t n = read(clients[i].fd,
                             clients[i].buf + clients[i].buf_len,
                             (size_t)space);

            if (n <= 0) {
                /* n == 0: clean EOF (peer closed).
                 * n  < 0: error (e.g. connection reset).                  */
                if (n < 0 && errno != ECONNRESET)
                    perror("read");
                remove_client(i);
                continue;
            }

            clients[i].buf_len += (int)n;
            if (process_client_data(&clients[i]) < 0)
                remove_client(i);
        }
    }
}

/* -------------------------------------------------------------------------
 * main
 * ---------------------------------------------------------------------- */
int main(int argc, char *argv[]) {
    int port = DEFAULT_PORT;
    if (argc == 2)
        port = atoi(argv[1]);

    signal(SIGINT, handle_sigint);

    int listen_fd = create_listen_socket(port);
    printf("select()-based TCP time server listening on port %d "
           "(max %d clients, Ctrl-C to quit)\n", port, MAX_CLIENTS);

    event_loop(listen_fd);

    /* Close all remaining client connections on shutdown. */
    for (int i = 0; i < num_clients; i++)
        close(clients[i].fd);
    close(listen_fd);
    printf("\nServer shut down.\n");
    return 0;
}
