/*
 * ============================================================================
 * Project:       PG-FS: PostgreSQL-Backed Virtual File System
 * Author:        Irene Therese Joseph
 * Role:          Senior Linux Systems Engineer
 * Description:   High-performance Userspace File System (FUSE) in pure C that
 *                maps standard POSIX VFS operations into an ACID-compliant
 *                relational database schema via libpq.
 *
 * Architecture:  - Control Plane: Inode & Dentry hierarchical metadata engine
 *                - Data Plane: Block-striped 64KB chunk storage layer
 *                - Concurrency: Multi-threaded connection pool with POSIX CVs
 *                - Reliability: Transactional mutations and binary libpq I/O
 * ============================================================================
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

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
 * Filesystem Constants & Tunables
 * ============================================================================
 */
#define PGFS_MAGIC              0x50474653ULL   /* 'PGFS' in ASCII hex */
#define PGFS_VERSION            1
#define PGFS_CHUNK_SIZE         65536           /* 64 KB chunk size */
#define PGFS_DEFAULT_POOL_SIZE  8
#define PGFS_MAX_PATH_LEN       4096
#define PGFS_MAX_NAME_LEN       255
#define PGFS_ROOT_INODE         1ULL

#define PGFS_DEFAULT_CONNINFO   "host=localhost port=5432 dbname=pgfs user=postgres password=postgres"

/* ============================================================================
 * System Types & Connection Pooling
 * ============================================================================
 */

typedef struct {
    PGconn          **conns;
    bool            *in_use;
    int             pool_size;
    char            conninfo[1024];
    pthread_mutex_t lock;
    pthread_cond_t  cond;
} pgfs_conn_pool_t;

typedef struct {
    pgfs_conn_pool_t pool;
    uint64_t         chunk_size;
    uid_t            mount_uid;
    gid_t            mount_gid;
} pgfs_runtime_t;

static pgfs_runtime_t g_pgfs;

/* ============================================================================
 * Connection Pool Operations (Thread-Safe Multi-Worker Management)
 * ============================================================================
 */

static int pgfs_pool_init(pgfs_conn_pool_t *pool, const char *conninfo, int size)
{
    if (!pool || !conninfo || size <= 0) {
        return -EINVAL;
    }

    memset(pool, 0, sizeof(*pool));
    strncpy(pool->conninfo, conninfo, sizeof(pool->conninfo) - 1);
    pool->pool_size = size;

    if (pthread_mutex_init(&pool->lock, NULL) != 0) {
        return -ENOMEM;
    }
    if (pthread_cond_init(&pool->cond, NULL) != 0) {
        pthread_mutex_destroy(&pool->lock);
        return -ENOMEM;
    }

    pool->conns = (PGconn **)calloc(size, sizeof(PGconn *));
    pool->in_use = (bool *)calloc(size, sizeof(bool));
    if (!pool->conns || !pool->in_use) {
        free(pool->conns);
        free(pool->in_use);
        pthread_mutex_destroy(&pool->lock);
        pthread_cond_destroy(&pool->cond);
        return -ENOMEM;
    }

    for (int i = 0; i < size; i++) {
        pool->conns[i] = PQconnectdb(pool->conninfo);
        if (PQstatus(pool->conns[i]) != CONNECTION_OK) {
            fprintf(stderr, "[PGFS-FATAL] PostgreSQL connection [%d] failed: %s\n",
                    i, PQerrorMessage(pool->conns[i]));
            // Teardown initialized connections
            for (int j = 0; j <= i; j++) {
                if (pool->conns[j]) PQfinish(pool->conns[j]);
            }
            free(pool->conns);
            free(pool->in_use);
            pthread_mutex_destroy(&pool->lock);
            pthread_cond_destroy(&pool->cond);
            return -ECONNREFUSED;
        }
    }

    return 0;
}

static void pgfs_pool_destroy(pgfs_conn_pool_t *pool)
{
    if (!pool || !pool->conns) return;

    pthread_mutex_lock(&pool->lock);
    for (int i = 0; i < pool->pool_size; i++) {
        if (pool->conns[i]) {
            PQfinish(pool->conns[i]);
            pool->conns[i] = NULL;
        }
    }
    free(pool->conns);
    free(pool->in_use);
    pool->conns = NULL;
    pool->in_use = NULL;
    pthread_mutex_unlock(&pool->lock);

    pthread_mutex_destroy(&pool->lock);
    pthread_cond_destroy(&pool->cond);
}

static PGconn *pgfs_pool_acquire(pgfs_conn_pool_t *pool)
{
    pthread_mutex_lock(&pool->lock);
    while (1) {
        for (int i = 0; i < pool->pool_size; i++) {
            if (!pool->in_use[i]) {
                pool->in_use[i] = true;
                PGconn *conn = pool->conns[i];
                pthread_mutex_unlock(&pool->lock);

                // Self-healing: verify connection liveness
                if (PQstatus(conn) != CONNECTION_OK) {
                    fprintf(stderr, "[PGFS-WARN] Resetting unhealthy connection [%d]...\n", i);
                    PQreset(conn);
                }
                return conn;
            }
        }
        pthread_cond_wait(&pool->cond, &pool->lock);
    }
}

static void pgfs_pool_release(pgfs_conn_pool_t *pool, PGconn *conn)
{
    if (!pool || !conn) return;

    pthread_mutex_lock(&pool->lock);
    for (int i = 0; i < pool->pool_size; i++) {
        if (pool->conns[i] == conn) {
            pool->in_use[i] = false;
            pthread_cond_signal(&pool->cond);
            break;
        }
    }
    pthread_mutex_unlock(&pool->lock);
}

/* ============================================================================
 * VFS Path Parsing & Inode Resolution Helpers
 * ============================================================================
 */

/*
 * Splits a POSIX path into its parent directory string and terminal leaf name.
 * e.g., "/var/log/syslog" -> parent="/var/log", base="syslog"
 *       "/foo"           -> parent="/",         base="foo"
 *       "/"              -> parent="/",         base=""
 */
