# Simple container setup for compiling and testing pgfuse
FROM ubuntu:22.04

# Avoid interactive keyboard prompts during package installation
ENV DEBIAN_FRONTEND=noninteractive
ENV PGPASSWORD=postgres

# Install C compiler, FUSE 3 libraries, PostgreSQL database, and build tools
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
COPY . /app

# Container entrypoint:
# 1. Starts the PostgreSQL background service
# 2. Configures the 'postgres' user password and creates the 'pgfs' database
# 3. Bootstraps the schema tables (inodes, dentries, chunks)
# 4. Compiles pgfuse with 'make'
# 5. Launches bash so you can mount and test the filesystem right away
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
