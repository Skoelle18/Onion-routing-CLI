# -*- mode: ruby -*-
# vi: set ft=ruby :

RELAY_NAME = ENV.fetch("RELAY_NAME", "onion-relay")
RELAY_PORT = ENV.fetch("RELAY_PORT", "9001")
OVERLAY_HOSTNAME = ENV.fetch("OVERLAY_HOSTNAME", RELAY_NAME)
OVERLAY = ENV.fetch("OVERLAY", "tailscale")
KEY_BITS = ENV.fetch("KEY_BITS", "2048")
TS_AUTHKEY = ENV.fetch("TS_AUTHKEY", "")
ADVERTISE_ADDR = ENV.fetch("ADVERTISE_ADDR", "")

WG_ADDRESS = ENV.fetch("WG_ADDRESS", "")
WG_PRIVATE_KEY = ENV.fetch("WG_PRIVATE_KEY", "")
WG_SERVER_PUBKEY = ENV.fetch("WG_SERVER_PUBKEY", "")
WG_ENDPOINT = ENV.fetch("WG_ENDPOINT", "")
WG_ALLOWED_IPS = ENV.fetch("WG_ALLOWED_IPS", "10.9.0.0/24")

Vagrant.configure("2") do |config|
  config.vm.box = "ubuntu/jammy64"
  config.vm.hostname = OVERLAY_HOSTNAME
  config.ssh.forward_agent = false

  # The relay VMs intentionally use VirtualBox NAT only. Reachability comes
  # from the overlay network, so no shared LAN or router port forwarding is
  # required.

  config.vm.provider "virtualbox" do |vb|
    vb.name = RELAY_NAME
    vb.memory = 2048
    vb.cpus = 2
  end

  config.vm.provision "shell",
    path: "scripts/provision-relay.sh",
    privileged: true,
    env: {
      "RELAY_NAME" => RELAY_NAME,
      "RELAY_PORT" => RELAY_PORT,
      "OVERLAY_HOSTNAME" => OVERLAY_HOSTNAME,
      "OVERLAY" => OVERLAY,
      "TS_AUTHKEY" => TS_AUTHKEY,
      "KEY_BITS" => KEY_BITS,
      "ADVERTISE_ADDR" => ADVERTISE_ADDR,
      "WG_ADDRESS" => WG_ADDRESS,
      "WG_PRIVATE_KEY" => WG_PRIVATE_KEY,
      "WG_SERVER_PUBKEY" => WG_SERVER_PUBKEY,
      "WG_ENDPOINT" => WG_ENDPOINT,
      "WG_ALLOWED_IPS" => WG_ALLOWED_IPS
    }
end
