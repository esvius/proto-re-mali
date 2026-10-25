#!/bin/bash
# Thermal-guarded build for a laptop: runs make on all cores at the lowest priority and pauses it while the CPU
# package is too hot (SIGSTOP above HOT, SIGCONT below COOL, degrees C).
# Usage: tools/cool_make.sh [make args...]      env: JOBS (all cores), HOT (93), COOL (85)
# The CPU throttles itself near 100°C; the laptop once shut down there, so the build pauses a few degrees below.
JOBS=${JOBS:-$(nproc)}
HOT=${HOT:-93}
COOL=${COOL:-85}
temp() {
    sensors 2>/dev/null | awk '/^Package id 0:/ {gsub(/[+°C]/, "", $4); print int($4); exit}'
}
nice -n 19 ionice -c 3 make -j"$JOBS" "$@" &
MAKE=$!
paused=0
while kill -0 $MAKE 2>/dev/null; do
    t=$(temp)
    if [ -n "$t" ]; then
        if [ $paused -eq 0 ] && [ "$t" -ge "$HOT" ]; then
            pkill -STOP -g "$(ps -o pgid= $MAKE | tr -d ' ')" 2>/dev/null || kill -STOP $MAKE
            paused=1
            echo "cool_make: ${t}°C, paused" >&2
        elif [ $paused -eq 1 ] && [ "$t" -le "$COOL" ]; then
            pkill -CONT -g "$(ps -o pgid= $MAKE | tr -d ' ')" 2>/dev/null || kill -CONT $MAKE
            paused=0
            echo "cool_make: ${t}°C, resumed" >&2
        fi
    fi
    sleep 3
done
wait $MAKE
