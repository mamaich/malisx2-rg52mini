# On the device: press.py <evdev key name> [ms] - holds one button of the pad for ms (150) and lets
# it go, the state re-written into the pad's evdev node every 3 ms (the driver
# re-reports most buttons every ~20 ms). Used to leave the pause menu a savestate was taken in.
import sys, time, evdev
from evdev import ecodes as E
pad = evdev.InputDevice("/dev/input/by-path/platform-play_joystick-event-joystick")
code = getattr(E, sys.argv[1])
end = time.monotonic() + (int(sys.argv[2]) if len(sys.argv) > 2 else 150) / 1000
while time.monotonic() < end:
    pad.write(E.EV_KEY, code, 1)
    pad.write(E.EV_SYN, E.SYN_REPORT, 0)
    time.sleep(0.003)
pad.write(E.EV_KEY, code, 0)
pad.write(E.EV_SYN, E.SYN_REPORT, 0)
print("pressed", sys.argv[1])
