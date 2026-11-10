# Build-Icarus-BLE.ps1 - build the Icarus firmware WITH Bluetooth LE MIDI, on your own PC, from the latest sources.
#
#   powershell -ExecutionPolicy Bypass -File Build-Icarus-BLE.ps1
#   (or double-click Build-Icarus-BLE.bat)
#
# The published Icarus releases are built with the radio OFF. This script is for people who want the
# Bluetooth build: it checks what the build needs, installs what is missing (it asks first), gets the
# latest sources and runs the normal checked build with the -BLE switch.
#
# It works two ways:
#   - inside a clone of the repository: it updates that clone (only when it is on 'main' with no local changes)
#   - on its own (just this file downloaded): it clones the repository into .\Icarus next to it
#
# Switches:
#   -CheckOnly        check (and install) the requirements, then stop: nothing is built
#   -NoUpdate         build the sources as they are; do not pull
#   -Yes              answer "yes" to every install question
#   -Pi32Root <dir>   where the JieLi pi32 toolchain is (the folder that holds bin\ and pi32v2-lib\)
#   -RepoUrl / -Branch  which repository and branch to build
#
# What it installs, and from where (always with winget, always after asking):
#   Git (Git.Git), Python 3.12 (Python.Python.3.12), LLVM-MinGW UCRT (MartinStorsjo.LLVM-MinGW.UCRT), numpy (pip)
# The JieLi pi32v2 toolchain (JieLi's own compiler for the chip) is not redistributed here: when it is
# missing the script offers to download JieLi's installer from JieLi's server and start it for you.
#
# The image this produces has the radio transmitter enabled. It is for your own unit: you are
# responsible for using it within your local radio regulations.

[CmdletBinding()]
param(
    [switch]$CheckOnly,
    [switch]$NoUpdate,
    [switch]$Yes,
    [string]$Pi32Root = "",
    [string]$RepoUrl = "https://github.com/28spawn/Icarus.git",
    [string]$Branch = "main"
)

$ErrorActionPreference = "Stop"
$JieliPage = "https://pkgman.jieliapp.com/doc/all"               # JieLi's own tools page (lists the toolchain installers)
$JieliToolchain = "https://pkgman.jieliapp.com/s/win-toolchain"  # JieLi's Windows toolchain installer, as linked from that page
$triedInstaller = $false

function Say([string]$t)  { Write-Host $t -ForegroundColor Cyan }
function Ok([string]$t)   { Write-Host ("  ok    " + $t) -ForegroundColor Green }
function Warn([string]$t) { Write-Host ("  note  " + $t) -ForegroundColor Yellow }
function Stop-With([string]$t) { Write-Host ""; Write-Host ("[STOPPED] " + $t) -ForegroundColor Red; exit 1 }

function Ask-Yes([string]$q) {
    if ($Yes) { return $true }
    $a = Read-Host ("  " + $q + " [Y/n]")
    return -not ($a -match '^(n|no)$')
}

# A program installed a moment ago is not on this window's PATH yet: read the PATH again from the registry.
function Update-SessionPath {
    $m = [Environment]::GetEnvironmentVariable("Path", "Machine")
    $u = [Environment]::GetEnvironmentVariable("Path", "User")
    $have = $env:PATH -split ';'
    foreach ($p in (($m + ";" + $u) -split ';')) {
        if ($p -and ($have -notcontains $p)) { $env:PATH = $env:PATH + ";" + $p }
    }
}

function Install-WithWinget([string]$Id, [string]$What) {
    if (-not (Get-Command winget.exe -ErrorAction SilentlyContinue)) {
        Stop-With "$What is missing and winget is not available to install it. Install $What by hand, then run this again."
    }
    if (-not (Ask-Yes "$What is missing. Install it now (winget install $Id)?")) {
        Stop-With "$What is needed for the build."
    }
    & winget install --id $Id -e --accept-package-agreements --accept-source-agreements
    Update-SessionPath
}

# The build calls plain "python": it must be a real Python 3.10+, not the Microsoft Store placeholder.
function Test-Python {
    if (-not (Get-Command python.exe -ErrorAction SilentlyContinue)) { return $false }
    $old = $ErrorActionPreference; $ErrorActionPreference = "Continue"
    $out = & python -c "import sys; print(sys.version_info[:2] >= (3, 10))" 2>$null
    $ErrorActionPreference = $old
    return ("$out".Trim() -eq "True")
}

function Test-Pi32([string]$Root) {
    if (-not $Root) { return $false }
    foreach ($f in @("bin\clang.exe", "bin\pi32v2-ld.exe", "bin\llvm-objcopy.exe", "bin\llvm-nm.exe", "pi32v2-include", "pi32v2-lib\r3\libc.a", "pi32v2-lib\r3\libm.a", "pi32v2-lib\r3\libcompiler_rt.a")) {
        if (-not (Test-Path (Join-Path $Root $f))) { return $false }
    }
    return $true
}

