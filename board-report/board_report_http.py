#!/usr/bin/env python3
"""board_report_http.py - READ-ONLY identification of a Saturn 4 Ultra 16K board + firmware, from a PC on the same network.

No telnet, no password, nothing is installed or changed on the printer: it only downloads a few files through the printer's own
web server (port 3030, the one the Elegoo app and slicer use; it serves the file system read-only without a login, on the stock
firmware too). Needs only Python 3 (standard library).

    python3 board_report_http.py <printer ip> [--no-image] > report.txt

--no-image skips the 30 MB check of the stored firmware copy (saves about a minute). The report holds no personal data: MAC
addresses are masked, there is no serial number, Wi-Fi data, file name or printer ID. Read it before you post it.
"""
import hashlib, re, socket, sys, urllib.request, urllib.error
from concurrent.futures import ThreadPoolExecutor

IP = None
PORT = 3030
MAC = re.compile(r"([0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}")
STOCK = {"chitu": "116dfee541b33b913a2fd898b0c7c0f4", "ChituUpgrade.bin": "2fa5c763a49e565b963ec0cf87704762",
         "ChituFPGA.bin": "36ac00b63562d342d7826a352286582d", "gauge": "b6979fd45a811fac2e7a1e74742c9ef9"}


def url(p):
    return "http://%s:%d%s" % (IP, PORT, p)


def get(p, limit=None, timeout=20):
    """bytes of a file, or None (not found / not reachable)."""
    try:
        with urllib.request.urlopen(url(p), timeout=timeout) as r:
            return r.read(limit) if limit else r.read()
    except (urllib.error.URLError, OSError):
        return None


def sysattr(p, quiet=1.2):
    """text of a /sys or /proc/device-tree attribute. The app's web server announces a fixed length for these and then waits for the
    rest, so a normal download would hang: read until it goes quiet. None = not found."""
    try:
        s = socket.create_connection((IP, PORT), timeout=quiet + 3)
        s.sendall(("GET %s HTTP/1.0\r\nHost: %s\r\n\r\n" % (p, IP)).encode())
        s.settimeout(quiet)
        buf = b""
        try:
            while True:
                c = s.recv(4096)
                if not c:
                    break
                buf += c
        except socket.timeout:
            pass
        s.close()
    except OSError:
        return None
    head, _, body = buf.partition(b"\r\n\r\n")
    if not head.startswith(b"HTTP/1.") or b" 200 " not in head.split(b"\r\n")[0]:
        return None
    return body.decode("utf-8", "replace").replace("\x00", " ").strip()


def sysattrs(paths):
    with ThreadPoolExecutor(max_workers=16) as ex:
        return dict(zip(paths, ex.map(sysattr, paths)))


def text(p):
    d = get(p, 65536)
    return None if d is None else d.decode("utf-8", "replace").replace("\x00", " ").strip()


def md5(p):
    h = hashlib.md5()
    try:
        with urllib.request.urlopen(url(p), timeout=30) as r:
            while True:
                b = r.read(1 << 16)
                if not b:
                    break
                h.update(b)
    except (urllib.error.URLError, OSError):
        return None
    return h.hexdigest()


def listing(p):
    d = get(p if p.endswith("/") else p + "/", 1 << 20)
    if d is None:
        return []
    return [n.rstrip("/") for n in re.findall(r'<a href="([^"?]+)"', d.decode("utf-8", "replace")) if n not in ("../", "./", "/") and n[0] not in "#."]


def sec(t):
    print("\n== " + t)


def tag(name, got, stock):
    return "%-28s %s %s" % (name, got or "(not readable)", "" if not got else ("= stock" if got == stock else "(not the stock value %s...)" % stock[:8]))


