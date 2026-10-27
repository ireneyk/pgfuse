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

## 📊 Complete Execution Guide: Commands, Outputs & Explanations

> [!IMPORTANT]
> **Understanding the Two Terminal Environments:**  
> `pgfuse` is a Linux program that requires the Linux kernel (`FUSE 3`). On Windows, you cannot run `./pgfuse` directly in PowerShell because Windows does not have the Linux FUSE kernel driver.  
> 
> * **Phase 1 (Windows PowerShell):** You use Docker to launch a Linux container. Your prompt looks like:  
>   `PS C:\Users\...\pgfuse>`  
> * **Phase 2 (Linux Container):** Once Docker starts, your prompt changes to Linux:  
>   `root@<container-id>:/app#`  
>   **All filesystem commands (`./pgfuse`, `mkdir`, `echo`, `psql`) must be run inside this Linux prompt!**

---

### Master Execution Table

| Environment | Step / Action | Command to Run | Expected Terminal Output | Easy Explanation of Output |
| :--- | :--- | :--- | :--- | :--- |
| **Windows PowerShell**<br>`PS C:\...> ` | **1. Open Docker Desktop** | *(Launch Docker Desktop from Windows Start Menu)* | Docker Desktop icon in taskbar shows green / running | Starts the Docker engine on your PC so it can run Linux containers. |
| **Windows PowerShell**<br>`PS C:\...> ` | **2. Build Container** | `docker build -t pgfuse .` | `[+] Building ...`<br>`=> => naming to docker.io/library/pgfuse:latest` | Compiles your project inside an Ubuntu 22.04 image with GCC, FUSE 3, and PostgreSQL installed. |
| **Windows PowerShell**<br>`PS C:\...> ` | **3. Run Linux Container** | `docker run --privileged -it --rm pgfuse` | `* Starting PostgreSQL 14 database server`<br>`CREATE DATABASE`<br>`Successfully built pgfuse with fuse3 and libpq.`<br>`[PGFUSE READY] Container is up!`<br><br>👉 **Prompt changes to:**<br>`root@b774f64f403c:/app#` | Starts PostgreSQL, loads `schema.sql`, compiles the binary, and drops you into a **Linux bash prompt**. Notice your prompt is now `root@...:/app#`! |
| **Linux Container**<br>`root@...:/app#` | **4. Mount Filesystem** | `mkdir -p /mnt/myfolder`<br>`./pgfuse /mnt/myfolder` | `==========================================================`<br>` pgfuse: PostgreSQL-Backed Virtual File System`<br>` Engine: 1.0.0 \| Extents: 64 KB \| Pool: 8 Channels`<br>` Status: Active Mount - ACID POSIX VFS Translation Live`<br>`==========================================================` | **The virtual disk is alive!** `pgfuse` opens 8 database connections and mounts at `/mnt/myfolder`. Any standard Linux file command run here will be converted into SQL. |
| **Linux Container**<br>`root@...:/app#` | **5. Create Folder** | `mkdir -p /mnt/myfolder/documents` | *(Silent return, exit code 0)* | Standard Linux `mkdir` worked! In PostgreSQL, a new directory Inode is created and linked to root (`/`). |
| **Linux Container**<br>`root@...:/app#` | **6. Write File** | `echo "Hello pgfuse!" > /mnt/myfolder/documents/test.txt` | *(Silent return, exit code 0)* | The Linux kernel called `create()` and `write()`. `pgfuse` saved a 14-byte binary block into PostgreSQL and registered `test.txt` inside `documents`. |
| **Linux Container**<br>`root@...:/app#` | **7. Read File** | `cat /mnt/myfolder/documents/test.txt` | `Hello pgfuse!` | `pgfuse` fetched the 64KB binary chunk from PostgreSQL across the socket and displayed the text to your terminal. |
| **Linux Container**<br>`root@...:/app#` | **8. Inspect Metadata** | `stat /mnt/myfolder/documents/test.txt` | `File: .../test.txt`<br>`Size: 14  Blocks: 1  IO Block: 65536 regular file`<br>`Inode: 3  Links: 1  Access: (0644/-rw-r--r--)` | Calls `getattr()`. Returns exact file metadata from `pgfuse_inodes`: size (14 bytes), 64KB block geometry, Inode ID 3, permissions, and timestamps. |
| **Linux Container**<br>`root@...:/app#` | **9. Check Directory in SQL** | `sudo -u postgres psql -d pgfs -c "SELECT * FROM pgfuse_dentries;"` | ` parent_ino \|   name    \| ino `<br>`------------+-----------+-----`<br>`          1 \| documents \|   2`<br>`          2 \| test.txt  \|   3` | **Direct proof!** Shows the directory tree in PostgreSQL: root (Inode 1) holds `documents` (Inode 2), and `documents` holds `test.txt` (Inode 3). |
| **Linux Container**<br>`root@...:/app#` | **10. Check Data Chunks in SQL** | `sudo -u postgres psql -d pgfs -c "SELECT ino, chunk_index, chunk_size FROM pgfuse_chunks;"` | ` ino \| chunk_index \| chunk_size `<br>`-----+-------------+------------`<br>`   3 \|           0 \|         14` | Proves that file content is stored directly inside the PostgreSQL `pgfuse_chunks` table as a 64KB binary block. |
| **Linux Container**<br>`root@...:/app#` | **11. Storage Summary View** | `sudo -u postgres psql -d pgfs -c "SELECT * FROM v_pgfuse_storage_summary;"` | ` total_files \| total_directories \| total_chunks \| logical_bytes_used \| physical_bytes_stored `<br>`-------------+-------------------+--------------+--------------------+-----------------------`<br>`           1 \|                 2 \|            1 \|                 14 \|                    14` | Built-in SQL view calculating summary metrics across all tables: 1 file, 2 directories (including root), 1 chunk, and 14 bytes stored. |
| **Linux Container**<br>`root@...:/app#` | **12. Unmount Filesystem** | `fusermount3 -u /mnt/myfolder` | `[PGFUSE-INFO] Unmounting filesystem. Draining connection pool...`<br>`[PGFUSE-INFO] Teardown complete. Clean exit.` | Safely unmounts the virtual folder and closes all 8 database connections cleanly without leaking memory or locks. |

