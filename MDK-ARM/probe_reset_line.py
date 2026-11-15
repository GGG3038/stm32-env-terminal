"""Check whether DTR/RTS actually control NRST/BOOT0.

connect() syncs with the bootloader.  reset_from_flash() drives BOOT0 low and
pulses reset.  If the adapter is wired for auto-reset, the application now
runs, and the bootloader no longer answers 0x7F.  If it still answers, the
lines are not wired (or use different polarity), and the BOOT0 jumper must be
moved by hand.
"""
import sys

from stm32loader.main import Stm32Loader

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM9"

loader = Stm32Loader()
loader.parse_arguments(["-p", PORT, "-b", "115200", "-f", "F1"])
loader.connect()
print("Synced with bootloader", flush=True)

print("reset_from_flash(): BOOT0 low + reset pulse", flush=True)
loader.stm32.reset_from_flash()

conn = loader.stm32.connection
ser = conn.serial_connection
ser.parity = "N"
ser.timeout = 0.5
ser.reset_input_buffer()

print("Probing bootloader with 0x7F ...", flush=True)
ser.write(b"\x7f")
reply = ser.read(8)
if reply:
    print("Still in bootloader, reply=%s" % reply.hex(), flush=True)
else:
    print("No bootloader reply -> MCU is running the application", flush=True)

ser.close()
loader.stm32.connection = None
