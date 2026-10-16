# PG-FS: PostgreSQL-Backed Virtual File System

**Author:** Irene Therese Joseph  
**Role:** Senior Linux Systems Engineer  
**Specialization:** Linux Kernel VFS, Distributed Storage Systems, Cloud-Native Storage Abstractions  

---

## Executive Overview

**PG-FS** is an original, production-grade Filesystem in Userspace (FUSE) implemented in pure C (C11). It transparently bridges Linux POSIX Virtual File System (VFS) operations with an ACID-compliant relational backend powered by PostgreSQL (`libpq`). Rather than serializing monolithic file blobs into database rows or persisting data to physical block devices, PG-FS models enterprise storage architecture: it cleanly decouples the **Metadata Control Plane** (inodes, hierarchical dentries, and superblocks) from the **Data Plane** (fixed-size, striped 64KB binary chunks).

This project demonstrates deep systems programming rigor across the Linux storage stack, including VFS syscall translation, memory management, pointer arithmetic, thread synchronization, and database protocol optimization.

```
+------------------------------------------------------------------------------------+
|                                    USER SPACE                                      |
|                                                                                    |
|  [ POSIX Applications: ls, cat, cp, dd, fio, touch, mkdir, rm, truncate ]          |
|                                     |                                              |
|                                     v (Standard Syscalls)                          |
+------------------------------------------------------------------------------------+
|                                   LINUX KERNEL                                     |
|                                                                                    |
|                           [ Virtual File System (VFS) ]                            |
|                                     |                                              |
|                                     v                                              |
|                           [ FUSE Kernel Module ]                                   |
|                                     |                                              |
+-------------------------------------|----------------------------------------------+
|                                     v (/dev/fuse)                                  |
|                                                                                    |
|  [ PG-FS Userspace Daemon (pgfs) ]                                                 |
|     * Multi-Threaded FUSE Worker Threads                                           |
|     * Thread-Safe PostgreSQL Connection Pool (pthread_mutex + condvar)             |
|     * 64 KB Extent / Chunk Striping Engine (Read-Modify-Write)                     |
|     * Inode & Dentry Namespace Resolution Engine                                   |
|                                     |                                              |
|                                     v (libpq Binary Protocol / UNIX Sockets)       |
+-------------------------------------|----------------------------------------------+
|                                     v                                              |
|                         [ PostgreSQL RDBMS Instance ]                              |
|   +-------------------+  +-------------------+  +-------------------+              |
|   |   fs_superblock   |  |     fs_inodes     |  |    fs_dentries    |              |
|   +-------------------+  +-------------------+  +-------------------+              |
|   |   fs_chunks (Striped 64KB Binary BYTEA Blocks with Primary Key Index)          |
|   +-----------------------------------------------------------------+              |
+------------------------------------------------------------------------------------+
```

---

## Storage Architecture & Internals

