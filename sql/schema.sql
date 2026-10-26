-- ============================================================================
-- Database Schema for pgfuse
-- ============================================================================

-- Clean up any existing tables so we start fresh
DROP TABLE IF EXISTS pgfuse_chunks CASCADE;
DROP TABLE IF EXISTS pgfuse_dentries CASCADE;
DROP TABLE IF EXISTS pgfuse_inodes CASCADE;
DROP TABLE IF EXISTS pgfuse_superblock CASCADE;

-- ----------------------------------------------------------------------------
-- 1. Superblock: Stores global filesystem settings
-- ----------------------------------------------------------------------------
-- Holds basic filesystem information like block size (64KB) and total capacity (64GB).
CREATE TABLE pgfuse_superblock (
    magic_number      BIGINT       NOT NULL DEFAULT 1346913875, -- 0x50474653 spells 'PGFS' in ASCII
    fs_version        INT          NOT NULL DEFAULT 1,
    block_size        BIGINT       NOT NULL DEFAULT 65536,      -- 64 KB block size
    total_blocks      BIGINT       NOT NULL DEFAULT 1048576,    -- 1,048,576 blocks = 64 GB capacity
    created_at        TIMESTAMPTZ  NOT NULL DEFAULT CURRENT_TIMESTAMP,
    last_mounted_at   TIMESTAMPTZ  NOT NULL DEFAULT CURRENT_TIMESTAMP,
    mount_count       BIGINT       NOT NULL DEFAULT 0,
    fs_label          VARCHAR(64)  NOT NULL DEFAULT 'PGFUSE_VOLUME'
);

-- Seed initial superblock record
INSERT INTO pgfuse_superblock (magic_number, fs_version, block_size, total_blocks, fs_label)
VALUES (1346913875, 1, 65536, 1048576, 'PGFUSE_ROOT');

-- ----------------------------------------------------------------------------
-- 2. Inodes: File Identity & Metadata
-- ----------------------------------------------------------------------------
-- Every file and folder has an Inode ID.
-- IMPORTANT: An Inode has NO file name! It only stores metadata:
-- permissions, size, owner, and timestamps.
CREATE TABLE pgfuse_inodes (
    ino               BIGSERIAL    PRIMARY KEY,                  -- Unique 64-bit Inode ID
    mode              INTEGER      NOT NULL,                     -- File permissions + type (folder vs regular file)
    nlink             INTEGER      NOT NULL DEFAULT 1,           -- How many names point to this file (hard links)
    uid               INTEGER      NOT NULL DEFAULT 1000,        -- Owner User ID
    gid               INTEGER      NOT NULL DEFAULT 1000,        -- Owner Group ID
    size              BIGINT       NOT NULL DEFAULT 0,           -- File size in bytes
    atime             TIMESTAMPTZ  NOT NULL DEFAULT CURRENT_TIMESTAMP, -- Last read time
    mtime             TIMESTAMPTZ  NOT NULL DEFAULT CURRENT_TIMESTAMP, -- Last content write time
    ctime             TIMESTAMPTZ  NOT NULL DEFAULT CURRENT_TIMESTAMP  -- Last metadata change time
);

-- Inode 1 is reserved as the root directory ("/")
-- Mode 16877 decimal = 040755 octal (Folder + rwxr-xr-x permissions)
INSERT INTO pgfuse_inodes (ino, mode, nlink, uid, gid, size)
VALUES (1, 16877, 2, 0, 0, 4096);

-- Set sequence so the next file created gets Inode 2
SELECT setval('pgfuse_inodes_ino_seq', 1, true);

-- ----------------------------------------------------------------------------
-- 3. Dentries: The Directory Phonebook
-- ----------------------------------------------------------------------------
-- Maps (parent_folder_id, file_name) -> target_inode_id.
-- This separates names from data, allowing instant O(1) folder renames.
CREATE TABLE pgfuse_dentries (
    parent_ino        BIGINT       NOT NULL REFERENCES pgfuse_inodes(ino) ON DELETE CASCADE,
    name              VARCHAR(255) NOT NULL,
    ino               BIGINT       NOT NULL REFERENCES pgfuse_inodes(ino) ON DELETE CASCADE,
    PRIMARY KEY (parent_ino, name)
);

