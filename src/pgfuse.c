/*
 * pgfuse: PostgreSQL-Backed Virtual File System
 *
 * Translates standard Linux file operations (read, write, mkdir, open, unlink)
 * into PostgreSQL SQL queries using FUSE 3 and libpq.
 */

#include "pgfuse.h"

/* Global runtime state (holds our connection pool and user permissions) */
static pgfuse_runtime_t g_pgfuse;

/* ============================================================================
 * Thread-Safe Connection Pool
 *
 * Why we need this:
 * FUSE is multi-threaded, but libpq connections are not thread-safe.
 * If two threads execute queries on the same connection at once, it crashes.
 * So we create a pool of 8 connections and use mutex locks to borrow/return them.
 * ============================================================================
 */

/*
 * Initialize the connection pool:
 * Opens 8 connections to PostgreSQL and sets up our mutex lock and condition bell.
 */
int pgfuse_pool_init(pgfuse_conn_pool_t *pool, const char *conninfo, int size)
{
    if (!pool || !conninfo || size <= 0) return -EINVAL;

    memset(pool, 0, sizeof(*pool));
    strncpy(pool->conninfo, conninfo, sizeof(pool->conninfo) - 1);
    pool->pool_size = size;

    /* Initialize POSIX mutex lock and condition variable */
    if (pthread_mutex_init(&pool->lock, NULL) != 0) return -ENOMEM;
    if (pthread_cond_init(&pool->cond, NULL) != 0) {
        pthread_mutex_destroy(&pool->lock);
        return -ENOMEM;
    }

    /* Allocate memory for connection pointers and busy/free tracking flags */
    pool->conns = (PGconn **)calloc(size, sizeof(PGconn *));
    pool->in_use = (bool *)calloc(size, sizeof(bool));
    if (!pool->conns || !pool->in_use) {
        free(pool->conns);
        free(pool->in_use);
        pthread_mutex_destroy(&pool->lock);
        pthread_cond_destroy(&pool->cond);
        return -ENOMEM;
    }

    /* Connect each channel to the PostgreSQL database */
    for (int i = 0; i < size; i++) {
        pool->conns[i] = PQconnectdb(pool->conninfo);
        if (PQstatus(pool->conns[i]) != CONNECTION_OK) {
            fprintf(stderr, "[PGFUSE-FATAL] Database connection [%d] failed: %s\n",
                    i, PQerrorMessage(pool->conns[i]));
            /* Clean up any connections that already opened */
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

/*
 * Cleanly close all connections and destroy locks when unmounting
 */
void pgfuse_pool_destroy(pgfuse_conn_pool_t *pool)
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

/*
 * Borrow a connection:
 * Finds an idle connection, marks it busy, and returns it.
 * If all 8 connections are busy, sleeps on the condition variable until one is freed.
 */
PGconn *pgfuse_pool_acquire(pgfuse_conn_pool_t *pool)
{
    pthread_mutex_lock(&pool->lock); /* Lock pool */
    while (1) {
        for (int i = 0; i < pool->pool_size; i++) {
            if (!pool->in_use[i]) {
                pool->in_use[i] = true; /* Mark connection as busy */
                PGconn *conn = pool->conns[i];
                pthread_mutex_unlock(&pool->lock); /* Unlock pool */

                /* Self-healing: if the connection dropped, reconnect it automatically */
                if (PQstatus(conn) != CONNECTION_OK) {
                    fprintf(stderr, "[PGFUSE-WARN] Reconnecting dropped channel [%d]...\n", i);
                    PQreset(conn);
                }
                return conn;
            }
        }
        /* All channels are busy: sleep until someone returns a connection */
        pthread_cond_wait(&pool->cond, &pool->lock);
    }
}

/*
 * Return a connection:
 * Marks connection idle and rings the bell (cond_signal) to wake up a waiting thread.
 */
void pgfuse_pool_release(pgfuse_conn_pool_t *pool, PGconn *conn)
{
    if (!pool || !conn) return;

    pthread_mutex_lock(&pool->lock);
    for (int i = 0; i < pool->pool_size; i++) {
        if (pool->conns[i] == conn) {
            pool->in_use[i] = false; /* Mark connection free */
            pthread_cond_signal(&pool->cond); /* Wake up a waiting thread */
            break;
        }
    }
    pthread_mutex_unlock(&pool->lock);
}

/* ============================================================================
 * Path & Namespace Resolution Helpers
 * ============================================================================
 */

/*
 * Split a full path into parent folder and file name:
 * e.g., "/documents/report.txt" -> parent = "/documents", base = "report.txt"
 *       "/hello.txt"            -> parent = "/",          base = "hello.txt"
 */
int pgfuse_split_path(const char *path, char *parent_dir, char *base_name)
{
    if (!path || !parent_dir || !base_name) return -EINVAL;

    size_t len = strlen(path);
    if (len == 0 || path[0] != '/') return -EINVAL;

    /* Handle root directory */
    if (strcmp(path, "/") == 0) {
        strcpy(parent_dir, "/");
        base_name[0] = '\0';
        return 0;
    }

    /* Find the last slash */
    const char *last_slash = strrchr(path, '/');
    if (!last_slash) return -EINVAL;

    if (last_slash == path) {
        strcpy(parent_dir, "/");
        strncpy(base_name, path + 1, PGFUSE_MAX_NAME_LEN - 1);
        base_name[PGFUSE_MAX_NAME_LEN - 1] = '\0';
    } else {
        size_t parent_len = (size_t)(last_slash - path);
        if (parent_len >= PGFUSE_MAX_PATH_LEN) return -ENAMETOOLONG;
        strncpy(parent_dir, path, parent_len);
        parent_dir[parent_len] = '\0';

        strncpy(base_name, last_slash + 1, PGFUSE_MAX_NAME_LEN - 1);
        base_name[PGFUSE_MAX_NAME_LEN - 1] = '\0';
    }

    return 0;
}

/*
 * Translate a path string like "/home/user/file.txt" into its Inode ID number.
 * Starts at root folder (Inode 1) and walks down each folder using strtok_r.
 */
int pgfuse_resolve_path(PGconn *conn, const char *path, uint64_t *out_ino, mode_t *out_mode)
{
    if (!conn || !path || !out_ino || !out_mode) return -EINVAL;

    /* Root "/" is always Inode 1 */
    if (strcmp(path, "/") == 0) {
        *out_ino = PGFUSE_ROOT_INODE;
        const char *params[1] = {"1"};
        PGresult *res = PQexecParams(conn,
            "SELECT mode FROM pgfuse_inodes WHERE ino = $1::BIGINT;",
            1, NULL, params, NULL, NULL, 0);

        if (PQresultStatus(res) != PGRES_TUPLES_OK || PQntuples(res) == 0) {
            PQclear(res);
            return -EIO;
        }
        *out_mode = (mode_t)strtoul(PQgetvalue(res, 0, 0), NULL, 10);
        PQclear(res);
        return 0;
    }

    char path_buf[PGFUSE_MAX_PATH_LEN];
    strncpy(path_buf, path, sizeof(path_buf) - 1);
    path_buf[sizeof(path_buf) - 1] = '\0';

    uint64_t curr_ino = PGFUSE_ROOT_INODE;
    mode_t   curr_mode = S_IFDIR | 0755;

    /* Split path by slashes safely using thread-safe strtok_r */
    char *saveptr = NULL;
    char *token = strtok_r(path_buf, "/", &saveptr);

    while (token != NULL) {
        /* Every intermediate step must be a folder */
        if (!S_ISDIR(curr_mode)) return -ENOTDIR;

        char parent_str[32];
        snprintf(parent_str, sizeof(parent_str), "%" PRIu64, curr_ino);
        const char *params[2] = {parent_str, token};

        /* Search dentries table: find child where parent = curr_ino and name = token */
        PGresult *res = PQexecParams(conn,
            "SELECT d.ino, i.mode FROM pgfuse_dentries d "
            "JOIN pgfuse_inodes i ON d.ino = i.ino "
            "WHERE d.parent_ino = $1::BIGINT AND d.name = $2;",
            2, NULL, params, NULL, NULL, 0);

        if (PQresultStatus(res) != PGRES_TUPLES_OK) {
            PQclear(res);
            return -EIO;
        }

        if (PQntuples(res) == 0) {
            PQclear(res);
            return -ENOENT; /* File or folder not found */
        }

        /* Move to the next child inode */
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
 * Core FUSE Operations (Handling Linux System Calls)
 * ============================================================================
 */

/*
 * getattr: Called when Linux runs 'ls -l' or 'stat file.txt'.
 * Queries PostgreSQL for file size, permissions, and timestamps.
 */
#if FUSE_USE_VERSION >= 30
static int pgfuse_vfs_getattr(const char *path, struct stat *stbuf, struct fuse_file_info *fi)
#else
static int pgfuse_vfs_getattr(const char *path, struct stat *stbuf)
#endif
{
    (void)fi;
    if (!path || !stbuf) return -EINVAL;
    memset(stbuf, 0, sizeof(struct stat));

    PGconn *conn = pgfuse_pool_acquire(&g_pgfuse.pool);
    uint64_t ino = 0;
    mode_t mode = 0;

    /* 1. Resolve path to its Inode number */
    int ret = pgfuse_resolve_path(conn, path, &ino, &mode);
    if (ret != 0) {
        pgfuse_pool_release(&g_pgfuse.pool, conn);
        return ret;
    }

    char ino_str[32];
    snprintf(ino_str, sizeof(ino_str), "%" PRIu64, ino);
    const char *params[1] = {ino_str};

    /* 2. Fetch metadata from the inodes table */
    PGresult *res = PQexecParams(conn,
        "SELECT ino, mode, nlink, uid, gid, size, "
        "EXTRACT(EPOCH FROM atime)::BIGINT, "
        "EXTRACT(EPOCH FROM mtime)::BIGINT, "
        "EXTRACT(EPOCH FROM ctime)::BIGINT "
        "FROM pgfuse_inodes WHERE ino = $1::BIGINT;",
        1, NULL, params, NULL, NULL, 0);

    if (PQresultStatus(res) != PGRES_TUPLES_OK || PQntuples(res) == 0) {
        PQclear(res);
        pgfuse_pool_release(&g_pgfuse.pool, conn);
        return -EIO;
    }

    /* 3. Fill the Linux struct stat with the values */
    stbuf->st_ino   = (ino_t)strtoull(PQgetvalue(res, 0, 0), NULL, 10);
    stbuf->st_mode  = (mode_t)strtoul(PQgetvalue(res, 0, 1), NULL, 10);
    stbuf->st_nlink = (nlink_t)strtoul(PQgetvalue(res, 0, 2), NULL, 10);
    stbuf->st_uid   = (uid_t)strtoul(PQgetvalue(res, 0, 3), NULL, 10);
    stbuf->st_gid   = (gid_t)strtoul(PQgetvalue(res, 0, 4), NULL, 10);
    stbuf->st_size  = (off_t)strtoll(PQgetvalue(res, 0, 5), NULL, 10);
    stbuf->st_atime = (time_t)strtoll(PQgetvalue(res, 0, 6), NULL, 10);
    stbuf->st_mtime = (time_t)strtoll(PQgetvalue(res, 0, 7), NULL, 10);
    stbuf->st_ctime = (time_t)strtoll(PQgetvalue(res, 0, 8), NULL, 10);

    stbuf->st_blksize = PGFUSE_CHUNK_SIZE;
    stbuf->st_blocks  = (stbuf->st_size + 511) / 512;

    PQclear(res);
    pgfuse_pool_release(&g_pgfuse.pool, conn);
    return 0;
}

/*
 * readdir: Called when Linux runs 'ls' to list folder contents.
 * Returns "." (self), ".." (parent), and all child names from the dentries table.
 */
#if FUSE_USE_VERSION >= 30
static int pgfuse_vfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                              off_t offset, struct fuse_file_info *fi,
                              enum fuse_readdir_flags flags)
#else
static int pgfuse_vfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                              off_t offset, struct fuse_file_info *fi)
#endif
{
    (void)offset;
    (void)fi;
#if FUSE_USE_VERSION >= 30
    (void)flags;
#endif

    PGconn *conn = pgfuse_pool_acquire(&g_pgfuse.pool);
    uint64_t dir_ino = 0;
    mode_t dir_mode = 0;

    int ret = pgfuse_resolve_path(conn, path, &dir_ino, &dir_mode);
    if (ret != 0) {
        pgfuse_pool_release(&g_pgfuse.pool, conn);
        return ret;
    }

    if (!S_ISDIR(dir_mode)) {
        pgfuse_pool_release(&g_pgfuse.pool, conn);
        return -ENOTDIR;
    }

    /* Add standard "." (self) and ".." (parent) */
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

    /* Query all child entries from dentries table */
    PGresult *res = PQexecParams(conn,
        "SELECT d.name, d.ino, i.mode FROM pgfuse_dentries d "
        "JOIN pgfuse_inodes i ON d.ino = i.ino "
        "WHERE d.parent_ino = $1::BIGINT ORDER BY d.name;",
        1, NULL, params, NULL, NULL, 0);

    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        PQclear(res);
        pgfuse_pool_release(&g_pgfuse.pool, conn);
        return -EIO;
    }

    /* Feed each file/folder name into FUSE's buffer */
    int count = PQntuples(res);
    for (int i = 0; i < count; i++) {
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
    pgfuse_pool_release(&g_pgfuse.pool, conn);
    return 0;
}

/*
 * open: Called when an application opens a file.
 * Optimization: Caches the Inode ID into fi->fh so read/write skip path lookups!
 */
static int pgfuse_vfs_open(const char *path, struct fuse_file_info *fi)
{
    PGconn *conn = pgfuse_pool_acquire(&g_pgfuse.pool);
    uint64_t ino = 0;
    mode_t mode = 0;

    int ret = pgfuse_resolve_path(conn, path, &ino, &mode);
    if (ret != 0) {
        pgfuse_pool_release(&g_pgfuse.pool, conn);
        return ret;
    }

    if (S_ISDIR(mode)) {
        pgfuse_pool_release(&g_pgfuse.pool, conn);
        return -EISDIR; /* Cannot open a folder as a regular file */
    }

    /* Cache Inode ID in file handle (fi->fh) for fast future I/O */
    if (fi) {
        fi->fh = ino;
    }

    pgfuse_pool_release(&g_pgfuse.pool, conn);
    return 0;
}

/*
 * create: Called on 'touch file.txt' or open(..., O_CREAT).
 * Atomically inserts a new Inode and a new Dentry inside a transaction.
 */
static int pgfuse_vfs_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
    char parent_dir[PGFUSE_MAX_PATH_LEN];
    char base_name[PGFUSE_MAX_NAME_LEN];

    int ret = pgfuse_split_path(path, parent_dir, base_name);
    if (ret != 0) return ret;

    PGconn *conn = pgfuse_pool_acquire(&g_pgfuse.pool);

    uint64_t parent_ino = 0;
    mode_t parent_mode = 0;
    ret = pgfuse_resolve_path(conn, parent_dir, &parent_ino, &parent_mode);
    if (ret != 0) {
        pgfuse_pool_release(&g_pgfuse.pool, conn);
        return ret;
    }

    if (!S_ISDIR(parent_mode)) {
        pgfuse_pool_release(&g_pgfuse.pool, conn);
        return -ENOTDIR;
    }

    /* Begin atomic SQL transaction */
    PGresult *res = PQexec(conn, "BEGIN;");
    PQclear(res);

    /* 1. Insert new Inode */
    mode_t final_mode = S_IFREG | (mode & 07777);
    char mode_str[32], uid_str[32], gid_str[32];
    snprintf(mode_str, sizeof(mode_str), "%u", (unsigned int)final_mode);
    snprintf(uid_str, sizeof(uid_str), "%u", (unsigned int)fuse_get_context()->uid);
    snprintf(gid_str, sizeof(gid_str), "%u", (unsigned int)fuse_get_context()->gid);

    const char *inode_params[3] = {mode_str, uid_str, gid_str};
    res = PQexecParams(conn,
        "INSERT INTO pgfuse_inodes (mode, nlink, uid, gid, size) "
        "VALUES ($1::INT, 1, $2::INT, $3::INT, 0) RETURNING ino;",
        3, NULL, inode_params, NULL, NULL, 0);

    if (PQresultStatus(res) != PGRES_TUPLES_OK || PQntuples(res) == 0) {
        PQclear(res);
        res = PQexec(conn, "ROLLBACK;");
        PQclear(res);
        pgfuse_pool_release(&g_pgfuse.pool, conn);
        return -EIO;
    }

    uint64_t new_ino = (uint64_t)strtoull(PQgetvalue(res, 0, 0), NULL, 10);
    PQclear(res);

    /* 2. Insert dentry into the parent folder */
    char parent_str[32], new_ino_str[32];
    snprintf(parent_str, sizeof(parent_str), "%" PRIu64, parent_ino);
    snprintf(new_ino_str, sizeof(new_ino_str), "%" PRIu64, new_ino);

    const char *dentry_params[3] = {parent_str, base_name, new_ino_str};
    res = PQexecParams(conn,
        "INSERT INTO pgfuse_dentries (parent_ino, name, ino) "
        "VALUES ($1::BIGINT, $2, $3::BIGINT);",
        3, NULL, dentry_params, NULL, NULL, 0);

    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        /* File already exists! Rollback */
        PQclear(res);
        res = PQexec(conn, "ROLLBACK;");
        PQclear(res);
        pgfuse_pool_release(&g_pgfuse.pool, conn);
        return -EEXIST;
    }
    PQclear(res);

    /* 3. Update parent folder modification time */
    const char *touch_params[1] = {parent_str};
    res = PQexecParams(conn,
        "UPDATE pgfuse_inodes SET mtime = CURRENT_TIMESTAMP, ctime = CURRENT_TIMESTAMP "
        "WHERE ino = $1::BIGINT;",
        1, NULL, touch_params, NULL, NULL, 0);
    PQclear(res);

    res = PQexec(conn, "COMMIT;");
    PQclear(res);

    if (fi) {
        fi->fh = new_ino; /* Cache file handle */
    }

    pgfuse_pool_release(&g_pgfuse.pool, conn);
    return 0;
}

/*
 * read: Slices data from 64KB database chunks into user buffer.
 * Uses resultFormat = 1 for zero-copy raw binary transfer (no hex strings).
 */
static int pgfuse_vfs_read(const char *path, char *buf, size_t size, off_t offset,
                           struct fuse_file_info *fi)
{
    if (!buf) return -EINVAL;
    if (size == 0) return 0;

    PGconn *conn = pgfuse_pool_acquire(&g_pgfuse.pool);
    uint64_t ino = (fi && fi->fh) ? fi->fh : 0;

    if (ino == 0) {
        mode_t mode = 0;
        int ret = pgfuse_resolve_path(conn, path, &ino, &mode);
        if (ret != 0) {
            pgfuse_pool_release(&g_pgfuse.pool, conn);
            return ret;
        }
    }

    char ino_str[32];
    snprintf(ino_str, sizeof(ino_str), "%" PRIu64, ino);
    const char *size_params[1] = {ino_str};

    /* Get file size */
    PGresult *res = PQexecParams(conn,
        "SELECT size FROM pgfuse_inodes WHERE ino = $1::BIGINT;",
        1, NULL, size_params, NULL, NULL, 0);

    if (PQresultStatus(res) != PGRES_TUPLES_OK || PQntuples(res) == 0) {
        PQclear(res);
        pgfuse_pool_release(&g_pgfuse.pool, conn);
        return -EIO;
    }

    off_t file_size = (off_t)strtoll(PQgetvalue(res, 0, 0), NULL, 10);
    PQclear(res);

    /* If offset is past EOF, return 0 bytes */
    if (offset >= file_size) {
        pgfuse_pool_release(&g_pgfuse.pool, conn);
        return 0;
    }

    if (offset + (off_t)size > file_size) {
        size = (size_t)(file_size - offset);
    }

    /* Zero out buffer: empty chunks/sparse holes automatically read as zeroes */
    memset(buf, 0, size);

    /* Calculate which 64KB chunks we need */
    uint64_t first_chunk = (uint64_t)(offset / PGFUSE_CHUNK_SIZE);
    uint64_t last_chunk  = (uint64_t)((offset + size - 1) / PGFUSE_CHUNK_SIZE);
    size_t   bytes_read  = 0;

    for (uint64_t c = first_chunk; c <= last_chunk; c++) {
        char chunk_str[32];
        snprintf(chunk_str, sizeof(chunk_str), "%" PRIu64, c);
        const char *chunk_params[2] = {ino_str, chunk_str};

        /* resultFormat = 1 asks PostgreSQL to return RAW BINARY bytes! */
        res = PQexecParams(conn,
            "SELECT chunk_size, chunk_data FROM pgfuse_chunks "
            "WHERE ino = $1::BIGINT AND chunk_index = $2::BIGINT;",
            2, NULL, chunk_params, NULL, NULL, 1);

        off_t chunk_base = (off_t)(c * PGFUSE_CHUNK_SIZE);
        off_t start_in_chunk = (offset > chunk_base) ? (offset - chunk_base) : 0;
        off_t end_in_chunk = ((offset + (off_t)size) < (chunk_base + PGFUSE_CHUNK_SIZE))
                             ? (offset + (off_t)size - chunk_base)
                             : PGFUSE_CHUNK_SIZE;
        size_t chunk_span = (size_t)(end_in_chunk - start_in_chunk);
        size_t buf_pos = (size_t)((chunk_base + start_in_chunk) - offset);

        if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0) {
            const char *raw_data = PQgetvalue(res, 0, 1);
            int data_len = PQgetlength(res, 0, 1);

            if ((int)start_in_chunk < data_len) {
                size_t available = (size_t)(data_len - start_in_chunk);
                size_t to_copy = (chunk_span < available) ? chunk_span : available;
                /* Zero-copy slice directly into user buffer */
                memcpy(buf + buf_pos, raw_data + start_in_chunk, to_copy);
            }
        }

        PQclear(res);
        bytes_read += chunk_span;
    }

    pgfuse_pool_release(&g_pgfuse.pool, conn);
    return (int)size;
}

