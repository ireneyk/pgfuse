/*
 * pgfuse: PostgreSQL-Backed Virtual File System
 *
 * This program translates standard Linux file operations (read, write, mkdir, open, unlink)
 * into PostgreSQL SQL queries using FUSE 3 (Filesystem in Userspace) and libpq.
 */

#include "pgfuse.h"

/*
 * g_pgfuse:
 * Global runtime state structure holding:
 * - The connection pool
 * - Default 64KB chunk size
 * - Current user/group IDs
 */
static pgfuse_runtime_t g_pgfuse;

/* ============================================================================
 * Thread-Safe PostgreSQL Connection Pool
 *
 * Why we need this:
 * 1. FUSE processes filesystem calls concurrently across multiple worker threads.
 * 2. PostgreSQL's C client library (libpq) is NOT thread-safe on a single connection.
 * 3. Sharing one connection across multiple threads causes memory corruption and crashes.
 *
 * Solution:
 * We create a pool of 8 separate PostgreSQL connections.
 * Worker threads borrow an idle connection, run their SQL query, and return it.
 * A POSIX mutex lock and condition variable synchronize borrowing and returning.
 * ============================================================================
 */

/*
 * pgfuse_pool_init:
 * Initializes the connection pool with 'size' active connections (default 8).
 *
 * Parameters:
 * - pool:     Pointer to our pgfuse_conn_pool_t struct.
 * - conninfo: PostgreSQL connection string (host, port, dbname, user, password).
 * - size:     Number of parallel connections to open (8).
 *
 * Return: 0 on success, or a negative Linux error code (-EINVAL, -ENOMEM, -ECONNREFUSED).
 */
