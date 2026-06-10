#!/usr/bin/env bash
set -euo pipefail

make clean
make
make fat16-disk

(
    sleep 2
    echo 'sendkey h'
    sleep 0.1
    echo 'sendkey a'
    sleep 0.1
    echo 'sendkey n'
    sleep 0.1
    echo 'sendkey g'
    sleep 0.1
    echo 'sendkey spc'
    sleep 0.1
    echo 'sendkey shift-7'
    sleep 0.1
    echo 'sendkey ret'
    sleep 1
    echo 'sendkey p'
    sleep 0.1
    echo 'sendkey s'
    sleep 0.1
    echo 'sendkey ret'
    sleep 2
    echo 'sendkey ctrl-c'
    sleep 1
    echo 'sendkey p'
    sleep 0.1
    echo 'sendkey s'
    sleep 0.1
    echo 'sendkey ret'
    sleep 2
    echo 'quit'
) | qemu-system-x86_64 \
    -drive format=raw,file=build/photon.img,if=floppy \
    -drive format=raw,file=build/disk.img,if=ide,index=0,media=disk \
    -boot a \
    -display none \
    -serial file:background_test_serial.log \
    -monitor stdio
