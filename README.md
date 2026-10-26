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

Only the essential files are kept — clean, minimal, and easy to explain:

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
├── .dockerignore       # Docker build exclusions
└── README.md          # Project documentation and interview guide
```

---

## 🚀 Quick Start (Using Docker)

The easiest way to build, test, and demonstrate `pgfuse` is with Docker.

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

## 💡 How to Demonstrate to an Interviewer

Here is a quick, step-by-step walkthrough you can follow live in an interview:

### Step 1: Create a mount folder and mount `pgfuse`
```bash
mkdir -p /mnt/myfolder
./pgfuse /mnt/myfolder
```
> **What to explain:** "This starts our userspace daemon and mounts it at `/mnt/myfolder`. Any standard Linux command running in this folder will now be intercepted by our C program and translated into SQL queries."

---

### Step 2: Create a directory and write a file
```bash
mkdir -p /mnt/myfolder/documents
echo "Hello from pgfuse! This text is stored in PostgreSQL." > /mnt/myfolder/documents/test.txt
cat /mnt/myfolder/documents/test.txt
```
> **What to explain:** "Standard Linux commands like `mkdir`, `echo`, and `cat` work completely transparently. The kernel thinks it is talking to a normal hard drive, but the data is going through FUSE into PostgreSQL."

---

### Step 3: Check file details with `stat` and `ls -la`
```bash
ls -la /mnt/myfolder/documents
stat /mnt/myfolder/documents/test.txt
```
> **What to explain:** "`getattr` queries our `pgfuse_inodes` table to return the exact file size, file mode (permissions), owner ID, and modified timestamps."

---

### Step 4: Show the data inside PostgreSQL! (The "Aha!" Moment)
Check the directory table:
```bash
sudo -u postgres psql -d pgfs -c "SELECT * FROM pgfuse_dentries;"
```
You will see:
* `documents` pointing to an Inode ID.
* `test.txt` inside `documents` pointing to another Inode ID.

Check the raw file chunk data:
```bash
sudo -u postgres psql -d pgfs -c "SELECT ino, chunk_index, chunk_size FROM pgfuse_chunks;"
```

Check the overall storage summary view:
```bash
sudo -u postgres psql -d pgfs -c "SELECT * FROM v_pgfuse_storage_summary;"
```
> **What to explain:** "Here is proof that the file is not on disk. In PostgreSQL, `pgfuse_dentries` shows the directory hierarchy, `pgfuse_inodes` stores the metadata, and `pgfuse_chunks` holds the 64KB binary blocks."

---

### Step 5: Unmount when done
```bash
fusermount3 -u /mnt/myfolder
```

---

## 🛠️ Native Build (Without Docker)

If you are running on an Ubuntu/Debian system with PostgreSQL installed locally:

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

## 📋 Interview Cheatsheet: Key Questions & Answers

**Q: What is FUSE?**  
**A:** Filesystem in Userspace. It allows developers to create custom filesystems in regular user code without writing complex or risky kernel drivers. The Linux kernel forwards filesystem calls (`open`, `read`, `write`, `mkdir`) through `/dev/fuse` to our C application.

**Q: Why separate Inodes and Dentries?**  
**A:** In standard POSIX filesystems, a file's identity and metadata are stored in an Inode, while its name and location live in a Directory Entry (dentry). By splitting them in PostgreSQL, renaming a directory or moving a file is an $O(1)$ operation (updating 1 row in `pgfuse_dentries`), rather than updating every path string in the whole subtree.

**Q: Why slice files into 64KB chunks instead of one big column?**  
**A:** If a file is 500MB and a user edits 10 bytes in the middle, storing the whole file in one `BYTEA` column would require reading and rewriting all 500MB to disk and database write-ahead logs. With 64KB chunking, we only fetch, modify, and save the single affected 64KB chunk.

**Q: Why do you need a connection pool?**  
**A:** FUSE handles filesystem calls on multiple threads in parallel for speed. PostgreSQL's `libpq` client library is not thread-safe on a single connection. Our connection pool maintains 8 active connections and uses a mutex lock and condition variable so worker threads safely borrow and return connections without conflicts.
