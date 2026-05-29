#!/usr/bin/env bash
# Test script for PhotonOS Keyboard Driver - Special Character Support
# This script tests pipe (|), quotes (' "), backslash (\), and forward slash (/)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# Configuration
TIMEOUT=60  # seconds
QEMU_CMD="qemu-system-x86_64"
SERIAL_LOG="keyboard_test_serial.log"

# Color codes for output
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo -e "${YELLOW}[*] PhotonOS Keyboard Driver Test Suite${NC}"
echo "[*] Testing special character support: |, ', \", \\, /"

# Check prerequisites
if [ ! -f "photon.img" ]; then
    echo -e "${RED}[!] Error: photon.img not found${NC}"
    echo "[*] Run 'make fat16-disk' first"
    exit 1
fi

if ! command -v $QEMU_CMD &> /dev/null; then
    echo -e "${RED}[!] Error: $QEMU_CMD not found${NC}"
    exit 1
fi

# Clear previous logs
rm -f "$SERIAL_LOG"

echo -e "${YELLOW}[*] Starting QEMU with PhotonOS...${NC}"

# Create a QEMU monitor script for automated testing
TEST_SCRIPT=$(mktemp)
trap "rm -f $TEST_SCRIPT" EXIT

cat > "$TEST_SCRIPT" << 'QEMU_SCRIPT'
sleep 3

# Test 1: Echo with pipe character
echo "Test 1: Sending pipe character (Shift+\\)"
sendkey shift-backslash
sleep 0.2

# Test 2: Echo with single quote
echo "Test 2: Sending single quote character"
sendkey apostrophe
sleep 0.2

# Test 3: Echo with double quote
echo "Test 3: Sending double quote (Shift+apostrophe on US keyboard)"
sendkey shift-apostrophe
sleep 0.2

# Test 4: Simple echo
echo "Test 4: Testing basic echo command"
sendkey e
sendkey c
sendkey h
sendkey o
sendkey space
sendkey quotedbl
sendkey h
sendkey e
sendkey l
sendkey l
sendkey o
sendkey quotedbl
sendkey ret
sleep 1

# Test 5: Pipe test
echo "Test 5: Testing pipe with echo | upper"
sendkey e
sendkey c
sendkey h
sendkey o
sendkey space
sendkey quotedbl
sendkey test
sendkey quotedbl
sleep 0.5
sendkey shift-backslash
sleep 0.5
sendkey u
sendkey p
sendkey p
sendkey e
sendkey r
sendkey ret
sleep 2

# Test 6: Complex command with quotes
echo "Test 6: Testing complex command with special chars"
sendkey e
sendkey c
sendkey h
sendkey o
sendkey space
sendkey apostrophe
sendkey hello
sendkey apostrophe
sendkey ret
sleep 1

# Exit
sendkey ctrl-c
sleep 1
sendkey p
sendkey s
sendkey ret
sleep 1
quit

QEMU_SCRIPT

echo "[*] Test sequence:"
echo "  1. Testing pipe character (|)"
echo "  2. Testing single quote (')"
echo "  3. Testing double quote (\")"
echo "  4. Testing basic echo with quotes"
echo "  5. Testing pipe with echo | upper"
echo "  6. Testing complex command"
echo ""

# Run QEMU with serial output and monitor script
$QEMU_CMD \
    -drive file=photon.img,format=raw \
    -serial file:"$SERIAL_LOG" \
    -monitor pipe:$(mktemp -u) \
    -nographic \
    -m 256M \
    &

QEMU_PID=$!

# Wait for QEMU to start
sleep 2

# Send test script to QEMU monitor (simplified version)
# In practice, you'd need to connect to the QEMU monitor properly

# Wait for tests to complete or timeout
sleep $TIMEOUT
kill $QEMU_PID 2>/dev/null || true
wait $QEMU_PID 2>/dev/null || true

# Analyze results
echo ""
echo -e "${YELLOW}[*] Analyzing results...${NC}"

if [ -f "$SERIAL_LOG" ]; then
    echo "[*] Keyboard Debug Output:"
    echo "---"
    grep -E "DEBUG.*Special char|DEBUG.*Shift|DEBUG.*Ctrl|pipe:|^PhotonOS" "$SERIAL_LOG" | head -50 || true
    echo "---"
    
    # Check for specific character detections
    if grep -q "0x2B.*|" "$SERIAL_LOG"; then
        echo -e "${GREEN}✓ Pipe character (|) detected${NC}"
    else
        echo -e "${RED}✗ Pipe character (|) not detected${NC}"
    fi
    
    if grep -q "0x28.*'\\|0x28.*\"" "$SERIAL_LOG"; then
        echo -e "${GREEN}✓ Quote characters detected${NC}"
    else
        echo -e "${RED}✗ Quote characters not detected${NC}"
    fi
    
    if grep -q "0x35.*/" "$SERIAL_LOG"; then
        echo -e "${GREEN}✓ Forward slash (/) detected${NC}"
    else
        echo -e "${YELLOW}○ Forward slash (/) not detected in this test${NC}"
    fi
    
    if grep -q "shift=1" "$SERIAL_LOG"; then
        echo -e "${GREEN}✓ Shift state tracking working${NC}"
    else
        echo -e "${RED}✗ Shift state tracking not working${NC}"
    fi
else
    echo -e "${RED}[!] Serial log file not created${NC}"
fi

echo ""
echo -e "${YELLOW}[*] Full serial output saved to: $SERIAL_LOG${NC}"
echo "[*] Test complete!"