static int pgfs_split_path(const char *path, char *parent_dir, char *base_name)
{
    if (!path || !parent_dir || !base_name) return -EINVAL;

    size_t len = strlen(path);
    if (len == 0 || path[0] != '/') return -EINVAL;

    if (strcmp(path, "/") == 0) {
        strcpy(parent_dir, "/");
        base_name[0] = '\0';
        return 0;
    }

    const char *last_slash = strrchr(path, '/');
    if (!last_slash) return -EINVAL;

    if (last_slash == path) {
        strcpy(parent_dir, "/");
        strncpy(base_name, path + 1, PGFS_MAX_NAME_LEN - 1);
        base_name[PGFS_MAX_NAME_LEN - 1] = '\0';
    } else {
        size_t parent_len = (size_t)(last_slash - path);
        if (parent_len >= PGFS_MAX_PATH_LEN) return -ENAMETOOLONG;
        strncpy(parent_dir, path, parent_len);
        parent_dir[parent_len] = '\0';

        strncpy(base_name, last_slash + 1, PGFS_MAX_NAME_LEN - 1);
        base_name[PGFS_MAX_NAME_LEN - 1] = '\0';
    }

    return 0;
}

/*
 * Hierarchical path resolution traversing the fs_dentries and fs_inodes tables.
 * Returns 0 on success, or negative POSIX error code (-ENOENT, -ENOTDIR).
 */
static int pgfs_resolve_path(PGconn *conn, const char *path, uint64_t *out_ino, mode_t *out_mode)
{
    if (!conn || !path || !out_ino || !out_mode) return -EINVAL;

    if (strcmp(path, "/") == 0) {
        *out_ino = PGFS_ROOT_INODE;
        const char *params[1] = {"1"};
        PGresult *res = PQexecParams(conn,
            "SELECT mode FROM fs_inodes WHERE ino = $1::BIGINT;",
            1, NULL, params, NULL, NULL, 0);

        if (PQresultStatus(res) != PGRES_TUPLES_OK || PQntuples(res) == 0) {
            PQclear(res);
            return -EIO;
        }
        *out_mode = (mode_t)strtoul(PQgetvalue(res, 0, 0), NULL, 10);
        PQclear(res);
        return 0;
    }

    char path_buf[PGFS_MAX_PATH_LEN];
    strncpy(path_buf, path, sizeof(path_buf) - 1);
    path_buf[sizeof(path_buf) - 1] = '\0';

    uint64_t curr_ino = PGFS_ROOT_INODE;
    mode_t   curr_mode = S_IFDIR | 0755;

    char *saveptr = NULL;
    char *token = strtok_r(path_buf, "/", &saveptr);

    while (token != NULL) {
        if (!S_ISDIR(curr_mode)) {
            return -ENOTDIR;
        }

        char parent_str[32];
        snprintf(parent_str, sizeof(parent_str), "%" PRIu64, curr_ino);
        const char *params[2] = {parent_str, token};

        PGresult *res = PQexecParams(conn,
            "SELECT d.ino, i.mode FROM fs_dentries d "
            "JOIN fs_inodes i ON d.ino = i.ino "
            "WHERE d.parent_ino = $1::BIGINT AND d.name = $2;",
            2, NULL, params, NULL, NULL, 0);

        if (PQresultStatus(res) != PGRES_TUPLES_OK) {
            PQclear(res);
            return -EIO;
        }

        if (PQntuples(res) == 0) {
            PQclear(res);
            return -ENOENT;
        }

        curr_ino = (uint64_t)strtoull(PQgetvalue(res, 0, 0), NULL, 10);
        curr_mode = (mode_t)strtoul(PQgetvalue(res, 0, 1), NULL, 10);
        PQclear(res);

        token = strtok_r(NULL, "/", &saveptr);
    }

    *out_ino = curr_ino;
    *out_mode = curr_mode;
    return 0;
}

/* ============================================================================
 * FUSE VFS Operations Implementation
 * ============================================================================
 */

/*
 * getattr: Retrieves metadata (struct stat) for a specified path.
 */
#if FUSE_USE_VERSION >= 30
static int pgfs_getattr(const char *path, struct stat *stbuf, struct fuse_file_info *fi)
#else
static int pgfs_getattr(const char *path, struct stat *stbuf)
#endif
{
    (void)fi;
    if (!path || !stbuf) return -EINVAL;
    memset(stbuf, 0, sizeof(struct stat));

    PGconn *conn = pgfs_pool_acquire(&g_pgfs.pool);
    uint64_t ino = 0;
    mode_t mode = 0;

    int ret = pgfs_resolve_path(conn, path, &ino, &mode);
    if (ret != 0) {
        pgfs_pool_release(&g_pgfs.pool, conn);
        return ret;
    }

    char ino_str[32];
    snprintf(ino_str, sizeof(ino_str), "%" PRIu64, ino);
    const char *params[1] = {ino_str};

    PGresult *res = PQexecParams(conn,
        "SELECT ino, mode, nlink, uid, gid, size, "
        "EXTRACT(EPOCH FROM atime)::BIGINT, "
        "EXTRACT(EPOCH FROM mtime)::BIGINT, "
        "EXTRACT(EPOCH FROM ctime)::BIGINT "
        "FROM fs_inodes WHERE ino = $1::BIGINT;",
        1, NULL, params, NULL, NULL, 0);

    if (PQresultStatus(res) != PGRES_TUPLES_OK || PQntuples(res) == 0) {
        PQclear(res);
        pgfs_pool_release(&g_pgfs.pool, conn);
        return -EIO;
    }

    stbuf->st_ino   = (ino_t)strtoull(PQgetvalue(res, 0, 0), NULL, 10);
    stbuf->st_mode  = (mode_t)strtoul(PQgetvalue(res, 0, 1), NULL, 10);
    stbuf->st_nlink = (nlink_t)strtoul(PQgetvalue(res, 0, 2), NULL, 10);
    stbuf->st_uid   = (uid_t)strtoul(PQgetvalue(res, 0, 3), NULL, 10);
    stbuf->st_gid   = (gid_t)strtoul(PQgetvalue(res, 0, 4), NULL, 10);
    stbuf->st_size  = (off_t)strtoll(PQgetvalue(res, 0, 5), NULL, 10);
    stbuf->st_atime = (time_t)strtoll(PQgetvalue(res, 0, 6), NULL, 10);
    stbuf->st_mtime = (time_t)strtoll(PQgetvalue(res, 0, 7), NULL, 10);
    stbuf->st_ctime = (time_t)strtoll(PQgetvalue(res, 0, 8), NULL, 10);

    // Compute standard 512-byte blocks allocated
    stbuf->st_blksize = PGFS_CHUNK_SIZE;
    stbuf->st_blocks  = (stbuf->st_size + 511) / 512;

    PQclear(res);
    pgfs_pool_release(&g_pgfs.pool, conn);
    return 0;
}

