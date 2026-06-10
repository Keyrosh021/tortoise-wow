#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN_DIR="$ROOT_DIR/bin"
if [[ -f "$ROOT_DIR/etc/aiplayerbot.conf" ]]; then
    CONF_FILE="$ROOT_DIR/etc/aiplayerbot.conf"
else
    CONF_FILE="$BIN_DIR/aiplayerbot.conf"
fi
MANGOS_CONF="$BIN_DIR/mangosd.conf"
RESULT_DIR="$ROOT_DIR/benchmark_results"
MYSQL_BASE=(mysql -h127.0.0.1 -P3306 -uroot -pmangos -N)
COUNTS=("$@")

if [[ ${#COUNTS[@]} -eq 0 ]]; then
    COUNTS=(100 200 400 800)
fi

mkdir -p "$RESULT_DIR"

if [[ ! -f "$CONF_FILE" ]]; then
    echo "Missing config: $CONF_FILE" >&2
    exit 1
fi

if [[ ! -x "$BIN_DIR/mangosd" ]]; then
    echo "Missing binary: $BIN_DIR/mangosd" >&2
    exit 1
fi

BACKUP_FILE="$(mktemp "$RESULT_DIR/aiplayerbot.conf.backup.XXXXXX")"
cp "$CONF_FILE" "$BACKUP_FILE"
MANGOS_BACKUP_FILE="$(mktemp "$RESULT_DIR/mangosd.conf.backup.XXXXXX")"
cp "$MANGOS_CONF" "$MANGOS_BACKUP_FILE"

restore_config() {
    if [[ -f "$BACKUP_FILE" ]]; then
        cp "$BACKUP_FILE" "$CONF_FILE"
    fi
    if [[ -f "$MANGOS_BACKUP_FILE" ]]; then
        cp "$MANGOS_BACKUP_FILE" "$MANGOS_CONF"
    fi
}

cleanup() {
    restore_config
    if [[ -n "${SERVER_PID:-}" ]] && kill -0 "$SERVER_PID" 2>/dev/null; then
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
    pkill -f 'mangosd -c mangosd.conf' || true
    pkill -f "$BIN_DIR/mangosd" 2>/dev/null || true
}

trap cleanup EXIT INT TERM

set_conf_value() {
    local key="$1"
    local value="$2"
    sed -i -E "s|^[[:space:]]*(${key})[[:space:]]*=.*$|\\1 = ${value}|" "$CONF_FILE"
}

get_conf_value() {
    local key="$1"
    awk -F= -v key="$key" '
        $1 ~ "^[[:space:]]*" key "[[:space:]]*$" {
            gsub(/[[:space:]]/, "", $2);
            print $2;
        }
    ' "$CONF_FILE" | tail -n1
}

query_single() {
    local sql="$1"
    "${MYSQL_BASE[@]}" -e "$sql"
}

reset_rndbot_online_flags() {
    query_single "UPDATE tw_char.characters SET online = 0 WHERE account IN (SELECT id FROM tw_logon.account WHERE username LIKE 'RNDBOT%');" >/dev/null
}

start_server() {
    local count="$1"
    local run_dir="$2"
    local log_file="$run_dir/mangosd.log"

    pkill -f "$BIN_DIR/mangosd" 2>/dev/null || true
    local wait_seconds=0
    while pgrep -f "$BIN_DIR/mangosd" >/dev/null 2>&1; do
        if (( wait_seconds >= 30 )); then
            echo "Failed to stop existing mangosd processes" >&2
            return 1
        fi
        sleep 1
        ((wait_seconds += 1))
    done

    pushd "$BIN_DIR" >/dev/null
    setsid ./mangosd -c "$MANGOS_CONF" </dev/null >"$log_file" 2>&1 &
    SERVER_PID=$!
    popd >/dev/null
}

wait_for_startup() {
    local run_dir="$1"
    local log_file="$run_dir/mangosd.log"
    local seconds=0

    while (( seconds < 180 )); do
        if ! kill -0 "$SERVER_PID" 2>/dev/null; then
            return 1
        fi

        if grep -q 'World server is up and running!' "$log_file"; then
            return 0
        fi

        sleep 1
        ((seconds += 1))
    done

    return 1
}

measure_stage() {
    local count="$1"
    local run_dir="$2"
    local sample_file="$run_dir/samples.tsv"
    local max_logins expected_login_seconds timeout_seconds

    max_logins="$(awk -F= '/^[[:space:]]*AiPlayerbot.RandomBotsMaxLoginsPerInterval[[:space:]]*=/{gsub(/[[:space:]]/, "", $2); print $2}' "$CONF_FILE" | tail -n1)"
    if [[ -z "$max_logins" ]]; then
        max_logins=10
    fi

    expected_login_seconds=$(( (count + max_logins - 1) / max_logins ))
    timeout_seconds=$(( expected_login_seconds + 180 ))
    if (( timeout_seconds < 240 )); then
        timeout_seconds=240
    fi

    echo -e "elapsed\tonline\tevent_rows\tstate\trss_kb\tcpu_pct" >"$sample_file"

    local elapsed=0
    local stable_samples=0
    local last_online=-1
    local peak_online=0
    local final_state="timeout"

    while (( elapsed <= timeout_seconds )); do
        if ! kill -0 "$SERVER_PID" 2>/dev/null; then
            final_state="crashed"
            break
        fi

        local online event_rows ps_line state rss cpu
        online="$(query_single "SELECT COUNT(*) FROM tw_char.characters WHERE online = 1 AND account IN (SELECT id FROM tw_logon.account WHERE username LIKE 'RNDBOT%');")"
        event_rows="$(query_single 'SELECT COUNT(*) FROM tw_char.ai_playerbot_random_bots;')"
        ps_line="$(ps -p "$SERVER_PID" -o stat=,rss=,%cpu= | awk '{$1=$1; print}')"
        state="$(awk '{print $1}' <<<"$ps_line")"
        rss="$(awk '{print $2}' <<<"$ps_line")"
        cpu="$(awk '{print $3}' <<<"$ps_line")"

        echo -e "${elapsed}\t${online}\t${event_rows}\t${state}\t${rss}\t${cpu}" >>"$sample_file"

        if (( online > peak_online )); then
            peak_online=$online
        fi

        if (( online == count )); then
            final_state="reached_target"
            break
        fi

        if (( online == last_online )); then
            ((stable_samples += 1))
        else
            stable_samples=0
            last_online=$online
        fi

        if (( elapsed > expected_login_seconds + 60 && stable_samples >= 12 )); then
            final_state="stalled"
            break
        fi

        sleep 5
        ((elapsed += 5))
    done

    echo "count=${count}" >"$run_dir/summary.txt"
    echo "expected_login_seconds=${expected_login_seconds}" >>"$run_dir/summary.txt"
    echo "result=${final_state}" >>"$run_dir/summary.txt"
    echo "peak_online=${peak_online}" >>"$run_dir/summary.txt"
    echo "final_online=$(tail -n1 "$sample_file" | cut -f2)" >>"$run_dir/summary.txt"
    echo "final_event_rows=$(tail -n1 "$sample_file" | cut -f3)" >>"$run_dir/summary.txt"
}

for count in "${COUNTS[@]}"; do
    if ! [[ "$count" =~ ^[0-9]+$ ]]; then
        echo "Invalid count: $count" >&2
        exit 1
    fi

    restore_config
    set_conf_value 'AiPlayerbot.MinRandomBots' "$count"
    set_conf_value 'AiPlayerbot.MaxRandomBots' "$count"
    current_account_count="$(get_conf_value 'AiPlayerbot.RandomBotAccountCount')"
    if [[ -z "$current_account_count" ]]; then
        current_account_count=0
    fi
    min_account_count=$(( (count + 8) / 9 ))
    if (( current_account_count < min_account_count )); then
        set_conf_value 'AiPlayerbot.RandomBotAccountCount' "$min_account_count"
    fi
    sed -i -E "s|^[[:space:]]*(Console.Enable)[[:space:]]*=.*$|\\1 = 0|" "$MANGOS_CONF"
    sed -i -E "s|^[[:space:]]*(WorldServerPort)[[:space:]]*=.*$|\\1 = $((18000 + count))|" "$MANGOS_CONF"

    timestamp="$(date +%Y%m%d-%H%M%S)"
    run_dir="$RESULT_DIR/${timestamp}-${count}"
    mkdir -p "$run_dir"
    cp "$CONF_FILE" "$run_dir/aiplayerbot.conf"

    echo "[bench] starting count=$count"
    reset_rndbot_online_flags
    start_server "$count" "$run_dir"

    if ! wait_for_startup "$run_dir"; then
        echo "count=${count}" >"$run_dir/summary.txt"
        echo "result=startup_failed" >>"$run_dir/summary.txt"
        if [[ -n "${SERVER_PID:-}" ]] && kill -0 "$SERVER_PID" 2>/dev/null; then
            kill "$SERVER_PID" 2>/dev/null || true
            wait "$SERVER_PID" 2>/dev/null || true
        fi
        SERVER_PID=
        continue
    fi

    measure_stage "$count" "$run_dir"

    if [[ -n "${SERVER_PID:-}" ]] && kill -0 "$SERVER_PID" 2>/dev/null; then
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
    SERVER_PID=
done
