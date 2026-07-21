# FAT16 Filesystem

This document details the BIOS Parameter Block (BPB) validation, cluster allocation chains, and directory reading/writing of the FAT16 filesystem implementation in PhotonOS.

---

## 1. FAT16 BPB Validation

During `fat16_mount()`:
1. The kernel reads sector `0` (the Boot Sector) from the drive.
2. The BIOS Parameter Block (`BPB`) is parsed and validated:
   - **Bytes Per Sector**: Must be `512`.
   - **FAT Count**: Typically `2`.
   - **Signature**: Must contain the boot signature `0x55AA` at offset `510`.
   - Checks total sectors and sectors per FAT.

---

## 2. Directory Navigation and File Operations

- **Root Directory Layout**: Unlike directories in subfolders, the root directory has a fixed size and location computed from BPB parameters:
  ```text
  Root LBA = Reserved Sectors + (FAT Count × Sectors Per FAT)
  ```
- **Directories Parsing**: Files are parsed as 32-byte entries containing name, extension, attributes, creation date, first cluster index, and file size.
- **Reading (`fat16_read`)**: To read a file, the driver:
  1. Locates its first cluster.
  2. Resolves the cluster chain by traversing the File Allocation Table (FAT).
  3. Reads the corresponding sectors via the ATA driver and fills the buffer.

---

## 3. Writing and Cluster Allocation Chains

Writing to a FAT16 partition is implemented through `fat16_write_file()` and integrated into the VFS layer (`fat16_vfs_write` / `fat16_vfs_create`).

### A. Concurrency Protection (`fat16_mutex`)
To prevent corruption when multiple processes perform file system operations (such as creating files, extending files, or editing metadata), the FAT16 driver synchronizes all active routines using a global lock:
```c
static mutex_t fat16_mutex;
```
Any entry point modifying filesystem metadata (e.g., `fat16_write_file`, `fat16_vfs_create`) acquires `fat16_mutex` at start and releases it on exit, ensuring serialized updates.

### B. Cluster Allocation & Resizing
When a write request is received:
1. **Lookup**: The driver looks up the root directory entry. If the file doesn't exist, it allocates a starting cluster via `allocate_cluster_unlocked()` and creates the directory entry.
2. **Chain Resizing**: The driver compares the current file size with the requested size. It calls `resize_chain_unlocked()` to grow or shrink the cluster allocation chain:
   - To grow, the driver traverses to the end of the existing chain, searches the FAT table for free clusters (`0x0000`), links them, and updates their values to the End-Of-Chain marker (`0xFFFF`).
   - To shrink, it truncates the chain at the required cluster count and returns unused clusters to the free pool by setting their FAT entries back to `0x0000`.
3. **Writing Payload**: Once the cluster chain is sized, the driver calls `write_file_content_unlocked()`. It maps buffer data to sectors, using the ATA driver to perform physical disk sector writes.
4. **Directory Sync**: Finally, the new file size and starting cluster are recorded back to the file's 32-byte directory entry on disk, and the FAT sector caches are flushed.

---

## 4. VFS Integration

The FAT16 driver exposes VFS function pointers for file lookup, creation, reading, and writing.
During kernel boot, if a FAT16 structure is detected on the primary master IDE drive `/dev/hda`, the volume is mounted, and the VFS root node pointers are mapped to the FAT16 VFS handlers.

---

## 5. Current Limitations

- **No Subdirectory Writes**: Writing files is constrained to the root directory `/`. Reading subdirectories is supported, but creation and modification of files are limited to root.
- **Filename Limit**: Supports only the 8.3 filename layout (8 characters for name, 3 for extension). Long Filenames (LFN) are ignored.
- **Single Threaded I/O**: The driver relies on synchronous ATA PIO operations, blocking the calling thread until disk I/O completes.

