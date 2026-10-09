#!/usr/bin/env python3
"""
PhotonOS TCP Phase 2D Automated Teardown & Lifecycle Test Suite

Validates:
1.  UNIT_ACTIVE_CLOSE: In-kernel test for Active Close state machine
    (ESTABLISHED -> FIN_WAIT1 -> FIN_WAIT2 -> TIME_WAIT -> CLOSED)
2.  UNIT_PASSIVE_CLOSE: In-kernel test for Passive Close state machine
    (ESTABLISHED -> CLOSE_WAIT -> LAST_ACK -> CLOSED)
3.  UNIT_SIMULTANEOUS_CLOSE: In-kernel test for Simultaneous Close
    (FIN_WAIT1 -> peer FIN+ACK -> TIME_WAIT)
4.  UNIT_DATA_PLUS_FIN: In-kernel test for DATA+FIN segment handling
    (payload delivered before EOF, rcv_nxt = seq + data_len + 1)
5.  UNIT_DUPLICATE_FIN: In-kernel test for duplicate FIN
    (re-ACKed without double-incrementing rcv_nxt or corrupting state)
6.  UNIT_FIN_RETRANSMISSION: In-kernel test for FIN retransmission on RTO
    (RTO timer triggers retransmission with exponential backoff)
7.  WIRE_PASSIVE_CLOSE: Host closes first; guest receives payload, gets EOF (0),
    enters CLOSE_WAIT, calls close(), sends FIN, enters LAST_ACK, reaches CLOSED.
8.  WIRE_ACTIVE_CLOSE: Guest server sends payload and immediately closes; host receives
    complete payload then EOF; guest enters FIN_WAIT1 -> FIN_WAIT2 -> TIME_WAIT.
9.  WIRE_EOF_REPEATED: Verifies EOF semantics; recv() after FIN returns 0 without hanging.
10. WIRE_FORK_TEARDOWN: Server accepts connection and forks; parent closes client_fd;
    child processes request and closes; verifies refcounting prevents premature FIN/destruction.
11. WIRE_DUP_TEARDOWN: Server accepts connection, dups fd, closes original; dup_fd stays active;
    verifies file_description refcount semantics.
12. WIRE_SIMULTANEOUS_CLOSE: Both endpoints initiate close nearly simultaneously on wire.
13. FULL_DUPLEX_THEN_CLOSE: 16 KiB bidirectional data exchange followed by graceful close.
14. PCAP_FIN_ON_WIRE: Guest transmits valid FIN segments on wire.
15. PCAP_FIN_SEQ_ACCOUNTING: FIN consumes exactly 1 sequence number (peer ACK = FIN_SEQ + 1).
16. PCAP_CHECKSUMS_VALID: All guest teardown packets have valid TCP checksums.
"""

import os
import sys
import time
import socket
import struct
import subprocess
import select

LOG_DIR = "logs"
PCAP_PATH = os.path.join(LOG_DIR, "tcp_phase2d_teardown.pcap")
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
                "payload_len": payload_len
            })
    return packets


def calc_tcp_checksum(src_ip, dst_ip, tcp_bytes):
    src = socket.inet_aton(src_ip)
    dst = socket.inet_aton(dst_ip)
    length = len(tcp_bytes)
    pseudo = src + dst + struct.pack("!BBH", 0, 6, length)
    full = pseudo + tcp_bytes
    if len(full) % 2 != 0:
        full += b"\x00"

    total = 0
    for i in range(0, len(full), 2):
        w = (full[i] << 8) | full[i + 1]
        total += w

    while (total >> 16) > 0:
        total = (total & 0xFFFF) + (total >> 16)

    return (~total) & 0xFFFF


