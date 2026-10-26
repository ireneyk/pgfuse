# ============================================================================
# Makefile for pgfuse (PostgreSQL-Backed Virtual File System)
#
# Builds the 'pgfuse' binary from C source code.
# Automatically detects libfuse3 and PostgreSQL libpq libraries.
# ============================================================================

# Default C compiler
CC       ?= gcc
PREFIX   ?= /usr/local
BINDIR   ?= $(PREFIX)/bin

# Auto-detect FUSE library using pkg-config (prefers fuse3, falls back to fuse)
FUSE_PKG := $(shell pkg-config --exists fuse3 && echo fuse3 || (pkg-config --exists fuse && echo fuse || echo none))

ifeq ($(FUSE_PKG),none)
    # If pkg-config is not available, fall back to standard 64-bit FUSE 3 flags
    $(warning [PGFUSE-BUILD] Neither 'fuse3' nor 'fuse' pkg-config found. Defaulting to system flags.)
    FUSE_CFLAGS := -D_FILE_OFFSET_BITS=64 -I/usr/include/fuse3
    FUSE_LIBS   := -lfuse3
else
    # Query compiler and linker flags directly from pkg-config
    FUSE_CFLAGS := $(shell pkg-config --cflags $(FUSE_PKG))
    FUSE_LIBS   := $(shell pkg-config --libs $(FUSE_PKG))
endif

# Auto-detect PostgreSQL client library (libpq)
LIBPQ_CFLAGS := $(shell pkg-config --cflags libpq 2>/dev/null || pg_config --cflags 2>/dev/null || echo -I/usr/include/postgresql)
LIBPQ_LIBS   := $(shell pkg-config --libs libpq 2>/dev/null || pg_config --libs 2>/dev/null || echo -lpq)

# Compiler flags:
# -std=c11            : Use modern C11 standard
# -Wall -Wextra       : Enable helpful compiler warnings to keep code clean
# -D_GNU_SOURCE       : Enable POSIX/GNU string functions like strtok_r
# -O2                 : Optimize execution speed
# -pthread            : Enable multi-threading support (used for connection pool)
# -Iinclude           : Tell compiler to search the include/ folder for pgfuse.h
INCLUDES := -Iinclude
CFLAGS   += -std=c11 -Wall -Wextra -Wno-unused-parameter -D_GNU_SOURCE \
            -O2 -g -pthread $(INCLUDES) $(FUSE_CFLAGS) $(LIBPQ_CFLAGS)

# Linker flags: links POSIX threads, libfuse3, and PostgreSQL libpq
LDFLAGS  += -pthread $(FUSE_LIBS) $(LIBPQ_LIBS)

# Target binary name and source files
TARGET   := pgfuse
SOURCES  := src/pgfuse.c
OBJECTS  := $(SOURCES:.c=.o)

# Default password for local test environment
PGPASSWORD ?= postgres
export PGPASSWORD

.PHONY: all clean install uninstall check test-schema help

# Default target: check dependencies and compile the binary
all: check $(TARGET)

# Link compiled object files into the final pgfuse executable
$(TARGET): $(OBJECTS)
	@echo "  [LD]    $@"
	@$(CC) $(OBJECTS) $(LDFLAGS) -o $@
	@echo "  [BUILD] Successfully built $(TARGET) with $(FUSE_PKG) and libpq."

# Compile each .c source file into a .o object file
%.o: %.c
	@echo "  [CC]    $<"
	@$(CC) $(CFLAGS) -c $< -o $@

# Verify that all needed libraries are present on the system
check:
	@echo "============================================================"
	@echo " pgfuse: Checking build tools and libraries"
	@echo "============================================================"
	@echo "  FUSE Provider:    $(FUSE_PKG)"
	@echo "  FUSE Cflags:      $(FUSE_CFLAGS)"
	@echo "  FUSE Libs:        $(FUSE_LIBS)"
	@echo "  libpq Cflags:     $(LIBPQ_CFLAGS)"
	@echo "  libpq Libs:       $(LIBPQ_LIBS)"
	@echo "============================================================"

# Helper target to reload the SQL schema into PostgreSQL
test-schema:
	@echo "  [PG-INIT] Bootstrapping PostgreSQL schema using sql/schema.sql..."
	@PGPASSWORD=$(PGPASSWORD) psql -h localhost -U postgres -d pgfs -f sql/schema.sql 2>/dev/null || \
		sudo -u postgres psql -d pgfs -f sql/schema.sql || \
		(echo "[PG-ERROR] Failed to bootstrap schema. Ensure database 'pgfs' exists." && exit 1)

# Install the binary system-wide
install: $(TARGET)
	@echo "  [INSTALL] Installing $(TARGET) to $(DESTDIR)$(BINDIR)..."
	@install -d $(DESTDIR)$(BINDIR)
	@install -m 755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)

# Remove the installed binary
uninstall:
	@echo "  [UNINSTALL] Removing $(TARGET) from $(DESTDIR)$(BINDIR)..."
	@rm -f $(DESTDIR)$(BINDIR)/$(TARGET)

# Clean up generated object files and compiled binary
clean:
	@echo "  [CLEAN] Removing object files and binary..."
	@rm -f $(OBJECTS) $(TARGET)

# Help menu showing available commands
help:
	@echo "Available make commands:"
	@echo "  make            - Compile the pgfuse binary"
	@echo "  make check      - Verify FUSE and PostgreSQL libraries"
	@echo "  make test-schema- Load sql/schema.sql into PostgreSQL"
	@echo "  make clean      - Remove object files and binary"
