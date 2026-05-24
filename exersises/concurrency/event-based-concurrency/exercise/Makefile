CC      = gcc
CFLAGS  = -Wall -Wextra -std=c11 -g

TARGETS = tcp-time-server select-server file-server aio-file-server

.PHONY: all clean rust rust-file-server rust-tokio

all: $(TARGETS) rust

tcp-time-server: tcp-time-server.c
	$(CC) $(CFLAGS) -o $@ $<

select-server: select-server.c
	$(CC) $(CFLAGS) -o $@ $<

file-server: file-server.c
	$(CC) $(CFLAGS) -o $@ $<

# Exercise 4: POSIX aio_read() version; -lrt for the POSIX realtime library
aio-file-server: aio-file-server.c
	$(CC) $(CFLAGS) -o $@ $< -lrt

rust: rust-file-server rust-tokio

rust-file-server:
	cargo build --manifest-path rust-file-server/Cargo.toml

rust-tokio:
	cargo build --manifest-path tokio-file-server/Cargo.toml

clean:
	rm -f $(TARGETS)
	cargo clean --manifest-path rust-file-server/Cargo.toml 2>/dev/null || true
	cargo clean --manifest-path tokio-file-server/Cargo.toml 2>/dev/null || true