/*
 * write: Chunk-Aligned Read-Modify-Write Engine.
 * Modifying 10 bytes in a 1GB file reads and rewrites ONLY a 64KB chunk.
 */
static int pgfuse_vfs_write(const char *path, const char *buf, size_t size, off_t offset,
                            struct fuse_file_info *fi)
{
    if (!buf) return -EINVAL;
    if (size == 0) return 0;

    PGconn *conn = pgfuse_pool_acquire(&g_pgfuse.pool);
    uint64_t ino = (fi && fi->fh) ? fi->fh : 0;

    if (ino == 0) {
        mode_t mode = 0;
        int ret = pgfuse_resolve_path(conn, path, &ino, &mode);
        if (ret != 0) {
            pgfuse_pool_release(&g_pgfuse.pool, conn);
            return ret;
        }
    }

    char ino_str[32];
    snprintf(ino_str, sizeof(ino_str), "%" PRIu64, ino);

    /* Get current file size */
    const char *size_params[1] = {ino_str};
    PGresult *res = PQexecParams(conn,
        "SELECT size FROM pgfuse_inodes WHERE ino = $1::BIGINT;",
        1, NULL, size_params, NULL, NULL, 0);

    if (PQresultStatus(res) != PGRES_TUPLES_OK || PQntuples(res) == 0) {
        PQclear(res);
        pgfuse_pool_release(&g_pgfuse.pool, conn);
        return -EIO;
    }

