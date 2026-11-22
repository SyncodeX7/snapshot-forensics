#!/usr/bin/env python3
"""Send HMP commands to a QEMU monitor unix socket.

Usage: qemu_monitor.py /path/to/mon.sock "pmemsave 0 0x20000000 /tmp/ram.raw" quit

Commands are sent one at a time and each one is awaited before the next is
issued, so a slow pmemsave always completes before quit.
"""

import socket
import sys
import time

PROMPT = b"(qemu)"


def recv_until_prompt(sock, timeout):
    sock.settimeout(timeout)
    data = bytearray()
    while True:
        try:
            chunk = sock.recv(65536)
        except socket.timeout:
            break
        if not chunk:
            break
        data.extend(chunk)
        if PROMPT in data:
            break
    return bytes(data)


def main():
    if len(sys.argv) < 3:
        print(__doc__, file=sys.stderr)
        return 2
    path = sys.argv[1]
    commands = sys.argv[2:]

    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.settimeout(5.0)
    for _ in range(40):
        try:
            sock.connect(path)
            break
        except (FileNotFoundError, ConnectionRefusedError):
            time.sleep(0.25)
    else:
        print("cannot connect to %s" % path, file=sys.stderr)
        return 1

    # Drain the banner/prompt.
    recv_until_prompt(sock, 2.0)

    for cmd in commands:
        sock.sendall(cmd.encode() + b"\n")
        # pmemsave of 512 MiB can take a few seconds under I/O load.
        out = recv_until_prompt(sock, 900.0)
        sys.stdout.write(out.decode("utf-8", "replace"))
        sys.stdout.flush()

    sock.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
