# Сборка установщика SecVMS. Версию берёт из CMakeLists.txt (project(SecVMS VERSION x.y.z)).
# Запуск:  powershell -ExecutionPolicy Bypass -File installer\build.ps1
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$cm   = Get-Content (Join-Path $root 'CMakeLists.txt') -Raw
if ($cm -notmatch 'project\(SecVMS\s+VERSION\s+([0-9.]+)') { throw 'Версия не найдена в CMakeLists.txt' }
$ver = $Matches[1]

$iscc = @(
  "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe",
  "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
  "$env:ProgramFiles\Inno Setup 6\ISCC.exe"
) | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $iscc) { throw 'Inno Setup 6 не найден (winget install JRSoftware.InnoSetup)' }

$exe = Join-Path $root 'dist\SecVMS\SecVMS.exe'
if (-not (Test-Path $exe)) { throw "Нет dist\SecVMS\SecVMS.exe — сначала собери и скопируй exe в dist" }

Write-Host "SecVMS $ver -> установщик..."
& $iscc "/DAppVersion=$ver" (Join-Path $PSScriptRoot 'SecVMS.iss') | Select-Object -Last 2
$out = Join-Path $PSScriptRoot "Output\SecVMS-Setup-$ver.exe"
if (Test-Path $out) { Write-Host ("Готово: {0} ({1:N1} МБ)" -f $out, ((Get-Item $out).Length/1MB)) }