    off_t current_size = (off_t)strtoll(PQgetvalue(res, 0, 0), NULL, 10);
    PQclear(res);

    uint64_t first_chunk = (uint64_t)(offset / PGFUSE_CHUNK_SIZE);
    uint64_t last_chunk  = (uint64_t)((offset + size - 1) / PGFUSE_CHUNK_SIZE);
    size_t   bytes_written = 0;

    /* Temporary 64KB staging buffer allocated on heap */
    char *chunk_staging = (char *)malloc(PGFUSE_CHUNK_SIZE);
    if (!chunk_staging) {
        pgfuse_pool_release(&g_pgfuse.pool, conn);
        return -ENOMEM;
    }

    for (uint64_t c = first_chunk; c <= last_chunk; c++) {
        off_t chunk_base = (off_t)(c * PGFUSE_CHUNK_SIZE);
        off_t chunk_offset = (offset > chunk_base) ? (offset - chunk_base) : 0;
        size_t to_write = size - bytes_written;
        size_t max_chunk_space = (size_t)(PGFUSE_CHUNK_SIZE - chunk_offset);
        size_t write_len = (to_write < max_chunk_space) ? to_write : max_chunk_space;

        char chunk_str[32];
        snprintf(chunk_str, sizeof(chunk_str), "%" PRIu64, c);
        int final_chunk_len = 0;

        /* CASE 1: Unaligned write -> READ-MODIFY-WRITE */
        if (chunk_offset > 0 || write_len < PGFUSE_CHUNK_SIZE) {
            memset(chunk_staging, 0, PGFUSE_CHUNK_SIZE);

            /* 1. Read existing chunk from database */
            const char *fetch_params[2] = {ino_str, chunk_str};
            res = PQexecParams(conn,
                "SELECT chunk_size, chunk_data FROM pgfuse_chunks "
                "WHERE ino = $1::BIGINT AND chunk_index = $2::BIGINT;",
                2, NULL, fetch_params, NULL, NULL, 1);

            int existing_len = 0;
            if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0) {
                const char *raw_data = PQgetvalue(res, 0, 1);
                existing_len = PQgetlength(res, 0, 1);
                if (existing_len > PGFUSE_CHUNK_SIZE) existing_len = PGFUSE_CHUNK_SIZE;
                memcpy(chunk_staging, raw_data, existing_len);
            }
            PQclear(res);

            /* 2. Modify: overlay new bytes */
            memcpy(chunk_staging + chunk_offset, buf + bytes_written, write_len);
            int span_end = (int)(chunk_offset + write_len);
            final_chunk_len = (span_end > existing_len) ? span_end : existing_len;
        } 
        /* CASE 2: Full 64KB write -> FAST PATH (skip reading) */
        else {
            memcpy(chunk_staging, buf + bytes_written, PGFUSE_CHUNK_SIZE);
            final_chunk_len = PGFUSE_CHUNK_SIZE;
        }

