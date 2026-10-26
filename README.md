# pgfuse: PostgreSQL-Backed Virtual File System

A lightweight, high-performance Linux virtual filesystem written in C using **FUSE 3** (`libfuse3`) and backed by **PostgreSQL** (`libpq`).

`pgfuse` lets you mount a folder in Linux where all files, directories, permissions, and binary contents are transparently stored inside relational PostgreSQL database tables.

---

## 🎯 What Problem Does This Solve?

Standard database storage often stores whole files as huge BLOBs or uses flat path strings (like `/folder/subfolder/file.txt`). That causes two big issues:
1. **Renaming is slow:** If you rename a folder with 50,000 files, a flat path design has to update 50,000 database rows ($O(N)$).
2. **Editing is slow:** If you change 1 byte inside a 100MB file, the database has to rewrite all 100MB of data.

### How `pgfuse` fixes this:
* **Inode & Dentry Split:** Like Linux ext4, files are identified by an Inode number. Directory entries (dentries) simply map `(parent_folder, name) -> inode`. Renaming a folder takes **1 single row update** ($O(1)$), no matter how many files are inside.
* **64KB Block Striping:** Files are sliced into 64KB chunks (`pgfuse_chunks`). If you edit a small part of a large file, only that specific 64KB chunk is updated.
* **Thread-Safe Connection Pool:** FUSE handles file operations across multiple threads simultaneously. Because PostgreSQL's `libpq` is not thread-safe on a single connection, `pgfuse` uses an 8-connection pool protected by POSIX mutexes (`pthread_mutex_t`) and condition variables (`pthread_cond_t`).
* **Binary Wire Protocol:** Uses PostgreSQL's binary format (`resultFormat = 1` and `paramFormats = 1`) to stream raw bytes directly to and from memory without hex encoding overhead.

---

## 🏗️ Architecture

```
+--------------------------------------------------------------------+
|                         USER APPLICATIONS                          |
|             (ls, cat, echo, cp, mkdir, rm, nano, etc.)             |
+--------------------------------------------------------------------+
                                  |
                                  v (Standard Linux System Calls)
+--------------------------------------------------------------------+
|                         LINUX KERNEL (VFS)                         |
|                       Kernel FUSE Module                           |
+--------------------------------------------------------------------+
                                  |
                                  v (/dev/fuse)
+--------------------------------------------------------------------+
|                           pgfuse DAEMON                            |
|  * Multi-Threaded FUSE 3 Event Loop                                |
|  * 8-Channel Connection Pool (pthread_mutex + condvar)             |
|  * 64KB Chunk Slicing & Read-Modify-Write Engine                   |
|  * Inode / Dentry Path Resolver                                    |
+--------------------------------------------------------------------+
                                  |
                                  v (libpq Binary Wire Protocol)
+--------------------------------------------------------------------+
|                        POSTGRESQL DATABASE                         |
|  * pgfuse_inodes     : File metadata (size, mode, uid, timestamps) |
|  * pgfuse_dentries   : Directory trees (parent_ino, name -> ino)   |
|  * pgfuse_chunks     : 64KB raw binary blocks (ino, chunk_index)   |
|  * pgfuse_superblock : Filesystem geometry & statistics            |
+--------------------------------------------------------------------+
```

---

## 📁 Project Structure

Clean, modular architecture organized for reliability and maintainability:

```
pgfuse/
├── src/
│   └── pgfuse.c       # Core filesystem engine and POSIX callback implementations
├── include/
│   └── pgfuse.h       # Struct definitions, tunables, and helper prototypes
├── sql/
│   └── schema.sql     # PostgreSQL database tables, indexes, and views
├── Makefile           # Build script with auto-detection for libfuse3 & libpq
├── Dockerfile         # One-command container build and test environment
├── .gitignore         # Build artifact exclusions
└── README.md          # Project documentation and architecture guide
```

---

## 🚀 Quick Start (Using Docker)

The easiest way to build, test, and run `pgfuse` is with Docker.

### 1. Build the Docker image
```bash
docker build -t pgfuse .
```

### 2. Run the container
```bash
docker run --privileged -it --rm pgfuse
```
*(The `--privileged` flag is required because FUSE mounts a virtual filesystem inside the Linux kernel).*

Once started, the container automatically starts PostgreSQL, creates the database, loads `sql/schema.sql`, compiles `pgfuse`, and gives you a bash shell.

---

## 💻 Interactive Usage & Verification

Here is an end-to-end verification workflow demonstrating the virtual filesystem in action:

