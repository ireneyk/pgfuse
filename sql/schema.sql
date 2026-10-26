-- ============================================================================
-- Database Schema for pgfuse
--
-- Translates Linux filesystem concepts (inodes, directories, data blocks)
-- into relational PostgreSQL tables.
-- ============================================================================

-- Clean up any existing tables so we start fresh:
-- CASCADE: Automatically drops dependent foreign keys and indexes too.
DROP TABLE IF EXISTS pgfuse_chunks CASCADE;
DROP TABLE IF EXISTS pgfuse_dentries CASCADE;
DROP TABLE IF EXISTS pgfuse_inodes CASCADE;
DROP TABLE IF EXISTS pgfuse_superblock CASCADE;

-- ----------------------------------------------------------------------------
-- 1. Superblock: Global Filesystem Settings
-- ----------------------------------------------------------------------------
-- In Linux filesystems (like ext4), the "superblock" holds global filesystem info:
-- block size, total storage capacity, and the filesystem magic identifier.
CREATE TABLE pgfuse_superblock (
    -- magic_number: 1346913875 is 0x50474653 in hex ('PGFS' in ASCII).
    -- Tools like 'stat -f' check this magic number to recognize our filesystem.
    magic_number      BIGINT       NOT NULL DEFAULT 1346913875,
    fs_version        INT          NOT NULL DEFAULT 1,
    -- block_size: 64KB chunk size (65,536 bytes) used for file data slicing.
    block_size        BIGINT       NOT NULL DEFAULT 65536,
    -- total_blocks: 1,048,576 blocks = 64 GB total virtual disk capacity.
    total_blocks      BIGINT       NOT NULL DEFAULT 1048576,
    -- TIMESTAMPTZ: PostgreSQL data type storing exact date, time, and timezone.
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
-- In Linux, files are NOT identified by their names! They are identified by an Inode number.
-- An Inode stores only metadata (permissions, file size, owner, timestamps).
CREATE TABLE pgfuse_inodes (
    -- BIGSERIAL: An auto-incrementing 64-bit integer (1, 2, 3...) that generates unique Inode IDs.
    ino               BIGSERIAL    PRIMARY KEY,
    -- mode: Linux integer encoding file type (folder vs file) and permissions (like 0755 or 0644).
    mode              INTEGER      NOT NULL,
    -- nlink: Number of directory entries pointing to this inode (hard links).
    -- Regular files start at 1. Folders start at 2 (self and '.' entry).
    nlink             INTEGER      NOT NULL DEFAULT 1,
    -- uid / gid: User ID and Group ID of the file owner (e.g. 1000 for regular user).
    uid               INTEGER      NOT NULL DEFAULT 1000,
    gid               INTEGER      NOT NULL DEFAULT 1000,
    -- size: Logical file size in bytes (e.g. 1024 bytes).
    size              BIGINT       NOT NULL DEFAULT 0,
    -- POSIX Timestamps:
    -- atime = Access Time: last time the file was read.
    -- mtime = Modification Time: last time the file's content was changed.
    -- ctime = Change Time: last time the file's metadata/permissions were changed.
    atime             TIMESTAMPTZ  NOT NULL DEFAULT CURRENT_TIMESTAMP,
    -- TIMESTAMPTZ: Timestamp with Time Zone (records exact microsecond time).
    mtime             TIMESTAMPTZ  NOT NULL DEFAULT CURRENT_TIMESTAMP,
    ctime             TIMESTAMPTZ  NOT NULL DEFAULT CURRENT_TIMESTAMP
);

-- Inode 1 is permanently reserved as the root directory ("/")
-- Mode 16877 decimal = 040755 octal (040000 = S_IFDIR directory + 0755 permissions).
INSERT INTO pgfuse_inodes (ino, mode, nlink, uid, gid, size)
VALUES (1, 16877, 2, 0, 0, 4096);

-- Set sequence so the next file created automatically gets Inode 2
SELECT setval('pgfuse_inodes_ino_seq', 1, true);

-- ----------------------------------------------------------------------------
-- 3. Dentries: Directory Entries (The Phonebook)
-- ----------------------------------------------------------------------------
-- Maps (parent_folder_id, file_name) -> target_inode_id.
-- Why separate Dentries from Inodes?
-- If you rename a folder containing 50,000 files, we only update 1 single row
-- in this table (O(1) rename), rather than updating 50,000 files!
CREATE TABLE pgfuse_dentries (
    parent_ino        BIGINT       NOT NULL REFERENCES pgfuse_inodes(ino) ON DELETE CASCADE,
    name              VARCHAR(255) NOT NULL,
    ino               BIGINT       NOT NULL REFERENCES pgfuse_inodes(ino) ON DELETE CASCADE,
    PRIMARY KEY (parent_ino, name)
);

-- Secondary B-tree index: makes looking up parent folders from an Inode lightning fast
CREATE INDEX idx_pgfuse_dentries_child ON pgfuse_dentries (ino);

-- ----------------------------------------------------------------------------
-- 4. Chunks: Sliced 64KB Binary Data Blocks
-- ----------------------------------------------------------------------------
-- Large files are sliced into uniform 64KB blocks.
-- Why? Modifying 5 bytes inside a 50MB file only updates one 64KB chunk row,
-- instead of rewriting the entire 50MB.
CREATE TABLE pgfuse_chunks (
    ino               BIGINT       NOT NULL REFERENCES pgfuse_inodes(ino) ON DELETE CASCADE,
    -- chunk_index: Slice number inside the file (0 = first 64KB, 1 = second 64KB, etc.).
    chunk_index       BIGINT       NOT NULL,
    -- chunk_size: How many bytes are actually stored in this chunk (up to 65536).
    chunk_size        INTEGER      NOT NULL,
    -- BYTEA: PostgreSQL binary data type ("byte array"). Stores raw file bytes without corruption.
    chunk_data        BYTEA        NOT NULL,
    PRIMARY KEY (ino, chunk_index)
);

-- ----------------------------------------------------------------------------
-- 5. Helper Functions & Monitoring Views
-- ----------------------------------------------------------------------------

-- pgfuse_create_dir:
-- Atomically creates a new directory inode and links its dentry in one single database call.
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
    -- 1. Create the inode record for the folder (size starts at 4096 bytes, nlink starts at 2)
    INSERT INTO pgfuse_inodes (mode, nlink, uid, gid, size)
    VALUES (p_mode, 2, p_uid, p_gid, 4096)
    RETURNING ino INTO v_new_ino;

    -- 2. Link the folder name to its parent directory
    INSERT INTO pgfuse_dentries (parent_ino, name, ino)
    VALUES (p_parent_ino, p_name, v_new_ino);

    -- 3. In Linux, creating a subfolder increments the parent folder's nlink (due to '..' link)
    UPDATE pgfuse_inodes 
    SET nlink = nlink + 1, mtime = CURRENT_TIMESTAMP, ctime = CURRENT_TIMESTAMP 
    WHERE ino = p_parent_ino;

    RETURN v_new_ino;
END;
$$ LANGUAGE plpgsql;

-- pgfuse_create_file:
-- Atomically creates a new regular file inode and links its dentry in one step.
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
    -- 1. Create the inode record for the regular file (size starts at 0 bytes, nlink starts at 1)
    INSERT INTO pgfuse_inodes (mode, nlink, uid, gid, size)
    VALUES (p_mode, 1, p_uid, p_gid, 0)
    RETURNING ino INTO v_new_ino;

    -- 2. Link the file name to its parent directory
    INSERT INTO pgfuse_dentries (parent_ino, name, ino)
    VALUES (p_parent_ino, p_name, v_new_ino);

    -- 3. Update the parent folder's modification time (mtime)
    UPDATE pgfuse_inodes 
    SET mtime = CURRENT_TIMESTAMP, ctime = CURRENT_TIMESTAMP 
    WHERE ino = p_parent_ino;

    RETURN v_new_ino;
END;
$$ LANGUAGE plpgsql;

-- v_pgfuse_storage_summary:
-- Diagnostic SQL view: calculates total files, directories, chunks, and storage bytes.
-- Notes on functions:
--   mode & 61440: Bitwise AND with S_IFMT (0170000 octal).
--     32768 = 0100000 octal (S_IFREG = regular file).
--     16384 = 0040000 octal (S_IFDIR = directory folder).
--   COALESCE: If sum is NULL (empty table), return 0 instead.
--   OCTET_LENGTH: Counts the exact number of raw bytes stored inside the BYTEA column.
CREATE OR REPLACE VIEW v_pgfuse_storage_summary AS
SELECT 
    (SELECT COUNT(*) FROM pgfuse_inodes WHERE (mode & 61440) = 32768) AS total_files,
    (SELECT COUNT(*) FROM pgfuse_inodes WHERE (mode & 61440) = 16384) AS total_directories,
    (SELECT COUNT(*) FROM pgfuse_chunks) AS total_chunks,
    (SELECT COALESCE(SUM(size), 0) FROM pgfuse_inodes) AS logical_bytes_used,
    (SELECT COALESCE(SUM(OCTET_LENGTH(chunk_data)), 0) FROM pgfuse_chunks) AS physical_bytes_stored;
