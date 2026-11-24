[CmdletBinding()]
param(
    [switch]$SkipOptional,
    [switch]$SkipVagrant,
    [switch]$ForceInstall
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
$Build = Join-Path $Root 'build'
$Step = 0

$script:VcpkgRoot = if ($env:VCPKG_ROOT) { $env:VCPKG_ROOT } else { Join-Path $env:USERPROFILE 'vcpkg' }

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

function Get-VersionToken {
    param([string]$Value)
    if ([string]::IsNullOrWhiteSpace($Value)) { return $null }
    $match = [regex]::Match($Value, '\d+(?:\.\d+){0,3}')
    if ($match.Success) { return $match.Value }
    return $null
}

function Normalize-Version {
    param([string]$Token)
    if ([string]::IsNullOrWhiteSpace($Token)) { return $null }
    $parts = @($Token.Split('.'))
    while ($parts.Count -lt 4) { $parts += '0' }
    return ($parts[0..3] -join '.')
}

function Test-VersionAtLeast {
    param([string]$Current, [string]$Minimum)
    $have = Normalize-Version (Get-VersionToken $Current)
    $need = Normalize-Version (Get-VersionToken $Minimum)
    if (-not $have -or -not $need) { return $false }
    return ([version]$have -ge [version]$need)
}

function Get-VersionFromCommand {
    param(
        [string]$Path,
        [string[]]$Arguments = @('--version')
    )
    try {
        $output = (& $Path @Arguments 2>&1 | Select-Object -First 2 | Out-String)
        return Get-VersionToken $output
    } catch {
        return $null
    }
}

function Refresh-Path {
    $programFiles = [Environment]::GetEnvironmentVariable('ProgramFiles')
    $programFilesX86 = [Environment]::GetEnvironmentVariable('ProgramFiles(x86)')
    $gitBin = if ($programFiles) { Join-Path $programFiles 'Git\cmd' } else { $null }
    $cmakeBin = if ($programFiles) { Join-Path $programFiles 'CMake\bin' } else { $null }
    $vsInstaller = if ($programFilesX86) { Join-Path $programFilesX86 'Microsoft Visual Studio\Installer' } else { $null }
    $candidates = @(
        $env:Path
        [Environment]::GetEnvironmentVariable('Path', 'Machine')
        [Environment]::GetEnvironmentVariable('Path', 'User')
        $gitBin
        $cmakeBin
        $vsInstaller
        $script:VcpkgRoot
    )
    $entries = $candidates | Where-Object { -not [string]::IsNullOrWhiteSpace($_) } |
        ForEach-Object { $_ -split ';' } |
        Where-Object {
            -not [string]::IsNullOrWhiteSpace($_) -and
            $_ -notmatch '^"?[A-Za-z]:\\Windows\\System32\\WindowsPowerShell'
        } | Select-Object -Unique
    $env:Path = ($entries -join ';')
}

function Find-Executable {
    param(
        [string]$Name,
        [string[]]$Candidates = @()
    )
    $command = Get-Command $Name -CommandType Application -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }
    foreach ($candidate in $Candidates) {
        if ($candidate -and (Test-Path $candidate)) { return $candidate }
    }
    return $null
}

function Get-VisualStudioVersion {
    $programFiles = [Environment]::GetEnvironmentVariable('ProgramFiles')
    $programFilesX86 = [Environment]::GetEnvironmentVariable('ProgramFiles(x86)')
    $locations = @(
        (Join-Path $programFilesX86 'Microsoft Visual Studio\Installer\vswhere.exe'),
        (Join-Path $programFiles 'Microsoft Visual Studio\Installer\vswhere.exe')
    )
    foreach ($vswhere in $locations) {
        if (-not $vswhere -or -not (Test-Path $vswhere)) { continue }
        try {
            $output = (& $vswhere -latest -products * `
                -requires Microsoft.VisualStudio.Workload.VCTools `
                -property installationVersion 2>&1 | Select-Object -First 1 | Out-String)
            $version = Get-VersionToken $output
            if ($version) { return $version }
        } catch {
            continue
        }
    }
    return $null
}

