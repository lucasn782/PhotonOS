#!/usr/bin/env python3
"""Observable QEMU checks for ELF argv/envp construction and exec replacement."""
import os
import select
import subprocess
import time


def main():
    proc = subprocess.Popen(
        ["qemu-system-x86_64", "-smp", "4",
         "-drive", "format=raw,file=build/photon.img,if=floppy",
         "-drive", "format=raw,file=build/disk.img,if=ide,index=0,media=disk",
         "-boot", "a", "-serial", "stdio", "-display", "none", "-monitor", "null"],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        bufsize=0)
    os.set_blocking(proc.stdout.fileno(), False)
    output = ""

    def drain(seconds=0.2):
        nonlocal output
        deadline = time.time() + seconds
        while time.time() < deadline:
            ready, _, _ = select.select([proc.stdout], [], [], 0.05)
            if ready:
                data = os.read(proc.stdout.fileno(), 8192)
                if data:
                    output += data.decode("ascii", errors="replace")

    def wait_for(token, timeout=30, start=0):
        deadline = time.time() + timeout
        while token not in output[start:] and time.time() < deadline and proc.poll() is None:
            drain()
        return token in output[start:]

    def command(line, marker):
        start = len(output)
        proc.stdin.write((line + "\n").encode("ascii"))
        proc.stdin.flush()
        if not wait_for(marker, 15, start):
            raise RuntimeError(f"timeout waiting for {marker!r} after {line!r}")
        if marker != "PhotonOS / > " and not wait_for("PhotonOS / > ", 15, start):
            raise RuntimeError(f"shell prompt did not return after {line!r}")
        drain(0.3)

    try:
        if not wait_for("PhotonOS / > ", 40):
            raise RuntimeError("shell did not become ready")
        command('/bin/argprobe first "two words" "" last', "PhotonOS / > ")
        command("/bin/execvetest", "[EXECVE-TEST] ALL PASS")
        command("/bin/argprobe", "PhotonOS / > ")
        command("hello", "PhotonOS / > ")
        command("forktest", "PhotonOS / > ")
        command("vfstest", "PhotonOS / > ")

        checks = {
            "exec failure leaves caller alive": "failure keeps caller image alive" in output,
            "invalid argv rejected": "invalid argv pointer rejected" in output,
            "invalid envp rejected": "invalid envp pointer rejected" in output,
            "unterminated argv rejected": "unterminated oversized argv rejected" in output,
            "oversized string rejected": "oversized argument rejected" in output,
            "malformed ELF rejected": "truncated ELF header rejected" in output,
            "maximum argv count accepted": "maximum valid argv count accepted" in output,
            "maximum environment count accepted": "maximum environment count accepted" in output,
            "single argument accepted": "single argument accepted" in output,
            "maximum string length accepted": "maximum length string accepted" in output,
            "fork exec waits for child": "fork exec child waitpid" in output,
            "args including empty/spaces": "argv[2]=<two words>" in output and "argv[3]=<>" in output,
            "initial stack alignment": "entry-rsp-mod16=8" in output,
            "legacy ELF command": "PhotonOS: Ola do espaco" in output,
            "fork regression": "fork()" in output and "fork OK" in output,
            "VFS regression": "[VFS TEST] ALL TESTS PASSED" in output,
            "custom env/getenv": "[ARGPROBE] PASS argv/envp/getenv" in output,
            "descriptor survives exec": "[ARGPROBE] PASS inherited-fd" in output,
            "env value preserves equals": "PHOTON_PROBE=<value=with=equals>" in output,
            "environment includes empty value": "envp[3]=<EMPTY=>" in output,
        }
        for label, passed in checks.items():
            print(f"{'PASS' if passed else 'FAIL'} {label}")
        if not all(checks.values()):
            print(output)
            return 1
        return 0
    except Exception:
        print(output)
        raise
    finally:
        proc.terminate()
        proc.wait(timeout=5)


if __name__ == "__main__":
    raise SystemExit(main())
