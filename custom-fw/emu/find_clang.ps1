# custom-fw/emu/find_clang.ps1 - find (or help install) a C compiler that can build the emulator DLL.
#
# Dot-source it, then call:   $clang = Find-EmuClang          (returns the full path of a working clang.exe, or throws with exact instructions)
#
# Why this exists: a plain "LLVM" install (winget LLVM.LLVM, the llvm.org installer) gives you clang.exe but NO C library headers on Windows
# ("fatal error: 'stdlib.h' file not found", 'string.h', 'math.h'): it expects Visual Studio's / the Windows SDK's headers. The emulator build wants a
# self-contained toolchain, LLVM-MinGW (UCRT), which carries its own headers. So a clang is accepted only if it can compile AND link a small probe that
# includes the headers the firmware needs; the first one that can is used, whatever order they were installed in.

function Test-EmuClang([string]$Exe) {
    if (-not $Exe -or -not (Test-Path $Exe)) { return $false }
    $dir = Join-Path ([IO.Path]::GetTempPath()) ("emu_probe_" + [Guid]::NewGuid().ToString("N"))
    New-Item -ItemType Directory -Path $dir -Force | Out-Null
    try {
        Set-Content -Path (Join-Path $dir "p.c") -Encoding ascii -Value "#include <stdlib.h>`n#include <string.h>`n#include <stdio.h>`n#include <math.h>`n#include <stdint.h>`nint probe(int x){ char b[8]; memset(b,0,8); return (int)(sqrt((double)x)+(double)strlen(b)+(double)(uintptr_t)malloc(1)); }"
        $old = $ErrorActionPreference; $ErrorActionPreference = "Continue"
        & $Exe -shared -o (Join-Path $dir "p.dll") (Join-Path $dir "p.c") 2>&1 | Out-Null
        $ok = ($LASTEXITCODE -eq 0) -and (Test-Path (Join-Path $dir "p.dll"))
        $ErrorActionPreference = $old
        return $ok
    } finally { Remove-Item -Recurse -Force $dir -ErrorAction SilentlyContinue }
}

function Get-EmuClangCandidates {
    $c = New-Object System.Collections.Generic.List[string]
    # 1. LLVM-MinGW from winget / scoop / manual unzip - the one that is known to work, so it goes first
    $roots = @("$env:LOCALAPPDATA\Microsoft\WinGet\Packages", "$env:USERPROFILE\scoop\apps", "C:\llvm-mingw", "C:\Program Files\llvm-mingw", "$env:USERPROFILE\llvm-mingw", "C:\msys64\clang64\bin", "C:\msys64\ucrt64\bin")
    foreach ($r in $roots) {
        if (Test-Path $r) {
            Get-ChildItem $r -Recurse -Filter "clang.exe" -ErrorAction SilentlyContinue | Where-Object { $_.FullName -match "mingw|llvm|clang" } | ForEach-Object { $c.Add($_.FullName) }
        }
    }
    # 2. whatever is on PATH
    Get-Command clang.exe -All -ErrorAction SilentlyContinue | ForEach-Object { $c.Add($_.Source) }
    # 3. a plain LLVM install (works only if Visual Studio's headers are present)
    foreach ($p in @("C:\Program Files\LLVM\bin\clang.exe", "C:\Program Files (x86)\LLVM\bin\clang.exe")) { if (Test-Path $p) { $c.Add($p) } }
    return $c | Select-Object -Unique
}

function Find-EmuClang {
    foreach ($try in 1..2) {
        foreach ($exe in (Get-EmuClangCandidates)) {
            if (Test-EmuClang $exe) {
                $dir = Split-Path -Parent $exe
                if (($env:PATH -split ';') -notcontains $dir) { $env:PATH = "$dir;$env:PATH" }     # this session only: the compile line says plain "clang"
                Write-Host "  compiler: $exe" -ForegroundColor DarkGray
                return $exe
            }
        }
        if ($try -eq 2) { break }
        $found = @(Get-EmuClangCandidates)
        Write-Host ""
        if ($found.Count -gt 0) {
            Write-Host "  A clang was found ($($found[0])) but it cannot compile the C standard headers (stdlib.h / string.h / math.h)." -ForegroundColor Yellow
            Write-Host "  That is what a plain LLVM install does on Windows: it has no C library of its own." -ForegroundColor Yellow
        } else {
            Write-Host "  No clang compiler was found." -ForegroundColor Yellow
        }
        Write-Host "  The emulator needs LLVM-MinGW (UCRT), a clang that carries its own headers (about 250 MB)." -ForegroundColor Yellow
        $winget = Get-Command winget.exe -ErrorAction SilentlyContinue
        if (-not $winget) { break }
        $ans = Read-Host "  Install it now with winget (winget install MartinStorsjo.LLVM-MinGW.UCRT)? [Y/n]"
        if ($ans -match '^(n|no)$') { break }
        & winget install --id MartinStorsjo.LLVM-MinGW.UCRT -e --accept-package-agreements --accept-source-agreements
    }
    throw @"

Could not find a clang that can build the emulator.

Do ONE of these, then run Start-Emulator.bat again (open a NEW window so the PATH is refreshed):
  A) winget install MartinStorsjo.LLVM-MinGW.UCRT
  B) download llvm-mingw-*-ucrt-x86_64.zip from https://github.com/mstorsjo/llvm-mingw/releases , unzip it to C:\llvm-mingw
     (nothing else to do: the launcher looks there)

A plain "LLVM" (winget LLVM.LLVM) is NOT enough: it has no stdlib.h / string.h / math.h on Windows. You can leave it installed, the launcher skips it.
"@
}
