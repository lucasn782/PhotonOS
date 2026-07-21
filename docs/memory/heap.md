# Kernel Heap Allocator

This document details the architecture, blocks metadata, allocation and free algorithms, and concurrency locks of the PhotonOS Kernel Heap manager.

---

## 1. Heap Configuration

PhotonOS implements a first-fit linked-list allocator for dynamic kernel allocations in Ring 0:

- **Heap Base Address**: `0xFFFFFFFF90000000ULL`.
- **Initial Pages**: 4 pages (`16 KiB`).
- **Maximum Limit Pages**: 256 pages (`1 MiB`).
- **Alignment**: All block sizes are aligned up to 16-byte boundaries.
- **Primitivas**:
  - `kmalloc(size)`: Dynamically allocates memory blocks.
  - `kfree(ptr)`: Releases allocated blocks and coalesces them.

---

## 2. Block Metadata (`struct heap_block`)

Each dynamic allocation is prefixed by a header containing block metadata:

```c
struct heap_block {
    uint64_t magic;           // Signature magic set to 0x48454150424C4B31 (HEAP_MAGIC)
    size_t size;              // Size of the payload area (excluding this header)
    int free;                 // Status flag: 1 if free, 0 if allocated
    struct heap_block *next;  // Link to next block
    struct heap_block *prev;  // Link to previous block
};
```

---

## 3. Allocation and Deallocation Algorithms

### kmalloc (`first-fit`)
1. Align the requested size up to 16 bytes.
2. Acquire `heap_lock` (with interrupts disabled).
3. Traverse the linked list starting at `heap_head`.
4. If a block is marked `free` and has `size >= requested`:
   - If the block size exceeds requirements by more than `32` bytes (`HEAP_MIN_SPLIT` + header size), split the block to conserve memory.
   - Mark the block as `free = 0`.
   - Release `heap_lock` and return a pointer to the payload area (`block + 1`).
5. If no suitable block is found, call `heap_expand()` to map more pages, then retry.

### kfree
1. Acquire `heap_lock` (with interrupts disabled).
2. Retrieve the header pointer: `block = (struct heap_block *)ptr - 1`.
3. Verify that `block->magic == HEAP_MAGIC`.
4. Set `block->free = 1`.
5. Call `heap_coalesce()` to merge the freed block with adjacent free blocks in the list.
6. Release `heap_lock`.

---

## 4. Concurrency Locking (`heap_lock`)

The kernel heap is shared globally. To prevent concurrent corruption during dynamic allocations and cleanups across multiple cores:

- Access is synchronized using `heap_lock`:
  ```c
  static spinlock_t heap_lock;
  ```
- Any execution in `kmalloc` or `kfree` is wrapped in `spin_lock_irqsave(&heap_lock)` and `spin_unlock_irqrestore(&heap_lock, flags)`.
