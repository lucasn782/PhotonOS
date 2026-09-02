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
            "-display", "none"
        ],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True
    )

    time.sleep(3)
    proc.stdin.write("vfstest\n")
    proc.stdin.flush()
    time.sleep(2)
    proc.stdin.write("ping 127.0.0.1\n")
    proc.stdin.flush()
    time.sleep(3)
    proc.terminate()
    out, _ = proc.communicate()
    print("OUTPUT:\n" + out)

if __name__ == "__main__":
    test()
