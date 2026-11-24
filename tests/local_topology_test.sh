#!/usr/bin/env bash
# Local single-machine topology:
#   sender -> relay1 -> relay2 -> relay3 -> receiver
set -euo pipefail

BIN="${1:?usage: local_topology_test.sh <onion-binary> <workdir>}"
WORK="${2:?usage: local_topology_test.sh <onion-binary> <workdir>}"

rm -rf "$WORK"
mkdir -p "$WORK"

PIDS=()
cleanup() {
    if ((${#PIDS[@]} > 0)); then
        for pid in "${PIDS[@]}"; do
            kill "$pid" 2>/dev/null || true
        done
        wait 2>/dev/null || true
    fi
}
trap cleanup EXIT

for name in relay1 relay2 relay3 dest; do
    "$BIN" keygen --out "$WORK/keys" --name "$name" >"$WORK/keygen-$name.log" 2>&1
done

cat >"$WORK/relays.conf" <<EOF
relay 127.0.0.1:19101 "$WORK/keys/relay1.pub.pem"
relay 127.0.0.1:19102 "$WORK/keys/relay2.pub.pem"
relay 127.0.0.1:19103 "$WORK/keys/relay3.pub.pem"
destination 127.0.0.1:19100 "$WORK/keys/dest.pub.pem"
EOF

"$BIN" listen --port 19100 --key "$WORK/keys/dest.pem" --verbose \
    >"$WORK/dest.out" 2>"$WORK/dest.err" &
PIDS+=("$!")

for index in 1 2 3; do
    "$BIN" relay --listen "1910$index" --key "$WORK/keys/relay$index.pem" \
        --advertise-addr 127.0.0.1 --verbose \
        >"$WORK/relay$index.log" 2>&1 &
    PIDS+=("$!")
done

ready=0
for _ in $(seq 1 40); do
    if "$BIN" check --path "$WORK/relays.conf" >"$WORK/check.log" 2>&1; then
        ready=1
        break
    fi
    sleep 0.1
done
if [[ "$ready" -ne 1 ]]; then
    echo "check failed:" >&2
    cat "$WORK/check.log" >&2
    exit 1
fi

reachable_count=$(grep -c '\[reachable\]' "$WORK/check.log" || true)
if [[ "$reachable_count" -ne 4 ]]; then
    echo "expected 4 reachable hops, got $reachable_count" >&2
    cat "$WORK/check.log" >&2
    exit 1
fi

MESSAGE="hello from localhost through three relays"
"$BIN" send --path "$WORK/relays.conf" --dest 127.0.0.1:19100 \
    --message "$MESSAGE" --verbose >"$WORK/send.out" 2>"$WORK/send.err"

received=0
for _ in $(seq 1 40); do
    if grep -Fq "$MESSAGE" "$WORK/dest.out"; then
        received=1
        break
    fi
    sleep 0.1
done
if [[ "$received" -ne 1 ]]; then
    echo "message was not delivered" >&2
    cat "$WORK/send.err" "$WORK/dest.err" "$WORK/relay1.log" "$WORK/relay2.log" "$WORK/relay3.log" >&2
    exit 1
fi

for index in 1 2 3; do
    log_file="$WORK/relay$index.log"
    grep -q 'predecessor' "$log_file"
    grep -q 'successor' "$log_file"
    if grep -Fq "$MESSAGE" "$log_file"; then
        echo "relay $index logged plaintext" >&2
        exit 1
    fi
done

echo "local topology OK: 3 relays, message delivered, no relay logged plaintext"
