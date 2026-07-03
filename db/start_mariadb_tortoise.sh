#!/bin/bash

set -euo pipefail
shopt -s nullglob

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
INSTANCE_DIR="$ROOT_DIR/dbs/tortoise"
DATA_DIR="$INSTANCE_DIR/data"
RUN_DIR="$INSTANCE_DIR/run"
LOG_DIR="$INSTANCE_DIR/logs"
TMP_DIR="$INSTANCE_DIR/tmp"
SOCKET="$RUN_DIR/mariadb.sock"
PID_FILE="$RUN_DIR/mariadb.pid"
LOG_FILE="$LOG_DIR/error.log"
BOOTSTRAP_MARKER="$INSTANCE_DIR/.bootstrapped"
IMPORT_MARKER="$INSTANCE_DIR/.sql_imported"
CREATE_DATABASES_SQL="$ROOT_DIR/sql/create_databases.sql"
BASE_DIR="$ROOT_DIR/sql/base"
UPDATES_DIR="$ROOT_DIR/sql/database_updates"
ROOT_PASSWORD="${ROOT_PASSWORD:-mangos}"
BIND_IP="${MARIADB_TORTOISE_BIND_IP:-0.0.0.0}"
PORT="${MARIADB_TORTOISE_PORT:-3306}"
LAN_HOST_PATTERN="${MARIADB_TORTOISE_LAN_HOST_PATTERN:-192.168.1.%}"

mkdir -p "$RUN_DIR" "$LOG_DIR" "$TMP_DIR"

wait_for_login() {
    local attempts="$1"
    local use_password="$2"

    for _ in $(seq 1 "$attempts"); do
        if [[ "$use_password" == "yes" ]]; then
            if mysqladmin --socket="$SOCKET" -u root -p"$ROOT_PASSWORD" ping >/dev/null 2>&1; then
                return 0
            fi
        else
            if mysqladmin --socket="$SOCKET" -u root ping >/dev/null 2>&1; then
                return 0
            fi
        fi
        sleep 1
    done

    return 1
}

stop_running_instance() {
    if mysqladmin --socket="$SOCKET" -u root -p"$ROOT_PASSWORD" ping >/dev/null 2>&1; then
        mysqladmin --socket="$SOCKET" -u root -p"$ROOT_PASSWORD" shutdown >/dev/null 2>&1 || true
    elif [[ -f "$PID_FILE" ]] && kill -0 "$(cat "$PID_FILE")" >/dev/null 2>&1; then
        kill "$(cat "$PID_FILE")"
    fi

    for _ in {1..30}; do
        if [[ ! -f "$PID_FILE" ]]; then
            return 0
        fi

        if ! kill -0 "$(cat "$PID_FILE")" >/dev/null 2>&1; then
            rm -f "$PID_FILE"
            return 0
        fi

        sleep 1
    done

    echo "MariaDB instance did not stop cleanly. Check $LOG_FILE"
    exit 1
}

start_server() {
    local bind_ip="$1"

    nohup mariadbd \
        --datadir="$DATA_DIR" \
        --port="$PORT" \
        --bind-address="$bind_ip" \
        --socket="$SOCKET" \
        --pid-file="$PID_FILE" \
        --log-error="$LOG_FILE" \
        --tmpdir="$TMP_DIR" \
        >/dev/null 2>&1 &
}

init_data_dir() {
    if [[ -d "$DATA_DIR/mysql" ]]; then
        return
    fi

    mkdir -p "$DATA_DIR"
    mariadb-install-db \
        --datadir="$DATA_DIR" \
        --auth-root-authentication-method=normal \
        --skip-test-db \
        >/dev/null
}

