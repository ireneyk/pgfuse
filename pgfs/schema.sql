-- ============================================================================
-- Project:       PG-FS (PostgreSQL-Backed Virtual File System)
-- Author:        Irene Therese Joseph
-- Role:          Senior Linux Systems Engineer
-- Description:   Production-grade relational schema for virtual filesystem
--                metadata, hierarchical dentries, and striped binary chunks.
-- ============================================================================

-- Clean up any existing deployment
DROP TABLE IF EXISTS fs_chunks CASCADE;
DROP TABLE IF EXISTS fs_dentries CASCADE;
DROP TABLE IF EXISTS fs_inodes CASCADE;
DROP TABLE IF EXISTS fs_superblock CASCADE;

-- ----------------------------------------------------------------------------
-- 1. Superblock: Global Filesystem Parameters & Integrity
-- ----------------------------------------------------------------------------
CREATE TABLE fs_superblock (
    magic_number      BIGINT       NOT NULL DEFAULT 1346913875, -- 0x50474653 ('PGFS')
    fs_version        INT          NOT NULL DEFAULT 1,
    block_size        BIGINT       NOT NULL DEFAULT 65536,      -- 64 KB chunk size
    total_blocks      BIGINT       NOT NULL DEFAULT 1048576,    -- 64 GB virtual capacity
    created_at        TIMESTAMPTZ  NOT NULL DEFAULT CURRENT_TIMESTAMP,
    last_mounted_at   TIMESTAMPTZ  NOT NULL DEFAULT CURRENT_TIMESTAMP,
    mount_count       BIGINT       NOT NULL DEFAULT 0,
    fs_label          VARCHAR(64)  NOT NULL DEFAULT 'PGFS_DATA_VOLUME'
);

-- Initialize default superblock record
INSERT INTO fs_superblock (magic_number, fs_version, block_size, total_blocks, fs_label)
VALUES (1346913875, 1, 65536, 1048576, 'PGFS_ENTERPRISE_ROOT');

-- ----------------------------------------------------------------------------
-- 2. Inodes: Core File and Directory Attributes
-- ----------------------------------------------------------------------------
-- Maps POSIX struct stat attributes directly to relational fields.
-- Inode 1 is reserved as the root directory ('/').
CREATE TABLE fs_inodes (
    ino               BIGSERIAL    PRIMARY KEY,
    mode              INTEGER      NOT NULL,                     -- File mode (permissions + S_IFREG / S_IFDIR)
    nlink             INTEGER      NOT NULL DEFAULT 1,           -- Hard link counter
    uid               INTEGER      NOT NULL DEFAULT 1000,        -- Owner UID
    gid               INTEGER      NOT NULL DEFAULT 1000,        -- Owner GID
    size              BIGINT       NOT NULL DEFAULT 0,           -- File size in bytes
    atime             TIMESTAMPTZ  NOT NULL DEFAULT CURRENT_TIMESTAMP,
    mtime             TIMESTAMPTZ  NOT NULL DEFAULT CURRENT_TIMESTAMP,
    ctime             TIMESTAMPTZ  NOT NULL DEFAULT CURRENT_TIMESTAMP
);

-- Seed Inode 1: Filesystem Root Directory ('/')
-- Mode 040755 = S_IFDIR (0040000) | rwxr-xr-x (0755) = 16877 decimal
INSERT INTO fs_inodes (ino, mode, nlink, uid, gid, size)
VALUES (1, 16877, 2, 0, 0, 4096);

-- Synchronize the auto-increment sequence to start after Inode 1
SELECT setval('fs_inodes_ino_seq', 1, true);

-- ----------------------------------------------------------------------------
-- 3. Dentries: Hierarchical Directory Namespace
-- ----------------------------------------------------------------------------
-- Maps parent directory inode + entry name to child target inode.
CREATE TABLE fs_dentries (
    parent_ino        BIGINT       NOT NULL REFERENCES fs_inodes(ino) ON DELETE CASCADE,
    name              VARCHAR(255) NOT NULL,
    ino               BIGINT       NOT NULL REFERENCES fs_inodes(ino) ON DELETE CASCADE,
    PRIMARY KEY (parent_ino, name)
);

-- High-performance lookup index on child inode references
CREATE INDEX idx_dentries_child_ino ON fs_dentries (ino);

-- ----------------------------------------------------------------------------
-- 4. Chunks: Striped Binary Data Storage Layer
-- ----------------------------------------------------------------------------
-- Files are partitioned into fixed 64KB blocks (chunks) to avoid massive BLOB
-- rewrites on small random writes, mirroring Ceph RADOS and GPFS NSD striping.
CREATE TABLE fs_chunks (
    ino               BIGINT       NOT NULL REFERENCES fs_inodes(ino) ON DELETE CASCADE,
    chunk_index       BIGINT       NOT NULL,
    chunk_size        INTEGER      NOT NULL,
    chunk_data        BYTEA        NOT NULL,
    PRIMARY KEY (ino, chunk_index)
);

