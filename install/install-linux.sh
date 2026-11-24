#!/usr/bin/env bash
# Guided installer for Linux hosts (CachyOS/Arch, Debian/Ubuntu, Fedora).
# It reports ISSUE / WHY / FIX on any failed step so you can resume safely.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="$ROOT/build"
STEP=0

step() {
    STEP=$((STEP + 1))
    printf '\n=== Step %d: %s ===\n' "$STEP" "$1"
}

info() { printf '    %s\n' "$1"; }

fail() {
    local issue="$1" why="$2" fix="$3"
    printf '\n----------------------------------------\n' >&2
    printf 'ISSUE: %s\n' "$issue" >&2
    printf 'WHY:   %s\n' "$why" >&2
    printf 'FIX:   %s\n' "$fix" >&2
    printf '----------------------------------------\n' >&2
    exit 1
}

run_or_fail() {
    local why="$1" fix="$2"
    shift 2
    if ! "$@"; then
        fail "Command failed: $*" "$why" "$fix"
    fi
}

step "Detect Linux distribution"
[[ -r /etc/os-release ]] || fail "/etc/os-release is missing" \
    "This installer needs a standard Linux distro." \
    "Use a supported distro (Arch/CachyOS, Debian/Ubuntu, or Fedora)."
# shellcheck disable=SC1091
source /etc/os-release
DETECTED="${ID:-unknown} ${ID_LIKE:-}"
case "$DETECTED" in
    *arch*)   PM=pacman ;;
    *debian*|*ubuntu*) PM=apt ;;
    *fedora*|*rhel*|*centos*) PM=dnf ;;
    *) fail "Unsupported distribution: $DETECTED" \
           "Package names differ between distributions." \
           "Install g++, cmake, OpenSSL headers, GoogleTest, git, and python3 manually, then rerun this script." ;;
esac
info "Detected: ${PRETTY_NAME:-$DETECTED} (package manager: $PM)"

step "Install core build dependencies"
case "$PM" in
    pacman)
        run_or_fail "Package databases may be stale or the keyring needs refresh." \
            "Run: sudo pacman-key --refresh-keys && sudo pacman -Syu" \
            sudo pacman -Syu --noconfirm base-devel cmake openssl gtest git python3
        ;;
    apt)
        run_or_fail "APT repositories may be unreachable or stale." \
            "Run: sudo apt-get update && sudo apt-get upgrade" \
            sudo apt-get update
        run_or_fail "Required build packages could not be installed." \
            "Check your internet connection and APT sources." \
            sudo apt-get install -y build-essential cmake libssl-dev libgtest-dev git python3
        ;;
    dnf)
        run_or_fail "DNF repositories may be unreachable or stale." \
            "Run: sudo dnf update" \
            sudo dnf install -y gcc-c++ cmake openssl-devel gtest-devel git python3
        ;;
esac

step "Verify toolchain"
command -v g++ >/dev/null 2>&1 || fail "g++ not found" \
    "The C++ compiler was not installed or is not on PATH." \
    "Install a C++17 compiler (package: base-devel / build-essential / gcc-c++)."
command -v cmake >/dev/null 2>&1 || fail "cmake not found" \
    "CMake was not installed or is not on PATH." \
    "Install cmake with your package manager."
info "Compiler: $(g++ --version | head -n1)"
info "CMake:    $(cmake --version | head -n1)"

step "Configure and build"
run_or_fail "CMake configuration failed, usually because OpenSSL or GTest was not found." \
    "Install OpenSSL development headers and GoogleTest, then rerun: cmake -S . -B build -DONION_BUILD_TESTS=ON" \
    cmake -S "$ROOT" -B "$BUILD" -DONION_BUILD_TESTS=ON
run_or_fail "Compilation failed." \
    "Read the compiler output above. If OpenSSL headers are missing, install libssl-dev/openssl-devel." \
    cmake --build "$BUILD" -j"$(nproc)"

step "Run tests"
run_or_fail "At least one test failed." \
    "Run ctest with verbose output to see which test failed: ctest --test-dir build --output-on-failure -V" \
    ctest --test-dir "$BUILD" --output-on-failure
