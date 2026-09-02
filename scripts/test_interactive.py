#!/usr/bin/env python3
import subprocess
import time
import sys

def test():
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
        text=False,
    )

    # Let it run for 6 seconds, printing output in real time
    time.sleep(4)
    # Send commands
    print("\n--- SENDING vfstest ---\n", flush=True)
    proc.stdin.write(b"vfstest\n")
    proc.stdin.flush()
    time.sleep(3)
    print("\n--- SENDING ping ---\n", flush=True)
    proc.stdin.write(b"ping 127.0.0.1\n")
    proc.stdin.flush()
    time.sleep(3)

    proc.terminate()
    out, _ = proc.communicate(timeout=5)
    print("FULL RAW OUTPUT:\n", flush=True)
    print(out.decode('ascii', errors='replace'), flush=True)

if __name__ == "__main__":
    test()