-- ----------------------------------------------------------------------------
-- 5. Stored Procedures & Transactional Helper Functions
-- ----------------------------------------------------------------------------

-- Atomically allocates a new directory and links it to its parent
CREATE OR REPLACE FUNCTION pgfs_create_dir(
    p_parent_ino BIGINT,
    p_name VARCHAR(255),
    p_mode INTEGER,
    p_uid INTEGER,
    p_gid INTEGER
) RETURNS BIGINT AS $$
DECLARE
    v_new_ino BIGINT;
BEGIN
    -- Verify parent directory exists and is a directory (S_IFDIR = 0040000)
    IF NOT EXISTS (
        SELECT 1 FROM fs_inodes 
        WHERE ino = p_parent_ino AND (mode & 61440) = 16384
    ) THEN
        RAISE EXCEPTION 'Parent inode % is invalid or not a directory', p_parent_ino;
    END IF;

    -- Create Inode
    INSERT INTO fs_inodes (mode, nlink, uid, gid, size)
    VALUES (p_mode, 2, p_uid, p_gid, 4096)
    RETURNING ino INTO v_new_ino;

    -- Create Dentry
    INSERT INTO fs_dentries (parent_ino, name, ino)
    VALUES (p_parent_ino, p_name, v_new_ino);

    -- Increment parent link count (standard UNIX directory hierarchy)
    UPDATE fs_inodes 
    SET nlink = nlink + 1, mtime = CURRENT_TIMESTAMP, ctime = CURRENT_TIMESTAMP 
    WHERE ino = p_parent_ino;

    RETURN v_new_ino;
END;
$$ LANGUAGE plpgsql;

-- Atomically allocates a new regular file and links it to its parent
CREATE OR REPLACE FUNCTION pgfs_create_file(
    p_parent_ino BIGINT,
    p_name VARCHAR(255),
    p_mode INTEGER,
    p_uid INTEGER,
    p_gid INTEGER
) RETURNS BIGINT AS $$
DECLARE
    v_new_ino BIGINT;
BEGIN
    -- Create Inode
    INSERT INTO fs_inodes (mode, nlink, uid, gid, size)
    VALUES (p_mode, 1, p_uid, p_gid, 0)
    RETURNING ino INTO v_new_ino;

    -- Create Dentry
    INSERT INTO fs_dentries (parent_ino, name, ino)
    VALUES (p_parent_ino, p_name, v_new_ino);

    -- Touch parent directory timestamps
    UPDATE fs_inodes 
    SET mtime = CURRENT_TIMESTAMP, ctime = CURRENT_TIMESTAMP 
    WHERE ino = p_parent_ino;

    RETURN v_new_ino;
END;
$$ LANGUAGE plpgsql;

-- Atomically removes a regular file dentry and cascades inode deletion if nlink drops to 0
CREATE OR REPLACE FUNCTION pgfs_unlink_file(
    p_parent_ino BIGINT,
    p_name VARCHAR(255)
) RETURNS INTEGER AS $$
DECLARE
    v_target_ino BIGINT;
    v_current_nlink INTEGER;
BEGIN
    -- Find target inode
    SELECT ino INTO v_target_ino 
    FROM fs_dentries 
    WHERE parent_ino = p_parent_ino AND name = p_name;

    IF v_target_ino IS NULL THEN
        RETURN -2; -- -ENOENT
    END IF;

    -- Remove dentry
    DELETE FROM fs_dentries 
    WHERE parent_ino = p_parent_ino AND name = p_name;

    -- Decrement nlink
    UPDATE fs_inodes 
    SET nlink = nlink - 1, ctime = CURRENT_TIMESTAMP 
    WHERE ino = v_target_ino
    RETURNING nlink INTO v_current_nlink;

    -- If no hard links remain, clean up chunks and inode
    IF v_current_nlink <= 0 THEN
        DELETE FROM fs_chunks WHERE ino = v_target_ino;
        DELETE FROM fs_inodes WHERE ino = v_target_ino;
    END IF;

    -- Update parent timestamps
    UPDATE fs_inodes 
    SET mtime = CURRENT_TIMESTAMP, ctime = CURRENT_TIMESTAMP 
    WHERE ino = p_parent_ino;

    RETURN 0;
END;
$$ LANGUAGE plpgsql;

-- Diagnostic views for storage administration and capacity planning
CREATE OR REPLACE VIEW v_pgfs_storage_summary AS
SELECT 
    (SELECT COUNT(*) FROM fs_inodes WHERE (mode & 61440) = 32768) AS total_files,
    (SELECT COUNT(*) FROM fs_inodes WHERE (mode & 61440) = 16384) AS total_directories,
    (SELECT COUNT(*) FROM fs_chunks) AS total_chunks,
    (SELECT COALESCE(SUM(size), 0) FROM fs_inodes) AS logical_bytes_used,
    (SELECT COALESCE(SUM(OCTET_LENGTH(chunk_data)), 0) FROM fs_chunks) AS physical_bytes_stored;