---

## 💻 Step-by-Step Interactive Walkthrough

If you want to run the demonstration step-by-step:

### Step 1: In Windows PowerShell, start the Docker container
```powershell
docker run --privileged -it --rm pgfuse
```
*(The `--privileged` flag is required because FUSE mounts a virtual filesystem inside the Linux kernel).*

Once running, your prompt will change to:
```text
root@b774f64f403c:/app#
```

---

### Step 2: Mount the virtual filesystem
```bash
mkdir -p /mnt/myfolder
./pgfuse /mnt/myfolder
```
> **Under the hood:** This starts the userspace daemon and mounts it at `/mnt/myfolder`. Any standard Linux command executed in this path is intercepted by the daemon and mapped to PostgreSQL queries.

---

### Step 3: Create a directory and write a file
```bash
mkdir -p /mnt/myfolder/documents
echo "Hello from pgfuse! This text is stored in PostgreSQL." > /mnt/myfolder/documents/test.txt
cat /mnt/myfolder/documents/test.txt
```
> **Under the hood:** Standard Linux commands like `mkdir`, `echo`, and `cat` operate transparently. The kernel treats `/mnt/myfolder` as standard POSIX storage while reads and writes are dispatched to PostgreSQL via `libpq`.

---

### Step 4: Inspect file metadata with `stat` and `ls -la`
```bash
ls -la /mnt/myfolder/documents
stat /mnt/myfolder/documents/test.txt
```
> **Under the hood:** The `getattr` callback queries `pgfuse_inodes` to return exact file size, mode (permissions), owner ID, and modified timestamps.

---

### Step 5: Inspect relational storage in PostgreSQL
Check the directory hierarchy:
```bash
sudo -u postgres psql -d pgfs -c "SELECT * FROM pgfuse_dentries;"
```

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

### Step 6: Unmount when finished
```bash
fusermount3 -u /mnt/myfolder
```

---

## 🛠️ Native Linux Build (Without Docker)

For native Linux systems (Ubuntu/Debian) with a local PostgreSQL instance:

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
