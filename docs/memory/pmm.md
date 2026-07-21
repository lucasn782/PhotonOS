# Physical Memory Manager (PMM)

This document describes the bitmap-based Physical Memory Manager (PMM) of PhotonOS, page allocation, and frame reference counting structures.

---

## 1. PMM Specifications

The Physical Memory Manager (PMM) handles physical memory allocations in chunks of **4 KiB** pages.

- **Total Managed Memory**: 128 MiB (`134,217,728` bytes).
- **Page Frame Size**: 4096 bytes.
- **Total Physical Frames**: `32,768` blocks.
- **Primitivas**:
  - `pmm_alloc()`: Finds and claims a free physical frame.
  - `pmm_free()`: Decrements references and releases physical frames back to the pool.
  - `pmm_ref_inc()`: Increments the reference count of a frame (COW).
  - `pmm_ref_get()`: Retrieves the current reference count of a frame.

---

## 2. Bitmap Implementation

The allocation status of each page frame is tracked using a bitmask array `pmm_bitmap`:

- **Bitmap Size**: `4,096` bytes (`32,768` bits).
- Each bit corresponds to a physical frame index (`PFN` - Physical Frame Number):
  - `0`: Frame is free.
  - `1`: Frame is allocated or reserved.

```c
/* memory.c */
static uint64_t pmm_bitmap[512]; // 512 * 8 bytes = 4096 bytes
```

---

## 3. Frame Reference Counting (`pmm_refcounts`)

To support Copy-On-Write (COW) during process forks, a static references array `pmm_refcounts` tracks the number of virtual mappings pointing to each physical frame:

```c
static uint32_t pmm_refcounts[32768]; // 128 KiB memory footprint
```

### Reference Operations:
- **`pmm_alloc()`**: Finds the first `0` bit in `pmm_bitmap`, marks it as `1`, sets its `refcount` to `1`, and returns the frame's physical address.
- **`pmm_free(ptr)`**: Decrements the refcount of the corresponding PFN. If it reaches `0`, the bit in the `pmm_bitmap` is cleared, releasing the frame.
- **`pmm_ref_inc(ptr)`**: Increments the refcount (e.g. when mapping a page to a child's address space in `sys_fork`).
- **`pmm_ref_get(ptr)`**: Gets the count to decide if a page fault should trigger a page copy or an in-place flags upgrade.

---

## 4. Boot-time Reserved Region

During `pmm_init()`:
1. All bitmaps and refcounts are zeroed.
2. The initial range of physical memory (reserved for the bootloader, kernel text, page tables, and MMIO structures) is set to `1` in the bitmap, and their refcounts are initialized to `1` to prevent them from ever being allocated or freed dynamically.
