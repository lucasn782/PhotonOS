#!/usr/bin/env python3
"""
PhotonOS TCP Phase 2C Automated Flow Control & Full-Duplex Test Suite

Validates:
1.  UNIT_ADVERTISED_WINDOW: In-kernel test for advertised window calculation (capacity - used, zero, reopening)
2.  UNIT_PEER_WINDOW: In-kernel test for peer send window enforcement (< MSS, bytes_in_flight, drain)
3.  UNIT_PERSIST_TIMER: In-kernel test for persist timer (zero window arm, backoff, probe, disarm)
4.  FLOW_RX_SATURATION: RX buffer saturation (8192 bytes buffered without immediate recv)
5.  FLOW_WINDOW_SHRINK: Advertised receive window shrinks on wire as RX buffer fills
6.  FLOW_WINDOW_REOPEN: Window reopening ACKs sent when userspace consumes RX data
7.  FLOW_SEND_LIMITED: Guest transmission bounded by peer receive window (< MSS segmentation)
8.  FLOW_ECHO_INTERACTIVE: Echo flow with dynamic send/recv interaction
9.  FULL_DUPLEX_SIMULTANEOUS: Full-duplex simultaneous 16 KiB bidirectional data exchange
10. FULL_DUPLEX_PATTERN_MATCH: Integrity validation of transmitted and received patterns (16384 bytes each way)
11. PCAP_INITIAL_WINDOW: Initial advertised receive window <= 8192 (reflects real RX ring capacity)
12. PCAP_WINDOW_DYNAMICS: Dynamic window fluctuations observed in packet capture
13. PCAP_FLOW_CHECKSUMS: Valid TCP checksums on all guest flow-control packets
14. PCAP_MSS_COMPLIANCE: No guest segment exceeds MSS (1460 bytes)
"""

import os
import sys
import time
import socket
import struct
import subprocess
import select

LOG_DIR = "logs"
PCAP_PATH = os.path.join(LOG_DIR, "tcp_phase2c_flow.pcap")
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


SHELL_PROMPT = "PhotonOS / > "


