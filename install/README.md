# Guided installers

These scripts install dependencies, build the project, run tests, and optionally
provision a Vagrant relay. When a step fails, they print:

```text
ISSUE: what failed
WHY:   the likely cause
FIX:   the command or action to continue
```

## Linux (CachyOS/Arch, Debian/Ubuntu, Fedora)

From Command Prompt/Terminal in the project root:

```bash
chmod +x install/install-linux.sh
./install/install-linux.sh
```

The script detects your package manager, installs core dependencies, builds,
runs `ctest`, and offers to provision a relay VM with a hidden Tailscale auth
key prompt. Secrets are never written to disk.

If a step fails, apply the printed `FIX` and rerun the script; completed steps
are safe to repeat.

## Windows 11 (Command Prompt)

Double-click `install\install-windows.cmd`, or run:

```cmd
install\install-windows.cmd
```

The wrapper launches PowerShell with the installer. It:

1. Installs Git, CMake, and Visual Studio Build Tools with the C++ workload.
2. Bootstraps vcpkg into `%USERPROFILE%\vcpkg`.
3. Installs `openssl:x64-windows` and `gtest:x64-windows`.
4. Configures with the Visual Studio 17 2022 generator.
5. Builds and runs tests in `Release`.
6. Offers to install Tailscale, VirtualBox, Vagrant, and optionally provision a relay.

To skip the overlay/Vagrant portion:

```cmd
install\install-windows.cmd -SkipVagrant
```

To skip all optional installs (useful if tools are already present):

```cmd
install\install-windows.cmd -SkipOptional
```

The native binary is:

```text
build\Release\onion.exe
```

## Common fixes

| Symptom | Fix |
| --- | --- |
| `winget` not found | Install App Installer from the Microsoft Store. |
| Visual Studio generator error | Install VS 2022 Build Tools with “Desktop development with C++”. |
| OpenSSL not found in CMake | Run `%USERPROFILE%\vcpkg\vcpkg.exe install openssl:x64-windows gtest:x64-windows`. |
| `vboxdrv` not loading on Linux | Reboot, load `vboxdrv`, or sign VirtualBox kernel modules for Secure Boot. |
| Tailscale IP missing | Run `tailscale up` and confirm `tailscale ip -4` on the host. |
| `vagrant up` fails | Run `vagrant up --debug`; check auth key, VirtualBox, and internet access. |
| Tests fail on Windows | Run `ctest --test-dir build -C Release --output-on-failure -V`. |

## Security note

Never commit Tailscale auth keys or private keys. The installers prompt for the
auth key at runtime and pass it only as an environment variable to `vagrant up`.