function Find-Pi32 {
    $cands = @($Pi32Root, $env:PI32_ROOT)
    if ($env:USERPROFILE) { $cands += (Join-Path $env:USERPROFILE 'tools\jieli\C$\JL\pi32') }
    $cands += @("C:\JL\pi32", "C:\jieli\pi32")
    foreach ($c in $cands) { if (Test-Pi32 $c) { return (Resolve-Path $c).Path } }
    return $null
}

Write-Host ""
Say "=== Icarus firmware with Bluetooth LE - build it yourself ==="
Write-Host ""

if ($env:OS -ne "Windows_NT") { Stop-With "This script is for Windows 10/11 (the build pipeline is custom-fw\build.ps1)." }

# ---- 1. Git -----------------------------------------------------------------
Say "[1/6] Git"
if (-not (Get-Command git.exe -ErrorAction SilentlyContinue)) { Install-WithWinget "Git.Git" "Git" }
if (-not (Get-Command git.exe -ErrorAction SilentlyContinue)) { Stop-With "Git was installed but is not on PATH yet. Open a NEW window and run this again." }
Ok ((& git --version) -join " ")

# ---- 2. The sources ---------------------------------------------------------
Say "[2/6] The sources"
$here = $PSScriptRoot
$inRepo = (Test-Path (Join-Path $here ".git")) -and (Test-Path (Join-Path $here "custom-fw\build.ps1"))
if ($inRepo) {
    $Repo = $here
} else {
    $Repo = Join-Path $here "Icarus"
    if (-not (Test-Path (Join-Path $Repo ".git"))) {
        Write-Host "  cloning $RepoUrl into $Repo"
        & git clone --branch $Branch $RepoUrl $Repo
        if ($LASTEXITCODE -ne 0) { Stop-With "git clone failed (see the message above)." }
        $NoUpdate = $true       # a fresh clone is the latest
    }
}
if (-not $NoUpdate) {
    # The build rewrites two tracked paths (the build id header and the release folder); they do not count as local changes.
    $generated = @("custom-fw/include/pa_build_id.h", "custom-fw/release")
    $cur = ("" + (& git -C $Repo rev-parse --abbrev-ref HEAD)).Trim()
    $dirty = @(& git -C $Repo status --porcelain --untracked-files=no -- . ":(exclude)custom-fw/include/pa_build_id.h" ":(exclude)custom-fw/release")
    if ($cur -ne $Branch) {
        Warn "this clone is on branch '$cur', not '$Branch': building it as it is (no update)."
    } elseif ($dirty.Count -gt 0) {
        Warn "this clone has local changes: building it as it is (no update)."
    } else {
        & git -C $Repo checkout -q -- $generated
        & git -C $Repo pull --ff-only origin $Branch
        if ($LASTEXITCODE -ne 0) { Stop-With "git pull failed (see the message above). Run again with -NoUpdate to build what is there." }
    }
}
if (-not (Test-Path (Join-Path $Repo "custom-fw\third_party\optimist_ble"))) { Stop-With "the Bluetooth stack (custom-fw\third_party\optimist_ble) is not in $Repo." }
Ok ("{0}  ({1})" -f $Repo, (("" + (& git -C $Repo log -1 --format="%h %s")).Trim()))

# ---- 3. Python --------------------------------------------------------------
Say "[3/6] Python 3.10+ and numpy"
if (-not (Test-Python)) { Install-WithWinget "Python.Python.3.12" "Python 3.10 or newer" }
if (-not (Test-Python)) { Stop-With "Python was installed but 'python' is not usable in this window yet. Open a NEW window and run this again. (If 'python' opens the Microsoft Store, turn off the python.exe alias in Settings > Apps > App execution aliases.)" }
Ok ((& python --version) -join " ")
$old = $ErrorActionPreference; $ErrorActionPreference = "Continue"
& python -c "import numpy" 2>$null
$haveNumpy = ($LASTEXITCODE -eq 0)
$ErrorActionPreference = $old
if (-not $haveNumpy) {
    if (-not (Ask-Yes "The Python package numpy is missing (the build's test suites use it). Install it with pip?")) { Stop-With "numpy is needed for the build." }
    & python -m pip install "numpy>=1.24"
    if ($LASTEXITCODE -ne 0) { Stop-With "pip could not install numpy (see the message above)." }
}
Ok "numpy"

# ---- 4. The PC compiler (the test suites and the emulator DLL) ---------------
Say "[4/6] The PC compiler (LLVM-MinGW clang)"
. (Join-Path $Repo "custom-fw\emu\find_clang.ps1")
try { $null = Find-EmuClang } catch { Stop-With $_.Exception.Message }
Ok "clang for the PC"