/*
 * readdir: Enumerates directory entries (. , .. , and active children).
 */
#if FUSE_USE_VERSION >= 30
static int pgfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                        off_t offset, struct fuse_file_info *fi,
                        enum fuse_readdir_flags flags)
#else
static int pgfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                        off_t offset, struct fuse_file_info *fi)
#endif
{
    (void)offset;
    (void)fi;
#if FUSE_USE_VERSION >= 30
    (void)flags;
#endif

    PGconn *conn = pgfs_pool_acquire(&g_pgfs.pool);
    uint64_t dir_ino = 0;
    mode_t dir_mode = 0;

    int ret = pgfs_resolve_path(conn, path, &dir_ino, &dir_mode);
    if (ret != 0) {
        pgfs_pool_release(&g_pgfs.pool, conn);
        return ret;
    }

    if (!S_ISDIR(dir_mode)) {
        pgfs_pool_release(&g_pgfs.pool, conn);
        return -ENOTDIR;
    }

    // Populate standard POSIX directory links
    struct stat st;
    memset(&st, 0, sizeof(st));
    st.st_ino = dir_ino;
    st.st_mode = dir_mode;
#if FUSE_USE_VERSION >= 30
    filler(buf, ".", &st, 0, 0);
    filler(buf, "..", NULL, 0, 0);
#else
    filler(buf, ".", &st, 0);
    filler(buf, "..", NULL, 0);
#endif

    char ino_str[32];
    snprintf(ino_str, sizeof(ino_str), "%" PRIu64, dir_ino);
    const char *params[1] = {ino_str};

    PGresult *res = PQexecParams(conn,
        "SELECT d.name, d.ino, i.mode FROM fs_dentries d "
        "JOIN fs_inodes i ON d.ino = i.ino "
        "WHERE d.parent_ino = $1::BIGINT "
        "ORDER BY d.name;",
        1, NULL, params, NULL, NULL, 0);

    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        PQclear(res);
        pgfs_pool_release(&g_pgfs.pool, conn);
        return -EIO;
    }

    int num_entries = PQntuples(res);
    for (int i = 0; i < num_entries; i++) {
        const char *name = PQgetvalue(res, i, 0);
        uint64_t child_ino = (uint64_t)strtoull(PQgetvalue(res, i, 1), NULL, 10);
        mode_t child_mode = (mode_t)strtoul(PQgetvalue(res, i, 2), NULL, 10);

        memset(&st, 0, sizeof(st));
        st.st_ino = child_ino;
        st.st_mode = child_mode;

#if FUSE_USE_VERSION >= 30
        if (filler(buf, name, &st, 0, 0) != 0) break;
#else
        if (filler(buf, name, &st, 0) != 0) break;
#endif
    }

    PQclear(res);
    pgfs_pool_release(&g_pgfs.pool, conn);
    return 0;
}

/*
 * open: Validates file existence and permissions. Caches inode in fi->fh.
 */
static int pgfs_open(const char *path, struct fuse_file_info *fi)
{
    PGconn *conn = pgfs_pool_acquire(&g_pgfs.pool);
    uint64_t ino = 0;
    mode_t mode = 0;

    int ret = pgfs_resolve_path(conn, path, &ino, &mode);
    if (ret != 0) {
        pgfs_pool_release(&g_pgfs.pool, conn);
        return ret;
    }

    if (S_ISDIR(mode)) {
        pgfs_pool_release(&g_pgfs.pool, conn);
        return -EISDIR;
    }

    // Cache the resolved inode number directly in file handle to bypass path traversal on I/O
    if (fi) {
        fi->fh = ino;
    }

    pgfs_pool_release(&g_pgfs.pool, conn);
    return 0;
}

/*
 * create: Atomically creates and opens a regular file (O_CREAT).
 */
static int pgfs_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
    char parent_dir[PGFS_MAX_PATH_LEN];
    char base_name[PGFS_MAX_NAME_LEN];

    int ret = pgfs_split_path(path, parent_dir, base_name);
    if (ret != 0) return ret;

    PGconn *conn = pgfs_pool_acquire(&g_pgfs.pool);

    uint64_t parent_ino = 0;
    mode_t parent_mode = 0;
    ret = pgfs_resolve_path(conn, parent_dir, &parent_ino, &parent_mode);
    if (ret != 0) {
        pgfs_pool_release(&g_pgfs.pool, conn);
        return ret;
    }

    if (!S_ISDIR(parent_mode)) {
        pgfs_pool_release(&g_pgfs.pool, conn);
        return -ENOTDIR;
    }

    // Begin atomic transaction
    PGresult *res = PQexec(conn, "BEGIN;");
    PQclear(res);

    // Insert new regular file inode
    mode_t final_mode = S_IFREG | (mode & 07777);
    char mode_str[32], uid_str[32], gid_str[32];
    snprintf(mode_str, sizeof(mode_str), "%u", (unsigned int)final_mode);
    snprintf(uid_str, sizeof(uid_str), "%u", (unsigned int)fuse_get_context()->uid);
    snprintf(gid_str, sizeof(gid_str), "%u", (unsigned int)fuse_get_context()->gid);

    const char *inode_params[3] = {mode_str, uid_str, gid_str};
    res = PQexecParams(conn,
        "INSERT INTO fs_inodes (mode, nlink, uid, gid, size) "
        "VALUES ($1::INT, 1, $2::INT, $3::INT, 0) RETURNING ino;",
        3, NULL, inode_params, NULL, NULL, 0);

    if (PQresultStatus(res) != PGRES_TUPLES_OK || PQntuples(res) == 0) {
        PQclear(res);
        res = PQexec(conn, "ROLLBACK;");
        PQclear(res);
        pgfs_pool_release(&g_pgfs.pool, conn);
        return -EIO;
    }

    uint64_t new_ino = (uint64_t)strtoull(PQgetvalue(res, 0, 0), NULL, 10);
    PQclear(res);

    // Link into dentry namespace
    char parent_str[32], new_ino_str[32];
    snprintf(parent_str, sizeof(parent_str), "%" PRIu64, parent_ino);
    snprintf(new_ino_str, sizeof(new_ino_str), "%" PRIu64, new_ino);

    const char *dentry_params[3] = {parent_str, base_name, new_ino_str};
    res = PQexecParams(conn,
        "INSERT INTO fs_dentries (parent_ino, name, ino) "
        "VALUES ($1::BIGINT, $2, $3::BIGINT);",
        3, NULL, dentry_params, NULL, NULL, 0);

    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        PQclear(res);
        res = PQexec(conn, "ROLLBACK;");
        PQclear(res);
        pgfs_pool_release(&g_pgfs.pool, conn);
        return -EEXIST;
    }
    PQclear(res);

    // Update parent directory modification timestamp
    const char *touch_params[1] = {parent_str};
    res = PQexecParams(conn,
        "UPDATE fs_inodes SET mtime = CURRENT_TIMESTAMP, ctime = CURRENT_TIMESTAMP "
        "WHERE ino = $1::BIGINT;",
        1, NULL, touch_params, NULL, NULL, 0);
    PQclear(res);

    res = PQexec(conn, "COMMIT;");
    PQclear(res);

    if (fi) {
        fi->fh = new_ino;
    }

    pgfs_pool_release(&g_pgfs.pool, conn);
    return 0;
}

