# ============================================================================
# Makefile for pgfuse (PostgreSQL-Backed Virtual File System)
#
# Builds the 'pgfuse' binary from C source code.
# Automatically detects libfuse3 and PostgreSQL libpq libraries.
# ============================================================================

# CC: C compiler executable (defaults to GCC)
CC       ?= gcc
# PREFIX: Base installation directory (standard /usr/local)
PREFIX   ?= /usr/local
# BINDIR: Target folder for the compiled binary (/usr/local/bin)
BINDIR   ?= $(PREFIX)/bin

# Auto-detect FUSE library using pkg-config (prefers modern fuse3, falls back to fuse)
# pkg-config: System utility that outputs the exact include paths and library flags needed
FUSE_PKG := $(shell pkg-config --exists fuse3 && echo fuse3 || (pkg-config --exists fuse && echo fuse || echo none))

ifeq ($(FUSE_PKG),none)
    # If pkg-config is not installed, fall back to default 64-bit FUSE 3 paths:
    # -D_FILE_OFFSET_BITS=64: Enables 64-bit file offsets (supports files > 2GB)
    # -lfuse3: Links the FUSE 3 shared library
    $(warning [PGFUSE-BUILD] Neither 'fuse3' nor 'fuse' pkg-config found. Defaulting to system flags.)
    FUSE_CFLAGS := -D_FILE_OFFSET_BITS=64 -I/usr/include/fuse3
    FUSE_LIBS   := -lfuse3
else
    # Automatically get compiler headers (-I) and linker library flags (-l) from pkg-config
    FUSE_CFLAGS := $(shell pkg-config --cflags $(FUSE_PKG))
    FUSE_LIBS   := $(shell pkg-config --libs $(FUSE_PKG))
endif

# Auto-detect PostgreSQL client library (libpq):
# LIBPQ_CFLAGS: Include path for PostgreSQL headers (libpq-fe.h)
# LIBPQ_LIBS: Linker flag (-lpq) to link the PostgreSQL client library
LIBPQ_CFLAGS := $(shell pkg-config --cflags libpq 2>/dev/null || pg_config --cflags 2>/dev/null || echo -I/usr/include/postgresql)
LIBPQ_LIBS   := $(shell pkg-config --libs libpq 2>/dev/null || pg_config --libs 2>/dev/null || echo -lpq)

# Compiler flags (CFLAGS):
# -std=c11            : Use modern ISO C11 standard
# -Wall -Wextra       : Enable helpful compiler warnings to catch potential bugs early
# -Wno-unused-parameter: Suppress warnings for unused parameters in standard FUSE callbacks
# -D_GNU_SOURCE       : Unlock POSIX/GNU extensions like strtok_r for thread-safe string parsing
# -O2                 : Optimize execution performance
# -g                  : Include debug symbols for gdb debugging
# -pthread            : Enable POSIX multi-threading support (used for connection pool mutexes)
# -Iinclude           : Tell compiler to look in the include/ folder for pgfuse.h
INCLUDES := -Iinclude
CFLAGS   += -std=c11 -Wall -Wextra -Wno-unused-parameter -D_GNU_SOURCE \
            -O2 -g -pthread $(INCLUDES) $(FUSE_CFLAGS) $(LIBPQ_CFLAGS)

# Linker flags (LDFLAGS):
# Combines POSIX threads (-pthread), FUSE library (-lfuse3), and PostgreSQL client (-lpq)
LDFLAGS  += -pthread $(FUSE_LIBS) $(LIBPQ_LIBS)

# TARGET: Output executable name
TARGET   := pgfuse
# SOURCES: List of C source code files
SOURCES  := src/pgfuse.c
# OBJECTS: Compiled object files (.c files compiled into .o files)
OBJECTS  := $(SOURCES:.c=.o)

# Default password for local test environment
PGPASSWORD ?= postgres
export PGPASSWORD

# .PHONY: Tells Make that these targets are action commands, not physical files on disk
.PHONY: all clean install uninstall check test-schema help

# Default target: verifies dependencies then builds the pgfuse executable
all: check $(TARGET)

# Link rule: combines object files (.o) and libraries into the final 'pgfuse' binary
$(TARGET): $(OBJECTS)
	@echo "  [LD]    $@"
	@$(CC) $(OBJECTS) $(LDFLAGS) -o $@
	@echo "  [BUILD] Successfully built $(TARGET) with $(FUSE_PKG) and libpq."

# Compile rule: converts each .c source file into a .o object file
%.o: %.c
	@echo "  [CC]    $<"
	@$(CC) $(CFLAGS) -c $< -o $@

# check: Verifies that pkg-config can find FUSE 3 and PostgreSQL on this machine
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

# test-schema: Loads sql/schema.sql into the PostgreSQL 'pgfs' database
test-schema:
	@echo "  [PG-INIT] Bootstrapping PostgreSQL schema using sql/schema.sql..."
	@PGPASSWORD=$(PGPASSWORD) psql -h localhost -U postgres -d pgfs -f sql/schema.sql 2>/dev/null || \
		sudo -u postgres psql -d pgfs -f sql/schema.sql || \
		(echo "[PG-ERROR] Failed to bootstrap schema. Ensure database 'pgfs' exists." && exit 1)

# install: Copies the compiled pgfuse executable to /usr/local/bin
install: $(TARGET)
	@echo "  [INSTALL] Installing $(TARGET) to $(DESTDIR)$(BINDIR)..."
	@install -d $(DESTDIR)$(BINDIR)
	@install -m 755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)

# uninstall: Removes the installed pgfuse executable from /usr/local/bin
uninstall:
	@echo "  [UNINSTALL] Removing $(TARGET) from $(DESTDIR)$(BINDIR)..."
	@rm -f $(DESTDIR)$(BINDIR)/$(TARGET)

# clean: Deletes compiled .o object files and the pgfuse binary so you can do a fresh rebuild
clean:
	@echo "  [CLEAN] Removing object files and binary..."
	@rm -f $(OBJECTS) $(TARGET)

# help: Displays a summary of available Make commands
help:
	@echo "Available make commands:"
	@echo "  make            - Compile the pgfuse binary"
	@echo "  make check      - Verify FUSE and PostgreSQL libraries"
	@echo "  make test-schema- Load sql/schema.sql into PostgreSQL"
	@echo "  make clean      - Remove object files and binary"
