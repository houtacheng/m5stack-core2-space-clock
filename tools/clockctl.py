#!/usr/bin/env python3
"""clockctl - an adb-like command line for the Space Clock (no USB needed).

Usage:  clockctl.py <ip> [--token TOKEN] <command> [args]

The token is shown on the Firmware page of the clock's settings ("Remote debugging") and is printed at boot.
Put it in the CLOCK_TOKEN environment variable or in ~/.clockctl (one line) to leave out --token.

Commands
  info                 status as JSON (version, memory, Wi-Fi, battery, screen, services ...)
  log [-f] [-n BYTES]  print the recent serial log; -f follows it like `adb logcat`
  cmd "t:..."          run one debug command and print its output (t:lstat, t:time, t:bt, t:emo ...)
  shell                interactive prompt for debug commands
  shot FILE.png|.bmp   screenshot of the clock's display
  reboot               restart the clock
  ota FILE.bin         install firmware over Wi-Fi (the clock must be in normal mode)
"""
import argparse
import json
import os
import socket
import struct
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid
import zlib


def read_token(arg):
    if arg:
        return arg
    if os.environ.get("CLOCK_TOKEN"):
        return os.environ["CLOCK_TOKEN"].strip()
    path = os.path.expanduser("~/.clockctl")
    if os.path.exists(path):
        return open(path).read().strip()
    sys.exit("no token: use --token, CLOCK_TOKEN or ~/.clockctl")


class Clock:
    def __init__(self, host, token):
        self.base = "http://%s" % host
        self.token = token

    def url(self, path, **query):
        query["token"] = self.token
        return "%s%s?%s" % (self.base, path, urllib.parse.urlencode(query))

    def get(self, path, timeout=15, **query):
        with urllib.request.urlopen(self.url(path, **query), timeout=timeout) as r:
            return r.read(), r.headers

    def post(self, path, data=b"", timeout=30, **query):
        req = urllib.request.Request(self.url(path, **query), data=data, method="POST")
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.read()


def bmp_to_png(bmp, out):
    """Minimal BMP (24-bit, bottom-up) to PNG converter, standard library only."""
    off = struct.unpack_from("<I", bmp, 10)[0]
    w, h = struct.unpack_from("<ii", bmp, 18)
    row = ((w * 3 + 3) // 4) * 4
    raw = bytearray()
    for y in range(h - 1, -1, -1):  # PNG rows run top-down
        line = bmp[off + y * row: off + y * row + w * 3]
        raw.append(0)
        for x in range(w):
            b, g, r = line[x * 3: x * 3 + 3]
            raw += bytes((r, g, b))

    def chunk(kind, data):
        c = struct.pack(">I", len(data)) + kind + data
        return c + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 6)) + chunk(b"IEND", b"")
    open(out, "wb").write(png)


def cmd_log(clock, args):
    nxt = None
    if args.bytes:
        body, hdr = clock.get("/adb/log")
        nxt = int(hdr.get("X-Next", 0))
        sys.stdout.write(body.decode("utf-8", "replace")[-args.bytes:])
    else:
        nxt = None
    while True:
        query = {} if nxt is None else {"from": nxt}
        body, hdr = clock.get("/adb/log", **query)
        sys.stdout.write(body.decode("utf-8", "replace"))
        sys.stdout.flush()
        nxt = int(hdr.get("X-Next", 0))
        if not args.follow:
            return
        time.sleep(0.7)


def cmd_cmd(clock, text):
    out = clock.post("/adb/cmd", urllib.parse.urlencode({"cmd": text}).encode(), timeout=90)
    sys.stdout.write(out.decode("utf-8", "replace"))


def cmd_ota(clock, path):
    data = open(path, "rb").read()
    boundary = uuid.uuid4().hex
    body = (("--%s\r\nContent-Disposition: form-data; name=\"firmware\"; filename=\"%s\"\r\n"
             "Content-Type: application/octet-stream\r\n\r\n" % (boundary, os.path.basename(path))).encode()
            + data + ("\r\n--%s--\r\n" % boundary).encode())
    # Wi-Fi power saving makes uploads slow: opening /sd switches it off for a while.
    try:
        urllib.request.urlopen(clock.base + "/sd", timeout=10).read()
    except Exception:
        pass
    req = urllib.request.Request(clock.base + "/update", data=body, method="POST",
                                 headers={"Content-Type": "multipart/form-data; boundary=" + boundary})
    print("uploading %d bytes ..." % len(data))
    with urllib.request.urlopen(req, timeout=300) as r:
        print("HTTP", r.status, "- the clock restarts now")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("host")
    ap.add_argument("--token")
    sub = ap.add_subparsers(dest="command", required=True)
    sub.add_parser("info")
    lg = sub.add_parser("log")
    lg.add_argument("-f", "--follow", action="store_true")
    lg.add_argument("-n", "--bytes", type=int, default=0)
    c = sub.add_parser("cmd")
    c.add_argument("text")
    sub.add_parser("shell")
    s = sub.add_parser("shot")
    s.add_argument("file")
    sub.add_parser("reboot")
    o = sub.add_parser("ota")
    o.add_argument("file")
    args = ap.parse_args()
    clock = Clock(args.host, read_token(args.token))
    try:
        if args.command == "info":
            body, _ = clock.get("/adb/info")
            sys.stdout.write(body.decode())
        elif args.command == "log":
            cmd_log(clock, args)
        elif args.command == "cmd":
            cmd_cmd(clock, args.text)
        elif args.command == "shell":
            print("debug commands (t:...); Ctrl-D to leave")
            while True:
                try:
                    line = input("clock> ").strip()
                except EOFError:
                    print()
                    break
                if line:
                    cmd_cmd(clock, line)
        elif args.command == "shot":
            bmp, _ = clock.get("/adb/shot", timeout=60)
            if args.file.lower().endswith(".png"):
                bmp_to_png(bmp, args.file)
            else:
                open(args.file, "wb").write(bmp)
            print("saved", args.file)
        elif args.command == "reboot":
            sys.stdout.write(clock.post("/adb/reboot").decode())
        elif args.command == "ota":
            cmd_ota(clock, args.file)
    except urllib.error.HTTPError as e:
        sys.exit("HTTP %d: %s" % (e.code, e.read().decode("utf-8", "replace").strip()))
    except (urllib.error.URLError, TimeoutError, socket.timeout, OSError) as e:
        sys.exit("cannot reach the clock: %s" % e)
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
