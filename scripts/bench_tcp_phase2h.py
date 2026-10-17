#!/usr/bin/env python3
"""
PhotonOS TCP Phase 2H — Benchmark Suite
scripts/bench_tcp_phase2h.py

Reproducible TCP performance benchmark measuring:
1. TX Single Stream 4 KiB (Short payload)
2. TX Single Stream 8 KiB (Medium payload, matches internal buffer)
3. TX Single Stream 12 KiB (Large payload, exceeds 8 KiB internal buffer)
4. RX Single Stream 8 KiB (Saturation flow control)
5. Full-Duplex 32 KiB (Simultaneous 16 KiB TX + 16 KiB RX)

Outputs results in JSON and prints summary statistics (mean, median, min, max, stddev).
Supports comparison mode: --compare baseline.json
"""

import os
import sys
import time
import socket
import struct
import subprocess
import select
import json
import argparse
import statistics

LOG_DIR = "logs"
PCAP_PATH = os.path.join(LOG_DIR, "bench_tcp_phase2h.pcap")
GUEST_PORT = 8088
HOST_PORT = 18088
SHELL_PROMPT = "PhotonOS / > "


def parse_pcap_metrics(pcap_path):
    if not os.path.exists(pcap_path):
        return {"total_packets": 0, "tcp_packets": 0, "guest_data_bytes": 0, "host_data_bytes": 0}

    total_packets = 0
    tcp_packets = 0
    guest_data_bytes = 0
    host_data_bytes = 0

    with open(pcap_path, "rb") as f:
        global_hdr = f.read(24)
        if len(global_hdr) < 24:
            return {"total_packets": 0, "tcp_packets": 0, "guest_data_bytes": 0, "host_data_bytes": 0}

        while True:
            pkt_hdr = f.read(16)
            if len(pkt_hdr) < 16:
                break
            ts_sec, ts_usec, incl_len, orig_len = struct.unpack("<IIII", pkt_hdr)
            data = f.read(incl_len)
            if len(data) < incl_len:
                break

            total_packets += 1
            if len(data) < 14 + 20 + 20:
                continue

            eth_type = struct.unpack("!H", data[12:14])[0]
            if eth_type != 0x0800:
                continue

            ip_data = data[14:]
            ver_ihl = ip_data[0]
            ihl = (ver_ihl & 0x0F) * 4
            protocol = ip_data[9]
            src_ip = socket.inet_ntoa(ip_data[12:16])
            dst_ip = socket.inet_ntoa(ip_data[16:20])

            if protocol != 6:
                continue

            tcp_packets += 1
            tcp_data = ip_data[ihl:]
            if len(tcp_data) < 20:
                continue

            data_offset = ((tcp_data[12] >> 4) & 0x0F) * 4
            payload_len = len(tcp_data) - data_offset

            if src_ip == "10.0.2.15":  # Guest -> Host
                guest_data_bytes += payload_len
            else:  # Host -> Guest
                host_data_bytes += payload_len

    return {
        "total_packets": total_packets,
        "tcp_packets": tcp_packets,
        "guest_data_bytes": guest_data_bytes,
        "host_data_bytes": host_data_bytes
    }


