"""End-to-end test of the PC target (Linux/macOS): this script plays the
calculator over a pseudo-terminal, the app under test is the board, and a
local HTTP server is the internet.

    python test/pc/e2e.py .pio/build/pc/program
    python test/pc/e2e.py .pio/build/pc/program --bridge

With --bridge the same test runs through bridge mode: calculator pty ->
`program PORT --bridge BOARD` -> a second copy of the app playing the board.
"""
import binascii
import http.server
import os
import pty
import select
import struct
import subprocess
import sys
import tempfile
import threading
import time
import tty

BODY = b"".join(b"line %04d of the test body\n" % i for i in range(200))  # ~5 KB, several reads


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        path = self.path.split("?")[0]
        if path == "/data":
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Content-Length", str(len(BODY)))
            self.end_headers()
            self.wfile.write(BODY)
        elif path == "/moved":
            self.send_response(302)
            self.send_header("Location", "/data")
            self.send_header("Content-Length", "0")
            self.end_headers()
        else:
            self.send_error(404)

    def do_POST(self):
        # echo the upload back, and say where it "went"
        data = self.rfile.read(int(self.headers["Content-Length"]))
        self.send_response(201)
        self.send_header("Location", "/items/%d" % len(data))
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_HEAD(self):
        self.send_response(200)
        self.send_header("Content-Length", str(len(BODY)))
        self.end_headers()

    def log_message(self, fmt, *args):
        print("server: " + fmt % args, flush=True)


def frame(typ, seq, payload=b""):
    body = struct.pack("<BBBH", 0, typ, seq, len(payload)) + payload
    return b"\xA5" + body + struct.pack("<H", binascii.crc_hqx(body, 0xFFFF))


class Link:
    def __init__(self, fd):
        self.fd, self.seq, self.buf = fd, 0, b""

    def call(self, typ, payload=b"", timeout=2.0):
        """Send one frame, return (flags, payload) of the matching reply."""
        self.seq = (self.seq + 1) & 0xFF
        os.write(self.fd, frame(typ, self.seq, payload))
        end = time.time() + timeout
        while time.time() < end:
            if select.select([self.fd], [], [], 0.05)[0]:
                self.buf += os.read(self.fd, 4096)
            while True:
                i = self.buf.find(b"\xA5")
                if i < 0 or len(self.buf) - i < 8:
                    break
                n = struct.unpack_from("<H", self.buf, i + 4)[0]
                if len(self.buf) - i < 8 + n:
                    break
                f, self.buf = self.buf[i:i + 8 + n], self.buf[i + 8 + n:]
                crc = struct.unpack_from("<H", f, 6 + n)[0]
                if crc == binascii.crc_hqx(f[1:6 + n], 0xFFFF) and f[2] == typ and f[3] == self.seq:
                    return f[1], f[6:6 + n]
        raise AssertionError("no reply to type 0x%02X" % typ)


def get(link, url, method=1, upload=b""):
    """REQ_BEGIN [+ BODY_WRITE loop] + poll + BODY_READ loop.
    Returns (err, http_status, body)."""
    u = url.encode()
    flags, _ = link.call(0x10, struct.pack("<BBBIHH", method, 0, 5, len(upload), len(u), 0) + u)
    assert flags == 1, "REQ_BEGIN failed: %r" % flags
    end = time.time() + 10
    off = 0
    while off < len(upload) and time.time() < end:  # also the connect poll
        flags, r = link.call(0x20, struct.pack("<IB", off, 50) + upload[off:off + 200])
        assert flags == 1, "BODY_WRITE error %r" % r
        off = struct.unpack_from("<I", r)[0]
    while time.time() < end:
        _, st = link.call(0x11)
        state, err, status = st[0], st[1], struct.unpack_from("<H", st, 2)[0]
        if state == 7:
            return err, status, None
        if state in (5, 6):
            break
        time.sleep(0.05)
    body, off = b"", 0
    while time.time() < end:
        flags, r = link.call(0x21, struct.pack("<IHB", off, 200, 50))
        assert flags == 1, "BODY_READ error %r" % r
        body += r[5:]
        off += len(r) - 5
        if r[4] & 1:
            return err, status, body
    raise AssertionError("body never finished")


def relay(a, b):
    """Copy bytes both ways between two pty masters, forever."""
    while True:
        for fd in select.select([a, b], [], [])[0]:
            os.write(b if fd == a else a, os.read(fd, 4096))


def raw_pty():
    master, slave = pty.openpty()
    tty.setraw(master)
    return master, slave


