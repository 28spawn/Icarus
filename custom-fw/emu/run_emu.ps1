# custom-fw/emu/run_emu.ps1 — Build & Launch Icarus Live Emulator + ASIO Audio Workbench
param([switch]$Rebuild)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Set-Location $RepoRoot
$Dll = Join-Path $RepoRoot "custom-fw\emu\icarus_emu.dll"

# ---- prerequisites ---------------------------------------------------------
$py = Get-Command python -ErrorAction SilentlyContinue
if (-not $py) {
    Write-Host "[ERROR] python was not found on PATH. Install Python 3.10+ from https://python.org and tick 'Add to PATH'." -ForegroundColor Red
    exit 1
}
& python -c "import numpy, sounddevice" 2>&1 | Out-Null
if ($LASTEXITCODE -ne 0) {
    Write-Host "[ERROR] The emulator needs Python packages: numpy, sounddevice." -ForegroundColor Red
    Write-Host "        Install them with:" -ForegroundColor Yellow
    Write-Host "          python -m pip install -r custom-fw/emu/requirements.txt" -ForegroundColor Yellow
    exit 1
}

# ---- use a prebuilt DLL when possible, build when needed -------------------
if (-not $Rebuild -and -not (Test-Path $Dll)) {
    $prev = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $RepoRoot "custom-fw\emu\fetch_emu_dll.ps1")
    $ErrorActionPreference = $prev
}

if (-not $Rebuild -and (Test-Path $Dll)) {
    Write-Host "[1/2] Using existing icarus_emu.dll (pass -Rebuild to recompile)." -ForegroundColor Cyan
}
else {
    . (Join-Path $PSScriptRoot "find_clang.ps1")
    $null = Find-EmuClang                  # a clang that can really compile the C headers; offers to install LLVM-MinGW if there is none

    Write-Host "[1/2] Compiling Icarus firmware into the native emulator DLL (icarus_emu.dll)..." -ForegroundColor Cyan

    # ---- the source list ---------------------------------------------------
    # Globbed through custom-fw/sources.txt, so a new module can never be
    # silently forgotten by one target. The only difference between the targets is
    # which board files they replace themselves, and that is written down ONCE.
    . custom-fw\tools\Read-Sources.ps1
    $manifest = Read-SourceManifest
    $sources = @(Get-SourceFiles -Target "emu" -Sections $manifest | ForEach-Object { "custom-fw/" + $_ })

    clang -O3 -shared -Wall -Wextra -Icustom-fw/include -DPA_HOST_SIM=1 -DPA_EMU_DLL=1 `
      custom-fw/emu/emu_bridge.c `
      $sources `
      -o custom-fw/emu/icarus_emu.dll
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $Dll)) {
        Write-Host "[ERROR] the firmware did not compile into icarus_emu.dll (see the compiler messages above)." -ForegroundColor Red
        Write-Host "        If they say 'stdlib.h' / 'string.h' / 'math.h' file not found, the clang in use has no C library: install LLVM-MinGW (winget install MartinStorsjo.LLVM-MinGW.UCRT) and run again." -ForegroundColor Yellow
        exit 1
    }
}

# Pick the first free port in 8080..8099. 8080 is commonly taken by some other
# dev server; before this the emulator just died with "address already in use".
$used = @((Get-NetTCPConnection -State Listen -ErrorAction SilentlyContinue).LocalPort)
$port = 8080
while (($used -contains $port) -and ($port -lt 8100)) { $port++ }

Write-Host "[2/2] Starting Low-Latency ASIO Audio Host & Web UI at http://localhost:$port ..." -ForegroundColor Green
Start-Process "http://localhost:$port"
python custom-fw/emu/server.py --port $port
