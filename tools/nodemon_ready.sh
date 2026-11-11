#!/bin/bash
# nodemon_ready.sh — can miniprof-nodemon open system-wide counters here?
# Yes if it holds CAP_PERFMON, or if perf_event_paranoid <= 0 (then any
# user may, and no capability is needed), or if we are root.
# Exit 0 = ready (prints why), 1 = not (prints what to do).
H=${1:-./miniprof-nodemon}
P=$(cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null || echo 2)
if getcap "$H" 2>/dev/null | grep -q cap_perfmon; then echo "ready: $H has CAP_PERFMON"; exit 0; fi
if [ "$P" -le 0 ]; then echo "ready: perf_event_paranoid=$P, no capability needed"; exit 0; fi
if [ "$(id -u)" = 0 ]; then echo "ready: running as root"; exit 0; fi
echo "not ready: perf_event_paranoid=$P and $H has no CAP_PERFMON — run: make nodemon-setcap" \
     "(or ask an admin: setcap cap_perfmon+ep $H)"
exit 1