        /* 3. Write: upsert chunk into PostgreSQL */
        char chunk_len_str[32];
        snprintf(chunk_len_str, sizeof(chunk_len_str), "%d", final_chunk_len);

        const char *upsert_values[4] = {ino_str, chunk_str, chunk_len_str, chunk_staging};
        int upsert_lengths[4] = {
            (int)strlen(ino_str),
            (int)strlen(chunk_str),
            (int)strlen(chunk_len_str),
            final_chunk_len
        };
        /* paramFormats[3] = 1 tells PostgreSQL: parameter 4 is RAW BINARY! */
        int upsert_formats[4] = {0, 0, 0, 1};

        res = PQexecParams(conn,
            "INSERT INTO pgfuse_chunks (ino, chunk_index, chunk_size, chunk_data) "
            "VALUES ($1::BIGINT, $2::BIGINT, $3::INT, $4::BYTEA) "
            "ON CONFLICT (ino, chunk_index) DO UPDATE SET "
            "chunk_size = EXCLUDED.chunk_size, "
            "chunk_data = EXCLUDED.chunk_data;",
            4, NULL, upsert_values, upsert_lengths, upsert_formats, 0);

        if (PQresultStatus(res) != PGRES_COMMAND_OK) {
            fprintf(stderr, "[PGFUSE-ERROR] Upsert failed on ino %" PRIu64 " chunk %" PRIu64 ": %s\n",
                    ino, c, PQerrorMessage(conn));
            PQclear(res);
            free(chunk_staging);
            pgfuse_pool_release(&g_pgfuse.pool, conn);
            return -EIO;
        }
        PQclear(res);

