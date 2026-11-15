"""Reset the MCU into the application (BOOT0 low) and listen on the same port.

Use after flashing: stm32loader's connect() can only sync when BOOT0 is high;
reset_from_flash() then drives BOOT0 low and pulses reset, so the application
starts.  The port stays open so DTR/RTS are not toggled by a re-open.
"""
import sys
import time

from stm32loader.main import Stm32Loader

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM9"
LISTEN_S = float(sys.argv[2]) if len(sys.argv) > 2 else 20.0

loader = Stm32Loader()
loader.parse_arguments(["-p", PORT, "-b", "115200", "-f", "F1"])
loader.connect()

print("Resetting into application (BOOT0 low)", flush=True)
loader.stm32.reset_from_flash()

conn = loader.stm32.connection
ser = conn.serial_connection
ser.parity = "N"
ser.timeout = 0.2

print("---- listening %s @115200 8N1 for %.0fs ----" % (PORT, LISTEN_S), flush=True)
t0 = time.time()
rx = bytearray()
while time.time() - t0 < LISTEN_S:
    chunk = ser.read(256)
    if chunk:
        rx += chunk
        print("[t=%.2f] %r" % (time.time() - t0, bytes(chunk)), flush=True)

print("\n---- %d bytes ----" % len(rx), flush=True)
ser.close()
loader.stm32.connection = None
