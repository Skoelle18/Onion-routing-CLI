# Onion Routing System (educational)

A small C++17 command-line onion router. A sender builds layered RSA-OAEP +
AES-256-GCM envelopes, and each relay unwraps exactly one layer before
forwarding ciphertext to its successor. Relay VMs are provisioned with Vagrant
and reach each other over an overlay VPN, so the two physical laptops can be on
different Wi-Fi networks, behind different NATs, with no shared LAN and no
router configuration.

> **This is not a substitute for Tor and makes no strong anonymity guarantees.**
> It has no guard nodes, padding, cover traffic, forward secrecy, directory
> authorities, or resistance to traffic analysis. It is intended for demos and
> learning only.

## Threat model and privacy notes

- The sender knows the full path. Each relay knows only its immediate
  predecessor (the TCP peer it accepted) and its immediate successor (read from
  the layer it decrypted). The destination knows the last relay.
- Relay logs contain peer address, successor address, and ciphertext size. They
  never contain plaintext or inner ciphertext contents.
- The overlay VPN sees connection metadata: which devices talk to which, when,
  and for how long. Onion layers protect message contents and hide the full
  path from individual relays, **not** from the overlay operator or an observer
  of the overlay.
- A modified byte in any layer causes RSA-OAEP or AES-GCM authentication to
  fail; the relay drops the layer, logs the failure, and returns
  `DECRYPT_FAILED`.

## Prerequisites

- Linux host with `g++` (C++17), CMake 3.16+, OpenSSL development headers,
  GoogleTest (or internet access for CMake to fetch it), and `bash`;
  **or** Windows 11 with Visual Studio 2022 Build Tools (C++ workload), CMake,
  and vcpkg.
- VirtualBox and Vagrant on both laptops.
- A Tailscale account and an auth key, **or** a small VPS with a public IP for
  the self-hosted WireGuard alternative.
- For relay VMs: nothing beyond Vagrant's default NAT networking. No bridged
  adapter, shared LAN, or router port forwarding is needed.

Check your toolchain:

```bash
g++ --version
cmake --version
openssl version
vagrant --version
```

## Build and local verification

```bash
cmake -S . -B build -DONION_BUILD_TESTS=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Or use the helper, which also runs the localhost topology and tamper tests:

```bash
./scripts/local-test.sh
```

The local test starts three relay processes and one receiver on `127.0.0.1`,
sends a message through all three relays, runs `onion check`, and confirms that
no relay logged plaintext. It typically completes in a few seconds.

### Guided installers

For a step-by-step setup that reports `ISSUE` / `WHY` / `FIX` on failures:

```bash
# Linux (CachyOS/Arch, Debian/Ubuntu, Fedora)
./install/install-linux.sh
```

```cmd
:: Windows 11, from Command Prompt
install\install-windows.cmd
```

The Windows installer builds a native `onion.exe` using MSVC and vcpkg; WSL is
not required. See `install/README.md` for flags and common fixes.

## CLI

```text
onion keygen  --out <dir> [--bits N] [--name NAME]
onion relay   --listen <port> --key <privkey> [--advertise-addr <ip>] [--verbose]
onion listen  --port <port> --key <privkey> [--verbose]
onion send    --path <relays.conf> --dest <ip:port> --message "<text>" [--verbose]
onion check   --path <relays.conf> [--verbose]
onion build   --path <relays.conf> --dest <ip:port> --message "<text>" --out <file>
onion inject  --addr <ip:port> --file <onion.bin>
```

`build` and `inject` are test/demo helpers: they let you build a raw onion,
tamper with it, and inject it into the first relay to observe rejection.

RSA keys default to 2048 bits and are configurable with `--bits` (minimum
2048). Private keys are written with `0600` permissions. The receiver also uses
an RSA keypair; otherwise the last relay or the network would see plaintext.

## Wire format

All TCP messages use one length-prefixed frame:

```text
[uint32 BE payload_len][payload]

payload:
  "ONF1"       4 bytes
  type         1 byte
    0x01 ONION   encrypted layer, forwarded hop by hop
    0x02 STATUS  response code, returned hop by hop
    0x03 PING    local reachability probe
    0x04 PONG    reply to PING
  body         variable bytes
```

The `ONION` body is an encrypted envelope:

```text
"ONLV"                    4 bytes
version                   1 byte = 1
wrapped_key_len           2 bytes BE
wrapped_key               RSA-2048-OAEP-SHA256(AES-256 key)
iv_len                    1 byte = 12
iv                        random 12-byte AES-GCM IV
ciphertext_len            4 bytes BE
ciphertext || tag         AES-256-GCM ciphertext with 16-byte tag
```

Decrypting one envelope yields an inner payload:

```text
"ONPL"                    4 bytes
version                   1 byte = 1
kind                      1 byte
  0x01 FORWARD            inner bytes are the next hop's envelope
  0x02 DELIVER            inner bytes are the final plaintext message