int pgfuse_pool_init(pgfuse_conn_pool_t *pool, const char *conninfo, int size)
{
    if (!pool || !conninfo || size <= 0) return -EINVAL;

    memset(pool, 0, sizeof(*pool));
    strncpy(pool->conninfo, conninfo, sizeof(pool->conninfo) - 1);
    pool->pool_size = size;

    /*
     * pthread_mutex_init:
     * Initializes a mutual exclusion lock to ensure only one thread modifies
     * the connection pool array at any single moment.
     */
    if (pthread_mutex_init(&pool->lock, NULL) != 0) return -ENOMEM;

    /*
     * pthread_cond_init:
     * Initializes a condition variable (a "waiting room" bell).
     * If all 8 connections are busy, threads sleep on this condition variable
     * until a connection is returned.
     */
    if (pthread_cond_init(&pool->cond, NULL) != 0) {
        pthread_mutex_destroy(&pool->lock);
        return -ENOMEM;
    }

    /* Allocate memory for connection pointers and in-use tracking flags */
    pool->conns = (PGconn **)calloc(size, sizeof(PGconn *));
    pool->in_use = (bool *)calloc(size, sizeof(bool));
    if (!pool->conns || !pool->in_use) {
        free(pool->conns);
        free(pool->in_use);
        pthread_mutex_destroy(&pool->lock);
        pthread_cond_destroy(&pool->cond);
        return -ENOMEM;
    }

    /* Open each connection channel to PostgreSQL */
    for (int i = 0; i < size; i++) {
        /* PQconnectdb: Opens a new network connection to the database */
        pool->conns[i] = PQconnectdb(pool->conninfo);

        /* PQstatus: Checks if the connection succeeded (returns CONNECTION_OK) */
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
 * pgfuse_pool_destroy:
 * Cleanly shuts down the pool on unmount: closes all database connections
 * and frees allocated mutexes and memory.
 */
void pgfuse_pool_destroy(pgfuse_conn_pool_t *pool)
{
    if (!pool || !pool->conns) return;

    pthread_mutex_lock(&pool->lock);
    for (int i = 0; i < pool->pool_size; i++) {
        if (pool->conns[i]) {
            /* PQfinish: Closes connection and frees internal libpq memory */
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
 * pgfuse_pool_acquire:
 * Borrows an idle database connection from the pool.
 * If all connections are busy, it safely puts the calling thread to sleep
 * until another thread returns one.
 *
 * Self-healing feature:
 * If a borrowed connection was dropped by PostgreSQL, it automatically
 * reconnects using PQreset before returning it to the worker thread.
 */
PGconn *pgfuse_pool_acquire(pgfuse_conn_pool_t *pool)
{
    pthread_mutex_lock(&pool->lock); /* Acquire lock to safely inspect pool */
    while (1) {
        for (int i = 0; i < pool->pool_size; i++) {
            if (!pool->in_use[i]) {
                pool->in_use[i] = true; /* Mark connection as busy */
                PGconn *conn = pool->conns[i];
                pthread_mutex_unlock(&pool->lock); /* Release lock */

                /* Self-healing: if connection dropped, reconnect automatically */
                if (PQstatus(conn) != CONNECTION_OK) {
                    fprintf(stderr, "[PGFUSE-WARN] Reconnecting dropped channel [%d]...\n", i);
                    PQreset(conn);
                }
                return conn;
            }
        }
        /* All channels are busy: sleep until another thread calls pgfuse_pool_release */
        pthread_cond_wait(&pool->cond, &pool->lock);
    }
}

/*
 * pgfuse_pool_release:
 * Returns a borrowed connection back to the pool and rings the condition bell
 * (pthread_cond_signal) to wake up any thread waiting for an available connection.
 */
void pgfuse_pool_release(pgfuse_conn_pool_t *pool, PGconn *conn)
{
    if (!pool || !conn) return;

    pthread_mutex_lock(&pool->lock);
    for (int i = 0; i < pool->pool_size; i++) {
        if (pool->conns[i] == conn) {
            pool->in_use[i] = false; /* Mark connection as idle */
            /* pthread_cond_signal: Wakes up one thread waiting in pthread_cond_wait */
            pthread_cond_signal(&pool->cond);
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
 * pgfuse_split_path:
 * Takes a full file path string and splits it into:
 * 1. parent_dir: The directory containing the item (e.g. "/documents")
 * 2. base_name:  The file or folder's individual name (e.g. "report.txt")
 *
 * Examples:
 *   "/documents/report.txt" -> parent = "/documents", base = "report.txt"
 *   "/hello.txt"            -> parent = "/",          base = "hello.txt"
 *   "/"                     -> parent = "/",          base = ""
 */
int pgfuse_split_path(const char *path, char *parent_dir, char *base_name)
{
    if (!path || !parent_dir || !base_name) return -EINVAL;

    size_t len = strlen(path);
    if (len == 0 || path[0] != '/') return -EINVAL;

    /* Handle root folder */
    if (strcmp(path, "/") == 0) {
        strcpy(parent_dir, "/");
        base_name[0] = '\0';
        return 0;
    }

    /* strrchr: Finds the last occurrence of '/' in the path string */
    const char *last_slash = strrchr(path, '/');
    if (!last_slash) return -EINVAL;

    if (last_slash == path) {
        /* File is directly in root folder, e.g. "/file.txt" */
        strcpy(parent_dir, "/");
        strncpy(base_name, path + 1, PGFUSE_MAX_NAME_LEN - 1);
        base_name[PGFUSE_MAX_NAME_LEN - 1] = '\0';
    } else {
        /* File is inside a subfolder, e.g. "/a/b/file.txt" */
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
 * pgfuse_resolve_path:
 * Translates a path string like "/documents/report.txt" into its target Inode ID.
 *
 * How it works:
 * 1. Starts at the root directory (always Inode 1).
 * 2. Uses strtok_r (thread-safe string tokenizer) to split the path by '/'.
 * 3. Walks step-by-step down the directory tree, querying pgfuse_dentries:
 *    SELECT ino FROM pgfuse_dentries WHERE parent_ino = ... AND name = ...
 * 4. Outputs out_ino (the file's Inode number) and out_mode (permissions & type).
 */
int pgfuse_resolve_path(PGconn *conn, const char *path, uint64_t *out_ino, mode_t *out_mode)
{
    if (!conn || !path || !out_ino || !out_mode) return -EINVAL;

    /* Root "/" is permanently fixed as Inode 1 */
    if (strcmp(path, "/") == 0) {
        *out_ino = PGFUSE_ROOT_INODE;
        const char *params[1] = {"1"};
        /* Query root inode permissions */
        PGresult *res = PQexecParams(conn,
            "SELECT mode FROM pgfuse_inodes WHERE ino = $1::BIGINT;",
            1, NULL, params, NULL, NULL, 0);

        /* PQresultStatus == PGRES_TUPLES_OK: Confirms the SELECT query returned rows */
        if (PQresultStatus(res) != PGRES_TUPLES_OK || PQntuples(res) == 0) {
            PQclear(res);
            return -EIO;
        }
        /* strtoul: Converts string mode ("16877") to unsigned long integer */
        *out_mode = (mode_t)strtoul(PQgetvalue(res, 0, 0), NULL, 10);
        PQclear(res);
        return 0;
    }

    char path_buf[PGFUSE_MAX_PATH_LEN];
    strncpy(path_buf, path, sizeof(path_buf) - 1);
    path_buf[sizeof(path_buf) - 1] = '\0';

    uint64_t curr_ino = PGFUSE_ROOT_INODE;
    mode_t   curr_mode = S_IFDIR | 0755;

    /*
     * strtok_r: Thread-safe string tokenizer.
     * Splits "/documents/notes/todo.txt" into "documents", then "notes", then "todo.txt".
     */
    char *saveptr = NULL;
    char *token = strtok_r(path_buf, "/", &saveptr);

    while (token != NULL) {
        /* S_ISDIR: Linux macro checking if the current inode is a directory */
        if (!S_ISDIR(curr_mode)) return -ENOTDIR;

        char parent_str[32];
        /* PRIu64: Standard portable format specifier for 64-bit unsigned integers */
        snprintf(parent_str, sizeof(parent_str), "%" PRIu64, curr_ino);
        const char *params[2] = {parent_str, token};

        /* Look up the child entry inside the dentries table */
        PGresult *res = PQexecParams(conn,
            "SELECT d.ino, i.mode FROM pgfuse_dentries d "
            "JOIN pgfuse_inodes i ON d.ino = i.ino "
            "WHERE d.parent_ino = $1::BIGINT AND d.name = $2;",
            2, NULL, params, NULL, NULL, 0);

        if (PQresultStatus(res) != PGRES_TUPLES_OK) {
            PQclear(res);
            return -EIO;
        }

        /* PQntuples: Returns number of rows. 0 rows means file/folder does not exist */
        if (PQntuples(res) == 0) {
            PQclear(res);
            return -ENOENT; /* -ENOENT = Error: No such file or directory */
        }

        /* Move to the child Inode and continue down the path */
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
 * Core FUSE Operations (Translating Linux VFS Calls into SQL)
 * ============================================================================
 */

/*
 * pgfuse_vfs_getattr:
 * "Get Attributes" — Called whenever Linux runs 'ls -l', 'stat', or checks file info.
 *
 * Parameters:
 * - path:  Path to the file or folder being inspected.
 * - stbuf: Pointer to Linux 'struct stat' where we fill in size, mode, timestamps, etc.
 * - fi:    Optional file info handle (provided by FUSE 3).
 *
 * Return: 0 on success, or negative error code.
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

    /*
     * 2. Fetch file metadata from pgfuse_inodes:
     * EXTRACT(EPOCH FROM ...): Converts timestamp to Unix epoch seconds (time_t).
     */
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

    /*
     * 3. Populate Linux 'struct stat':
     * - st_ino:     Inode ID
     * - st_mode:    File type + permissions (e.g. 0100644 for regular file)
     * - st_nlink:   Hard link count
     * - st_uid:     Owner User ID
     * - st_gid:     Owner Group ID
     * - st_size:    File size in bytes
     * - st_atime:   Last read timestamp
     * - st_mtime:   Last write timestamp
     * - st_ctime:   Last metadata change timestamp
     * - st_blksize: Filesystem chunk size (64KB)
     * - st_blocks:  Number of 512-byte blocks allocated
     */
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
 * pgfuse_vfs_readdir:
 * "Read Directory" — Called when Linux runs 'ls' to list directory contents.
 *
 * Parameters:
 * - path:   Path to the directory being read.
 * - buf:    Output buffer passed by FUSE where file names are added.
 * - filler: Callback function provided by FUSE. We invoke filler(buf, name, &st, ...)
 *           for every item in the directory to add it to the list.
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

    /* Make sure the target is actually a directory */
    if (!S_ISDIR(dir_mode)) {
        pgfuse_pool_release(&g_pgfuse.pool, conn);
        return -ENOTDIR;
    }

    /*
     * Add the standard POSIX directory navigation entries:
     * - "."  points to the current directory
     * - ".." points to the parent directory
     */
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

    /* Query child entries from pgfuse_dentries */
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

    /* Feed each file and folder name into FUSE's buffer using filler() */
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
 * pgfuse_vfs_open:
 * "Open File" — Called when an application opens an existing file.
 *
 * Performance optimization:
 * Caches the resolved Inode ID directly inside 'fi->fh' (file handle).
 * That way, all subsequent read() and write() calls on this file can skip
 * path resolution and go directly to the Inode!
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
        return -EISDIR; /* Cannot open a directory with open() */
    }

    /* Store Inode number in file handle for fast read/write */
    if (fi) {
        fi->fh = ino;
    }

    pgfuse_pool_release(&g_pgfuse.pool, conn);
    return 0;
}

/*
 * pgfuse_vfs_create:
 * "Create File" — Called on 'touch file.txt' or open(..., O_CREAT).
 *
 * Creates a new Inode in pgfuse_inodes and links a new Dentry in pgfuse_dentries.
 * Wrapped in an ACID SQL transaction (BEGIN ... COMMIT) to prevent orphaned files.
 */
static int pgfuse_vfs_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
    char parent_dir[PGFUSE_MAX_PATH_LEN];
    char base_name[PGFUSE_MAX_NAME_LEN];

    /* Split path into parent folder and file name */
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

    /* Start atomic database transaction */
    PGresult *res = PQexec(conn, "BEGIN;");
    PQclear(res);

    /* 1. Insert new Inode record */
    mode_t final_mode = S_IFREG | (mode & 07777);
    char mode_str[32], uid_str[32], gid_str[32];
    snprintf(mode_str, sizeof(mode_str), "%u", (unsigned int)final_mode);
    /* fuse_get_context: Returns the UID and GID of the process creating the file */
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

    /* 2. Link file name to its parent folder in pgfuse_dentries */
    char parent_str[32], new_ino_str[32];
    snprintf(parent_str, sizeof(parent_str), "%" PRIu64, parent_ino);
    snprintf(new_ino_str, sizeof(new_ino_str), "%" PRIu64, new_ino);

    const char *dentry_params[3] = {parent_str, base_name, new_ino_str};
    res = PQexecParams(conn,
        "INSERT INTO pgfuse_dentries (parent_ino, name, ino) "
        "VALUES ($1::BIGINT, $2, $3::BIGINT);",
        3, NULL, dentry_params, NULL, NULL, 0);

    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        /* File name already exists in this folder: rollback transaction */
        PQclear(res);
        res = PQexec(conn, "ROLLBACK;");
        PQclear(res);
        pgfuse_pool_release(&g_pgfuse.pool, conn);
        return -EEXIST; /* -EEXIST = File exists */
    }
    PQclear(res);

    /* 3. Update parent folder's modification time (mtime) */
    const char *touch_params[1] = {parent_str};
    res = PQexecParams(conn,
        "UPDATE pgfuse_inodes SET mtime = CURRENT_TIMESTAMP, ctime = CURRENT_TIMESTAMP "
        "WHERE ino = $1::BIGINT;",
        1, NULL, touch_params, NULL, NULL, 0);
    PQclear(res);

    /* Commit transaction */
    res = PQexec(conn, "COMMIT;");
    PQclear(res);

    if (fi) {
        fi->fh = new_ino; /* Cache Inode in file handle */
    }

    pgfuse_pool_release(&g_pgfuse.pool, conn);
    return 0;
}

/*
 * pgfuse_vfs_read:
 * "Read File" — Called when an application reads data from a file (e.g. 'cat').
 *
 * Parameters:
 * - path:   Path to the file.
 * - buf:    Destination memory buffer where we copy read bytes.
 * - size:   Number of bytes requested by the caller.
 * - offset: Starting byte position within the file.
 * - fi:     File info struct (fi->fh holds our cached Inode ID).
 *
 * Return: Number of bytes actually read and copied to buf.
 */
static int pgfuse_vfs_read(const char *path, char *buf, size_t size, off_t offset,
                           struct fuse_file_info *fi)
{
    if (!buf) return -EINVAL;
    if (size == 0) return 0;

    PGconn *conn = pgfuse_pool_acquire(&g_pgfuse.pool);
    /* Fast path: use cached Inode ID from fi->fh if available */
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

    /* Get current file size */
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

    /* If read offset is past End-Of-File (EOF), return 0 bytes */
    if (offset >= file_size) {
        pgfuse_pool_release(&g_pgfuse.pool, conn);
        return 0;
    }

    /* Clamp read size so we don't read past EOF */
    if (offset + (off_t)size > file_size) {
        size = (size_t)(file_size - offset);
    }

    /* Zero out buffer: missing chunks automatically read as zero bytes (sparse file support) */
    memset(buf, 0, size);

    /* Calculate chunk indices spanning this read request */
    uint64_t first_chunk = (uint64_t)(offset / PGFUSE_CHUNK_SIZE);
    uint64_t last_chunk  = (uint64_t)((offset + size - 1) / PGFUSE_CHUNK_SIZE);
    size_t   bytes_read  = 0;

    for (uint64_t c = first_chunk; c <= last_chunk; c++) {
        char chunk_str[32];
        snprintf(chunk_str, sizeof(chunk_str), "%" PRIu64, c);
        const char *chunk_params[2] = {ino_str, chunk_str};

        /*
         * PQexecParams with resultFormat = 1:
         * Asks PostgreSQL to transmit RAW BINARY BYTES across the socket!
         * This avoids hex encoding overhead and enables zero-copy memory operations.
         */
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
            /* PQgetvalue: Direct pointer to the raw binary byte array */
            const char *raw_data = PQgetvalue(res, 0, 1);
            int data_len = PQgetlength(res, 0, 1);

            if ((int)start_in_chunk < data_len) {
                size_t available = (size_t)(data_len - start_in_chunk);
                size_t to_copy = (chunk_span < available) ? chunk_span : available;
                /* Zero-copy slice directly into the caller's output buffer */
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
 * pgfuse_vfs_write:
 * "Write File" — Called when saving data to a file (e.g. echo, cp, dd, write()).
 *
 * Implements a Read-Modify-Write (RMW) Engine for 64KB block striping:
 * - If writing aligned full 64KB blocks: Streams data directly (Fast Path).
 * - If writing unaligned boundary bytes: Reads existing 64KB block, overlays
 *   new bytes in memory, and upserts the chunk back using PostgreSQL's
 *   ON CONFLICT (ino, chunk_index) DO UPDATE mechanism.
 *
 * Return: Number of bytes written on success.
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

    /* Fetch current file size */
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

    /* Heap-allocated 64KB staging buffer (avoids stack overflow) */
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

            /* 2. Modify: splice new bytes into the buffer */
            memcpy(chunk_staging + chunk_offset, buf + bytes_written, write_len);
            int span_end = (int)(chunk_offset + write_len);
            final_chunk_len = (span_end > existing_len) ? span_end : existing_len;
        } 
        /* CASE 2: Full 64KB write -> FAST PATH (skip reading existing chunk) */
        else {
            memcpy(chunk_staging, buf + bytes_written, PGFUSE_CHUNK_SIZE);
            final_chunk_len = PGFUSE_CHUNK_SIZE;
        }

        /* 3. Write: Upsert chunk into pgfuse_chunks using binary parameter binding */
        char chunk_len_str[32];
        snprintf(chunk_len_str, sizeof(chunk_len_str), "%d", final_chunk_len);

        const char *upsert_values[4] = {ino_str, chunk_str, chunk_len_str, chunk_staging};
        int upsert_lengths[4] = {
            (int)strlen(ino_str),
            (int)strlen(chunk_str),
            (int)strlen(chunk_len_str),
            final_chunk_len
        };
        /* paramFormats[3] = 1: Tells PostgreSQL the 4th parameter is RAW BINARY (BYTEA) */
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

    /* Update file size in pgfuse_inodes if the write extended the file */
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
 * pgfuse_vfs_mkdir:
 * "Make Directory" — Called on 'mkdir foldername'.
 *
 * 1. Inserts an Inode in pgfuse_inodes with S_IFDIR mode and nlink = 2.
 * 2. Inserts a Dentry in pgfuse_dentries linking foldername to parent Inode.
 * 3. Increments parent folder's nlink by 1 (standard UNIX filesystem rule).
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

    /* Set mode with S_IFDIR directory flag */
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

    /* Link dentry in parent directory */
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

    /* Increment parent directory's nlink counter */
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
 * pgfuse_vfs_unlink:
 * "Unlink File" — Called when deleting a regular file with 'rm file.txt'.
 *
 * 1. Deletes the dentry row from pgfuse_dentries.
 * 2. Decrements nlink in pgfuse_inodes.
 * 3. Crucial POSIX rule: Only if nlink reaches 0 do we delete the file's data chunks
 *    and Inode record (supports UNIX hard links).
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
        return -EISDIR; /* Cannot use unlink() on a directory; must use rmdir() */
    }

    PGresult *res = PQexec(conn, "BEGIN;");
    PQclear(res);

    char parent_str[32], target_str[32];
    snprintf(parent_str, sizeof(parent_str), "%" PRIu64, parent_ino);
    snprintf(target_str, sizeof(target_str), "%" PRIu64, target_ino);

    /* 1. Remove dentry link from directory */
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

        /* 3. If no links remain, reclaim storage: delete chunks and inode */
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

    /* Update parent folder timestamp */
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
 * pgfuse_vfs_rmdir:
 * "Remove Directory" — Called when removing a folder with 'rmdir foldername'.
 *
 * Enforces standard POSIX rule:
 * If the folder has any items inside it, fails immediately with -ENOTEMPTY.
 * Root folder (Inode 1) cannot be deleted (-EBUSY).
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

    /* Root directory protection */
    if (target_ino == PGFUSE_ROOT_INODE) {
        pgfuse_pool_release(&g_pgfuse.pool, conn);
        return -EBUSY; /* -EBUSY = Resource busy: cannot delete root directory */
    }

    char target_str[32], parent_str[32];
    snprintf(target_str, sizeof(target_str), "%" PRIu64, target_ino);
    snprintf(parent_str, sizeof(parent_str), "%" PRIu64, parent_ino);

    /* Check if directory has any children */
    const char *check_params[1] = {target_str};
    PGresult *res = PQexecParams(conn,
        "SELECT COUNT(*) FROM pgfuse_dentries WHERE parent_ino = $1::BIGINT;",
        1, NULL, check_params, NULL, NULL, 0);

    int entry_count = atoi(PQgetvalue(res, 0, 0));
    PQclear(res);

    if (entry_count > 0) {
        pgfuse_pool_release(&g_pgfuse.pool, conn);
        return -ENOTEMPTY; /* -ENOTEMPTY = Directory not empty */
    }

    res = PQexec(conn, "BEGIN;");
    PQclear(res);

    /* Delete dentry link */
    const char *del_dentry_params[2] = {parent_str, base_name};
    res = PQexecParams(conn,
        "DELETE FROM pgfuse_dentries WHERE parent_ino = $1::BIGINT AND name = $2;",
        2, NULL, del_dentry_params, NULL, NULL, 0);
    PQclear(res);

    /* Delete inode record */
    const char *del_inode_params[1] = {target_str};
    res = PQexecParams(conn,
        "DELETE FROM pgfuse_inodes WHERE ino = $1::BIGINT;",
        1, NULL, del_inode_params, NULL, NULL, 0);
    PQclear(res);

    /* Decrement parent directory's nlink counter */
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
 * pgfuse_vfs_truncate:
 * "Resize File" — Called when resizing a file (e.g. 'truncate -s 0 file.txt' or '> file.txt').
 *
 * Drops any chunks beyond the new size boundary and updates file size in pgfuse_inodes.
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
        /* Truncate to 0: delete all chunks for this file */
        const char *del_params[1] = {ino_str};
        res = PQexecParams(conn,
            "DELETE FROM pgfuse_chunks WHERE ino = $1::BIGINT;",
            1, NULL, del_params, NULL, NULL, 0);
        PQclear(res);
    } else {
        uint64_t last_valid_chunk = (uint64_t)((size - 1) / PGFUSE_CHUNK_SIZE);
        char chunk_str[32];
        snprintf(chunk_str, sizeof(chunk_str), "%" PRIu64, last_valid_chunk);

        /* Delete any chunks past the new size ceiling */
        const char *purge_params[2] = {ino_str, chunk_str};
        res = PQexecParams(conn,
            "DELETE FROM pgfuse_chunks WHERE ino = $1::BIGINT AND chunk_index > $2::BIGINT;",
            2, NULL, purge_params, NULL, NULL, 0);
        PQclear(res);
    }

    /* Update new size in inodes table */
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
 * pgfuse_vfs_statfs:
 * "Filesystem Capacity Stats" — Called when Linux runs 'df -h' or 'stat -f'.
 *
 * Populates struct statvfs:
 * - f_bsize:   Filesystem block size (64KB)
 * - f_blocks:  Total blocks in the filesystem (1,048,576 blocks = 64GB)
 * - f_bfree:   Unallocated free blocks
 * - f_bavail:  Available blocks for non-root users
 * - f_files:   Total inodes supported
 * - f_ffree:   Free inodes available
 * - f_namemax: Maximum file name length (255 chars)
 */
static int pgfuse_vfs_statfs(const char *path, struct statvfs *stbuf)
{
    (void)path;
    if (!stbuf) return -EINVAL;
    memset(stbuf, 0, sizeof(struct statvfs));

    PGconn *conn = pgfuse_pool_acquire(&g_pgfuse.pool);

    /* Read geometry from pgfuse_superblock */
    PGresult *res = PQexec(conn,
        "SELECT block_size, total_blocks FROM pgfuse_superblock LIMIT 1;");

    uint64_t bsize = PGFUSE_CHUNK_SIZE;
    uint64_t total_blocks = 1048576; /* 64 GB */

    if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0) {
        bsize = (uint64_t)strtoull(PQgetvalue(res, 0, 0), NULL, 10);
        total_blocks = (uint64_t)strtoull(PQgetvalue(res, 0, 1), NULL, 10);
    }
    PQclear(res);

    /* Count currently allocated 64KB chunks in the database */
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
 * pgfuse_vfs_init:
 * "Filesystem Initialization" — Called immediately when the filesystem is mounted.
 * Sets kernel caching timeouts and prints the startup banner.
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
 * pgfuse_vfs_destroy:
 * "Filesystem Shutdown" — Called when unmounting (fusermount3 -u).
 * Cleanly drains and closes all database connections in the pool.
 */
static void pgfuse_vfs_destroy(void *private_data)
{
    (void)private_data;
    printf("[PGFUSE-INFO] Unmounting filesystem. Draining connection pool...\n");
    pgfuse_pool_destroy(&g_pgfuse.pool);
    printf("[PGFUSE-INFO] Teardown complete. Clean exit.\n");
}

/* ============================================================================
 * FUSE Operation Dispatch Table
 *
 * This struct maps FUSE callback names to our C functions.
 * When the Linux kernel executes a system call on our mounted folder,
 * FUSE invokes the corresponding function pointer from this table.
 * ============================================================================
 */
static struct fuse_operations pgfuse_vfs_ops = {
    .getattr    = pgfuse_vfs_getattr,  /* Called on stat, ls -l */
    .readdir    = pgfuse_vfs_readdir,  /* Called on ls, readdir */
    .open       = pgfuse_vfs_open,     /* Called on open() */
    .read       = pgfuse_vfs_read,     /* Called on read(), cat */
    .write      = pgfuse_vfs_write,    /* Called on write(), cp, echo */
    .mkdir      = pgfuse_vfs_mkdir,    /* Called on mkdir */
    .unlink     = pgfuse_vfs_unlink,   /* Called on rm (delete file) */
    .rmdir      = pgfuse_vfs_rmdir,    /* Called on rmdir (delete folder) */
    .truncate   = pgfuse_vfs_truncate, /* Called on truncate, > redirection */
    .create     = pgfuse_vfs_create,   /* Called on touch, open(O_CREAT) */
    .statfs     = pgfuse_vfs_statfs,   /* Called on df -h */
    .init       = pgfuse_vfs_init,     /* Called when filesystem mounts */
    .destroy    = pgfuse_vfs_destroy,  /* Called when filesystem unmounts */
};

/* ============================================================================
 * Main Entry Point
 *
 * 1. Reads database connection settings from environment or defaults.
 * 2. Bootstraps the 8-channel PostgreSQL connection pool.
 * 3. Calls fuse_main to enter the kernel event loop.
 * ============================================================================
 */
int main(int argc, char *argv[])
{
    /* Read PostgreSQL connection string from environment or use default */
    const char *conninfo = getenv("PGFUSE_CONNSTRING");
    if (!conninfo || strlen(conninfo) == 0) {
        conninfo = PGFUSE_DEFAULT_CONNINFO;
    }

    /* Read connection pool size from environment or use default (8) */
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

    /* Hand off control to the FUSE event loop: intercepts VFS calls until unmounted */
    return fuse_main(argc, argv, &pgfuse_vfs_ops, NULL);
}
