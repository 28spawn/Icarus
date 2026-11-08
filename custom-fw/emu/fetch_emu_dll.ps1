# custom-fw/emu/fetch_emu_dll.ps1 — download the prebuilt emulator DLL.
#
# The DLL is a build artefact (git ignores *.dll), so a fresh clone does not
# contain it. This script fetches the pinned release asset, verifies its
# SHA-256, and places it at custom-fw/emu/icarus_emu.dll.
#
# The pin lives in emulator_release.json:
#   { "repo": "28spawn/Icarus", "tag": "v0.x.y",
#     "asset": "icarus_emu.dll", "sha256": "<64 hex characters>" }
#
# Empty tag or sha256 means "no release published yet": exit 2 and let the
# caller build locally. Exit codes: 0 = DLL in place, 1 = download/verify
# failed, 2 = not configured.
param(
    [string]$Url = "",
    [string]$Sha256 = "",
    [string]$Dest = ""
)

$ErrorActionPreference = "Stop"
$Here = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $Dest) { $Dest = Join-Path $Here "icarus_emu.dll" }
$ConfigPath = Join-Path $Here "emulator_release.json"

if (Test-Path $Dest) {
    Write-Host "[Emulator] icarus_emu.dll already present - not downloading."
    exit 0
}

$cfg = $null
if (Test-Path $ConfigPath) {
    try { $cfg = Get-Content $ConfigPath -Raw | ConvertFrom-Json } catch { $cfg = $null }
}

if (-not $Url) {
    if (-not $cfg -or -not $cfg.tag -or -not $cfg.sha256) {
        Write-Host "[Emulator] No prebuilt DLL is pinned in emulator_release.json yet."
        exit 2
    }
    $Url = "https://github.com/$($cfg.repo)/releases/download/$($cfg.tag)/$($cfg.asset)"
    if (-not $Sha256) { $Sha256 = $cfg.sha256 }
}

$tmp = "$Dest.download"
try {
    Write-Host "[Emulator] Downloading $Url"
    Invoke-WebRequest -Uri $Url -OutFile $tmp -UseBasicParsing -TimeoutSec 90
}
catch {
    Remove-Item $tmp -Force -ErrorAction SilentlyContinue
    Write-Warning "[Emulator] Could not download icarus_emu.dll: $($_.Exception.Message)"
    exit 1
}

if ($Sha256) {
    $actual = (Get-FileHash $tmp -Algorithm SHA256).Hash.ToLower()
    if ($actual -ne $Sha256.ToLower()) {
        Remove-Item $tmp -Force -ErrorAction SilentlyContinue
        Write-Warning "[Emulator] icarus_emu.dll SHA-256 mismatch."
        Write-Warning "  expected $($Sha256.ToLower())"
        Write-Warning "  actual   $actual"
        Write-Warning "  The file was rejected; building locally is safer."
        exit 1
    }
    Write-Host "[Emulator] SHA-256 verified."
}
else {
    Write-Warning "[Emulator] No SHA-256 pin; the downloaded DLL was not verified."
}

Move-Item -Force $tmp $Dest
Write-Host "[Emulator] Saved $Dest"
exit 0
