[CmdletBinding()]
param(
    [string]$Board = "pocket_amp"
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Set-Location $RepoRoot

$BoardDef = if ($Board -eq "blackbox") { "-DBOARD_BLACKBOX=1" } else { "-DBOARD_POCKET_AMP=1" }

. (Join-Path $PSScriptRoot "find_clang.ps1")
$null = Find-EmuClang                      # a clang that can really compile the C headers (see find_clang.ps1)

Write-Host "Compiling Icarus firmware into Native Emulator DLL (icarus_emu.dll)..." -ForegroundColor Cyan

# ---- the source list -------------------------------------------------------
# Globbed through custom-fw/sources.txt, so a new module can never be
# silently forgotten by one target. The only difference between the targets is
# which board files they replace themselves, and that is written down ONCE.
. custom-fw\tools\Read-Sources.ps1
$manifest = Read-SourceManifest
$sources = @(Get-SourceFiles -Target "emu" -Sections $manifest | ForEach-Object { "custom-fw/" + $_ })

# PA_STOCK_FLASH_LOADER: the emulator reads the stock amps and cabs from its simulated flash
# (0x0BF000, loaded via src/board/spi_nor.c); the device build
# keeps the embedded tables (src/sys/stock_assets.c says why)
clang -O3 -shared -Wall -Wextra -Icustom-fw/include $BoardDef -DPA_HOST_SIM=1 -DPA_EMU_DLL=1 -DPA_STOCK_FLASH_LOADER=1 `
  custom-fw/emu/emu_bridge.c `
  $sources `
  -o custom-fw/emu/icarus_emu.dll

if ($LASTEXITCODE -eq 0) {
    Write-Host "SUCCESS: icarus_emu.dll compiled!" -ForegroundColor Green
} else {
    Write-Error "FAILED to compile icarus_emu.dll"
}
