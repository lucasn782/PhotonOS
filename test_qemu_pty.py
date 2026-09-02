import pty
import os
import subprocess
import time
import select
import sys

def run_boot1():
    print("=== [BOOT #1] Starting PhotonOS QEMU with PTY ===", flush=True)
    master, slave = pty.openpty()
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
        stdin=slave,
        stdout=slave,
        stderr=slave,
        close_fds=True
    )
    os.close(slave)

    output = []
    start_time = time.time()
    sent = False

    while time.time() - start_time < 8:
        r, _, _ = select.select([master], [], [], 0.2)
        if master in r:
            try:
                data = os.read(master, 1024)
                if data:
                    text = data.decode("ascii", errors="ignore")
                    sys.stdout.write(text)
                    sys.stdout.flush()
                    output.append(text)
            except OSError:
                break

        if not sent and time.time() - start_time > 3.5:
            print("\n>>> SENDING COMMAND: persist1 <<<", flush=True)
            os.write(master, b"persist1\n")
            sent = True

    proc.terminate()
    proc.wait()
    os.close(master)
    return "".join(output)

def run_boot2():
    print("\n=== [BOOT #2 REBOOT] Starting PhotonOS QEMU after Reboot with PTY ===", flush=True)
    master, slave = pty.openpty()
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
        stdin=slave,
        stdout=slave,
        stderr=slave,
        close_fds=True
    )
    os.close(slave)

    output = []
    start_time = time.time()
    stage = 0

    while time.time() - start_time < 12:
        r, _, _ = select.select([master], [], [], 0.2)
        if master in r:
            try:
                data = os.read(master, 1024)
                if data:
                    text = data.decode("ascii", errors="ignore")
                    sys.stdout.write(text)
                    sys.stdout.flush()
                    output.append(text)
            except OSError:
                break

        elapsed = time.time() - start_time
        if stage == 0 and elapsed > 3.5:
            print("\n>>> SENDING COMMAND: persist2 <<<", flush=True)
            os.write(master, b"persist2\n")
            stage = 1
        elif stage == 1 and elapsed > 6.0:
            print("\n>>> SENDING COMMAND: vfstest <<<", flush=True)
            os.write(master, b"vfstest\n")
            stage = 2
        elif stage == 2 and elapsed > 8.5:
            print("\n>>> SENDING COMMAND: ping 127.0.0.1 <<<", flush=True)
            os.write(master, b"ping 127.0.0.1\n")
            stage = 3

    proc.terminate()
    proc.wait()
    os.close(master)
    return "".join(output)

if __name__ == "__main__":
    out1 = run_boot1()
    out2 = run_boot2()
    print("=== SUMMARY BOOT 1 ===")
    print(out1)
    print("=== SUMMARY BOOT 2 ===")
    print(out2)
