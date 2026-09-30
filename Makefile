# ricoded - small Unix lyrics viewer (see README.md)
#
# GNU make and BSD make compatible.

CC      = gcc
CFLAGS  = -std=c99 -Wall -Wextra -Wpedantic -O2
PREFIX ?= /usr/local
BINDIR  = $(PREFIX)/bin

all: ricoded ricoded-ng

ricoded: ricoded.c
	$(CC) $(CFLAGS) -o $@ ricoded.c

ricoded-ng: ricoded-ng.c
	$(CC) $(CFLAGS) -o $@ ricoded-ng.c

clean:
	rm -f ricoded ricoded-ng

install: all
	install -d "$(DESTDIR)$(BINDIR)"
	install -m 755 ricoded ricoded-ng "$(DESTDIR)$(BINDIR)"

.PHONY: all clean install
