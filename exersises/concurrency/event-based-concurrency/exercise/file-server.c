/* Enable all glibc extensions (realpath, PATH_MAX, etc.) under -std=c11. */
#define _GNU_SOURCE

/*
 * Exercise 3: select()-based file-serving TCP server
 *
 * Protocol (line-oriented, connection stays open for multiple requests):
 *
 *   Client → Server:   <relative-filename>\n
 *   Server → Client:   +OK <size>\n<size bytes of file content>
 *                  or: -ERR <reason>\n
 *
 * Security measures:
 *   • All files are served from a configurable DOCROOT directory only.
 *   • realpath() resolves symlinks and ".." before any path is opened.
 *   • The resolved path must have DOCROOT as a strict prefix; any request
 *     that escapes the root is rejected with -ERR.
 *   • Absolute paths in requests are rejected immediately.
 *   • Only regular files are served (no directories, devices, etc.).
 *
 * File cache (exercise 5):
 *   • A small in-process LRU-like cache stores recently served files.
 *   • Sending SIGUSR1 flushes the entire cache (administrative action).
 *
 * Usage:
 *   ./file-server [port [docroot]]
 *   Default port:    8082
 *   Default docroot: ./docroot
 *
 * Example:
 *   ./file-server 8082 ./docroot
 *   echo "hello.txt" | nc localhost 8082
 *   kill -USR1 <pid>    # flush cache
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define DEFAULT_PORT    8082
#define BACKLOG         128
#define MAX_CLIENTS     64
#define REQ_BUF_SIZE    512
#define FILE_BUF_SIZE   4096

/* ── file cache constants ──────────────────────────────────────────────────── */

#define MAX_CACHE_ENTRIES   16
#define MAX_CACHE_FILE_SIZE (64 * 1024)   /* do not cache files larger than 64 KB */

/* ── globals ───────────────────────────────────────────────────────────────── */

static volatile int            running              = 1;
static volatile sig_atomic_t   cache_clear_requested = 0;
static char                    docroot[PATH_MAX];

static void handle_sigint(int sig)   { (void)sig; running = 0; }
static void handle_sigusr1(int sig)  { (void)sig; cache_clear_requested = 1; }

/* ── file cache ────────────────────────────────────────────────────────────── */

/*
 * Each entry stores the full pre-built response: "+OK <n>\n" + file bytes.
 * Sending from cache is a single write_all() call — no disk I/O.
 *
 * Eviction policy: FIFO.  When the table is full the oldest entry (index 0)
 * is freed and the rest shift down by one slot.
 *
 * Thread-safety: the server is single-threaded so no locking is needed.
 * The signal handler only sets a flag; the actual cache clear happens in the
 * main loop body, never inside the handler.
 */
typedef struct {
    char   key[PATH_MAX]; /* resolved canonical path of the file      */
    char  *resp;          /* malloc'd buffer: header + file content   */
    size_t resp_size;     /* total bytes in resp                      */
} CacheEntry;

static CacheEntry cache[MAX_CACHE_ENTRIES];
static int        cache_size = 0;

static CacheEntry *cache_lookup(const char *key) {
    for (int i = 0; i < cache_size; i++)
        if (strcmp(cache[i].key, key) == 0)
            return &cache[i];
    return NULL;
}

/* Takes ownership of resp on success. */
static void cache_insert(const char *key, char *resp, size_t resp_size) {
    /* Update in-place if the key already exists. */
    for (int i = 0; i < cache_size; i++) {
        if (strcmp(cache[i].key, key) == 0) {
            free(cache[i].resp);
            cache[i].resp      = resp;
            cache[i].resp_size = resp_size;
            return;
        }
    }
    /* Evict the oldest entry (index 0) when the table is full. */
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
    for (int i = 0; i < cache_size; i++) {
        free(cache[i].resp);
        cache[i].resp = NULL;
    }
    cache_size = 0;
    fprintf(stderr, "[cache] cleared (%d entries flushed)\n", n);
}

/* ── per-connection state ──────────────────────────────────────────────────── */
typedef struct {
    int  fd;
    char ip[INET_ADDRSTRLEN];
    int  port;
    char buf[REQ_BUF_SIZE];
    int  buf_len;
} Client;

static Client clients[MAX_CLIENTS];
static int    num_clients = 0;

/* ── helpers ───────────────────────────────────────────────────────────────── */

static int create_listen_socket(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); exit(EXIT_FAILURE); }

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    /* Non-blocking so we can drain all pending accepts at once. */
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);

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
    close(clients[i].fd);
    printf("[%s:%d] disconnected\n", clients[i].ip, clients[i].port);
    clients[i] = clients[--num_clients];
}

