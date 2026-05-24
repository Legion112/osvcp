CC      = gcc
CFLAGS  = -Wall -Wextra -std=c11 -g

TARGETS = tcp-time-server

.PHONY: all clean

all: $(TARGETS)

tcp-time-server: tcp-time-server.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(TARGETS)
