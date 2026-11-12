#!/bin/bash
# On the device: grab.sh <out.ppm> - the panel's current plane, turned to landscape.
fmt=$(sudo grep -A6 "plane\[57\]" /sys/kernel/debug/dri/0/state | grep -o "format=[A-Z0-9]*" | head -1 | cut -d= -f2)
case "$fmt" in AR24) pf=bgra;; AB24) pf=rgba;; XB24) pf=rgb0;; *) pf=bgr0;; esac
sudo rm -f "$1"
sudo ffmpeg -loglevel error -y -f kmsgrab -i - -frames:v 1 -vf hwdownload,format=$pf,transpose=2,format=rgb24 -c:v ppm -f image2 "$1"
sudo chmod 644 "$1"
