#!/usr/bin/env bash
# Run inside a relay VM to produce a relays.conf stanza and the public key.
set -euo pipefail

PORT="${RELAY_PORT:-9001}"
KEY_DIR="/etc/onion/keys"
PRIVATE_KEY="$KEY_DIR/relay.pem"
PUBLIC_KEY="$KEY_DIR/relay.pub.pem"
ADDR_FILE="$KEY_DIR/advertise.addr"

if [[ ! -f "$PRIVATE_KEY" || ! -f "$PUBLIC_KEY" ]]; then
    echo "ERROR: relay keypair not found in $KEY_DIR" >&2
    exit 1
fi

if [[ -s "$ADDR_FILE" ]]; then
    ADDR="$(cat "$ADDR_FILE")"
elif command -v tailscale >/dev/null 2>&1; then
    ADDR="$(tailscale ip -4 | head -n1):$PORT"
elif [[ -f /etc/wireguard/wg0.conf ]]; then
    ADDR="$(grep '^Address = ' /etc/wireguard/wg0.conf | head -n1 | awk '{print $3}'):$PORT"
else
    echo "ERROR: cannot determine overlay address; pass --advertise-addr when starting relay" >&2
    exit 1
fi

echo "# Copy this line into the sender's relays.conf:"
echo "relay $ADDR $PUBLIC_KEY"
echo
echo "# Public key PEM:"
cat "$PUBLIC_KEY"
