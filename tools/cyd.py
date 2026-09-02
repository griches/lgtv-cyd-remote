#!/usr/bin/env python3
"""Talk to the CYD remote over serial in ONE session.

Opening the port can reboot the board (CH340 auto-reset), so every step of a
task must happen within a single open. This tool opens once, waits out a boot
banner if one appears, then runs the commands in order.

usage: tools/cyd.py CMD [CMD ...] [--port PORT] [--wait SECS]

Commands are sent verbatim to the firmware (see handleSerial in src/main.cpp):
    p N        page      t X Y   tap      b NAME  remote button     h  heap
plus two handled here:
    shot FILE  dump the framebuffer to FILE (png)
    sleep N    wait N seconds, printing whatever the board logs

The panel returns each readback row shifted by one byte; corrected here.
"""
import sys, time, argparse, serial

def open_port(port, baud):
    s = serial.Serial(timeout=0.2)
    s.port, s.baudrate = port, baud
    s.dtr = False; s.rts = False
    s.open()
    # If the open rebooted the board, wait for setup() to finish.
    end = time.time() + 4
    buf = b""
    booted = False
    while time.time() < end:
        buf += s.read(4096)
        if b"LG remote booting" in buf: booted = True
        if booted and b"TVs, selected" in buf:
            time.sleep(0.3)
            break
        if not booted and time.time() > end - 3:  # quiet port: nothing rebooted
            break
    if booted:
        print("(board rebooted on open; waited for setup)", file=sys.stderr)
    s.reset_input_buffer()
    return s

def read_for(s, secs):
    end = time.time() + secs
    buf = b""
    while time.time() < end:
        buf += s.read(4096)
    return buf.decode("utf-8", "replace").strip()

def shot(s, path, shift=1):
    from PIL import Image
    s.reset_input_buffer()
    s.write(b"S\n")
    hdr = b""
    deadline = time.time() + 5
    while not hdr.startswith(b"SCREEN ") and time.time() < deadline:
        hdr = s.readline()
    if not hdr.startswith(b"SCREEN "):
        sys.exit("no SCREEN header from board")
    _, w, h = hdr.split()
    w, h = int(w), int(h)
    need = w * h * 3
    raw = b""
    deadline = time.time() + 30
    while len(raw) < need and time.time() < deadline:
        raw += s.read(need - len(raw))
    if len(raw) != need:
        sys.exit(f"short read: {len(raw)} of {need}")
    img = Image.new("RGB", (w, h))
    px = img.load()
    stride = w * 3
    for y in range(h):
        row = raw[y * stride + shift:(y + 1) * stride] + b"\0" * shift
        for x in range(w):
            px[x, y] = (row[x * 3], row[x * 3 + 1], row[x * 3 + 2])
    img.save(path)
    read_for(s, 0.3)  # swallow SCREEN_END
    print(f"saved {path} ({w}x{h})")

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmds", nargs="+")
    ap.add_argument("--port", default="/dev/cu.usbserial-11310")
    ap.add_argument("--baud", type=int, default=460800)
    ap.add_argument("--wait", type=float, default=1.0, help="seconds to listen after each command")
    a = ap.parse_args()
    s = open_port(a.port, a.baud)
    for c in a.cmds:
        if c.startswith("shot "):
            shot(s, c[5:].strip())
            continue
        if c.startswith("sleep "):
            out = read_for(s, float(c[6:]))
            if out:
                print(out)
            continue
        s.write((c + "\n").encode())
        out = read_for(s, a.wait)
        if out:
            print(out)
    s.close()

if __name__ == "__main__":
    main()