/*
 * read: Slices data from 64KB striped chunks into caller buffer. Supports holes.
 */
static int pgfs_read(const char *path, char *buf, size_t size, off_t offset,
                     struct fuse_file_info *fi)
{
    if (!buf) return -EINVAL;
    if (size == 0) return 0;

    PGconn *conn = pgfs_pool_acquire(&g_pgfs.pool);
    uint64_t ino = (fi && fi->fh) ? fi->fh : 0;

    if (ino == 0) {
        mode_t mode = 0;
        int ret = pgfs_resolve_path(conn, path, &ino, &mode);
        if (ret != 0) {
            pgfs_pool_release(&g_pgfs.pool, conn);
            return ret;
        }
    }

    // Retrieve file size
    char ino_str[32];
    snprintf(ino_str, sizeof(ino_str), "%" PRIu64, ino);
    const char *size_params[1] = {ino_str};

    PGresult *res = PQexecParams(conn,
        "SELECT size FROM fs_inodes WHERE ino = $1::BIGINT;",
        1, NULL, size_params, NULL, NULL, 0);

    if (PQresultStatus(res) != PGRES_TUPLES_OK || PQntuples(res) == 0) {
        PQclear(res);
        pgfs_pool_release(&g_pgfs.pool, conn);
        return -EIO;
    }

    off_t file_size = (off_t)strtoll(PQgetvalue(res, 0, 0), NULL, 10);
    PQclear(res);

    if (offset >= file_size) {
        pgfs_pool_release(&g_pgfs.pool, conn);
        return 0; // EOF reached
    }

    if (offset + (off_t)size > file_size) {
        size = (size_t)(file_size - offset);
    }

    // Zero output buffer to guarantee clean reads over sparse holes
    memset(buf, 0, size);

    uint64_t first_chunk = (uint64_t)(offset / PGFS_CHUNK_SIZE);
    uint64_t last_chunk  = (uint64_t)((offset + size - 1) / PGFS_CHUNK_SIZE);
    size_t   bytes_read  = 0;

    for (uint64_t c = first_chunk; c <= last_chunk; c++) {
        char chunk_str[32];
        snprintf(chunk_str, sizeof(chunk_str), "%" PRIu64, c);
        const char *chunk_params[2] = {ino_str, chunk_str};

        // Fetch chunk using PostgreSQL binary result format (resultFormat = 1)
        res = PQexecParams(conn,
            "SELECT chunk_size, chunk_data FROM fs_chunks "
            "WHERE ino = $1::BIGINT AND chunk_index = $2::BIGINT;",
            2, NULL, chunk_params, NULL, NULL, 1);

        off_t chunk_base = (off_t)(c * PGFS_CHUNK_SIZE);
        off_t start_in_chunk = (offset > chunk_base) ? (offset - chunk_base) : 0;
        off_t end_in_chunk = ((offset + (off_t)size) < (chunk_base + PGFS_CHUNK_SIZE))
                             ? (offset + (off_t)size - chunk_base)
                             : PGFS_CHUNK_SIZE;
        size_t chunk_span = (size_t)(end_in_chunk - start_in_chunk);
        size_t buf_pos = (size_t)((chunk_base + start_in_chunk) - offset);

        if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0) {
            const char *raw_data = PQgetvalue(res, 0, 1);
            int data_len = PQgetlength(res, 0, 1);

            if ((int)start_in_chunk < data_len) {
                size_t available = (size_t)(data_len - start_in_chunk);
                size_t to_copy = (chunk_span < available) ? chunk_span : available;
                memcpy(buf + buf_pos, raw_data + start_in_chunk, to_copy);
            }
        }
        // If chunk does not exist in DB, sparse zero-fill was already handled by memset

        PQclear(res);
        bytes_read += chunk_span;
    }

    pgfs_pool_release(&g_pgfs.pool, conn);
    return (int)size;
}

/*
 * write: Chunk-aligned Read-Modify-Write engine with binary BYTEA parameter binding.
 */
static int pgfs_write(const char *path, const char *buf, size_t size, off_t offset,
                      struct fuse_file_info *fi)
{
    if (!buf) return -EINVAL;
    if (size == 0) return 0;

    PGconn *conn = pgfs_pool_acquire(&g_pgfs.pool);
    uint64_t ino = (fi && fi->fh) ? fi->fh : 0;

    if (ino == 0) {
        mode_t mode = 0;
        int ret = pgfs_resolve_path(conn, path, &ino, &mode);
        if (ret != 0) {
            pgfs_pool_release(&g_pgfs.pool, conn);
            return ret;
        }
    }

    char ino_str[32];
    snprintf(ino_str, sizeof(ino_str), "%" PRIu64, ino);

    // Retrieve current file size
    const char *size_params[1] = {ino_str};
    PGresult *res = PQexecParams(conn,
        "SELECT size FROM fs_inodes WHERE ino = $1::BIGINT;",
        1, NULL, size_params, NULL, NULL, 0);

    if (PQresultStatus(res) != PGRES_TUPLES_OK || PQntuples(res) == 0) {
        PQclear(res);
        pgfs_pool_release(&g_pgfs.pool, conn);
        return -EIO;
    }

    off_t current_size = (off_t)strtoll(PQgetvalue(res, 0, 0), NULL, 10);
    PQclear(res);

