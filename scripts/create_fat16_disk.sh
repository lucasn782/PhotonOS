#!/usr/bin/env bash
set -euo pipefail

DISK_IMG=${DISK_IMG:-disk.img}
USER_DIR=${USER_DIR:-build/user}
MOUNT_DIR=${MOUNT_DIR:-/tmp/photon_mnt}

copy_with_mtools() {
    command -v mcopy >/dev/null 2>&1 || return 1

    mcopy -i "$DISK_IMG" -o "$USER_DIR/shell.elf" ::shell || return 1
    mcopy -i "$DISK_IMG" -o "$USER_DIR/hello.elf" ::hello || return 1
    mcopy -i "$DISK_IMG" -o "$USER_DIR/upper.elf" ::upper || return 1
    mcopy -i "$DISK_IMG" -o "$USER_DIR/rev.elf" ::rev || return 1
    mcopy -i "$DISK_IMG" -o "$USER_DIR/hang.elf" ::hang || return 1
    mcopy -i "$DISK_IMG" -o "$USER_DIR/spin.elf" ::spin || return 1
    mcopy -i "$DISK_IMG" -o "$USER_DIR/ping.elf" ::ping || return 1
}

copy_with_mount() {
    command -v sudo >/dev/null 2>&1 || return 1

    mkdir -p "$MOUNT_DIR"
    sudo mount -o loop "$DISK_IMG" "$MOUNT_DIR" || return 1
    trap 'sudo umount "$MOUNT_DIR" >/dev/null 2>&1 || true' EXIT

    if ! sudo cp "$USER_DIR/shell.elf" "$MOUNT_DIR/shell" ||
        ! sudo cp "$USER_DIR/hello.elf" "$MOUNT_DIR/hello" ||
        ! sudo cp "$USER_DIR/upper.elf" "$MOUNT_DIR/upper" ||
        ! sudo cp "$USER_DIR/rev.elf" "$MOUNT_DIR/rev" ||
        ! sudo cp "$USER_DIR/hang.elf" "$MOUNT_DIR/hang" ||
        ! sudo cp "$USER_DIR/spin.elf" "$MOUNT_DIR/spin" ||
        ! sudo cp "$USER_DIR/ping.elf" "$MOUNT_DIR/ping"; then
        sudo umount "$MOUNT_DIR" >/dev/null 2>&1 || true
        trap - EXIT
        return 1
    fi

    sudo umount "$MOUNT_DIR"
    trap - EXIT
}

create_with_mkfs() {
    command -v mkfs.vfat >/dev/null 2>&1 || return 1

    dd if=/dev/zero of="$DISK_IMG" bs=1M count=16 status=none
    mkfs.vfat -F 16 "$DISK_IMG" >/dev/null

    copy_with_mtools || copy_with_mount || return 1
}

create_with_python() {
    command -v python3 >/dev/null 2>&1 || return 1

    python3 - "$DISK_IMG" \
        "$USER_DIR/shell.elf":shell \
        "$USER_DIR/hello.elf":hello \
        "$USER_DIR/upper.elf":upper \
        "$USER_DIR/rev.elf":rev \
        "$USER_DIR/hang.elf":hang \
        "$USER_DIR/spin.elf":spin \
        "$USER_DIR/ping.elf":ping <<'PY'
import math
import struct
import sys

image = sys.argv[1]
files = []
for spec in sys.argv[2:]:
    src, dst = spec.split(":", 1)
    with open(src, "rb") as handle:
        files.append((dst, handle.read()))

files.append(("info.txt", b"FAT16 driver test: file read is working perfectly!\n"))

sector_size = 512
total_sectors = 16 * 1024 * 1024 // sector_size
sectors_per_cluster = 2
reserved_sectors = 1
fat_count = 2
root_entry_count = 512
sectors_per_fat = 64
root_dir_sectors = (root_entry_count * 32 + sector_size - 1) // sector_size
root_lba = reserved_sectors + fat_count * sectors_per_fat
data_lba = root_lba + root_dir_sectors
cluster_size = sector_size * sectors_per_cluster

disk = bytearray(total_sectors * sector_size)

boot = bytearray(sector_size)
boot[0:3] = b"\xEB\x3C\x90"
boot[3:11] = b"PHOTONOS"
struct.pack_into("<H", boot, 11, sector_size)
boot[13] = sectors_per_cluster
struct.pack_into("<H", boot, 14, reserved_sectors)
boot[16] = fat_count
struct.pack_into("<H", boot, 17, root_entry_count)
struct.pack_into("<H", boot, 19, total_sectors)
boot[21] = 0xF8
struct.pack_into("<H", boot, 22, sectors_per_fat)
struct.pack_into("<H", boot, 24, 63)
struct.pack_into("<H", boot, 26, 16)
struct.pack_into("<I", boot, 28, 0)
struct.pack_into("<I", boot, 32, 0)
boot[36] = 0x80
boot[38] = 0x29
struct.pack_into("<I", boot, 39, 0x50484F54)
boot[43:54] = b"PHOTONOS   "
boot[54:62] = b"FAT16   "
boot[510:512] = b"\x55\xAA"
disk[0:sector_size] = boot

fat_entries = [0] * ((sectors_per_fat * sector_size) // 2)
fat_entries[0] = 0xFFF8
fat_entries[1] = 0xFFFF

root = bytearray(root_dir_sectors * sector_size)
next_cluster = 2

def fat_name(name):
    if "." in name:
        base, ext = name.split(".", 1)
    else:
        base, ext = name, ""
    return base.upper().encode("ascii")[:8].ljust(8, b" ") + \
        ext.upper().encode("ascii")[:3].ljust(3, b" ")

for index, (name, data) in enumerate(files):
    clusters = max(1, math.ceil(len(data) / cluster_size))
    first_cluster = next_cluster
    for i in range(clusters):
        cluster = next_cluster + i
        fat_entries[cluster] = 0xFFFF if i == clusters - 1 else cluster + 1
        lba = data_lba + (cluster - 2) * sectors_per_cluster
        start = lba * sector_size
        chunk = data[i * cluster_size:(i + 1) * cluster_size]
        disk[start:start + len(chunk)] = chunk
    next_cluster += clusters

    entry = index * 32
    root[entry:entry + 11] = fat_name(name)
    root[entry + 11] = 0x20
    struct.pack_into("<H", root, entry + 26, first_cluster)
    struct.pack_into("<I", root, entry + 28, len(data))

for fat_index in range(fat_count):
    fat_offset = (reserved_sectors + fat_index * sectors_per_fat) * sector_size
    for entry_index, value in enumerate(fat_entries):
        struct.pack_into("<H", disk, fat_offset + entry_index * 2, value)

root_offset = root_lba * sector_size
disk[root_offset:root_offset + len(root)] = root

with open(image, "wb") as handle:
    handle.write(disk)
PY
}

if ! create_with_mkfs; then
    create_with_python
fi
