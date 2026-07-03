#!/bin/bash
# Resilient mangosd runner for measurement.
# - Holds stdin open (tail -f /dev/null) so the console never sees EOF and
#   self-shuts-down when detached.
# - Auto-restarts on crash (matching how the baseline run was generated), and
#   logs each (re)start + exit code so we can SEE crash frequency.
# - HANG WATCHDOG: a frozen (not crashed) mangosd stays alive but stops advancing
#   the world, so the crash-restart loop (which only fires on process EXIT) would
#   stall forever. The watchdog detects "no bot_events.csv progress" and kills the
#   hung process so the loop restarts it -- no more long-term freezes.
cd "$(dirname "$0")" || exit 1
: > boot.log
restart=0

HANG_TIMEOUT=150   # seconds with no bot_events.csv write => world is frozen
(
  while true; do
    sleep 30
    pid=$(pidof mangosd 2>/dev/null) || continue
    [ -z "$pid" ] && continue
    pid=$(echo "$pid" | awk '{print $1}')   # one mangosd expected; take the first
    uptime=$(ps -o etimes= -p "$pid" 2>/dev/null | tr -d ' ')
    # require the world to be up a while (past boot) before trusting the progress signal
    [ -z "$uptime" ] && continue
    [ "$uptime" -le 180 ] && continue
    [ -f logs/bot_events.csv ] || continue
    last=$(stat -c %Y logs/bot_events.csv 2>/dev/null) || continue
    age=$(( $(date +%s) - last ))
    if [ "$age" -gt "$HANG_TIMEOUT" ]; then
      msg="=== HANG DETECTED: no bot_events.csv progress for ${age}s (uptime ${uptime}s), killing pid=$pid $(date '+%F %T') ==="
      echo "$msg" >> boot.log
      echo "$msg" >> crashes.log
      kill -9 "$pid" 2>/dev/null
      sleep 5
    fi
  done
) &
WATCHDOG_PID=$!

# LEARNING REPORT loop: every run automatically digests what the fleet learned
# (per-fight reward + the cohort A/B experiment) into a saved, timestamped report
# under logs/learning_reports/. So we always walk away with something each run.
(
  sleep 300   # let bots log in + have some fights first
  while true; do
    python3 tools/bot_learning_report.py --save >/dev/null 2>&1
    sleep 600
  done
) &
REPORT_PID=$!
trap 'kill $WATCHDOG_PID $REPORT_PID 2>/dev/null' EXIT INT TERM

tail -f /dev/null | while true; do
    echo "=== START #$restart $(date '+%F %T') ===" >> boot.log
    ./bin/mangosd >> boot.log 2>&1
    code=$?
    echo "=== EXIT #$restart code=$code $(date '+%F %T') ===" >> boot.log
    # Preserve crash backtraces across the restart-truncate (boot.log gets large/rotated).
    if [ "$code" -ge 128 ]; then
        { echo "=== CRASH #$restart code=$code $(date '+%F %T') ==="; tail -n 40 boot.log; echo; } >> crashes.log
    fi
    restart=$((restart+1))
    sleep 2
done
