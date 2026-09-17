"""Bounded USB capture, optionally sending a bring-up command (no reset)."""
import argparse
import time
import serial

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--port", default="/dev/cu.usbmodem2101")
parser.add_argument("--seconds", type=float, default=10)
parser.add_argument("--command", action="append", default=[])
args = parser.parse_args()
if args.seconds <= 0:
    parser.error("--seconds must be positive")

connection = serial.Serial(baudrate=115200, timeout=0.2)
connection.dtr = False
connection.rts = False
connection.port = args.port
connection.open()
with connection as port:
    for command in args.command:
        port.write((command + "\n").encode("ascii"))
    deadline = time.monotonic() + args.seconds
    while time.monotonic() < deadline:
        data = port.read(port.in_waiting or 1)
        if data:
            print(data.decode("utf-8", errors="replace"), end="", flush=True)
