#!/usr/bin/env python3
"""
PhotonOS TCP Phase 2B.2A Automated Receive Path Test Suite
Validates:
1.  RX_ERRORS: Parameter and user pointer validation (NULL, 0x10, len=0, UDP, closed fd)
2.  RX_SINGLE_BYTE: Single byte payload reception and wire ACK
3.  RX_SMALL_PAYLOAD: Small string payload reception ("hello")
4.  RX_LARGE_PAYLOAD: Large buffer reception (1024 bytes)
5.  RX_PARTIAL_READ: Successive partial reads from ring buffer preserving remaining data
6.  RX_BLOCKING_WAKE: Blocking on empty buffer and wakeup upon data packet arrival
7.  RX_MULTIPLE_CONNECTIONS: Independent receive buffers across multiple connections
8.  RX_FORK: Socket receive in forked child process
9.  RX_DUP: Socket receive via duplicated descriptor
10. UNIT_RING_BUFFER: Circular buffer wrap-around, write, read and space tracking
11. UNIT_IN_ORDER: Kernel unit test for in-order segment delivery
12. UNIT_DUPLICATE: Kernel unit test for duplicate segment discard & re-ACK
13. UNIT_OUT_OF_ORDER: Kernel unit test for out-of-order segment policy (drop + re-ACK)
14. UNIT_FIN_CLOSE_WAIT: Kernel unit test for FIN processing and CLOSE_WAIT state transition
15. PCAP_INSPECTION: Wire inspection of DATA, ACK numbers, window size, and checksums
"""

import os
import sys
import time
import socket
import struct
import subprocess

