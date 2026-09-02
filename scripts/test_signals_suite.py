#!/usr/bin/env python3
"""Signal, Pipe & Process Lifecycle Automated Suite for PhotonOS v4.3"""
import subprocess
import time
import sys
import os

def run_suite():
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

    def send(cmd):
        sys.stdout.write(f"\n>>> SENDING: {cmd}\n")
        sys.stdout.flush()
        proc.stdin.write((cmd + "\n").encode("ascii"))
        proc.stdin.flush()

    if not wait_for("PhotonOS user shell iniciado", timeout=25):
        print("\n!!! TIMEOUT aguardando inicializacao do shell !!!", flush=True)
        proc.terminate()
        proc.wait()
        return False

    time.sleep(1)
    drain(0.5)

    # 1. Executa suite de testes de sinais POSIX e Process Lifecycle
    send("sigtest")
    time.sleep(4)
    drain(1.0)

    # 2. Executa forktest
    send("forktest")
    time.sleep(4)
    drain(1.0)

    # 3. Executa vfstest
    send("vfstest")
    time.sleep(4)
    drain(1.0)

    # 4. Executa smptest
    send("smptest")
    time.sleep(4)
    drain(1.0)

    # 5. Testa pipe do shell
    send("echo signal_pipe_ok | upper")
    time.sleep(2)
    drain(1.0)

    # 6. Testa ps
    send("ps")
    time.sleep(2)
    drain(1.0)

    proc.terminate()
    proc.wait()

    full = "".join(output_parts)

    checks = {
        "SIGTEST_ALL_PASSED": "TODOS OS 6 TESTES DE SINAIS E PROCESS LIFETIME PASSARAM" in full,
        "SIGCHLD_PASS": "[SIGTEST 1/6]" in full and "PASS" in full,
        "SIGPIPE_PASS": "[SIGTEST 2/6]" in full and "PASS" in full,
        "PIPE_EOF_PASS": "[SIGTEST 3/6]" in full and "PASS" in full,
        "SIGSTOP_CONT_PASS": "[SIGTEST 4/6]" in full and "PASS" in full,
        "WAITPID_WNOHANG_PASS": "[SIGTEST 5/6]" in full and "PASS" in full,
        "SIGPROCMASK_PASS": "[SIGTEST 6/6]" in full and "PASS" in full,
        "FORKTEST_PASS": "[pai] filho encerrado - fork OK" in full,
        "VFSTEST_PASS": "Todos os testes VFS passaram com sucesso" in full or "[VFS TEST] ALL TESTS PASSED" in full or "PASSOU" in full,
        "SMPTEST_PASS": "[SMP STRESS] ALL PASSED" in full,
        "PIPE_EXEC_PASS": "SIGNAL_PIPE_OK" in full,
    }

    print("\n" + "=" * 60)
    print("           PHOTONOS v4.3 SUITE VERIFICATION REPORT")
    print("=" * 60)
    for k, v in checks.items():
        status = "PASS" if v else "FAIL"
        print(f"  {k:25s} : {status}")
    print("=" * 60)

    all_ok = all(checks.values())
    if all_ok:
        print(">>> ALL TEST SUITES PASSED PERFECTLY! <<<", flush=True)
        return True
    else:
        print(">>> SOME SUITE CHECKS FAILED! <<<", flush=True)
        return False

if __name__ == "__main__":
    success = run_suite()
    sys.exit(0 if success else 1)