static void add_client(int fd, struct sockaddr_in *addr) {
    if (num_clients >= MAX_CLIENTS) {
        fprintf(stderr, "max clients reached – rejecting fd %d\n", fd);
        close(fd);
        return;
    }
    Client *c  = &clients[num_clients++];
    c->fd      = fd;
    c->buf_len = 0;
    c->port    = ntohs(addr->sin_port);
    inet_ntop(AF_INET, &addr->sin_addr, c->ip, sizeof(c->ip));
    printf("[%s:%d] connected (%d clients)\n", c->ip, c->port, num_clients);
}

/* Write all bytes in buf to fd, handling partial writes. */
static int write_all(int fd, const char *buf, size_t len) {
    while (len > 0) {
        ssize_t n = write(fd, buf, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        buf += n;
        len -= (size_t)n;
    }
    return 0;
}

/* Send a formatted error response to the client. */
static void send_error(int fd, const char *reason) {
    char msg[256];
    int len = snprintf(msg, sizeof(msg), "-ERR %s\n", reason);
    write_all(fd, msg, (size_t)len);
}

/*
 * Serve one file request.
 *
 * filename: the raw string the client sent (not yet validated).
 * Returns 0 on success, -1 if the connection should be closed.
 *
 * Cache behaviour:
 *   Hit  → send pre-built resp from cache; no disk I/O.
 *   Miss → read file, build full response, insert into cache, send.
 *   Large file (> MAX_CACHE_FILE_SIZE) → stream without caching.
 */
static int serve_file(Client *c, const char *filename) {
    /* ── Security check 1: reject empty names and absolute paths ── */
    if (filename[0] == '\0') {
        send_error(c->fd, "empty filename");
        return 0;
    }
    if (filename[0] == '/') {
        send_error(c->fd, "absolute paths not allowed");
        return 0;
    }

    /* ── Security check 2: resolve the full path and verify it stays
     *    inside docroot (prevents ../../etc/passwd escapes) ── */
    char candidate[PATH_MAX * 2];
    snprintf(candidate, sizeof(candidate), "%s/%s", docroot, filename);

    char resolved[PATH_MAX];
    if (realpath(candidate, resolved) == NULL) {
        send_error(c->fd, "not found");
        return 0;
    }

    size_t root_len = strlen(docroot);
    if (strncmp(resolved, docroot, root_len) != 0 ||
        (resolved[root_len] != '/' && resolved[root_len] != '\0')) {
        send_error(c->fd, "access denied");
        return 0;
    }

    /* ── Cache lookup (after path validation, before any disk I/O) ── */
    CacheEntry *ce = cache_lookup(resolved);
    if (ce) {
        if (write_all(c->fd, ce->resp, ce->resp_size) < 0) return -1;
        printf("[%s:%d] served '%s' [cache]\n", c->ip, c->port, filename);
        return 0;
    }

    /* ── Security check 3: only serve regular files ── */
    struct stat st;
    if (stat(resolved, &st) < 0) {
        send_error(c->fd, "stat failed");
        return 0;
    }
    if (!S_ISREG(st.st_mode)) {
        send_error(c->fd, "not a regular file");
        return 0;
    }

    int file_fd = open(resolved, O_RDONLY);
    if (file_fd < 0) {
        send_error(c->fd, "open failed");
        return 0;
    }

    /* Build the header once — used for both cache and streaming paths. */
    char header[64];
    int hlen = snprintf(header, sizeof(header), "+OK %lld\n", (long long)st.st_size);

    /* ── Cache-eligible path: read entire file into one buffer ── */
    if (st.st_size >= 0 && (size_t)st.st_size <= MAX_CACHE_FILE_SIZE) {
        size_t fsize     = (size_t)st.st_size;
        size_t resp_size = (size_t)hlen + fsize;
        char  *resp      = malloc(resp_size + 1); /* +1 to avoid malloc(0) */
        if (resp) {
            memcpy(resp, header, (size_t)hlen);
            ssize_t nr = (fsize > 0) ? pread(file_fd, resp + hlen, fsize, 0) : 0;
            if (nr == (ssize_t)fsize) {
                close(file_fd);
                if (write_all(c->fd, resp, resp_size) < 0) {
                    free(resp);
                    return -1;
                }
                cache_insert(resolved, resp, resp_size); /* resp ownership transferred */
                printf("[%s:%d] served '%s' (%lld bytes)\n",
                       c->ip, c->port, filename, (long long)st.st_size);
                return 0;
            }
            free(resp); /* pread failed; fall through to streaming */
        }
        /* malloc failure or pread error: fall through to streaming path. */
    }

    /* ── Streaming path: large file or allocation failure ── */
    if (write_all(c->fd, header, (size_t)hlen) < 0) {
        close(file_fd);
        return -1;
    }
    char fbuf[FILE_BUF_SIZE];
    ssize_t nr;
    while ((nr = read(file_fd, fbuf, sizeof(fbuf))) > 0) {
        if (write_all(c->fd, fbuf, (size_t)nr) < 0) {
            close(file_fd);
            return -1;
        }
    }
    close(file_fd);
    printf("[%s:%d] served '%s' (%lld bytes)\n",
           c->ip, c->port, filename, (long long)st.st_size);
    return 0;
}

/*
 * Process all complete newline-terminated lines buffered for this client.
 * Returns 0 normally, -1 if the connection should be torn down.
 */
static int process_requests(Client *c) {
    while (1) {
        char *nl = memchr(c->buf, '\n', (size_t)c->buf_len);
        if (!nl) break;

        *nl = '\0';

        /* Strip optional carriage return (for clients that send \r\n). */
        int len = (int)(nl - c->buf);
        if (len > 0 && c->buf[len - 1] == '\r')
            c->buf[--len] = '\0';

        if (serve_file(c, c->buf) < 0)
            return -1;

        /* Consume this request from the buffer. */
        int consumed = (int)(nl - c->buf) + 1;
        c->buf_len  -= consumed;
        memmove(c->buf, nl + 1, (size_t)c->buf_len);
    }

    if (c->buf_len >= REQ_BUF_SIZE - 1) {
        fprintf(stderr, "[%s:%d] request too long – closing\n", c->ip, c->port);
        return -1;
    }
    return 0;
}

/* ── event loop ────────────────────────────────────────────────────────────── */

static void event_loop(int listen_fd) {
    while (running) {
        /* Process any pending cache-clear signal before blocking in select(). */
        if (cache_clear_requested) {
            cache_clear_requested = 0;
            cache_clear();
        }

        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(listen_fd, &read_fds);
        int max_fd = listen_fd;

        for (int i = 0; i < num_clients; i++) {
            FD_SET(clients[i].fd, &read_fds);
            if (clients[i].fd > max_fd) max_fd = clients[i].fd;
        }

        int ready = select(max_fd + 1, &read_fds, NULL, NULL, NULL);
        if (ready < 0) {
            /*
             * EINTR means a signal interrupted select().  Either SIGINT set
             * running=0 (loop exits via while condition) or SIGUSR1 set
             * cache_clear_requested=1 (handled at the top of next iteration).
             * Either way, continue re-evaluates the loop condition and flags.
             */
            if (errno == EINTR) continue;
            perror("select");
            break;
        }

        /* Drain all pending new connections. */
        if (FD_ISSET(listen_fd, &read_fds)) {
            while (1) {
                struct sockaddr_in addr;
                socklen_t len = sizeof(addr);
                int cfd = accept(listen_fd, (struct sockaddr *)&addr, &len);
                if (cfd < 0) {
                    if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                        perror("accept");
                    break;
                }
                add_client(cfd, &addr);
            }
        }

        for (int i = num_clients - 1; i >= 0; i--) {
            if (!FD_ISSET(clients[i].fd, &read_fds)) continue;

            int space = REQ_BUF_SIZE - 1 - clients[i].buf_len;
            ssize_t n = read(clients[i].fd,
                             clients[i].buf + clients[i].buf_len,
                             (size_t)space);
            if (n <= 0) {
                if (n < 0 && errno != ECONNRESET) perror("read");
                remove_client(i);
                continue;
            }
            clients[i].buf_len += (int)n;
            if (process_requests(&clients[i]) < 0)
                remove_client(i);
        }
    }
}

/* ── main ──────────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    int         port    = DEFAULT_PORT;
    const char *rootarg = "./docroot";

    if (argc >= 2) port    = atoi(argv[1]);
    if (argc >= 3) rootarg = argv[2];

    /* Resolve docroot to a canonical absolute path once at startup. */
    if (realpath(rootarg, docroot) == NULL) {
        fprintf(stderr, "docroot '%s': %s\n", rootarg, strerror(errno));
        return EXIT_FAILURE;
    }

    signal(SIGINT,  handle_sigint);
    signal(SIGUSR1, handle_sigusr1); /* SIGUSR1 flushes the file cache */
    /* Ignore SIGPIPE so a broken client connection doesn't kill the server. */
    signal(SIGPIPE, SIG_IGN);

    int listen_fd = create_listen_socket(port);
    printf("File server listening on port %d, docroot=%s\n"
           "(send SIGUSR1 to flush the file cache)\n", port, docroot);

    event_loop(listen_fd);

    for (int i = 0; i < num_clients; i++) close(clients[i].fd);
    close(listen_fd);
    cache_clear();
    printf("\nServer shut down.\n");
    return 0;
}
