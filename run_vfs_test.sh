#!/bin/bash
(
    sleep 3
    echo "vfstest"
    sleep 2
    echo "ping 127.0.0.1"
    sleep 4
    echo "forktest"
    sleep 2
) | timeout 15s qemu-system-x86_64 \
    -smp 4 \
    -vga std \
    -display vnc=:1 \
    -serial stdio \
    -drive format=raw,file=build/photon.img \
    -drive format=raw,file=build/disk.img \
    -monitor null 2>&1
