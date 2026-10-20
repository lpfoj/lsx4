#!/bin/bash
# What are the blocked threads waiting ON?
#
# schedstat says a thread is blocked; it does not say on what. If every worker is
# parked on the SAME futex word, one lock is serialising the whole game and no
# amount of code optimisation will fill the idle cores. If they are parked on
# different words - or not on futexes at all - the sleeping is honest and the
# frame really is short of work.
#
# /proc/<tid>/syscall gives the syscall number and its arguments for a thread
# that is currently blocked in the kernel; for futex(2) the first argument is the
# address being waited on. Sampling it costs two file reads per thread and does
# not stop anything, unlike a tracer.
#
# usage: futexmap.sh <serial> [seconds] [name-filter]

set -u
export MSYS2_ARG_CONV_EXCL='*'
SERIAL="${1:?serial required}"
SECONDS_TO_RUN="${2:-8}"
FILTER="${3:-}"
PKG=app.lsx4.android

a() { adb -s "$SERIAL" "$@"; }

PID=$(a shell pidof "$PKG" 2>/dev/null | tr -d '\r' | awk '{print $1}')
if [ -z "$PID" ]; then echo "NOT_RUNNING"; exit 1; fi
echo "pid=$PID  окно ${SECONDS_TO_RUN}s"

RAW=$(a shell "
  end=\$((\$(date +%s) + $SECONDS_TO_RUN))
  while [ \$(date +%s) -lt \$end ]; do
    for d in /proc/$PID/task/*; do
      n=\$(cat \$d/comm 2>/dev/null)
      case \"\$n\" in ${FILTER:+*$FILTER*}${FILTER:-*}) ;; *) continue;; esac
      s=\$(cat \$d/syscall 2>/dev/null)
      echo \"\$n|\$s\"
    done
  done
" 2>/dev/null | tr -d '\r')

TMP="${TMPDIR:-/tmp}/futexmap.$$"
printf '%s\n' "$RAW" > "$TMP"

python3 - "$TMP" <<'PY'
import collections, sys
lines = [l for l in open(sys.argv[1], encoding='utf-8', errors='replace') if '|' in l]
# arm64: futex is syscall 98; the first argument is the word being waited on.
FUTEX = '98'
per_thread_waits = collections.defaultdict(collections.Counter)
totals = collections.Counter()
samples = collections.Counter()
for line in lines:
    name, _, rest = line.partition('|')
    name = name.strip()
    fields = rest.split()
    samples[name] += 1
    if not fields:
        continue
    if fields[0] == FUTEX and len(fields) > 1:
        addr = fields[1]
        per_thread_waits[name][addr] += 1
        totals[addr] += 1
    elif fields[0] == 'running':
        per_thread_waits[name]['<on cpu>'] += 1

if not samples:
    print("NO_SAMPLES"); raise SystemExit(1)

print("%-20s %7s  %s" % ("поток", "проб", "где стоит (доля)"))
for name, n in samples.most_common(14):
    top = per_thread_waits[name].most_common(2)
    where = "  ".join("%s %.0f%%" % (a, 100.0 * c / n) for a, c in top)
    print("%-20s %7d  %s" % (name[:20], n, where))

print()
print("Самые популярные слова ожидания по всем потокам:")
for addr, n in totals.most_common(6):
    holders = sum(1 for t in per_thread_waits if addr in per_thread_waits[t])
    print("   %-18s проб %6d, потоков %d" % (addr, n, holders))

if totals:
    addr, n = totals.most_common(1)[0]
    holders = sum(1 for t in per_thread_waits if addr in per_thread_waits[t])
    share = n / sum(totals.values())
    print()
    if holders >= 3 and share > 0.5:
        print("ВЕРДИКТ: сериализация. %d потоков стоят на одном слове %s (%.0f%% всех"
              % (holders, addr, 100 * share))
        print("ожиданий). Это один замок на всю игру - ядра не заполнятся, пока он есть,")
        print("и оптимизация инструкций тут ничего не даст.")
    else:
        print("ВЕРДИКТ: единого замка не видно - ожидания рассеяны по %d словам."
              % len(totals))
        print("Значит простой честный, и заполнять ядра надо новой работой, а не")
        print("расшивкой блокировки.")
PY
rm -f "$TMP"
