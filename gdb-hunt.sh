#!/bin/bash
cd /home/zuppier/tw/server_dev/tortoise-wow-pr79-replay
export ASAN_OPTIONS="handle_segv=0:abort_on_error=0:allow_user_segv_handler=1:log_path=logs/asan3"
exec gdb -batch \
  -ex "set pagination off" \
  -ex "handle SIGSEGV stop nopass" \
  -ex "run" \
  -ex "echo \n=== CRASH THREAD BT ===\n" \
  -ex "bt 25" \
  -ex "echo \n=== ALL THREADS ===\n" \
  -ex "thread apply all bt 8" \
  --args ./bin/mangosd
