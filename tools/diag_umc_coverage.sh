#!/bin/bash
# diag_umc_coverage.sh — which NUMA domains' DRAM traffic each counter sees.
#
# For every NUMA node n: one process streams with cores AND memory on n
# (numactl -N n -m n), ~8 GB of loop reads, while miniprof-nodemon counts.
# Prints what the memory-controller counters saw per socket against the
# known traffic, then (sudo) compares the data-fabric DRAM-channel events
# for the last NUMA node: local / remote / both requester umasks.
#
# Usage (repo root, helper built and granted):  tools/diag_umc_coverage.sh
set -u
H=./miniprof-nodemon
make nodemon examples/membw_validate >/dev/null || exit 1
getcap "$H" | grep -q cap_perfmon || { echo "run: make nodemon-setcap"; exit 1; }
FAM=$(awk -F: '/^cpu family/{gsub(/ /,"",$2); print $2; exit}' /proc/cpuinfo)
UA=zen4; [ "$FAM" = 26 ] && UA=zen5
NN=$(ls -d /sys/devices/system/node/node* | wc -l)
N=33554432; K=10
EXP=$(awk -v n=$N -v k=$K 'BEGIN{printf "%.2f", 24*n*k/1e9}')
echo "uarch $UA, $NN NUMA nodes; each run: loop reads $EXP GB from its own node"
printf '\n%-6s %-34s %s\n' node "memory-controller reads (GB) per socket" "workload"
for n in $(seq 0 $((NN - 1))); do
    { sleep 0.3
      MINIPROF_MODE=off numactl -N $n -m $n ./examples/membw_validate $N $K > /tmp/diag_w.$$ 2>&1
      echo stop; } | "$H" --uarch $UA > /tmp/diag_nm.$$
    S=$(awk '/^socket/{split($3,a,"="); printf "s%s=%7.2f  ", $2, a[2]/1e9}' /tmp/diag_nm.$$)
    printf '%-6s %-34s %s\n' "$n" "$S" "$(grep -o 'loop rate [0-9.]* GB/s' /tmp/diag_w.$$)"
done
rm -f /tmp/diag_w.$$ /tmp/diag_nm.$$

if [ "$UA" = zen4 ] && sudo -n true 2>/dev/null; then
    L=$((NN - 1))
    echo; echo "data-fabric DRAM channels cs9..cs11 (memory on node $L), umask local 0x7fe / remote 0xbfe / both 0xffe:"
    EV=""
    for cs in 9 10 11; do
        e=$(printf '0x%x' $(( 0x1f + 0x40 * cs )))
        for um in 0x7fe 0xbfe 0xffe; do EV="$EV,amd_df/event=$e,umask=$um,name=cs${cs}_um$um/"; done
    done
    sudo -n perf stat -a -e "${EV#,}" -- numactl -N $L -m $L \
        env MINIPROF_MODE=off ./examples/membw_validate $N $K 2>&1 | grep -E 'cs[0-9]+_um|loop rate'
fi