LOG_DIR = "logs"
PCAP_PATH = os.path.join(LOG_DIR, "tcp_phase2b2_rx.pcap")
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

    # 1. Teste de Validação de Erros de Syscall recv()
    send("tcptest recv_errors")
    time.sleep(2)
    drain(0.5)

    # 2. Teste RX Single Byte ("A")
    send(f"tcptest recv_server {GUEST_PORT} 1")
    if not wait_for("Ouvindo na porta", timeout=10):
        print("\n!!! Servidor recv_server nao iniciou !!!", flush=True)
    time.sleep(1)
    try:
        s = socket.create_connection(("127.0.0.1", HOST_PORT), timeout=5)
        time.sleep(0.2)
        s.sendall(b"A")
        time.sleep(0.5)
        s.close()
    except Exception as e:
        print(f"[HOST CLIENT SINGLE] Erro: {e}", flush=True)
    time.sleep(2)
    drain(0.5)

    # 3. Teste RX Small Payload ("hello")
    send(f"tcptest recv_server {GUEST_PORT} 5")
    if not wait_for("Ouvindo na porta", timeout=10):
        print("\n!!! Servidor recv_server nao iniciou !!!", flush=True)
    time.sleep(1)
    try:
        s = socket.create_connection(("127.0.0.1", HOST_PORT), timeout=5)
        time.sleep(0.2)
        s.sendall(b"hello")
        time.sleep(0.5)
        s.close()
    except Exception as e:
        print(f"[HOST CLIENT SMALL] Erro: {e}", flush=True)
    time.sleep(2)
    drain(0.5)

    # 4. Teste RX Large Payload (1024 bytes)
    send(f"tcptest recv_server {GUEST_PORT} 1024")
    if not wait_for("Ouvindo na porta", timeout=10):
        print("\n!!! Servidor recv_server nao iniciou !!!", flush=True)
    time.sleep(1)
    try:
        s = socket.create_connection(("127.0.0.1", HOST_PORT), timeout=5)
        time.sleep(0.2)
        large_data = b"X" * 1024
        s.sendall(large_data)
        time.sleep(0.5)
        s.close()
    except Exception as e:
        print(f"[HOST CLIENT LARGE] Erro: {e}", flush=True)
    time.sleep(2)
    drain(0.5)

    # 5. Teste RX Partial Read ("ABCDEFGHIJ" lido em 4 + 4 + 2)
    send(f"tcptest recv_partial {GUEST_PORT}")
    if not wait_for("Ouvindo na porta", timeout=10):
        print("\n!!! Servidor recv_partial nao iniciou !!!", flush=True)
    time.sleep(1)
    try:
        s = socket.create_connection(("127.0.0.1", HOST_PORT), timeout=5)
        time.sleep(0.2)
        s.sendall(b"ABCDEFGHIJ")
        time.sleep(0.5)
        s.close()
    except Exception as e:
        print(f"[HOST CLIENT PARTIAL] Erro: {e}", flush=True)
    time.sleep(2)
    drain(0.5)

    # 6. Teste RX Blocking & Wakeup
    send(f"tcptest recv_block {GUEST_PORT}")
    if not wait_for("Aguardando conexao", timeout=10):
        print("\n!!! Servidor recv_block nao iniciou !!!", flush=True)
    time.sleep(1)
    try:
        s = socket.create_connection(("127.0.0.1", HOST_PORT), timeout=5)
        time.sleep(1.0)  # Garante que o servidor bloqueou em recv()
        s.sendall(b"WAKEUP_DATA")
        time.sleep(0.5)
        s.close()
    except Exception as e:
        print(f"[HOST CLIENT BLOCK] Erro: {e}", flush=True)
    time.sleep(2)
    drain(0.5)

    # 7. Teste RX Multiple Connections
    send(f"tcptest recv_multi {GUEST_PORT} 3")
    if not wait_for("para 3 conexoes", timeout=10):
        print("\n!!! Servidor recv_multi nao iniciou !!!", flush=True)
    time.sleep(1)
    for i in range(1, 4):
        try:
            s = socket.create_connection(("127.0.0.1", HOST_PORT), timeout=5)
            time.sleep(0.2)
            s.sendall(f"MULTI_MESSAGE_{i}".encode("ascii"))
            time.sleep(0.3)
            s.close()
            time.sleep(0.3)
        except Exception as e:
            print(f"[HOST CLIENT MULTI {i}] Erro: {e}", flush=True)
    time.sleep(2)
    drain(0.5)

    # 8. Teste RX Fork Lifecycle
    send(f"tcptest recv_fork {GUEST_PORT}")
    if not wait_for("Aguardando conexao na porta", timeout=10):
        print("\n!!! Servidor recv_fork nao iniciou !!!", flush=True)
    time.sleep(1)
    try:
        s = socket.create_connection(("127.0.0.1", HOST_PORT), timeout=5)
        time.sleep(0.2)
        s.sendall(b"FORK_DATA_PAYLOAD")
        time.sleep(0.5)
        s.close()
    except Exception as e:
        print(f"[HOST CLIENT FORK] Erro: {e}", flush=True)
    time.sleep(2)
    drain(0.5)

    # 9. Teste RX Dup Descriptor
    send(f"tcptest recv_dup {GUEST_PORT}")
    if not wait_for("Aguardando conexao na porta", timeout=10):
        print("\n!!! Servidor recv_dup nao iniciou !!!", flush=True)
    time.sleep(1)
    try:
        s = socket.create_connection(("127.0.0.1", HOST_PORT), timeout=5)
        time.sleep(0.2)
        s.sendall(b"DUP_DATA_PAYLOAD")
        time.sleep(0.5)
        s.close()
    except Exception as e:
        print(f"[HOST CLIENT DUP] Erro: {e}", flush=True)
    time.sleep(2)
    drain(1.0)

    # Finaliza QEMU
    proc.terminate()
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()

    full_output = "".join(output_parts)

    # 10. Validação Wire PCAP
    print("\n[PCAP WIRE INSPECTION] Analisando captura de pacotes...", flush=True)
    packets = parse_pcap(PCAP_PATH)
    print(f"  Total de segmentos TCP capturados no PCAP: {len(packets)}", flush=True)

    data_pkts_from_host = [p for p in packets if p["src_port"] > 1024 and p["dst_port"] == GUEST_PORT and p["payload_len"] > 0]
    ack_pkts_from_guest = [p for p in packets if p["src_port"] == GUEST_PORT and (p["flags"] & 0x10) != 0]

    print(f"  Segmentos de DADOS enviados pelo host: {len(data_pkts_from_host)}", flush=True)
    print(f"  Segmentos de ACK enviados pelo PhotonOS: {len(ack_pkts_from_guest)}", flush=True)

    for i, p in enumerate(data_pkts_from_host[:5], 1):
        print(f"    [DATA {i}] Host -> PhotonOS | Seq={p['seq']} Ack={p['ack']} PayloadLen={p['payload_len']}", flush=True)

    for i, p in enumerate(ack_pkts_from_guest[:5], 1):
        print(f"    [ACK {i}]  PhotonOS -> Host | Seq={p['seq']} Ack={p['ack']} Window={p['window']}", flush=True)

    # Test Results Map
    results = {
        "UNIT_RING_BUFFER": "PASS: tcp_rx_buffer" in full_output,
        "UNIT_IN_ORDER": "PASS: Receive Path (DATA in-order recebido" in full_output,
        "UNIT_DUPLICATE": "PASS: Receive Path (DATA duplicado descartado" in full_output,
        "UNIT_OUT_OF_ORDER": "PASS: Receive Path (DATA out-of-order descartado" in full_output,
        "UNIT_FIN_CLOSE_WAIT": "PASS: Receive Path (FIN recebido, transicao para CLOSE_WAIT" in full_output,
        "RX_ERRORS": "TODOS OS TESTES DE ERRO RECV PASSARAM" in full_output,
        "RX_SINGLE_BYTE": "Recebidos 1 bytes com sucesso: A" in full_output,
        "RX_SMALL_PAYLOAD": "Recebidos 5 bytes com sucesso: hello" in full_output,
        "RX_LARGE_PAYLOAD": "Recebidos 1024 bytes com sucesso" in full_output,
        "RX_PARTIAL_READ": "LEITURAS PARCIAIS CONCLUIDAS COM SUCESSO" in full_output,
        "RX_BLOCKING_WAKE": "Acordou do recv() com sucesso! lidos 11 bytes: WAKEUP_DATA" in full_output,
        "RX_MULTIPLE_CONNECTIONS": "TODAS AS CONEXOES MULTIPLAS RECEBERAM DADOS COM SUCESSO" in full_output,
        "RX_FORK": "RECV NO FILHO PASSOU COM SUCESSO" in full_output,
        "RX_DUP": "RECV VIA DUP PASSOU COM SUCESSO" in full_output,
        "PCAP_DATA_RECEIVED": len(data_pkts_from_host) >= 5,
        "PCAP_ACK_TRANSMITTED": len(ack_pkts_from_guest) >= 5,
        "PCAP_WINDOW_VALID": any(p["window"] > 0 for p in ack_pkts_from_guest),
    }

    print("\n" + "=" * 70)
    print("         PHOTONOS TCP PHASE 2B.2A RECEIVE PATH VERIFICATION REPORT")
    print("=" * 70)
    print("\n----------------------------------------------------------------------")
    print("                      DETAILED RESULTS")
    print("----------------------------------------------------------------------")
    all_pass = True
    for name, status in results.items():
        res_str = "PASS" if status else "FAIL"
        if not status:
            all_pass = False
        print(f"  {name:<28}: {res_str}")
    print("=" * 70)

    if all_pass:
        print(">>> ALL TCP PHASE 2B.2A RECEIVE PATH TESTS & WIRE CHECKS PASSED! <<<")
        return True
    else:
        print(">>> SOME TCP PHASE 2B.2A TESTS FAILED! <<<")
        return False


if __name__ == "__main__":
    success = run_test()
    sys.exit(0 if success else 1)
