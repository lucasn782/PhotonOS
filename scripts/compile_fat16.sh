#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"

echo "======================================"
echo "FAT16 Compilation Test"
echo "======================================"
echo ""

echo "[*] Compiling fat16.c..."
gcc -ffreestanding -m64 -nostdlib -mno-red-zone -fno-pic -fno-pie \
    -fno-stack-protector -Wall -Wextra -c fat16.c -o fat16.o 2>&1

if [ $? -eq 0 ]; then
    echo ""
    echo "[✓] SUCCESS: fat16.c compiled without errors!"
    echo ""
    ls -lh fat16.o
    echo ""
    echo "File details:"
    file fat16.o
else
    echo ""
    echo "[✗] COMPILATION FAILED"
    exit 1
fi

echo ""
echo "======================================"
echo "Compilation Test Complete"
echo "======================================"