-- Secondary index for fast reverse lookups
CREATE INDEX idx_pgfuse_dentries_child ON pgfuse_dentries (ino);

-- ----------------------------------------------------------------------------
-- 4. Chunks: Sliced 64KB Binary Data Blocks
-- ----------------------------------------------------------------------------
-- Large files are sliced into 64KB chunks to prevent write-amplification.
-- Modifying 5 bytes only updates one 64KB chunk, not the whole file.
CREATE TABLE pgfuse_chunks (
    ino               BIGINT       NOT NULL REFERENCES pgfuse_inodes(ino) ON DELETE CASCADE,
    chunk_index       BIGINT       NOT NULL,                     -- Piece number (0, 1, 2...)
    chunk_size        INTEGER      NOT NULL,                     -- Actual bytes used in this chunk
    chunk_data        BYTEA        NOT NULL,                     -- Raw binary bytes
    PRIMARY KEY (ino, chunk_index)
);

-- ----------------------------------------------------------------------------
-- 5. Helper Functions & Monitoring Views
-- ----------------------------------------------------------------------------

-- Atomically creates a new directory and links its dentry in one step
CREATE OR REPLACE FUNCTION pgfuse_create_dir(
    p_parent_ino BIGINT,
    p_name VARCHAR(255),
    p_mode INTEGER,
    p_uid INTEGER,
    p_gid INTEGER
) RETURNS BIGINT AS $$
DECLARE
    v_new_ino BIGINT;
BEGIN
    INSERT INTO pgfuse_inodes (mode, nlink, uid, gid, size)
    VALUES (p_mode, 2, p_uid, p_gid, 4096)
    RETURNING ino INTO v_new_ino;

    INSERT INTO pgfuse_dentries (parent_ino, name, ino)
    VALUES (p_parent_ino, p_name, v_new_ino);

    -- Directories start with nlink=2 and increment the parent folder's nlink
    UPDATE pgfuse_inodes 
    SET nlink = nlink + 1, mtime = CURRENT_TIMESTAMP, ctime = CURRENT_TIMESTAMP 
    WHERE ino = p_parent_ino;

    RETURN v_new_ino;
END;
$$ LANGUAGE plpgsql;

-- Atomically creates a new regular file and links its dentry
CREATE OR REPLACE FUNCTION pgfuse_create_file(
    p_parent_ino BIGINT,
    p_name VARCHAR(255),
    p_mode INTEGER,
    p_uid INTEGER,
    p_gid INTEGER
) RETURNS BIGINT AS $$
DECLARE
    v_new_ino BIGINT;
BEGIN
    INSERT INTO pgfuse_inodes (mode, nlink, uid, gid, size)
    VALUES (p_mode, 1, p_uid, p_gid, 0)
    RETURNING ino INTO v_new_ino;

    INSERT INTO pgfuse_dentries (parent_ino, name, ino)
    VALUES (p_parent_ino, p_name, v_new_ino);

    UPDATE pgfuse_inodes 
    SET mtime = CURRENT_TIMESTAMP, ctime = CURRENT_TIMESTAMP 
    WHERE ino = p_parent_ino;

    RETURN v_new_ino;
END;
$$ LANGUAGE plpgsql;

-- Handy diagnostic view to show interviewers storage statistics
CREATE OR REPLACE VIEW v_pgfuse_storage_summary AS
SELECT 
    (SELECT COUNT(*) FROM pgfuse_inodes WHERE (mode & 61440) = 32768) AS total_files,
    (SELECT COUNT(*) FROM pgfuse_inodes WHERE (mode & 61440) = 16384) AS total_directories,
    (SELECT COUNT(*) FROM pgfuse_chunks) AS total_chunks,
    (SELECT COALESCE(SUM(size), 0) FROM pgfuse_inodes) AS logical_bytes_used,
    (SELECT COALESCE(SUM(OCTET_LENGTH(chunk_data)), 0) FROM pgfuse_chunks) AS physical_bytes_stored;
