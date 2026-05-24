#define _GNU_SOURCE

/*
 * Exercise 4: AIO file-serving TCP server (C / POSIX AIO)
 *
 * Builds on file-server.c but replaces all blocking operations with
 * non-blocking equivalents:
 *
 *   Network reads   → select() on read_fds  (was already non-blocking)
 *   File reads      → aio_read()            (new)
 *   Network writes  → select() on write_fds + O_NONBLOCK socket  (new)
 *
 * Per-client state machine
 * ────────────────────────
 *
 *   STATE_READING ──► (complete request line arrives)
 *        │
 *        │  validate, open, stat, malloc, write header into buf, aio_read()
 *        ▼
 *   STATE_AIO     ──► (poll aio_error() every loop iteration)
 *        │
 *        │  aio_error() == 0: transition, close file fd
 *        ▼
 *   STATE_WRITING ──► (select write_fds, write in chunks, advance offset)
 *        │
 *        │  all bytes sent
 *        └──────────────────────────────────────────────► STATE_READING
 *
 * The event loop never blocks except inside select().  Every other call
 * (read, write, accept) is guaranteed to be ready before we call it.
 *
 * Compile: gcc -Wall -Wextra -std=c11 -g -o aio-file-server aio-file-server.c -lrt
 * Usage:   ./aio-file-server [port [docroot]]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <aio.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* ── constants ─────────────────────────────────────────────────────────────── */

#define DEFAULT_PORT  8084
#define BACKLOG       128
#define MAX_CLIENTS   64
#define REQ_BUF_SIZE  512
#define WRITE_CHUNK   4096   /* bytes per non-blocking write attempt */

/* ── globals ────────────────────────────────────────────────────────────────── */

static volatile int running = 1;
static char         docroot[PATH_MAX];

static void handle_sigint(int sig) { (void)sig; running = 0; }

/* ── per-client state ───────────────────────────────────────────────────────── */

typedef enum {
    STATE_READING,   /* accumulating the request line                     */
    STATE_AIO,       /* aio_read() is in-flight for the file content      */
    STATE_WRITING,   /* sending the response buffer to the client socket  */
} ClientState;

typedef struct {
    int              sock;
    char             ip[INET_ADDRSTRLEN];
    int              port;

    /* request accumulation ------------------------------------------------ */
    char             req_buf[REQ_BUF_SIZE];
    int              req_len;

    /* AIO + response ------------------------------------------------------- */
    ClientState      state;
    int              file_fd;    /* open during STATE_AIO; closed on transition */
    char            *resp;       /* malloc'd: "+OK N\n" + file content          */
    size_t           resp_size;  /* total bytes to send                         */
    size_t           resp_sent;  /* bytes already written                       */
    struct aiocb     aio;        /* POSIX AIO control block                     */
} Client;

static Client clients[MAX_CLIENTS];
static int    num_clients = 0;

/* ── helpers ────────────────────────────────────────────────────────────────── */

static void set_nonblocking(int fd) {
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
}

static int create_listen_socket(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); exit(EXIT_FAILURE); }

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    set_nonblocking(fd);

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

static void remove_client(int i) {
    Client *c = &clients[i];

    /* Cancel any in-flight AIO before closing the file. */
    if (c->state == STATE_AIO) {
        aio_cancel(c->file_fd, &c->aio);
        /* Call aio_return to release any kernel resources. */
        while (aio_error(&c->aio) == EINPROGRESS)
            ;
        aio_return(&c->aio);
        close(c->file_fd);
    }
    free(c->resp);
    close(c->sock);
    printf("[%s:%d] disconnected\n", c->ip, c->port);
    clients[i] = clients[--num_clients];
}

static void add_client(int fd, struct sockaddr_in *addr) {
    if (num_clients >= MAX_CLIENTS) {
        fprintf(stderr, "max clients – rejecting fd %d\n", fd);
        close(fd);
        return;
    }
    Client *c  = &clients[num_clients++];
    *c = (Client){0};
    c->sock  = fd;
    c->state = STATE_READING;
    c->port  = ntohs(addr->sin_port);
    inet_ntop(AF_INET, &addr->sin_addr, c->ip, sizeof(c->ip));
    set_nonblocking(c->sock);  /* O_NONBLOCK so writes never stall the loop */
    printf("[%s:%d] connected (%d clients)\n", c->ip, c->port, num_clients);
}

/* Write len bytes to fd, returning immediately on EAGAIN (partial is OK). */
static ssize_t nb_write(int fd, const char *buf, size_t len) {
    ssize_t n = write(fd, buf, len > WRITE_CHUNK ? WRITE_CHUNK : len);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        return 0;   /* not ready yet; try again next iteration */
    return n;
}

static void sock_send(int fd, const char *msg) {
    size_t len = strlen(msg);
    while (len > 0) {
        ssize_t n = write(fd, msg, len);
        if (n < 0) { if (errno == EINTR) continue; return; }
        msg += n; len -= (size_t)n;
    }
}

