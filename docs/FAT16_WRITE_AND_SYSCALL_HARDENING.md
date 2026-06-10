# 📁 FAT16 Write Operations, sys_write Hardening, and ELF Size Optimization

This document provides a detailed overview of the implementation, design decisions, and system validations for file writing on FAT16, system call security isolation, and link-time kernel size compaction.

---

## 🎯 Overview

With the arrival of PhotonOS v1.4, the operating system achieves complete writing capabilities to the FAT16 filesystem from user space (Ring 3). The core additions include:
1. **FAT16 Driver Write Stack**: Implementation of cluster allocation and sector synchronization.
2. **Hardened System Call Interface**: Ensuring memory boundary safety and user buffer isolation.
3. **Linker Size Reduction**: Compacting the binary size to fit the strict 144KB boot loader limits.

---

## 🛠️ Architectural Details

### 1. FAT16 Cluster Allocation & Synchronization
The FAT16 driver (`src/drivers/fat16.c`) includes a complete write pipeline:
- **`ensure_chain_capacity`**: Traverses the FAT in memory looking for free clusters (`0x0000`). If the file size grows beyond its current cluster limit, the driver allocates a new cluster, marks it as End-of-File (`0xFFFF`), and updates the previous cluster's entry to point to it.
- **ATA Write Integration**: Writes are committed to the virtual disk via the ATA PIO block driver (`src/drivers/ata.c`).
- **Metadata Synchronization**: Updates the 32-byte directory entry in the parent directory. Fields like `file_size` and `first_cluster` (if the file was empty) are synchronized to the disk.

### 2. Hardened `sys_write`
The system call wrapper in `kernel.c` is hardened against Ring 3 pointer exploits:
- **User Memory Validation**: Uses `vmm_is_mapped` to verify that the start and end of the user-space buffer are mapped in the current process's PML4 table before any access.
- **Kernel Bounce Buffer**: Allocates a temporary page-size buffer on the Kernel Heap using `kmalloc` to copy user data before the write operation starts, preventing race conditions or unsafe modifications.
- **Size Capping**: Imposes a limit of `4096` bytes per write call to prevent memory exhaustion of the Kernel Heap.

### 3. ELF Section Merging (`-N` Flag)
The kernel embeds user programs (like the shell, hello, ping) inside its own image as an initrd blob. By default, the linker aligns ELF sections (`.text`, `.rodata`, `.data`) to `0x1000` (4KB) boundaries.
- **The Issue**: This alignment generated massive amounts of zero padding inside the kernel binary, ballooning `photon.bin` to `149,176` bytes (exceeding the strict boot limit of `147,456` bytes).
- **The Solution**: We added the `-N` (OMAGIC) flag to the linker options of the user programs in the [Makefile](file:///c:/Users/3AM-IT/Documents/PhotonOS/Makefile):
  ```makefile
  USER_LDFLAGS := -nostdlib -s -N -z max-page-size=0x1000 -Ttext=0x8000001000 -e _start
  ```
- **Why it is safe**: The OMAGIC flag merges the text, rodata, and data sections into a single read-write-execute (RWE) program header. The custom ELF loader (`src/kernel/elf.c`) maps the segments into process memory consecutively. By eliminating page-boundary padding, `photon.bin` shrunk from **149KB** to **96KB** (saving 53KB of memory space).

---

## 🔍 Validation and Commands

### Compilation and Packaging
To clean, build, and package the files into the FAT16 disk image:
```bash
make clean
make
make fat16-disk
```

### In-Emulator Testing
1. Launch PhotonOS in QEMU:
   ```bash
   make run-fat16
   ```
2. In the interactive Shell, create and write to a file:
   ```text
   PhotonOS /> write notes.txt HelloFromPhotonOS
   write: gravado
   ```
3. Retrieve the file content to verify:
   ```text
   PhotonOS /> cat notes.txt
   HelloFromPhotonOS
   ```
