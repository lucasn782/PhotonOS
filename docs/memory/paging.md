# Paging Specifications

This document defines the page flags, mask variables, entry structures, and sizes of the x86_64 paging tables used in PhotonOS.

---

## 1. Paging Hierarchy Parameters

PhotonOS uses 4-level paging where each table level contains **512 entries** (`VMM_TABLE_ENTRIES`):

- **Page Size**: `4096` bytes.
- **PTE Address Mask**: `0x000FFFFFFFFFF000ULL` (extracts physical address from entry).
- **PTE Flags Mask**: `0xFFFULL` (extracts low-level attributes).

---

## 2. Page Table Entry (PTE) Flags

Each entry in a page table has a 12-bit attribute section. The following flags are defined:

| Constant | Value | Description |
| :--- | :---: | :--- |
| `PAGE_PRESENT` | `1U << 0` | Page is loaded in physical RAM. |
| `PAGE_WRITABLE`| `1U << 1` | Read/Write access allowed if set. Read-only if cleared. |
| `PAGE_USER`    | `1U << 2` | Ring 3 (User) access allowed. Ring 0 only if cleared. |
| `PAGE_WRITE_THROUGH` | `1U << 3` | Enables write-through caching. |
| `PAGE_CACHE_DISABLE` | `1U << 4` | Disables caching. Crucial for hardware MMIO. |
| `PAGE_COW`     | `1ULL << 9` | Custom flag indicating Copy-On-Write status (bit 9). |

---

## 3. Custom Bit 9 for Copy-On-Write

The x86_64 architecture reserves bits 9, 10, and 11 in page table entries for custom use by the operating system software (referred to as `OS-Available` or `Available for system programmer use` in the Intel Software Developer's Manual).

PhotonOS uses **bit 9** (`PAGE_COW = 0x200`) to tag pages that are shared between processes under the Copy-On-Write protocol:

1. During `sys_fork`, the kernel removes the `PAGE_WRITABLE` flag from the PTE and sets the `PAGE_COW` flag.
2. If a process attempts to write to this page, the MMU triggers a Page Fault (`#PF`) because the page is not writable.
3. The `#PF` handler inspects the PTE, identifies that `PAGE_COW` is enabled, and duplicates the physical page to resolve the fault.