next_addr_len             1 byte
next_addr                 ASCII "ip:port" or "[ipv6]:port"
inner_len                 4 bytes BE
inner                     next envelope or final message
```

The next-hop address is inside the authenticated ciphertext, so only the relay
that owns the layer learns its successor. Maximum frame size is 16 MiB.

`STATUS` codes are `OK`, `DECRYPT_FAILED`, `UNREACHABLE_HOP`, `PROTOCOL_ERROR`,
and `TIMEOUT`. `onion send` reports these distinctly, so an unreachable hop is
never confused with a decryption failure.

## Path configuration

`config/relays.conf.example`:

```text
relay 100.64.0.2:9001 "keys/relay-b1.pub.pem"
relay 100.64.0.3:9001 "keys/relay-a2.pub.pem"
destination 100.64.0.5:9000 "keys/dest.pub.pem"
```

Quotes are optional but recommended when paths contain spaces. Relative key
paths resolve relative to the config file. `send --dest` must match the
`destination` entry; this prevents accidentally delivering a message to an
endpoint whose key was not used when building the onion.

## Overlay network setup

### Option A: Tailscale (default)

On **each physical laptop**:

1. Sign in at <https://login.tailscale.com/admin>.
2. Go to **Settings → Authentication keys** and generate a key. Choose a
   reusable key or a one-use/ephemeral key depending on your demo.
3. Install Tailscale from <https://tailscale.com/download> and log in (`tailscale up`).
4. Confirm both laptops have stable `100.x.y.z` addresses (`tailscale ip -4`).

Never commit the auth key. Pass it only at `vagrant up` time:

```bash
export TS_AUTHKEY='tskey-auth-...'
```

The Vagrant provisioner installs Tailscale inside each VM, joins the tailnet
with `--hostname $OVERLAY_HOSTNAME`, and uses Tailscale userspace networking
automatically if `/dev/net/tun` is unavailable.

### Option B: self-hosted WireGuard hub (no third-party account)

Use a small VPS with a public IP as the hub. On the VPS:

```bash
./scripts/wireguard-hub-setup.sh ./hub 10.9.0.1/24 10.9.0.2/32 vpn.example.com:51820
sudo cp ./hub/wg0.conf /etc/wireguard/wg0.conf
sudo wg-quick up wg0
sudo sysctl -w net.ipv4.ip_forward=1
```

Generate a separate client address/key pair for each laptop and relay VM (for
example `10.9.0.2/32`, `10.9.0.3/32`, ...), add their public keys as peers on
the hub, and bring up `wg0` on every host. Then provision a relay with:

```bash
OVERLAY=wireguard \
WG_ADDRESS=10.9.0.3/32 \
WG_PRIVATE_KEY='<client-private-key>' \
WG_SERVER_PUBKEY='<hub-public-key>' \
WG_ENDPOINT='vpn.example.com:51820' \
WG_ALLOWED_IPS=10.9.0.0/24 \
RELAY_NAME=relay-b1 OVERLAY_HOSTNAME=relay-b1 RELAY_PORT=9001 \
vagrant up
```

## Vagrant relay provisioning

The same Vagrantfile works on both laptops. Use different VM names and overlay
hostnames on each machine.

On **Laptop B** (Relay 1):

```bash
TS_AUTHKEY="$TS_AUTHKEY" \
RELAY_NAME=relay-b1 OVERLAY_HOSTNAME=relay-b1 RELAY_PORT=9001 \
vagrant up
vagrant ssh -c 'bash /vagrant/scripts/print-relay-info.sh'
```

On **Laptop A** (Relay 2):

```bash
TS_AUTHKEY="$TS_AUTHKEY" \
RELAY_NAME=relay-a2 OVERLAY_HOSTNAME=relay-a2 RELAY_PORT=9001 \
vagrant up
vagrant ssh -c 'bash /vagrant/scripts/print-relay-info.sh'
```

On a **Windows 11** host, use PowerShell environment variables instead:

```powershell
$env:TS_AUTHKEY = "tskey-auth-..."
$env:RELAY_NAME = "relay-b1"
$env:OVERLAY_HOSTNAME = "relay-b1"
$env:RELAY_PORT = "9001"
vagrant up
vagrant ssh -c "bash /vagrant/scripts/print-relay-info.sh"
```

`vagrant up` installs build dependencies, copies the project into `/opt/onion-src`,
builds and installs the binary to `/opt/onion/bin/onion`, generates the relay
keypair in `/etc/onion/keys`, and starts `onion-relay.service`. The private key
never leaves the VM; the helper prints only the public key and overlay address.

If a VM was already provisioned and you change env vars, run:

```bash
vagrant reload --provision
```

Useful overrides:

- `ADVERTISE_ADDR`: explicit address written into `relays.conf`.
- `KEY_BITS`: RSA key size, default `2048`.
- `OVERLAY`: `tailscale` (default) or `wireguard`.

Check the service:

```bash
vagrant ssh -c 'systemctl status onion-relay.service'
```

## Receiver setup on Laptop B

Laptop B is both a relay host and the receiver host. Build the project there
too (or copy over the `onion` binary), then generate a receiver key and start
the listener:

On Linux:

```bash
mkdir -p ~/onion-demo/keys
./build/onion keygen --out ~/onion-demo/keys --name dest
./build/onion listen --port 9000 --key ~/onion-demo/keys/dest.pem --verbose
```

On Windows 11 (native Command Prompt):

```cmd
mkdir %USERPROFILE%\onion-demo\keys
build\Release\onion.exe keygen --out %USERPROFILE%\onion-demo\keys --name dest
build\Release\onion.exe listen --port 9000 --key %USERPROFILE%\onion-demo\keys\dest.pem --verbose
```

Copy `dest.pub.pem` to Laptop A.

## Two-network walkthrough

Target path:

```text
Host 1 (sender, Laptop A)
  -> Relay 1 (VM on Laptop B)
  -> Relay 2 (VM on Laptop A)
  -> Host 2 (receiver, Laptop B)
