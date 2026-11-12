#!/bin/bash
# On the device: estest.sh <system> <rom> <seconds> [emulator [core]]
# Start a game the way EmulationStation would: the system's command from
# es_systems.cfg with its default (first) emulator and core unless given, the
# governor from es_settings.cfg, as ark with ES's LimitNICE. After <seconds>
# print whether it still runs and any crash lines, then leave it running for a
# screen grab; "estest.sh stop" ends it and starts ES again.
U=estest
if [ "$1" = stop ]; then
  sudo systemctl stop $U 2>/dev/null; sudo systemctl reset-failed $U 2>/dev/null
  # the launch script restarts killer_daemon.service (Restart=always), so pkill alone brings it back
  sudo systemctl stop killer_daemon.service 2>/dev/null; sudo pkill -f killer_daemon.py 2>/dev/null
  sudo systemctl start emulationstation
  exit 0
fi
sys=$1; rom=$2; secs=$3; emu=${4:-}; core=${5:-}
cmd=$(python3 - "$sys" "$rom" "$emu" "$core" <<'EOF'
import os, sys, shlex, re, xml.etree.ElementTree as ET
name, rom, emu, core = sys.argv[1:5]
gov = ""
try:
    m = re.search(r'name="GlobalPerformanceGovernor" value="([^"]*)"',
                  open("/home/ark/.emulationstation/es_settings.cfg").read())
    gov = m.group(1) if m else ""
except OSError:
    pass
# ESTEST_GOV=<governor>: the one to start with instead of ES's setting
gov = os.environ.get("ESTEST_GOV", gov)
for s in ET.parse("/etc/emulationstation/es_systems.cfg").getroot().iter("system"):
    if s.findtext("name") != name:
        continue
    c = s.findtext("command")
    emus = s.find("emulators")
    if not emu and emus is not None and len(emus):
        emu = emus[0].get("name", "")
        cs = emus[0].find("cores")
        if not core and cs is not None and len(cs):
            core = cs[0].text.strip()
    print(c.replace("%ROM%", shlex.quote(rom)).replace("%EMULATOR%", emu)
           .replace("%CORE%", core).replace("%GOVERNOR%", gov))
    print("# emulator=%s core=%s governor=%s" % (emu, core, gov), file=sys.stderr)
    break
else:
    sys.exit("no system " + name)
EOF
) || exit 1
sudo systemctl stop emulationstation; sleep 2
sudo systemctl stop $U 2>/dev/null; sudo systemctl reset-failed $U 2>/dev/null
since=$(date '+%Y-%m-%d %H:%M:%S')
sudo systemd-run --quiet --unit=$U --uid=ark --gid=ark -p LimitNICE=-20 \
  -p WorkingDirectory=/home/ark --setenv=HOME=/home/ark --setenv=USER=ark \
  --setenv=SDL_VIDEO_EGL_DRIVER=libEGL.so bash -c "$cmd"
sleep "$secs"
state=$(systemctl is-active $U)
pids=$(sudo fuser /dev/dri/card0 2>/dev/null)
holder=$(for p in $pids; do ps -o ni=,comm= -p $p; done | tr -s ' ' | tr '\n' ';')
crash=$(sudo journalctl -u $U --since "$since" --no-pager -o cat | grep -i -E "SIGSEGV|SIGILL|Illegal instruction|Segmentation|Aborted|core dumped|Failed to load content|Failed to open libretro|DEBUGBREAK|undefined symbol|cannot open shared" | head -3 | tr '\n' ' ')
echo "$sys | $(basename "$rom") | unit=$state | card0: ${holder:-none} | ${crash:-no crash lines}"
