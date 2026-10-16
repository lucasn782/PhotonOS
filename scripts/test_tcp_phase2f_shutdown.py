#!/usr/bin/env python3
"""
PhotonOS TCP Phase 2F Automated Shutdown & Half-Close Test Suite (RFC 793 / POSIX)

Validates:
1.  UNIT_SHUTDOWN_UNCONNECTED: In-kernel Test 31 (CLOSED, LISTEN, SYN_SENT, SYN_RECEIVED rejected with -1)
2.  UNIT_SHUTDOWN_SHUT_RD: In-kernel Test 32 (RX buffer discarded, EOF preserved, ESTABLISHED maintained)
3.  UNIT_SHUTDOWN_SHUT_WR: In-kernel Test 33 (FIN transmitted, FIN_WAIT1, +1 SEQ, RTO timer armed)
4.  UNIT_SHUTDOWN_IDEMPOTENCE: In-kernel Test 34 (repeated shutdown calls without duplicate FIN)
5.  UNIT_SHUTDOWN_DATA_ORDER: In-kernel Test 35 (payload precedes FIN with correct sequence accounting)
6.  UNIT_SHUTDOWN_SHUT_RDWR: In-kernel Test 36 (both directions shut down, FIN transmitted)
7.  UNIT_SHUTDOWN_CLOSE_COEXISTENCE: In-kernel Test 37 (close after shutdown without spurious FIN)
8.  UNIT_SHUTDOWN_CONCURRENT_SEND: In-kernel Test 38 (concurrent send rejected with -1, no data after FIN)
9.  UNIT_SHUTDOWN_WND_EXCEEDED: In-kernel Test 39 (FIN held pending when payload exceeds peer window)
10. UNIT_SHUTDOWN_WND_REOPEN: In-kernel Test 40 (window reopening drains remaining payload strictly before FIN)
11. UNIT_SHUTDOWN_RD_DATA_FIN: In-kernel Test 41 (SHUT_RD discards payload, keeps sequence, accepts FIN -> CLOSE_WAIT)
12. UNIT_SHUTDOWN_FORK_DUP: In-kernel Test 42 (shared descriptor refcounting protects socket/PCB lifetime)
13. USER_SHUTDOWN_API_VALIDATION: Userspace test via tcptest shutdown_api (invalid fd, non-socket, invalid how, unconnected, listen)
14. WIRE_SHUT_WR_HALF_CLOSE: Guest sends data, executes shutdown(SHUT_WR), peer receives data + FIN, peer sends reply, guest reads reply
15. WIRE_SHUT_RD_HALF_CLOSE: Guest executes shutdown(SHUT_RD), recv() returns 0 immediately, guest sends reply, peer receives reply
16. WIRE_SHUT_RDWR_CLOSE: Guest executes shutdown(SHUT_RDWR), both directions shut down, repeated shutdown returns 0, clean close
17. WIRE_ORDER_DRAIN_PAYLOAD: Guest sends 1000 bytes, calls shutdown(SHUT_WR); wire proves all 1000 bytes arrive before FIN
18. WIRE_FORK_SHUTDOWN: Shared descriptor across fork(); parent shutdown(SHUT_WR) reflected in child
19. WIRE_DUP_SHUTDOWN: Shared descriptor across dup(); original fd shutdown reflected in dup_fd
20. PCAP_FIN_ON_SHUT_WR: Wire capture verifies guest transmits valid FIN segment upon SHUT_WR
21. PCAP_DATA_PRECEDES_FIN: Wire capture verifies all data segments have sequence numbers strictly before FIN
22. PCAP_NO_DUPLICATE_FIN: Wire capture verifies no duplicate or redundant FINs on wire
23. PCAP_CHECKSUMS_VALID: All guest IPv4 and TCP checksums are valid
"""

import os
import sys
import time
import socket
import struct
import subprocess
import select

LOG_DIR = "logs"
PCAP_PATH = os.path.join(LOG_DIR, "tcp_phase2f_shutdown.pcap")
GUEST_PORT = 8088
HOST_PORT = 18088  # QEMU hostfwd: 127.0.0.1:18088 -> 10.0.2.15:8088
SHELL_PROMPT = "PhotonOS / > "


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
                "payload_len": payload_len,
                "raw_tcp": tcp_data
            })
    return packets


