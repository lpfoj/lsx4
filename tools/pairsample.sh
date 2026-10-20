#!/bin/bash
# Do two threads work at the same time, or do they take turns?
#
# Aggregate schedstat cannot answer this: "Game:Main blocked 49%, GpuComm blocked
# 58%" is equally consistent with an honest shortage of work and with a strict
# hand-off where each thread sleeps while the other runs. The two demand opposite
# fixes, so the overlap has to be sampled directly - both threads at the same
# instant, many times.
#
# usage: pairsample.sh <serial> [seconds] [nameA] [nameB]
# Costs about one core on the device for the duration; the game uses ~3.2 of 8,
# so the sampler competes for a spare core rather than the ones under test.

set -u
export MSYS2_ARG_CONV_EXCL='*'
SERIAL="${1:?serial required}"
SECONDS_TO_RUN="${2:-8}"
NAME_A="${3:-Game:Main}"
NAME_B="${4:-GpuComm}"
PKG=app.lsx4.android

a() { adb -s "$SERIAL" "$@"; }

PID=$(a shell pidof "$PKG" 2>/dev/null | tr -d '\r' | awk '{print $1}')
if [ -z "$PID" ]; then echo "NOT_RUNNING"; exit 1; fi

# comm is truncated to 15 characters by the kernel, so match on a prefix.
find_tid() {
  a shell "for d in /proc/$PID/task/*; do n=\$(cat \$d/comm 2>/dev/null); case \"\$n\" in *$1*) echo \${d##*/} \$n;; esac; done" 2>/dev/null | tr -d '\r' | head -1
}

RA=$(find_tid "$NAME_A"); TA=${RA%% *}
RB=$(find_tid "$NAME_B"); TB=${RB%% *}
if [ -z "$TA" ] || [ -z "$TB" ]; then
  echo "TID_NOT_FOUND A='$RA' B='$RB'"; exit 2
fi
echo "pid=$PID  A=$RA  B=$TB $(echo "$RB" | cut -d' ' -f2-)"

# One shell loop reading both stat files back to back: the pair is as close to
# simultaneous as /proc allows. State is the field after the last ')', because a
# thread name may itself contain spaces or parentheses.
RAW=$(a shell "
  end=\$((\$(date +%s) + $SECONDS_TO_RUN))
  while [ \$(date +%s) -lt \$end ]; do
    sa=\$(cat /proc/$PID/task/$TA/stat 2>/dev/null); sa=\${sa##*) }
    sb=\$(cat /proc/$PID/task/$TB/stat 2>/dev/null); sb=\${sb##*) }
    echo \"\${sa%% *}\${sb%% *}\"
  done
" 2>/dev/null | tr -d '\r')

python3 - "$NAME_A" "$NAME_B" <<PY
import sys, collections
name_a, name_b = sys.argv[1], sys.argv[2]
pairs = [l.strip() for l in """$RAW""".splitlines() if len(l.strip()) == 2]
n = len(pairs)
if n == 0:
    print("NO_SAMPLES"); raise SystemExit(1)
c = collections.Counter(pairs)
both = sum(v for k, v in c.items() if k[0] == 'R' and k[1] == 'R')
only_a = sum(v for k, v in c.items() if k[0] == 'R' and k[1] != 'R')
only_b = sum(v for k, v in c.items() if k[0] != 'R' and k[1] == 'R')
neither = n - both - only_a - only_b
pa, pb = (both + only_a) / n, (both + only_b) / n
print("samples            %d" % n)
print("both running       %5.1f%%" % (100 * both / n))
print("only %-13s %5.1f%%" % (name_a, 100 * only_a / n))
print("only %-13s %5.1f%%" % (name_b, 100 * only_b / n))
print("neither            %5.1f%%" % (100 * neither / n))
print()
# If the two were independent, both would be running p(A)*p(B) of the time.
# Measured overlap far below that is a hand-off; at or above it, they are simply
# short of work and the sleeping is honest.
exp = pa * pb
print("expected overlap if independent  %5.1f%%" % (100 * exp))
print("measured / expected              %5.2f" % ((both / n / exp) if exp else 0))
print()
if exp and (both / n) < 0.35 * exp:
    print("VERDICT: hand-off. The threads take turns, so the frame is a chain of")
    print("serial segments. Overlapping them pays immediately, with no change to")
    print("the amount of work done.")
elif exp and (both / n) > 0.75 * exp:
    print("VERDICT: independent. The sleeping is an honest shortage of work, not a")
    print("lock. Only doing less work shortens the frame.")
else:
    print("VERDICT: mixed. Partial serialisation - worth a second run before")
    print("spending anything on it.")
PY
