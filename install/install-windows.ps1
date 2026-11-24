[CmdletBinding()]
param(
    [switch]$SkipOptional,
    [switch]$SkipVagrant
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
$Build = Join-Path $Root 'build'
$Step = 0

function Write-Step {
    param([string]$Message)
    $script:Step++
    Write-Host "`n=== Step $($script:Step): $Message ===" -ForegroundColor Cyan
}

function Write-Info {
    param([string]$Message)
    Write-Host "    $Message"
}

function Write-Issue {
    param([string]$Issue, [string]$Why, [string]$Fix)
    Write-Host "`n----------------------------------------" -ForegroundColor Red
    Write-Host "ISSUE: $Issue" -ForegroundColor Red
    Write-Host "WHY:   $Why" -ForegroundColor Yellow
    Write-Host "FIX:   $Fix" -ForegroundColor Green
    Write-Host "----------------------------------------" -ForegroundColor Red
    exit 1
}

function Invoke-Checked {
    param(
        [string]$Why,
        [string]$Fix,
        [scriptblock]$Action
    )
    try {
        $global:LASTEXITCODE = 0
        & $Action
        if ($LASTEXITCODE -ne 0) {
            throw "native command exited with code $LASTEXITCODE"
        }
    } catch {
        Write-Issue -Issue "Command failed: $($_.Exception.Message)" -Why $Why -Fix $Fix
    }
}

function Require-Command {
    param([string]$Name, [string]$Fix)
    if (-not (Get-Command $Name -ErrorAction SilentlyContinue)) {
        Write-Issue -Issue "'$Name' is not on PATH" -Why "The required tool was not installed." -Fix $Fix
    }
}

function Test-IsAdmin {
    $principal = New-Object Security.Principal.WindowsPrincipal(
        [Security.Principal.WindowsIdentity]::GetCurrent())
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

Write-Step "Check Windows environment"
if ([Environment]::OSVersion.Version.Major -lt 10) {
    Write-Issue -Issue "Unsupported Windows version" `
        -Why "This installer targets Windows 10/11 with winget." `
        -Fix "Upgrade to Windows 11, or install the dependencies manually."
}
if (-not (Test-IsAdmin)) {
    Write-Info "WARNING: not running as Administrator; installs may prompt for elevation."
}
Require-Command -Name winget -Fix "Install App Installer from the Microsoft Store, then rerun this script."

if (-not $SkipOptional) {
    Write-Step "Install Git, CMake, and Visual Studio Build Tools"
    Invoke-Checked -Why "winget may be offline, blocked by policy, or missing source agreements." `
        -Fix "Run winget source reset --force, or install the tools manually from their vendor pages." `
        -Action {
            winget install --id Git.Git --exact --accept-source-agreements --accept-package-agreements
            winget install --id Kitware.CMake --exact --accept-source-agreements --accept-package-agreements
            winget install --id Microsoft.VisualStudio.2022.BuildTools --exact `
                --accept-source-agreements --accept-package-agreements `
                --override "--wait --quiet --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
        }
} else {
    Write-Step "Skip optional tool installation (requested)"
}

$env:Path = [System.Environment]::GetEnvironmentVariable("Path", "Machine") + ";" +
            [System.Environment]::GetEnvironmentVariable("Path", "User")
Require-Command -Name cmake -Fix "Install CMake from https://cmake.org/download/ and reopen Command Prompt."

Write-Step "Install vcpkg dependencies (OpenSSL, GoogleTest)"
$vcpkgRoot = Join-Path $env:USERPROFILE 'vcpkg'
if (-not (Test-Path (Join-Path $vcpkgRoot 'vcpkg.exe'))) {
    Require-Command -Name git -Fix "Install Git from https://git-scm.com/download/win."
    if (-not (Test-Path $vcpkgRoot)) {
        Invoke-Checked -Why "GitHub may be unreachable or blocked." `
            -Fix "Clone vcpkg manually into $vcpkgRoot, then rerun this script." `
            -Action { git clone https://github.com/microsoft/vcpkg.git $vcpkgRoot }
    }
    Invoke-Checked -Why "The vcpkg bootstrapper could not download its binaries." `
        -Fix "Run: $vcpkgRoot\bootstrap-vcpkg.bat manually and check your internet connection." `
        -Action { & (Join-Path $vcpkgRoot 'bootstrap-vcpkg.bat') }
}
$vcpkgExe = Join-Path $vcpkgRoot 'vcpkg.exe'
Invoke-Checked -Why "OpenSSL or GoogleTest could not be built by vcpkg." `
    -Fix "Run: $vcpkgExe install openssl:x64-windows gtest:x64-windows and inspect the errors." `
    -Action { & $vcpkgExe install openssl:x64-windows gtest:x64-windows }

Write-Step "Configure the project with MSVC and vcpkg"
$toolchain = Join-Path $vcpkgRoot 'scripts\buildsystems\vcpkg.cmake'
Invoke-Checked -Why "CMake could not find the Visual Studio 2022 generator or vcpkg toolchain." `
    -Fix "Install Visual Studio 2022 Build Tools with the 'Desktop development with C++' workload, then rerun." `
    -Action {
        cmake -S $Root -B $Build -G "Visual Studio 17 2022" -A x64 `
            "-DCMAKE_TOOLCHAIN_FILE=$toolchain" `
            -DVCPKG_TARGET_TRIPLET=x64-windows `
            -DONION_BUILD_TESTS=ON
    }

Write-Step "Build"
Invoke-Checked -Why "Compilation failed." `
    -Fix "Read the compiler output above. Ensure the C++ workload and vcpkg OpenSSL are installed." `
    -Action { cmake --build $Build --config Release --parallel }

Write-Step "Run tests"
Invoke-Checked -Why "At least one test failed." `
    -Fix "Run: ctest --test-dir build -C Release --output-on-failure -V to see details." `
    -Action { ctest --test-dir $Build -C Release --output-on-failure }

$binary = Join-Path $Build 'Release\onion.exe'
if (-not (Test-Path $binary)) {
    Write-Issue -Issue "onion.exe was not produced" `
        -Why "The build completed but the binary is missing." `
        -Fix "Check the build output above and rerun: cmake --build build --config Release"
}
Write-Info "Binary: $binary"

if ($SkipOptional -or $SkipVagrant) {
    Write-Step "Skip overlay/Vagrant setup (requested)"
} else {
    Write-Step "Install Tailscale, VirtualBox, and Vagrant"
    Invoke-Checked -Why "winget may be blocked by policy or the packages may need elevation." `
        -Fix "Install Tailscale, Oracle VirtualBox, and HashiCorp Vagrant manually from their vendor pages." `
        -Action {
            winget install --id Tailscale.Tailscale --exact --accept-source-agreements --accept-package-agreements
            winget install --id Oracle.VirtualBox --exact --accept-source-agreements --accept-package-agreements
            winget install --id HashiCorp.Vagrant --exact --accept-source-agreements --accept-package-agreements
        }

    Write-Info "Log into Tailscale in the system tray before provisioning a relay."
    Write-Info "If VirtualBox fails to start, disable Hyper-V or reboot to load its driver."

    if ([Environment]::UserInteractive) {
        $answer = Read-Host "    Provision a relay VM now? [y/N]"
        if ($answer -match '^[Yy]') {
            $secureKey = Read-Host "    Tailscale auth key (hidden)" -AsSecureString
            $keyBstr = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($secureKey)
            $tsKey = [Runtime.InteropServices.Marshal]::PtrToStringUni($keyBstr)
            [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($keyBstr)
            if ([string]::IsNullOrWhiteSpace($tsKey)) {
                Write-Issue -Issue "Tailscale auth key is empty" `
                    -Why "Provisioning needs a key from the Tailscale admin console." `
                    -Fix "Generate a key at https://login.tailscale.com/admin/settings/keys and rerun."
            }
            $relayName = Read-Host "    Relay VM name [relay-b1]"
            if ([string]::IsNullOrWhiteSpace($relayName)) { $relayName = 'relay-b1' }
            $hostname = Read-Host "    Overlay hostname [$relayName]"
            if ([string]::IsNullOrWhiteSpace($hostname)) { $hostname = $relayName }
            $port = Read-Host "    Relay port [9001]"
            if ([string]::IsNullOrWhiteSpace($port)) { $port = '9001' }

            $env:TS_AUTHKEY = $tsKey
            $env:RELAY_NAME = $relayName
            $env:OVERLAY_HOSTNAME = $hostname
            $env:RELAY_PORT = $port

            Invoke-Checked -Why "vagrant up failed. Common causes: bad auth key, VirtualBox driver, or no internet." `
                -Fix "Run: vagrant up --debug. Check Tailscale login and VirtualBox installation." `
                -Action { vagrant up }
            Invoke-Checked -Why "Could not read relay information from the VM." `
                -Fix "Run: vagrant ssh, then check /etc/onion/keys." `
                -Action { vagrant ssh -c "bash /vagrant/scripts/print-relay-info.sh" }
        }
    }
}

Write-Host "`n=== Installation complete ===" -ForegroundColor Green
Write-Info "Binary: $binary"
Write-Info "Local test: ctest --test-dir build -C Release --output-on-failure"
Write-Info "Docs: README.md"
