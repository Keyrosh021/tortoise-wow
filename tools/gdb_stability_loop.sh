#!/usr/bin/env bash
set -euo pipefail

ROOT="/home/zuppier/tw/server_dev/tortoise-wow-pr79-replay"
BIN_DIR="$ROOT/bin"
LOG_DIR="$ROOT/logs"
RUN_DIR="$ROOT/stability_runs"
MYSQL=(mysql -h127.0.0.1 -P3306 -uroot -pmangos)

RUNS="${1:-5}"
SOAK_SECONDS="${2:-180}"
TARGET_BOT_COUNT="${3:-2000}"

mkdir -p "$RUN_DIR"

reset_state() {
  "${MYSQL[@]}" -e "
    DELETE FROM tw_char.ai_playerbot_random_bots WHERE event='login';
    UPDATE tw_char.characters SET online=0 WHERE online=1;
    UPDATE tw_char.ai_playerbot_random_bots SET value=${TARGET_BOT_COUNT} WHERE owner=0 AND event='bot_count';
  "
}

sample_online() {
  "${MYSQL[@]}" -Nse "
    SELECT COUNT(*)
    FROM tw_char.characters c
    JOIN tw_logon.account a ON a.id = c.account
    WHERE a.username LIKE 'RNDBOT%' AND c.online = 1;
  "
}

for run in $(seq 1 "$RUNS"); do
  echo "=== RUN ${run}/${RUNS} ==="
  pkill -f './mangosd -c mangosd.conf' || true
  pkill -f 'gdb -batch -ex run -ex bt --args ./mangosd -c mangosd.conf' || true
  sleep 2

  reset_state

  before_latest_log="$(ls -1t "$LOG_DIR"/server_*.log 2>/dev/null | head -n 1 || true)"
  gdb_out="$RUN_DIR/gdb_run_${run}.txt"

  (
    cd "$BIN_DIR"
    gdb -batch -ex run -ex bt --args ./mangosd -c mangosd.conf >"$gdb_out" 2>&1
  ) &
  gdb_pid=$!

  stable=1
  online=0
  for _ in $(seq 1 "$SOAK_SECONDS"); do
    if ! kill -0 "$gdb_pid" 2>/dev/null; then
      stable=0
      break
    fi
    sleep 1
  done

  if kill -0 "$gdb_pid" 2>/dev/null; then
    online="$(sample_online || echo 0)"
    pkill -f './mangosd -c mangosd.conf' || true
    sleep 2
    pkill -f 'gdb -batch -ex run -ex bt --args ./mangosd -c mangosd.conf' || true
    wait "$gdb_pid" || true
  else
    wait "$gdb_pid" || true
  fi

  after_latest_log="$(ls -1t "$LOG_DIR"/server_*.log 2>/dev/null | head -n 1 || true)"
  run_log="${after_latest_log:-$before_latest_log}"

  echo "run=${run} stable=${stable} online=${online} log=${run_log}" | tee "$RUN_DIR/run_${run}.summary"

  if [[ "$stable" != "1" ]]; then
    echo "RUN ${run} FAILED: gdb/mangosd exited before soak completed"
    exit 1
  fi

  if rg -q "SIGSEGV|Segmentation fault|Assertion|Aborted|malloc\\(" "$gdb_out" "$run_log"; then
    echo "RUN ${run} FAILED: crash signature detected"
    exit 1
  fi
done

echo "All ${RUNS} runs completed without crash signatures."
