"""Attach to the Daisy's USB serial port and print what it says.

    vs-env/Scripts/python.exe firmware/monitor.py     # find it and connect
    vs-env/Scripts/python.exe firmware/monitor.py --list
    vs-env/Scripts/python.exe firmware/monitor.py --port COM7
    vs-env/Scripts/python.exe firmware/monitor.py --save dronebench.txt

Finds the port by USB vendor/product ID rather than by name, because the
Daisy shows up as a generic "USB Serial Device" on Windows and the COM
number changes between plug-ins.

The baud rate does not matter. This is USB CDC, not a real UART -- the
number is negotiated away and ignored. If a tool insists on one, any value
works.

The firmware calls StartLog(true), which blocks until something opens this
port. So: start this, then tap RESET, and the board prints from the top.
If you see nothing at all, the board is waiting for you, not hung.

Resetting also makes the USB device disappear, which used to kill the
monitor at exactly the moment the board had something to say. It now
reattaches instead, looking the board up again by USB id because the COM
number can change across a re-enumeration. --once restores the old
exit-on-reset behaviour.

Ported from the ddsp branch unchanged apart from these paths -- it finds
the board by USB id and prints lines, neither of which depends on what the
firmware is.
"""

import argparse
import sys
import time

import serial
from serial.tools import list_ports

# STMicroelectronics VID, and the CDC PID libDaisy's USB descriptor uses.
STM_VID = 0x0483
DAISY_PIDS = {0x5740, 0xDF11}        # 5740 = CDC serial, DF11 = DFU bootloader


def candidates():
    return [p for p in list_ports.comports() if (p.vid, p.pid) != (None, None)]


def find_daisy():
    ports = candidates()
    for p in ports:
        if p.vid == STM_VID and p.pid == 0x5740:
            return p, None
    for p in ports:
        if p.vid == STM_VID and p.pid == 0xDF11:
            return None, (
                f"{p.device} is the Daisy in DFU mode, not running firmware.\n"
                f"  Tap RESET (without holding BOOT) to leave the bootloader.")
    if not ports:
        return None, "No USB serial devices at all. Is the Daisy plugged in?"
    listing = "\n".join(f"  {p.device}  {p.description}" for p in ports)
    return None, f"No Daisy found. Ports present:\n{listing}"


def pump(port, sink):
    """Read lines until the port goes away."""
    # A short timeout rather than blocking, so Ctrl+C stays responsive.
    with serial.Serial(port, 115200, timeout=0.2) as ser:
        buf = b""
        while True:
            chunk = ser.read(4096)
            if not chunk:
                continue
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                text = line.decode("utf-8", "replace").rstrip("\r")
                print(text, flush=True)
                if sink:
                    sink.write(text + "\n")
                    sink.flush()


def reattach(wait):
    """Wait for the board to come back after a reset.

    Looks it up by USB id rather than reopening the name it had: the COM
    number can change across a re-enumeration.
    """
    deadline = time.time() + wait
    while time.time() < deadline:
        time.sleep(0.3)
        found, _ = find_daisy()
        if found:
            # The CDC endpoint is not openable the instant it enumerates;
            # opening too early fails the same way it just did.
            time.sleep(0.5)
            return found.device
    raise SystemExit(f"board did not come back within {wait:.0f}s")


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", help="skip auto-detect")
    ap.add_argument("--list", action="store_true", help="show ports and exit")
    ap.add_argument("--save", metavar="FILE", help="also write the output here")
    ap.add_argument("--wait", type=float, default=60.0,
                    help="seconds to wait for the board to appear")
    ap.add_argument("--once", action="store_true",
                    help="exit on reset instead of reattaching")
    args = ap.parse_args()

    if args.list:
        ports = candidates()
        if not ports:
            print("no USB serial devices")
        for p in ports:
            tag = ""
            if p.vid == STM_VID and p.pid == 0x5740:
                tag = "   <-- Daisy, running firmware"
            elif p.vid == STM_VID and p.pid == 0xDF11:
                tag = "   <-- Daisy, in DFU mode"
            print(f"{p.device}  {p.description}  "
                  f"[{p.vid:04x}:{p.pid:04x}]{tag}")
        return

    port = args.port
    if port is None:
        # Poll rather than fail: the usual order is to start this first and
        # then reset the board.
        deadline = time.time() + args.wait
        while True:
            found, why = find_daisy()
            if found:
                port = found.device
                break
            if time.time() > deadline:
                raise SystemExit(f"{why}\n\nTry: python firmware/monitor.py --list")
            print(f"\rwaiting for the Daisy... ({why.splitlines()[0]})",
                  end="", flush=True)
            time.sleep(0.5)
        print(f"\rfound {port}" + " " * 60)

    sink = open(args.save, "w", encoding="utf-8") if args.save else None
    print(f"attached to {port}. Ctrl+C to stop.\n")
    try:
        while True:
            try:
                pump(port, sink)
            except (serial.SerialException, OSError) as exc:
                # Resetting the board tears down the USB CDC device, so the
                # port disappears mid-read. That is also the normal way to
                # make the firmware talk -- it calls StartLog(true) and
                # waits for a monitor, so you attach first and then reset
                # -- and exiting here meant the monitor was dead at exactly
                # the moment the board had something to say.
                if args.once:
                    raise SystemExit(f"\n{port}: {exc}")
                print(f"\n[board reset -- waiting for it to come back]",
                      flush=True)
                port = reattach(args.wait)
                print(f"[reattached to {port}]\n", flush=True)
    except KeyboardInterrupt:
        print("\nstopped")
    finally:
        if sink:
            sink.close()


if __name__ == "__main__":
    main()
