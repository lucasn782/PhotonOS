#!/bin/bash
rm -f boot1_serial.log boot2_serial.log

echo "[BOOT #1] Gravando arquivo persistente em FAT16..."
(
    sleep 3.5
    echo 'sendkey p'
    echo 'sendkey e'
    echo 'sendkey r'
    echo 'sendkey s'
    echo 'sendkey i'
    echo 'sendkey s'
    echo 'sendkey t'
    echo 'sendkey 1'
    echo 'sendkey ret'
    sleep 3
    echo 'quit'
) | timeout 12s qemu-system-x86_64 \
    -smp 4 \
    -serial file:boot1_serial.log \
    -drive format=raw,file=build/photon.img \
    -drive format=raw,file=build/disk.img \
    -display none \
    -monitor stdio >/dev/null 2>&1 || true

echo "=== BOOT #1 LOG ==="
cat boot1_serial.log

echo "[BOOT #2] Lendo arquivo apos REBOOT..."
(
    sleep 3.5
    echo 'sendkey p'
    echo 'sendkey e'
    echo 'sendkey r'
    echo 'sendkey s'
    echo 'sendkey i'
    echo 'sendkey s'
    echo 'sendkey t'
    echo 'sendkey 2'
    echo 'sendkey ret'
    sleep 3
    echo 'sendkey v'
    echo 'sendkey f'
    echo 'sendkey s'
    echo 'sendkey t'
    echo 'sendkey e'
    echo 'sendkey s'
    echo 'sendkey t'
    echo 'sendkey ret'
    sleep 3
    echo 'quit'
) | timeout 15s qemu-system-x86_64 \
    -smp 4 \
    -serial file:boot2_serial.log \
    -drive format=raw,file=build/photon.img \
    -drive format=raw,file=build/disk.img \
    -display none \
    -monitor stdio >/dev/null 2>&1 || true

echo "=== BOOT #2 LOG ==="
cat boot2_serial.log