        bytes_written += write_len;
    }

    free(chunk_staging);

    /* Update file size in inodes table if write expanded the file */
    off_t new_size = (offset + (off_t)size > current_size) ? (offset + (off_t)size) : current_size;
    char new_size_str[32];
    snprintf(new_size_str, sizeof(new_size_str), "%lld", (long long)new_size);

    const char *update_params[2] = {new_size_str, ino_str};
    res = PQexecParams(conn,
        "UPDATE pgfuse_inodes SET size = $1::BIGINT, mtime = CURRENT_TIMESTAMP, ctime = CURRENT_TIMESTAMP "
        "WHERE ino = $2::BIGINT;",
        2, NULL, update_params, NULL, NULL, 0);
    PQclear(res);

    pgfuse_pool_release(&g_pgfuse.pool, conn);
    return (int)size;
}

/*
 * mkdir: Called on 'mkdir foldername'.
 * Creates a new directory Inode (mode S_IFDIR) and links a dentry.
 */
static int pgfuse_vfs_mkdir(const char *path, mode_t mode)
{
    char parent_dir[PGFUSE_MAX_PATH_LEN];
    char base_name[PGFUSE_MAX_NAME_LEN];

    int ret = pgfuse_split_path(path, parent_dir, base_name);
    if (ret != 0) return ret;

    PGconn *conn = pgfuse_pool_acquire(&g_pgfuse.pool);

    uint64_t parent_ino = 0;
    mode_t parent_mode = 0;
    ret = pgfuse_resolve_path(conn, parent_dir, &parent_ino, &parent_mode);
    if (ret != 0) {
        pgfuse_pool_release(&g_pgfuse.pool, conn);
        return ret;
    }

    if (!S_ISDIR(parent_mode)) {
        pgfuse_pool_release(&g_pgfuse.pool, conn);
        return -ENOTDIR;
    }

    PGresult *res = PQexec(conn, "BEGIN;");
    PQclear(res);

    mode_t final_mode = S_IFDIR | (mode & 07777);
    char mode_str[32], uid_str[32], gid_str[32];
    snprintf(mode_str, sizeof(mode_str), "%u", (unsigned int)final_mode);
    snprintf(uid_str, sizeof(uid_str), "%u", (unsigned int)fuse_get_context()->uid);
    snprintf(gid_str, sizeof(gid_str), "%u", (unsigned int)fuse_get_context()->gid);

    const char *inode_params[3] = {mode_str, uid_str, gid_str};
    res = PQexecParams(conn,
        "INSERT INTO pgfuse_inodes (mode, nlink, uid, gid, size) "
        "VALUES ($1::INT, 2, $2::INT, $3::INT, 4096) RETURNING ino;",
        3, NULL, inode_params, NULL, NULL, 0);

    if (PQresultStatus(res) != PGRES_TUPLES_OK || PQntuples(res) == 0) {
        PQclear(res);
        res = PQexec(conn, "ROLLBACK;");
        PQclear(res);
        pgfuse_pool_release(&g_pgfuse.pool, conn);
        return -EIO;
    }

    uint64_t new_ino = (uint64_t)strtoull(PQgetvalue(res, 0, 0), NULL, 10);
    PQclear(res);

    char parent_str[32], new_ino_str[32];
    snprintf(parent_str, sizeof(parent_str), "%" PRIu64, parent_ino);
    snprintf(new_ino_str, sizeof(new_ino_str), "%" PRIu64, new_ino);

    const char *dentry_params[3] = {parent_str, base_name, new_ino_str};
    res = PQexecParams(conn,
        "INSERT INTO pgfuse_dentries (parent_ino, name, ino) "
        "VALUES ($1::BIGINT, $2, $3::BIGINT);",
        3, NULL, dentry_params, NULL, NULL, 0);

    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        PQclear(res);
        res = PQexec(conn, "ROLLBACK;");
        PQclear(res);
        pgfuse_pool_release(&g_pgfuse.pool, conn);
        return -EEXIST;
    }
    PQclear(res);

    /* Increment parent link count */
    const char *parent_update[1] = {parent_str};
    res = PQexecParams(conn,
        "UPDATE pgfuse_inodes "
        "SET nlink = nlink + 1, mtime = CURRENT_TIMESTAMP, ctime = CURRENT_TIMESTAMP "
        "WHERE ino = $1::BIGINT;",
        1, NULL, parent_update, NULL, NULL, 0);
    PQclear(res);

    res = PQexec(conn, "COMMIT;");
    PQclear(res);

    pgfuse_pool_release(&g_pgfuse.pool, conn);
    return 0;
}

