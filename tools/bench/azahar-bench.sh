#!/bin/bash
# On the console: MBROM=<rom> azahar-bench.sh <label> [NAME=value...] - a 3DS game in RetroArch's
# Azahar core from the RetroArch state in slot 0 (the .state next to the ROM; only loaded, never
# written: config_save_on_exit and the state autosaves are off), every emulated frame timestamped by
# a uprobe on retro_run: the frame rate on the frames 60-360 and 360-660 after the load.
# EmulationStation is stopped and not started again: the caller does that after its last run.
# NAME=value: environment of the emulator (DARKOS_VULKAN=<driver .so>, MALISX2_STATS=5, knobs),
# except these, which the script takes:
#   CORE=<core .so>   another build of the core (bound over the installed one for the run; also
#                     $CVCORE). It must export retro_run.
#   UNCAP=1           no frame limit (vsync and audio sync off), so a scene that holds 60 fps shows
#                     what the drivers can do. Azahar is still paced by page flips: compare the GPU
#                     time per frame (overlap.py), not the frame rate.
#   GPUHZ=<Hz>        the GPU's devfreq capped at that once the frames run (900 MHz back at the end),
#                     to make a scene GPU-bound.
#   GPUTRACE=1        kbase's job-slot starts and stops and the DRM flips too, kept with the frames
#                     in /tmp/cvtrace-<label>.txt for overlap.py, slots.py and pace.py.
#   GRABS=<n,n,...>   screen grabs at these frames, /tmp/cvb-<label>-<n>.ppm (default: one at frame
#                     500, /tmp/cvb-<label>.ppm).
#   FRAMES=<n>        frames to run (670). MAXS=<s>: give up after that long (200).
#   COREOPT=<key>=<value>[,...]  core options for the run, e.g. citra_resolution_factor=3.
#   MEMMAX=<size>     MemoryMax of the run's unit, to see the emulator under less memory.
#   GAPS=1            also the frame rate from frame 660 to the end, the longest gap between frames
#                     and the gaps over 50 ms (stalls).
# The emulator's log is /tmp/cv-<label>.log.
label=$1; shift
ROM=${MBROM:?set MBROM to the ROM; its RetroArch state of slot 0 lies beside it}
C=/home/ark/.config/retroarch/cores/azahar_libretro.so
CORE=${CVCORE:-$C}
for x in "$@"; do case $x in CORE=*) CORE=${x#CORE=};; UNCAP=*) UNCAP=${x#UNCAP=};; GPUHZ=*) GPUHZ=${x#GPUHZ=};; GRABS=*) GRABS=${x#GRABS=}; GRABS=${GRABS//,/ };; FRAMES=*) FRAMES=${x#FRAMES=};; MAXS=*) MAXS=${x#MAXS=};; GPUTRACE=*) GPUTRACE=${x#GPUTRACE=};; MEMMAX=*) MEMMAX=${x#MEMMAX=};; GAPS=*) GAPS=${x#GAPS=};; COREOPT=*) COREOPT=${x#COREOPT=};; esac; done
U=mbrun; T=/sys/kernel/tracing
GPU=$(ls -d /sys/class/devfreq/*gpu* 2>/dev/null | head -1)
RUNOFF=0x$(readelf -Ws --dyn-syms $CORE | awk '$8 == "retro_run" {print $2; exit}')
sudo pkill -x retroarch; sleep 2; sudo pkill -9 -x retroarch 2>/dev/null
sudo systemctl stop $U 2>/dev/null; sudo systemctl reset-failed $U 2>/dev/null
sudo systemctl stop emulationstation 2>/dev/null
# ES's "black" screensaver turns the backlight off and ES lights it again; stopped meanwhile, the
# panel stays dark, so the brightness ES last set comes back here
b=$(cat /sys/class/backlight/backlight/brightness); [ "$b" = 0 ] && [ -s /run/darkos-brightness ] && sudo sh -c "cat /run/darkos-brightness > /sys/class/backlight/backlight/brightness"
while mountpoint -q $C; do sudo umount $C; done
[ "$CORE" = "$C" ] || sudo mount --bind $CORE $C || exit 1
# cool down a little between runs (on the charger it rarely gets below 65 C)
for i in $(seq 1 40); do [ $(cat /sys/class/thermal/thermal_zone0/temp) -lt ${COOL:-66000} ] && break; sleep 3; done
t0=$(( $(cat /sys/class/thermal/thermal_zone0/temp) / 1000 ))
cat > /tmp/cv.cfg <<CFG
network_cmd_enable = "true"
network_cmd_port = "55355"
savestates_in_content_dir = "true"
savestate_auto_load = "false"
savestate_auto_save = "false"
CFG
[ "$UNCAP" = 1 ] && echo 'video_vsync = "false"' >> /tmp/cv.cfg && echo 'audio_sync = "false"' >> /tmp/cv.cfg
# COREOPT=<key>=<value>[,...]: the run's core options are the user's (retroarch-core-options.cfg)
# with these replaced, from a copy in /tmp (global_core_options, core_options_path); the user's
# files are not touched. E.g. COREOPT=citra_async_shader_compilation=disabled.
if [ -n "$COREOPT" ]; then
  f=/home/ark/.config/retroarch/retroarch-core-options.cfg
  if [ -f $f ]; then cp $f /tmp/cv-opts.cfg; else : > /tmp/cv-opts.cfg; fi
  IFS=, read -ra kvs <<< "$COREOPT"
  for kv in "${kvs[@]}"; do sed -i "/^${kv%%=*} = /d" /tmp/cv-opts.cfg; echo "${kv%%=*} = \"${kv#*=}\"" >> /tmp/cv-opts.cfg; done
  echo 'global_core_options = "true"' >> /tmp/cv.cfg
  echo 'core_options_path = "/tmp/cv-opts.cfg"' >> /tmp/cv.cfg
fi
# the file offset of retro_run: through the LOAD segment that holds it (cross builds on the PC put their
# code 0x10000 above its file offset, the image build's chroot not)
off=$(readelf -lW $CORE | while read t o v p f m rest; do [ "$t" = LOAD ] && (( RUNOFF >= v && RUNOFF < v + m )) && { printf "0x%x" $(( RUNOFF - v + o )); break; }; done)
sudo sh -c "[ -d $T/events ] || mount -t tracefs nodev $T; echo 0 > $T/tracing_on; echo > $T/uprobe_events; echo 'p:uprobes/cv_run $CORE:$off' >> $T/uprobe_events; echo > $T/trace; echo 16384 > $T/buffer_size_kb; echo 1 > $T/events/uprobes/cv_run/enable; echo 1 > $T/tracing_on"
# GPUTRACE=1: kbase's job-slot starts and stops too (slot 0 fragment, 1 vertex/tiler/compute),
# and the DRM flip events, kept in /tmp/cvtrace-<label>.txt with the frames
[ -n "$GPUTRACE" ] && sudo sh -c "echo 1 > $T/events/mali/mali_job_slots_event/enable; echo 1 > $T/events/drm/drm_vblank_event_delivered/enable"
envs=(); for x in "$@"; do case $x in CORE=*|GRABS=*|FRAMES=*|MAXS=*|GPUTRACE=*|UNCAP=*|GPUHZ=*|MEMMAX=*|GAPS=*|COREOPT=*) ;; *) envs+=(--setenv="$x");; esac; done
[ -n "$MEMMAX" ] && envs+=(-p MemoryMax=$MEMMAX)
sudo systemd-run --quiet --unit=$U --uid=ark --gid=ark -p LimitNICE=-20 \
  -p WorkingDirectory=/home/ark --setenv=HOME=/home/ark --setenv=USER=ark "${envs[@]}" \
  bash -c "sudo perfmax '$ROM'; nice -n -19 /opt/retroarch/bin/retroarch -v -c /home/ark/.config/retroarch/retroarch.cfg --appendconfig /tmp/cv.cfg -L $C -e 0 '$ROM' > /tmp/cv.log 2>&1; sudo perfnorm"
fs=""; gs=""; n=0
for i in $(seq 1 ${MAXS:-200}); do
  sleep 1
  n=$(sudo grep -c cv_run $T/trace)
  if [ -n "$GPUHZ" ] && [ -z "$gpu_capped" ] && [ $n -gt 0 ]; then
    echo $GPUHZ | sudo tee $GPU/max_freq > /dev/null; gpu_capped=1
  fi
  if [ $n -gt 60 ]; then
    fs="$fs $(( $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq) / 1000 ))"
    gs="$gs $(cut -d@ -f1 $GPU/load 2>/dev/null)"
  fi
  [ -z "$started_grabs" ] && { started_grabs=1; GRABS_LEFT=${GRABS:-500}; }
  next=${GRABS_LEFT%% *}
  if [ -n "$GRABS_LEFT" ] && [ $n -ge $next ]; then
    GRABS_LEFT=$(echo "$GRABS_LEFT" | cut -s -d' ' -f2-)
    gl_label=$label; [ "${GRABS:-500}" != "500" ] && gl_label=$label-$next
    fmt=$(sudo cat /sys/kernel/debug/dri/0/state | awk '/^plane\[/{fb=0} /fb=[1-9]/{fb=1} fb && /format=/{sub(/.*format=/,""); print $1; exit}')
    case $fmt in AR24) f=bgra;; XR24) f=bgr0;; AB24) f=rgba;; XB24) f=rgb0;; *) f=bgr0;; esac
    sudo rm -f /tmp/cvb-$gl_label.ppm
    sudo ffmpeg -loglevel quiet -y -f kmsgrab -i - -frames:v 1 -vf hwdownload,format=$f,transpose=2,format=rgb24 -c:v ppm -f image2 /tmp/cvb-$gl_label.ppm
    sudo chmod 644 /tmp/cvb-$gl_label.ppm 2>/dev/null
  fi
  [ $n -ge ${FRAMES:-670} ] && [ -z "$GRABS_LEFT" ] && break
done
sudo sh -c "echo 0 > $T/tracing_on; echo 0 > $T/events/uprobes/cv_run/enable"
[ -n "$gpu_capped" ] && echo 900000000 | sudo tee $GPU/max_freq > /dev/null
[ -n "$GPUTRACE" ] && { sudo sh -c "echo 0 > $T/events/mali/mali_job_slots_event/enable; echo 0 > $T/events/drm/drm_vblank_event_delivered/enable"; sudo cat $T/trace > /tmp/cvtrace-$label.txt; }
res=$(sudo grep cv_run $T/trace | awk -v gaps="$GAPS" '
  { for (i = 1; i <= NF; i++) if ($i ~ /^[0-9]+\.[0-9]+:$/) { t[++n] = substr($i, 1, length($i) - 1) + 0; break } }
  END { s1 = (n >= 360) ? 300 / (t[360] - t[60]) : 0; s2 = (n >= 660) ? 300 / (t[660] - t[360]) : 0
        printf "frames %d | fps 60-360 %.2f | 360-660 %.2f", n, s1, s2
        if (gaps) { s3 = (n > 1000) ? (n - 660) / (t[n] - t[660]) : 0; mx = 0; c = 0
          for (i = 61; i <= n; i++) { d = t[i] - t[i - 1]; if (d > mx) mx = d; if (d > 0.05) c++ }
          printf " | 660-end %.2f | max gap %.0f ms | gaps over 50 ms %d", s3, mx * 1000, c } }')
sudo sh -c "echo > $T/trace; echo > $T/uprobe_events"
mhz=$(echo $fs | awk '{for(i=1;i<=NF;i++) s+=$i; if (NF) printf "%.0f", s/NF}')
gl=$(echo $gs | awk '{for(i=1;i<=NF;i++) s+=$i; if (NF) printf "%.0f", s/NF}')
cp /tmp/cv.log /tmp/cv-$label.log
sudo pkill -x retroarch; sleep 2; sudo pkill -9 -x retroarch 2>/dev/null
sudo systemctl stop $U 2>/dev/null; sudo systemctl reset-failed $U 2>/dev/null
# the stock core back (ES starts Azahar through it)
while mountpoint -q $C; do sudo umount $C; done
echo "$label: $res | cpu avg ${mhz} MHz, gpu load avg ${gl}% | ${t0}->$(( $(cat /sys/class/thermal/thermal_zone0/temp) / 1000 )) C | $(cat /sys/class/power_supply/battery/capacity)% $(cat /sys/class/power_supply/battery/status) | $*"