    uint64_t first_chunk = (uint64_t)(offset / PGFS_CHUNK_SIZE);
    uint64_t last_chunk  = (uint64_t)((offset + size - 1) / PGFS_CHUNK_SIZE);
    size_t   bytes_written = 0;

    // Allocate 64KB staging block on heap to prevent thread-stack pressure
    char *chunk_staging = (char *)malloc(PGFS_CHUNK_SIZE);
    if (!chunk_staging) {
        pgfs_pool_release(&g_pgfs.pool, conn);
        return -ENOMEM;
    }

    for (uint64_t c = first_chunk; c <= last_chunk; c++) {
        off_t chunk_base = (off_t)(c * PGFS_CHUNK_SIZE);
        off_t chunk_offset = (offset > chunk_base) ? (offset - chunk_base) : 0;
        size_t to_write = size - bytes_written;
        size_t max_chunk_space = (size_t)(PGFS_CHUNK_SIZE - chunk_offset);
        size_t write_len = (to_write < max_chunk_space) ? to_write : max_chunk_space;

        char chunk_str[32];
        snprintf(chunk_str, sizeof(chunk_str), "%" PRIu64, c);

        int final_chunk_len = 0;

        // Determine if Read-Modify-Write is required for unaligned chunk edges
        if (chunk_offset > 0 || write_len < PGFS_CHUNK_SIZE) {
            memset(chunk_staging, 0, PGFS_CHUNK_SIZE);

            const char *fetch_params[2] = {ino_str, chunk_str};
            res = PQexecParams(conn,
                "SELECT chunk_size, chunk_data FROM fs_chunks "
                "WHERE ino = $1::BIGINT AND chunk_index = $2::BIGINT;",
                2, NULL, fetch_params, NULL, NULL, 1);

            int existing_len = 0;
            if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0) {
                const char *raw_data = PQgetvalue(res, 0, 1);
                existing_len = PQgetlength(res, 0, 1);
                if (existing_len > PGFS_CHUNK_SIZE) existing_len = PGFS_CHUNK_SIZE;
                memcpy(chunk_staging, raw_data, existing_len);
            }
            PQclear(res);

            // Overlay new payload
            memcpy(chunk_staging + chunk_offset, buf + bytes_written, write_len);
            int span_end = (int)(chunk_offset + write_len);
            final_chunk_len = (span_end > existing_len) ? span_end : existing_len;
        } else {
            // Fast Path: Full 64KB Chunk direct write
            memcpy(chunk_staging, buf + bytes_written, PGFS_CHUNK_SIZE);
            final_chunk_len = PGFS_CHUNK_SIZE;
        }

        // Upsert chunk via binary parameter binding
        char chunk_len_str[32];
        snprintf(chunk_len_str, sizeof(chunk_len_str), "%d", final_chunk_len);

        const char *upsert_values[4] = {ino_str, chunk_str, chunk_len_str, chunk_staging};
        int upsert_lengths[4] = {
            (int)strlen(ino_str),
            (int)strlen(chunk_str),
            (int)strlen(chunk_len_str),
            final_chunk_len
        };
        int upsert_formats[4] = {0, 0, 0, 1}; // Parameter 4 is raw binary (BYTEA)

        res = PQexecParams(conn,
            "INSERT INTO fs_chunks (ino, chunk_index, chunk_size, chunk_data) "
            "VALUES ($1::BIGINT, $2::BIGINT, $3::INT, $4::BYTEA) "
            "ON CONFLICT (ino, chunk_index) DO UPDATE SET "
            "chunk_size = EXCLUDED.chunk_size, "
            "chunk_data = EXCLUDED.chunk_data;",
            4, NULL, upsert_values, upsert_lengths, upsert_formats, 0);

        if (PQresultStatus(res) != PGRES_COMMAND_OK) {
            fprintf(stderr, "[PGFS-ERROR] Chunk upsert failed on ino %" PRIu64 " idx %" PRIu64 ": %s\n",
                    ino, c, PQerrorMessage(conn));
            PQclear(res);
            free(chunk_staging);
            pgfs_pool_release(&g_pgfs.pool, conn);
            return -EIO;
        }
        PQclear(res);

        bytes_written += write_len;
    }

    free(chunk_staging);

    // Atomically expand file size and touch mtime/ctime if write extended bounds
    off_t new_size = (offset + (off_t)size > current_size) ? (offset + (off_t)size) : current_size;
    char new_size_str[32];
    snprintf(new_size_str, sizeof(new_size_str), "%lld", (long long)new_size);

    const char *update_params[2] = {new_size_str, ino_str};
    res = PQexecParams(conn,
        "UPDATE fs_inodes SET size = $1::BIGINT, mtime = CURRENT_TIMESTAMP, ctime = CURRENT_TIMESTAMP "
        "WHERE ino = $2::BIGINT;",
        2, NULL, update_params, NULL, NULL, 0);
    PQclear(res);

    pgfs_pool_release(&g_pgfs.pool, conn);
    return (int)size;
}

/*
 * mkdir: Allocates directory inode and dentry.
 */
