#ifndef PGFUSE_H
#define PGFUSE_H

/* Enable POSIX and GNU extensions (like strtok_r) */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

/* Tell libfuse we are using the modern FUSE 3 API */
#ifndef FUSE_USE_VERSION
#define FUSE_USE_VERSION 31
#endif

#include <fuse.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <inttypes.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <pthread.h>
#include <libpq-fe.h>

/* ============================================================================
 * Filesystem Constants & Geometry
 * ============================================================================
 */

#define PGFUSE_NAME                 "pgfuse"
#define PGFUSE_VERSION_STR          "1.0.0"

/* Magic number: 0x50474653 is 'PGFS' in ASCII hex (used as filesystem identifier) */
#define PGFUSE_MAGIC                0x50474653ULL

#define PGFUSE_VERSION              1

/* 
 * 64KB chunk size (65536 bytes). 
 * Big files are sliced into 64KB blocks so edits only rewrite one block,
 * not the whole file.
 */
#define PGFUSE_CHUNK_SIZE           65536

/* Number of parallel PostgreSQL database connections in our pool */
#define PGFUSE_DEFAULT_POOL_SIZE    8

/* Standard Linux path and file name limits */
#define PGFUSE_MAX_PATH_LEN         4096
#define PGFUSE_MAX_NAME_LEN         255

/* Root folder "/" is permanently fixed as Inode 1 */
#define PGFUSE_ROOT_INODE           1ULL

/* Default local PostgreSQL connection details */
#define PGFUSE_DEFAULT_CONNINFO     "host=localhost port=5432 dbname=pgfs user=postgres password=postgres"

/* ============================================================================
 * Data Structures
 * ============================================================================
 */

/*
 * Connection Pool:
 * Manages multiple database connections so worker threads can read/write
 * in parallel without crashing libpq (which is not thread-safe).
 */
typedef struct {
    PGconn          **conns;        /* Array of active PostgreSQL connections */
    bool            *in_use;        /* Flags: true = connection busy, false = free */
    int             pool_size;      /* Total number of connections (default 8) */
    char            conninfo[1024]; /* Connection string */
    pthread_mutex_t lock;           /* Mutex lock to protect connection borrowing */
    pthread_cond_t  cond;           /* Condition bell: wakes threads when a connection frees up */
} pgfuse_conn_pool_t;

/*
 * Global Runtime State:
 * Holds the connection pool, chunk size, and current user IDs.
 */
typedef struct {
    pgfuse_conn_pool_t pool;
    uint64_t           chunk_size;
    uid_t              mount_uid;
    gid_t              mount_gid;
} pgfuse_runtime_t;

/* ============================================================================
 * Helper Prototypes
 * ============================================================================
 */

/* Pool lifecycle: setup, shutdown, borrow, return */
int  pgfuse_pool_init(pgfuse_conn_pool_t *pool, const char *conninfo, int size);
void pgfuse_pool_destroy(pgfuse_conn_pool_t *pool);
PGconn *pgfuse_pool_acquire(pgfuse_conn_pool_t *pool);
void pgfuse_pool_release(pgfuse_conn_pool_t *pool, PGconn *conn);

/* Path splitting and inode resolution */
int  pgfuse_split_path(const char *path, char *parent_dir, char *base_name);
int  pgfuse_resolve_path(PGconn *conn, const char *path, uint64_t *out_ino, mode_t *out_mode);

#endif /* PGFUSE_H */