bootstrap_users() {
    if [[ -f "$BOOTSTRAP_MARKER" ]]; then
        return
    fi

    start_server "127.0.0.1"

    if ! wait_for_login 30 no; then
        echo "Bootstrap MariaDB did not become ready. Check $LOG_FILE"
        exit 1
    fi

    mariadb --socket="$SOCKET" -u root <<SQL
ALTER USER 'root'@'localhost' IDENTIFIED BY '${ROOT_PASSWORD}';
CREATE USER IF NOT EXISTS 'root'@'127.0.0.1' IDENTIFIED BY '${ROOT_PASSWORD}';
CREATE USER IF NOT EXISTS 'root'@'${LAN_HOST_PATTERN}' IDENTIFIED BY '${ROOT_PASSWORD}';
GRANT ALL PRIVILEGES ON *.* TO 'root'@'localhost' WITH GRANT OPTION;
GRANT ALL PRIVILEGES ON *.* TO 'root'@'127.0.0.1' WITH GRANT OPTION;
GRANT ALL PRIVILEGES ON *.* TO 'root'@'${LAN_HOST_PATTERN}' WITH GRANT OPTION;
FLUSH PRIVILEGES;
SQL

    touch "$BOOTSTRAP_MARKER"
}

ensure_network_access() {
    mariadb --socket="$SOCKET" -u root -p"$ROOT_PASSWORD" <<SQL
CREATE USER IF NOT EXISTS 'root'@'127.0.0.1' IDENTIFIED BY '${ROOT_PASSWORD}';
CREATE USER IF NOT EXISTS 'root'@'${LAN_HOST_PATTERN}' IDENTIFIED BY '${ROOT_PASSWORD}';
ALTER USER 'root'@'localhost' IDENTIFIED BY '${ROOT_PASSWORD}';
ALTER USER 'root'@'127.0.0.1' IDENTIFIED BY '${ROOT_PASSWORD}';
ALTER USER 'root'@'${LAN_HOST_PATTERN}' IDENTIFIED BY '${ROOT_PASSWORD}';
GRANT ALL PRIVILEGES ON *.* TO 'root'@'localhost' WITH GRANT OPTION;
GRANT ALL PRIVILEGES ON *.* TO 'root'@'127.0.0.1' WITH GRANT OPTION;
GRANT ALL PRIVILEGES ON *.* TO 'root'@'${LAN_HOST_PATTERN}' WITH GRANT OPTION;
FLUSH PRIVILEGES;
SQL
}

import_sql() {
    if [[ -f "$IMPORT_MARKER" ]]; then
        return
    fi

    mariadb --socket="$SOCKET" -u root -p"$ROOT_PASSWORD" < "$CREATE_DATABASES_SQL"

    for sql_file in "$BASE_DIR"/*.sql; do
        mariadb --socket="$SOCKET" -u root -p"$ROOT_PASSWORD" tw_world < "$sql_file"
    done

    for sql_file in "$UPDATES_DIR"/*_world.sql; do
        mariadb --socket="$SOCKET" -u root -p"$ROOT_PASSWORD" tw_world < "$sql_file"
    done

    touch "$IMPORT_MARKER"
}

if mysqladmin --socket="$SOCKET" -u root -p"$ROOT_PASSWORD" ping >/dev/null 2>&1; then
    ensure_network_access
    echo "Tortoise MariaDB is already running on $SOCKET"
    echo "Bind:   $BIND_IP:$PORT"
    echo "Access: root@${LAN_HOST_PATTERN} granted on *.*"
    exit 0
fi

if [[ -f "$PID_FILE" ]] && kill -0 "$(cat "$PID_FILE")" >/dev/null 2>&1; then
    echo "Tortoise MariaDB appears to already be running with PID $(cat "$PID_FILE")"
    exit 0
fi

init_data_dir
bootstrap_users
import_sql
stop_running_instance

start_server "$BIND_IP"

if ! wait_for_login 30 yes; then
    echo "Tortoise MariaDB did not become ready. Check $LOG_FILE"
    exit 1
fi

ensure_network_access

echo "Tortoise MariaDB started"
echo "Bind:   $BIND_IP:$PORT"
echo "Access: root@${LAN_HOST_PATTERN} granted on *.*"
echo "Socket: $SOCKET"
echo "User:   root"
echo "Pass:   $ROOT_PASSWORD"
echo "Log:    $LOG_FILE"
