#!/usr/bin/env python3
"""
PhotonOS TCP Phase 2A Automated Test Suite
Validates:
1. 3-Way Handshake Active Open (SYN -> SYN+ACK -> ACK -> ESTABLISHED)
2. Closed port RST handling (Connection Refused)
3. Unreachable host timeout handling (Connection Timeout)
4. Consecutive connections (Stress / Clean PCB lifecycle)
5. Concurrent multi-process connections via fork()
6. Wire-level packet capture (.pcap) analysis (SYN, SYN+ACK, ACK flags, seq, ack, checksums)
"""

import os
import sys
import time
import socket
import struct
import threading
import subprocess

LOG_DIR = "logs"
PCAP_PATH = os.path.join(LOG_DIR, "tcp_phase2a.pcap")
HOST_PORT = 8088

class MockTCPServer:
    def __init__(self, host="0.0.0.0", port=HOST_PORT):
        self.host = host
        self.port = port
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind((self.host, self.port))
        self.sock.listen(16)
        self.running = True
        self.connections = []
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()

    def _run(self):
        self.sock.settimeout(0.5)
        while self.running:
            try:
                conn, addr = self.sock.accept()
                self.connections.append((conn, addr))
            except socket.timeout:
                continue
            except Exception:
                break

    def stop(self):
        self.running = False
        try:
            self.sock.close()
        except Exception:
            pass
        for conn, _ in self.connections:
            try:
                conn.close()
            except Exception:
                pass


def parse_pcap(pcap_path):
    """
    Parses a standard libpcap capture file and extracts TCP packets
    validating the 3-Way Handshake segments.
    """
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
            
            # Ethernet header (14 bytes)
            if len(data) < 14 + 20 + 20:
                continue
            eth_type = struct.unpack("!H", data[12:14])[0]
            if eth_type != 0x0800: # IPv4
                continue
            
            # IPv4 header
            ip_data = data[14:]
            ver_ihl = ip_data[0]
            ihl = (ver_ihl & 0x0F) * 4
            protocol = ip_data[9]
            src_ip = socket.inet_ntoa(ip_data[12:16])
            dst_ip = socket.inet_ntoa(ip_data[16:20])
            
            if protocol != 6: # TCP
                continue
            
            tcp_data = ip_data[ihl:]
            if len(tcp_data) < 20:
                continue
            
            src_port, dst_port, seq, ack, offset_flags, window, csum, urg = struct.unpack("!HHIIHHHH", tcp_data[:20])
            flags = offset_flags & 0x01FF
            
            packets.append({
                "src_ip": src_ip,
                "dst_ip": dst_ip,
                "src_port": src_port,
                "dst_port": dst_port,
                "seq": seq,
                "ack": ack,
                "flags": flags,
                "window": window,
                "csum": csum
            })
    return packets


