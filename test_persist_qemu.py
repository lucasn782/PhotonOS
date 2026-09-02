import subprocess
import threading
import time
import sys

def stream_output(proc, prefix, lines_out):
    try:
        while True:
            line = proc.stdout.readline()
            if not line:
                break
            print(f"[{prefix}] {line.strip()}", flush=True)
            lines_out.append(line)
    except Exception:
        pass

def run_boot1():
    print("=== [BOOT #1] Starting PhotonOS QEMU ===", flush=True)
    proc = subprocess.Popen(
        [
            "qemu-system-x86_64",
            "-smp", "4",
            "-serial", "stdio",
            "-drive", "format=raw,file=build/photon.img",
            "-drive", "format=raw,file=build/disk.img",
            "-display", "none",
            "-monitor", "null"
        ],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1
    )
    lines = []
    t = threading.Thread(target=stream_output, args=(proc, "BOOT1", lines))
    t.daemon = True
    t.start()

    time.sleep(3.5)
    print("=== [BOOT #1] Sending command: persist1 ===", flush=True)
    proc.stdin.write("persist1\n")
    proc.stdin.flush()
    time.sleep(3)
    proc.terminate()
    proc.wait()
    return "".join(lines)

def run_boot2():
    print("=== [BOOT #2 REBOOT] Starting PhotonOS QEMU after Reboot ===", flush=True)
    proc = subprocess.Popen(
        [
            "qemu-system-x86_64",
            "-smp", "4",
            "-serial", "stdio",
            "-drive", "format=raw,file=build/photon.img",
            "-drive", "format=raw,file=build/disk.img",
            "-display", "none",
            "-monitor", "null"
        ],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1
    )
    lines = []
    t = threading.Thread(target=stream_output, args=(proc, "BOOT2", lines))
    t.daemon = True
    t.start()

    time.sleep(3.5)
    print("=== [BOOT #2] Sending command: persist2 ===", flush=True)
    proc.stdin.write("persist2\n")
    proc.stdin.flush()
    time.sleep(3)

    print("=== [BOOT #2] Sending command: vfstest ===", flush=True)
    proc.stdin.write("vfstest\n")
    proc.stdin.flush()
    time.sleep(3)

    print("=== [BOOT #2] Sending command: ping 127.0.0.1 ===", flush=True)
    proc.stdin.write("ping 127.0.0.1\n")
    proc.stdin.flush()
    time.sleep(4)

    proc.terminate()
    proc.wait()
    return "".join(lines)

if __name__ == "__main__":
    out1 = run_boot1()
    out2 = run_boot2()