info "Binary: $BUILD/onion"

step "Optional: overlay and Vagrant tools"
if command -v vagrant >/dev/null 2>&1 && command -v virtualbox >/dev/null 2>&1; then
    info "Vagrant and VirtualBox are already installed."
else
    info "Vagrant/VirtualBox not found. Installing them is recommended for relay VMs."
    case "$PM" in
        pacman)
            sudo pacman -Syu --noconfirm vagrant virtualbox virtualbox-host-dkms linux-headers \
                || info "WARNING: VirtualBox install failed; see FIX below."
            if getent group vboxusers >/dev/null 2>&1; then
                sudo usermod -aG vboxusers "$USER" || true
            fi
            (sudo modprobe vboxdrv 2>/dev/null) || info \
                "WARNING: vboxdrv not loaded. Reboot, or sign VirtualBox modules if Secure Boot is enabled."
            ;;
        apt)
            sudo apt-get install -y vagrant virtualbox virtualbox-dkms linux-headers-generic \
                || info "WARNING: VirtualBox install failed; see FIX below."
            sudo usermod -aG vboxusers "$USER" || true
            ;;
        dnf)
            sudo dnf install -y vagrant VirtualBox VirtualBox-kmodsrc kernel-devel \
                || info "WARNING: VirtualBox install failed; see FIX below."
            sudo usermod -aG vboxusers "$USER" || true
            ;;
    esac
    info "If VirtualBox still fails: reboot, load vboxdrv, and handle Secure Boot signing."
fi

if command -v tailscale >/dev/null 2>&1; then
    info "Tailscale is installed."
else
    case "$PM" in
        pacman) sudo pacman -Syu --noconfirm tailscale || info "WARNING: Tailscale install failed." ;;
        apt) info "Install Tailscale from https://tailscale.com/download/linux for your Ubuntu version." ;;
        dnf) sudo dnf install -y tailscale || info "WARNING: Tailscale install failed." ;;
    esac
fi

step "Optional: provision a relay VM now"
if [[ -t 0 ]] && command -v vagrant >/dev/null 2>&1; then
    read -r -p "    Provision a relay VM? [y/N]: " provision
    if [[ "$provision" =~ ^[Yy] ]]; then
        read -r -s -p "    Tailscale auth key (hidden): " TS_AUTHKEY
        printf '\n'
        [[ -n "$TS_AUTHKEY" ]] || fail "Tailscale auth key is empty" \
            "Provisioning needs a key from https://login.tailscale.com/admin/settings/keys" \
            "Generate a key and rerun this step."
        read -r -p "    Relay VM name [relay-a1]: " RELAY_NAME
        RELAY_NAME="${RELAY_NAME:-relay-a1}"
        read -r -p "    Overlay hostname [$RELAY_NAME]: " OVERLAY_HOSTNAME
        OVERLAY_HOSTNAME="${OVERLAY_HOSTNAME:-$RELAY_NAME}"
        read -r -p "    Relay port [9001]: " RELAY_PORT
        RELAY_PORT="${RELAY_PORT:-9001}"

        run_or_fail "vagrant up failed." \
            "Common causes: invalid TS_AUTHKEY, VirtualBox not loaded, or no internet in the VM. Run: vagrant up --debug" \
            env TS_AUTHKEY="$TS_AUTHKEY" RELAY_NAME="$RELAY_NAME" \
                OVERLAY_HOSTNAME="$OVERLAY_HOSTNAME" RELAY_PORT="$RELAY_PORT" \
                vagrant up
        run_or_fail "Could not read relay information from the VM." \
            "SSH into the VM manually: vagrant ssh" \
            vagrant ssh -c "bash /vagrant/scripts/print-relay-info.sh"
    else
        info "Skipped. Run 'vagrant up' later with TS_AUTHKEY set."
    fi
else
    info "Skipped (non-interactive shell or Vagrant unavailable)."
fi

printf '\n=== Installation complete ===\n'
info "Binary: $BUILD/onion"
info "Local demo: ./scripts/local-test.sh"
info "Docs: README.md"
