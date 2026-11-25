# Guided installers

These scripts check what is already installed before doing anything. They do
**not** run a full system update or upgrade. A package manager index refresh is
done only when an APT package actually needs to be installed on Linux.

When a required tool is missing or older than the supported version, the script
asks before installing that specific tool. Optional tools are skipped unless
you agree. Every failure prints:

```text
ISSUE: what failed
WHY:   the likely cause
FIX:   the command or action to continue
```

## Linux (CachyOS/Arch, Debian/Ubuntu, Fedora)

```bash
chmod +x install/install-linux.sh
./install/install-linux.sh
```

The script:

1. Detects the package manager from `/etc/os-release`.
2. Checks versions of `g++` (7.0+), `cmake` (3.16+), `git` (2.0+), `python3`
   (3.6+), OpenSSL (1.1.1+ with headers), and GoogleTest.
3. Installs only the specific packages that are missing or too old.
4. Checks optional Vagrant (2.2+), VirtualBox (6.1+), and Tailscale.
5. Builds, runs `ctest`, and offers to provision a relay VM with a hidden
   Tailscale auth-key prompt.

Supported environment overrides:

```bash
# Automatically accept installs of missing/outdated tools.
FORCE_INSTALL=1 ./install/install-linux.sh

# Never prompt. Required missing/outdated tools produce an ISSUE/WHY/FIX error.
NONINTERACTIVE=1 ./install/install-linux.sh
```

Secrets are never written to disk. If a step fails, apply the printed `FIX`
and rerun; completed steps are safe to repeat.

## Windows 11 (Command Prompt)

Double-click `install\install-windows.cmd`, or run:

```cmd
install\install-windows.cmd
```

The wrapper launches PowerShell with the installer. The installer checks first:

- **Git** 2.0+ via `git --version`.
- **CMake** 3.16+ via `cmake --version`.
- **Visual Studio 2022 Build Tools** 17.0+ with the C++ workload via
  `vswhere.exe`.
- **vcpkg**: bootstraps only if `vcpkg.exe` does not exist.
- **vcpkg ports**: runs `vcpkg list` and installs only missing
  `openssl:x64-windows` / `gtest:x64-windows` ports.
- Optional **Tailscale**, **VirtualBox** (6.1+), and **Vagrant** (2.2+).

It then configures with the Visual Studio 17 2022 generator, builds, runs tests
in `Release`, and produces:

```text
build\Release\onion.exe
```

Flags:

```cmd
:: Skip Tailscale/VirtualBox/Vagrant and relay provisioning.
install\install-windows.cmd -SkipVagrant

:: Also skip the build-tool checks. Use this only when your toolchain is ready.
install\install-windows.cmd -SkipOptional

:: Automatically accept installs of missing/outdated tools.
install\install-windows.cmd -ForceInstall
```

`winget` is required only if a tool actually needs to be installed. If it is
missing, the installer prints how to install App Installer from the Microsoft
Store.

## Manual steps the installers cannot do

See **Manual steps required even with the installers** in `README.md`. In
short: the installers can install clients and VMs, but you must create/sign in
to Tailscale, generate an auth key, log in on both physical laptops, copy relay
public keys into `relays.conf`, and start `onion listen` yourself.

## Common fixes

| Symptom | Fix |
| --- | --- |
| `winget` not found | Install App Installer from the Microsoft Store. |
| Visual Studio generator error | Install VS 2022 Build Tools with “Desktop development with C++”. |
| OpenSSL not found in CMake | Run `%USERPROFILE%\vcpkg\vcpkg.exe install openssl:x64-windows gtest:x64-windows`. |
| `vboxdrv` not loading on Linux | Reboot, load `vboxdrv`, or sign VirtualBox kernel modules for Secure Boot. |
| Tailscale IP missing | Run `tailscale up` on Linux or sign in via the system tray on Windows, then `tailscale ip -4` / `tailscale.exe ip -4`. |
| `vagrant up` fails | Run `vagrant up --debug`; check auth key, VirtualBox, and internet access. |
| `relay keypair not found in /etc/onion/keys` | The key directory is root-only; run `vagrant ssh -c "sudo bash /vagrant/scripts/print-relay-info.sh"`. If keys are missing, re-run with a valid `TS_AUTHKEY` and `vagrant up --provision`. |
| Tests fail on Windows | Run `ctest --test-dir build -C Release --output-on-failure -V`. |

## Security note

Never commit Tailscale auth keys or private keys. The installers prompt for the
auth key at runtime and pass it only as an environment variable to `vagrant up`.
