#!/usr/bin/env bash
# Generates a minimal WireGuard hub config on a VPS with a public IP.
# Usage: wireguard-hub-setup.sh [output-dir] [server-subnet-cidr] [client-address] [endpoint]
set -euo pipefail

command -v wg >/dev/null 2>&1 || { echo "wireguard-tools (wg) is required" >&2; exit 1; }

OUT="${1:-./wireguard-hub}"
SERVER_CIDR="${2:-10.9.0.1/24}"
CLIENT_ADDRESS="${3:-10.9.0.2/32}"
ENDPOINT="${4:-vpn.example.com:51820}"

mkdir -p "$OUT"
chmod 0700 "$OUT"

SERVER_PRIVATE="$(wg genkey)"
SERVER_PUBLIC="$(printf '%s' "$SERVER_PRIVATE" | wg pubkey)"
CLIENT_PRIVATE="$(wg genkey)"
CLIENT_PUBLIC="$(printf '%s' "$CLIENT_PRIVATE" | wg pubkey)"
SERVER_ADDRESS="${SERVER_CIDR%/*}"

cat >"$OUT/wg0.conf" <<EOF
[Interface]
Address = ${SERVER_CIDR}
ListenPort = 51820
PrivateKey = ${SERVER_PRIVATE}
SaveConfig = false

[Peer]
PublicKey = ${CLIENT_PUBLIC}
AllowedIPs = ${CLIENT_ADDRESS}
EOF
chmod 0600 "$OUT/wg0.conf"

cat >"$OUT/client.conf" <<EOF
[Interface]
Address = ${CLIENT_ADDRESS}
PrivateKey = ${CLIENT_PRIVATE}

[Peer]
PublicKey = ${SERVER_PUBLIC}
Endpoint = ${ENDPOINT}
AllowedIPs = ${SERVER_ADDRESS}/24
PersistentKeepalive = 25
EOF
chmod 0600 "$OUT/client.conf"

echo "server config: $OUT/wg0.conf   (copy to /etc/wireguard/wg0.conf on the VPS)"
echo "client config: $OUT/client.conf (copy to /etc/wireguard/wg0.conf on a laptop/VM)"
echo "server public key: $SERVER_PUBLIC"
echo "client public key: $CLIENT_PUBLIC"
echo
echo "On the VPS: wg-quick up wg0 && sysctl -w net.ipv4.ip_forward=1"