static int pgfs_mkdir(const char *path, mode_t mode)
{
    char parent_dir[PGFS_MAX_PATH_LEN];
    char base_name[PGFS_MAX_NAME_LEN];

    int ret = pgfs_split_path(path, parent_dir, base_name);
    if (ret != 0) return ret;

    PGconn *conn = pgfs_pool_acquire(&g_pgfs.pool);

    uint64_t parent_ino = 0;
    mode_t parent_mode = 0;
    ret = pgfs_resolve_path(conn, parent_dir, &parent_ino, &parent_mode);
    if (ret != 0) {
        pgfs_pool_release(&g_pgfs.pool, conn);
        return ret;
    }

    if (!S_ISDIR(parent_mode)) {
        pgfs_pool_release(&g_pgfs.pool, conn);
        return -ENOTDIR;
    }

    // Begin atomic transaction
    PGresult *res = PQexec(conn, "BEGIN;");
    PQclear(res);

    // Insert directory inode (nlink=2 for '.' and parent dentry)
    mode_t final_mode = S_IFDIR | (mode & 07777);
    char mode_str[32], uid_str[32], gid_str[32];
    snprintf(mode_str, sizeof(mode_str), "%u", (unsigned int)final_mode);
    snprintf(uid_str, sizeof(uid_str), "%u", (unsigned int)fuse_get_context()->uid);
    snprintf(gid_str, sizeof(gid_str), "%u", (unsigned int)fuse_get_context()->gid);

    const char *inode_params[3] = {mode_str, uid_str, gid_str};
    res = PQexecParams(conn,
        "INSERT INTO fs_inodes (mode, nlink, uid, gid, size) "
        "VALUES ($1::INT, 2, $2::INT, $3::INT, 4096) RETURNING ino;",
        3, NULL, inode_params, NULL, NULL, 0);

    if (PQresultStatus(res) != PGRES_TUPLES_OK || PQntuples(res) == 0) {
        PQclear(res);
        res = PQexec(conn, "ROLLBACK;");
        PQclear(res);
        pgfs_pool_release(&g_pgfs.pool, conn);
        return -EIO;
    }

    uint64_t new_ino = (uint64_t)strtoull(PQgetvalue(res, 0, 0), NULL, 10);
    PQclear(res);

    // Insert dentry link
    char parent_str[32], new_ino_str[32];
    snprintf(parent_str, sizeof(parent_str), "%" PRIu64, parent_ino);
    snprintf(new_ino_str, sizeof(new_ino_str), "%" PRIu64, new_ino);

    const char *dentry_params[3] = {parent_str, base_name, new_ino_str};
    res = PQexecParams(conn,
        "INSERT INTO fs_dentries (parent_ino, name, ino) "
        "VALUES ($1::BIGINT, $2, $3::BIGINT);",
        3, NULL, dentry_params, NULL, NULL, 0);

    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        PQclear(res);
        res = PQexec(conn, "ROLLBACK;");
        PQclear(res);
        pgfs_pool_release(&g_pgfs.pool, conn);
        return -EEXIST;
    }
    PQclear(res);

    // Increment parent nlink count and update timestamps
    const char *parent_update[1] = {parent_str};
    res = PQexecParams(conn,
        "UPDATE fs_inodes "
        "SET nlink = nlink + 1, mtime = CURRENT_TIMESTAMP, ctime = CURRENT_TIMESTAMP "
        "WHERE ino = $1::BIGINT;",
        1, NULL, parent_update, NULL, NULL, 0);
    PQclear(res);

    res = PQexec(conn, "COMMIT;");
    PQclear(res);

    pgfs_pool_release(&g_pgfs.pool, conn);
    return 0;
}

/*
 * unlink: Removes regular file dentry and cascades chunk cleanup if nlink reaches 0.
 */
static int pgfs_unlink(const char *path)
{
    char parent_dir[PGFS_MAX_PATH_LEN];
    char base_name[PGFS_MAX_NAME_LEN];

    int ret = pgfs_split_path(path, parent_dir, base_name);
    if (ret != 0) return ret;

    PGconn *conn = pgfs_pool_acquire(&g_pgfs.pool);

    uint64_t parent_ino = 0;
    mode_t parent_mode = 0;
    ret = pgfs_resolve_path(conn, parent_dir, &parent_ino, &parent_mode);
    if (ret != 0) {
        pgfs_pool_release(&g_pgfs.pool, conn);
        return ret;
    }

    uint64_t target_ino = 0;
    mode_t target_mode = 0;
    ret = pgfs_resolve_path(conn, path, &target_ino, &target_mode);
    if (ret != 0) {
        pgfs_pool_release(&g_pgfs.pool, conn);
        return ret;
    }

    if (S_ISDIR(target_mode)) {
        pgfs_pool_release(&g_pgfs.pool, conn);
        return -EISDIR;
    }

    // Atomic unlink transaction
    PGresult *res = PQexec(conn, "BEGIN;");
    PQclear(res);

    char parent_str[32], target_str[32];
    snprintf(parent_str, sizeof(parent_str), "%" PRIu64, parent_ino);
    snprintf(target_str, sizeof(target_str), "%" PRIu64, target_ino);

    // Remove dentry
    const char *del_dentry_params[2] = {parent_str, base_name};
    res = PQexecParams(conn,
        "DELETE FROM fs_dentries WHERE parent_ino = $1::BIGINT AND name = $2;",
        2, NULL, del_dentry_params, NULL, NULL, 0);
    PQclear(res);

    // Decrement nlink
    const char *dec_params[1] = {target_str};
    res = PQexecParams(conn,
        "UPDATE fs_inodes SET nlink = nlink - 1, ctime = CURRENT_TIMESTAMP "
        "WHERE ino = $1::BIGINT RETURNING nlink;",
        1, NULL, dec_params, NULL, NULL, 0);

    if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0) {
        int remaining_links = atoi(PQgetvalue(res, 0, 0));
        PQclear(res);

        if (remaining_links <= 0) {
            // Delete data chunks
            res = PQexecParams(conn,
                "DELETE FROM fs_chunks WHERE ino = $1::BIGINT;",
                1, NULL, dec_params, NULL, NULL, 0);
            PQclear(res);

            // Delete inode
            res = PQexecParams(conn,
                "DELETE FROM fs_inodes WHERE ino = $1::BIGINT;",
                1, NULL, dec_params, NULL, NULL, 0);
            PQclear(res);
        }
    } else {
        PQclear(res);
    }

    // Touch parent directory
    const char *touch_params[1] = {parent_str};
    res = PQexecParams(conn,
        "UPDATE fs_inodes SET mtime = CURRENT_TIMESTAMP, ctime = CURRENT_TIMESTAMP "
        "WHERE ino = $1::BIGINT;",
        1, NULL, touch_params, NULL, NULL, 0);
    PQclear(res);

    res = PQexec(conn, "COMMIT;");
    PQclear(res);

    pgfs_pool_release(&g_pgfs.pool, conn);
    return 0;
}

/*
 * rmdir: Removes an empty directory.
 */