/*
 * unlink: Called on 'rm file.txt'.
 * Decrements nlink. Only deletes the chunks and Inode if nlink reaches 0!
 */
static int pgfuse_vfs_unlink(const char *path)
{
    char parent_dir[PGFUSE_MAX_PATH_LEN];
    char base_name[PGFUSE_MAX_NAME_LEN];

    int ret = pgfuse_split_path(path, parent_dir, base_name);
    if (ret != 0) return ret;

    PGconn *conn = pgfuse_pool_acquire(&g_pgfuse.pool);

    uint64_t parent_ino = 0, target_ino = 0;
    mode_t parent_mode = 0, target_mode = 0;

    pgfuse_resolve_path(conn, parent_dir, &parent_ino, &parent_mode);
    pgfuse_resolve_path(conn, path, &target_ino, &target_mode);

    if (S_ISDIR(target_mode)) {
        pgfuse_pool_release(&g_pgfuse.pool, conn);
        return -EISDIR;
    }

    PGresult *res = PQexec(conn, "BEGIN;");
    PQclear(res);

    char parent_str[32], target_str[32];
    snprintf(parent_str, sizeof(parent_str), "%" PRIu64, parent_ino);
    snprintf(target_str, sizeof(target_str), "%" PRIu64, target_ino);

    /* 1. Remove name from directory */
    const char *del_dentry_params[2] = {parent_str, base_name};
    res = PQexecParams(conn,
        "DELETE FROM pgfuse_dentries WHERE parent_ino = $1::BIGINT AND name = $2;",
        2, NULL, del_dentry_params, NULL, NULL, 0);
    PQclear(res);

    /* 2. Decrement nlink count */
    const char *dec_params[1] = {target_str};
    res = PQexecParams(conn,
        "UPDATE pgfuse_inodes SET nlink = nlink - 1, ctime = CURRENT_TIMESTAMP "
        "WHERE ino = $1::BIGINT RETURNING nlink;",
        1, NULL, dec_params, NULL, NULL, 0);

    if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0) {
        int remaining_links = atoi(PQgetvalue(res, 0, 0));
        PQclear(res);

        /* 3. If no links left, delete chunks and inode */
        if (remaining_links <= 0) {
            res = PQexecParams(conn,
                "DELETE FROM pgfuse_chunks WHERE ino = $1::BIGINT;",
                1, NULL, dec_params, NULL, NULL, 0);
            PQclear(res);

            res = PQexecParams(conn,
                "DELETE FROM pgfuse_inodes WHERE ino = $1::BIGINT;",
                1, NULL, dec_params, NULL, NULL, 0);
            PQclear(res);
        }
    } else {
        PQclear(res);
    }

    const char *touch_params[1] = {parent_str};
    res = PQexecParams(conn,
        "UPDATE pgfuse_inodes SET mtime = CURRENT_TIMESTAMP, ctime = CURRENT_TIMESTAMP "
        "WHERE ino = $1::BIGINT;",
        1, NULL, touch_params, NULL, NULL, 0);
    PQclear(res);

    res = PQexec(conn, "COMMIT;");
    PQclear(res);

    pgfuse_pool_release(&g_pgfuse.pool, conn);
    return 0;
}