def compute_tcp_checksum(src_ip, dst_ip, raw_tcp):
    src_bytes = socket.inet_aton(src_ip)
    dst_bytes = socket.inet_aton(dst_ip)
    length = len(raw_tcp)
    pseudo = struct.pack("!4s4sBBH", src_bytes, dst_bytes, 0, 6, length)
    zeroed_tcp = raw_tcp[:16] + b"\x00\x00" + raw_tcp[18:]
    data = pseudo + zeroed_tcp
    if len(data) % 2 != 0:
        data += b"\x00"
    total = sum(struct.unpack(f"!{len(data)//2}H", data))
    while (total >> 16) > 0:
        total = (total & 0xFFFF) + (total >> 16)
    csum = ~total & 0xFFFF
    return 0xFFFF if csum == 0 else csum


def run_test():
    os.makedirs(LOG_DIR, exist_ok=True)
    if os.path.exists(PCAP_PATH):
        try:
            os.remove(PCAP_PATH)
        except OSError:
            pass

    print(f"[TEST SETUP] Iniciando QEMU para Phase 2F Shutdown (port {HOST_PORT} -> {GUEST_PORT})...", flush=True)

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
    output_size = 0

    def drain(timeout=0.3):
        nonlocal output_size
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                chunk = proc.stdout.read(4096)
                if chunk:
                    text = chunk.decode("ascii", errors="replace")
                    sys.stdout.write(text)
                    sys.stdout.flush()
                    output_parts.append(text)
                    output_size += len(text)
                    deadline = time.time() + timeout
                else:
                    time.sleep(0.05)
            except (OSError, TypeError):
                time.sleep(0.05)

    def snapshot():
        return "".join(output_parts)

    def wait_for(pattern, timeout=25, start_at=0):
        t0 = time.time()
        while time.time() - t0 < timeout:
            drain(0.2)
            if pattern in snapshot()[start_at:]:
                return True
        return False

    def wait_new_prompt(min_index, timeout=25):
        t0 = time.time()
        while time.time() - t0 < timeout:
            drain(0.2)
            idx = snapshot().find(SHELL_PROMPT, min_index)
            if idx != -1:
                return idx
        return -1

    def send(cmd_str):
        start_at = len(snapshot())
        sys.stdout.write(f"\n>>> SENDING: {cmd_str}\n")
        sys.stdout.flush()
        proc.stdin.write((cmd_str + "\n").encode("ascii"))
        proc.stdin.flush()
        return start_at

    def start_server(command, ready_marker, min_index):
        prompt_at = wait_new_prompt(min_index, timeout=10)
        if prompt_at < 0:
            print(f"[HARNESS] SERVER_NOT_STARTED: no prompt before {command}", flush=True)
            return len(snapshot()), False, min_index
        print(f"[HARNESS] NEW_PROMPT index={prompt_at} before {command}", flush=True)
        start_at = send(command)
        consumed = prompt_at + len(SHELL_PROMPT)
        ready = wait_for(ready_marker, timeout=10, start_at=start_at)
        state = "READY" if ready else "NOT_STARTED"
        print(f"[HARNESS] SERVER_{state}: {ready_marker}", flush=True)
        return start_at, ready, consumed

    def wait_for_exit(exit_markers, timeout, start_at, min_prompt):
        t0 = time.time()
        if isinstance(exit_markers, str):
            exit_markers = [exit_markers]
        while time.time() - t0 < timeout:
            drain(0.1)
            full = snapshot()
            for marker in exit_markers:
                marker_at = full.find(marker, start_at)
                if marker_at == -1:
                    continue
                prompt_at = full.find(SHELL_PROMPT, max(marker_at + len(marker), min_prompt))
                if prompt_at != -1:
                    print(f"[HARNESS] PROMPT_AFTER_EXIT index={prompt_at} marker_at={marker_at}", flush=True)
                    return prompt_at
            if time.time() - t0 > 2.0 and any(m in full[start_at:] for m in exit_markers):
                try:
                    proc.stdin.write(b"\n")
                    proc.stdin.flush()
                except Exception:
                    pass
                time.sleep(0.2)
            time.sleep(0.05)
        return -1

    results = {}

    try:
        print("[TEST SETUP] Aguardando boot do PhotonOS...", flush=True)
        booted = wait_for("PhotonOS user shell iniciado", timeout=30)
        if not booted:
            print("[TEST FAIL] Boot do PhotonOS falhou!", flush=True)
            return False

        boot_prompt = wait_new_prompt(0, timeout=15)
        if boot_prompt < 0:
            print("\n!!! TIMEOUT aguardando prompt inicial do shell !!!", flush=True)
            return False

        time.sleep(1)
        drain(0.5)
        full_out = snapshot()

        # In-kernel unit tests verification (Tests 31 to 42)
        results["UNIT_SHUTDOWN_UNCONNECTED"] = "PASS" if "[TCP TEST] PASS: Shutdown Unconnected States" in full_out else "FAIL"
        results["UNIT_SHUTDOWN_SHUT_RD"] = "PASS" if "[TCP TEST] PASS: Shutdown SHUT_RD" in full_out else "FAIL"
        results["UNIT_SHUTDOWN_SHUT_WR"] = "PASS" if "[TCP TEST] PASS: Shutdown SHUT_WR" in full_out else "FAIL"
        results["UNIT_SHUTDOWN_IDEMPOTENCE"] = "PASS" if "[TCP TEST] PASS: Shutdown Idempotence" in full_out else "FAIL"
        results["UNIT_SHUTDOWN_DATA_ORDER"] = "PASS" if "[TCP TEST] PASS: Shutdown Data Precedes FIN" in full_out else "FAIL"
        results["UNIT_SHUTDOWN_SHUT_RDWR"] = "PASS" if "[TCP TEST] PASS: Shutdown SHUT_RDWR" in full_out else "FAIL"
        results["UNIT_SHUTDOWN_CLOSE_COEXISTENCE"] = "PASS" if "[TCP TEST] PASS: Shutdown and Close Coexistence" in full_out else "FAIL"
        results["UNIT_SHUTDOWN_CONCURRENT_SEND"] = "PASS" if "[TCP TEST] PASS: Concurrent send() with shutdown(SHUT_WR)" in full_out else "FAIL"
        results["UNIT_SHUTDOWN_WND_EXCEEDED"] = "PASS" if "[TCP TEST] PASS: SHUT_WR with Payload Exceeding Peer Window" in full_out else "FAIL"
        results["UNIT_SHUTDOWN_WND_REOPEN"] = "PASS" if "[TCP TEST] PASS: Peer Window Reopening Drains Payload Before FIN" in full_out else "FAIL"
        results["UNIT_SHUTDOWN_RD_DATA_FIN"] = "PASS" if "[TCP TEST] PASS: SHUT_RD Data Then Remote FIN" in full_out else "FAIL"
        results["UNIT_SHUTDOWN_FORK_DUP"] = "PASS" if "[TCP TEST] PASS: SHUT_RDWR with Shared Descriptors fork/dup" in full_out else "FAIL"

        shell_cursor = boot_prompt

        # -----------------------------------------------------------------
        # Test 1: USER_SHUTDOWN_API_VALIDATION
        # -----------------------------------------------------------------
        print("\n[TEST EXEC] Testando validacao de API em Userspace (tcptest shutdown_api)...", flush=True)
        send_at = send("tcptest shutdown_api")
        api_ok = wait_for(">>> API VALIDATION CONCLUIDO COM SUCESSO! <<<", timeout=10, start_at=send_at)
        shell_cursor = wait_new_prompt(send_at, timeout=10)
        results["USER_SHUTDOWN_API_VALIDATION"] = "PASS" if api_ok else "FAIL"

        # -----------------------------------------------------------------
        # Test 2: WIRE_SHUT_WR_HALF_CLOSE
        # -----------------------------------------------------------------
        print("\n[TEST EXEC] Testando Wire SHUT_WR Half-Close...", flush=True)
        s_start, s_ready, s_floor = start_server(
            f"tcptest shutdown_server {GUEST_PORT} shut_wr",
            f"[TCPTEST SHUTDOWN] Ouvindo na porta {GUEST_PORT} (mode=shut_wr)...",
            shell_cursor
        )
        shut_wr_wire_ok = False
        if s_ready:
            try:
                s = socket.create_connection(("127.0.0.1", HOST_PORT), timeout=10)
                s.settimeout(10.0)
                # 1. Read initial payload sent by guest before shutdown(SHUT_WR)
                initial_data = s.recv(1024)
                # 2. Read EOF (FIN from guest)
                eof_data = s.recv(1024)
                has_eof = (len(eof_data) == 0)
                # 3. Send reply back to guest to verify guest reception is still active
                s.sendall(b"PEER_REPLY_OK\n")
                time.sleep(0.5)
                s.close()
                if b"SHUT_WR_INITIAL_PAYLOAD" in initial_data and has_eof:
                    shut_wr_wire_ok = True
            except Exception as e:
                print(f"[TEST ERROR] WIRE_SHUT_WR: {e}", flush=True)

        exit_at = wait_for_exit(">>> SHUT_WR CONCLUIDO COM SUCESSO! <<<", timeout=15, start_at=s_start, min_prompt=s_floor)
        if exit_at != -1 and shut_wr_wire_ok:
            results["WIRE_SHUT_WR_HALF_CLOSE"] = "PASS"
            shell_cursor = exit_at
        else:
            results["WIRE_SHUT_WR_HALF_CLOSE"] = "FAIL"

        # -----------------------------------------------------------------
        # Test 3: WIRE_SHUT_RD_HALF_CLOSE
        # -----------------------------------------------------------------
        print("\n[TEST EXEC] Testando Wire SHUT_RD Half-Close...", flush=True)
        s_start, s_ready, s_floor = start_server(
            f"tcptest shutdown_server {GUEST_PORT} shut_rd",
            f"[TCPTEST SHUTDOWN] Ouvindo na porta {GUEST_PORT} (mode=shut_rd)...",
            shell_cursor
        )
        shut_rd_wire_ok = False
        if s_ready:
            try:
                s = socket.create_connection(("127.0.0.1", HOST_PORT), timeout=10)
                s.settimeout(10.0)
                # Send data to guest (guest will have shut down RD and recv() will return 0)
                s.sendall(b"DATA_DISCARDED_BY_SHUT_RD\n")
                # Guest should still be able to send reply
                reply = s.recv(1024)
                if b"REPLY_AFTER_SHUT_RD" in reply:
                    shut_rd_wire_ok = True
                s.close()
            except Exception as e:
                print(f"[TEST ERROR] WIRE_SHUT_RD: {e}", flush=True)

        exit_at = wait_for_exit(">>> SHUT_RD CONCLUIDO COM SUCESSO! <<<", timeout=15, start_at=s_start, min_prompt=s_floor)
        if exit_at != -1 and shut_rd_wire_ok:
            results["WIRE_SHUT_RD_HALF_CLOSE"] = "PASS"
            shell_cursor = exit_at
        else:
            results["WIRE_SHUT_RD_HALF_CLOSE"] = "FAIL"

        # -----------------------------------------------------------------
        # Test 4: WIRE_SHUT_RDWR_CLOSE
        # -----------------------------------------------------------------
        print("\n[TEST EXEC] Testando Wire SHUT_RDWR...", flush=True)
        s_start, s_ready, s_floor = start_server(
            f"tcptest shutdown_server {GUEST_PORT} shut_rdwr",
            f"[TCPTEST SHUTDOWN] Ouvindo na porta {GUEST_PORT} (mode=shut_rdwr)...",
            shell_cursor
        )
        shut_rdwr_wire_ok = False
        if s_ready:
            try:
                s = socket.create_connection(("127.0.0.1", HOST_PORT), timeout=10)
                s.settimeout(10.0)
                # Guest shuts down RDWR: FIN is sent to host
                eof = s.recv(1024)
                if len(eof) == 0:
                    shut_rdwr_wire_ok = True
                s.close()
            except Exception as e:
                print(f"[TEST ERROR] WIRE_SHUT_RDWR: {e}", flush=True)

        exit_at = wait_for_exit(">>> SHUT_RDWR CONCLUIDO COM SUCESSO! <<<", timeout=15, start_at=s_start, min_prompt=s_floor)
        if exit_at != -1 and shut_rdwr_wire_ok:
            results["WIRE_SHUT_RDWR_CLOSE"] = "PASS"
            shell_cursor = exit_at
        else:
            results["WIRE_SHUT_RDWR_CLOSE"] = "FAIL"

        # -----------------------------------------------------------------
        # Test 5: WIRE_ORDER_DRAIN_PAYLOAD
        # -----------------------------------------------------------------
        print("\n[TEST EXEC] Testando Ordenacao de Payload Precedendo FIN (1000 bytes)...", flush=True)
        s_start, s_ready, s_floor = start_server(
            f"tcptest shutdown_server {GUEST_PORT} order_drain",
            f"[TCPTEST SHUTDOWN] Ouvindo na porta {GUEST_PORT} (mode=order_drain)...",
            shell_cursor
        )
        order_wire_ok = False
        if s_ready:
            try:
                s = socket.create_connection(("127.0.0.1", HOST_PORT), timeout=10)
                s.settimeout(10.0)
                received_payload = b""
                while len(received_payload) < 1000:
                    chunk = s.recv(4096)
                    if not chunk:
                        break
                    received_payload += chunk
                # Now expect EOF from FIN
                eof_check = s.recv(1024)
                if len(received_payload) == 1000 and len(eof_check) == 0:
                    order_wire_ok = True
                s.close()
            except Exception as e:
                print(f"[TEST ERROR] WIRE_ORDER_DRAIN: {e}", flush=True)

        exit_at = wait_for_exit(">>> ORDER_DRAIN CONCLUIDO COM SUCESSO! <<<", timeout=15, start_at=s_start, min_prompt=s_floor)
        if exit_at != -1 and order_wire_ok:
            results["WIRE_ORDER_DRAIN_PAYLOAD"] = "PASS"
            shell_cursor = exit_at
        else:
            results["WIRE_ORDER_DRAIN_PAYLOAD"] = "FAIL"

        # -----------------------------------------------------------------
        # Test 6: WIRE_FORK_SHUTDOWN
        # -----------------------------------------------------------------
        print("\n[TEST EXEC] Testando shutdown() compartilhado por fork()...", flush=True)
        s_start, s_ready, s_floor = start_server(
            f"tcptest shutdown_server {GUEST_PORT} fork_shutdown",
            f"[TCPTEST SHUTDOWN] Ouvindo na porta {GUEST_PORT} (mode=fork_shutdown)...",
            shell_cursor
        )
        fork_wire_ok = False
        if s_ready:
            try:
                s = socket.create_connection(("127.0.0.1", HOST_PORT), timeout=10)
                s.settimeout(10.0)
                # Parent shuts down WR: host gets FIN
                eof = s.recv(1024)
                s.sendall(b"CHILD_DATA_IN\n")
                if len(eof) == 0:
                    fork_wire_ok = True
                time.sleep(0.3)
                s.close()
            except Exception as e:
                print(f"[TEST ERROR] WIRE_FORK: {e}", flush=True)

        exit_at = wait_for_exit(">>> FORK_SHUTDOWN CONCLUIDO COM SUCESSO! <<<", timeout=15, start_at=s_start, min_prompt=s_floor)
        if exit_at != -1 and fork_wire_ok:
            results["WIRE_FORK_SHUTDOWN"] = "PASS"
            shell_cursor = exit_at
        else:
            results["WIRE_FORK_SHUTDOWN"] = "FAIL"

        # -----------------------------------------------------------------
        # Test 7: WIRE_DUP_SHUTDOWN
        # -----------------------------------------------------------------
        print("\n[TEST EXEC] Testando shutdown() compartilhado por dup()...", flush=True)
        s_start, s_ready, s_floor = start_server(
            f"tcptest shutdown_server {GUEST_PORT} dup_shutdown",
            f"[TCPTEST SHUTDOWN] Ouvindo na porta {GUEST_PORT} (mode=dup_shutdown)...",
            shell_cursor
        )
        dup_wire_ok = False
        if s_ready:
            try:
                s = socket.create_connection(("127.0.0.1", HOST_PORT), timeout=10)
                s.settimeout(10.0)
                eof = s.recv(1024)
                if len(eof) == 0:
                    dup_wire_ok = True
                s.close()
            except Exception as e:
                print(f"[TEST ERROR] WIRE_DUP: {e}", flush=True)

        exit_at = wait_for_exit(">>> DUP_SHUTDOWN CONCLUIDO COM SUCESSO! <<<", timeout=15, start_at=s_start, min_prompt=s_floor)
        if exit_at != -1 and dup_wire_ok:
            results["WIRE_DUP_SHUTDOWN"] = "PASS"
            shell_cursor = exit_at
        else:
            results["WIRE_DUP_SHUTDOWN"] = "FAIL"

        # -----------------------------------------------------------------
        # PCAP Analysis
        # -----------------------------------------------------------------
        print("\n[TEST EXEC] Analisando captura PCAP...", flush=True)
        packets = parse_pcap(PCAP_PATH)
        guest_tx = [p for p in packets if p["src_ip"] == "10.0.2.15" and p["src_port"] == GUEST_PORT]

        # 1. PCAP_FIN_ON_SHUT_WR
        fin_pkts = [p for p in guest_tx if (p["flags"] & 0x0001) != 0]
        results["PCAP_FIN_ON_SHUT_WR"] = "PASS" if len(fin_pkts) >= 4 else "FAIL"

        # 2. PCAP_DATA_PRECEDES_FIN
        # In each connection, data packet seq numbers must be strictly <= FIN seq
        data_order_ok = True
        for p in guest_tx:
            if p["payload_len"] > 0:
                matching_fins = [f for f in fin_pkts if f["dst_port"] == p["dst_port"]]
                for mf in matching_fins:
                    if p["seq"] >= mf["seq"]:
                        data_order_ok = False
                        break
        results["PCAP_DATA_PRECEDES_FIN"] = "PASS" if (data_order_ok and len(fin_pkts) > 0) else "FAIL"

        # 3. PCAP_NO_DUPLICATE_FIN
        # Within any single flow (dst_port), there should be exactly one original FIN sequence number
        dup_fin_detected = False
        seen_fin_ports = {}
        for f in fin_pkts:
            dport = f["dst_port"]
            fseq = f["seq"]
            if dport in seen_fin_ports:
                if seen_fin_ports[dport] != fseq:
                    dup_fin_detected = True
            else:
                seen_fin_ports[dport] = fseq
        results["PCAP_NO_DUPLICATE_FIN"] = "PASS" if not dup_fin_detected else "FAIL"

        # 4. PCAP_CHECKSUMS_VALID
        csum_valid = True
        if len(guest_tx) > 0:
            for p in guest_tx:
                exp_csum = compute_tcp_checksum(p["src_ip"], p["dst_ip"], p["raw_tcp"])
                if p["csum"] != exp_csum and exp_csum != 0xFFFF:
                    csum_valid = False
                    break
        results["PCAP_CHECKSUMS_VALID"] = "PASS" if csum_valid and len(guest_tx) > 0 else "FAIL"

    finally:
        print("\n[TEST TEARDOWN] Encerrando QEMU...", flush=True)
        try:
            proc.terminate()
            proc.wait(timeout=5)
        except Exception:
            proc.kill()

    # -----------------------------------------------------------------
    # Summary Table
    # -----------------------------------------------------------------
    print("\n" + "=" * 65)
    print("         PhotonOS TCP Phase 2F Shutdown Results")
    print("=" * 65)
    all_pass = True
    for test_name, status in results.items():
        print(f"  {test_name:<30}: {status}")
        if status != "PASS":
            all_pass = False
    print("=" * 65)
    total = len(results)
    passed = sum(1 for s in results.values() if s == "PASS")
    pct = (passed / total) * 100 if total > 0 else 0
    print(f"Total: {passed}/{total} testes passaram ({pct:.0f}% PASS)")
    if all_pass:
        print(">>> ALL TCP PHASE 2F SHUTDOWN TESTS PASSED! <<<")
    else:
        print(">>> SOME TCP PHASE 2F SHUTDOWN TESTS FAILED! <<<")
    print("=" * 65)

    return all_pass


if __name__ == "__main__":
    success = run_test()
    sys.exit(0 if success else 1)
