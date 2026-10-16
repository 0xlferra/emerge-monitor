# SPDX-License-Identifier: GPL-2.0-or-later
PREFIX    ?= /usr/local
BINDIR    ?= $(PREFIX)/bin
DESTDIR   ?=
PKG_CONFIG ?= pkg-config

CC      ?= cc
CFLAGS  ?= -O2 -g
LDFLAGS ?=
# Always applied, even when CFLAGS is given on the command line.
override CFLAGS   += -std=c11 -Wall -Wextra -Wformat=2 -Wshadow
override CPPFLAGS += -D_GNU_SOURCE

SQLITE_CFLAGS := $(shell $(PKG_CONFIG) --cflags sqlite3)
SQLITE_LIBS   := $(shell $(PKG_CONFIG) --libs sqlite3)
CURL_CFLAGS   := $(shell $(PKG_CONFIG) --cflags libcurl)
CURL_LIBS     := $(shell $(PKG_CONFIG) --libs libcurl)

COMMON_SRC := src/common/sbuf.c src/common/emerge_log.c src/common/util.c
COMMON_OBJ := $(COMMON_SRC:.c=.o)
SERVER_OBJ := src/server/main.o src/server/store.o src/server/dashboard.o
AGENT_OBJ  := src/agent/main.o

.PHONY: all clean check install
all: emon-server emon-agent

emon-server: $(SERVER_OBJ) $(COMMON_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(SQLITE_LIBS)

emon-agent: $(AGENT_OBJ) $(COMMON_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(CURL_LIBS)

src/server/%.o: src/server/%.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SQLITE_CFLAGS) -c -o $@ $<

src/agent/%.o: src/agent/%.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(CURL_CFLAGS) -c -o $@ $<

src/common/%.o: src/common/%.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

# Embed the dashboard as a C string.
src/server/dashboard.c: src/server/dashboard.html tools/embed.awk
	awk -f tools/embed.awk -v name=dashboard_html $< > $@

tests/test_parser: tests/test_parser.c $(COMMON_OBJ)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $^

check: tests/test_parser all
	./tests/test_parser tests/data/emerge.log
	./tests/integration.sh

install: all
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 emon-server emon-agent $(DESTDIR)$(BINDIR)

clean:
	rm -f emon-server emon-agent tests/test_parser src/server/dashboard.c \
	      src/*/*.o
