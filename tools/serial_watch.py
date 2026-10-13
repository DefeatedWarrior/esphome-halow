#!/usr/bin/env python3
"""Log an ESP32's USB serial output, reconnecting whenever the port drops.

`esphome logs` exits as soon as the board resets and its USB-Serial-JTAG port
re-enumerates, so it misses everything a boot loop or power-on reset prints.
This keeps reopening the port for the given duration and marks every drop, so
each boot and its reset reason end up in one log.

    python tools/serial_watch.py 90 watch.log            # first /dev/cu.usbmodem*
    python tools/serial_watch.py 90 watch.log --port /dev/ttyACM0

Requires pyserial (installed with ESPHome). Strip ANSI colours when reading:
    sed 's/\\x1b\\[[0-9;]*m//g' watch.log
"""

import argparse
import glob
import sys
import time

import serial


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("seconds", type=float, help="how long to watch")
    parser.add_argument("logfile", help="file to write")
    parser.add_argument(
        "--port",
        help="serial port (default: first match of /dev/cu.usbmodem* or /dev/ttyACM*)",
    )
    parser.add_argument("--baud", type=int, default=115200)
    args = parser.parse_args()

    patterns = [args.port] if args.port else ["/dev/cu.usbmodem*", "/dev/ttyACM*"]
    end = time.time() + args.seconds
    drops = 0
    busy_reported = False

    with open(args.logfile, "w", encoding="utf-8") as out:

        def mark(message):
            out.write(f"\n=== {time.strftime('%H:%M:%S')} {message} ===\n")
            out.flush()

        while time.time() < end:
            ports = [p for pattern in patterns for p in sorted(glob.glob(pattern))]
            if not ports:
                time.sleep(0.05)
                continue
            try:
                ser = serial.Serial(ports[0], args.baud, timeout=0.2)
            except (OSError, serial.SerialException) as err:
                # Another program (e.g. a browser's web serial page) holds the port:
                # say so once, then keep retrying quietly
                if not busy_reported:
                    mark(f"cannot open {ports[0]}: {err}")
                    print(f"{ports[0]} is busy; close whatever has it open", file=sys.stderr)
                    busy_reported = True
                time.sleep(1)
                continue
            busy_reported = False
            try:
                with ser:
                    mark(f"opened {ports[0]}")
                    while time.time() < end:
                        data = ser.read(4096)
                        if data:
                            out.write(data.decode("utf-8", "replace"))
                            out.flush()
            except (OSError, serial.SerialException) as err:
                drops += 1
                mark(f"lost port: {err}")
                time.sleep(0.05)

    print(f"done; port dropped {drops} time(s)", file=sys.stderr)


if __name__ == "__main__":
    main()