def main():
    global IP
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if not args:
        sys.exit(__doc__)
    IP = args[0]
    skip_image = "--no-image" in sys.argv
    if get("/customer/resources/", 2000) is None and get("/media/", 2000) is None:
        sys.exit("cannot reach http://%s:%d/ - is the printer on and on the same network (IP from its Settings screen)?" % (IP, PORT))

    print("BOARD REPORT (over http, read only)  - Saturn 4 Ultra 16K")
    # all the sysfs attributes at once (parallel: each one waits for the quiet time)
    mtds = ["/sys/class/mtd/mtd%d/%s" % (i, k) for i in range(16) for k in ("name", "size")]
    ubis = ["/sys/class/ubi/ubi0_%d/%s" % (i, k) for i in range(8) for k in ("name", "data_bytes")]
    emmc = ["/sys/block/mmcblk0/device/" + f for f in ("name", "manfid", "oemid", "date", "fwrev", "hwrev", "prv", "type")] + ["/sys/block/mmcblk0/size"]
    dts = ["/proc/device-tree/model", "/proc/device-tree/compatible"]
    usbdirs = listing("/sys/bus/usb/devices")
    usb = ["/sys/bus/usb/devices/%s/%s" % (n, k) for n in usbdirs for k in ("idVendor", "idProduct", "product")]
    A = sysattrs(mtds + ubis + emmc + dts + usb)

    sec("device tree / kernel")
    print("model:", A["/proc/device-tree/model"])
    print("compatible:", A["/proc/device-tree/compatible"])
    print("modules dir:", " ".join(listing("/lib/modules")) or "(n/a)")
    print("os-release:", (text("/etc/os-release") or "(n/a)").replace("\n", " | "))

    sec("flash layout (mtd = NAND, ubi volumes)")
    for i in range(16):
        if A["/sys/class/mtd/mtd%d/name" % i] is not None:
            print("mtd%d %-10s %s" % (i, A["/sys/class/mtd/mtd%d/size" % i], A["/sys/class/mtd/mtd%d/name" % i]))
    for i in range(8):
        if A["/sys/class/ubi/ubi0_%d/name" % i] is not None:
            print("ubi0_%d %-12s %s" % (i, A["/sys/class/ubi/ubi0_%d/name" % i], A["/sys/class/ubi/ubi0_%d/data_bytes" % i]))

    sec("eMMC (no serial number)")
    for f in ("name", "manfid", "oemid", "date", "fwrev", "hwrev", "prv", "type"):
        print("%s=%s" % (f, A["/sys/block/mmcblk0/device/" + f]))
    print("size_sectors=%s" % A["/sys/block/mmcblk0/size"])

    sec("software checksums (stock V1.5.9 values: chitu %s..., ChituUpgrade.bin %s..., ChituFPGA.bin %s..., gauge board image %s...)"
        % (STOCK["chitu"][:8], STOCK["ChituUpgrade.bin"][:8], STOCK["ChituFPGA.bin"][:8], STOCK["gauge"][:8]))
    print(tag("chitu (the app)", md5("/customer/resources/chitu"), STOCK["chitu"]))
    for n in listing("/customer/resources"):
        if re.match(r"upgrade_cbd_app_lcd_strain_gauge_.*\.bin$", n):
            print(tag(n, md5("/customer/resources/" + n), STOCK["gauge"]))
    print(tag("ChituFPGA.bin", md5("/media/mmcblk0p1/ChituFPGA.bin"), STOCK["ChituFPGA.bin"]))
    if skip_image:
        print("ChituUpgrade.bin (stored copy) skipped (--no-image)")
    else:
        print("(checking the stored firmware copy, about 30 MB ...)", file=sys.stderr)
        print(tag("ChituUpgrade.bin (stored)", md5("/media/mmcblk0p1/ChituUpgrade.bin"), STOCK["ChituUpgrade.bin"]))
    app = get("/customer/resources/chitu")
    if app:
        vs = sorted(set(re.findall(rb"V\d+\.\d+\.\d+", app)))[:5]
        print("chitu size", len(app), "| versions:", " ".join(v.decode() for v in vs))
    print("(a printer that runs the saturn4-fw mod shows the patched chitu 629a8c47... here: not the stock value, expected)")
    b = text("/customer/mod/BUILD")
    if b:
        print("-- saturn4-fw mod present:", b.replace("\n", " | "), "|", (text("/customer/mod/VERSION") or "")[:60])

    sec("USB devices (vendor:product name)")
    for n in usbdirs:
        v = A["/sys/bus/usb/devices/%s/idVendor" % n]
        if v:
            print("%s:%s %s" % (v, A["/sys/bus/usb/devices/%s/idProduct" % n], A["/sys/bus/usb/devices/%s/product" % n] or ""))

    sec("app log: version / FPGA / gauge lines (first 25; request lines and the printer ID are left out)")
    keep = re.compile(r"version|fpga|hardware|hw_|ceb-iap|strain|tmc|firmware", re.I)
    drop = re.compile(r"sdcp|MainboardID|RequestID|wlan|ssid|psk|file_name|file_manager")
    for f in ("/media/mmcblk0p2/log1", "/media/mmcblk0p2/log2"):
        d = get(f, 8 << 20, 60)
        if d:
            n = 0
            for line in d.decode("utf-8", "replace").splitlines():
                if keep.search(line) and not drop.search(line):
                    print(MAC.sub("XX:XX:XX:XX:XX:XX", line))
                    n += 1
                    if n >= 25:
                        break
            break
    else:
        print("(log not readable)")
    print("\nEND OF REPORT")


if __name__ == "__main__":
    main()
