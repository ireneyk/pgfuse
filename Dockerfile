# Simple container setup for compiling and testing pgfuse
FROM ubuntu:22.04

# Avoid interactive keyboard prompts during package installation
# DEBIAN_FRONTEND=noninteractive: Prevents apt from freezing to ask questions like timezone
ENV DEBIAN_FRONTEND=noninteractive
# Default password used for PostgreSQL database connection
ENV PGPASSWORD=postgres

# Install required tools and libraries:
# - build-essential     : GCC compiler, make, and standard C library headers
# - pkg-config          : Tool that automatically finds library compiler/linker flags
# - libfuse3-dev        : FUSE 3 development headers (fuse.h) and libraries
# - libpq-dev           : PostgreSQL C client library headers (libpq-fe.h)
# - postgresql          : PostgreSQL database server
# - postgresql-contrib  : Extra PostgreSQL extensions and tools
# - fuse3               : Linux userspace FUSE 3 mount utilities (fusermount3)
# - git                 : Version control tool
# - sudo                : Allows running psql commands as the 'postgres' system user
RUN apt-get update && apt-get install -y \
    build-essential \
    pkg-config \
    libfuse3-dev \
    libpq-dev \
    postgresql \
    postgresql-contrib \
    fuse3 \
    git \
    sudo

# Set the working directory inside the container
WORKDIR /app
# Copy all project files from host computer into /app
COPY . /app

# Container entrypoint script:
# 1. Starts the PostgreSQL service in the background
# 2. Configures the 'postgres' user password and creates the 'pgfs' database
# 3. Bootstraps the schema tables (inodes, dentries, chunks) using psql
# 4. Compiles the pgfuse binary with 'make'
# 5. Launches bash so you can mount and test the filesystem interactively
CMD service postgresql start && \
    sudo -u postgres psql -c "ALTER USER postgres WITH PASSWORD 'postgres';" && \
    sudo -u postgres psql -c "CREATE DATABASE pgfs;" 2>/dev/null || true && \
    sudo -u postgres psql -d pgfs -f sql/schema.sql && \
    make && \
    echo "\n============================================================" && \
    echo " [PGFUSE READY] Container is up! You can now run:" && \
    echo "   mkdir -p /mnt/myfolder" && \
    echo "   ./pgfuse /mnt/myfolder" && \
    echo "============================================================\n" && \
    bash
