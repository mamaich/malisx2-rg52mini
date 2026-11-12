#!/bin/bash
# On the PC: train-armsx2.sh <instrumented .so on the console> <run>... - training runs of ARMSX2
# for malisx2's PGO profile, started the way ES starts PS2 games (bench/estest.sh), the driver chosen
# by an "armsx2-sdl = <path>" line in /etc/darkos/vulkan.conf (put back at the end). A run is
#   <rom>|<state or ->|<seconds>[|<press>]
# with the state a savestate file given to ARMSX2 by --state-from-file (copied to /tmp first, so
# its name has no spaces), and <press> seconds after the start a press of BTN_SOUTH (press.py):
# Jak and Daxter's memory card dialog after a state load (ARMSX2's McdEnableEjection), 13 s. Each
# run ends with "systemctl stop" of the unit, which lets ARMSX2 shut down; the profile runtime
# writes /home/ark/perf/msx-prof/*.profraw as the process exits, and the count and size of those
# files is printed after every run.
# BENCH_PRE: a command for the console before every game (say, one that sets the volume).
H=$(dirname "$(readlink -f "$0")")
D="bash $H/../dev.sh"; so=$1; shift
L=/usr/local/bin/standalone-pcsx2sa
$D "cat > /tmp/estest.sh" < $H/../bench/estest.sh
$D "cat > /tmp/press.py" < $H/../bench/press.py
$D 'grep -q "^armsx2-sdl = /home/ark/" /etc/darkos/vulkan.conf || sudo cp /etc/darkos/vulkan.conf /tmp/vulkan.conf.ab-orig; mkdir -p /home/ark/perf/msx-prof' < /dev/null
for run in "$@"; do
  IFS='|' read -r rom state secs press <<< "$run"
  echo "=== $(basename "$rom") | state ${state} | ${secs} s${press:+ | press at $press s}"
  hook=""; [ -n "$press" ] && hook="( sleep $press; sudo python3 /tmp/press.py BTN_SOUTH > /dev/null ) &"
  $D "${BENCH_PRE:+$BENCH_PRE; }set -e
    sudo sed -i '/^armsx2-sdl *=/d' /etc/darkos/vulkan.conf; echo 'armsx2-sdl = $so' | sudo tee -a /etc/darkos/vulkan.conf > /dev/null
    sudo umount $L 2>/dev/null || true
    if [ '$state' != - ]; then
      cp \"$state\" /tmp/pgo-state.p2s
      sed 's#^/opt/pcsx2/bin/armsx2-sdl \"\\\$ROM\"#/opt/pcsx2/bin/armsx2-sdl --state-from-file /tmp/pgo-state.p2s \"\\\$ROM\"#' $L > /tmp/standalone-pgo
      chmod 755 /tmp/standalone-pgo; grep -c state-from-file /tmp/standalone-pgo
      sudo mount --bind /tmp/standalone-pgo $L
    fi
    $hook
    bash /tmp/estest.sh ps2 \"$rom\" $secs 2>&1 | tail -1
    p=\$(pgrep -x armsx2-sdl | head -1); grep -o '/[^ ]*libvulkan_malisx2[^ ]*' /proc/\$p/maps 2>/dev/null | sort -u | head -1
    sudo systemctl stop estest 2>/dev/null || true
    for i in \$(seq 1 30); do pgrep -x armsx2-sdl > /dev/null || break; sleep 0.5; done
    pgrep -x armsx2-sdl > /dev/null && { echo 'armsx2 still running: killed'; sudo pkill -9 -x armsx2-sdl; } || true
    sudo systemctl reset-failed estest 2>/dev/null || true
    sudo umount $L 2>/dev/null || true
    echo \"profiles: \$(ls /home/ark/perf/msx-prof | wc -l) files, \$(du -sh /home/ark/perf/msx-prof | cut -f1)\"" < /dev/null
done
$D 'sudo cp /tmp/vulkan.conf.ab-orig /etc/darkos/vulkan.conf; grep -c "^armsx2-sdl" /etc/darkos/vulkan.conf; mount | grep -c standalone-pcsx2sa; bash /tmp/estest.sh stop; systemctl is-active emulationstation' < /dev/null
