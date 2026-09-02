import subprocess
import os
import time
import sys

def run_qemu_session(commands, session_name):
    print(f"\n==================================================", flush=True)
    print(f"   STARTING {session_name}", flush=True)
    print(f"==================================================", flush=True)

    proc = subprocess.Popen(
        [
            "qemu-system-x86_64",
            "-smp", "4",
            "-vga", "std",
            "-display", "vnc=:1",
            "-serial", "stdio",
            "-drive", "format=raw,file=build/photon.img,if=floppy",
            "-drive", "format=raw,file=build/disk.img,if=ide,index=0,media=disk",
            "-boot", "a",
            "-monitor", "null"
        ],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        bufsize=0
    )

    os.set_blocking(proc.stdout.fileno(), False)

    output = []
    start_time = time.time()
    cmd_index = 0

    while time.time() - start_time < 14:
        try:
            chunk = proc.stdout.read(1024)
            if chunk:
                text = chunk.decode('ascii', errors='ignore')
                sys.stdout.write(text)
                sys.stdout.flush()
                output.append(text)
        except (OSError, TypeError):
            pass

        elapsed = time.time() - start_time
        if cmd_index < len(commands):
            cmd_time, cmd_str = commands[cmd_index]
            if elapsed >= cmd_time:
                print(f"\n>>> SENDING COMMAND TO SHELL: {cmd_str} <<<", flush=True)
                proc.stdin.write((cmd_str + "\n").encode('ascii'))
                proc.stdin.flush()
                cmd_index += 1

        time.sleep(0.05)

    proc.terminate()
    proc.wait()
    return "".join(output)

if __name__ == "__main__":
    boot1_cmds = [
        (4.0, "persist1")
    ]
    out1 = run_qemu_session(boot1_cmds, "BOOT 1 (WRITE & PERSIST TO FAT16)")

    boot2_cmds = [
        (4.0, "persist2"),
        (6.5, "vfstest"),
        (9.0, "ping 127.0.0.1")
    ]
    out2 = run_qemu_session(boot2_cmds, "BOOT 2 (REBOOT & READ PERSISTENT DATA)")

    print("\n==================================================", flush=True)
    print("             SUMMARY VERIFICATION                 ", flush=True)
    print("==================================================", flush=True)
    full_out = out1 + "\n" + out2

    if "PERSISTENCE REBOOT TEST PASSED 100%" in full_out:
        print(">>> PERSISTENCE REBOOT TEST: PASS <<<", flush=True)
    else:
        print(">>> PERSISTENCE REBOOT TEST: FAILED <<<", flush=True)

    if "ALL TESTS PASSED" in full_out:
        print(">>> RING 3 VFS SUITE: PASS <<<", flush=True)
    else:
        print(">>> RING 3 VFS SUITE: FAILED <<<", flush=True)
