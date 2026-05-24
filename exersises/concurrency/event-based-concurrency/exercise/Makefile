CC      = gcc
CFLAGS  = -Wall -Wextra -std=c11 -g

TARGETS = tcp-time-server select-server file-server

.PHONY: all clean

all: $(TARGETS)

tcp-time-server: tcp-time-server.c
	$(CC) $(CFLAGS) -o $@ $<

select-server: select-server.c
	$(CC) $(CFLAGS) -o $@ $<

file-server: file-server.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(TARGETS)
