#!/bin/bash
# On the console: train-azahar.sh <instrumented libvulkan_malisx2 .so>
# Training runs of RetroArch's Azahar core for malisx2's PGO profile: each game boots on the
# instrumented driver with its malisx2 pipeline cache deleted, so the preload and the game compile
# everything, plays a while untouched, and quits through RetroArch (two QUITs) so the profile is
# written (/home/ark/perf/msx-prof, the directory the instrumented build was given). malisx2's
# caches of the games are kept and put back; libmali's (hash 24E2E0C4) are not touched.
# PGO_GAMES: the games, a line each, <3DS title id>:<rom>:<seconds> (no ' in the path). The
# image's profile trains on Super Mario 3D Land, 60 s, and Luigi's Mansion (00040000001D1A00),
# 90 s; not on the game the result is measured with. CORE: another build of the core.
so=$1; P=/home/ark/perf/msx-prof; SH=/roms/n3ds/Azahar/shaders/vulkan/pipeline; HOLD=/home/ark/perf/msx-trainhold
C=/home/ark/.config/retroarch/cores/azahar_libretro.so; CORE=${CORE:-$C}; U=pgot
GAMES=${PGO_GAMES:-"0004000000053F00:/roms/n3ds/Super Mario 3D Land (Europe) (En,Fr,De,Es,It,Nl,Pt,Ru) (Rev 2).3ds:60"}
mkdir -p $P $HOLD; rm -f $P/*
cat > /tmp/pgot.json <<J
{ "file_format_version": "1.0.1", "ICD": { "library_path": "$so", "api_version": "1.3.0" } }
J
printf 'network_cmd_enable = "true"\nnetwork_cmd_port = "55355"\n' > /tmp/pgot.cfg
sudo systemctl stop emulationstation; sudo pkill -x retroarch; sleep 1
# ES's "black" screensaver turns the backlight off and ES lights it again; stopped meanwhile, the
# panel stays dark, so the brightness ES last set comes back here
b=$(cat /sys/class/backlight/backlight/brightness); [ "$b" = 0 ] && [ -s /run/darkos-brightness ] && sudo sh -c "cat /run/darkos-brightness > /sys/class/backlight/backlight/brightness"
while mountpoint -q $C; do sudo umount $C; done; [ "$CORE" = "$C" ] || sudo mount --bind $CORE $C
quit() { for q in 1 2; do python3 -c "import socket; socket.socket(socket.AF_INET,socket.SOCK_DGRAM).sendto(b'QUIT',('127.0.0.1',55355))"; sleep 0.4; done
         for i in $(seq 1 100); do pgrep -x retroarch > /dev/null || return 0; sleep 0.2; done; sudo pkill -9 -x retroarch; return 1; }
echo "$GAMES" | while IFS=: read tid rom secs; do
  # malisx2's caches (any hash but libmali's 24E2E0C4) aside
  for f in $SH/$tid-*-*.bin; do case $f in *-24E2E0C4.bin) ;; *) [ -f "$f" ] && mv "$f" $HOLD/;; esac; done
  sudo systemctl reset-failed $U 2>/dev/null
  sudo systemd-run --quiet --unit=$U --uid=ark --gid=ark -p WorkingDirectory=/home/ark --setenv=HOME=/home/ark \
    --setenv=VK_DRIVER_FILES=/tmp/pgot.json \
    bash -c "nice -n -19 /opt/retroarch/bin/retroarch -v -c /home/ark/.config/retroarch/retroarch.cfg --appendconfig /tmp/pgot.cfg -L $C '$rom' > /tmp/pgot.log 2>&1"
  sleep $secs
  pre=$(grep -a -m1 "Preload:" /tmp/pgot.log | sed 's/.*Preload: //'); drv=$(grep -a -m1 -o "malisx2 build [0-9a-f]*" /tmp/pgot.log)
  quit && q=clean || q=killed
  # the training run's own cache goes, the kept ones come back
  for f in $SH/$tid-*-*.bin; do case $f in *-24E2E0C4.bin) ;; *) rm -f "$f";; esac; done
  mv $HOLD/$tid-* $SH/ 2>/dev/null
  echo "$tid: ${pre:-no preload line} | ${drv:-driver line missing} | exit $q | profiles $(ls $P | wc -l), $(du -sh $P | cut -f1)"
done
sudo systemctl reset-failed $U 2>/dev/null
while mountpoint -q $C; do sudo umount $C; done
sudo systemctl start emulationstation
