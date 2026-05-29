#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

echo "[*] Starting QEMU FAT16 Baseline Validation..."
echo "[*] Disk images check..."

if [ ! -f "photon.img" ]; then
    echo "[!] Error: photon.img not found"
    exit 1
fi

if [ ! -f "disk.img" ]; then
    echo "[*] Creating FAT16 disk image..."
    bash create_fat16_disk.sh
fi

echo "[*] Running headless QEMU test (2 minute timeout)..."

# Create test commands for the monitor
TEST_SCRIPT=$(mktemp)
trap "rm -f $TEST_SCRIPT" EXIT

cat > "$TEST_SCRIPT" << 'EOF'
sleep 2
echo 'Testing shell startup via key injection...'
sendkey h
sendkey a
sendkey n
sendkey g
sendkey spc
sendkey shift-7
sendkey ret
sleep 1
echo 'Testing ps command...'
sendkey p
sendkey s
sendkey ret
sleep 2
quit
EOF

# Run QEMU with monitor input
timeout 120 bash -c "cat $TEST_SCRIPT | qemu-system-x86_64 \
    -drive format=raw,file=photon.img \
    -hda disk.img \
    -display none \
    -serial file:fat16_baseline_serial.log \
    -monitor stdio 2>&1" || true

echo ""
echo "[*] QEMU Test Complete. Serial Log Output:"
echo "=========================================="
if [ -f fat16_baseline_serial.log ]; then
    tail -100 fat16_baseline_serial.log || cat fat16_baseline_serial.log
else
    echo "[!] Warning: fat16_baseline_serial.log not found"
fi
echo "=========================================="