# ---- 5. The JieLi pi32v2 toolchain (the device compiler) ---------------------
Say "[5/6] The JieLi pi32v2 toolchain (the compiler for the device)"
$pi32 = Find-Pi32
while (-not $pi32) {
    Write-Host ""
    Warn "the JieLi pi32v2 toolchain was not found."
    Write-Host "  It is JieLi's own compiler for the AC791N chip. It cannot be bundled with Icarus, so you install it once:"
    Write-Host "    JieLi's installer : $JieliToolchain   (about 53 MB, from JieLi's own server)"
    Write-Host "    JieLi's tools page: $JieliPage"
    Write-Host "  Keep the installer's default folder (C:\JL\pi32). The folder must hold"
    Write-Host "  bin\clang.exe, bin\pi32v2-ld.exe  and  pi32v2-lib\r3\libc.a ."
    if ($Yes) {
        if ($triedInstaller) { Stop-With "the JieLi toolchain is still not found. Install it, then run this again (or pass -Pi32Root <folder>)." }
        $a = "d"
    } else {
        $a = Read-Host "  Type D to download and start JieLi's installer, O to open the tools page, a folder path, Enter to look again, Q to quit"
    }
    if ($a -match '^(q|quit)$') { Stop-With "the JieLi toolchain is needed for the build." }
    if ($a -match '^o$') { Start-Process $JieliPage; continue }
    if ($a -match '^d$') {
        $triedInstaller = $true
        $exe = Join-Path $env:TEMP "jieli-pi32-toolchain-setup.exe"
        Write-Host "  downloading to $exe ..."
        try {
            [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
            $pp = $ProgressPreference; $ProgressPreference = "SilentlyContinue"
            Invoke-WebRequest -Uri $JieliToolchain -OutFile $exe -UseBasicParsing
            $ProgressPreference = $pp
        } catch { Warn ("the download failed: " + $_.Exception.Message); if ($Yes) { Stop-With "could not download JieLi's installer." }; continue }
        Write-Host ("  SHA-256 of the installer: " + (Get-FileHash $exe -Algorithm SHA256).Hash.ToLower())
        Write-Host "  starting the installer: follow its steps, this window waits until it closes."
        try { Start-Process $exe -Wait } catch { Warn ("the installer did not start: " + $_.Exception.Message) }
    } elseif ($a) { $Pi32Root = $a.Trim('"') }
    $pi32 = Find-Pi32
}
$env:PI32_ROOT = $pi32
Ok $pi32

if ($CheckOnly) {
    Write-Host ""
    Say "Everything the Bluetooth build needs is in place. (-CheckOnly: nothing was built.)"
    exit 0
}

# ---- 6. The build ------------------------------------------------------------
Say "[6/6] The checked build, with Bluetooth LE enabled (several minutes)"
Write-Host "  The build downloads three small JieLi SDK files (Apache-2.0) the first time and checks them by SHA-256."
# The build rewrites two tracked paths (the release folder and the build id header). If they were untouched
# before, they are put back afterwards, so a Bluetooth image can never be committed by accident.
$generated = @("custom-fw/include/pa_build_id.h", "custom-fw/release")
$wasClean = (@(& git -C $Repo status --porcelain -- $generated).Count -eq 0)
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $Repo "custom-fw\build.ps1") -BLE
if ($LASTEXITCODE -ne 0) { Stop-With "the build failed (see the first error above). No firmware image was written." }

# The vendor-free package is always written. (custom-fw\Icarus.fwsc is extra: the build only makes it when it finds a stock 'Pocket AMP.fwsc'.)
$fwsc = Get-ChildItem (Join-Path $Repo "custom-fw\release") -Recurse -Filter "Icarus_*.fwsc" -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 1 | ForEach-Object { $_.FullName }
if (-not $fwsc) { Stop-With "the build finished but no Icarus_*.fwsc is in custom-fw\release." }
# Keep the Bluetooth package in a git-ignored place (*.fwsc is ignored outside custom-fw\release) and put the tracked files back.
$keep = Join-Path $Repo "custom-fw\Icarus_BLE.fwsc"
Copy-Item -Force $fwsc $keep
$fwsc = $keep
$restored = $false
if ($wasClean) {
    & git -C $Repo checkout -q -- $generated
    $restored = ($LASTEXITCODE -eq 0)
}
$hash = (Get-FileHash $fwsc -Algorithm SHA256).Hash.ToLower()
Write-Host ""
Say "=== Done: Icarus with Bluetooth LE ==="
Write-Host "  firmware package : $fwsc"
Write-Host "  SHA-256          : $hash"
Write-Host "  install it with  : $(Join-Path $Repo 'custom-fw\web\Icarus_Web_Editor.html')  (Chrome or Edge, 'Install this firmware')"
Write-Host ""
Warn "This image turns the radio transmitter on. It is your own build for your own unit: do not pass it on,"
Warn "and use it within your local radio regulations. Back up your unit first (see RECOVERY.md)."
if (-not $restored) { Warn "custom-fw\release and pa_build_id.h now hold this Bluetooth build: do not commit them." }
exit 0