### Step 1: Create a mount point and mount `pgfuse`
```bash
mkdir -p /mnt/myfolder
./pgfuse /mnt/myfolder
```
> **Under the hood:** This starts the userspace daemon and mounts it at `/mnt/myfolder`. Any standard Linux command executed in this path is intercepted by the daemon and mapped to PostgreSQL queries.

---

### Step 2: Create a directory and write a file
```bash
mkdir -p /mnt/myfolder/documents
echo "Hello from pgfuse! This text is stored in PostgreSQL." > /mnt/myfolder/documents/test.txt
cat /mnt/myfolder/documents/test.txt
```
> **Under the hood:** Standard Linux commands like `mkdir`, `echo`, and `cat` operate transparently. The kernel treats `/mnt/myfolder` as standard POSIX storage while reads and writes are dispatched to PostgreSQL via `libpq`.

---

### Step 3: Inspect file metadata with `stat` and `ls -la`
```bash
ls -la /mnt/myfolder/documents
stat /mnt/myfolder/documents/test.txt
```
> **Under the hood:** The `getattr` callback queries `pgfuse_inodes` to return exact file size, mode (permissions), owner ID, and modified timestamps.

---

### Step 4: Inspect relational storage in PostgreSQL
Check the directory hierarchy:
```bash
sudo -u postgres psql -d pgfs -c "SELECT * FROM pgfuse_dentries;"
```
* `documents` maps to its directory Inode ID.
* `test.txt` inside `documents` maps to its file Inode ID.

Check the 64KB chunk storage:
```bash
sudo -u postgres psql -d pgfs -c "SELECT ino, chunk_index, chunk_size FROM pgfuse_chunks;"
```

Inspect overall filesystem metrics:
```bash
sudo -u postgres psql -d pgfs -c "SELECT * FROM v_pgfuse_storage_summary;"
```
> **Under the hood:** This confirms relational persistence: `pgfuse_dentries` manages the hierarchy, `pgfuse_inodes` retains metadata, and `pgfuse_chunks` stores 64KB raw binary blocks.

---

### Step 5: Unmount when finished
```bash
fusermount3 -u /mnt/myfolder
```

---

## 🛠️ Native Build (Without Docker)

For environments running Ubuntu/Debian with a local PostgreSQL instance:

### 1. Install prerequisites
```bash
sudo apt-get update
sudo apt-get install -y build-essential pkg-config libfuse3-dev libpq-dev postgresql postgresql-contrib
```

### 2. Set up PostgreSQL
```bash
sudo -u postgres psql -c "ALTER USER postgres WITH PASSWORD 'postgres';"
sudo -u postgres psql -c "CREATE DATABASE pgfs;"
sudo -u postgres psql -d pgfs -f sql/schema.sql
```

### 3. Compile and Run
```bash
make
mkdir -p /mnt/pgfuse
./pgfuse /mnt/pgfuse
```

---

## ⚙️ Design Decisions & Technical Architecture

### 1. Why FUSE 3?
FUSE (Filesystem in Userspace) allows creating custom filesystems in userspace without writing or loading unstable kernel drivers. The Linux kernel passes VFS system calls (`open`, `read`, `write`, `mkdir`) across `/dev/fuse` to userspace callback handlers.

### 2. Why Separate Inodes and Dentries?
In traditional POSIX filesystems, file metadata is decoupled from directory naming. By mirroring this separation in PostgreSQL:
* **$O(1)$ Atomic Renames:** Renaming a folder updates only a single row in `pgfuse_dentries`, regardless of how many thousands of files reside in the subtree.
* **Hard Links:** Multiple dentry records can reference the same Inode without duplicating data chunks or file metadata.

### 3. Why 64KB Chunk Striping Instead of Monolithic BLOBs?
Storing entire files as single database BLOBs creates severe write amplification. For instance, modifying 10 bytes in a 500MB file would force the database to read, rewrite, and write-ahead-log (WAL) all 500MB. With 64KB striping, only the specific 64KB chunk spanning the targeted byte offset is read and updated.

### 4. Why a Dedicated Connection Pool?
The FUSE kernel driver dispatches file operations concurrently across worker threads. Because `libpq` connections are not thread-safe, sharing a single connection causes memory and protocol corruption. `pgfuse` implements an 8-connection pool synchronized via POSIX mutexes (`pthread_mutex_t`) and condition variables (`pthread_cond_t`) with automatic connection health verification (`PQstatus` / `PQreset`).
