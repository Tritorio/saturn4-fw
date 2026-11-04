#!/usr/bin/env python3
"""Minimal telnet client: log in, run one shell command line, print its output.
Usage: TPW=<root password> tools/tsh.py HOST 'command; command'
  env TTIMEOUT=<s> (default 60), TPUT=local=remote[,...] uploads files first. The first output lines echo the command itself.
  Pull files: '... echo ==B""EGIN==; tar cf - PATHS | gzip | base64; echo ==E""ND=='
  (busybox tar has no -z, and -C applies to the whole archive).
Stdlib only (Python 3.13+ has no telnetlib). The password is never stored."""
import base64, gzip, os, socket, sys, time

IAC, DONT, DO, WONT, WILL, SB, SE = 255, 254, 253, 252, 251, 250, 240


def strip_iac(sock, data):
    out, i = bytearray(), 0
    while i < len(data):
        b = data[i]
        if b == IAC and i + 1 < len(data):
            cmd = data[i + 1]
            if cmd in (DO, DONT, WILL, WONT) and i + 2 < len(data):
                opt = data[i + 2]
                # refuse everything -> plain NVT
                sock.sendall(bytes([IAC, WONT if cmd in (DO, DONT) else DONT, opt]))
                i += 3; continue
            if cmd == SB:
                j = data.find(bytes([IAC, SE]), i)
                i = len(data) if j < 0 else j + 2; continue
            i += 2; continue
        out.append(b); i += 1
    return bytes(out)


def read_until(sock, marks, timeout):
    buf, end = b"", time.time() + timeout
    while time.time() < end:
        sock.settimeout(max(0.1, end - time.time()))
        try:
            d = sock.recv(65536)
        except socket.timeout:
            break
        if not d:
            break
        buf += strip_iac(sock, d)
        if any(m in buf for m in marks):
            break
    return buf


def main():
    host, cmd = sys.argv[1], sys.argv[2]
    timeout = float(os.environ.get("TTIMEOUT", "60"))
    s = socket.create_connection((host, 23), timeout=10)
    read_until(s, [b"login:"], 10); s.sendall(b"root\r\n")
    read_until(s, [b"Password:"], 10); s.sendall(os.environ["TPW"].encode() + b"\r\n")
    r = read_until(s, [b"# ", b"$ ", b": ", b"incorrect"], 10)   # root prompt is "~: "
    if b"incorrect" in r:
        sys.exit("login failed")
    # optional uploads first: TPUT="local=remote[,local=remote...]". gzip + base64 in
    # 512-char lines (the shell's line buffer is limited), paced by an echo every 32 lines.
    for pair in filter(None, os.environ.get("TPUT", "").split(",")):
        src, dst = pair.split("=", 1)
        b64 = base64.b64encode(gzip.compress(open(src, "rb").read())).decode()
        s.sendall(("stty -echo 2>/dev/null; rm -f %s.b64\r\n" % dst).encode())
        lines = [b64[i:i + 512] for i in range(0, len(b64), 512)]
        for i, chunk in enumerate(lines):
            s.sendall(("echo %s >> %s.b64\r\n" % (chunk, dst)).encode())
            if i % 32 == 31 or i == len(lines) - 1:
                s.sendall(("echo __S%d''__\r\n" % i).encode())
                read_until(s, [("__S%d__" % i).encode()], 30)
        s.sendall(("base64 -d %s.b64 | gunzip > %s && rm %s.b64; echo __U''P__\r\n" % (dst, dst, dst)).encode())
        read_until(s, [b"__UP__"], 30)
    mark = "__END_%d__" % os.getpid()
    # marker built from two parts so the echoed command line does not match it
    s.sendall(("stty -echo 2>/dev/null; %s; echo __END_''%d__\r\n" % (cmd, os.getpid())).encode())
    out = read_until(s, [mark.encode() + b"\r\n", mark.encode() + b"\n"], timeout)
    s.sendall(b"exit\r\n"); s.close()
    text = out.decode("utf-8", "replace").replace("\r\n", "\n")
    text = text.split(mark)[0]
    sys.stdout.write(text)


if __name__ == "__main__":
    main()