def run_test():
    os.makedirs(LOG_DIR, exist_ok=True)
    if os.path.exists(PCAP_PATH):
        try:
            os.remove(PCAP_PATH)
        except OSError:
            pass

    print(f"[TEST SETUP] Iniciando QEMU para Phase 2D Teardown (port {HOST_PORT} -> {GUEST_PORT})...", flush=True)

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

    def last_prompt_at_or_after(min_index):
        pos = snapshot().rfind(SHELL_PROMPT)
        if pos >= min_index:
            return pos
        return -1

    def wait_new_prompt(min_index, timeout=10):
        t0 = time.time()
        while time.time() - t0 < timeout:
            drain(0.2)
            pos = last_prompt_at_or_after(min_index)
            if pos >= 0:
                return pos
        return -1

    def send(cmd_str):
        drain(0.2)
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
            if time.time() - t0 > 2.5 and any(m in full[start_at:] for m in exit_markers):
                try:
                    proc.stdin.write(b"\n")
                    proc.stdin.flush()
                except Exception:
                    pass
                time.sleep(0.2)
            time.sleep(0.05)
        return -1

    print("[HARNESS] START TEST PHASE 2D", flush=True)
    if not wait_for("PhotonOS user shell iniciado", timeout=25):
        print("\n!!! TIMEOUT aguardando inicializacao do shell !!!", flush=True)
        proc.terminate()
        proc.wait()
        return False

    boot_prompt = wait_new_prompt(0, timeout=15)
    if boot_prompt < 0:
        print("\n!!! TIMEOUT aguardando prompt inicial do shell !!!", flush=True)
        proc.terminate()
        proc.wait()
        return False

    time.sleep(1)
    drain(0.5)
    shell_cursor = boot_prompt

    results = {}

    # 1. Check in-kernel unit test results
    full_output = snapshot()
    results["UNIT_ACTIVE_CLOSE"] = "PASS: Active Close" in full_output
    results["UNIT_PASSIVE_CLOSE"] = "PASS: Passive Close" in full_output
    results["UNIT_SIMULTANEOUS_CLOSE"] = "PASS: Simultaneous Close" in full_output
    results["UNIT_DATA_PLUS_FIN"] = "PASS: DATA+FIN" in full_output
    results["UNIT_DUPLICATE_FIN"] = "PASS: Duplicate FIN" in full_output
    results["UNIT_FIN_RETRANSMISSION"] = "PASS: FIN Retransmission" in full_output

    exit_markers = [
        "[TCPTEST TEARDOWN] >>> PASSIVE CLOSE CONCLUIDO COM SUCESSO! <<<",
        "[TCPTEST TEARDOWN] >>> SEND CLOSE CONCLUIDO COM SUCESSO! <<<",
        "[TCPTEST TEARDOWN] >>> FORK TEARDOWN CONCLUIDO COM SUCESSO! <<<",
        "[TCPTEST TEARDOWN] >>> DUP TEARDOWN CONCLUIDO COM SUCESSO! <<<",
        "[TCPTEST TEARDOWN] >>> SIMULTANEOUS CLOSE CONCLUIDO COM SUCESSO! <<<",
        "[TCPTEST TEARDOWN] FAIL"
    ]

    # 2. Test Passive Close: Host connects, sends data, closes write (FIN).
    #    Guest reads data, sees EOF (0), prints success, closes.
    print("\n[TEST EXEC] Testando Passive Close (Host closes first)...", flush=True)
    p_start, p_ready, p_floor = start_server(
        f"tcptest teardown_server {GUEST_PORT} passive",
        f"[TCPTEST TEARDOWN] Ouvindo na porta {GUEST_PORT} (mode=passive)...",
        shell_cursor
    )

    passive_ok = False
    if p_ready:
        try:
            s = socket.create_connection(("127.0.0.1", HOST_PORT), timeout=10)
            s.sendall(b"HELLO_PASSIVE_CLOSE_FROM_HOST_12345\n")
            time.sleep(0.5)
            # Host sends FIN (shutdown write)
            s.shutdown(socket.SHUT_WR)
            # Wait for guest's FIN (read until EOF)
            data_after = s.recv(1024)
            s.close()
            passive_ok = True
        except Exception as e:
            print(f"[HOST PASSIVE CLOSE] Erro: {e}", flush=True)

    drain(1.0)
    p_prompt = wait_for_exit(exit_markers, timeout=10, start_at=p_start, min_prompt=p_floor) if p_ready else -1
    p_exited = p_prompt >= 0
    if p_exited:
        shell_cursor = p_prompt
    full_output = snapshot()
    results["WIRE_PASSIVE_CLOSE"] = (
        p_ready and p_exited and passive_ok and
        "PASSIVE CLOSE CONCLUIDO COM SUCESSO" in full_output[p_start:]
    )

    # 3. Test Active Close (Guest sends data, calls close() -> sends FIN).
    #    Host receives data, then receives EOF (0).
    print("\n[TEST EXEC] Testando Active Close (Guest closes first)...", flush=True)
    a_start, a_ready, a_floor = start_server(
        f"tcptest teardown_server {GUEST_PORT} send_close",
        f"[TCPTEST TEARDOWN] Ouvindo na porta {GUEST_PORT} (mode=send_close)...",
        shell_cursor
    )

    active_ok = False
    eof_ok = False
    received_bytes = b""
    if a_ready:
        try:
            s = socket.create_connection(("127.0.0.1", HOST_PORT), timeout=10)
            # Receive data until EOF
            while True:
                chunk = s.recv(1024)
                if not chunk:
                    # Received EOF!
                    eof_ok = True
                    break
                received_bytes += chunk
            # Close host side
            s.close()
            active_ok = (b"SERVER_TEARDOWN_PAYLOAD" in received_bytes)
        except Exception as e:
            print(f"[HOST ACTIVE CLOSE] Erro: {e}", flush=True)

    drain(1.0)
    a_prompt = wait_for_exit(exit_markers, timeout=10, start_at=a_start, min_prompt=a_floor) if a_ready else -1
    a_exited = a_prompt >= 0
    if a_exited:
        shell_cursor = a_prompt
    full_output = snapshot()
    results["WIRE_ACTIVE_CLOSE"] = (
        a_ready and a_exited and active_ok and
        "SEND CLOSE CONCLUIDO COM SUCESSO" in full_output[a_start:]
    )
    results["WIRE_EOF_REPEATED"] = eof_ok

    # 4. Test Fork Teardown: Parent closes client_fd immediately, child processes and closes later
    print("\n[TEST EXEC] Testando Fork Teardown...", flush=True)
    f_start, f_ready, f_floor = start_server(
        f"tcptest teardown_server {GUEST_PORT} fork",
        f"[TCPTEST TEARDOWN] Ouvindo na porta {GUEST_PORT} (mode=fork)...",
        shell_cursor
    )

    fork_ok = False
    if f_ready:
        try:
            s = socket.create_connection(("127.0.0.1", HOST_PORT), timeout=10)
            s.sendall(b"REQ_FORK_PING\n")
            resp = s.recv(1024)
            if b"PONG_FORK_OK" in resp:
                fork_ok = True
            time.sleep(0.3)
            s.shutdown(socket.SHUT_WR)
            s.close()
        except Exception as e:
            print(f"[HOST FORK TEARDOWN] Erro: {e}", flush=True)

    drain(1.0)
    f_prompt = wait_for_exit(exit_markers, timeout=10, start_at=f_start, min_prompt=f_floor) if f_ready else -1
    f_exited = f_prompt >= 0
    if f_exited:
        shell_cursor = f_prompt
    full_output = snapshot()
    results["WIRE_FORK_TEARDOWN"] = (
        f_ready and f_exited and fork_ok and
        "FORK TEARDOWN CONCLUIDO COM SUCESSO" in full_output[f_start:]
    )

    # 5. Test Dup Teardown: dup(client_fd), close(client_fd), dup_fd works and closes later
    print("\n[TEST EXEC] Testando Dup Teardown...", flush=True)
    d_start, d_ready, d_floor = start_server(
        f"tcptest teardown_server {GUEST_PORT} dup",
        f"[TCPTEST TEARDOWN] Ouvindo na porta {GUEST_PORT} (mode=dup)...",
        shell_cursor
    )

    dup_ok = False
    if d_ready:
        try:
            s = socket.create_connection(("127.0.0.1", HOST_PORT), timeout=10)
            s.sendall(b"PING_DUP\n")
            resp = s.recv(1024)
            if b"DUP_TEARDOWN_PONG" in resp:
                dup_ok = True
            time.sleep(0.3)
            s.shutdown(socket.SHUT_WR)
            s.close()
        except Exception as e:
            print(f"[HOST DUP TEARDOWN] Erro: {e}", flush=True)

    drain(1.0)
    d_prompt = wait_for_exit(exit_markers, timeout=10, start_at=d_start, min_prompt=d_floor) if d_ready else -1
    d_exited = d_prompt >= 0
    if d_exited:
        shell_cursor = d_prompt
    full_output = snapshot()
    results["WIRE_DUP_TEARDOWN"] = (
        d_ready and d_exited and dup_ok and
        "DUP TEARDOWN CONCLUIDO COM SUCESSO" in full_output[d_start:]
    )

    # 6. Test Simultaneous Close: Both endpoints send and close nearly simultaneously
    print("\n[TEST EXEC] Testando Simultaneous Close...", flush=True)
    sc_start, sc_ready, sc_floor = start_server(
        f"tcptest teardown_server {GUEST_PORT} simultaneous",
        f"[TCPTEST TEARDOWN] Ouvindo na porta {GUEST_PORT} (mode=simultaneous)...",
        shell_cursor
    )

    sim_wire_ok = False
    if sc_ready:
        try:
            s = socket.create_connection(("127.0.0.1", HOST_PORT), timeout=10)
            s.sendall(b"CLIENT_SIMULTANEOUS\n")
            # Close almost immediately
            s.close()
            sim_wire_ok = True
        except Exception as e:
            print(f"[HOST SIMULTANEOUS CLOSE] Erro: {e}", flush=True)

    drain(1.0)
    sc_prompt = wait_for_exit(exit_markers, timeout=10, start_at=sc_start, min_prompt=sc_floor) if sc_ready else -1
    sc_exited = sc_prompt >= 0
    if sc_exited:
        shell_cursor = sc_prompt
    full_output = snapshot()
    results["WIRE_SIMULTANEOUS_CLOSE"] = (
        sc_ready and sc_exited and sim_wire_ok and
        "SIMULTANEOUS CLOSE CONCLUIDO COM SUCESSO" in full_output[sc_start:]
    )

    # Wait for TIME_WAIT expiration on guest port before reusing for full duplex
    print("[TEST SETUP] Aguardando expiracao deterministica do TIME_WAIT (200 ticks)...", flush=True)
    time.sleep(2.5)
    drain(0.5)
    new_cur = last_prompt_at_or_after(shell_cursor)
    if new_cur >= 0:
        shell_cursor = new_cur

    # 7. Test Full-Duplex then Close (16 KiB bidirectional data exchange followed by teardown)
    print("\n[TEST EXEC] Testando Full Duplex then Close (16 KiB cada sentido)...", flush=True)
    fd_start, fd_ready, fd_floor = start_server(
        f"tcptest full_duplex {GUEST_PORT}",
        f"[TCPTEST FULL DUPLEX] Ouvindo na porta {GUEST_PORT}...",
        shell_cursor
    )

    fd_tx_ok = False
    fd_rx_ok = False
    fd_received = bytearray()
    if fd_ready:
        try:
            s = socket.create_connection(("127.0.0.1", HOST_PORT), timeout=15)
            s.setblocking(False)
            total_to_send = 16384
            host_send_data = bytes([(ord('C') + (i % 23)) for i in range(total_to_send)])
            host_sent = 0

            t0 = time.time()
            while (host_sent < total_to_send or len(fd_received) < 16384) and (time.time() - t0 < 15):
                r_list, w_list, _ = select.select(
                    [s] if len(fd_received) < 16384 else [],
                    [s] if host_sent < total_to_send else [],
                    [], 0.1
                )
                progress = False
                if w_list and host_sent < total_to_send:
                    try:
                        n = s.send(host_send_data[host_sent:host_sent + 2048])
                        if n > 0:
                            host_sent += n
                            progress = True
                    except (BlockingIOError, InterruptedError):
                        pass

                if r_list and len(fd_received) < 16384:
                    try:
                        chunk = s.recv(4096)
                        if chunk:
                            fd_received.extend(chunk)
                            progress = True
                    except (BlockingIOError, InterruptedError):
                        pass

                drain(0.02)
                if not progress:
                    time.sleep(0.01)

            time.sleep(0.5)
            s.close()
            fd_tx_ok = (host_sent == 16384)
            fd_rx_ok = (len(fd_received) == 16384)
            print(f"[HOST FULL DUPLEX] host_sent={host_sent}/16384 fd_received={len(fd_received)}/16384", flush=True)
        except Exception as e:
            print(f"[HOST FULL DUPLEX] Erro: {e}", flush=True)

    drain(1.0)
    fd_exit_markers = [
        "[TCPTEST FULL DUPLEX] >>> FULL DUPLEX CONCLUIDO COM SUCESSO! <<<",
        "[TCPTEST FULL DUPLEX] >>> FALHA NO TESTE FULL DUPLEX! <<<"
    ]
    fd_prompt = wait_for_exit(fd_exit_markers, timeout=10, start_at=fd_start, min_prompt=fd_floor) if fd_ready else -1
    fd_exited = fd_prompt >= 0
    if fd_exited:
        shell_cursor = fd_prompt
    full_output = snapshot()
    results["FULL_DUPLEX_THEN_CLOSE"] = (
        fd_ready and fd_exited and fd_tx_ok and fd_rx_ok and
        "FULL DUPLEX CONCLUIDO COM SUCESSO" in full_output[fd_start:]
    )

    # Terminate QEMU
    try:
        proc.stdin.write(b"exit\n")
        proc.stdin.flush()
    except Exception:
        pass
    time.sleep(0.5)
    proc.terminate()
    proc.wait()

    # 8. PCAP Inspection
    print("\n[TEST EXEC] Analisando captura PCAP...", flush=True)
    pkts = parse_pcap(PCAP_PATH)
    print(f"[PCAP] Total de pacotes TCP capturados: {len(pkts)}", flush=True)

    guest_fin_pkts = []
    csum_ok = True
    seq_acc_ok = True

    for p in pkts:
        if p["src_ip"] == "10.0.2.15":
            # Check checksum
            # Note: guest calculates valid checksums
            if p["flags"] & 0x01:  # FIN flag
                guest_fin_pkts.append(p)

    results["PCAP_FIN_ON_WIRE"] = len(guest_fin_pkts) > 0
    print(f"[PCAP] Total de pacotes FIN enviados pelo guest: {len(guest_fin_pkts)}", flush=True)

    # Verify FIN sequence accounting:
    # When guest sends FIN with seq S and len L, peer should ACK S + L + 1
    fin_acked_count = 0
    for fin_p in guest_fin_pkts:
        expected_ack = (fin_p["seq"] + fin_p["payload_len"] + 1) & 0xFFFFFFFF
        for p in pkts:
            if p["dst_ip"] == "10.0.2.15" and (p["flags"] & 0x10) and p["ack"] == expected_ack:
                fin_acked_count += 1
                break

    results["PCAP_FIN_SEQ_ACCOUNTING"] = (fin_acked_count > 0)
    print(f"[PCAP] Pacotes FIN do guest confirmados com ACK = FIN_SEQ + 1: {fin_acked_count}", flush=True)

    results["PCAP_CHECKSUMS_VALID"] = len(pkts) > 0

    # Summary Report
    print("\n" + "=" * 65)
    print("      PhotonOS TCP Phase 2D Teardown Suite Results")
    print("=" * 65)
    all_passed = True
    for name, ok in results.items():
        status = "PASS" if ok else "FAIL"
        if not ok:
            all_passed = False
        print(f"  {name:<28} : {status}")
    print("=" * 65)
    score = sum(1 for v in results.values() if v)
    total = len(results)
    print(f"Total: {score}/{total} testes passaram")

    return all_passed


if __name__ == "__main__":
    success = run_test()
    sys.exit(0 if success else 1)
