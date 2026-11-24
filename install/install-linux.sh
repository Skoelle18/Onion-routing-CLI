#!/usr/bin/env bash
# Guided installer for Linux hosts (CachyOS/Arch, Debian/Ubuntu, Fedora).
# - No full system upgrade.
# - Detects installed tools and versions first.
# - Installs only when a required tool is missing or too old.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="$ROOT/build"
STEP=0
APT_INDEX_UPDATED=0
FORCE_INSTALL="${FORCE_INSTALL:-0}"
NONINTERACTIVE="${NONINTERACTIVE:-0}"

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

version_ge() {
    local have="$1" need="$2"
    [[ "$(printf '%s\n%s\n' "$need" "$have" | sort -V | head -n1)" == "$need" ]]
}

extract_version() {
    grep -oE '[0-9]+(\.[0-9]+)*' | head -n1
}

ask_yes_no() {
    local question="$1"
    [[ "$NONINTERACTIVE" == "1" ]] && return 1
    local answer
    read -r -p "    $question [y/N]: " answer
    [[ "$answer" =~ ^[Yy] ]]
}

resolve_package() {
    local logical="$1"
    case "$logical" in
        cxx)
            case "$PM" in
                pacman) printf 'base-devel' ;;
                apt) printf 'build-essential' ;;
                dnf) printf 'gcc-c++' ;;
            esac
            ;;
        python)
            case "$PM" in
                pacman) printf 'python' ;;
                *) printf 'python3' ;;
            esac
            ;;
        openssl)
            case "$PM" in
                pacman) printf 'openssl' ;;
                apt) printf 'libssl-dev' ;;
                dnf) printf 'openssl-devel' ;;
            esac
            ;;
        gtest)
            case "$PM" in
                pacman) printf 'gtest' ;;
                apt) printf 'libgtest-dev' ;;
                dnf) printf 'gtest-devel' ;;
            esac
            ;;
        *) printf '%s' "$logical" ;;
    esac
}

install_package() {
    local package="$1" reason="$2" confirmed="${3:-0}"
    if [[ "$confirmed" != "1" && "$FORCE_INSTALL" != "1" ]] &&
       ! ask_yes_no "$reason — install '$package' now?"; then
        local fix_cmd
        case "$PM" in
            pacman) fix_cmd="sudo pacman -S --needed $package" ;;
            apt) fix_cmd="sudo apt-get install -y $package" ;;
            dnf) fix_cmd="sudo dnf install -y $package" ;;
        esac
        fail "Required package not installed: $package" \
            "You declined the install, and the build cannot proceed without it." \
            "Install it manually, then rerun this script. Command: $fix_cmd"
    fi

    info "Installing '$package' (no system upgrade)."
    case "$PM" in
        pacman)
            run_or_fail "Could not install $package." \
                "Check pacman output. You may need sudo privileges." \
                "Run: sudo pacman -S --needed $package" \
                sudo pacman -S --needed --noconfirm "$package"
            ;;
        apt)
            if [[ "$APT_INDEX_UPDATED" -eq 0 ]]; then
                info "Refreshing package index only (not upgrading the system)."
                run_or_fail "Package index refresh failed." \
                    "Check your internet connection and APT sources." \
                    "Run: sudo apt-get update" \
                    sudo apt-get update
                APT_INDEX_UPDATED=1
            fi
            run_or_fail "Could not install $package." \
                "Check APT output. You may need sudo privileges." \
                "Run: sudo apt-get install -y $package" \
                sudo apt-get install -y "$package"
            ;;
        dnf)
            run_or_fail "Could not install $package." \
                "Check DNF output. You may need sudo privileges." \
                "Run: sudo dnf install -y $package" \
                sudo dnf install -y "$package"
            ;;
    esac
}

ensure_command() {
    local tool="$1" minimum="$2" logical="$3" description="$4"
    local package
    package="$(resolve_package "$logical")"
    if command -v "$tool" >/dev/null 2>&1; then
        local current
        current="$("$tool" --version 2>&1 | extract_version)"
        if [[ -n "$current" ]] && version_ge "$current" "$minimum"; then
            info "$description: $tool $current (OK)"
            return
        fi
        install_package "$package" "$description $tool $current is older than required $minimum"
    else
        install_package "$package" "$description $tool is not installed"
    fi
}

headers_available() {
    local header="$1"
    local cxx="${CXX:-g++}"
    command -v "$cxx" >/dev/null 2>&1 || return 1
    printf '#include <%s>\nint main() { return 0; }\n' "$header" |
        "$cxx" -x c++ -fsyntax-only - >/dev/null 2>&1
}

