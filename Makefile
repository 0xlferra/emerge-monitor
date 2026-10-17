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

# Evaluated only when used, so building just one program needs just its
# dependency.
CURL_CFLAGS = $(shell $(PKG_CONFIG) --cflags libcurl)
CURL_LIBS   = $(shell $(PKG_CONFIG) --libs libcurl)

# SQLITE_SRC=/path/to/sqlite3.c builds SQLite into emon-server instead of
# linking the system library (used for static and cross builds).
ifdef SQLITE_SRC
SQLITE_CFLAGS = -I$(dir $(SQLITE_SRC))
SQLITE_LIBS   =
SQLITE_OBJ    = src/server/sqlite3.o
else
SQLITE_CFLAGS = $(shell $(PKG_CONFIG) --cflags sqlite3)
SQLITE_LIBS   = $(shell $(PKG_CONFIG) --libs sqlite3)
SQLITE_OBJ    =
endif
# The server is single threaded and never loads extensions.
SQLITE_DEFS = -DSQLITE_THREADSAFE=0 -DSQLITE_OMIT_LOAD_EXTENSION \
              -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_DQS=0

COMMON_SRC := src/common/sbuf.c src/common/emerge_log.c src/common/util.c
COMMON_OBJ := $(COMMON_SRC:.c=.o)
SERVER_OBJ := src/server/main.o src/server/store.o src/server/buildlog.o \
              src/server/dashboard.o
AGENT_OBJ  := src/agent/main.o

.PHONY: all clean check install
all: emon-server emon-agent

emon-server: $(SERVER_OBJ) $(SQLITE_OBJ) $(COMMON_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(SQLITE_LIBS)

# Third-party code: our warning flags would only add noise.
src/server/sqlite3.o: $(SQLITE_SRC)
	$(CC) $(CPPFLAGS) $(filter-out -W%,$(CFLAGS)) $(SQLITE_DEFS) -c -o $@ $<

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

tests/test_buildlog: tests/test_buildlog.c src/server/buildlog.o $(COMMON_OBJ)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $^

check: tests/test_parser tests/test_buildlog all
	./tests/test_parser tests/data/emerge.log
	./tests/test_buildlog
	./tests/integration.sh

install: all
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 emon-server emon-agent $(DESTDIR)$(BINDIR)

clean:
	rm -f emon-server emon-agent tests/test_parser tests/test_buildlog src/server/dashboard.c \
	      src/*/*.o
