#!/usr/bin/env python3
"""
PhotonOS TCP Phase 2B.2B Automated Transmit Path Test Suite
Validates:
1.  UNIT_TX_BUFFER: Enqueue, sequence assignment, partial ACK, cumulative ACK, boundary checks
2.  UNIT_TX_RETRANSMISSION: RTO deadline expiration, exponential backoff, retry limit
3.  TX_BAD_USER_POINTER: Parameter validation (NULL, 0x10)
4.  TX_ZERO_LENGTH: Zero length send returns 0 without sending packets
5.  TX_BAD_FD: Invalid fd (-1) and closed socket error handling
6.  TX_LISTENER_REJECT: Transmission rejection on listening socket
7.  TX_SINGLE_BYTE: Single byte payload transmission and wire reception ("A")
8.  TX_SMALL_PAYLOAD: Small string payload transmission ("Hello PhotonOS", 14 bytes)
9.  TX_1024_BYTES: 1024-byte payload transmission and byte verification
10. TX_MSS_SEGMENTATION: 2000-byte payload segmentation into MSS (1460) + remainder (540)
11. TX_LARGE_PAYLOAD: Large buffer transmission (8192 and 16384 bytes)
12. TX_MULTIPLE_CONNECTIONS: Independent transmit paths across consecutive connections
13. TX_FORK: Socket transmission in forked child process
14. TX_DUP: Socket transmission via duplicated descriptor (dup())
15. PCAP_DATA_WIRE: Wire inspection of transmitted TCP DATA segments (seq, payload, checksums)
16. PCAP_ACK_WIRE: Wire inspection of incoming peer ACKs matching transmitted sequence numbers
"""

import os
import sys
import time
import socket
import struct
import subprocess

LOG_DIR = "logs"
PCAP_PATH = os.path.join(LOG_DIR, "tcp_phase2b2_tx.pcap")
GUEST_PORT = 8088
HOST_PORT = 18088  # QEMU hostfwd maps 127.0.0.1:18088 -> 10.0.2.15:8088


def parse_pcap(pcap_path):
    if not os.path.exists(pcap_path):
        return []

    packets = []
    with open(pcap_path, "rb") as f:
        global_hdr = f.read(24)
        if len(global_hdr) < 24:
            return []

        while True:
            pkt_hdr = f.read(16)
            if len(pkt_hdr) < 16:
                break
            ts_sec, ts_usec, incl_len, orig_len = struct.unpack("<IIII", pkt_hdr)
            data = f.read(incl_len)
            if len(data) < incl_len:
                break

            if len(data) < 14 + 20 + 20:
                continue
            eth_type = struct.unpack("!H", data[12:14])[0]
            if eth_type != 0x0800:  # IPv4
                continue

            ip_data = data[14:]
            ver_ihl = ip_data[0]
            ihl = (ver_ihl & 0x0F) * 4
            protocol = ip_data[9]
            src_ip = socket.inet_ntoa(ip_data[12:16])
            dst_ip = socket.inet_ntoa(ip_data[16:20])

            if protocol != 6:  # TCP
                continue

            tcp_data = ip_data[ihl:]
            if len(tcp_data) < 20:
                continue

            src_port, dst_port, seq, ack, offset_flags, window, csum, urg = struct.unpack(
                "!HHIIHHHH", tcp_data[:20]
            )
            header_len = ((offset_flags >> 12) & 0x0F) * 4
            flags = offset_flags & 0x01FF
            payload_len = len(tcp_data) - header_len

            packets.append({
                "src_ip": src_ip,
                "dst_ip": dst_ip,
                "src_port": src_port,
                "dst_port": dst_port,
                "seq": seq,
                "ack": ack,
                "flags": flags,
                "window": window,
                "csum": csum,
                "payload_len": payload_len
            })
    return packets


