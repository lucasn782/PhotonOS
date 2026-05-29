#!/usr/bin/env bash
set -euo pipefail

TAP_NAME=${TAP_NAME:-tap0}
HOST_IP_CIDR=${HOST_IP_CIDR:-10.0.2.1/24}
GUEST_IP=${GUEST_IP:-10.0.2.15}
BOOT_WAIT_SECONDS=${BOOT_WAIT_SECONDS:-4}
LOG_DIR=${LOG_DIR:-logs}
PCAP_FILE=${PCAP_FILE:-$LOG_DIR/net_test.pcap}
SERIAL_LOG=${SERIAL_LOG:-$LOG_DIR/net_test_serial.log}
QEMU_PID=""

fail() {
    echo "ERRO: $*" >&2
    exit 1
}

need_cmd() {
    command -v "$1" >/dev/null 2>&1 || fail "comando obrigatorio nao encontrado: $1"
}

cleanup() {
    local status=$?

    if [[ -n "$QEMU_PID" ]] && kill -0 "$QEMU_PID" >/dev/null 2>&1; then
        kill "$QEMU_PID" >/dev/null 2>&1 || true
        wait "$QEMU_PID" >/dev/null 2>&1 || true
    fi

    if ip link show "$TAP_NAME" >/dev/null 2>&1; then
        ip link set "$TAP_NAME" down >/dev/null 2>&1 || true
        ip tuntap del dev "$TAP_NAME" mode tap >/dev/null 2>&1 || true
    fi

    exit "$status"
}

print_result() {
    local label=$1
    local status=$2

    if [[ "$status" -eq 0 ]]; then
        echo "[OK]   $label"
    else
        echo "[FAIL] $label"
    fi
}

if [[ "${EUID:-$(id -u)}" -ne 0 ]]; then
    fail "execute com sudo: sudo bash scripts/test_network.sh"
fi

need_cmd ip
need_cmd qemu-system-x86_64
need_cmd arping
need_cmd ping

[[ -f build/photon.img ]] || fail "build/photon.img nao encontrado. Execute make antes do teste."

mkdir -p "$LOG_DIR"
trap cleanup EXIT INT TERM

if ip link show "$TAP_NAME" >/dev/null 2>&1; then
    ip link set "$TAP_NAME" down >/dev/null 2>&1 || true
    ip tuntap del dev "$TAP_NAME" mode tap >/dev/null 2>&1 || true
fi

echo "NET-TEST: criando interface $TAP_NAME..."
ip tuntap add dev "$TAP_NAME" mode tap
ip addr add "$HOST_IP_CIDR" dev "$TAP_NAME"
ip link set "$TAP_NAME" up

qemu_drives=(
    -drive file=build/photon.img,format=raw,index=0,media=disk
)

if [[ -f build/disk.img ]]; then
    qemu_drives+=(
        -drive file=build/disk.img,format=raw,index=1,media=disk
    )
else
    echo "NET-TEST: build/disk.img nao encontrado; iniciando sem disco secundario."
fi

echo "NET-TEST: iniciando QEMU headless com captura em $PCAP_FILE..."
qemu-system-x86_64 \
    -display none \
    -no-reboot \
    "${qemu_drives[@]}" \
    -netdev tap,id=net0,ifname="$TAP_NAME",script=no,downscript=no \
    -device e1000,netdev=net0 \
    -object filter-dump,id=netdump,netdev=net0,file="$PCAP_FILE" \
    -serial "file:$SERIAL_LOG" &

QEMU_PID=$!

echo "NET-TEST: aguardando boot do PhotonOS por ${BOOT_WAIT_SECONDS}s..."
sleep "$BOOT_WAIT_SECONDS"

arp_status=1
icmp_status=1

echo "NET-TEST: validando ARP para $GUEST_IP..."
if arping -c 2 -I "$TAP_NAME" "$GUEST_IP"; then
    arp_status=0
fi

echo "NET-TEST: validando ICMP Echo para $GUEST_IP..."
if ping -c 3 -I "$TAP_NAME" "$GUEST_IP"; then
    icmp_status=0
fi

echo
echo "=================================================="
echo " Resultado da Esteira Automatizada de Rede"
echo "=================================================="
print_result "ARP reply recebido de $GUEST_IP" "$arp_status"
print_result "ICMP Echo Reply recebido de $GUEST_IP" "$icmp_status"
echo "PCAP:   $PCAP_FILE"
echo "Serial: $SERIAL_LOG"
echo "=================================================="

if [[ "$arp_status" -ne 0 || "$icmp_status" -ne 0 ]]; then
    exit 1
fi
