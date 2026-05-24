/*
 * Exercise 1: Simple sequential TCP server
 *
 * Accepts and serves one TCP connection at a time.
 * Each request returns the current time of day.
 *
 * Usage:
 *   ./tcp-time-server [port]   (default port: 8080)
 *
 * Test:
 *   nc localhost 8080
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>

#define DEFAULT_PORT 8080
#define BACKLOG      8
#define BUF_SIZE     256

static volatile int running = 1;

static void handle_sigint(int sig) {
    (void)sig;
    running = 0;
}

/* Return the current time as a human-readable string (no trailing newline). */
static void current_time_str(char *buf, size_t len) {
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    strftime(buf, len, "%Y-%m-%d %H:%M:%S", tm_info);
}

/* Handle a single accepted connection: send time, then close. */
static void serve_connection(int client_fd, const char *client_ip) {
    char time_buf[64];
    char response[BUF_SIZE];

    current_time_str(time_buf, sizeof(time_buf));
    snprintf(response, sizeof(response), "Current time: %s\n", time_buf);

    ssize_t sent = write(client_fd, response, strlen(response));
    if (sent < 0)
        perror("write");

    printf("  served %s -> %s", client_ip, response);
}

/* Create, bind, and listen on a TCP socket. Returns the listening fd. */
static int create_listen_socket(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); exit(EXIT_FAILURE); }

    /* Allow rapid restart without waiting for TIME_WAIT to expire. */
    int opt = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt");
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons((uint16_t)port),
        .sin_addr.s_addr = INADDR_ANY,
    };

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        exit(EXIT_FAILURE);
    }

    if (listen(fd, BACKLOG) < 0) {
        perror("listen");
        exit(EXIT_FAILURE);
    }

    return fd;
}

int main(int argc, char *argv[]) {
    int port = DEFAULT_PORT;
    if (argc == 2)
        port = atoi(argv[1]);

    signal(SIGINT, handle_sigint);

    int listen_fd = create_listen_socket(port);
    printf("Sequential TCP time server listening on port %d (Ctrl-C to quit)\n", port);

    while (running) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);

        int client_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &addr_len);
        if (client_fd < 0) {
            if (errno == EINTR)   /* interrupted by signal */
                break;
            perror("accept");
            continue;
        }

        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
        printf("Connection from %s:%d\n", client_ip, ntohs(client_addr.sin_port));

        /* Serve the request synchronously — the server is blocked here
         * until this connection is fully handled before accepting the next. */
        serve_connection(client_fd, client_ip);

        close(client_fd);
    }

    close(listen_fd);
    printf("\nServer shut down.\n");
    return 0;
}