def run_benchmark(iterations=3, output_json=None):
    os.makedirs(LOG_DIR, exist_ok=True)
    if os.path.exists(PCAP_PATH):
        try:
            os.remove(PCAP_PATH)
        except OSError:
            pass

    print(f"[BENCH SETUP] Starting QEMU for TCP Phase 2H Benchmark...", flush=True)

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

    def snapshot():
        return "".join(output_parts)

    def wait_for(pattern, timeout=25, start_at=0):
        t0 = time.time()
        while time.time() - t0 < timeout:
            drain(0.2)
            if pattern in snapshot()[start_at:]:
                return True
        return False

    def wait_new_prompt(min_index, timeout=10):
        t0 = time.time()
        while time.time() - t0 < timeout:
            drain(0.2)
            pos = snapshot().rfind(SHELL_PROMPT)
            if pos >= min_index:
                return pos
            if time.time() - t0 > 2.0:
                try:
                    proc.stdin.write(b"\n")
                    proc.stdin.flush()
                except (OSError, BrokenPipeError):
                    pass
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
        prompt_at = wait_new_prompt(min_index, timeout=15)
        if prompt_at < 0:
            print(f"[HARNESS] SERVER_NOT_STARTED: no prompt before {command}", flush=True)
            return len(snapshot()), False, min_index
        start_at = send(command)
        consumed = prompt_at + len(SHELL_PROMPT)
        ready = wait_for(ready_marker, timeout=12, start_at=start_at)
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
                    return prompt_at
            if time.time() - t0 > 2.0 and any(m in full[start_at:] for m in exit_markers):
                try:
                    proc.stdin.write(b"\n")
                    proc.stdin.flush()
                except (OSError, BrokenPipeError):
                    pass
        return -1

    # Wait for boot
    print("[BENCH SETUP] Waiting for boot prompt...", flush=True)
    boot_ok = wait_for(SHELL_PROMPT, timeout=30)
    if not boot_ok:
        print("[BENCH SETUP] FAIL: System did not reach shell prompt!", flush=True)
        proc.kill()
        return None

    last_prompt = snapshot().rfind(SHELL_PROMPT)
    bench_results = {
        "metadata": {
            "date": time.strftime("%Y-%m-%d %H:%M:%S"),
            "qemu_smp": 4,
            "iterations": iterations,
            "guest_port": GUEST_PORT,
            "host_port": HOST_PORT
        },
        "scenarios": {}
    }

    # Helper to benchmark TX scenarios
    def bench_tx_scenario(scenario_name, tcptest_mode, expected_bytes):
        runs = []
        nonlocal last_prompt
        for i in range(iterations):
            print(f"\n--- [BENCH] {scenario_name} (iteration {i+1}/{iterations}) ---", flush=True)
            cmd = f"tcptest cc_server {GUEST_PORT} {tcptest_mode}"
            start_at, ready, floor = start_server(cmd, "[TCPTEST CC] Ouvindo na porta", last_prompt)
            if not ready:
                print(f"[BENCH] Server failed to start for {scenario_name}", flush=True)
                continue

            rx_data = bytearray()
            t0 = time.perf_counter()
            try:
                s = socket.create_connection(("127.0.0.1", HOST_PORT), timeout=10)
                s.settimeout(5.0)
                while len(rx_data) < expected_bytes:
                    chunk = s.recv(4096)
                    if not chunk:
                        break
                    rx_data.extend(chunk)
                t1 = time.perf_counter()
                s.close()
            except Exception as e:
                t1 = time.perf_counter()
                print(f"[BENCH] Socket error: {e}", flush=True)

            elapsed_ms = (t1 - t0) * 1000.0
            bytes_received = len(rx_data)
            throughput_kb_s = (bytes_received / 1024.0) / (t1 - t0) if (t1 - t0) > 0 else 0

            exit_prompt = wait_for_exit(
                "[TCPTEST CC] >>> CONGESTION CONTROL CONCLUIDO COM SUCESSO! <<<",
                timeout=10, start_at=start_at, min_prompt=floor
            )
            if exit_prompt >= 0:
                last_prompt = exit_prompt

            print(f"[BENCH RESULT] {scenario_name} iter {i+1}: {bytes_received}/{expected_bytes} bytes in {elapsed_ms:.2f} ms ({throughput_kb_s:.2f} KB/s)", flush=True)
            if bytes_received == expected_bytes:
                runs.append({
                    "bytes": bytes_received,
                    "elapsed_ms": elapsed_ms,
                    "throughput_kb_s": throughput_kb_s
                })
            time.sleep(0.5)

        if runs:
            throughputs = [r["throughput_kb_s"] for r in runs]
            elapseds = [r["elapsed_ms"] for r in runs]
            bench_results["scenarios"][scenario_name] = {
                "expected_bytes": expected_bytes,
                "successful_runs": len(runs),
                "throughput_median_kb_s": statistics.median(throughputs),
                "throughput_mean_kb_s": statistics.mean(throughputs),
                "throughput_min_kb_s": min(throughputs),
                "throughput_max_kb_s": max(throughputs),
                "throughput_stddev_kb_s": statistics.stdev(throughputs) if len(throughputs) > 1 else 0.0,
                "elapsed_median_ms": statistics.median(elapseds),
                "elapsed_mean_ms": statistics.mean(elapseds),
                "runs": runs
            }

    # Helper to benchmark RX saturation
    def bench_rx_scenario(scenario_name, expected_bytes=8192):
        runs = []
        nonlocal last_prompt
        for i in range(iterations):
            print(f"\n--- [BENCH] {scenario_name} (iteration {i+1}/{iterations}) ---", flush=True)
            cmd = f"tcptest flow_server {GUEST_PORT} saturation"
            start_at, ready, floor = start_server(cmd, "[TCPTEST FLOW] Ouvindo na porta", last_prompt)
            if not ready:
                print(f"[BENCH] Server failed to start for {scenario_name}", flush=True)
                continue

            host_data = bytes([(ord('A') + (b % 26)) for b in range(expected_bytes)])
            sent = 0
            t0 = time.perf_counter()
            try:
                s = socket.create_connection(("127.0.0.1", HOST_PORT), timeout=10)
                while sent < expected_bytes:
                    n = s.send(host_data[sent:sent+2048])
                    if n > 0:
                        sent += n
                    time.sleep(0.01)
                t1 = time.perf_counter()
                time.sleep(0.2)
                s.close()
            except Exception as e:
                t1 = time.perf_counter()
                print(f"[BENCH] Socket error: {e}", flush=True)

            elapsed_ms = (t1 - t0) * 1000.0
            throughput_kb_s = (sent / 1024.0) / (t1 - t0) if (t1 - t0) > 0 else 0

            exit_prompt = wait_for_exit(
                "[TCPTEST FLOW] >>> FLUXO CONCLUIDO COM SUCESSO! <<<",
                timeout=12, start_at=start_at, min_prompt=floor
            )
            if exit_prompt >= 0:
                last_prompt = exit_prompt

            print(f"[BENCH RESULT] {scenario_name} iter {i+1}: sent {sent}/{expected_bytes} bytes in {elapsed_ms:.2f} ms ({throughput_kb_s:.2f} KB/s)", flush=True)
            if sent == expected_bytes:
                runs.append({
                    "bytes": sent,
                    "elapsed_ms": elapsed_ms,
                    "throughput_kb_s": throughput_kb_s
                })
            time.sleep(0.5)

        if runs:
            throughputs = [r["throughput_kb_s"] for r in runs]
            elapseds = [r["elapsed_ms"] for r in runs]
            bench_results["scenarios"][scenario_name] = {
                "expected_bytes": expected_bytes,
                "successful_runs": len(runs),
                "throughput_median_kb_s": statistics.median(throughputs),
                "throughput_mean_kb_s": statistics.mean(throughputs),
                "throughput_min_kb_s": min(throughputs),
                "throughput_max_kb_s": max(throughputs),
                "throughput_stddev_kb_s": statistics.stdev(throughputs) if len(throughputs) > 1 else 0.0,
                "elapsed_median_ms": statistics.median(elapseds),
                "elapsed_mean_ms": statistics.mean(elapseds),
                "runs": runs
            }

    # Helper to benchmark Full-Duplex
    def bench_full_duplex_scenario(scenario_name, bytes_per_dir=16384):
        runs = []
        nonlocal last_prompt
        for i in range(iterations):
            print(f"\n--- [BENCH] {scenario_name} (iteration {i+1}/{iterations}) ---", flush=True)
            cmd = f"tcptest full_duplex {GUEST_PORT}"
            start_at, ready, floor = start_server(cmd, "[TCPTEST FULL DUPLEX] Ouvindo na porta", last_prompt)
            if not ready:
                print(f"[BENCH] Server failed to start for {scenario_name}", flush=True)
                continue

            host_send_data = bytes([(ord('C') + (b % 23)) for b in range(bytes_per_dir)])
            host_sent = 0
            fd_received = bytearray()
            t0 = time.perf_counter()
            try:
                s = socket.create_connection(("127.0.0.1", HOST_PORT), timeout=10)
                s.setblocking(False)
                t_loop_start = time.time()
                while (host_sent < bytes_per_dir or len(fd_received) < bytes_per_dir) and (time.time() - t_loop_start < 25):
                    r_list, w_list, _ = select.select(
                        [s] if len(fd_received) < bytes_per_dir else [],
                        [s] if host_sent < bytes_per_dir else [],
                        [], 0.05
                    )
                    progress = False
                    if w_list and host_sent < bytes_per_dir:
                        try:
                            n = s.send(host_send_data[host_sent:host_sent + 2048])
                            if n > 0:
                                host_sent += n
                                progress = True
                        except (BlockingIOError, InterruptedError):
                            pass
                    if r_list and len(fd_received) < bytes_per_dir:
                        try:
                            chunk = s.recv(4096)
                            if chunk:
                                fd_received.extend(chunk)
                                progress = True
                        except (BlockingIOError, InterruptedError):
                            pass
                    drain(0.01)
                    if not progress:
                        time.sleep(0.01)

                t1 = time.perf_counter()
                time.sleep(0.2)
                s.close()
            except Exception as e:
                t1 = time.perf_counter()
                print(f"[BENCH] Full duplex error: {e}", flush=True)

            elapsed_ms = (t1 - t0) * 1000.0
            total_bytes = host_sent + len(fd_received)
            throughput_kb_s = (total_bytes / 1024.0) / (t1 - t0) if (t1 - t0) > 0 else 0

            exit_prompt = wait_for_exit(
                "[TCPTEST FULL DUPLEX] >>> FULL DUPLEX CONCLUIDO COM SUCESSO! <<<",
                timeout=12, start_at=start_at, min_prompt=floor
            )
            if exit_prompt >= 0:
                last_prompt = exit_prompt

            print(f"[BENCH RESULT] {scenario_name} iter {i+1}: sent {host_sent}/{bytes_per_dir}, recv {len(fd_received)}/{bytes_per_dir} in {elapsed_ms:.2f} ms ({throughput_kb_s:.2f} KB/s total)", flush=True)
            if host_sent == bytes_per_dir and len(fd_received) == bytes_per_dir:
                runs.append({
                    "tx_bytes": host_sent,
                    "rx_bytes": len(fd_received),
                    "total_bytes": total_bytes,
                    "elapsed_ms": elapsed_ms,
                    "throughput_kb_s": throughput_kb_s
                })
            time.sleep(0.5)

        if runs:
            throughputs = [r["throughput_kb_s"] for r in runs]
            elapseds = [r["elapsed_ms"] for r in runs]
            bench_results["scenarios"][scenario_name] = {
                "expected_bytes_total": bytes_per_dir * 2,
                "successful_runs": len(runs),
                "throughput_median_kb_s": statistics.median(throughputs),
                "throughput_mean_kb_s": statistics.mean(throughputs),
                "throughput_min_kb_s": min(throughputs),
                "throughput_max_kb_s": max(throughputs),
                "throughput_stddev_kb_s": statistics.stdev(throughputs) if len(throughputs) > 1 else 0.0,
                "elapsed_median_ms": statistics.median(elapseds),
                "elapsed_mean_ms": statistics.mean(elapseds),
                "runs": runs
            }

    # Execute all scenarios
    bench_tx_scenario("TX_SINGLE_STREAM_4KB", "loss_recovery", 4000)
    bench_tx_scenario("TX_SINGLE_STREAM_8KB", "slow_start", 8192)
    bench_tx_scenario("TX_SINGLE_STREAM_12KB", "fast_recovery", 12000)
    bench_rx_scenario("RX_SATURATION_8KB", 8192)
    bench_full_duplex_scenario("FULL_DUPLEX_32KB_TOTAL", 16384)

    # Clean shutdown
    try:
        proc.stdin.write(b"exit\n")
        proc.stdin.flush()
        proc.wait(timeout=5)
    except Exception:
        proc.kill()

    # Capture PCAP metrics
    pcap_metrics = parse_pcap_metrics(PCAP_PATH)
    bench_results["pcap"] = pcap_metrics

    # Print summary table
    print("\n" + "=" * 80)
    print("PHOTONOS TCP PHASE 2H BENCHMARK SUMMARY")
    print("=" * 80)
    print(f"{'Scenario':<28} | {'Median Tput (KB/s)':<20} | {'Median Time (ms)':<18} | {'Runs'}")
    print("-" * 80)
    for name, data in bench_results["scenarios"].items():
        print(f"{name:<28} | {data['throughput_median_kb_s']:<20.2f} | {data['elapsed_median_ms']:<18.2f} | {data['successful_runs']}/{iterations}")
    print("-" * 80)
    print(f"PCAP Metrics: {pcap_metrics['total_packets']} total pkts, {pcap_metrics['tcp_packets']} TCP pkts, {pcap_metrics['guest_data_bytes']}B guest payload, {pcap_metrics['host_data_bytes']}B host payload")
    print("=" * 80 + "\n")

    if output_json:
        with open(output_json, "w") as f:
            json.dump(bench_results, f, indent=2)
        print(f"[BENCH] Saved results to {output_json}")

    return bench_results


