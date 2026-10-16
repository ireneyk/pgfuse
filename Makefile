# ============================================================================
# Project:       PG-FS (PostgreSQL-Backed Virtual File System)
# Author:        Irene Therese Joseph
# Role:          Senior Linux Systems Engineer
# Description:   Production Makefile for compiling PG-FS with pkg-config
#                auto-discovery for libfuse (fuse3 / fuse) and PostgreSQL libpq.
# ============================================================================

CC       ?= gcc
PREFIX   ?= /usr/local
BINDIR   ?= $(PREFIX)/bin

# Auto-detect FUSE library (fuse3 preferred, fallback to legacy fuse)
FUSE_PKG := $(shell pkg-config --exists fuse3 && echo fuse3 || (pkg-config --exists fuse && echo fuse || echo none))

ifeq ($(FUSE_PKG),none)
    $(warning [PGFS-BUILD] Neither 'fuse3' nor 'fuse' pkg-config found. Defaulting to system flags.)
    FUSE_CFLAGS := -D_FILE_OFFSET_BITS=64 -I/usr/include/fuse3
    FUSE_LIBS   := -lfuse3
else
    FUSE_CFLAGS := $(shell pkg-config --cflags $(FUSE_PKG))
    FUSE_LIBS   := $(shell pkg-config --libs $(FUSE_PKG))
endif

# Auto-detect PostgreSQL libpq
LIBPQ_CFLAGS := $(shell pkg-config --cflags libpq 2>/dev/null || pg_config --cflags 2>/dev/null || echo -I/usr/include/postgresql)
LIBPQ_LIBS   := $(shell pkg-config --libs libpq 2>/dev/null || pg_config --libs 2>/dev/null || echo -lpq)

# Strict enterprise compiler flags
CFLAGS   += -std=c11 -Wall -Wextra -Wno-unused-parameter -D_GNU_SOURCE \
            -O2 -g -pthread $(FUSE_CFLAGS) $(LIBPQ_CFLAGS)
LDFLAGS  += -pthread $(FUSE_LIBS) $(LIBPQ_LIBS)

TARGET   := pgfs
SOURCES  := pgfs.c
OBJECTS  := $(SOURCES:.c=.o)

.PHONY: all clean install uninstall check test-schema help

all: check $(TARGET)

$(TARGET): $(OBJECTS)
	@echo "  [LD]    $@"
	@$(CC) $(OBJECTS) $(LDFLAGS) -o $@
	@echo "  [BUILD] Successfully built $(TARGET) with $(FUSE_PKG) and libpq."

%.o: %.c
	@echo "  [CC]    $<"
	@$(CC) $(CFLAGS) -c $< -o $@

check:
	@echo "============================================================"
	@echo " PG-FS: Verifying Build Toolchain and Dependencies"
	@echo "============================================================"
	@echo "  FUSE Provider:    $(FUSE_PKG)"
	@echo "  FUSE Cflags:      $(FUSE_CFLAGS)"
	@echo "  FUSE Libs:        $(FUSE_LIBS)"
	@echo "  libpq Cflags:     $(LIBPQ_CFLAGS)"
	@echo "  libpq Libs:       $(LIBPQ_LIBS)"
	@echo "============================================================"

test-schema:
	@echo "  [PG-INIT] Bootstrapping PostgreSQL schema using schema.sql..."
	@psql -h localhost -U postgres -d pgfs -f schema.sql || \
		(echo "[PG-ERROR] Failed to bootstrap schema. Ensure database 'pgfs' exists." && exit 1)

install: $(TARGET)
	@echo "  [INSTALL] Installing $(TARGET) to $(DESTDIR)$(BINDIR)..."
	@install -d $(DESTDIR)$(BINDIR)
	@install -m 755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)

uninstall:
	@echo "  [UNINSTALL] Removing $(TARGET) from $(DESTDIR)$(BINDIR)..."
	@rm -f $(DESTDIR)$(BINDIR)/$(TARGET)

clean:
	@echo "  [CLEAN] Removing object files and binary..."
	@rm -f $(OBJECTS) $(TARGET)

help:
	@echo "PG-FS Build System Targets:"
	@echo "  make            - Build the pgfs virtual filesystem binary"
	@echo "  make check      - Verify pkg-config flags for FUSE and libpq"
	@echo "  make test-schema- Bootstrap schema.sql into local PostgreSQL instance"
	@echo "  make install    - Install pgfs binary into $(DESTDIR)$(BINDIR)"
	@echo "  make clean      - Clean object files and binaries"
