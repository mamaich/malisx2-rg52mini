#!/bin/bash
# On the PC: DOLUSER=<dir> dolphin-ab.sh <rounds> <label=path.so[@NAME=v,...]>... - Dolphin (nogui on
# DRM, HLE audio) on each given Vulkan driver in turn, interleaved within a round, ES stopped
# ("@NAME=v,..." is more environment for that label). GAMES="mp nd" by default: Metroid Prime from
# boot ($MP_ROM) and ND Demo ($ND_ROM), RUNS seconds each (70); fps and frame times by the kernel's
# flip count (flipseg2.sh) between the shown frames FR (default "300 1300 1300 2100"). Each label
# runs on its own copy of Dolphin's user directory (<DOLUSER>-ab-<label>, made once and kept), so
# its shader and pipeline caches are its own and alternating drivers do not throw them away: round 1
# warms them, later rounds are warm runs. Before each run the SoC cools below 62 C. 60 s in, the GPU
# memory of all kbase contexts (ES stopped: Dolphin's) and Dolphin's VmRSS end the result line.
# BENCH_PRE: a command for the console before every game (say, one that sets the volume).
H=$(dirname "$(readlink -f "$0")")
D="bash $H/../dev.sh"; rounds=$1; shift
U=${DOLUSER:?set DOLUSER to a copy of Dolphin user directory on the console}
MP_ROM=${MP_ROM:-"/roms/gc/Metroid Prime (USA) (Rev 2).rvz"}
ND_ROM=${ND_ROM:-"/roms/gc/ND Demo - Tech Demo (USA).iso"}
FR=${FR:-300 1300 1300 2100}
$D "cat > /tmp/flipseg2.sh" < $H/flipseg2.sh
for v in "$@"; do
  lbl=${v%%=*}
  $D "[ -d '$U-ab-$lbl' ] || cp -a '$U' '$U-ab-$lbl'; du -sh '$U-ab-$lbl' | cut -f1 | sed 's/^/$lbl user dir: /'" < /dev/null
done
for r in $(seq 1 $rounds); do
  for g in ${GAMES:-mp nd}; do
    case $g in
      mp) rom=$MP_ROM ;;
      nd) rom=$ND_ROM ;;
      *) echo "unknown game $g"; continue ;;
    esac
    for v in "$@"; do
      lbl=${v%%=*}; so=${v#*=}; envs=${so#*@}; [ "$envs" = "$so" ] && envs=; so=${so%%@*}
      $D "${BENCH_PRE:+$BENCH_PRE; }for i in \$(seq 1 60); do [ \$(cat /sys/class/thermal/thermal_zone0/temp) -lt 62000 ] && break; sleep 3; done
        t0=\$(cat /sys/class/thermal/thermal_zone0/temp)
        sudo systemctl stop emulationstation; sleep 2
        b=\$(cat /sys/class/backlight/backlight/brightness); [ \"\$b\" = 0 ] && [ -s /run/darkos-brightness ] && sudo sh -c 'cat /run/darkos-brightness > /sys/class/backlight/backlight/brightness'
        sudo bash /tmp/flipseg2.sh start
        rm -f /tmp/dolab-mem.txt
        ( sleep 60; p=\$(pgrep -x dolphin-emu-nog | head -1); g=\$(sudo awk '/kctx/ {s += \$NF} END {printf \"%d\", s * 4 / 1024}' /sys/kernel/debug/mali0/gpu_memory 2>/dev/null)
          r=\$(awk '/^VmRSS:/ {printf \"%d\", \$2 / 1024}' /proc/\$p/status 2>/dev/null); echo \"gpu \${g}M rss \${r}M at 60 s\" > /tmp/dolab-mem.txt ) &
        DARKOS_VULKAN=$so ${envs//,/ } DOLPHIN_EMU_USERPATH='$U-ab-$lbl/' timeout -s INT ${RUNS:-70} /opt/dolphin/dolphin-emu-nogui -p drm -a HLE \
          -e '$rom' > /tmp/dolab-$g-$lbl.log 2>&1; rc=\$?
        res=\$(sudo bash /tmp/flipseg2.sh stop '$g/$lbl#$r' $FR)
        wait
        sleep 1; pgrep -x dolphin-emu-nog > /dev/null && sudo pkill -9 -x dolphin-emu-nog
        e=\$(grep -a -c -E 'E\\[|device lost' /tmp/dolab-$g-$lbl.log)
        echo \"\$res | \$((t0/1000))->\$((\$(cat /sys/class/thermal/thermal_zone0/temp)/1000)) C | rc \$rc, \$e error lines | \$(cat /tmp/dolab-mem.txt 2>/dev/null)\"" < /dev/null
      sleep 3
    done
  done
done
$D 'sudo systemctl start emulationstation; sleep 2; systemctl is-active emulationstation' < /dev/null
