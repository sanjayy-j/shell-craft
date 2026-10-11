#!/usr/bin/env python3
"""
Automated interactive terminal (PTY) test for ShellCraft:
Tests process groups, tcsetpgrp(), Ctrl+Z (SIGTSTP), jobs, bg %1, fg %1, Ctrl+C (SIGINT),
and verifies that runtime trace and pipeline analyzer record signal/terminal transitions.
"""

import fcntl
import os
import pty
import select
import subprocess
import sys
import termios
import time


def read_until(master_fd: int, substring: str, timeout: float = 5.0) -> str:
    buf = b""
    deadline = time.time() + timeout
    target = substring.encode("utf-8")
    while time.time() < deadline:
        rlist, _, _ = select.select([master_fd], [], [], 0.1)
        if master_fd in rlist:
            try:
                chunk = os.read(master_fd, 4096)
                if not chunk:
                    break
                buf += chunk
                if target in buf:
                    return buf.decode("utf-8", errors="replace")
            except OSError:
                break
    raise AssertionError(
        f"Timed out waiting for {substring!r}. Buffer so far:\n{buf.decode('utf-8', errors='replace')}"
    )


def send_cmd(master_fd: int, cmd: str) -> None:
    os.write(master_fd, (cmd + "\n").encode("utf-8"))


def setup_controlling_tty():
    os.setsid()
    fcntl.ioctl(0, termios.TIOCSCTTY, 0)


def main() -> int:
    if len(sys.argv) < 2:
        print("Usage: interactive_pty_test.py <path-to-shellcraft>", file=sys.stderr)
        return 1

    shell_bin = sys.argv[1]
    master_fd, slave_fd = pty.openpty()

    proc = subprocess.Popen(
        [shell_bin],
        stdin=slave_fd,
        stdout=slave_fd,
        stderr=slave_fd,
        preexec_fn=setup_controlling_tty,
        close_fds=True,
    )
    os.close(slave_fd)

    try:
        # 1. Wait for initial interactive prompt
        read_until(master_fd, "shellcraft> ")

        # 2. Launch a foreground sleep command under trace
        send_cmd(master_fd, "trace sleep 30")
        time.sleep(0.3)

        # 3. Send real Ctrl+Z (ASCII 0x1a / VSUSP) over the PTY master
        os.write(master_fd, b"\x1a")
        out = read_until(master_fd, "shellcraft> ")
        assert "[1] Stopped" in out, f"Expected '[1] Stopped' after Ctrl+Z, got:\n{out}"

        # 4. Run 'jobs' and verify Stopped state
        send_cmd(master_fd, "jobs")
        out = read_until(master_fd, "shellcraft> ")
        assert "[1] Stopped" in out, f"Expected '[1] Stopped' in jobs output, got:\n{out}"

        # 5. Resume in background with 'bg %1'
        send_cmd(master_fd, "bg %1")
        out = read_until(master_fd, "shellcraft> ")
        assert "[1] sleep 30 &" in out, f"Expected '[1] sleep 30 &' from bg %1, got:\n{out}"

        send_cmd(master_fd, "jobs")
        out = read_until(master_fd, "shellcraft> ")
        assert "[1] Running sleep 30" in out, f"Expected '[1] Running sleep 30' in jobs output, got:\n{out}"

        # 6. Bring back to foreground with 'fg %1'
        send_cmd(master_fd, "fg %1")
        time.sleep(0.3)

        # 7. Interrupt foreground job with real Ctrl+C (ASCII 0x03 / VINTR) over the PTY master
        os.write(master_fd, b"\x03")
        read_until(master_fd, "shellcraft> ")

        # 8. Verify shell reclaimed terminal and is still alive and responsive after Ctrl+C
        send_cmd(master_fd, "echo interactive_ok")
        out = read_until(master_fd, "shellcraft> ")
        assert "interactive_ok" in out, f"Shell did not respond after Ctrl+C:\n{out}"

        # 9. Verify trace and diagnosis of SIGINT termination
        send_cmd(master_fd, "trace sleep 10")
        time.sleep(0.3)
        os.write(master_fd, b"\x03")
        read_until(master_fd, "shellcraft> ")

        send_cmd(master_fd, "diagnose last")
        out = read_until(master_fd, "shellcraft> ")
        assert "SIGINT" in out, f"Expected SIGINT in diagnose last output, got:\n{out}"

        send_cmd(master_fd, "trace last")
        out = read_until(master_fd, "shellcraft> ")
        assert "TERMINAL_TRANSFERRED" in out and "TERMINAL_RECLAIMED" in out and "PROCESS_SIGNALED" in out, (
            f"Expected terminal handoff and signal events in trace last, got:\n{out}"
        )

        # 10. Clean exit
        send_cmd(master_fd, "exit")
        proc.wait(timeout=3.0)
        assert proc.returncode == 0, f"Expected return code 0, got {proc.returncode}"
        return 0

    finally:
        os.close(master_fd)
        if proc.poll() is None:
            proc.kill()


if __name__ == "__main__":
    sys.exit(main())