function Confirm-Install {
    param(
        [string]$Reason,
        [string]$Target
    )
    if ($ForceInstall) {
        Write-Info "ForceInstall is set; installing '$Target'."
        return $true
    }
    if (-not [Environment]::UserInteractive) { return $false }
    $answer = Read-Host "    $Reason - install '$Target' now? [y/N]"
    return ($answer -match '^[Yy]')
}

function Confirm-OptionalInstall {
    param(
        [string]$Reason,
        [string]$Target
    )
    if ($ForceInstall) { return $true }
    if (-not [Environment]::UserInteractive) { return $false }
    $answer = Read-Host "    $Reason - install '$Target' now? [y/N]"
    return ($answer -match '^[Yy]')
}

function Assert-Winget {
    if (-not (Get-Command winget -CommandType Application -ErrorAction SilentlyContinue)) {
        Write-Issue -Issue "'winget' is not on PATH" `
            -Why "winget is needed to install missing or outdated tools." `
            -Fix "Install App Installer from the Microsoft Store, then rerun this script."
    }
}

function Install-Winget {
    param(
        [string]$Id,
        [string]$Why,
        [string]$Fix,
        [string[]]$ExtraArguments = @()
    )
    Assert-Winget
    Invoke-Checked -Why $Why -Fix $Fix -Action {
        $wingetArgs = @(
            'install', '--id', $Id, '--exact',
            '--accept-source-agreements', '--accept-package-agreements'
        ) + $ExtraArguments
        & winget @wingetArgs
    }
    Refresh-Path
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
Write-Info "No system-wide update will be performed."
Refresh-Path

Write-Step "Check required build tools (install only if missing or too old)"

$git = Get-Command git -CommandType Application -ErrorAction SilentlyContinue
$gitVersion = if ($git) { Get-VersionFromCommand $git.Source @('--version') } else { $null }
if ($gitVersion -and (Test-VersionAtLeast $gitVersion '2.0.0')) {
    Write-Info "Git: $gitVersion (OK)"
} else {
    $reason = if ($gitVersion) { "Git $gitVersion is older than required 2.0.0" } else { "Git is not installed" }
    if (Confirm-Install $reason 'Git.Git') {
        Install-Winget -Id 'Git.Git' `
            -Why "winget may be offline or blocked by policy." `
            -Fix "Run winget source reset --force, or install Git from https://git-scm.com/download/win."
    } else {
        Write-Issue -Issue "Git is required" -Why $reason `
            -Fix "Install Git manually from https://git-scm.com/download/win, then rerun this script."
    }
}

$cmake = Get-Command cmake -CommandType Application -ErrorAction SilentlyContinue
$cmakeVersion = if ($cmake) { Get-VersionFromCommand $cmake.Source @('--version') } else { $null }
if ($cmakeVersion -and (Test-VersionAtLeast $cmakeVersion '3.16.0')) {
    Write-Info "CMake: $cmakeVersion (OK)"
} else {
    $reason = if ($cmakeVersion) { "CMake $cmakeVersion is older than required 3.16.0" } else { "CMake is not installed" }
    if (Confirm-Install $reason 'Kitware.CMake') {
        Install-Winget -Id 'Kitware.CMake' `
            -Why "winget may be offline or blocked by policy." `
            -Fix "Run winget source reset --force, or install CMake from https://cmake.org/download/."
    } else {
        Write-Issue -Issue "CMake is required" -Why $reason `
            -Fix "Install CMake manually from https://cmake.org/download/, then rerun this script."
    }
}

$vsVersion = Get-VisualStudioVersion
if ($vsVersion -and (Test-VersionAtLeast $vsVersion '17.0.0')) {
    Write-Info "Visual Studio Build Tools: $vsVersion with C++ workload (OK)"
} else {
    $reason = if ($vsVersion) {
        "Visual Studio Build Tools $vsVersion is older than required 17.0"
    } else {
        "Visual Studio 2022 Build Tools with the C++ workload were not found"
    }
    if (Confirm-Install $reason 'Microsoft.VisualStudio.2022.BuildTools') {
        Install-Winget -Id 'Microsoft.VisualStudio.2022.BuildTools' `
            -Why "winget may be offline, or the VS installer needs elevation." `
            -Fix "Install VS 2022 Build Tools with 'Desktop development with C++' from https://visualstudio.microsoft.com/downloads/." `
            -ExtraArguments @(
                '--override',
                '--wait --quiet --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended'
            )
    } else {
        Write-Issue -Issue "Visual Studio 2022 C++ toolchain is required" -Why $reason `
            -Fix "Install VS 2022 Build Tools with 'Desktop development with C++', then rerun this script."
    }
}

$git = Get-Command git -CommandType Application -ErrorAction SilentlyContinue
if (-not $git) {
    Write-Issue -Issue "git is still not on PATH" `
        -Why "The installer could not refresh PATH after installing Git." `
        -Fix "Reopen Command Prompt and rerun this script."
}
$cmake = Get-Command cmake -CommandType Application -ErrorAction SilentlyContinue
if (-not $cmake) {
    Write-Issue -Issue "cmake is still not on PATH" `
        -Why "The installer could not refresh PATH after installing CMake." `
        -Fix "Reopen Command Prompt and rerun this script."
}
$vsVersion = Get-VisualStudioVersion
if (-not $vsVersion -or -not (Test-VersionAtLeast $vsVersion '17.0.0')) {
    Write-Issue -Issue "Visual Studio 2022 C++ workload is still missing" `
        -Why "vswhere.exe did not report a compatible installation after setup." `
        -Fix "Run Visual Studio Installer, add 'Desktop development with C++', then rerun this script."
}

Write-Step "Check vcpkg and required ports (install only if missing)"

$vcpkgExe = Join-Path $script:VcpkgRoot 'vcpkg.exe'
if (-not (Test-Path $vcpkgExe)) {
    $reason = "vcpkg is not installed at $($script:VcpkgRoot)"
    if (Confirm-Install $reason 'vcpkg') {
        if (-not (Test-Path $script:VcpkgRoot)) {
            Invoke-Checked -Why "GitHub may be unreachable or blocked." `
                -Fix "Clone vcpkg manually into $($script:VcpkgRoot), then rerun this script." `
                -Action { & git clone https://github.com/microsoft/vcpkg.git $script:VcpkgRoot }
        }
        Invoke-Checked -Why "The vcpkg bootstrapper could not download its binaries." `
            -Fix "Run: $($script:VcpkgRoot)\bootstrap-vcpkg.bat manually and check your internet connection." `
            -Action { & (Join-Path $script:VcpkgRoot 'bootstrap-vcpkg.bat') }
    } else {
        Write-Issue -Issue "vcpkg is required" -Why $reason `
            -Fix "Install vcpkg from https://github.com/microsoft/vcpkg and rerun this script."
    }
}
if (-not (Test-Path $vcpkgExe)) {
    Write-Issue -Issue "vcpkg.exe was not found after setup" `
        -Why "Bootstrap did not produce the executable." `
        -Fix "Run $($script:VcpkgRoot)\bootstrap-vcpkg.bat manually."
}
Write-Info "vcpkg: $vcpkgExe (OK)"

$requiredPorts = @('openssl:x64-windows', 'gtest:x64-windows')
try {
    $installedLines = & $vcpkgExe list 2>&1
} catch {
    Write-Issue -Issue "Could not read installed vcpkg ports" `
        -Why "vcpkg list failed: $($_.Exception.Message)" `
        -Fix "Run: $vcpkgExe list and fix any reported problem."
}
if ($LASTEXITCODE -ne 0) {
    Write-Issue -Issue "Could not read installed vcpkg ports" `
        -Why "vcpkg list failed." `
        -Fix "Run: $vcpkgExe list and fix any reported problem."
}
$missingPorts = @($requiredPorts | Where-Object {
    $port = $_
    -not ($installedLines | Where-Object { $_ -match "^$([regex]::Escape($port))\s" })
})
if ($missingPorts.Count -eq 0) {
    Write-Info "vcpkg ports openssl:x64-windows and gtest:x64-windows (OK)"
} else {
    $reason = "missing vcpkg port(s): $($missingPorts -join ', ')"
    if (Confirm-Install $reason ($missingPorts -join ', ')) {
        Invoke-Checked -Why "OpenSSL or GoogleTest could not be built by vcpkg." `
            -Fix "Run: $vcpkgExe install $($missingPorts -join ' ') and inspect the errors." `
            -Action { & $vcpkgExe install @missingPorts }
    } else {
        Write-Issue -Issue "Required vcpkg ports are missing" -Why $reason `
            -Fix "Run: $vcpkgExe install $($missingPorts -join ' ')"
    }
}

Write-Step "Configure the project with MSVC and vcpkg"
$toolchain = Join-Path $script:VcpkgRoot 'scripts\buildsystems\vcpkg.cmake'
Invoke-Checked -Why "CMake could not find the Visual Studio 2022 generator or vcpkg toolchain." `
    -Fix "Install VS 2022 Build Tools with the 'Desktop development with C++' workload, then rerun." `
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
    Write-Step "Check optional overlay tools (install only if missing or too old)"

    $programFiles = [Environment]::GetEnvironmentVariable('ProgramFiles')
    $tailscalePath = Find-Executable 'tailscale.exe' @((Join-Path $programFiles 'Tailscale\tailscale.exe'))
    $tailscaleVersion = if ($tailscalePath) { Get-VersionFromCommand $tailscalePath @('version') } else { $null }
    if ($tailscaleVersion) {
        Write-Info "Tailscale: $tailscaleVersion (OK)"
    } elseif (Confirm-OptionalInstall "Tailscale is not installed or could not be version-checked" 'Tailscale.Tailscale') {
        Install-Winget -Id 'Tailscale.Tailscale' `
            -Why "winget may be blocked by policy or need elevation." `
            -Fix "Install Tailscale from https://tailscale.com/download/windows."
    } else {
        Write-Info "Skipped Tailscale install."
    }

    $vboxPath = Find-Executable 'VBoxManage.exe' @((Join-Path $programFiles 'Oracle\VirtualBox\VBoxManage.exe'))
    $vboxVersion = if ($vboxPath) { Get-VersionFromCommand $vboxPath @('--version') } else { $null }
    if ($vboxVersion -and (Test-VersionAtLeast $vboxVersion '6.1.0')) {
        Write-Info "VirtualBox: $vboxVersion (OK)"
    } else {
        $reason = if ($vboxVersion) {
            "VirtualBox $vboxVersion is older than recommended 6.1.0"
        } else {
            "VirtualBox is not installed"
        }
        if (Confirm-OptionalInstall $reason 'Oracle.VirtualBox') {
            Install-Winget -Id 'Oracle.VirtualBox' `
                -Why "winget may be blocked by policy or need elevation." `
                -Fix "Install VirtualBox from https://www.virtualbox.org/wiki/Downloads."
        } else {
            Write-Info "Skipped VirtualBox install."
        }
    }

    $vagrant = Get-Command vagrant -CommandType Application -ErrorAction SilentlyContinue
    $vagrantVersion = if ($vagrant) { Get-VersionFromCommand $vagrant.Source @('--version') } else { $null }
    if ($vagrantVersion -and (Test-VersionAtLeast $vagrantVersion '2.2.0')) {
        Write-Info "Vagrant: $vagrantVersion (OK)"
    } else {
        $reason = if ($vagrantVersion) {
            "Vagrant $vagrantVersion is older than recommended 2.2.0"
        } else {
            "Vagrant is not installed"
        }
        if (Confirm-OptionalInstall $reason 'HashiCorp.Vagrant') {
            Install-Winget -Id 'HashiCorp.Vagrant' `
                -Why "winget may be blocked by policy or need elevation." `
                -Fix "Install Vagrant from https://developer.hashicorp.com/vagrant/downloads."
        } else {
            Write-Info "Skipped Vagrant install."
        }
    }

    Write-Info "Manual Tailscale steps remain: create/sign in to an account, generate an auth key, and log in on both laptops."
    Write-Info "If VirtualBox fails to start, disable Hyper-V or reboot to load its driver."

    $vagrant = Get-Command vagrant -CommandType Application -ErrorAction SilentlyContinue
    if ([Environment]::UserInteractive -and $vagrant) {
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
    } else {
        Write-Info "Skipped relay provisioning (non-interactive shell or Vagrant unavailable)."
    }
}

Write-Host "`n=== Installation complete ===" -ForegroundColor Green
Write-Info "Binary: $binary"
Write-Info "Local test: ctest --test-dir build -C Release --output-on-failure"
Write-Info "Docs: README.md"
Write-Info "No system-wide update was performed."