/*
 * rmdir: Called on 'rmdir foldername'.
 * Fails if folder is not empty (POSIX standard).
 */
static int pgfuse_vfs_rmdir(const char *path)
{
    char parent_dir[PGFUSE_MAX_PATH_LEN];
    char base_name[PGFUSE_MAX_NAME_LEN];

    int ret = pgfuse_split_path(path, parent_dir, base_name);
    if (ret != 0) return ret;

    PGconn *conn = pgfuse_pool_acquire(&g_pgfuse.pool);

    uint64_t parent_ino = 0, target_ino = 0;
    mode_t parent_mode = 0, target_mode = 0;

    pgfuse_resolve_path(conn, parent_dir, &parent_ino, &parent_mode);
    pgfuse_resolve_path(conn, path, &target_ino, &target_mode);

    if (!S_ISDIR(target_mode)) {
        pgfuse_pool_release(&g_pgfuse.pool, conn);
        return -ENOTDIR;
    }

    if (target_ino == PGFUSE_ROOT_INODE) {
        pgfuse_pool_release(&g_pgfuse.pool, conn);
        return -EBUSY; /* Cannot delete root */
    }

    char target_str[32], parent_str[32];
    snprintf(target_str, sizeof(target_str), "%" PRIu64, target_ino);
    snprintf(parent_str, sizeof(parent_str), "%" PRIu64, parent_ino);

    /* Verify directory is empty */
    const char *check_params[1] = {target_str};
    PGresult *res = PQexecParams(conn,
        "SELECT COUNT(*) FROM pgfuse_dentries WHERE parent_ino = $1::BIGINT;",
        1, NULL, check_params, NULL, NULL, 0);

    int entry_count = atoi(PQgetvalue(res, 0, 0));
    PQclear(res);

    if (entry_count > 0) {
        pgfuse_pool_release(&g_pgfuse.pool, conn);
        return -ENOTEMPTY;
    }

    res = PQexec(conn, "BEGIN;");
    PQclear(res);

    const char *del_dentry_params[2] = {parent_str, base_name};
    res = PQexecParams(conn,
        "DELETE FROM pgfuse_dentries WHERE parent_ino = $1::BIGINT AND name = $2;",
        2, NULL, del_dentry_params, NULL, NULL, 0);
    PQclear(res);

    const char *del_inode_params[1] = {target_str};
    res = PQexecParams(conn,
        "DELETE FROM pgfuse_inodes WHERE ino = $1::BIGINT;",
        1, NULL, del_inode_params, NULL, NULL, 0);
    PQclear(res);

    const char *parent_update[1] = {parent_str};
    res = PQexecParams(conn,
        "UPDATE pgfuse_inodes "
        "SET nlink = nlink - 1, mtime = CURRENT_TIMESTAMP, ctime = CURRENT_TIMESTAMP "
        "WHERE ino = $1::BIGINT;",
        1, NULL, parent_update, NULL, NULL, 0);
    PQclear(res);

    res = PQexec(conn, "COMMIT;");
    PQclear(res);

    pgfuse_pool_release(&g_pgfuse.pool, conn);
    return 0;
}

/*
 * truncate: Resizes a file (e.g. 'truncate -s 0 file.txt').
 * Drops discarded chunks past the new size boundary.
 */
#if FUSE_USE_VERSION >= 30
static int pgfuse_vfs_truncate(const char *path, off_t size, struct fuse_file_info *fi)
#else
static int pgfuse_vfs_truncate(const char *path, off_t size)
#endif
{
#if FUSE_USE_VERSION >= 30
    (void)fi;
#endif
    if (size < 0) return -EINVAL;

    PGconn *conn = pgfuse_pool_acquire(&g_pgfuse.pool);
    uint64_t ino = 0;
    mode_t mode = 0;

    int ret = pgfuse_resolve_path(conn, path, &ino, &mode);
    if (ret != 0) {
        pgfuse_pool_release(&g_pgfuse.pool, conn);
        return ret;
    }

    if (S_ISDIR(mode)) {
        pgfuse_pool_release(&g_pgfuse.pool, conn);
        return -EISDIR;
    }

    char ino_str[32];
    snprintf(ino_str, sizeof(ino_str), "%" PRIu64, ino);

    PGresult *res = PQexec(conn, "BEGIN;");
    PQclear(res);

    if (size == 0) {
        /* Truncate to 0: delete all chunks */
        const char *del_params[1] = {ino_str};
        res = PQexecParams(conn,
            "DELETE FROM pgfuse_chunks WHERE ino = $1::BIGINT;",
            1, NULL, del_params, NULL, NULL, 0);
        PQclear(res);
    } else {
        uint64_t last_valid_chunk = (uint64_t)((size - 1) / PGFUSE_CHUNK_SIZE);
        char chunk_str[32];
        snprintf(chunk_str, sizeof(chunk_str), "%" PRIu64, last_valid_chunk);

        /* Delete chunks past the new size */
        const char *purge_params[2] = {ino_str, chunk_str};
        res = PQexecParams(conn,
            "DELETE FROM pgfuse_chunks WHERE ino = $1::BIGINT AND chunk_index > $2::BIGINT;",
            2, NULL, purge_params, NULL, NULL, 0);
        PQclear(res);
    }

    /* Update file size in inodes table */
    char size_str[32];
    snprintf(size_str, sizeof(size_str), "%lld", (long long)size);
    const char *size_params[2] = {size_str, ino_str};
    res = PQexecParams(conn,
        "UPDATE pgfuse_inodes SET size = $1::BIGINT, mtime = CURRENT_TIMESTAMP, ctime = CURRENT_TIMESTAMP "
        "WHERE ino = $2::BIGINT;",
        2, NULL, size_params, NULL, NULL, 0);
    PQclear(res);

    res = PQexec(conn, "COMMIT;");
    PQclear(res);

    pgfuse_pool_release(&g_pgfuse.pool, conn);
    return 0;
}