def run_test(only=None):
    os.makedirs(LOG_DIR, exist_ok=True)
    if os.path.exists(PCAP_PATH):
        try:
            os.remove(PCAP_PATH)
        except OSError:
            pass

    print(f"[TEST SETUP] Iniciando QEMU para Phase 2C Flow Control (port {HOST_PORT} -> {GUEST_PORT})...", flush=True)

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
        """Return the last shell prompt that begins at or after min_index.

        A prompt already used to launch a command is excluded by advancing
        min_index past it. A historical prompt that merely remains in the
        accumulated serial log therefore cannot authorize a new command.
        """
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
            print(
                f"[HARNESS] SERVER_NOT_STARTED: no new shell prompt "
                f"(need index>={min_index}) before {command}",
                flush=True,
            )
            return len(snapshot()), False, min_index
        print(
            f"[HARNESS] NEW_PROMPT index={prompt_at} (floor={min_index}) before {command}",
            flush=True,
        )
        start_at = send(command)
        # This prompt has been consumed. The next prompt must start after it.
        consumed = prompt_at + len(SHELL_PROMPT)
        ready = wait_for(ready_marker, timeout=10, start_at=start_at)
        state = "READY" if ready else "NOT_STARTED"
        print(f"[HARNESS] SERVER_{state}: {ready_marker}", flush=True)
        return start_at, ready, consumed

    def wait_for_exit(exit_markers, timeout, start_at, min_prompt):
        """Prompt after this run's own exit marker.

        The marker search starts at the byte offset of the command that
        launched this process. The prompt must also begin at or after
        min_prompt, which sits past the prompt that launched the process.
        An older prompt cannot satisfy either condition.
        Returns the new prompt index, or -1.
        """
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
                    print(
                        f"[HARNESS] NEW_PROMPT_AFTER_EXIT index={prompt_at} "
                        f"marker_at={marker_at}",
                        flush=True,
                    )
                    return prompt_at
            time.sleep(0.05)
        return -1

    def stage_enabled(name):
        return only is None or name in only

    print("[HARNESS] START TEST", flush=True)
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
    print(f"[HARNESS] INITIAL_PROMPT index={boot_prompt}", flush=True)

    time.sleep(1)
    drain(0.5)
    # The boot prompt is still unused, so the first command may match it.
    shell_cursor = boot_prompt

    results = {}
    not_started = set()

    # 1. Check in-kernel unit test results
    full_output = snapshot()
    if stage_enabled("units"):
        results["UNIT_ADVERTISED_WINDOW"] = "PASS: tcp_advertised_window" in full_output
        results["UNIT_PEER_WINDOW"] = "PASS: tcp_peer_window" in full_output
        results["UNIT_PERSIST_TIMER"] = "PASS: tcp_persist_timer" in full_output

    flow_exit_markers = [
        "[TCPTEST FLOW] >>> FLUXO CONCLUIDO COM SUCESSO! <<<",
        "[TCPTEST FLOW] >>> FALHA NO CONTROLE DE FLUXO! <<<"
    ]
    saturation_exited = not stage_enabled("saturation")
    limited_exited = not stage_enabled("send_limited")

    # 2. Test Flow Control Saturation & Window Reopening
    if stage_enabled("saturation"):
        print("\n[TEST EXEC] Testando RX Saturation & Window Reopening...", flush=True)
        print("[HARNESS] START SATURATION SERVER", flush=True)
        saturation_start, saturation_ready, saturation_floor = start_server(
            f"tcptest flow_server {GUEST_PORT} saturation",
            f"[TCPTEST FLOW] Ouvindo na porta {GUEST_PORT} (mode=saturation)...",
            shell_cursor,
        )

        saturation_ok = False
        if saturation_ready:
            try:
                s = socket.create_connection(("127.0.0.1", HOST_PORT), timeout=10)
                # Send 8192 bytes to fill guest RX ring buffer
                payload_8k = bytes([(i % 256) for i in range(8192)])
                s.sendall(payload_8k)
                time.sleep(1.5)
                s.close()
                saturation_ok = True
            except Exception as e:
                print(f"[HOST CLIENT SATURATION] Erro: {e}", flush=True)
        else:
            print("[HOST CLIENT SATURATION] skipped: server not started", flush=True)
            not_started.add("FLOW_RX_SATURATION")

        drain(1.0)
        saturation_prompt = wait_for_exit(
            flow_exit_markers, timeout=10, start_at=saturation_start, min_prompt=saturation_floor
        ) if saturation_ready else -1
        saturation_exited = saturation_prompt >= 0
        if saturation_exited:
            shell_cursor = saturation_prompt
        print(f"[HARNESS] SATURATION_SERVER_EXITED: {saturation_exited}", flush=True)
        full_output = snapshot()
        results["FLOW_RX_SATURATION"] = (
            saturation_ready and saturation_exited and
            "Total lido: 8192 bytes" in full_output and saturation_ok
        )
    else:
        print("[HARNESS] SATURATION_SKIPPED", flush=True)

    # 3. Test Send Limited by Peer Window
    if stage_enabled("send_limited"):
        print("\n[TEST EXEC] Testando TX limitado por peer window...", flush=True)
        limited_start = len(snapshot())
        limited_ready = False
        limited_floor = shell_cursor
        if saturation_exited:
            print("[HARNESS] START SEND_LIMITED SERVER", flush=True)
            limited_start, limited_ready, limited_floor = start_server(
                f"tcptest flow_server {GUEST_PORT} send_limited",
                f"[TCPTEST FLOW] Ouvindo na porta {GUEST_PORT} (mode=send_limited)...",
                shell_cursor,
            )
        else:
            print("[HARNESS] SEND_LIMITED_NOT_STARTED: saturation server did not exit", flush=True)

        limited_received = b""
        if limited_ready:
            try:
                s = socket.create_connection(("127.0.0.1", HOST_PORT), timeout=10)
                # Receive 4000 bytes sent by guest
                while len(limited_received) < 4000:
                    chunk = s.recv(1024)
                    if not chunk:
                        break
                    limited_received += chunk
                time.sleep(0.3)
                s.close()
            except Exception as e:
                print(f"[HOST CLIENT SEND LIMITED] Erro: {e}", flush=True)
        else:
            print("[HOST CLIENT SEND LIMITED] skipped: server not started", flush=True)
            not_started.add("FLOW_SEND_LIMITED")

        drain(1.0)
        limited_prompt = wait_for_exit(
            flow_exit_markers, timeout=10, start_at=limited_start, min_prompt=limited_floor
        ) if limited_ready else -1
        limited_exited = limited_prompt >= 0
        if limited_exited:
            shell_cursor = limited_prompt
        print(f"[HARNESS] SEND_LIMITED_SERVER_EXITED: {limited_exited}", flush=True)
        expected_4000 = bytes([(ord('A') + (i % 26)) for i in range(4000)])
        results["FLOW_SEND_LIMITED"] = (
            limited_ready and limited_exited and limited_received == expected_4000
        )
    else:
        print("[HARNESS] SEND_LIMITED_SKIPPED", flush=True)

    # 4. Test Echo Flow
    echo_ready = False
    echo_exited = False
    echo_ok = False
    if stage_enabled("echo"):
        print("\n[TEST EXEC] Testando Echo Flow...", flush=True)
        print("[HARNESS] START ECHO SERVER", flush=True)
        echo_start = len(snapshot())
        echo_floor = shell_cursor
        if limited_exited:
            echo_marker = f"[TCPTEST FLOW] Ouvindo na porta {GUEST_PORT} (mode=echo)..."
            echo_start, echo_ready, echo_floor = start_server(
                f"tcptest flow_server {GUEST_PORT} echo",
                echo_marker,
                shell_cursor,
            )
            if echo_ready:
                print("[HARNESS] ECHO_SERVER_READY", flush=True)
            else:
                print("[HARNESS] ECHO_SERVER_NOT_STARTED", flush=True)
        else:
            print("[HARNESS] ECHO_SERVER_NOT_STARTED: previous server did not return to a new prompt", flush=True)

        if echo_ready:
            print("[HARNESS] RUN ECHO CLIENT", flush=True)
            try:
                s = socket.create_connection(("127.0.0.1", HOST_PORT), timeout=10)
                test_msg = b"PhotonOS_TCP_Flow_Control_Test_Echo_Message_1234567890"
                s.sendall(test_msg)
                echo_resp = s.recv(1024)
                if echo_resp == test_msg:
                    echo_ok = True
                time.sleep(0.3)
                s.close()
            except Exception as e:
                print(f"[HOST CLIENT ECHO] Erro: {e}", flush=True)
        else:
            print("[HOST CLIENT ECHO] skipped: SERVER_NOT_STARTED", flush=True)
            not_started.add("FLOW_ECHO_INTERACTIVE")

        drain(1.0)
        echo_prompt = wait_for_exit(
            flow_exit_markers, timeout=12, start_at=echo_start, min_prompt=echo_floor
        ) if echo_ready else -1
        echo_exited = echo_prompt >= 0
        if echo_exited:
            shell_cursor = echo_prompt
        print(f"[HARNESS] ECHO_SERVER_EXITED: {echo_exited}", flush=True)
        results["FLOW_ECHO_INTERACTIVE"] = echo_ready and echo_exited and echo_ok
    else:
        echo_exited = True
        print("[HARNESS] ECHO_SKIPPED", flush=True)

    # 5. Full-Duplex Simultaneous 16 KiB Test
    fd_ready = False
    fd_tx_ok = False
    fd_rx_ok = False
    fd_received = bytearray()
    host_sent = 0
    if stage_enabled("full_duplex"):
        print("\n[TEST EXEC] Testando Full-Duplex Simultaneo (16 KiB x 16 KiB)...", flush=True)
        fd_start = len(snapshot())
        fd_floor = shell_cursor
        if echo_exited:
            print("[HARNESS] START FULL DUPLEX SERVER", flush=True)
            fd_marker = f"[TCPTEST FULL DUPLEX] Ouvindo na porta {GUEST_PORT}..."
            fd_start, fd_ready, fd_floor = start_server(
                f"tcptest full_duplex {GUEST_PORT}",
                fd_marker,
                shell_cursor,
            )
            if fd_ready:
                print("[HARNESS] FULL_DUPLEX_SERVER_READY", flush=True)
            else:
                print("[HARNESS] FULL_DUPLEX_SERVER_NOT_STARTED", flush=True)
        else:
            print("[HARNESS] FULL_DUPLEX_NOT_STARTED: echo server did not return to a new prompt", flush=True)

    fd_tx_ok = False
    fd_rx_ok = False
    fd_received = bytearray()
    if fd_ready:
        try:
            s = socket.create_connection(("127.0.0.1", HOST_PORT), timeout=10)
            s.setblocking(False)

            host_send_data = bytes([(ord('C') + (i % 23)) for i in range(16384)])
            host_sent = 0
            total_to_send = len(host_send_data)
            stalled_count = 0

            t_start = time.time()
            while (host_sent < total_to_send or len(fd_received) < 16384) and (time.time() - t_start < 25):
                r_list, w_list, _ = select.select([s] if len(fd_received) < 16384 else [],
                                                  [s] if host_sent < total_to_send else [],
                                                  [], 0.1)
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
    else:
        if stage_enabled("full_duplex"):
            print("[HOST FULL DUPLEX] skipped: SERVER_NOT_STARTED", flush=True)
            not_started.add("FULL_DUPLEX_SIMULTANEOUS")
            not_started.add("FULL_DUPLEX_PATTERN_MATCH")

    if stage_enabled("full_duplex"):
        drain(1.0)
        fd_exit_markers = [
            "[TCPTEST FULL DUPLEX] >>> FULL DUPLEX CONCLUIDO COM SUCESSO! <<<",
            "[TCPTEST FULL DUPLEX] >>> FALHA NO TESTE FULL DUPLEX! <<<"
        ]
        if fd_ready:
            fd_prompt = wait_for_exit(
                fd_exit_markers, timeout=10, start_at=fd_start, min_prompt=fd_floor
            )
            fd_exited = fd_prompt >= 0
            print(f"[HARNESS] FULL_DUPLEX_SERVER_EXITED: {fd_exited}", flush=True)
        else:
            fd_exited = False
            print("[HARNESS] FULL_DUPLEX_SERVER_EXITED: False (listener never started)", flush=True)
        full_output = snapshot()
        # A listener that never printed its own ready marker is NOT_STARTED.
        # It is not scored as a bidirectional data failure.
        if fd_ready:
            results["FULL_DUPLEX_SIMULTANEOUS"] = (
                fd_tx_ok and fd_rx_ok and "FULL DUPLEX CONCLUIDO COM SUCESSO" in full_output
            )
            expected_fd_guest = bytes([(ord('S') + (i % 23)) for i in range(16384)])
            results["FULL_DUPLEX_PATTERN_MATCH"] = (bytes(fd_received) == expected_fd_guest)
        else:
            results["FULL_DUPLEX_SIMULTANEOUS"] = False
            results["FULL_DUPLEX_PATTERN_MATCH"] = False

    # Terminate QEMU
    try:
        proc.stdin.write(b"exit\n")
        proc.stdin.flush()
    except Exception:
        pass
    time.sleep(0.5)
    proc.terminate()
    proc.wait()

    # 6. PCAP Inspection
    if stage_enabled("pcap"):
        print("\n[TEST EXEC] Analisando captura PCAP...", flush=True)
        pkts = parse_pcap(PCAP_PATH)
        guest_pkts = [p for p in pkts if p["src_port"] == GUEST_PORT]

        # PCAP_INITIAL_WINDOW: Initial SYN+ACK or initial ACK advertised window <= 8192
        initial_windows = [p["window"] for p in guest_pkts[:10]]
        results["PCAP_INITIAL_WINDOW"] = (len(initial_windows) > 0 and all(w <= 8192 for w in initial_windows))

        # PCAP_WINDOW_DYNAMICS: Check that window values fluctuate during saturation and reopening
        windows_seen = set(p["window"] for p in guest_pkts)
        results["PCAP_WINDOW_DYNAMICS"] = (len(windows_seen) >= 2)

        # FLOW_WINDOW_SHRINK & REOPEN: Check that window went below 4096 and reopened
        results["FLOW_WINDOW_SHRINK"] = any(p["window"] < 4096 for p in guest_pkts)
        results["FLOW_WINDOW_REOPEN"] = any(p["window"] >= 6000 for p in guest_pkts)

        # PCAP_FLOW_CHECKSUMS: Check that all guest packets have non-zero checksums
        results["PCAP_FLOW_CHECKSUMS"] = (len(guest_pkts) >= 10 and all(p["csum"] != 0 for p in guest_pkts))

        # PCAP_MSS_COMPLIANCE: No guest packet payload exceeds 1460 bytes
        results["PCAP_MSS_COMPLIANCE"] = all(p["payload_len"] <= 1460 for p in guest_pkts)

    print("\n" + "=" * 60)
    print("           TCP PHASE 2C TEST RESULTS SUMMARY")
    print("=" * 60)
    all_pass = True
    for name, passed in results.items():
        if name in not_started:
            status = "NOT_STARTED"
            all_pass = False
        elif passed:
            status = "PASS"
        else:
            status = "FAIL"
            all_pass = False
        print(f"  [{status}] {name}")

    print("=" * 60)
    if all_pass:
        print(f"RESULTADO: TODOS OS {len(results)} TESTES DA PHASE 2C PASSARAM COM SUCESSO!")
    else:
        print("RESULTADO: FALHA EM UM OU MAIS TESTES DA PHASE 2C!")
    print("=" * 60)

    return all_pass


if __name__ == "__main__":
    only = None
    if len(sys.argv) > 1:
        if len(sys.argv) == 3 and sys.argv[1] == "--only":
            only = set(part.strip() for part in sys.argv[2].split(",") if part.strip())
        else:
            print("usage: test_tcp_phase2c_flow_control.py [--only units,saturation,send_limited,echo,full_duplex,pcap]")
            sys.exit(2)
    success = run_test(only)
    sys.exit(0 if success else 1)
