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
 * File cache (exercise 5):
 *   • A small in-process cache stores recently served files.
 *   • On a cache hit begin_file_serve() skips the AIO machinery entirely and
 *     goes straight to STATE_WRITING with a copy of the cached response.
 *   • On a cache miss, once aio_read() completes the response is stored.
 *   • Sending SIGUSR1 flushes the entire cache.
 *
 * Per-client state machine
 * ────────────────────────
 *
 *   STATE_READING ──► (complete request line arrives)
 *        │
 *        │  validate path → cache lookup
 *        │    hit  → copy cached resp → STATE_WRITING  (no AIO)
 *        │    miss → open, stat, malloc, write header, aio_read()
 *        ▼
 *   STATE_AIO     ──► (poll aio_error() every loop iteration)
 *        │
 *        │  aio_error() == 0: cache response, transition, close file fd
 *        ▼
 *   STATE_WRITING ──► (select write_fds, write in chunks, advance offset)
 *        │
 *        │  all bytes sent
 *        └──────────────────────────────────────────────► STATE_READING
 *
 * Compile: gcc -Wall -Wextra -std=c11 -g -o aio-file-server aio-file-server.c -lrt
 * Usage:   ./aio-file-server [port [docroot]]
 *          kill -USR1 <pid>    # flush cache
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
#include <sys/sendfile.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* ── constants ─────────────────────────────────────────────────────────────── */

#define DEFAULT_PORT        8084
#define BACKLOG             128
#define MAX_CLIENTS         64
#define REQ_BUF_SIZE        512
#define MAX_CACHE_ENTRIES   16
#define MAX_CACHE_FILE_SIZE (64 * 1024)

/* AIO completion signal: SIGRTMIN+1, blocked and delivered via signalfd. */
#define AIO_DONE_SIG (SIGRTMIN + 1)

/* ── globals ────────────────────────────────────────────────────────────────── */

static volatile int            running               = 1;
static volatile sig_atomic_t   cache_clear_requested = 0;
static char                    docroot[PATH_MAX];

static void handle_sigint(int sig)  { (void)sig; running = 0; }
static void handle_sigusr1(int sig) { (void)sig; cache_clear_requested = 1; }

/* ── file cache ─────────────────────────────────────────────────────────────── */

/*
 * Stores the full pre-built response (header + file bytes) for recently
 * served files.  On a cache hit the AIO machinery is bypassed entirely.
 *
 * Signal safety: handle_sigusr1() only sets cache_clear_requested.  The
 * actual cache_clear() call happens in the event loop, never inside the
 * signal handler, so no async-signal-safety concerns arise.
 */
typedef struct {
    char   key[PATH_MAX]; /* resolved canonical path */
    char  *resp;          /* malloc'd: "+OK <n>\n" + file bytes */
    size_t resp_size;
} CacheEntry;

static CacheEntry cache[MAX_CACHE_ENTRIES];
static int        cache_size = 0;

static CacheEntry *cache_lookup(const char *key) {
    for (int i = 0; i < cache_size; i++)
        if (strcmp(cache[i].key, key) == 0)
            return &cache[i];
    return NULL;
}

static void cache_insert(const char *key, char *resp, size_t resp_size) {
    for (int i = 0; i < cache_size; i++) {
        if (strcmp(cache[i].key, key) == 0) {
            free(cache[i].resp);
            cache[i].resp      = resp;
            cache[i].resp_size = resp_size;
            return;
        }
    }
    if (cache_size == MAX_CACHE_ENTRIES) {
        free(cache[0].resp);
        memmove(cache, cache + 1, sizeof(CacheEntry) * (MAX_CACHE_ENTRIES - 1));
        cache_size--;
    }
    strncpy(cache[cache_size].key, key, PATH_MAX - 1);
    cache[cache_size].key[PATH_MAX - 1] = '\0';
    cache[cache_size].resp      = resp;
    cache[cache_size].resp_size = resp_size;
    cache_size++;
}

