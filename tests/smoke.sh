#!/usr/bin/env bash
# test against a fake iPod folder and a fake Navidrome server
# usage: smoke.sh <navipod binary> <init_ipod binary> <mock_navidrome.py>
set -euo pipefail
NAVIPOD=$1
INIT=$2
MOCK_PY=$3
PORT=${NAVIPOD_TEST_PORT:-45533}

T=$(mktemp -d)
trap 'kill $MOCK 2>/dev/null || true; rm -rf "$T"' EXIT
export HOME="$T/home"
mkdir -p "$HOME" "$T/ipod/iPod_Control/Device"
printf 'ModelNumStr: xMB565\nFirewireGuid: 000A270012345678\npszSerialNumber: TEST123\n' \
    >"$T/ipod/iPod_Control/Device/SysInfo"
"$INIT" "$T/ipod"

python3 "$MOCK_PY" "$PORT" &
MOCK=$!
sleep 1

M="--mount $T/ipod"
check() {
    local got
    got=$("$NAVIPOD" info $M | sed -n 's/^Tracks: *\([0-9]*\) audio.*(\([0-9]*\) added.*/\1 \2/p')
    [ "$got" = "$1" ] || {
        echo "FAIL: $2 (got '$got', want '$1')"
        exit 1
    }
    echo "ok: $2"
}

"$NAVIPOD" pair --server "http://127.0.0.1:$PORT" --user t --password p $M >/dev/null
"$NAVIPOD" sync $M --all --playlist Mix --max-bitrate 192 --yes >/dev/null
check "4 4" "initial sync adds 4"
"$NAVIPOD" sync $M --yes >/dev/null
check "4 4" "second sync changes nothing"
"$NAVIPOD" sync $M --artist "Artist A" --yes >/dev/null
check "2 2" "narrowing the selection removes 2"
"$NAVIPOD" sync $M --artist "Artist A" --max-bitrate 100 --yes >/dev/null
check "2 2" "bitrate change re-transcodes"
"$NAVIPOD" unpair --erase --yes $M >/dev/null
check "0 0" "unpair --erase removes everything"
[ -d "$T/ipod/Photos" ] && echo "ok: Photos folder untouched"
echo "all passed"
