# TLB & TLB Shootdown

This document specifies translation caching, the Local APIC IPI-based TLB Shootdown protocol (Vector 0x79), synchronization variables, and single-core optimizations.

---

## 1. Local TLB Invalidation

The Translation Lookaside Buffer (TLB) caches virtual-to-physical address mappings. When page table entries (PTEs) are modified, the CPU's local TLB cache becomes out of date.

PhotonOS performs local TLB invalidation in two ways:
- **Single Page Invalidation (`vmm_flush_tlb`)**: Executes the `invlpg` instruction to flush a single virtual page mapping:
  ```c
  static inline void vmm_flush_tlb(uintptr_t addr)
  {
      __asm__ volatile ("invlpg (%0)" : : "r"(addr) : "memory");
  }
  ```
- **Complete Invalidation**: Reloading the `CR3` register (during address space switches or shootdowns) flushes the entire TLB cache of the active core:
  ```c
  __asm__ volatile ("mov %0, %%cr3" : : "r"(pml4) : "memory");
  ```

---

## 2. Multi-Core Consistency (TLB Shootdown)

Under Multiprocessing (SMP), multiple cores share the userspace page tables. When a page's permissions are downgraded (e.g., during `sys_fork` where writing is removed and COW is set), other active cores must discard their cached writable translations immediately.

PhotonOS coordinates this via the **TLB Shootdown Protocol**:

1. **Serialization**: The initiator acquires `vmm_lock` to protect the shootdown state.
2. **Setup**: The initiator sets the target address and resets the global acknowledgment counter:
   ```c
   tlb_shootdown_addr = virt;
   tlb_acknowledge_count = 0;
   ```
3. **IPI Broadcast**: The initiator issues a Local APIC Interrupt Command Register (ICR) write to dispatch Vector `0x79` with the "All Excluding Self" shorthand:
   ```c
   apic_write(0x300, 0x000C0000 | 0x79);
   ```
4. **Barrier Wait**: The initiator polls the global counter until all other active APs have processed the shootdown:
   ```c
   while (tlb_acknowledge_count < active_aps) {
       __asm__ volatile ("" : : : "memory");
   }
   ```
5. **AP Acknowledgment**: When receiving Vector `0x79`, secondary cores execute `smp_tlb_shootdown_handler()`:
   - Reload `CR3` to flush their entire TLB.
   - Increment `tlb_acknowledge_count` atomically.
   - Send EOI to the Local APIC.

---

## 3. Single-Core Optimization

To avoid deadlocks and unnecessary wait-loops during single-core execution:
- If `smp_ap_booted_count() == 0`, the TLB shootdown sequence (IPI broadcast and wait-loop) is bypassed entirely in `vmm_clone_address_space`.
- The local core only runs `vmm_flush_tlb(virt)` locally.
