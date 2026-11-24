#!/usr/bin/env bash
# Builds an onion, tampers with bytes at different layers, and verifies
# the relay rejects each modified onion with DECRYPT_FAILED.
set -euo pipefail

BIN="${1:?usage: tamper_test.sh <onion-binary> <workdir>}"
WORK="${2:?usage: tamper_test.sh <onion-binary> <workdir>}"

command -v python3 >/dev/null 2>&1 || { echo "python3 is required" >&2; exit 1; }

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

for name in relay dest; do
    "$BIN" keygen --out "$WORK/keys" --name "$name" >"$WORK/keygen-$name.log" 2>&1
done

cat >"$WORK/relays.conf" <<EOF
relay 127.0.0.1:19201 "$WORK/keys/relay.pub.pem"
destination 127.0.0.1:19200 "$WORK/keys/dest.pub.pem"
EOF

"$BIN" listen --port 19200 --key "$WORK/keys/dest.pem" --verbose \
    >"$WORK/dest.out" 2>"$WORK/dest.err" &
PIDS+=("$!")
"$BIN" relay --listen 19201 --key "$WORK/keys/relay.pem" \
    --advertise-addr 127.0.0.1 --verbose >"$WORK/relay.log" 2>&1 &
PIDS+=("$!")

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

MESSAGE="tamper target message"
"$BIN" build --path "$WORK/relays.conf" --dest 127.0.0.1:19200 \
    --message "$MESSAGE" --out "$WORK/good.bin" >"$WORK/build.out" 2>"$WORK/build.err"

python3 - "$WORK/good.bin" "$WORK/bad-wrapped-key.bin" "$WORK/bad-tag.bin" <<'PY'
import sys
src, out_key, out_tag = sys.argv[1:4]
data = bytearray(open(src, "rb").read())

# Envelope layout: 4 magic + 1 version + 2 wrapped-key length; wrapped key starts at 7.
key_variant = bytearray(data)
key_variant[20] ^= 0x01
open(out_key, "wb").write(key_variant)

# The final bytes are AES-GCM ciphertext || tag.
tag_variant = bytearray(data)
tag_variant[-1] ^= 0x01
open(out_tag, "wb").write(tag_variant)
PY

for variant in bad-wrapped-key.bin bad-tag.bin; do
    set +e
    "$BIN" inject --addr 127.0.0.1:19201 --file "$WORK/$variant" \
        >"$WORK/inject-$variant.out" 2>&1
    rc=$?
    set -e
    if [[ "$rc" -eq 0 ]]; then
        echo "$variant was accepted by the relay" >&2
        exit 1
    fi
    grep -q 'DECRYPT_FAILED' "$WORK/inject-$variant.out" || {
        echo "$variant did not produce DECRYPT_FAILED" >&2
        cat "$WORK/inject-$variant.out" >&2
        exit 1
    }
done

grep -q 'decryption failure' "$WORK/relay.log"

"$BIN" inject --addr 127.0.0.1:19201 --file "$WORK/good.bin" \
    >"$WORK/inject-good.out" 2>"$WORK/inject-good.err"

received=0
for _ in $(seq 1 40); do
    if grep -Fq "$MESSAGE" "$WORK/dest.out"; then
        received=1
        break
    fi
    sleep 0.1
done
if [[ "$received" -ne 1 ]]; then
    echo "untampered onion was not delivered" >&2
    cat "$WORK/inject-good.err" "$WORK/relay.log" >&2
    exit 1
fi

echo "tamper test OK: wrapped-key and GCM-tag changes rejected, valid onion delivered"
