#ifndef PGFUSE_H
#define PGFUSE_H

/*
 * _GNU_SOURCE:
 * Tells the C compiler to unlock Linux/GNU extensions (like strtok_r for thread-safe string splitting).
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

/*
 * FUSE_USE_VERSION 31:
 * Tells the FUSE header files that our program uses the modern FUSE 3.1 API.
 */
#ifndef FUSE_USE_VERSION
#define FUSE_USE_VERSION 31
#endif

/* System and library header files */
#include <fuse.h>          /* FUSE API: provides filesystem callback structures (getattr, read, write) */
#include <stdio.h>         /* Standard I/O: printf, fprintf, snprintf */
#include <stdlib.h>        /* Memory allocation: malloc, calloc, free */
#include <string.h>        /* String manipulation: strncpy, strcmp, strtok_r */
#include <stdint.h>        /* Fixed-width integers: uint64_t, int32_t */
#include <stdbool.h>       /* Boolean type: true, false */
#include <inttypes.h>      /* Format specifiers: PRIu64 for printing 64-bit numbers */
#include <errno.h>         /* Linux error codes: ENOENT (not found), EEXIST (already exists), etc. */
#include <fcntl.h>         /* File access modes: O_RDONLY, O_WRONLY, O_CREAT */
#include <unistd.h>        /* POSIX system calls: getuid, getgid */
#include <sys/types.h>     /* POSIX type definitions: uid_t, gid_t, off_t, mode_t */
#include <sys/stat.h>      /* File attribute structures: struct stat, S_IFREG, S_IFDIR */
#include <sys/statvfs.h>   /* Filesystem capacity structure: struct statvfs (used by df command) */
#include <time.h>          /* Timestamp handling: struct timespec, time_t */
#include <pthread.h>       /* Multi-threading: pthread_mutex_t (locks), pthread_cond_t (signals) */
#include <libpq-fe.h>      /* PostgreSQL frontend client library: PGconn, PGresult, PQexec */

/* ============================================================================
 * Filesystem Constants & Geometry
 * ============================================================================
 */

/* Human-readable filesystem name and version tag */
#define PGFUSE_NAME                 "pgfuse"
#define PGFUSE_VERSION_STR          "1.0.0"

/*
 * PGFUSE_MAGIC (0x50474653):
 * "Magic Number" identifying our filesystem type.
 * In ASCII hexadecimal: 0x50='P', 0x47='G', 0x46='F', 0x53='S' (spells "PGFS").
 * Linux tools like 'stat -f' check this magic number to recognize the filesystem.
 */
#define PGFUSE_MAGIC                0x50474653ULL

/* Filesystem on-disk schema version */
#define PGFUSE_VERSION              1

/* 
 * PGFUSE_CHUNK_SIZE (64KB = 65,536 bytes):
 * Files are split into 64KB blocks.
 * Purpose: If someone writes 5 bytes into a 100MB file, we only update one 64KB
 * chunk in PostgreSQL instead of rewriting the entire 100MB.
 */
#define PGFUSE_CHUNK_SIZE           65536

/*
 * PGFUSE_DEFAULT_POOL_SIZE (8):
 * Number of parallel database connections opened to PostgreSQL.
 * Allows up to 8 threads to query the database at the same time.
 */
#define PGFUSE_DEFAULT_POOL_SIZE    8

/* Linux standard buffer sizes */
#define PGFUSE_MAX_PATH_LEN         4096   /* Max characters in a full path: /a/b/c... */
#define PGFUSE_MAX_NAME_LEN         255    /* Max characters in a single file name: test.txt */

/*
 * PGFUSE_ROOT_INODE (1):
 * In Linux filesystems, the root folder "/" is assigned a fixed Inode number.
 * We designate Inode 1 as "/" in our database.
 */
#define PGFUSE_ROOT_INODE           1ULL

/* Default connection string used to connect to local PostgreSQL */
#define PGFUSE_DEFAULT_CONNINFO     "host=localhost port=5432 dbname=pgfs user=postgres password=postgres"

/* ============================================================================
 * Data Structures
 * ============================================================================
 */

/*
 * pgfuse_conn_pool_t (Connection Pool Struct):
 * Purpose: Manages multiple PostgreSQL database connections.
 * Why needed: FUSE processes file calls across multiple threads, but a PostgreSQL
 * connection (PGconn) is NOT thread-safe. This pool lets threads safely borrow
 * and return connections without interfering with each other.
 */
typedef struct {
    PGconn          **conns;        /* Array of active PostgreSQL database connection pointers */
    bool            *in_use;        /* Busy flags: true = currently borrowed by a thread, false = idle */
    int             pool_size;      /* Total number of connections available in the pool (default 8) */
    char            conninfo[1024]; /* Saved connection string: host, port, user, dbname, password */
    pthread_mutex_t lock;           /* Mutex lock: ensures only one thread picks a connection at a time */
    pthread_cond_t  cond;           /* Condition signal: wakes up waiting threads when a connection is returned */
} pgfuse_conn_pool_t;

/*
 * pgfuse_runtime_t (Global Runtime State Struct):
 * Purpose: Keeps all global variables bundled in one place during execution.
 */
typedef struct {
    pgfuse_conn_pool_t pool;        /* The active database connection pool */
    uint64_t           chunk_size;  /* Block size for file slicing (64KB) */
    uid_t              mount_uid;   /* User ID of the person running pgfuse (e.g. 1000) */
    gid_t              mount_gid;   /* Group ID of the person running pgfuse (e.g. 1000) */
} pgfuse_runtime_t;

/* ============================================================================
 * Helper Prototypes (Function Declarations)
 * ============================================================================
 */

/*
 * Connection Pool Lifecycle:
 * - pgfuse_pool_init:    Opens 'size' connections to PostgreSQL and sets up mutexes.
 * - pgfuse_pool_destroy: Closes all database connections and cleans up mutex memory.
 * - pgfuse_pool_acquire: Borrows an idle database connection (sleeps if all are busy).
 * - pgfuse_pool_release: Returns a borrowed connection back to the pool and signals waiting threads.
 */
int     pgfuse_pool_init(pgfuse_conn_pool_t *pool, const char *conninfo, int size);
void    pgfuse_pool_destroy(pgfuse_conn_pool_t *pool);
PGconn *pgfuse_pool_acquire(pgfuse_conn_pool_t *pool);
void    pgfuse_pool_release(pgfuse_conn_pool_t *pool, PGconn *conn);

/*
 * Path Resolution Helpers:
 * - pgfuse_split_path:   Splits "/home/user/file.txt" into parent "/home/user" and name "file.txt".
 * - pgfuse_resolve_path: Looks up each folder name in PostgreSQL to find the target's Inode number.
 */
int  pgfuse_split_path(const char *path, char *parent_dir, char *base_name);
int  pgfuse_resolve_path(PGconn *conn, const char *path, uint64_t *out_ino, mode_t *out_mode);

#endif /* PGFUSE_H */