/* ── begin_file_serve ───────────────────────────────────────────────────────── */

/*
 * Called when a complete request line has been received.
 * Validates the path, opens the file, builds the response buffer, and
 * issues aio_read() to fill the file portion asynchronously.
 * On error, sends -ERR immediately and leaves the client in STATE_READING.
 */
static void begin_file_serve(Client *c, const char *filename) {
    /* Security check 1 */
    if (filename[0] == '\0') { sock_send(c->sock, "-ERR empty filename\n"); return; }
    if (filename[0] == '/')  { sock_send(c->sock, "-ERR absolute paths not allowed\n"); return; }

    /* Security check 2: canonicalize */
    char candidate[PATH_MAX * 2];
    snprintf(candidate, sizeof(candidate), "%s/%s", docroot, filename);

    char resolved[PATH_MAX];
    if (realpath(candidate, resolved) == NULL) {
        sock_send(c->sock, "-ERR not found\n");
        return;
    }
    size_t rlen = strlen(docroot);
    if (strncmp(resolved, docroot, rlen) != 0 ||
        (resolved[rlen] != '/' && resolved[rlen] != '\0')) {
        sock_send(c->sock, "-ERR access denied\n");
        return;
    }

    /* Security check 3: regular files only */
    struct stat st;
    if (stat(resolved, &st) < 0 || !S_ISREG(st.st_mode)) {
        sock_send(c->sock, !S_ISREG(st.st_mode)
                  ? "-ERR not a regular file\n" : "-ERR stat failed\n");
        return;
    }

    /* Open the file. */
    int file_fd = open(resolved, O_RDONLY);
    if (file_fd < 0) { sock_send(c->sock, "-ERR open failed\n"); return; }

    /* Build the response buffer: header + file content.
     * aio_read() will fill the file portion; header is written now. */
    char header[64];
    int hlen = snprintf(header, sizeof(header), "+OK %lld\n", (long long)st.st_size);

    char *resp = malloc((size_t)hlen + (size_t)st.st_size);
    if (!resp) {
        sock_send(c->sock, "-ERR out of memory\n");
        close(file_fd);
        return;
    }
    memcpy(resp, header, (size_t)hlen);

    /* Issue the async read.
     *
     * aio_sigevent.sigev_notify = SIGEV_NONE means we will poll for
     * completion using aio_error() rather than receiving a signal.
     * The OSTEP chapter also describes SIGEV_SIGNAL (signal on complete)
     * which is more efficient but adds signal-handling complexity.       */
    c->file_fd         = file_fd;
    c->resp            = resp;
    c->resp_size       = (size_t)hlen + (size_t)st.st_size;
    c->resp_sent       = 0;
    c->aio             = (struct aiocb){0};
    c->aio.aio_fildes  = file_fd;
    c->aio.aio_buf     = resp + hlen;          /* write into buffer after header */
    c->aio.aio_nbytes  = (size_t)st.st_size;
    c->aio.aio_offset  = 0;
    c->aio.aio_sigevent.sigev_notify = SIGEV_NONE;

    if (aio_read(&c->aio) < 0) {
        perror("aio_read");
        free(resp); c->resp = NULL;
        close(file_fd);
        sock_send(c->sock, "-ERR aio_read failed\n");
        return;
    }

    c->state = STATE_AIO;
    printf("[%s:%d] aio_read started for '%s' (%lld bytes)\n",
           c->ip, c->port, filename, (long long)st.st_size);
}

/* ── check_aio ──────────────────────────────────────────────────────────────── */

/*
 * Called each event-loop iteration for every client in STATE_AIO.
 * Returns true if the client state changed (and needs no further work this
 * iteration), false if still pending.
 */
static int check_aio(Client *c) {
    int err = aio_error(&c->aio);

    if (err == EINPROGRESS)
        return 0;   /* still running */

    /* AIO is done — always call aio_return() to release kernel resources. */
    ssize_t ret = aio_return(&c->aio);
    close(c->file_fd);
    c->file_fd = -1;

    if (err != 0 || ret < 0) {
        fprintf(stderr, "[%s:%d] aio error: %s\n", c->ip, c->port, strerror(err));
        free(c->resp); c->resp = NULL;
        c->state = STATE_READING;
        sock_send(c->sock, "-ERR read error\n");
        return 1;
    }

    printf("[%s:%d] aio_read complete (%zd bytes), entering WRITING\n",
           c->ip, c->port, ret);
    c->state = STATE_WRITING;
    return 1;
}

/* ── event loop ──────────────────────────────────────────────────────────────── */