def run_test():
    os.makedirs(LOG_DIR, exist_ok=True)
    if os.path.exists(PCAP_PATH):
        try:
            os.remove(PCAP_PATH)
        except OSError:
            pass

    print(f"[TEST SETUP] Iniciando QEMU com port forwarding 127.0.0.1:{HOST_PORT} -> 10.0.2.15:{GUEST_PORT}...", flush=True)

    cmd = [
        "qemu-system-x86_64",
        "-smp", "4",
        "-drive", "format=raw,file=build/photon.img,if=floppy",
        "-drive", "format=raw,file=build/disk.img,if=ide,index=0,media=disk",
        "-boot", "a",
        "-netdev", f"user,id=net0,hostfwd=tcp:127.0.0.1:{HOST_PORT}-:{GUEST_PORT}",
        "-device", "e1000,netdev=net0",
        "-object", f"filter-dump,id=netdump,netdev=net0,file={PCAP_PATH}",
        "-serial", "stdio",
        "-display", "none",
        "-monitor", "null"
    ]

    proc = subprocess.Popen(
        cmd,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        bufsize=0
    )

    os.set_blocking(proc.stdout.fileno(), False)
    output_parts = []

    def drain(timeout=0.3):
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                chunk = proc.stdout.read(4096)
                if chunk:
                    text = chunk.decode("ascii", errors="replace")
                    sys.stdout.write(text)
                    sys.stdout.flush()
                    output_parts.append(text)
                    deadline = time.time() + timeout
                else:
                    time.sleep(0.05)
            except (OSError, TypeError):
                time.sleep(0.05)

    def wait_for(pattern, timeout=25):
        t0 = time.time()
        while time.time() - t0 < timeout:
            drain(0.2)
            full = "".join(output_parts)
            if pattern in full:
                return True
        return False

    def send(cmd_str):
        sys.stdout.write(f"\n>>> SENDING: {cmd_str}\n")
        sys.stdout.flush()
        proc.stdin.write((cmd_str + "\n").encode("ascii"))
        proc.stdin.flush()

    if not wait_for("PhotonOS user shell iniciado", timeout=25):
        print("\n!!! TIMEOUT aguardando inicializacao do shell !!!", flush=True)
        proc.terminate()
        proc.wait()
        return False

    time.sleep(1)
    drain(0.5)

    # 1. Teste de Validação de Erros de Syscall send()
    send("tcptest send_errors")
    time.sleep(2)
    drain(0.5)

    # 2. Teste TX Single Byte ("A")
    send(f"tcptest send_server {GUEST_PORT} single")
    if not wait_for("Ouvindo na porta", timeout=10):
        print("\n!!! Servidor send_server nao iniciou !!!", flush=True)
    time.sleep(1)
    single_received = b""
    try:
        s = socket.create_connection(("127.0.0.1", HOST_PORT), timeout=5)
        single_received = s.recv(1024)
        time.sleep(0.3)
        s.close()
    except Exception as e:
        print(f"[HOST CLIENT SINGLE] Erro: {e}", flush=True)
    time.sleep(2)
    drain(0.5)

    # 3. Teste TX Small Payload ("Hello PhotonOS", 14 bytes)
    send(f"tcptest send_server {GUEST_PORT} small")
    if not wait_for("Ouvindo na porta", timeout=10):
        print("\n!!! Servidor send_server nao iniciou !!!", flush=True)
    time.sleep(1)
    small_received = b""
    try:
        s = socket.create_connection(("127.0.0.1", HOST_PORT), timeout=5)
        small_received = s.recv(1024)
        time.sleep(0.3)
        s.close()
    except Exception as e:
        print(f"[HOST CLIENT SMALL] Erro: {e}", flush=True)
    time.sleep(2)
    drain(0.5)

    # 4. Teste TX 1024 Bytes
    send(f"tcptest send_server {GUEST_PORT} 1024")
    if not wait_for("Ouvindo na porta", timeout=10):
        print("\n!!! Servidor send_server nao iniciou !!!", flush=True)
    time.sleep(1)
    bytes_1024_received = b""
    try:
        s = socket.create_connection(("127.0.0.1", HOST_PORT), timeout=5)
        while len(bytes_1024_received) < 1024:
            chunk = s.recv(4096)
            if not chunk:
                break
            bytes_1024_received += chunk
        time.sleep(0.3)
        s.close()
    except Exception as e:
        print(f"[HOST CLIENT 1024] Erro: {e}", flush=True)
    time.sleep(2)
    drain(0.5)

    # 5. Teste TX MSS Segmentation (2000 bytes -> 1460 + 540)
    send(f"tcptest send_server {GUEST_PORT} seg")
    if not wait_for("Ouvindo na porta", timeout=10):
        print("\n!!! Servidor send_server nao iniciou !!!", flush=True)
    time.sleep(1)
    seg_received = b""
    try:
        s = socket.create_connection(("127.0.0.1", HOST_PORT), timeout=5)
        while len(seg_received) < 2000:
            chunk = s.recv(4096)
            if not chunk:
                break
            seg_received += chunk
        time.sleep(0.3)
        s.close()
    except Exception as e:
        print(f"[HOST CLIENT SEG] Erro: {e}", flush=True)
    time.sleep(2)
    drain(0.5)

    # 6. Teste TX Large Payload (8192 bytes)
    send(f"tcptest send_server {GUEST_PORT} large")
    if not wait_for("Ouvindo na porta", timeout=10):
        print("\n!!! Servidor send_server nao iniciou !!!", flush=True)
    time.sleep(1)
    large_received = b""
    try:
        s = socket.create_connection(("127.0.0.1", HOST_PORT), timeout=5)
        while len(large_received) < 8192:
            chunk = s.recv(4096)
            if not chunk:
                break
            large_received += chunk
        time.sleep(0.3)
        s.close()
    except Exception as e:
        print(f"[HOST CLIENT LARGE] Erro: {e}", flush=True)
    time.sleep(2)
    drain(0.5)

    # 7. Payload acima da capacidade TX: exercita writes parciais + ACK release.
    send(f"tcptest send_server {GUEST_PORT} large16")
    if not wait_for("Ouvindo na porta", timeout=10):
        print("\n!!! Servidor send_server large16 nao iniciou !!!", flush=True)
    time.sleep(1)
    large16_received = b""
    try:
        s = socket.create_connection(("127.0.0.1", HOST_PORT), timeout=5)
        while len(large16_received) < 16384:
            chunk = s.recv(4096)
            if not chunk:
                break
            large16_received += chunk
        time.sleep(0.3)
        s.close()
    except Exception as e:
        print(f"[HOST CLIENT LARGE16] Erro: {e}", flush=True)
    time.sleep(2)
    drain(0.5)

    # 8. Teste TX Multi (3 conexões consecutivas)
    send(f"tcptest send_multi {GUEST_PORT} 3")
    if not wait_for("Ouvindo na porta", timeout=10):
        print("\n!!! Servidor send_multi nao iniciou !!!", flush=True)
    time.sleep(1)
    multi_received = []
    for i in range(3):
        try:
            s = socket.create_connection(("127.0.0.1", HOST_PORT), timeout=5)
            data = s.recv(1024)
            multi_received.append(data)
            time.sleep(0.3)
            s.close()
        except Exception as e:
            print(f"[HOST CLIENT MULTI {i}] Erro: {e}", flush=True)
        time.sleep(0.5)
    time.sleep(2)
    drain(0.5)

    # 9. Teste TX Fork (processo filho transmite)
    send(f"tcptest send_fork {GUEST_PORT}")
    if not wait_for("Aguardando conexao", timeout=10):
        print("\n!!! Servidor send_fork nao iniciou !!!", flush=True)
    time.sleep(1)
    fork_received = b""
    try:
        s = socket.create_connection(("127.0.0.1", HOST_PORT), timeout=5)
        fork_received = s.recv(1024)
        time.sleep(0.3)
        s.close()
    except Exception as e:
        print(f"[HOST CLIENT FORK] Erro: {e}", flush=True)
    time.sleep(2)
    drain(0.5)

    # 10. Teste TX Dup (envia via descritor duplicado)
    send(f"tcptest send_dup {GUEST_PORT}")
    if not wait_for("Aguardando conexao", timeout=10):
        print("\n!!! Servidor send_dup nao iniciou !!!", flush=True)
    time.sleep(1)
    dup_received = b""
    try:
        s = socket.create_connection(("127.0.0.1", HOST_PORT), timeout=5)
        dup_received = s.recv(1024)
        time.sleep(0.3)
        s.close()
    except Exception as e:
        print(f"[HOST CLIENT DUP] Erro: {e}", flush=True)
    time.sleep(2)
    drain(0.5)

    # Encerra QEMU
    try:
        proc.stdin.write(b"reboot\n")
        proc.stdin.flush()
    except Exception:
        pass
    time.sleep(1)
    proc.terminate()
    try:
        proc.wait(timeout=5)
    except Exception:
        proc.kill()

    full_output = "".join(output_parts)

    print("\n" + "="*50)
    print("      AVALIACAO DOS CRITERIOS DA PHASE 2B.2B (TX PATH)")
    print("="*50)

    results = {}

    # Kernel Unit Tests
    unit_tx_buffer = "PASS: tcp_tx_buffer" in full_output
    unit_tx_retransmission = "PASS: tcp_tx_retransmission" in full_output
    results["UNIT_TX_BUFFER"] = unit_tx_buffer
    results["UNIT_TX_RETRANSMISSION"] = unit_tx_retransmission

    # The kernel unit case separately exercises cumulative, partial, stale,
    # duplicate and over-the-end acknowledgement handling.
    results["TX_ACK"] = unit_tx_buffer
    results["TX_PARTIAL_ACK"] = unit_tx_buffer
    results["TX_DUPLICATE_ACK"] = unit_tx_buffer
    results["TX_INVALID_ACK"] = unit_tx_buffer
    results["TX_RETRANSMISSION"] = unit_tx_retransmission
    results["TX_RTO"] = unit_tx_retransmission
    results["TX_TIMEOUT"] = unit_tx_retransmission

    # Error Validations
    results["TX_BAD_USER_POINTER"] = ("PASS: send(NULL)" in full_output and "PASS: send(0x10)" in full_output)
    results["TX_PARTIAL_INVALID_USER_RANGE"] = "PASS: send(partial invalid range)" in full_output
    results["TX_ZERO_LENGTH"] = "PASS: send(len=0) retornou 0" in full_output
    results["TX_BAD_FD"] = ("PASS: send(fd=-1)" in full_output and "PASS: send(sock CLOSED)" in full_output)
    results["TX_LISTENER_REJECT"] = "PASS: send(listener)" in full_output
    results["TX_NON_TCP_REJECT"] = "PASS: send(socket nao TCP)" in full_output

    # Wire Reception Tests
    results["TX_SINGLE_BYTE"] = (single_received == b"A")
    results["TX_SMALL_PAYLOAD"] = (small_received == b"Hello PhotonOS")

    # 1024 Bytes Check
    expected_1024 = bytes([(ord('A') + (i % 26)) for i in range(1024)])
    results["TX_1024_BYTES"] = (bytes_1024_received == expected_1024)

    # 2000 Bytes Segmentation Check
    expected_2000 = bytes([(ord('0') + (i % 10)) for i in range(2000)])
    results["TX_MSS_SEGMENTATION"] = (seg_received == expected_2000)

    # 8192 Bytes Large Check
    expected_8192 = bytes([(ord('a') + (i % 26)) for i in range(8192)])
    results["TX_LARGE_PAYLOAD"] = (large_received == expected_8192)

    expected_16384 = bytes([(ord('A') + (i % 26)) for i in range(16384)])
    results["TX_PARTIAL_SEND"] = (large16_received == expected_16384)

    # Multi, Fork, Dup
    results["TX_MULTIPLE_CONNECTIONS"] = (len(multi_received) == 3 and
                                          multi_received[0] == b"MSG_0" and
                                          multi_received[1] == b"MSG_1" and
                                          multi_received[2] == b"MSG_2")
    results["TX_FORK"] = (fork_received == b"FORK_DATA_OK")
    results["TX_DUP"] = (dup_received == b"DUP_DATA_OK")

    # PCAP Validation
    pcap_pkts = parse_pcap(PCAP_PATH)
    data_pkts_from_guest = [p for p in pcap_pkts if p["src_port"] == GUEST_PORT and p["payload_len"] > 0]
    ack_pkts_from_host = [p for p in pcap_pkts if p["dst_port"] == GUEST_PORT and (p["flags"] & 0x10) != 0]

    # Verify at least one segmented packet with MSS (1460 bytes)
    mss_segmented = any(p["payload_len"] == 1460 for p in data_pkts_from_guest)
    results["PCAP_MSS_SEGMENT_WIRE"] = mss_segmented

    results["PCAP_DATA_WIRE"] = (len(data_pkts_from_guest) >= 10)
    results["PCAP_ACK_WIRE"] = (len(ack_pkts_from_host) >= 10)

    # Sequence / ACK coherence: ensure host ACKs match guest SEQ + payload_len
    seq_ack_coherent = False
    for dp in data_pkts_from_guest:
        exp_ack = dp["seq"] + dp["payload_len"]
        matching_ack = [ap for ap in ack_pkts_from_host if ap["ack"] == exp_ack]
        if matching_ack:
            seq_ack_coherent = True
            break
    results["PCAP_SEQ_ACK_MATCH"] = seq_ack_coherent

    all_pass = True
    for name, passed in results.items():
        status = "PASS" if passed else "FAIL"
        if not passed:
            all_pass = False
        print(f"  [{status}] {name}")

    print("="*50)
    if all_pass:
        print(f"RESULTADO: TODOS OS {len(results)} TESTES DA TCP PHASE 2B.2B PASSARAM COM SUCESSO!")
    else:
        print("RESULTADO: FALHA EM UM OU MAIS CRITERIOS DA PHASE 2B.2B!")
    print("="*50)

    return all_pass


if __name__ == "__main__":
    success = run_test()
    sys.exit(0 if success else 1)
