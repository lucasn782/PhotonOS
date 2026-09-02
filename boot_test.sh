#!/bin/bash
for i in $(seq 1 10); do
    echo "=== BOOT $i ==="
    timeout 6s qemu-system-x86_64 -nographic -smp 4 -drive format=raw,file=build/photon.img -display none 2>&1 | grep -iE 'BOOT:|PMM|VMM|HEAP|VFS|APIC|SMP|Syscall|ELF|Scheduler|NET:|TCP|FAIL|PASS|Panic|FAULT|fault|Triple|Double|reset|Shell|shell|MOUSE' | head -35
    echo ""
done
