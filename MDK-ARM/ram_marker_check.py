"""Run the application, then read back its RAM boot-stage markers over ISP.

SRAM survives a system reset, so after letting the app run (or crash) we force
the bootloader and dump:
  0x20000000  g_boot_stage   0x11111111..0x88888888 = last init stage reached
  0x20000004  g_idle_count   >0 means the scheduler + idle task are running
  0x2000021C  g_task_stage[5] 0xAA00000N = task N entry was reached
"""
import struct
import sys
import time

import serial

from stm32loader.main import Stm32Loader

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM9"
RUN_S = float(sys.argv[2]) if len(sys.argv) > 2 else 8.0

# ---- 1. release reset with BOOT0 low: run the application ----
ser = serial.Serial(PORT, 115200, bytesize=8, parity="N", stopbits=1,
                    rtscts=0, timeout=0.2)
ser.rts = True          # RTS asserted = BOOT0 low  (stm32loader: setRTS(0) = BOOT0 high)
ser.dtr = False         # DTR deasserted = reset released
print("---- app running, UART listen %.0fs ----" % RUN_S, flush=True)

t0 = time.time()
rx = bytearray()
while time.time() - t0 < RUN_S:
    chunk = ser.read(256)
    if chunk:
        rx += chunk
sys.stdout.write(rx.decode("ascii", "replace"))
print("\n---- UART %d bytes ----" % len(rx), flush=True)

# ---- 2. force bootloader without closing the port ----
ser.dtr = True          # hold reset
ser.rts = False         # RTS deasserted = BOOT0 high
ser.dtr = False         # release reset -> bootloader
time.sleep(0.8)
ser.close()

# ---- 3. read back RAM markers ----
loader = Stm32Loader()
loader.parse_arguments(["-p", PORT, "-b", "115200", "-f", "F1"])
loader.connect()

raw = loader.stm32.read_memory_data(0x20000000, 8)
stage, idle = struct.unpack("<II", raw)
raw2 = loader.stm32.read_memory_data(0x2000021C, 20)
tasks = struct.unpack("<IIIII", raw2)

print("g_boot_stage = 0x%08X" % stage, flush=True)
print("g_idle_count = %u (0x%08X)" % (idle, idle), flush=True)
for i, v in enumerate(tasks):
    print("g_task_stage[%d] = 0x%08X" % (i, v), flush=True)

loader.stm32.reset_from_flash()
loader.stm32.connection.serial_connection.close()
loader.stm32.connection = None
