#!/usr/bin/env bash
# hang_watch.sh SECONDS CMD...: run CMD; if still running after SECONDS,
# print every descendant's state, kernel wait channel and gdb backtrace,
# then kill it. Exit 124 on a hang.
t=$1; shift
"$@" & top=$!
for i in $(seq 1 "$t"); do kill -0 $top 2>/dev/null || { wait $top; exit $?; }; sleep 1; done
echo "=== HANG after ${t}s: $*"
desc() { for c in $(pgrep -P "$1"); do echo "$c"; desc "$c"; done; }
for p in $(desc $top); do
    echo "--- pid $p: $(tr '\0' ' ' < /proc/$p/cmdline 2>/dev/null | cut -c1-150)"
    grep -E "^(State|Threads)" /proc/$p/status 2>/dev/null
    echo "wchan: $(cat /proc/$p/wchan 2>/dev/null)"
    for tdir in /proc/$p/task/*; do echo "  tid ${tdir##*/}: $(cat $tdir/wchan 2>/dev/null) $(cat $tdir/stat 2>/dev/null | cut -d' ' -f3)"; done
    case "$(cat /proc/$p/comm 2>/dev/null)" in
        make|bash|sh|mpirun|prterun|timeout) ;;
        *) sudo gdb -q -batch -p "$p" -ex "thread apply all bt 25" 2>&1 | grep -E "^Thread|^#" | head -80 ;;
    esac
done
pkill -KILL -P $top; kill -KILL $top 2>/dev/null
exit 124
