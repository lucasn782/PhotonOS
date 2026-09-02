#!/usr/bin/env python3
import subprocess
import time
import sys
import os

def boot_test(iteration):
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
    output = []
    start = time.time()
    while time.time() - start < 12:
        try:
            chunk = proc.stdout.read(4096)
            if chunk:
                text = chunk.decode("ascii", errors="replace")
                output.append(text)
                if "PhotonOS user shell iniciado" in "".join(output):
                    break
            else:
                time.sleep(0.05)
        except (OSError, TypeError):
            time.sleep(0.05)
    proc.terminate()
    proc.wait()
    full_output = "".join(output)
    
    checks = {
        "BOOT_IDT": "IDT READY" in full_output,
        "PMM": "PMM: inicializado" in full_output,
        "VMM": "VMM Iniciado" in full_output,
        "HEAP": "Heap: kmalloc/kfree inicializados" in full_output,
        "FAT16": "FAT16: volume montado" in full_output,
        "TCP": "[TCP TEST] Finalizada Suite de Testes TCP" in full_output,
        "SMP": "SMP: AP acordado com sucesso" in full_output,
        "SHELL": "PhotonOS user shell iniciado" in full_output,
    }
    
    all_ok = all(checks.values())
    status = "PASS" if all_ok else "FAIL"
    print(f"Boot #{iteration:02d}: {status} - Checks: {checks}", flush=True)
    return all_ok

if __name__ == "__main__":
    results = []
    print("=" * 60, flush=True)
    print("     EXECUTING 10 CONSECUTIVE BOOT REGRESSION TESTS", flush=True)
    print("=" * 60, flush=True)
    for i in range(1, 11):
        ok = boot_test(i)
        results.append(ok)
        time.sleep(0.3)

    print("=" * 60, flush=True)
    print(f"TOTAL SUCCESSFUL BOOTS: {sum(results)} / 10", flush=True)
    print("=" * 60, flush=True)
    if sum(results) == 10:
        sys.exit(0)
    else:
        sys.exit(1)
