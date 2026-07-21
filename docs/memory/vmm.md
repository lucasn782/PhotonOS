# Virtual Memory Manager (VMM)

This document describes the Virtual Memory Manager (VMM) of PhotonOS, page tables traversal, address space creation, switching, destruction, and locking mechanisms.

---

## 1. VMM Specifications

The Virtual Memory Manager (VMM) maps 64-bit virtual addresses to physical frames using the standard x86_64 **4-level paging hierarchy**:

- **PML4** (Page Map Level 4)
- **PDPT** (Page Directory Pointer Table)
- **PD** (Page Directory)
- **PT** (Page Table)

```text
Virtual Address Structure:
 63          47         38         29         20         11          0
┌───────────┬──────────┬──────────┬──────────┬──────────┬─────────────┐
│ Sign Ext  │ PML4 idx │ PDPT idx │  PD idx  │  PT idx  │ Page Offset │
│ (16 bits) │ (9 bits) │ (9 bits) │ (9 bits) │ (9 bits) │ (12 bits)   │
└───────────┴──────────┴──────────┴──────────┴──────────┴─────────────┘
```

---

## 2. Kernel and User Address Space Partitioning

PhotonOS splits the PML4 directory into two distinct regions:

- **Lower Half (PML4 index 1 to 255)**: Reserved for Userspace tasks (Ring 3). This region is cloned with Copy-On-Write settings during `sys_fork`.
- **Identity Map (PML4 index 0)**: Used during early boot. Mapeia low-memory structures and MMIO ranges (e.g., e1000 controller at `0xF0000000`).
- **Upper Half (PML4 index 256 to 511)**: Reserved for Kernel Space (Ring 0). Maps the kernel heap, VBE framebuffer, APIC registers, and kernel structures. This region is shared directly by reference across all tasks.

---

## 3. Concurrency Protection (`vmm_lock`)

Because page tables can be updated concurrently across multiple CPU cores (e.g., during memory mappings, forks, or page fault resolutions), the VMM is protected by the `vmm_lock` spinlock:

```c
static spinlock_t vmm_lock;
```

Any modification to page tables (via `vmm_map`, `vmm_map_in_space`, `vmm_clone_address_space`, `vmm_create_address_space`, or `vmm_destroy_address_space`) must acquire `vmm_lock` using `spin_lock_irqsave` to prevent concurrent modifications and deadlocks.

---

## 4. Address Space Management

### Creation (`vmm_create_address_space`)
Allocates a physical frame for a new PML4 table, clears it, and copies the shared entries under `vmm_lock` protection:
- `pml4[0] = kernel_pml4[0]` (Clones early identity mapping).
- `pml4[256..511] = kernel_pml4[256..511]` (Clones kernel mappings).

### Switching (`vmm_switch_address_space`)
Loads the physical address of the PML4 into the CPU `CR3` register, which flushes the processor TLB:
```assembly
mov %rax, %cr3
```

### Destruction (`vmm_destroy_address_space`)
Recursively traverses the user space region (index 1 to 255) of the target PML4:
- Free all Page Tables (PTs) marked present and user.
- Free all Page Directories (PDs) marked present and user.
- Free all Page Directory Pointer Tables (PDPTs).
- Free the PML4 frame itself.
- All traversals and freeing are serialized under `vmm_lock`.

---

## 5. Page Fault Handling & Copy-On-Write Integration

The VMM intercepts Page Fault exceptions (Interrupt `0x0E` or Vetor 14) via `vmm_page_fault_handler()`. The handler is responsible for resolving write-protection faults triggered by Copy-On-Write (COW) pages and terminating unauthorized memory access attempts.

### COW Fault Resolution Flow
1. **PTE Retrieval**: The handler reads the faulting virtual address from register `CR2` and traverses the 4-level paging tables (`PML4` → `PDPT` → `PD` → `PT`) to find the target Page Table Entry (PTE).
2. **COW Validation**: A fault is recognized as a COW event if:
   - The CPU exception code indicates a write access (`error_code & 0x02` is true).
   - The PTE is present and has the `PAGE_COW` flag set.
3. **Reference Count Evaluation**:
   - If the physical frame reference count is greater than 1 (shared page), a new physical frame is allocated. The original page's contents are copied to this new frame, the PTE is updated to point to the new frame, the `PAGE_COW` flag is cleared, and the `PAGE_WRITABLE` flag is restored. Finally, the old frame's reference count is decremented.
   - If the reference count is exactly 1 (exclusive page), no allocation or copying takes place. The `PAGE_COW` flag is cleared directly in-place, and `PAGE_WRITABLE` is set.
4. **TLB Invalidation**: The TLB entry for the faulting page is flushed using `invlpg` to make the new permissions active immediately.

### Fault Isolation (Ring 3 vs. Ring 0)
If the page fault cannot be resolved (e.g., standard null-pointer dereference, or out-of-memory during COW allocation):
- **Ring 3 Fault**: If the fault originated in User Mode (`(cs & 0x3) == 0x3`), the kernel terminates the offending task immediately via `scheduler_exit_current(-1)`. This prevents user-mode bugs from crashing the operating system.
- **Ring 0 Fault**: If the fault occurred in Kernel Mode, a critical failure is assumed, and the kernel enters a panic state (`KERNEL PANIC`).

