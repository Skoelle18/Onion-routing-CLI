#!/usr/bin/env bash
# Vagrant shell provisioner for a single onion relay VM.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive

OVERLAY="${OVERLAY:-tailscale}"
RELAY_PORT="${RELAY_PORT:-9001}"
OVERLAY_HOSTNAME="${OVERLAY_HOSTNAME:-${RELAY_NAME:-onion-relay}}"
KEY_BITS="${KEY_BITS:-2048}"
TS_AUTHKEY="${TS_AUTHKEY:-}"
ADVERTISE_ADDR="${ADVERTISE_ADDR:-}"

echo ">>> installing build dependencies"
apt-get update -qq
apt-get install -y -qq ca-certificates curl gnupg g++ cmake libssl-dev pkg-config >/dev/null

if [[ "$OVERLAY" == "tailscale" ]]; then
    if [[ -z "$TS_AUTHKEY" ]]; then
        echo "ERROR: TS_AUTHKEY is empty. Run: TS_AUTHKEY=tskey-... vagrant up" >&2
        exit 1
    fi
    echo ">>> installing Tailscale"
    curl -fsSL https://pkgs.tailscale.com/stable/ubuntu/jammy.noarmor.gpg \
        -o /usr/share/keyrings/tailscale-archive-keyring.gpg
    curl -fsSL https://pkgs.tailscale.com/stable/ubuntu/jammy.tailscale-keyring.list \
        -o /etc/apt/sources.list.d/tailscale.list
    apt-get update -qq
    apt-get install -y -qq tailscale >/dev/null

    TS_FLAGS=""
    if [[ ! -c /dev/net/tun ]]; then
        echo ">>> /dev/net/tun unavailable; using Tailscale userspace networking"
        TS_FLAGS="--userspace-networking"
    fi

    echo ">>> joining Tailscale as $OVERLAY_HOSTNAME"
    # shellcheck disable=SC2086
    tailscale up --authkey="$TS_AUTHKEY" --hostname="$OVERLAY_HOSTNAME" $TS_FLAGS

    if [[ -z "$ADVERTISE_ADDR" ]]; then
        for _ in $(seq 1 30); do
            ADVERTISE_ADDR="$(tailscale ip -4 2>/dev/null | head -n1 || true)"
            [[ -n "$ADVERTISE_ADDR" ]] && break
            sleep 2
        done
    fi
    if [[ -z "$ADVERTISE_ADDR" ]]; then
        echo "ERROR: Tailscale did not assign an IPv4 address" >&2
        tailscale status >&2 || true
        exit 1
    fi
elif [[ "$OVERLAY" == "wireguard" ]]; then
    : "${WG_ADDRESS:?WG_ADDRESS is required for WireGuard (e.g. 10.9.0.2/32)}"
    : "${WG_PRIVATE_KEY:?WG_PRIVATE_KEY is required for WireGuard}"
    : "${WG_SERVER_PUBKEY:?WG_SERVER_PUBKEY is required for WireGuard}"
    : "${WG_ENDPOINT:?WG_ENDPOINT is required for WireGuard (host:port)}"

    echo ">>> configuring WireGuard"
    apt-get install -y -qq wireguard >/dev/null
    install -d -m 0700 /etc/wireguard
    cat >/etc/wireguard/wg0.conf <<EOF
[Interface]
Address = ${WG_ADDRESS}
PrivateKey = ${WG_PRIVATE_KEY}

[Peer]
PublicKey = ${WG_SERVER_PUBKEY}
Endpoint = ${WG_ENDPOINT}
AllowedIPs = ${WG_ALLOWED_IPS:-10.9.0.0/24}
PersistentKeepalive = 25
EOF
    chmod 0600 /etc/wireguard/wg0.conf
    wg-quick up wg0
    systemctl enable wg-quick@wg0
    if [[ -z "$ADVERTISE_ADDR" ]]; then
        ADVERTISE_ADDR="${WG_ADDRESS%%/*}"
    fi
else
    echo "ERROR: OVERLAY must be 'tailscale' or 'wireguard'" >&2
    exit 1
fi

echo ">>> building onion binary"
rm -rf /opt/onion-src /opt/onion-build /opt/onion
install -d -m 0755 /opt/onion-src /opt/onion-build
tar -C /vagrant \
    --exclude='./build' \
    --exclude='./.vagrant' \
    --exclude='./keys' \
    --exclude='./local-run' \
    --exclude='./*.pem' \
    --exclude='./*.key' \
    -cf - . | tar -C /opt/onion-src -xf -

cmake -S /opt/onion-src -B /opt/onion-build \
    -DCMAKE_BUILD_TYPE=Release -DONION_BUILD_TESTS=OFF
cmake --build /opt/onion-build -j"$(nproc)"
cmake --install /opt/onion-build --prefix /opt/onion

echo ">>> generating relay keypair (private key stays in this VM)"
install -d -m 0700 /etc/onion/keys
/opt/onion/bin/onion keygen --out /etc/onion/keys --name relay --bits "$KEY_BITS" >/dev/null
chmod 0600 /etc/onion/keys/relay.pem
chmod 0644 /etc/onion/keys/relay.pub.pem

cat >/etc/onion/relay.env <<EOF
RELAY_PORT=${RELAY_PORT}
ADVERTISE_ADDR=${ADVERTISE_ADDR}
EOF
chmod 0600 /etc/onion/relay.env

cat >/opt/onion/bin/start-relay.sh <<'EOF'
#!/bin/sh
exec /opt/onion/bin/onion relay \
    --listen "${RELAY_PORT}" \
    --key /etc/onion/keys/relay.pem \
    --advertise-addr "${ADVERTISE_ADDR}" \
    --verbose
EOF
chmod 0755 /opt/onion/bin/start-relay.sh

echo ">>> installing systemd service"
cp /opt/onion-src/config/onion-relay.service /etc/systemd/system/onion-relay.service
systemctl daemon-reload
systemctl enable --now onion-relay.service

sleep 2
if ! systemctl is-active --quiet onion-relay.service; then
    echo "ERROR: onion-relay.service is not running" >&2
    journalctl -u onion-relay.service --no-pager -n 50 >&2 || true
    exit 1
fi

echo ">>> relay ready"
echo "    overlay address: ${ADVERTISE_ADDR}:${RELAY_PORT}"
echo "    public key:      /etc/onion/keys/relay.pub.pem"
echo "    run on the host: vagrant ssh -c 'sudo bash /vagrant/scripts/print-relay-info.sh'"