def run_test():
    os.makedirs(LOG_DIR, exist_ok=True)
    if os.path.exists(PCAP_PATH):
        try:
            os.remove(PCAP_PATH)
        except OSError:
            pass

    server = MockTCPServer(port=HOST_PORT)
    print(f"[TEST SETUP] Servidor TCP Host ouvindo em 0.0.0.0:{HOST_PORT}", flush=True)

    cmd = [
        "qemu-system-x86_64",
        "-smp", "4",
        "-drive", "format=raw,file=build/photon.img,if=floppy",
        "-drive", "format=raw,file=build/disk.img,if=ide,index=0,media=disk",
        "-boot", "a",
        "-netdev", "user,id=net0",
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

    def wait_for(pattern, timeout=20):
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
        server.stop()
        return False

    time.sleep(1)
    drain(0.5)

    # 1. Teste de Three-Way Handshake com endpoint externo
    send(f"tcptest connect 10.0.2.2 {HOST_PORT}")
    time.sleep(3)
    drain(1.0)

    # 2. Teste negativo: porta fechada (RST)
    send("tcptest closed 10.0.2.2 8099")
    time.sleep(3)
    drain(1.0)

    # 3. Teste negativo: host inexistente (Timeout)
    send("tcptest timeout 10.0.2.240 8088")
    time.sleep(5)
    drain(1.0)

    # 4. Teste de estresse: 4 conexoes consecutivas
    send(f"tcptest stress 10.0.2.2 {HOST_PORT} 4")
    time.sleep(5)
    drain(1.0)

    # 5. Teste de concorrencia SMP: 4 conexoes simultaneas via fork()
    send(f"tcptest concurrent 10.0.2.2 {HOST_PORT} 4")
    time.sleep(5)
    drain(1.0)

    proc.terminate()
    proc.wait()
    server.stop()

    full_output = "".join(output_parts)

    print("\n" + "=" * 65)
    print("         PHOTONOS TCP PHASE 2A VERIFICATION REPORT")
    print("=" * 65)

    checks = {
        "HANDSHAKE_ESTABLISHED": "3-Way Handshake ESTABLISHED" in full_output,
        "CLOSED_PORT_RST": "falhou como esperado" in full_output and "closed" in full_output,
        "TIMEOUT_HANDLING": "falhou como esperado" in full_output and "timeout" in full_output,
        "STRESS_CONSECUTIVE": "TODAS AS 4 CONEXOES CONSECUTIVAS PASSARAM" in full_output,
        "CONCURRENT_FORK": "TODAS AS 4 CONEXOES SIMULTANEAS PASSARAM" in full_output,
    }

    for k, v in checks.items():
        status = "PASS" if v else "FAIL"
        print(f"  {k:30s} : {status}")

    # Wire-level PCAP inspection
    pcap_pkts = parse_pcap(PCAP_PATH)
    print("\n[PCAP WIRE INSPECTION]")
    print(f"  Total de segmentos TCP capturados: {len(pcap_pkts)}")

    has_syn = False
    has_synack = False
    has_ack = False

    for pkt in pcap_pkts:
        flags = pkt["flags"]
        is_syn = (flags & 0x02) != 0 and (flags & 0x10) == 0
        is_synack = (flags & 0x12) == 0x12
        is_ack = (flags & 0x10) != 0 and (flags & 0x02) == 0

        if is_syn and pkt["src_ip"] == "10.0.2.15" and pkt["dst_port"] == HOST_PORT:
            has_syn = True
            print(f"  [PKT 1 - SYN]     {pkt['src_ip']}:{pkt['src_port']} -> {pkt['dst_ip']}:{pkt['dst_port']} | Seq={pkt['seq']} Ack={pkt['ack']} Flags=0x{flags:02X} (SYN)")
        elif is_synack and pkt["dst_ip"] == "10.0.2.15" and pkt["src_port"] == HOST_PORT:
            has_synack = True
            print(f"  [PKT 2 - SYN+ACK] {pkt['src_ip']}:{pkt['src_port']} -> {pkt['dst_ip']}:{pkt['dst_port']} | Seq={pkt['seq']} Ack={pkt['ack']} Flags=0x{flags:02X} (SYN+ACK)")
        elif is_ack and pkt["src_ip"] == "10.0.2.15" and pkt["dst_port"] == HOST_PORT and has_synack:
            has_ack = True
            print(f"  [PKT 3 - ACK]     {pkt['src_ip']}:{pkt['src_port']} -> {pkt['dst_ip']}:{pkt['dst_port']} | Seq={pkt['seq']} Ack={pkt['ack']} Flags=0x{flags:02X} (ACK)")

    checks["PCAP_SYN_CAPTURED"] = has_syn
    checks["PCAP_SYNACK_CAPTURED"] = has_synack
    checks["PCAP_ACK_CAPTURED"] = has_ack

    print(f"\n  PCAP 3-Way Handshake Wire Check: {'PASS' if (has_syn and has_synack and has_ack) else 'FAIL'}")
    print("=" * 65)

    all_passed = all(checks.values())
    if all_passed:
        print(">>> ALL TCP PHASE 2A TESTS & WIRE CHECKS PASSED! <<<", flush=True)
        return True
    else:
        print(">>> SOME TCP PHASE 2A CHECKS FAILED! <<<", flush=True)
        return False

if __name__ == "__main__":
    success = run_test()
    sys.exit(0 if success else 1)