ensure_openssl() {
    local minimum="1.1.1" package current=""
    package="$(resolve_package openssl)"

    if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists openssl 2>/dev/null; then
        current="$(pkg-config --modversion openssl)"
    elif command -v openssl >/dev/null 2>&1; then
        current="$(openssl version | extract_version)"
    fi

    if [[ -n "$current" ]] && version_ge "$current" "$minimum" &&
       headers_available openssl/evp.h; then
        info "OpenSSL headers: $current (OK)"
        return
    fi

    if [[ -n "$current" ]]; then
        install_package "$package" "OpenSSL $current is older than required $minimum, or headers are missing"
    else
        install_package "$package" "OpenSSL development headers are missing"
    fi
}

ensure_gtest() {
    local package
    package="$(resolve_package gtest)"

    if headers_available gtest/gtest.h ||
       compgen -G '/usr/lib*/cmake/GTest/GTestConfig.cmake' >/dev/null 2>&1 ||
       { command -v pkg-config >/dev/null 2>&1 && pkg-config --exists gtest 2>/dev/null; }; then
        info "GoogleTest headers/package (OK)"
        return
    fi
    install_package "$package" "GoogleTest is missing (needed for unit tests)"
}

ensure_optional_command() {
    local tool="$1" minimum="$2" package="$3" description="$4"
    package="$(resolve_package "$package")"
    if command -v "$tool" >/dev/null 2>&1; then
        local current
        current="$("$tool" --version 2>&1 | extract_version)"
        if [[ -z "$current" ]] || version_ge "$current" "$minimum"; then
            info "$description: $tool ${current:-installed} (OK)"
            return
        fi
        if ask_yes_no "$description $tool $current is older than recommended $minimum — install '$package'?"; then
            install_package "$package" "$description upgrade requested" 1
        else
            info "Skipped optional upgrade: $tool"
        fi
        return
    fi

    if ask_yes_no "$description $tool is not installed — install '$package'?"; then
        install_package "$package" "$description install requested" 1
    else
        info "Skipped optional tool: $tool"
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
           "Install g++, cmake, OpenSSL headers, GoogleTest, git, and python3 manually, then rerun." ;;
esac
info "Detected: ${PRETTY_NAME:-$DETECTED} (package manager: $PM)"
info "No system upgrade will be performed."

step "Check required build tools"
ensure_command g++ 7.0.0 cxx "C++ compiler"
ensure_command cmake 3.16.0 cmake "CMake"
ensure_command git 2.0.0 git "Git"
ensure_command python3 3.6.0 python "Python 3"
ensure_openssl
ensure_gtest

step "Verify toolchain"
info "Compiler: $(g++ --version | head -n1)"
info "CMake:    $(cmake --version | head -n1)"

step "Configure and build"
run_or_fail "CMake configuration failed, usually because OpenSSL or GTest was not found." \
    "Run: cmake -S . -B build -DONION_BUILD_TESTS=ON and inspect the error." \
    cmake -S "$ROOT" -B "$BUILD" -DONION_BUILD_TESTS=ON
run_or_fail "Compilation failed." \
    "Read the compiler output above. If OpenSSL headers are missing, install the development package." \
    cmake --build "$BUILD" -j"$(nproc)"

step "Run tests"
run_or_fail "At least one test failed." \
    "Run: ctest --test-dir build --output-on-failure -V to see details." \
    ctest --test-dir "$BUILD" --output-on-failure
info "Binary: $BUILD/onion"

step "Optional: overlay and Vagrant tools"
ensure_optional_command vagrant 2.2.0 vagrant "Vagrant"
ensure_optional_command VBoxManage 6.1.0 virtualbox "VirtualBox"
if command -v tailscale >/dev/null 2>&1; then
    info "Tailscale: installed (OK)"
else
    case "$PM" in
        pacman|dnf)
            if ask_yes_no "Tailscale is not installed — install it now?"; then
                install_package tailscale "Tailscale install requested" 1
            else
                info "Skipped Tailscale install. Install it manually from https://tailscale.com/download."
            fi
            ;;
        apt)
            info "Tailscale not found. Install it from https://tailscale.com/download/linux."
            ;;
    esac
fi

if [[ "$PM" == "pacman" ]]; then
    if getent group vboxusers >/dev/null 2>&1; then
        sudo -n usermod -aG vboxusers "$USER" 2>/dev/null || \
            info "WARNING: could not add $USER to vboxusers. Run: sudo usermod -aG vboxusers $USER"
    fi
    (sudo -n modprobe vboxdrv 2>/dev/null) || info \
        "WARNING: vboxdrv not loaded. Reboot, or sign VirtualBox modules if Secure Boot is enabled."
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
            "Common causes: invalid TS_AUTHKEY, VirtualBox driver, or no internet in the VM. Run: vagrant up --debug" \
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