```

1. On Laptop B, run `print-relay-info.sh` for `relay-b1`. On Laptop A, run it
   for `relay-a2`. Copy both `relay ...` lines into `relays.conf` on Laptop A.
2. Add the destination line using Laptop B's overlay IP and the copied
   `dest.pub.pem`.
3. On Laptop B, start `onion listen --port 9000 --key ...` (use
   `build\Release\onion.exe` on Windows).
4. On Laptop A, check reachability:

   ```bash
   ./build/onion check --path relays.conf --verbose
   ```

   Expected output: one `[reachable]` line per hop with an RTT.

5. On Laptop A, send the message:

   ```bash
   ./build/onion send --path relays.conf \
       --dest 100.64.0.5:9000 \
       --message "hello across two networks" --verbose
   ```

6. Laptop B's receiver prints `hello across two networks`.

Verbose relay logs show lines like:

```text
predecessor 100.64.0.9:51234 -> successor 100.64.0.3:9001, ciphertext 712 bytes
```

Each relay sees only that pair and the ciphertext length. No relay logs the
message.

## Connectivity behavior

- Relays bind to `0.0.0.0:<port>` by default.
- `--advertise-addr` controls the address printed for `relays.conf`; it does not
  change the bind address.
- Outbound hops use a connect timeout, two bounded retries with exponential
  backoff, TCP keepalive, and an I/O timeout.
- `onion check` opens a TCP connection to every configured hop and measures a
  PING/PONG round trip. It exits non-zero if any hop is unreachable.
- The tool only uses IP:port pairs, so it also works on a plain LAN or over
  public IPs.

## Firewall notes

- Allow TCP on the relay ports (`9001` in the examples) and receiver port
  (`9000`) from overlay addresses (`100.64.0.0/10` for Tailscale,
  `10.9.0.0/24` in the WireGuard example).
- On Ubuntu VMs: `sudo ufw allow from 100.64.0.0/10 to any port 9001 proto tcp`.
- Tailscale/WireGuard normally traverse NAT without inbound firewall rules;
  these rules only matter if a host firewall is restrictive.

## Fallback: direct port forwarding (usually fragile)

You can skip the overlay and forward relay ports on each router. This often
fails because:

- home and mobile connections commonly use carrier-grade NAT, so you do not
  control the public IP;
- phone hotspots and cafe Wi-Fi block inbound connections entirely;
- UPnP is disabled or unavailable on many routers;
- port numbers and public IPs change, requiring constant config updates.

If you must use it, give each relay a static LAN IP, forward its TCP port on
that laptop's router, and put the router's public IP:port in `relays.conf`.
The onion protocol itself does not care; only address reachability changes.

## Tests

```bash
ctest --test-dir build --output-on-failure
```

On Windows:

```cmd
ctest --test-dir build -C Release --output-on-failure
```

- `unit_tests`: crypto round-trips, wrong-key rejection, per-byte tamper
  detection, layer wrap/unwrap, framing, and address parsing.
- `local_topology`: three localhost relays, message delivery, `onion check`,
  and a check that no relay logged plaintext.
- `tamper_rejection`: builds an onion, flips a byte in the wrapped RSA key and
  in the GCM tag, injects both, and verifies `DECRYPT_FAILED` while an
  untampered onion still arrives.
- On Windows the two bash-driven multi-process tests are skipped; the C++
  `SocketTest.LoopbackPingPong` case exercises the same Winsock code path.

## Limitations

- No strong anonymity guarantees; not Tor.
- No forward secrecy: a recorded onion can be decrypted if a relay's private
  key is later compromised.
- No padding or cover traffic: packet sizes and timing leak information.
- The overlay sees metadata.
- Sequential per-connection relay handling is suitable for demos, not
  high-throughput production use.