static void cache_clear(void) {
    int n = cache_size;
    for (int i = 0; i < cache_size; i++) { free(cache[i].resp); cache[i].resp = NULL; }
    cache_size = 0;
    fprintf(stderr, "[cache] cleared (%d entries flushed)\n", n);
}

/* ── per-client state ───────────────────────────────────────────────────────── */

typedef enum {
    STATE_READING,   /* accumulating the request line                     */
    STATE_AIO,       /* aio_read() is in-flight for a small file          */
    STATE_WRITING,   /* sending the response buffer to the client socket  */
    STATE_SENDFILE,  /* sendfile() in progress for a large (>64 KB) file  */
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

    /* sendfile path (large files > MAX_CACHE_FILE_SIZE) ------------------- */
    int              sf_fd;         /* source file fd (open during STATE_SENDFILE)  */
    off_t            sf_offset;     /* current sendfile file offset                 */
    size_t           sf_remaining;  /* bytes left to transfer                       */

    /* cache --------------------------------------------------------------- */
    char             cache_key[PATH_MAX]; /* resolved path, set in begin_file_serve */
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
        while (aio_error(&c->aio) == EINPROGRESS)
            ;
        aio_return(&c->aio);
        close(c->file_fd);
    }
    /* Close the sendfile source fd. */
    if (c->state == STATE_SENDFILE && c->sf_fd >= 0)
        close(c->sf_fd);
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
    set_nonblocking(c->sock);
    printf("[%s:%d] connected (%d clients)\n", c->ip, c->port, num_clients);
}

