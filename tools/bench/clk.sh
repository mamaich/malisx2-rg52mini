#!/bin/sh
# On the console: clk.sh [label] - one line with the clocks now: GPU and CPU (cluster of cpu0)
# current/max, DDR current, SoC temperature. The bench scripts call it in the middle of a run, so
# the mode a game ran in (dArkOS performance: GPU up to 900 MHz; overclock: 1000 MHz and the CPU's
# 2208 MHz step) and any throttling show in their logs.
g=/sys/class/devfreq/ff320000.gpu; c=/sys/devices/system/cpu/cpu0/cpufreq
echo "clocks${1:+ $1}: gpu $(($(cat $g/cur_freq) / 1000000))/$(($(cat $g/max_freq) / 1000000)) MHz, cpu $(($(cat $c/scaling_cur_freq) / 1000))/$(($(cat $c/scaling_max_freq) / 1000)) MHz, ddr $(($(cat /sys/class/devfreq/dmc/cur_freq) / 1000000)) MHz, $(($(cat /sys/class/thermal/thermal_zone0/temp) / 1000)) C"
