#!/bin/bash
# On the PC: armsx2-ab.sh <rounds> <label=path.so[@NAME=v,NAME=v]>... - ARMSX2 from ES's PS2 command
# (estest.sh) on each given Vulkan driver in turn, interleaved within a round; fps and frame times
# by the kernel's flip count (flipseg2.sh). GAMES="jak gow" by default:
#   jak   Jak and Daxter's intro from boot ($JAK_ROM), frames 300-1850;
#   gow   God of War's demo ($GOW_ROM) resumed from the savestate $GOW_STATE, frames 100-1500,
#         100-800, 800-1500;
#   gows, jaks   the same games from two more savestates of your own ($GOWS_STATE, $JAKS_STATE).
# The savestates are not in the repository: take them in ARMSX2 and put them on the console
# (defaults /home/ark/bench/{gow,gows,jaks}.p2s). A state goes in through a copy of ES's launcher
# with --state-from-file, bound over it for the run. A run from a savestate can write the game's
# real saves (memory cards): back them up first.
# The driver is chosen for ARMSX2 by an "armsx2-sdl = <path>" line in /etc/darkos/vulkan.conf (the
# file is put back at the end); 40 s in, the process's maps show which driver it really loaded.
# "@NAME=v,..." after a driver: environment for the emulator in that label's runs. REBOOT=1 reboots
# the console first (a clean start for a series). GOWS_PRESS / JAKS_PRESS: seconds after the start
# at which a button is pressed (press.py), to leave a pause menu or a dialog the state was taken
# in. 60 s in, the GPU memory of all kbase contexts (debugfs gpu_memory; ES is stopped, so the
# emulator's) and the process's VmRSS end the result line.
# BENCH_PRE: a command for the console before every game (say, one that sets the volume).
H=$(dirname "$(readlink -f "$0")")
D="bash $H/../dev.sh"; rounds=$1; shift
sos=""; for v in "$@"; do so=${v#*=}; sos="$sos ${so%%@*}"; done
JAK=${JAK_ROM:-"/roms/ps2/Jak and Daxter - The Precursor Legacy (Europe) (En,Ja,Fr,De,Es,It).iso"}
GOW=${GOW_ROM:-"/roms/ps2/God of War (Europe, Australia) (En,Fr,De,Es,It) (Demo).iso"}
ST_GOW=${GOW_STATE:-/home/ark/bench/gow.p2s}
ST_GOWS=${GOWS_STATE:-/home/ark/bench/gows.p2s}
ST_JAKS=${JAKS_STATE:-/home/ark/bench/jaks.p2s}
L=/usr/local/bin/standalone-pcsx2sa
if [ "${REBOOT:-0}" = 1 ]; then
  $D 'sudo /sbin/reboot' < /dev/null; sleep 20
  for i in $(seq 1 40); do $D uptime < /dev/null 2>/dev/null && break; sleep 3; done
  sleep 30
fi
$D "cat > /tmp/estest.sh" < $H/estest.sh
$D "cat > /tmp/press.py" < $H/press.py
$D "cat > /tmp/flipseg2.sh" < $H/flipseg2.sh
$D 'grep -q "^armsx2-sdl = /home/ark/" /etc/darkos/vulkan.conf || sudo cp /etc/darkos/vulkan.conf /tmp/vulkan.conf.ab-orig
  for s in st:'"$ST_GOW"' gows:'"$ST_GOWS"' jaks:'"$ST_JAKS"'; do
    sed "s#^/opt/pcsx2/bin/armsx2-sdl \"\$ROM\"#/opt/pcsx2/bin/armsx2-sdl --state-from-file ${s#*:} \"\$ROM\"#" '$L' > /tmp/standalone-${s%%:*}
    chmod 755 /tmp/standalone-${s%%:*}; done; grep -c state-from-file /tmp/standalone-st
  for so in '"$sos"'; do sha256sum $so | cut -c1-16; done' < /dev/null
for r in $(seq 1 $rounds); do
  for g in ${GAMES:-jak gow}; do
    hook=":"
    case $g in
      jak) rom=$JAK; pairs="300 1850"; mnt=":" ;;
      gow) rom=$GOW; pairs="100 1500 100 800 800 1500"; mnt="sudo mount --bind /tmp/standalone-st $L" ;;
      gows) rom=$GOW; pairs="100 1500 100 800 800 1500"; mnt="sudo mount --bind /tmp/standalone-gows $L"; hook="( sleep ${GOWS_PRESS:-12}; sudo python3 /tmp/press.py ${GOWS_KEY:-BTN_SOUTH} > /dev/null ) &" ;;
      jaks) rom=$JAK; pairs="100 1500 100 800 800 1500"; mnt="sudo mount --bind /tmp/standalone-jaks $L"
        # JAKS_PRESS=<s>: a press that long after the start, for the memory card dialog
        # Jak opens 10-12 s in: ARMSX2 (McdEnableEjection) ejects the cards for a moment
        # when it loads a state; 13 s dismisses it
        [ -n "$JAKS_PRESS" ] && hook="( sleep $JAKS_PRESS; sudo python3 /tmp/press.py ${JAKS_KEY:-BTN_SOUTH} > /dev/null ) &" ;;
    esac
    for v in "$@"; do
      lbl=${v%%=*}; so=${v#*=}; envs=${so#*@}; [ "$envs" = "$so" ] && envs=; so=${so%%@*}
      ea=""; for e in ${envs//,/ }; do ea="$ea --setenv=$e"; done
      $D "${BENCH_PRE:+$BENCH_PRE; }sudo sed -i '/^armsx2-sdl *=/d' /etc/darkos/vulkan.conf; echo 'armsx2-sdl = $so' | sudo tee -a /etc/darkos/vulkan.conf > /dev/null
        for i in \$(seq 1 60); do [ \$(cat /sys/class/thermal/thermal_zone0/temp) -lt 62000 ] && break; sleep 3; done
        t0=\$(cat /sys/class/thermal/thermal_zone0/temp)
        sudo umount $L 2>/dev/null; $mnt || exit 1
        sudo systemctl stop emulationstation; sleep 2
        sudo bash /tmp/flipseg2.sh start
        ( sleep 40; p=\$(pgrep -x armsx2-sdl | head -1); grep -o '/[^ ]*libvulkan_malisx2[^ ]*\\|/[^ ]*libmali-[^ ]*' /proc/\$p/maps 2>/dev/null | sort -u | tr '\n' ' ' > /tmp/ab-drv.txt ) &
        rm -f /tmp/ab-mem.txt
        ( sleep 60; p=\$(pgrep -x armsx2-sdl | head -1); g=\$(sudo awk '/kctx/ {s += \$NF} END {printf \"%d\", s * 4 / 1024}' /sys/kernel/debug/mali0/gpu_memory 2>/dev/null)
          r=\$(awk '/^VmRSS:/ {printf \"%d\", \$2 / 1024}' /proc/\$p/status 2>/dev/null); echo \"gpu \${g}M rss \${r}M at 60 s\" > /tmp/ab-mem.txt ) &
        $hook
        sed 's#--setenv=SDL_VIDEO_EGL_DRIVER=libEGL.so#--setenv=SDL_VIDEO_EGL_DRIVER=libEGL.so$ea#' /tmp/estest.sh > /tmp/estest-run.sh
        bash /tmp/estest-run.sh ps2 '$rom' 75 > /tmp/ab-start.txt 2>&1
        wait
        res=\$(sudo bash /tmp/flipseg2.sh stop '$g/$lbl#$r' $pairs)
        sudo systemctl stop estest 2>/dev/null; sudo systemctl reset-failed estest 2>/dev/null
        sudo pkill -x armsx2-sdl 2>/dev/null; sleep 2; sudo umount $L 2>/dev/null
        echo \"\$res | \$((t0/1000))->\$((\$(cat /sys/class/thermal/thermal_zone0/temp)/1000)) C | \$(cat /tmp/ab-drv.txt) | \$(cat /tmp/ab-mem.txt 2>/dev/null)\"" < /dev/null
      sleep 5
    done
  done
done
$D 'sudo cp /tmp/vulkan.conf.ab-orig /etc/darkos/vulkan.conf; grep -c "^armsx2-sdl" /etc/darkos/vulkan.conf; mount | grep -c standalone-pcsx2sa; bash /tmp/estest.sh stop; systemctl is-active emulationstation' < /dev/null