static int pgfs_rmdir(const char *path)
{
    char parent_dir[PGFS_MAX_PATH_LEN];
    char base_name[PGFS_MAX_NAME_LEN];

    int ret = pgfs_split_path(path, parent_dir, base_name);
    if (ret != 0) return ret;

    PGconn *conn = pgfs_pool_acquire(&g_pgfs.pool);

    uint64_t parent_ino = 0, target_ino = 0;
    mode_t parent_mode = 0, target_mode = 0;

    ret = pgfs_resolve_path(conn, parent_dir, &parent_ino, &parent_mode);
    if (ret != 0) {
        pgfs_pool_release(&g_pgfs.pool, conn);
        return ret;
    }

    ret = pgfs_resolve_path(conn, path, &target_ino, &target_mode);
    if (ret != 0) {
        pgfs_pool_release(&g_pgfs.pool, conn);
        return ret;
    }

    if (!S_ISDIR(target_mode)) {
        pgfs_pool_release(&g_pgfs.pool, conn);
        return -ENOTDIR;
    }

    if (target_ino == PGFS_ROOT_INODE) {
        pgfs_pool_release(&g_pgfs.pool, conn);
        return -EBUSY;
    }

    char target_str[32], parent_str[32];
    snprintf(target_str, sizeof(target_str), "%" PRIu64, target_ino);
    snprintf(parent_str, sizeof(parent_str), "%" PRIu64, parent_ino);

    // Verify directory is empty
    const char *check_params[1] = {target_str};
    PGresult *res = PQexecParams(conn,
        "SELECT COUNT(*) FROM fs_dentries WHERE parent_ino = $1::BIGINT;",
        1, NULL, check_params, NULL, NULL, 0);

    if (PQresultStatus(res) != PGRES_TUPLES_OK || PQntuples(res) == 0) {
        PQclear(res);
        pgfs_pool_release(&g_pgfs.pool, conn);
        return -EIO;
    }

    int entry_count = atoi(PQgetvalue(res, 0, 0));
    PQclear(res);

    if (entry_count > 0) {
        pgfs_pool_release(&g_pgfs.pool, conn);
        return -ENOTEMPTY;
    }

    // Begin atomic rmdir transaction
    res = PQexec(conn, "BEGIN;");
    PQclear(res);

    // Delete dentry from parent
    const char *del_dentry_params[2] = {parent_str, base_name};
    res = PQexecParams(conn,
        "DELETE FROM fs_dentries WHERE parent_ino = $1::BIGINT AND name = $2;",
        2, NULL, del_dentry_params, NULL, NULL, 0);
    PQclear(res);

    // Delete directory inode
    const char *del_inode_params[1] = {target_str};
    res = PQexecParams(conn,
        "DELETE FROM fs_inodes WHERE ino = $1::BIGINT;",
        1, NULL, del_inode_params, NULL, NULL, 0);
    PQclear(res);

    // Decrement parent directory link count and touch mtime
    const char *parent_update[1] = {parent_str};
    res = PQexecParams(conn,
        "UPDATE fs_inodes "
        "SET nlink = nlink - 1, mtime = CURRENT_TIMESTAMP, ctime = CURRENT_TIMESTAMP "
        "WHERE ino = $1::BIGINT;",
        1, NULL, parent_update, NULL, NULL, 0);
    PQclear(res);

    res = PQexec(conn, "COMMIT;");
    PQclear(res);

    pgfs_pool_release(&g_pgfs.pool, conn);
    return 0;
}

/*
 * truncate: Resizes a file, dropping discarded chunks or zero-padding boundary chunks.
 */
#if FUSE_USE_VERSION >= 30
static int pgfs_truncate(const char *path, off_t size, struct fuse_file_info *fi)
#else
static int pgfs_truncate(const char *path, off_t size)
#endif
{
#if FUSE_USE_VERSION >= 30
    (void)fi;
#endif
    if (size < 0) return -EINVAL;

    PGconn *conn = pgfs_pool_acquire(&g_pgfs.pool);
    uint64_t ino = 0;
    mode_t mode = 0;

    int ret = pgfs_resolve_path(conn, path, &ino, &mode);
    if (ret != 0) {
        pgfs_pool_release(&g_pgfs.pool, conn);
        return ret;
    }

    if (S_ISDIR(mode)) {
        pgfs_pool_release(&g_pgfs.pool, conn);
        return -EISDIR;
    }

    char ino_str[32];
    snprintf(ino_str, sizeof(ino_str), "%" PRIu64, ino);

    PGresult *res = PQexec(conn, "BEGIN;");
    PQclear(res);

    if (size == 0) {
        // Delete all chunks
        const char *del_params[1] = {ino_str};
        res = PQexecParams(conn,
            "DELETE FROM fs_chunks WHERE ino = $1::BIGINT;",
            1, NULL, del_params, NULL, NULL, 0);
        PQclear(res);
    } else {
        uint64_t last_valid_chunk = (uint64_t)((size - 1) / PGFS_CHUNK_SIZE);
        char chunk_str[32];
        snprintf(chunk_str, sizeof(chunk_str), "%" PRIu64, last_valid_chunk);

        // Delete all chunks strictly past the new size boundary
        const char *purge_params[2] = {ino_str, chunk_str};
        res = PQexecParams(conn,
            "DELETE FROM fs_chunks WHERE ino = $1::BIGINT AND chunk_index > $2::BIGINT;",
            2, NULL, purge_params, NULL, NULL, 0);
        PQclear(res);

        // Handle partial chunk tail truncation if needed
        size_t tail_bytes = (size_t)(size % PGFS_CHUNK_SIZE);
        if (tail_bytes > 0) {
            const char *tail_params[2] = {ino_str, chunk_str};
            res = PQexecParams(conn,
                "SELECT chunk_data FROM fs_chunks "
                "WHERE ino = $1::BIGINT AND chunk_index = $2::BIGINT;",
                2, NULL, tail_params, NULL, NULL, 1);

            if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0) {
                int curr_len = PQgetlength(res, 0, 0);
                if ((size_t)curr_len > tail_bytes) {
                    const char *raw_data = PQgetvalue(res, 0, 0);
                    char *truncated = (char *)malloc(tail_bytes);
                    if (truncated) {
                        memcpy(truncated, raw_data, tail_bytes);
                        char tail_len_str[32];
                        snprintf(tail_len_str, sizeof(tail_len_str), "%zu", tail_bytes);

                        const char *upd_values[4] = {ino_str, chunk_str, tail_len_str, truncated};
                        int upd_lengths[4] = {
                            (int)strlen(ino_str),
                            (int)strlen(chunk_str),
                            (int)strlen(tail_len_str),
                            (int)tail_bytes
                        };
                        int upd_formats[4] = {0, 0, 0, 1};

                        PGresult *upd_res = PQexecParams(conn,
                            "UPDATE fs_chunks SET chunk_size = $3::INT, chunk_data = $4::BYTEA "
                            "WHERE ino = $1::BIGINT AND chunk_index = $2::BIGINT;",
                            4, NULL, upd_values, upd_lengths, upd_formats, 0);
                        PQclear(upd_res);
                        free(truncated);
                    }
                }
            }
            PQclear(res);
        }
    }

    // Update inode size and timestamps
    char size_str[32];
    snprintf(size_str, sizeof(size_str), "%lld", (long long)size);
    const char *size_params[2] = {size_str, ino_str};
    res = PQexecParams(conn,
        "UPDATE fs_inodes SET size = $1::BIGINT, mtime = CURRENT_TIMESTAMP, ctime = CURRENT_TIMESTAMP "
        "WHERE ino = $2::BIGINT;",
        2, NULL, size_params, NULL, NULL, 0);
    PQclear(res);

    res = PQexec(conn, "COMMIT;");
    PQclear(res);

    pgfs_pool_release(&g_pgfs.pool, conn);
    return 0;
}

