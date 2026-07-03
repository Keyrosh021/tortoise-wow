#!/usr/bin/env bash
# auto_build_deploy.sh — RUN THIS ON THE HOST (your actual machine), not in any sandbox.
# Bridges Cowork scheduled runs (which can only edit files) to the live server:
# watches signals/build_request.txt; on a new request it builds mangosd, deploys it
# per WORKER_HANDOFF.md §4, and writes the outcome (incl. compile errors) to
# signals/build_result.txt for the next scheduled run to read.
#
# Start it once and leave it running:
#   nohup bash tools/auto_build_deploy.sh >> tools/auto_build_deploy.log 2>&1 & disown

REPO="/home/zuppier/tw/server_dev/tortoise-wow-pr79-replay"
SIG="$REPO/signals"
STATE="$SIG/.last_processed"
mkdir -p "$SIG"
touch "$STATE"
echo "[watcher] started $(date '+%F %T') pid=$$"

ensure_supervisor() {
    if ! pgrep -f 'while true.*mangosd' >/dev/null 2>&1; then
        echo "[watcher] supervisor dead — relaunching"
        setsid bash -c "cd $REPO; while true; do echo \"=== START \$(date +%T) ===\" >> boot.log; tail -f /dev/null | ./bin/mangosd >> boot.log 2>&1; echo \"=== EXIT \$? \$(date +%T) ===\" >> boot.log; sleep 3; done" >/dev/null 2>&1 &
        disown
    fi
}

ensure_db() {
    mysqladmin -h127.0.0.1 -P3306 -uroot -pmangos ping >/dev/null 2>&1 && return 0
    echo "[watcher] mariadb down — starting"
    (cd "$REPO/db" && bash start_mariadb_tortoise.sh) >/dev/null 2>&1 || true
}

deploy() {
    cd "$REPO" || return 1
    local SUP MG
    SUP=$(pgrep -f 'while true.*mangosd' | head -1 || true)
    MG=$(fuser bin/mangosd 2>/dev/null | tr -d ' ' || true)
    [ -n "$SUP" ] && kill -STOP "$SUP" 2>/dev/null || true
    if [ -n "$MG" ]; then
        kill -TERM "$MG" 2>/dev/null || true
        for i in $(seq 1 18); do kill -0 "$MG" 2>/dev/null || break; sleep 1; done
        if kill -0 "$MG" 2>/dev/null; then kill -9 "$MG" 2>/dev/null || true; fi
        for i in $(seq 1 15); do
            st=$(ps -o stat= -p "$MG" 2>/dev/null | tr -d ' ')
            if [ -z "$st" ] || [ "${st:0:1}" = "Z" ]; then break; fi
            sleep 1
        done
    fi
    cmake --install build >/dev/null 2>&1 || true
    local OK=1
    if ! cmp -s bin/mangosd build/src/mangosd/mangosd; then OK=0; fi
    if [ -n "$SUP" ]; then kill -CONT "$SUP" 2>/dev/null || true; else ensure_supervisor; fi
    return $((1 - OK))
}

while true; do
    REQ="$SIG/build_request.txt"
    if [ -f "$REQ" ] && [ "$REQ" -nt "$STATE" ]; then
        echo "[watcher] $(date '+%F %T') request: $(head -1 "$REQ")"
        cd "$REPO" || { sleep 30; continue; }
        BUILD_LOG=$(mktemp)
        if cmake --build build -j6 --target mangosd >"$BUILD_LOG" 2>&1; then
            if deploy; then
                {
                    echo "status=OK ts=$(date '+%F %T')"
                    echo "request=$(head -1 "$REQ")"
                    echo "deployed_md5=$(md5sum bin/mangosd | cut -d' ' -f1)"
                } >"$SIG/build_result.txt"
                echo "[watcher] deployed OK"
            else
                {
                    echo "status=DEPLOY_MISMATCH ts=$(date '+%F %T')"
                    echo "request=$(head -1 "$REQ")"
                    echo "note=build succeeded but bin/mangosd != build/src/mangosd/mangosd after install"
                } >"$SIG/build_result.txt"
                echo "[watcher] DEPLOY MISMATCH"
            fi
        else
            {
                echo "status=BUILD_FAILED ts=$(date '+%F %T')"
                echo "request=$(head -1 "$REQ")"
                echo "---- errors (first 80 matching lines) ----"
                grep -aiE "error:|undefined reference|fatal" "$BUILD_LOG" | head -80
            } >"$SIG/build_result.txt"
            echo "[watcher] build FAILED"
        fi
        rm -f "$BUILD_LOG"
        touch "$STATE"
    fi
    ensure_db
    ensure_supervisor
    sleep 30
done