/* Write up to len bytes to fd, returning immediately on EAGAIN (partial is OK). */
static ssize_t nb_write(int fd, const char *buf, size_t len) {
    ssize_t n = write(fd, buf, len);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        return 0;
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
 *
 * Cache hit:        copies the cached response into a new buffer and transitions
 *                   the client directly to STATE_WRITING — no file open, no AIO.
 * Cache miss, small (≤ MAX_CACHE_FILE_SIZE):
 *                   validates the path, opens the file, builds the response
 *                   buffer header, and issues aio_read() to fill the file portion.
 *                   AIO completion is reported via AIO_DONE_SIG → signalfd.
 * Cache miss, large (> MAX_CACHE_FILE_SIZE):
 *                   sends the "+OK N\n" header synchronously (small, fits in the
 *                   socket buffer), then transfers the file body with sendfile(2)
 *                   via non-blocking STATE_SENDFILE iterations.  No user-space
 *                   copy, no malloc for the body.
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

    /* Save the resolved path so check_aio() can use it as the cache key. */
    strncpy(c->cache_key, resolved, PATH_MAX - 1);
    c->cache_key[PATH_MAX - 1] = '\0';

    /* ── Cache hit: skip all disk I/O ── */
    CacheEntry *ce = cache_lookup(resolved);
    if (ce) {
        char *resp_copy = malloc(ce->resp_size);
        if (resp_copy) {
            memcpy(resp_copy, ce->resp, ce->resp_size);
            c->resp      = resp_copy;
            c->resp_size = ce->resp_size;
            c->resp_sent = 0;
            c->state     = STATE_WRITING;
            printf("[%s:%d] serving '%s' [cache]\n", c->ip, c->port, filename);
            return;
        }
        /* malloc failed; fall through to the disk path */
    }

    /* ── Stat the file (needed for both paths) ── */
    struct stat st;
    if (stat(resolved, &st) < 0 || !S_ISREG(st.st_mode)) {
        sock_send(c->sock, !S_ISREG(st.st_mode)
                  ? "-ERR not a regular file\n" : "-ERR stat failed\n");
        return;
    }

    int file_fd = open(resolved, O_RDONLY);
    if (file_fd < 0) { sock_send(c->sock, "-ERR open failed\n"); return; }

    char header[64];
    int hlen = snprintf(header, sizeof(header), "+OK %lld\n", (long long)st.st_size);

    /* ── Large file: sendfile path (no user-space copy, no cache) ── */
    if ((size_t)st.st_size > MAX_CACHE_FILE_SIZE) {
        /*
         * Write the small header synchronously.  It fits in a single TCP
         * segment and the socket buffer is always large enough; sock_send()
         * loops until every byte is sent.
         */
        sock_send(c->sock, header);

        c->sf_fd        = file_fd;
        c->sf_offset    = 0;
        c->sf_remaining = (size_t)st.st_size;
        c->state        = STATE_SENDFILE;
        printf("[%s:%d] sendfile started for '%s' (%lld bytes)\n",
               c->ip, c->port, filename, (long long)st.st_size);
        return;
    }

    /* ── Small file: AIO path (reads into buffer, populates cache) ── */
    char *resp = malloc((size_t)hlen + (size_t)st.st_size);
    if (!resp) {
        sock_send(c->sock, "-ERR out of memory\n");
        close(file_fd);
        return;
    }
    memcpy(resp, header, (size_t)hlen);

    c->file_fd         = file_fd;
    c->resp            = resp;
    c->resp_size       = (size_t)hlen + (size_t)st.st_size;
    c->resp_sent       = 0;
    c->aio             = (struct aiocb){0};
    c->aio.aio_fildes  = file_fd;
    c->aio.aio_buf     = resp + hlen;
    c->aio.aio_nbytes  = (size_t)st.st_size;
    c->aio.aio_offset  = 0;
    /* Deliver AIO_DONE_SIG when the read completes; signalfd wakes select(). */
    c->aio.aio_sigevent.sigev_notify = SIGEV_SIGNAL;
    c->aio.aio_sigevent.sigev_signo  = AIO_DONE_SIG;

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
 * On success, caches the completed response (if small enough) before
 * transitioning to STATE_WRITING.
 */
static int check_aio(Client *c) {
    int err = aio_error(&c->aio);

    if (err == EINPROGRESS)
        return 0;

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

    /* Populate the cache (only for responses that fit within the limit). */
    if (c->resp_size <= MAX_CACHE_FILE_SIZE + 64 && c->cache_key[0] != '\0') {
        char *cached = malloc(c->resp_size);
        if (cached) {
            memcpy(cached, c->resp, c->resp_size);
            cache_insert(c->cache_key, cached, c->resp_size);
        }
    }

    printf("[%s:%d] aio_read complete (%zd bytes), entering WRITING\n",
           c->ip, c->port, ret);
    c->state = STATE_WRITING;
    return 1;
}

/* ── event loop ──────────────────────────────────────────────────────────────── */

static void event_loop(int listen_fd, int aio_sfd) {
    while (running) {
        /* Process any pending cache-clear signal before blocking. */
        if (cache_clear_requested) {
            cache_clear_requested = 0;
            cache_clear();
        }

        /*
         * Build read_fds (listen + aio_sfd + all clients) and
         * write_fds (STATE_WRITING and STATE_SENDFILE clients).
         */
        fd_set read_fds, write_fds;
        FD_ZERO(&read_fds);
        FD_ZERO(&write_fds);
        FD_SET(listen_fd, &read_fds);
        FD_SET(aio_sfd,   &read_fds);
        int max_fd = aio_sfd > listen_fd ? aio_sfd : listen_fd;

        for (int i = 0; i < num_clients; i++) {
            int fd = clients[i].sock;
            FD_SET(fd, &read_fds);
            if (clients[i].state == STATE_WRITING ||
                clients[i].state == STATE_SENDFILE)
                FD_SET(fd, &write_fds);
            if (fd > max_fd) max_fd = fd;
        }

        /*
         * No AIO polling timeout: AIO_DONE_SIG is delivered via aio_sfd so
         * select() wakes up exactly when a read completes.  Pass tvp=NULL to
         * block until a real event (socket I/O, AIO completion, or signal).
         */
        int ready = select(max_fd + 1, &read_fds, &write_fds, NULL, NULL);
        if (ready < 0) {
            /*
             * SIGINT  → running=0,               loop exits via while condition.
             * SIGUSR1 → cache_clear_requested=1, handled at top of next iter.
             */
            if (errno == EINTR) continue;
            perror("select");
            break;
        }

        /* Drain AIO completion signals so aio_sfd does not fill up. */
        if (FD_ISSET(aio_sfd, &read_fds)) {
            struct signalfd_siginfo ssi;
            while (read(aio_sfd, &ssi, sizeof(ssi)) == (ssize_t)sizeof(ssi))
                ;   /* drain all queued signals */
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

            /* Detect EOF / disconnect on any client in any state. */
            if (FD_ISSET(c->sock, &read_fds) && c->state != STATE_READING) {
                char probe;
                ssize_t n = recv(c->sock, &probe, 1, MSG_PEEK);
                if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                    remove_client(i);
                    continue;
                }
            }

            /* STATE_AIO: check for completion (signalfd woke us up). */
            if (c->state == STATE_AIO) {
                check_aio(c);
            }

            /* STATE_READING: read from socket. */
            if (c->state == STATE_READING && FD_ISSET(c->sock, &read_fds)) {
                int space = REQ_BUF_SIZE - 1 - c->req_len;
                ssize_t n = read(c->sock, c->req_buf + c->req_len, (size_t)space);
                if (n <= 0) { remove_client(i); continue; }
                c->req_len += (int)n;

                char *nl;
                while ((nl = memchr(c->req_buf, '\n', (size_t)c->req_len))) {
                    *nl = '\0';
                    int len = (int)(nl - c->req_buf);
                    if (len > 0 && c->req_buf[len-1] == '\r') c->req_buf[--len] = '\0';

                    begin_file_serve(c, c->req_buf);

                    int consumed = (int)(nl - c->req_buf) + 1;
                    c->req_len  -= consumed;
                    memmove(c->req_buf, nl + 1, (size_t)c->req_len);

                    if (c->state != STATE_READING) break;
                }
                if (c->req_len >= REQ_BUF_SIZE - 1) { remove_client(i); continue; }
            }

            /* STATE_WRITING: send response chunk (from in-memory buffer). */
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

            /* STATE_SENDFILE: zero-copy transfer of a large file body. */
            if (c->state == STATE_SENDFILE && FD_ISSET(c->sock, &write_fds)) {
                ssize_t sent = sendfile(c->sock, c->sf_fd,
                                        &c->sf_offset, c->sf_remaining);
                if (sent < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK)
                        ; /* socket buffer full — retry on next writable event */
                    else {
                        remove_client(i);
                        continue;
                    }
                } else {
                    c->sf_remaining -= (size_t)sent;
                }
                if (c->sf_remaining == 0) {
                    printf("[%s:%d] sendfile complete (%lld bytes)\n",
                           c->ip, c->port, (long long)c->sf_offset);
                    close(c->sf_fd); c->sf_fd = -1;
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
    signal(SIGUSR1, handle_sigusr1);
    signal(SIGPIPE, SIG_IGN);

    /*
     * Block AIO_DONE_SIG so it is not delivered as a signal but instead
     * accumulates in the signalfd queue.  select() will wake up when
     * aio_sfd becomes readable, which happens the moment aio_read()
     * completes — no 1 ms polling timeout needed.
     */
    sigset_t aio_mask;
    sigemptyset(&aio_mask);
    sigaddset(&aio_mask, AIO_DONE_SIG);
    if (sigprocmask(SIG_BLOCK, &aio_mask, NULL) < 0) {
        perror("sigprocmask"); return EXIT_FAILURE;
    }
    int aio_sfd = signalfd(-1, &aio_mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (aio_sfd < 0) { perror("signalfd"); return EXIT_FAILURE; }

    int listen_fd = create_listen_socket(port);
    printf("AIO file server listening on port %d, docroot=%s\n"
           "(send SIGUSR1 to flush the file cache)\n", port, docroot);

    event_loop(listen_fd, aio_sfd);

    for (int i = 0; i < num_clients; i++) close(clients[i].sock);
    close(listen_fd);
    close(aio_sfd);
    cache_clear();
    printf("\nServer shut down.\n");
    return 0;
}
