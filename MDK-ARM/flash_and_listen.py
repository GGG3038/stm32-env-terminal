"""Flash task6.bin over ISP and listen on the same still-open port after GO.

Re-opening COM9 with pyserial asserts DTR/RTS, which can reset the board back
into the bootloader when BOOT0 is still high.  Keeping the stm32loader
connection open after the GO command avoids that.
"""
import sys
import time

from stm32loader.main import Stm32Loader

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM9"
BIN = sys.argv[2] if len(sys.argv) > 2 else "task6/task6.bin"
LISTEN_S = float(sys.argv[3]) if len(sys.argv) > 3 else 20.0

loader = Stm32Loader()
loader.parse_arguments([
    "-p", PORT, "-b", "115200", "-f", "F1",
    "-e", "-w", "-v", BIN,
])
loader.connect()
loader.read_device_id()
loader.read_device_uid()
loader.perform_commands()

print("Sending GO 0x08000000", flush=True)
loader.stm32.go(0x08000000)

conn = loader.stm32.connection
ser = conn.serial_connection

# Safe to reconfigure: _dtr_state/_rts_state are False after stm32loader's
# reset sequence, so SetCommState will keep DTR/RTS deasserted.
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