def main():
    app = sys.argv[1]
    bridged = sys.argv[2:] == ["--bridge"]
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    base = "http://127.0.0.1:%d" % srv.server_address[1]

    master, slave = raw_pty()
    trace = tempfile.TemporaryFile(mode="w+")  # the app's packet trace (stdout)
    procs = []
    if bridged:
        # two ptys joined by a relay stand in for the board's USB-UART cable
        (bm, bs), (am, as_) = raw_pty(), raw_pty()
        threading.Thread(target=relay, args=(bm, am), daemon=True).start()
        procs.append(subprocess.Popen([app, os.ttyname(bs)], stdout=subprocess.DEVNULL))
        procs.append(subprocess.Popen([app, os.ttyname(slave), "--bridge", os.ttyname(as_)], stdout=trace))
    else:
        procs.append(subprocess.Popen([app, os.ttyname(slave)], stdout=trace))
    try:
        link = Link(master)
        time.sleep(0.5)  # app opens the port

        flags, r = link.call(0x02)  # before HELLO
        assert flags == 5 and r[0] == 0x02, "want ERR_NO_HELLO, got %r %r" % (flags, r)
        flags, r = link.call(0x01, bytes([0, 6, 0, 0, 0, 4]))
        assert flags == 1 and r[:2] == b"\x00\x06", "HELLO: %r %r" % (flags, r)
        assert len(r) == 11 and r[10] == 1, "HELLO should report 1 Wi-Fi slot: %r" % r
        flags, r = link.call(0x02)
        assert flags == 1 and len(r) == 13, "STATUS: %r" % r
        assert r[0] == 2 and r[1] == 0, "STATUS should say connected, slot 0: %r" % r
        assert r[12] & 1, "STATUS should report the Wi-Fi lock: %r" % r
        assert r[12] & 2, "STATUS should report a valid clock: %r" % r

        err, status, body = get(link, base + "/data")
        assert (err, status, body) == (0, 200, BODY), "err=0x%02X status=%r len=%d" % (err, status, len(body or b""))
        print("ok  GET /data: %d bytes" % len(body))

        err, status, body = get(link, base + "/data?token=s3cret")
        assert (err, status, body) == (0, 200, BODY), "query: err=0x%02X status=%r" % (err, status)
        print("ok  GET with a query string")

        err, status, body = get(link, base + "/moved")
        assert (err, status, body) == (0, 200, BODY), "redirect: %r" % ((err, status),)
        print("ok  GET /moved follows the 302")

        err, status, body = get(link, base + "/nope")
        assert (err, status) == (0, 404), "404: err=0x%02X status=%r" % (err, status)
        print("ok  GET /nope: 404")

        upload = bytes(range(256)) * 3  # binary, and several BODY_WRITEs
        err, status, body = get(link, base + "/items", method=2, upload=upload)
        assert (err, status, body) == (0, 201, upload), "POST: err=0x%02X status=%r" % (err, status)
        flags, r = link.call(0x13, struct.pack("<BHB", 0, 0, 8) + b"Location")
        assert flags == 1 and r[0] == 1 and r[3:] == b"/items/768", "HDR_GET: %r %r" % (flags, r)
        print("ok  POST %d bytes, Location via HDR_GET" % len(upload))

        err, status, body = get(link, base + "/data", method=6)
        assert (err, status, body) == (0, 200, b""), "HEAD: err=0x%02X status=%r" % (err, status)
        print("ok  HEAD: no body")

        flags, r = link.call(0x03)
        assert flags == 1 and r[1:1 + r[0]] == b"0.6.0" and r[2 + r[0]:] == b"PC", "INFO: %r" % r
        print("ok  INFO")

        err, _, _ = get(link, "http://no-such-host.invalid/")
        assert err == 0x21, "want ERR_DNS, got 0x%02X" % err
        print("ok  unknown host: ERR_DNS")

        err, _, _ = get(link, "http://127.0.0.1:1/")
        assert err == 0x22, "want ERR_CONNECT, got 0x%02X" % err
        print("ok  refused port: ERR_CONNECT")

        # the PC can't join other networks: one slot, "LAN", locked
        flags, r = link.call(0x40, b"\x00")
        assert flags == 1 and r == b"\x03LAN\x00", r
        flags, r = link.call(0x40, b"\x01")
        assert flags == 5 and r[0] == 0x08, "slot 1 should be ERR_BAD_ARG: %r %r" % (flags, r)
        flags, r = link.call(0x41, b"\x00\x04Home\x0ehunter22secret\x00")
        assert flags == 5 and r[0] == 0x0A, "WIFI_SET should be ERR_LOCKED: %r %r" % (flags, r)
        flags, r = link.call(0x42, b"\x00")
        assert flags == 5 and r[0] == 0x0A, "WIFI_FORGET should be ERR_LOCKED: %r %r" % (flags, r)
        flags, r = link.call(0x40, b"\x00")
        assert flags == 1 and r == b"\x03LAN\x00", r
        print("ok  one Wi-Fi slot, LAN, locked")
    finally:
        for proc in procs:
            proc.terminate()
            proc.wait(5)
        srv.shutdown()

    trace.seek(0)
    log = trace.read()
    print(log[:2000])
    for want in ("calc > #1 STATUS?", "calc < #1 STATUS -> error NO_HELLO", "HELLO ok v0.6",
                 "REQ_BEGIN GET http://127.0.0.1:", "/data?...", "REQ_STATUS BODY http=200",
                 "BODY_READ @0 max=200 wait=50ms", "bytes EOF", "WIFI_SET -> error LOCKED",
                 'WIFI_GET ssid="LAN"', "wifi_slots=1", "REQ_BEGIN POST http://127.0.0.1:",
                 "BODY_WRITE @0 200 bytes", "BODY_WRITE next=",
                 "HDR_GET Location", "found, 10 of 10 bytes (not shown)", 'INFO fw="0.6.0" board="PC"'):
        assert want in log, "trace is missing %r" % want
    for secret in ("s3cret", "hunter22secret"):
        assert secret not in log, "trace leaked %r" % secret
    print("ok  packet trace readable, no secrets")
    print("e2e ok")


if __name__ == "__main__":
    main()