/*
 * statfs: Called on 'df -h' to report virtual filesystem space.
 */
static int pgfuse_vfs_statfs(const char *path, struct statvfs *stbuf)
{
    (void)path;
    if (!stbuf) return -EINVAL;
    memset(stbuf, 0, sizeof(struct statvfs));

    PGconn *conn = pgfuse_pool_acquire(&g_pgfuse.pool);

    PGresult *res = PQexec(conn,
        "SELECT block_size, total_blocks FROM pgfuse_superblock LIMIT 1;");

    uint64_t bsize = PGFUSE_CHUNK_SIZE;
    uint64_t total_blocks = 1048576; /* 64 GB */

    if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0) {
        bsize = (uint64_t)strtoull(PQgetvalue(res, 0, 0), NULL, 10);
        total_blocks = (uint64_t)strtoull(PQgetvalue(res, 0, 1), NULL, 10);
    }
    PQclear(res);

    /* Count allocated chunks */
    res = PQexec(conn, "SELECT COUNT(*) FROM pgfuse_chunks;");
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
    stbuf->f_namemax = PGFUSE_MAX_NAME_LEN;

    pgfuse_pool_release(&g_pgfuse.pool, conn);
    return 0;
}

/*
 * init: Called on mount to print banner and set cache timeouts.
 */
#if FUSE_USE_VERSION >= 30
static void *pgfuse_vfs_init(struct fuse_conn_info *conn, struct fuse_config *cfg)
{
    (void)conn;
    if (cfg) {
        cfg->kernel_cache = 0;
        cfg->attr_timeout = 1.0;
        cfg->entry_timeout = 1.0;
    }
#else
static void *pgfuse_vfs_init(struct fuse_conn_info *conn)
{
    (void)conn;
#endif
    printf("\n==========================================================\n");
    printf(" pgfuse: PostgreSQL-Backed Virtual File System\n");
    printf(" Engine:  %s | Extents: %u KB | Pool: %d Channels\n",
           PGFUSE_VERSION_STR, PGFUSE_CHUNK_SIZE / 1024, g_pgfuse.pool.pool_size);
    printf(" Status:  Active Mount - ACID POSIX VFS Translation Live\n");
    printf("==========================================================\n\n");

    return NULL;
}

/*
 * destroy: Clean shutdown on unmount.
 */
static void pgfuse_vfs_destroy(void *private_data)
{
    (void)private_data;
    printf("[PGFUSE-INFO] Unmounting filesystem. Draining connection pool...\n");
    pgfuse_pool_destroy(&g_pgfuse.pool);
    printf("[PGFUSE-INFO] Teardown complete. Clean exit.\n");
}

/* ============================================================================
 * FUSE Function Pointer Table
 * ============================================================================
 */
static struct fuse_operations pgfuse_vfs_ops = {
    .getattr    = pgfuse_vfs_getattr,
    .readdir    = pgfuse_vfs_readdir,
    .open       = pgfuse_vfs_open,
    .read       = pgfuse_vfs_read,
    .write      = pgfuse_vfs_write,
    .mkdir      = pgfuse_vfs_mkdir,
    .unlink     = pgfuse_vfs_unlink,
    .rmdir      = pgfuse_vfs_rmdir,
    .truncate   = pgfuse_vfs_truncate,
    .create     = pgfuse_vfs_create,
    .statfs     = pgfuse_vfs_statfs,
    .init       = pgfuse_vfs_init,
    .destroy    = pgfuse_vfs_destroy,
};

/* ============================================================================
 * Main Entry Point
 * ============================================================================
 */
int main(int argc, char *argv[])
{
    const char *conninfo = getenv("PGFUSE_CONNSTRING");
    if (!conninfo || strlen(conninfo) == 0) {
        conninfo = PGFUSE_DEFAULT_CONNINFO;
    }

    int pool_size = PGFUSE_DEFAULT_POOL_SIZE;
    const char *pool_env = getenv("PGFUSE_POOL_SIZE");
    if (pool_env && atoi(pool_env) > 0) {
        pool_size = atoi(pool_env);
    }

    g_pgfuse.chunk_size = PGFUSE_CHUNK_SIZE;
    g_pgfuse.mount_uid  = getuid();
    g_pgfuse.mount_gid  = getgid();

    fprintf(stdout, "[PGFUSE-INIT] Bootstrapping PostgreSQL connection pool (%d channels)...\n", pool_size);
    int ret = pgfuse_pool_init(&g_pgfuse.pool, conninfo, pool_size);
    if (ret != 0) {
        fprintf(stderr, "[PGFUSE-FATAL] Database connection pool failed: error %d\n", ret);
        return EXIT_FAILURE;
    }

    /* Hand off control to the FUSE event loop */
    return fuse_main(argc, argv, &pgfuse_vfs_ops, NULL);
}
