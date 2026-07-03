#!/bin/bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SOCKET="$ROOT_DIR/dbs/tortoise/run/mariadb.sock"
PID_FILE="$ROOT_DIR/dbs/tortoise/run/mariadb.pid"
ROOT_PASSWORD="${ROOT_PASSWORD:-mangos}"

if mysqladmin --socket="$SOCKET" -u root -p"$ROOT_PASSWORD" ping >/dev/null 2>&1; then
    mysqladmin --socket="$SOCKET" -u root -p"$ROOT_PASSWORD" shutdown
    echo "Tortoise MariaDB stopped cleanly"
    exit 0
fi

if [[ -f "$PID_FILE" ]]; then
    PID="$(cat "$PID_FILE")"
    if kill -0 "$PID" >/dev/null 2>&1; then
        kill "$PID"
        echo "Tortoise MariaDB process $PID stopped"
        exit 0
    fi
fi

echo "Tortoise MariaDB is not running"