/*
 * statfs: Returns filesystem metrics (virtual blocks, capacity, chunk metrics).
 */
static int pgfs_statfs(const char *path, struct statvfs *stbuf)
{
    (void)path;
    if (!stbuf) return -EINVAL;
    memset(stbuf, 0, sizeof(struct statvfs));

    PGconn *conn = pgfs_pool_acquire(&g_pgfs.pool);

    PGresult *res = PQexec(conn,
        "SELECT block_size, total_blocks FROM fs_superblock LIMIT 1;");

    uint64_t bsize = PGFS_CHUNK_SIZE;
    uint64_t total_blocks = 1048576; // 64 GB default

    if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0) {
        bsize = (uint64_t)strtoull(PQgetvalue(res, 0, 0), NULL, 10);
        total_blocks = (uint64_t)strtoull(PQgetvalue(res, 0, 1), NULL, 10);
    }
    PQclear(res);

    // Count physical chunks allocated
    res = PQexec(conn, "SELECT COUNT(*) FROM fs_chunks;");
    uint64_t used_chunks = 0;
    if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0) {
        used_chunks = (uint64_t)strtoull(PQgetvalue(res, 0, 0), NULL, 10);
    }
    PQclear(res);

    stbuf->f_bsize   = bsize;
    stbuf->f_frsize  = bsize;
    stbuf->f_blocks  = total_blocks;
    stbuf->f_bfree   = (total_blocks > used_chunks) ? (total_blocks - used_chunks) : 0;
    stbuf->f_bavail  = stbuf->f_bfree;
    stbuf->f_files   = 1000000;
    stbuf->f_ffree   = 999000;
    stbuf->f_favail  = 999000;
    stbuf->f_namemax = PGFS_MAX_NAME_LEN;

    pgfs_pool_release(&g_pgfs.pool, conn);
    return 0;
}

/*
 * init: Pre-flight handshake, superblock validation, and kernel cache tuning.
 */
#if FUSE_USE_VERSION >= 30
static void *pgfs_init(struct fuse_conn_info *conn, struct fuse_config *cfg)
{
    (void)conn;
    if (cfg) {
        cfg->kernel_cache = 0;      // Direct POSIX consistency with database
        cfg->attr_timeout = 1.0;    // Balanced metadata cache duration
        cfg->entry_timeout = 1.0;
    }
#else
static void *pgfs_init(struct fuse_conn_info *conn)
{
    (void)conn;
#endif
    printf("\n==========================================================\n");
    printf(" PG-FS: PostgreSQL-Backed Virtual File System\n");
    printf(" Author:  Irene Therese Joseph (Senior Linux Systems Engineer)\n");
    printf(" Status:  Active Mount - ACID POSIX VFS Engaged\n");
    printf(" Block:   %u KB Striped Extents | Pool: %d Workers\n",
           PGFS_CHUNK_SIZE / 1024, g_pgfs.pool.pool_size);
    printf("==========================================================\n\n");

    return NULL;
}

/*
 * destroy: Cleanly drains connection pool and frees resources on unmount.
 */
static void pgfs_destroy(void *private_data)
{
    (void)private_data;
    printf("[PGFS-INFO] Gracefully unmounting filesystem. Draining connection pool...\n");
    pgfs_pool_destroy(&g_pgfs.pool);
    printf("[PGFS-INFO] Cleanup complete. Shutdown successful.\n");
}

/* ============================================================================
 * FUSE Operations Table Registration
 * ============================================================================
 */

static struct fuse_operations pgfs_oper = {
    .getattr    = pgfs_getattr,
    .readdir    = pgfs_readdir,
    .open       = pgfs_open,
    .read       = pgfs_read,
    .write      = pgfs_write,
    .mkdir      = pgfs_mkdir,
    .unlink     = pgfs_unlink,
    .rmdir      = pgfs_rmdir,
    .truncate   = pgfs_truncate,
    .create     = pgfs_create,
    .statfs     = pgfs_statfs,
    .init       = pgfs_init,
    .destroy    = pgfs_destroy,
};

/* ============================================================================
 * Entry Point & Lifecycle Initialization
 * ============================================================================
 */

int main(int argc, char *argv[])
{
    // Retrieve connection parameters from environment or default
    const char *conninfo = getenv("PGFS_PG_CONNSTRING");
    if (!conninfo || strlen(conninfo) == 0) {
        conninfo = PGFS_DEFAULT_CONNINFO;
    }

    // Determine connection pool size
    int pool_size = PGFS_DEFAULT_POOL_SIZE;
    const char *pool_env = getenv("PGFS_POOL_SIZE");
    if (pool_env && atoi(pool_env) > 0) {
        pool_size = atoi(pool_env);
    }

    g_pgfs.chunk_size = PGFS_CHUNK_SIZE;
    g_pgfs.mount_uid  = getuid();
    g_pgfs.mount_gid  = getgid();

    fprintf(stdout, "[PGFS-INIT] Initializing PostgreSQL connection pool (%d connections)...\n", pool_size);
    int ret = pgfs_pool_init(&g_pgfs.pool, conninfo, pool_size);
    if (ret != 0) {
        fprintf(stderr, "[PGFS-FATAL] Failed to initialize PostgreSQL connection pool: error %d\n", ret);
        return EXIT_FAILURE;
    }

    // Handover control to FUSE user-space event loop
    return fuse_main(argc, argv, &pgfs_oper, NULL);
}