static void event_loop(int listen_fd) {
    while (running) {
        /* Build read_fds (listen + all clients) and write_fds (WRITING clients). */
        fd_set read_fds, write_fds;
        FD_ZERO(&read_fds);
        FD_ZERO(&write_fds);
        FD_SET(listen_fd, &read_fds);
        int max_fd = listen_fd;
        int aio_pending = 0;

        for (int i = 0; i < num_clients; i++) {
            int fd = clients[i].sock;
            /* Monitor all clients for EOF / new requests. */
            FD_SET(fd, &read_fds);
            /* Only WRITING clients need write-readiness. */
            if (clients[i].state == STATE_WRITING)
                FD_SET(fd, &write_fds);
            if (clients[i].state == STATE_AIO)
                aio_pending = 1;
            if (fd > max_fd) max_fd = fd;
        }

        /*
         * Timeout strategy:
         *   • When AIO operations are in-flight, use a 1 ms timeout so we
         *     poll aio_error() frequently without busy-spinning.
         *   • When no AIO is pending, block indefinitely — no reason to wake.
         */
        struct timeval  tv  = {0, 1000};
        struct timeval *tvp = aio_pending ? &tv : NULL;

        int ready = select(max_fd + 1, &read_fds, &write_fds, NULL, tvp);
        if (ready < 0) {
            if (errno == EINTR) break;
            perror("select"); break;
        }

        /* Accept new connections (drain the queue). */
        if (FD_ISSET(listen_fd, &read_fds)) {
            while (num_clients < MAX_CLIENTS) {
                struct sockaddr_in addr;
                socklen_t len = sizeof(addr);
                int cfd = accept(listen_fd, (struct sockaddr *)&addr, &len);
                if (cfd < 0) {
                    if (errno != EAGAIN && errno != EWOULDBLOCK) perror("accept");
                    break;
                }
                add_client(cfd, &addr);
            }
        }

        /* Process clients — iterate backwards for safe swap-remove. */
        for (int i = num_clients - 1; i >= 0; i--) {
            Client *c = &clients[i];

            /* --- Detect EOF / disconnect on any client in any state. --- */
            if (FD_ISSET(c->sock, &read_fds) && c->state != STATE_READING) {
                /* A readable event on a non-READING client means EOF. */
                char probe;
                ssize_t n = recv(c->sock, &probe, 1, MSG_PEEK);
                if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                    remove_client(i);
                    continue;
                }
            }

            /* --- STATE_AIO: poll for completion. --- */
            if (c->state == STATE_AIO) {
                check_aio(c);
                /* Fall through: if it transitioned to WRITING this iteration
                 * we can start sending immediately (write_fds may not be set
                 * yet, but we'll catch it next loop). */
            }

            /* --- STATE_READING: read from socket. --- */
            if (c->state == STATE_READING && FD_ISSET(c->sock, &read_fds)) {
                int space = REQ_BUF_SIZE - 1 - c->req_len;
                ssize_t n = read(c->sock, c->req_buf + c->req_len, (size_t)space);
                if (n <= 0) { remove_client(i); continue; }
                c->req_len += (int)n;

                /* Process any complete lines in the buffer. */
                char *nl;
                while ((nl = memchr(c->req_buf, '\n', (size_t)c->req_len))) {
                    *nl = '\0';
                    int len = (int)(nl - c->req_buf);
                    if (len > 0 && c->req_buf[len-1] == '\r') c->req_buf[--len] = '\0';

                    begin_file_serve(c, c->req_buf);

                    int consumed = (int)(nl - c->req_buf) + 1;
                    c->req_len  -= consumed;
                    memmove(c->req_buf, nl + 1, (size_t)c->req_len);

                    if (c->state != STATE_READING) break; /* went async */
                }
                if (c->req_len >= REQ_BUF_SIZE - 1) { remove_client(i); continue; }
            }

            /* --- STATE_WRITING: send response chunk. --- */
            if (c->state == STATE_WRITING && FD_ISSET(c->sock, &write_fds)) {
                ssize_t n = nb_write(c->sock,
                                     c->resp + c->resp_sent,
                                     c->resp_size - c->resp_sent);
                if (n < 0) { remove_client(i); continue; }
                c->resp_sent += (size_t)n;

                if (c->resp_sent == c->resp_size) {
                    printf("[%s:%d] response sent (%zu bytes)\n",
                           c->ip, c->port, c->resp_size);
                    free(c->resp); c->resp = NULL;
                    c->state = STATE_READING;
                }
            }
        }
    }
}

/* ── main ───────────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    int         port    = DEFAULT_PORT;
    const char *rootarg = "./docroot";

    if (argc >= 2) port    = atoi(argv[1]);
    if (argc >= 3) rootarg = argv[2];

    if (realpath(rootarg, docroot) == NULL) {
        fprintf(stderr, "docroot '%s': %s\n", rootarg, strerror(errno));
        return EXIT_FAILURE;
    }

    signal(SIGINT,  handle_sigint);
    signal(SIGPIPE, SIG_IGN);

    int listen_fd = create_listen_socket(port);
    printf("AIO file server listening on port %d, docroot=%s\n", port, docroot);

    event_loop(listen_fd);

    for (int i = 0; i < num_clients; i++) close(clients[i].sock);
    close(listen_fd);
    printf("\nServer shut down.\n");
    return 0;
}
