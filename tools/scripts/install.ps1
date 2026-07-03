<#
.SYNOPSIS
  Kirito installer for Windows (PowerShell).

.DESCRIPTION
  Installs the `ki` interpreter and the `kpm` package manager, and creates the package directory
  %USERPROFILE%\.kirito\packages (which `ki` puts on its import path, so packages installed by
  `kpm` are importable directly). The install directory is added to your user PATH.

  Run:
    irm https://raw.githubusercontent.com/AzethMeron/KiritoLang/main/tools/scripts/install.ps1 | iex

  Or with options:
    & ([scriptblock]::Create((irm <url>))) -BinDir "C:\Tools\Kirito" -Ref main

.PARAMETER BinDir   Where to install ki.exe / kpm.cmd (default: %LOCALAPPDATA%\Programs\Kirito).
.PARAMETER Ref      Git ref to fetch kpm.ki / build from (default: main).
.PARAMETER FromSource  Build ki from source instead of downloading a release binary.
#>
param(
    [string]$BinDir = (Join-Path $env:LOCALAPPDATA "Programs\Kirito"),
    [string]$Ref = "main",
    [switch]$FromSource
)

$ErrorActionPreference = "Stop"
$Repo = "AzethMeron/KiritoLang"
$KiritoHome = Join-Path $env:USERPROFILE ".kirito"

function Say($m)  { Write-Host "==> $m" -ForegroundColor Cyan }
function Warn($m) { Write-Host "warning: $m" -ForegroundColor Yellow }

New-Item -ItemType Directory -Force -Path $BinDir, (Join-Path $KiritoHome "packages") | Out-Null

function Build-FromSource {
    Say "building ki from source ($Repo@$Ref)"
    foreach ($t in @("git", "cmake")) {
        if (-not (Get-Command $t -ErrorAction SilentlyContinue)) { throw "$t is required to build from source" }
    }
    $tmp = Join-Path $env:TEMP ("kirito-" + [guid]::NewGuid().ToString("N"))
    git clone --depth 1 --branch $Ref "https://github.com/$Repo" "$tmp\src"
    if ($LASTEXITCODE -ne 0) { git clone --depth 1 "https://github.com/$Repo" "$tmp\src" }
    # TLS (OpenSSL) is needed for kpm to fetch from GitHub. A from-source build may lack it; if so,
    # kpm won't work until you rebuild with -DKIRITO_ENABLE_TLS=ON and OpenSSL available.
    cmake -S "$tmp\src" -B "$tmp\build" -DCMAKE_BUILD_TYPE=Release
    cmake --build "$tmp\build" --config Release --target ki
    $exe = Get-ChildItem -Path "$tmp\build" -Recurse -Filter ki.exe | Select-Object -First 1
    if (-not $exe) { throw "build did not produce ki.exe" }
    Copy-Item $exe.FullName (Join-Path $BinDir "ki.exe") -Force
    Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
}

function Download-Release {
    if ([System.Environment]::Is64BitOperatingSystem) {
        $url = "https://github.com/$Repo/releases/latest/download/ki-windows-x64.exe"
        Say "downloading ki-windows-x64.exe"
        Invoke-WebRequest -Uri $url -OutFile (Join-Path $BinDir "ki.exe")
        return $true
    }
    return $false
}

if ($FromSource) {
    Build-FromSource
} else {
    try {
        if (-not (Download-Release)) { throw "no prebuilt binary for this architecture" }
    } catch {
        Warn "release download failed ($_); building from source"
        Build-FromSource
    }
}

# Install the kpm package manager (a Kirito script) + a .cmd launcher next to ki.exe.
Say "installing kpm"
Invoke-WebRequest -Uri "https://raw.githubusercontent.com/$Repo/$Ref/kpm/kpm.ki" -OutFile (Join-Path $KiritoHome "kpm.ki")
# KPM_SELF lets `kpm update-kpm` overwrite this kpm.ki; KPM_KI_PATH lets `kpm update-ki` replace
# ki.exe (Windows can't overwrite a running exe in place, so kpm moves the old one aside first).
$cmd = "@echo off`r`nsetlocal`r`nset `"KPM_SELF=%USERPROFILE%\.kirito\kpm.ki`"`r`nset `"KPM_KI_PATH=%~dp0ki.exe`"`r`n`"%~dp0ki.exe`" `"%USERPROFILE%\.kirito\kpm.ki`" %*`r`n"
Set-Content -Path (Join-Path $BinDir "kpm.cmd") -Value $cmd -Encoding ascii

# Add the install directory to the user PATH (persisted), if not already present.
$userPath = [Environment]::GetEnvironmentVariable("Path", "User")
if (-not $userPath) { $userPath = "" }
if (($userPath -split ';') -notcontains $BinDir) {
    [Environment]::SetEnvironmentVariable("Path", ($userPath.TrimEnd(';') + ";" + $BinDir), "User")
    Warn "added $BinDir to your user PATH — open a new terminal for it to take effect"
}
$env:Path = "$env:Path;$BinDir"

Say "verifying"
& (Join-Path $BinDir "ki.exe") --help | Out-Null

Say "Kirito installed:"
Write-Host "    ki  -> $(Join-Path $BinDir 'ki.exe')"
Write-Host "    kpm -> $(Join-Path $BinDir 'kpm.cmd')   (packages: $(Join-Path $KiritoHome 'packages'))"
Write-Host "Try:  ki --help      |      kpm help"
