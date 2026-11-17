"""Send commands to the emulator's serial console (localhost:12345) and print the output."""
import socket
import sys
import time

cmds = sys.argv[1:]
s = socket.create_connection(("127.0.0.1", 12345), timeout=5)
s.settimeout(0.5)


def drain(wait):
    out = b""
    end = time.time() + wait
    while time.time() < end:
        try:
            chunk = s.recv(65536)
            if not chunk:
                break
            out += chunk
        except socket.timeout:
            pass
    return out.decode("utf-8", "replace")


drain(1.0)
s.sendall(b"\r")
drain(1.0)
for c in cmds:
    if c.startswith("sleep:"):
        time.sleep(float(c[6:]))
        continue
    s.sendall(c.encode() + b"\r")
    print(f"$ {c}\n{drain(2.5)}")