def compare_benchmarks(baseline_file, current_file):
    with open(baseline_file, "r") as f:
        base = json.load(f)
    with open(current_file, "r") as f:
        curr = json.load(f)

    print("\n" + "=" * 90)
    print("PHOTONOS TCP PHASE 2H — BEFORE / AFTER COMPARISON")
    print("=" * 90)
    print(f"{'Scenario':<26} | {'Baseline (KB/s)':<16} | {'Optimized (KB/s)':<16} | {'Speedup':<12} | {'Time Diff'}")
    print("-" * 90)

    for name in base["scenarios"]:
        if name in curr["scenarios"]:
            b_data = base["scenarios"][name]
            c_data = curr["scenarios"][name]
            b_tput = b_data["throughput_median_kb_s"]
            c_tput = c_data["throughput_median_kb_s"]
            speedup = ((c_tput - b_tput) / b_tput) * 100.0 if b_tput > 0 else 0.0
            b_time = b_data["elapsed_median_ms"]
            c_time = c_data["elapsed_median_ms"]
            time_diff = c_time - b_time
            sign = "+" if speedup >= 0 else ""
            print(f"{name:<26} | {b_tput:<16.2f} | {c_tput:<16.2f} | {sign}{speedup:<10.2f}% | {time_diff:+.1f} ms")
    print("=" * 90 + "\n")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="PhotonOS TCP Phase 2H Benchmark")
    parser.add_argument("--iterations", type=int, default=3, help="Number of repetitions per scenario")
    parser.add_argument("--output", type=str, default="logs/bench_baseline.json", help="Output JSON path")
    parser.add_argument("--compare", type=str, default=None, help="Compare current output with a baseline JSON")
    args = parser.parse_args()

    if args.compare:
        if os.path.exists(args.compare) and os.path.exists(args.output):
            compare_benchmarks(args.compare, args.output)
        else:
            print(f"Error: files for comparison not found ({args.compare}, {args.output})")
    else:
        run_benchmark(iterations=args.iterations, output_json=args.output)