### 1. Inode & Dentry Separation (Hierarchical Namespace)
Traditional naive database-backed filesystems store paths as flat strings (e.g., `VARCHAR PRIMARY KEY` with paths like `/dir/subdir/file.txt`). While superficially simple, flat path tables violate fundamental POSIX semantics:
* **Atomic Renames ($O(1)$):** Renaming a deeply nested directory would require updating every descendant's path string ($O(N)$ row updates), causing catastrophic lock contention.
* **Hard Links:** Multiple directory entries pointing to the same file content and metadata cannot be represented with path-keyed records.
* **Directory Link Counts:** UNIX filesystems mandate `nlink` tracking (e.g., each subdirectory increments the parent's `st_nlink` by 1 due to `..`).

**PG-FS implements a genuine VFS Inode / Dentry split:**
* **`fs_inodes`**: Holds file and directory metadata (`mode`, `nlink`, `uid`, `gid`, `size`, `atime`, `mtime`, `ctime`). Inodes are identified by a 64-bit monotonically increasing sequence (`ino_t`).
* **`fs_dentries`**: Represents directory entries. A composite primary key `(parent_ino, name)` maps human-readable names to child target `ino` values. Inode 1 is hardcoded as the root directory (`/`).
* **Atomic Mutations:** Renaming or linking an entry modifies only dentry references while inodes and data chunks remain untouched.

### 2. Striped Chunking Model (Data Plane)
Storing entire file payloads in a single `BYTEA` or PostgreSQL Large Object (`LOB`) creates severe performance bottlenecks:
* Appending 1 byte to a 2GB file forces the database to read, rewrite, and write-ahead log (WAL) all 2GB of data.
* Random in-place writes (`pwrite`) become computationally prohibitive.

**PG-FS partitions files into uniform 64KB chunks (`PGFS_CHUNK_SIZE = 65536`):**
* Chunks are indexed by `(ino, chunk_index)`.
* **Read Slicing:** When reading an arbitrary byte range `[offset, offset + size)`, PG-FS calculates the spanned chunk indices $\lfloor \frac{\text{offset}}{\text{CHUNK\_SIZE}} \rfloor$ through $\lfloor \frac{\text{offset} + \text{size} - 1}{\text{CHUNK\_SIZE}} \rfloor$ and retrieves only the affected blocks.
* **Read-Modify-Write (RMW):** For unaligned boundary writes, PG-FS fetches the existing 64KB block, splices the updated bytes into memory, and upserts the chunk back using PostgreSQL's native `ON CONFLICT (ino, chunk_index) DO UPDATE` mechanism.
* **Direct Path:** Aligned, full-chunk writes skip the read phase entirely and stream directly to the database.
* **Sparse File Support:** Holes (unallocated chunks) in sparse files consume zero database storage. During reads, unallocated chunks are automatically synthesized as null bytes (`0x00`) via memory zeroing.

---

## POSIX-to-SQL Translation Matrix

The following matrix documents how kernel-level POSIX file operations handled by `fuse_operations` are transformed into PostgreSQL relational queries:

| POSIX System Call | FUSE Operation | Target Table(s) | PostgreSQL SQL / Execution Paradigm | Transactional Semantics & Guarantees |
| :--- | :--- | :--- | :--- | :--- |
| `stat()`, `lstat()` | `getattr` | `fs_inodes`, `fs_dentries` | `SELECT ino, mode, nlink, uid, gid, size, EXTRACT(EPOCH FROM ...) FROM fs_inodes WHERE ino = $1;` | Read Committed snapshot. Populates `struct stat` with exact file attributes and calculated block allocation (`(size + 511) / 512`). |
| `getdents64()`, `readdir()` | `readdir` | `fs_dentries`, `fs_inodes` | `SELECT d.name, d.ino, i.mode FROM fs_dentries d JOIN fs_inodes i ON d.ino = i.ino WHERE d.parent_ino = $1 ORDER BY d.name;` | Automatically synthesizes `.` and `..` dentries before streaming child records through the `fuse_fill_dir_t` callback buffer. |
| `open()` | `open` | `fs_inodes`, `fs_dentries` | Path resolution query verifying existence and permission bits. | Caches the resolved `ino` into `fi->fh` (file handle). All subsequent `read` and `write` operations bypass path tokenization. |
| `creat()`, `open(O_CREAT)` | `create` | `fs_inodes`, `fs_dentries` | `BEGIN;`<br>`INSERT INTO fs_inodes (...) VALUES (...) RETURNING ino;`<br>`INSERT INTO fs_dentries (parent_ino, name, ino) VALUES (...);`<br>`UPDATE fs_inodes SET mtime = NOW() WHERE ino = parent_ino;`<br>`COMMIT;` | Atomic ACID transaction. Ensures parent directory dentry and new inode are created simultaneously without orphaned metadata. |
| `read()`, `pread()` | `read` | `fs_inodes`, `fs_chunks` | `SELECT chunk_size, chunk_data FROM fs_chunks WHERE ino = $1 AND chunk_index = $2;` | Executes with binary wire format (`resultFormat = 1`). Raw binary payload is copied via pointer arithmetic directly to the caller's buffer. |
| `write()`, `pwrite()` | `write` | `fs_chunks`, `fs_inodes` | `INSERT INTO fs_chunks (ino, chunk_index, chunk_size, chunk_data) VALUES (...) ON CONFLICT (ino, chunk_index) DO UPDATE SET chunk_size = EXCLUDED.chunk_size, chunk_data = EXCLUDED.chunk_data;`<br>`UPDATE fs_inodes SET size = GREATEST(size, $1), mtime = NOW() WHERE ino = $2;` | Employs binary parameter binding (`paramFormats = 1`). Unaligned writes execute a Read-Modify-Write cycle. Inode size expands monotonically. |
| `mkdir()` | `mkdir` | `fs_inodes`, `fs_dentries` | `BEGIN;`<br>`INSERT INTO fs_inodes (mode, nlink, ...) VALUES (S_IFDIR \| mode, 2, ...);`<br>`INSERT INTO fs_dentries (...);`<br>`UPDATE fs_inodes SET nlink = nlink + 1 WHERE ino = parent_ino;`<br>`COMMIT;` | Initializes directory with `nlink = 2` (self `.` and parent dentry reference). Atomically increments the parent directory's `nlink`. |
| `unlink()` | `unlink` | `fs_dentries`, `fs_inodes`, `fs_chunks` | `BEGIN;`<br>`DELETE FROM fs_dentries WHERE parent_ino = $1 AND name = $2;`<br>`UPDATE fs_inodes SET nlink = nlink - 1 WHERE ino = $1 RETURNING nlink;`<br>`-- If nlink == 0: DELETE FROM fs_chunks; DELETE FROM fs_inodes;`<br>`COMMIT;` | Prevents data loss during hard link unlinking. Data chunks and inode are reclaimed if and only if hard link counter drops to zero. |
| `rmdir()` | `rmdir` | `fs_dentries`, `fs_inodes` | `SELECT COUNT(*) FROM fs_dentries WHERE parent_ino = $1;`<br>`BEGIN; DELETE FROM fs_dentries; DELETE FROM fs_inodes; UPDATE parent nlink - 1; COMMIT;` | Enforces POSIX `-ENOTEMPTY` semantics. Root inode (`ino = 1`) deletion is explicitly protected (`-EBUSY`). |
| `truncate()`, `ftruncate()` | `truncate` | `fs_inodes`, `fs_chunks` | `DELETE FROM fs_chunks WHERE ino = $1 AND chunk_index > $2;`<br>`UPDATE fs_inodes SET size = $1, mtime = NOW() WHERE ino = $2;` | Truncates boundary chunk payload to `size % CHUNK_SIZE` and drops all orphaned chunks past the new size ceiling. |
| `statvfs()` | `statfs` | `fs_superblock`, `fs_chunks`, `fs_inodes` | `SELECT block_size, total_blocks FROM fs_superblock;`<br>`SELECT COUNT(*) FROM fs_chunks;` | Reports virtual filesystem geometry: block size (64KB), total virtual capacity, allocated blocks, and free blocks for `df -h`. |

---

## Cloud-Native Storage Abstractions: Parallels to IBM Spectrum Fusion

The architectural paradigms implemented in **PG-FS** directly mirror the core design principles powering enterprise distributed filesystems such as **IBM Spectrum Fusion**, **IBM Spectrum Scale (GPFS)**, and **Red Hat OpenShift Data Foundation (Ceph / CephFS)**:

```
+-----------------------------------+---------------------------------------------------+
| Cloud-Native Storage Principle    | Architectural Implementation in PG-FS             |
+-----------------------------------+---------------------------------------------------+
| Disaggregated Control / Data Plane| * Metadata Control Plane: `fs_inodes`,            |
| (Separation of Metadata and Data) |   `fs_dentries` (ACID indexing via relational B-tree) |
|                                   | * Data Storage Plane: `fs_chunks` (uniform 64KB   |
|                                   |   block extents)                                  |
+-----------------------------------+---------------------------------------------------+
| Object / Chunk Striping           | * Identical to Ceph RADOS objects or GPFS NSD     |
| (Horizontal Block Striping)       |   striping: large contiguous byte streams are     |
|                                   |   chunked into discrete, addressable extents to   |
|                                   |   localize I/O and support random access.         |
+-----------------------------------+---------------------------------------------------+
| Multi-Attach RWX Without DLM      | * Multi-writer / ReadWriteMany (RWX) container    |
| (Container Native Storage)        |   workloads can safely access files concurrently. |
|                                   |   PostgreSQL's Row-Level Locking (RLL) and MVCC   |
|                                   |   eliminate the need for an external Distributed  |
|                                   |   Lock Manager (DLM) like Red Hat DLM or Consul.  |
+-----------------------------------+---------------------------------------------------+
| Snapshot Isolation & Versioning   | * PostgreSQL Multi-Version Concurrency Control    |
| (Point-in-Time Consistency)       |   (MVCC) and Write-Ahead Logging (WAL) provide    |
|                                   |   instant crash recovery, point-in-time recovery  |
|                                   |   (PITR), and consistent enterprise backups.      |
+-----------------------------------+---------------------------------------------------+
```

### Strategic Alignment with IBM Storage Architecture:
1. **Container Storage Interface (CSI) Parallels:** In Kubernetes and OpenShift environments, CSI drivers abstract block and file storage into storage classes. PG-FS functions as an application-level storage virtualization engine, demonstrating how persistent volume claims (PVCs) can be mapped dynamically onto backend object or relational stores.
2. **Metadata Scaling:** Similar to how IBM Spectrum Scale designates dedicated metadata NSDs, PG-FS maintains dedicated database indexes on `fs_dentries (parent_ino, name)` and `fs_chunks (ino, chunk_index)`. This ensures directory scans and chunk lookups execute with logarithmic $O(\log N)$ time complexity.
3. **Data Integrity & Crash Consistency:** POSIX requires synchronous directory changes (`fsync`) and consistency across system crashes. PG-FS utilizes PostgreSQL transactions (`BEGIN ... COMMIT`) to guarantee that multi-table metadata mutations (such as `mkdir` or `unlink`) are completely atomic. A system crash during `unlink` will never leave orphaned chunks or corrupted directory pointers.

---

## High-Performance Systems Implementation

### 1. Multi-Threaded Connection Pool (`pgfs_conn_pool_t`)
FUSE is multi-threaded by default: the kernel multiplexes incoming I/O requests across an internal worker thread pool. However, **PostgreSQL `libpq` connections (`PGconn`) are not thread-safe**. Sharing a single `PGconn` across concurrent threads results in protocol corruption and segmentation faults.

PG-FS implements a thread-safe connection pool using POSIX synchronization primitives:
* **Pool Synchronization:** Connections are managed via a mutex-protected pool with a condition variable (`pthread_mutex_t lock`, `pthread_cond_t cond`).
* **Acquisition (`pgfs_pool_acquire`):** Worker threads acquire an idle connection or block efficiently on the condition variable without busy-waiting.
* **Self-Healing Liveness:** During acquisition, connection state is validated (`PQstatus`). Unhealthy or dropped connections are transparently reset via `PQreset` before being returned to the worker.
* **Release (`pgfs_pool_release`):** Connections are marked idle upon request completion, signaling waiting worker threads via `pthread_cond_signal`.

### 2. High-Throughput Binary Protocol (Zero-Copy libpq I/O)
Standard `libpq` queries return text representations: binary data in `BYTEA` columns is transmitted as hex-encoded ASCII strings (e.g., `\x48656c6c6f...`), doubling network bandwidth and consuming CPU cycles in `PQunescapeBytea`.

**PG-FS circumvents this overhead using PostgreSQL's binary protocol:**
* **Binary Reads:** Queries specify `resultFormat = 1`. PostgreSQL delivers raw network bytes directly into memory. The pointer `PQgetvalue(res, 0, 1)` points directly to binary file bytes, enabling zero-copy `memcpy` operations into the FUSE buffer.
* **Binary Writes:** Writes utilize parameterized execution (`PQexecParams`) with `paramFormats[3] = 1`. Chunks are streamed in raw binary format without SQL character escaping, eliminating buffer expansion and SQL injection vectors.

### 3. Memory Safety & Heap Staging
* **Stack Pressure Mitigation:** Staging buffers for 64KB chunk operations are explicitly allocated on the heap (`malloc`/`free`) rather than thread stacks to prevent stack overflow in multi-threaded environments with constrained thread stack sizes (e.g., musl or custom pthreads).
* **Deterministic Resource Teardown:** Every `PQexec` and `PQexecParams` invocation is paired with an unconditional `PQclear` across all code branches (both success and error paths). All checked-out connections are guaranteed to be released back to the pool, preventing connection starvation.

---

## Database Schema & DDL Reference

The database structure is defined in `schema.sql`:

```sql
-- Superblock: Global filesystem parameters
CREATE TABLE fs_superblock (
    magic_number      BIGINT       NOT NULL DEFAULT 1346913875, -- 0x50474653
    fs_version        INT          NOT NULL DEFAULT 1,
    block_size        BIGINT       NOT NULL DEFAULT 65536,      -- 64 KB
    total_blocks      BIGINT       NOT NULL DEFAULT 1048576,    -- 64 GB
    created_at        TIMESTAMPTZ  NOT NULL DEFAULT CURRENT_TIMESTAMP,
    last_mounted_at   TIMESTAMPTZ  NOT NULL DEFAULT CURRENT_TIMESTAMP,
    mount_count       BIGINT       NOT NULL DEFAULT 0,
    fs_label          VARCHAR(64)  NOT NULL DEFAULT 'PGFS_DATA_VOLUME'
);

-- Inodes: POSIX attributes
CREATE TABLE fs_inodes (
    ino               BIGSERIAL    PRIMARY KEY,
    mode              INTEGER      NOT NULL,
    nlink             INTEGER      NOT NULL DEFAULT 1,
    uid               INTEGER      NOT NULL DEFAULT 1000,
    gid               INTEGER      NOT NULL DEFAULT 1000,
    size              BIGINT       NOT NULL DEFAULT 0,
    atime             TIMESTAMPTZ  NOT NULL DEFAULT CURRENT_TIMESTAMP,
    mtime             TIMESTAMPTZ  NOT NULL DEFAULT CURRENT_TIMESTAMP,
    ctime             TIMESTAMPTZ  NOT NULL DEFAULT CURRENT_TIMESTAMP
);

-- Dentries: Directory tree hierarchy
CREATE TABLE fs_dentries (
    parent_ino        BIGINT       NOT NULL REFERENCES fs_inodes(ino) ON DELETE CASCADE,
    name              VARCHAR(255) NOT NULL,
    ino               BIGINT       NOT NULL REFERENCES fs_inodes(ino) ON DELETE CASCADE,
    PRIMARY KEY (parent_ino, name)
);

-- Chunks: Striped 64KB data extents
CREATE TABLE fs_chunks (
    ino               BIGINT       NOT NULL REFERENCES fs_inodes(ino) ON DELETE CASCADE,
    chunk_index       BIGINT       NOT NULL,
    chunk_size        INTEGER      NOT NULL,
    chunk_data        BYTEA        NOT NULL,
    PRIMARY KEY (ino, chunk_index)
);
```

---

## Compilation, Deployment & Verification

### 1. Prerequisites

#### RHEL / CentOS / Rocky Linux / Fedora:
```bash
sudo dnf install -y gcc make pkgconf-pkg-config fuse3-devel libpq-devel postgresql-server
```

#### Ubuntu / Debian:
```bash
sudo apt-get update
sudo apt-get install -y build-essential pkg-config libfuse3-dev libpq-dev postgresql postgresql-contrib
```

---

### 2. Database Initialization
Create a dedicated PostgreSQL database named `pgfs` and apply the schema:

```bash
# Switch to postgres user and create database
sudo -u postgres psql -c "CREATE DATABASE pgfs;"
sudo -u postgres psql -c "ALTER USER postgres WITH PASSWORD 'postgres';"

# Bootstrap the schema
psql -h localhost -U postgres -d pgfs -f schema.sql
```

---

### 3. Building PG-FS
The included `Makefile` automatically discovers FUSE 3 (with fallback to legacy FUSE 2) and `libpq` via `pkg-config`:

```bash
# Check toolchain detection
make check

# Compile binary
make

# Output: pgfs executable generated
```

---

### 4. Mounting the Virtual Filesystem

```bash
# Create mount point
mkdir -p /mnt/pgfs

# Set database connection string (if different from default)
export PGFS_PG_CONNSTRING="host=localhost port=5432 dbname=pgfs user=postgres password=postgres"
export PGFS_POOL_SIZE=8

# Mount in foreground with debug output (recommended for initial verification):
./pgfs -f /mnt/pgfs

# Or mount in standard background daemon mode:
./pgfs /mnt/pgfs
```

---

### 5. Verification & POSIX Compliance Test Suite

Run the following test battery in another terminal:

```bash
# 1. Directory Tree & Hierarchical Navigation
mkdir -p /mnt/pgfs/ibm/storage/spectrum_fusion
ls -la /mnt/pgfs/ibm/storage

# 2. Sequential Write & Verification
echo "IBM Spectrum Fusion: Container Native Storage Architecture" > /mnt/pgfs/ibm/storage/spectrum_fusion/overview.txt
cat /mnt/pgfs/ibm/storage/spectrum_fusion/overview.txt

# 3. File Attributes & Timestamp Validation
stat /mnt/pgfs/ibm/storage/spectrum_fusion/overview.txt

# 4. Multi-Chunk Striping & Binary Integrity Test (50MB binary file spanning >800 chunks)
dd if=/dev/urandom of=/tmp/test_source.bin bs=1M count=50
cp /tmp/test_source.bin /mnt/pgfs/ibm/storage/spectrum_fusion/binary_blob.bin

# Validate cryptographic checksums match exactly
SOURCE_MD5=$(md5sum /tmp/test_source.bin | cut -d' ' -f1)
PGFS_MD5=$(md5sum /mnt/pgfs/ibm/storage/spectrum_fusion/binary_blob.bin | cut -d' ' -f1)

echo "Source MD5: $SOURCE_MD5"
echo "PG-FS  MD5: $PGFS_MD5"
if [ "$SOURCE_MD5" = "$PGFS_MD5" ]; then
    echo ">> [PASS] 50MB Binary Chunk Striping & Reassembly Verified."
else
    echo ">> [FAIL] Checksum mismatch."
fi

# 5. Database Inspection (Verify chunks stored in PostgreSQL)
psql -h localhost -U postgres -d pgfs -c "SELECT * FROM v_pgfs_storage_summary;"

# 6. File Truncation & Cleanup
truncate -s 10M /mnt/pgfs/ibm/storage/spectrum_fusion/binary_blob.bin
stat /mnt/pgfs/ibm/storage/spectrum_fusion/binary_blob.bin

# 7. Unlink and Rmdir Validation
rm /mnt/pgfs/ibm/storage/spectrum_fusion/binary_blob.bin
rm /mnt/pgfs/ibm/storage/spectrum_fusion/overview.txt
rmdir /mnt/pgfs/ibm/storage/spectrum_fusion
```

---

### 6. Unmounting the Filesystem

```bash
fusermount3 -u /mnt/pgfs
# (Or on legacy systems: fusermount -u /mnt/pgfs)
```

---

## Performance & Optimization Notes

1. **Prepared Statements:** The connection pool can be extended to prepare frequently executed queries (`PREP_GETATTR`, `PREP_LOOKUP_DENTRY`, `PREP_FETCH_CHUNK`) during connection initialization, cutting statement planning latency by up to 35%.
2. **PostgreSQL Tuning for Filesystem Workloads:**
   * Set `synchronous_commit = off` for ultra-high write IOPS if asynchronous journal persistence is acceptable.
   * Increase `shared_buffers` and `effective_cache_size` to cache frequent chunk lookups in RAM.
   * Enable `work_mem = 64MB` to accelerate concurrent readdir sorting.
3. **Chunk Size Trade-Offs:** The 64KB chunk size balances small write efficiency with metadata overhead. For high-throughput sequential workloads (e.g., media streaming or backup archives), bumping `PGFS_CHUNK_SIZE` to 128KB or 1MB minimizes database row count.

---

## Author Attribution

**Irene Therese Joseph**  
*Senior Linux Systems Engineer*  
*Specializing in VFS, Distributed Systems, Software-Defined Storage & Linux Systems Engineering*
