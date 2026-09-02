#!/usr/bin/env python3
"""Phase 7 VFS test: sends commands via serial once the shell prompt appears."""
import subprocess
import time
import sys
import os
import select

def run_test():
    proc = subprocess.Popen(
        [
            "qemu-system-x86_64",
            "-smp", "4",
            "-drive", "format=raw,file=build/photon.img,if=floppy",
            "-drive", "format=raw,file=build/disk.img,if=ide,index=0,media=disk",
            "-boot", "a",
            "-serial", "stdio",
            "-display", "none",
            "-monitor", "null",
        ],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        bufsize=0,
    )

    os.set_blocking(proc.stdout.fileno(), False)

    output_parts = []
    start = time.time()

    def drain(timeout=0.3):
        """Read all available output, waiting up to `timeout` seconds for new data."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                chunk = proc.stdout.read(4096)
                if chunk:
                    text = chunk.decode("ascii", errors="replace")
                    sys.stdout.write(text)
                    sys.stdout.flush()
                    output_parts.append(text)
                    deadline = time.time() + timeout  # reset on new data
                else:
                    time.sleep(0.05)
            except (OSError, TypeError):
                time.sleep(0.05)

    def wait_for(pattern, timeout=15):
        """Wait until `pattern` appears in collected output."""
        t0 = time.time()
        while time.time() - t0 < timeout:
            drain(0.2)
            full = "".join(output_parts)
            if pattern in full:
                return True
        return False

    def send(cmd):
        sys.stdout.write(f"\n>>> SENDING: {cmd}\n")
        sys.stdout.flush()
        proc.stdin.write((cmd + "\n").encode("ascii"))
        proc.stdin.flush()

    # Wait for kernel to boot and shell to be ready
    if not wait_for("Thread de rede em background iniciada", timeout=20):
        print("\n!!! TIMEOUT waiting for shell !!!", flush=True)
        proc.terminate()
        proc.wait()
        return "".join(output_parts)

    # Extra wait for scheduler to start user task and shell prompt
    time.sleep(2)
    drain(0.5)

    # Send vfstest
    send("vfstest")
    time.sleep(5)
    drain(1.0)

    # Send smptest (SMP Mass I/O Concurrency Stress Test)
    send("smptest")
    time.sleep(5)
    drain(1.0)

    # Test shell pipe: echo | upper
    send("echo photonpipe | upper")
    time.sleep(2)
    drain(1.0)

    # Test output redirection and input redirection
    send("echo testredirok > /testio.txt")
    time.sleep(2)
    drain(1.0)

    send("cat < /testio.txt")
    time.sleep(2)
    drain(1.0)

    # Send ping
    send("ping 127.0.0.1")
    time.sleep(3)
    drain(1.0)

    proc.terminate()
    proc.wait()
    drain(0.5)

    return "".join(output_parts)


if __name__ == "__main__":
    full = run_test()

    print("\n" + "=" * 60, flush=True)
    print("                RESULT SUMMARY", flush=True)
    print("=" * 60, flush=True)

    checks = {
        "BOOT":            "Long Mode" in full or "IDT READY" in full,
        "PMM":             "PMM: inicializado" in full,
        "VMM":             "VMM Iniciado" in full,
        "HEAP":            "kmalloc/kfree" in full,
        "ATA":             "ATA: disco" in full,
        "FAT16":           "FAT16: volume montado" in full,
        "TCP":             "TCP TEST" in full,
        "SMP":             "AP acordado" in full,
        "SHELL":           "serial COM1 ativo" in full,
        "VFS_ALL_PASS":    "ALL TESTS PASSED" in full,
        "EXT2_MOUNT_PASS": "mount dinamico" in full,
        "SMP_STRESS_PASS": "SMP STRESS" in full and "ALL PASSED" in full,
        "PIPE_PASS":       "PHOTONPIPE" in full or "pipe anonimo e transferencia de dados ok" in full,
        "REDIRECT_PASS":   "testredirok" in full,
    }

    for name, ok in checks.items():
        status = "PASS" if ok else "FAIL"
        print(f"  [{status}]  {name}", flush=True)

    # Show any VFS/SMP test lines
    for line in full.split("\n"):
        if "[VFS TEST]" in line or "[PERSIST" in line or "[SMP STRESS]" in line:
            print(f"  >> {line.strip()}", flush=True)
